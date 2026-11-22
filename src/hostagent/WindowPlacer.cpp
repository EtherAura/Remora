#include "WindowPlacer.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTimer>

namespace remora::hostagent {

namespace {

const QString kService = QStringLiteral("org.remora.HostAgent");
const QString kPath = QStringLiteral("/Placement");

// A JavaScript string literal for any text, by way of a JSON array.
QString jsString(const QString &s) {
    const QString arr = QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact));
    return arr.mid(1, arr.size() - 2);
}

QString scriptName(quint32 session) { return QStringLiteral("remora-session-%1").arg(session); }

// The per-session script. It places the app's window on the output and every later one of its
// windows until the session ends — a dialog or a second window opened from the phone belongs on
// the phone. Borderless and maximized, because the output IS the phone's screen and a title bar
// there is pixels the phone pays for; on every virtual desktop, because desktops are global across
// outputs and switching one at the desk must not take the app off the phone.
//
// Which window answered the launch:
//  - a new one — windowAdded, or windowActivated for a Wayland window that is added before it
//    names itself;
//  - an existing one of a single-instance app — the launch asks it to activate, and KWin's
//    focus-stealing prevention usually refuses (the desk is busy with something else), marking it
//    demandsAttention instead, which is what is watched;
//  - ADOPT, the second load, when a couple of seconds passed with no answer: the app's window that
//    demands attention, or the active one if it is the app's. Never any other: a slow-starting
//    app's other windows on the desk stay where they are.
//
// Each report says whether the window is new ("not among the app's windows when the script
// loaded") and how it was before, so the release can put a desk window back exactly.
const char kPlaceScript[] = R"JS(
var SESSION = %1, APP = %2, OUTPUT = %3, ADOPT = %4, MOVE = %5;
var placed = {}, before = {};
function output() {
    var s = workspace.screens;
    for (var i = 0; i < s.length; i++) if (s[i].name == OUTPUT) return s[i];
    return null;
}
function ours(w) { return w && w.desktopFileName == APP && w.normalWindow; }
function nothingPlaced() { for (var k in placed) return false; return true; }
function place(w) {
    var out = output();
    if (!out || placed[w.internalId.toString()]) return;
    // Marked first: activating it below re-enters the windowActivated handler.
    placed[w.internalId.toString()] = true;
    var isNew = !before[w.internalId.toString()];
    var was = w.frameGeometry, border = !w.noBorder, allDesktops = w.onAllDesktops;
    var g = out.geometry;
    w.noBorder = true;
    w.onAllDesktops = true;
    w.demandsAttention = false;
    w.frameGeometry = {x: g.x, y: g.y, width: g.width, height: g.height};
    w.setMaximize(true, true);
    workspace.activeWindow = w;
    callDBus("org.remora.HostAgent", "/Placement", "org.remora.HostAgent.Placement", "placed",
             JSON.stringify({session: SESSION, id: w.internalId.toString(), pid: w.pid,
                             isNew: isNew, x: was.x, y: was.y, width: was.width,
                             height: was.height, border: border, allDesktops: allDesktops}));
}
(function () {
    var ws = workspace.windowList();
    for (var i = 0; i < ws.length; i++) {
        if (ws[i].desktopFileName != APP) continue;
        before[ws[i].internalId.toString()] = true;
        (function (w) {
            w.demandsAttentionChanged.connect(function () {
                if (w.demandsAttention && nothingPlaced() && ours(w)) place(w);
            });
        })(ws[i]);
    }
})();
(function () {
    var ws = workspace.windowList();
    for (var i = 0; i < ws.length; i++)
        if (MOVE.indexOf(ws[i].internalId.toString()) >= 0) place(ws[i]);
})();
workspace.windowAdded.connect(function (w) { if (ours(w)) place(w); });
workspace.windowActivated.connect(function (w) { if (nothingPlaced() && ours(w)) place(w); });
if (ADOPT) {
    var ws = workspace.windowList(), pick = null;
    for (var i = 0; i < ws.length && !pick; i++) if (ours(ws[i]) && ws[i].demandsAttention) pick = ws[i];
    if (!pick && ours(workspace.activeWindow)) pick = workspace.activeWindow;
    if (pick) place(pick);
}
// Whether the window with keyboard focus is one of the phone's: the agent holds the phone's keys
// while it is not. Connected after the placing handler, so a window placed on activation counts.
function reportFocus(w) {
    var id = w ? w.internalId.toString() : "";
    callDBus("org.remora.HostAgent", "/Placement", "org.remora.HostAgent.Placement", "focus",
             JSON.stringify({session: SESSION, id: id, ours: !!placed[id]}));
}
workspace.windowActivated.connect(reportFocus);
reportFocus(workspace.activeWindow);
workspace.windowRemoved.connect(function (w) {
    var id = w.internalId.toString();
    if (!placed[id]) return;
    delete placed[id];
    var left = 0;
    for (var k in placed) left++;
    callDBus("org.remora.HostAgent", "/Placement", "org.remora.HostAgent.Placement",
             "windowClosed", JSON.stringify({session: SESSION, id: id, last: left == 0}));
});
)JS";

// Gives the phone's window keyboard focus again, the one it had last if it is still there.
const char kActivateScript[] = R"JS(
var IDS = %1;
var ws = workspace.windowList(), pick = null;
for (var k = 0; k < IDS.length && !pick; k++)
    for (var i = 0; i < ws.length && !pick; i++)
        if (ws[i].internalId.toString() == IDS[k]) pick = ws[i];
if (pick) workspace.activeWindow = pick;
)JS";

// Gives windows back to the desk. Run once, after the session's script is unloaded. A window
// that was already on the desk goes back exactly as it was; one opened for the phone is minimized
// rather than closed — closing could cost unsaved work, and a prompt about it would appear on a
// desk nobody is at.
const char kReleaseScript[] = R"JS(
var BACK = %1;
var ws = workspace.windowList();
for (var i = 0; i < ws.length; i++) {
    var w = ws[i], b = BACK[w.internalId.toString()];
    if (!b) continue;
    w.setMaximize(false, false);
    w.onAllDesktops = b.allDesktops;
    if (b.isNew) {
        w.noBorder = false;
        w.minimized = true;
    } else {
        w.noBorder = !b.border;
        w.frameGeometry = {x: b.x, y: b.y, width: b.width, height: b.height};
    }
}
)JS";

}  // namespace

WindowPlacer::WindowPlacer(QObject *parent)
    : QObject(parent),
      scriptDir_(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
                 QStringLiteral("/remora-host-agent")) {
    QDir().mkpath(scriptDir_);
}

WindowPlacer::~WindowPlacer() {
    const auto sessions = windows_.keys();
    for (const quint32 s : sessions) release(s);
}

bool WindowPlacer::exportOnBus(QString *error) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        *error = QStringLiteral("no D-Bus session bus");
        return false;
    }
    if (!bus.registerService(kService)) {
        *error = QStringLiteral("%1 is already on the session bus — is another host agent running?")
                     .arg(kService);
        return false;
    }
    if (!bus.registerObject(kPath, this, QDBusConnection::ExportScriptableSlots)) {
        *error = QStringLiteral("could not export %1").arg(kPath);
        return false;
    }
    return true;
}

bool WindowPlacer::runScript(const QString &name, const QString &source, QString *error) {
    const QString path = scriptDir_ + QLatin1Char('/') + name + QStringLiteral(".js");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(source.toUtf8()) < 0) {
        *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    f.close();
    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"));
    unloadScript(name);  // a leftover of the same name would shadow this one
    const QDBusReply<int> id = scripting.call(QStringLiteral("loadScript"), path, name);
    if (!id.isValid() || id.value() < 0) {
        *error = QStringLiteral("KWin would not load the placement script: %1")
                     .arg(id.error().message());
        return false;
    }
    QDBusInterface script(QStringLiteral("org.kde.KWin"),
                          QStringLiteral("/Scripting/Script%1").arg(id.value()),
                          QStringLiteral("org.kde.kwin.Script"));
    const QDBusMessage run = script.call(QStringLiteral("run"));
    if (run.type() == QDBusMessage::ErrorMessage) {
        *error = QStringLiteral("KWin would not run the placement script: %1")
                     .arg(run.errorMessage());
        return false;
    }
    return true;
}

void WindowPlacer::unloadScript(const QString &name) {
    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"));
    scripting.call(QStringLiteral("unloadScript"), name);
}

bool WindowPlacer::watch(quint32 session, const QString &appId, const QString &output,
                         QString *error) {
    windows_.insert(session, {});
    watched_.insert(session, {appId, output});
    return runScript(scriptName(session), placeScript(session, false), error);
}

QString WindowPlacer::placeScript(quint32 session, bool adopt, const QStringList &move) const {
    const auto [appId, output] = watched_.value(session);
    const QString moveJs = QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(move)).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(kPlaceScript)
        .arg(session)
        .arg(jsString(appId), jsString(output),
             adopt ? QStringLiteral("true") : QStringLiteral("false"), moveJs);
}

void WindowPlacer::moveTo(quint32 session, const QString &output) {
    auto it = watched_.find(session);
    if (it == watched_.end()) return;
    it->second = output;
    QString error;
    if (!runScript(scriptName(session), placeScript(session, false, windows_.value(session).keys()),
                   &error))
        qWarning("host agent: %s", qUtf8Printable(error));
}

void WindowPlacer::adoptIfUnplaced(quint32 session) {
    const auto it = windows_.constFind(session);
    if (it == windows_.constEnd() || !it->isEmpty()) return;  // gone, or already answered
    QString error;
    if (!runScript(scriptName(session), placeScript(session, true), &error))
        qWarning("host agent: %s", qUtf8Printable(error));
}

void WindowPlacer::activate(quint32 session) {
    const auto it = windows_.constFind(session);
    if (it == windows_.constEnd() || it->isEmpty()) return;
    QStringList ids;
    const QString last = lastFocused_.value(session);
    if (it->contains(last)) ids << last;
    for (auto w = it->cbegin(); w != it->cend(); ++w)
        if (w.key() != last) ids << w.key();
    runOnce(scriptName(session) + QStringLiteral("-activate"),
            QString::fromUtf8(kActivateScript)
                .arg(QString::fromUtf8(
                    QJsonDocument(QJsonArray::fromStringList(ids)).toJson(QJsonDocument::Compact))));
}

void WindowPlacer::runOnce(const QString &name, const QString &source) {
    QString error;
    if (!runScript(name, source, &error)) {
        qWarning("host agent: %s", qUtf8Printable(error));
        return;
    }
    // KWin reads a script's file off its own thread and evaluates it later; unloading it at once
    // can beat the evaluation. It has nothing to listen for afterwards, so a second is plenty.
    QTimer::singleShot(1000, this, [this, name] { unloadScript(name); });
}

void WindowPlacer::release(quint32 session) {
    unloadScript(scriptName(session));
    watched_.remove(session);
    lastFocused_.remove(session);
    const QHash<QString, Placed> windows = windows_.take(session);
    if (windows.isEmpty()) return;
    QJsonObject back;
    for (auto it = windows.cbegin(); it != windows.cend(); ++it) {
        back.insert(it.key(), QJsonObject{{QStringLiteral("isNew"), it->isNew},
                                          {QStringLiteral("x"), it->was.x()},
                                          {QStringLiteral("y"), it->was.y()},
                                          {QStringLiteral("width"), it->was.width()},
                                          {QStringLiteral("height"), it->was.height()},
                                          {QStringLiteral("border"), it->border},
                                          {QStringLiteral("allDesktops"), it->allDesktops}});
    }
    runOnce(scriptName(session) + QStringLiteral("-release"),
            QString::fromUtf8(kReleaseScript)
                .arg(QString::fromUtf8(QJsonDocument(back).toJson(QJsonDocument::Compact))));
}

// The script sends one JSON string per call: callDBus passes JavaScript numbers as whatever
// variant the engine holds — an int for one value, a double for a geometry — so a typed
// signature would match some calls and silently drop others.
void WindowPlacer::placed(const QString &json) {
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const quint32 session = quint32(o.value(QStringLiteral("session")).toInt());
    const QString id = o.value(QStringLiteral("id")).toString();
    auto it = windows_.find(session);
    if (it == windows_.end() || id.isEmpty()) return;  // a session already released
    const QRect was(int(o.value(QStringLiteral("x")).toDouble()),
                    int(o.value(QStringLiteral("y")).toDouble()),
                    int(o.value(QStringLiteral("width")).toDouble()),
                    int(o.value(QStringLiteral("height")).toDouble()));
    // A window re-placed by a reflow keeps what was recorded when it first came to the phone:
    // that is the state the release must give back.
    if (it->contains(id)) return;
    const bool isNew = o.value(QStringLiteral("isNew")).toBool();
    it->insert(id, {isNew, was, o.value(QStringLiteral("border")).toBool(true),
                    o.value(QStringLiteral("allDesktops")).toBool(false)});
    emit windowPlaced(session, id, o.value(QStringLiteral("pid")).toInt(), isNew);
}

void WindowPlacer::focus(const QString &json) {
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const quint32 session = quint32(o.value(QStringLiteral("session")).toInt());
    if (!watched_.contains(session)) return;
    const bool ours = o.value(QStringLiteral("ours")).toBool();
    if (ours) lastFocused_.insert(session, o.value(QStringLiteral("id")).toString());
    emit focusChanged(session, ours);
}

void WindowPlacer::windowClosed(const QString &json) {
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const quint32 session = quint32(o.value(QStringLiteral("session")).toInt());
    auto it = windows_.find(session);
    if (it == windows_.end()) return;
    it->remove(o.value(QStringLiteral("id")).toString());
    if (o.value(QStringLiteral("last")).toBool()) emit allWindowsClosed(session);
}

}  // namespace remora::hostagent
