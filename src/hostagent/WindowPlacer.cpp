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
// there is pixels the phone pays for, and because a tiling script leaves a maximized window alone —
// unless the phone asked for a smaller window (WIDTH and HEIGHT, percentages of the output's
// sides) or a desktop one (DESKTOP): then centred at that size, with its title bar, which the
// phone's pointer can move it by and resize it from, and which a tiling script arranges as it
// would any window of the desk's; on every virtual desktop and every activity, because both
// are global across outputs and switching either at the desk must not take the app off the phone
// (a window left on one activity vanished from the phone when the desk switched to another, and
// the phone showed the empty desktop).
//
// Which window answered the launch:
//  - a new one — windowAdded, or desktopFileNameChanged for a Wayland window that is added
//    before it names its app; never at its activation, which comes before windowAdded;
//  - an existing one the launch activated — windowActivated;
//  - an existing one of a single-instance app — the launch asks it to activate, and KWin's
//    focus-stealing prevention usually refuses (the desk is busy with something else), marking it
//    demandsAttention instead, which is what is watched;
//  - ADOPT, the second load, when a couple of seconds passed with no answer: the app's window that
//    demands attention, or the active one if it is the app's. Never any other: a slow-starting
//    app's other windows on the desk stay where they are.
//
// Each report says whether the window is new ("not among the app's windows when the script
// loaded") and how it was before, so the release can put a desk window back exactly, and the
// smallest it can be: KWin keeps a window that cannot fit on an output off it, so the agent
// lowers the output's scale until it does.
const char kPlaceScript[] = R"JS(
var SESSION = %1, APP = %2, OUTPUT = %3, ADOPT = %4, MOVE = %5, WIDTH = %6, HEIGHT = %7,
    DESKTOP = %8;
var placed = {}, before = {};
function output() {
    var s = workspace.screens;
    for (var i = 0; i < s.length; i++) if (s[i].name == OUTPUT) return s[i];
    return null;
}
function ours(w) { return w && w.desktopFileName == APP && w.normalWindow; }
function nothingPlaced() { for (var k in placed) return false; return true; }
// The output filled, maximized and borderless, or WIDTH and HEIGHT percent of its sides, centred:
// never below the window's own minimum, never past the output. Sent to the output through KWin
// first: a geometry set from a script is applied only once the app answers it, so a maximize asked
// for meanwhile was done on the output the window was still on (seen: maximized on the desk, where
// a tiling script then sized it as a tile).
function filled() { return WIDTH >= 100 && HEIGHT >= 100 && !DESKTOP; }
function fit(w, out) {
    var g = out.geometry;
    if (!w.output || w.output.name != out.name) workspace.sendClientToScreen(w, out);
    if (filled()) {
        w.noBorder = true;
        // Maximized already, and sized since by a tiling script that did not hear of it (below):
        // asking again is no change to KWin, so it is said again by way of maximized vertically,
        // never unmaximized, which would put the window back where it opened.
        if (w.maximizeMode == 3) w.setMaximize(true, false);
        w.setMaximize(true, true);
        return;
    }
    w.noBorder = false;
    var width = Math.min(g.width, Math.max(Math.round(g.width * WIDTH / 100), w.minSize.width));
    var height = Math.min(g.height, Math.max(Math.round(g.height * HEIGHT / 100), w.minSize.height));
    w.frameGeometry = {x: g.x + Math.round((g.width - width) / 2),
                       y: g.y + Math.round((g.height - height) / 2), width: width, height: height};
}
// Whether the window is as fit() left it: on the output, and filling it when it is to.
function settled(w, out) {
    if (!w.output || w.output.name != OUTPUT) return false;
    if (!filled()) return true;
    var f = w.frameGeometry, g = workspace.clientArea(KWin.MaximizeArea, out, workspace.currentDesktop);
    return Math.abs(f.x - g.x) < 1 && Math.abs(f.y - g.y) < 1 && Math.abs(f.width - g.width) < 1 &&
           Math.abs(f.height - g.height) < 1;
}
// A tiling script arranges a new window after this one placed it. KWin tells a script like this
// one of the window before a declarative one like Krohnkite, which then finds it unmaximized (KWin
// reports a maximize once the app has answered it) and has missed the maximize; it tiles the
// window, and holds it to the tile. Its geometry, set before the app answered the one asked here,
// is the one applied — on the desk, where the window opened (it never left), or as a tile on this
// output, maximized but with the desktop around it (seen both). While the session holds it, a
// window found otherwise once its maximize has landed is placed again — three times at most, so
// a script that keeps taking it back is not fought forever.
function keep(w) {
    var tries = 0;
    function check() {
        var out = output();
        if (!out || !placed[w.internalId.toString()] || tries >= 3 || settled(w, out)) return;
        if (filled() && w.output && w.output.name == OUTPUT && w.maximizeMode != 3) return;
        tries++;
        fit(w, out);
    }
    w.outputChanged.connect(check);
    w.frameGeometryChanged.connect(check);
    w.maximizedChanged.connect(check);
}
function place(w) {
    var out = output();
    if (!out || placed[w.internalId.toString()]) return;
    // Marked first: activating it below re-enters the windowActivated handler.
    placed[w.internalId.toString()] = true;
    var b = before[w.internalId.toString()], isNew = !b;
    var minimized = w.minimized || !!(b && b.minimized), maximized = w.maximizeMode;
    var border = !w.noBorder, allDesktops = w.onAllDesktops;
    var activities = [];  // none: every activity
    for (var i = 0; i < w.activities.length; i++) activities.push(String(w.activities[i]));
    w.minimized = false;
    // Restored first: KWin can already hold the window maximized, at the size it had (one that
    // did not fit the output, placed again once the scale was lowered: 20 px short each side,
    // measured with Elisa), and takes setMaximize(true) as no change. A desk window maximized
    // there is then at its own size, which is the one to record.
    w.setMaximize(false, false);
    var was = w.frameGeometry;
    w.onAllDesktops = true;
    w.activities = [];
    w.demandsAttention = false;
    fit(w, out);
    keep(w);
    // Focused when it first comes to the phone, the app the phone just opened. Not when it is
    // placed again (a reflow, a new window size): that would take focus from the desk, where
    // someone may be working, at every turn of the phone.
    if (MOVE.indexOf(w.internalId.toString()) < 0) workspace.activeWindow = w;
    callDBus("org.remora.HostAgent", "/Placement", "org.remora.HostAgent.Placement", "placed",
             JSON.stringify({session: SESSION, id: w.internalId.toString(), pid: w.pid,
                             isNew: isNew, x: was.x, y: was.y, width: was.width,
                             height: was.height, border: border, allDesktops: allDesktops,
                             minimized: minimized, maximized: maximized, activities: activities,
                             minWidth: w.minSize.width, minHeight: w.minSize.height}));
}
(function () {
    var ws = workspace.windowList();
    for (var i = 0; i < ws.length; i++) {
        if (ws[i].desktopFileName != APP) continue;
        // As it was before the launch: an app asked to activate its window has KWin unminimize
        // it before the window is placed.
        before[ws[i].internalId.toString()] = {minimized: ws[i].minimized};
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
// Windows KWin has announced. It activates a new Wayland window before it announces it, and one
// placed then was placed too early: KWin's own placement and a tiling script, told of it only
// afterwards, take it as they find it (seen: sent nowhere, and tiled on the desk by Krohnkite,
// which read it as unmaximized and missed the maximize).
var known = {};
(function () {
    var ws = workspace.windowList();
    for (var i = 0; i < ws.length; i++) known[ws[i].internalId.toString()] = true;
})();
workspace.windowAdded.connect(function (w) {
    known[w.internalId.toString()] = true;
    if (ours(w)) return place(w);
    // A Wayland window can be announced before it names its app.
    if (w.normalWindow) w.desktopFileNameChanged.connect(function () { if (ours(w)) place(w); });
});
workspace.windowActivated.connect(function (w) {
    if (w && known[w.internalId.toString()] && nothingPlaced() && ours(w)) place(w);
});
if (ADOPT) {
    var ws = workspace.windowList(), pick = null;
    for (var i = 0; i < ws.length && !pick; i++) if (ours(ws[i]) && ws[i].demandsAttention) pick = ws[i];
    if (!pick && ours(workspace.activeWindow)) pick = workspace.activeWindow;
    if (pick) place(pick);
}
// Whether the window with keyboard focus is the phone's: one placed here, or any other on the
// phone's screen (a dialog, which is not placed, opens over its window there) except the
// desktop's own, where typing would start a search on the desk. The agent types the phone's keys
// into nothing else. Connected after the placing handler, so a window placed on activation counts.
function onOutput(w) {
    return !!w && !!w.output && w.output.name == OUTPUT && !w.desktopWindow && !w.dock;
}
function reportFocus(w) {
    var id = w ? w.internalId.toString() : "";
    callDBus("org.remora.HostAgent", "/Placement", "org.remora.HostAgent.Placement", "focus",
             JSON.stringify({session: SESSION, id: id, ours: !!placed[id] || onOutput(w)}));
}
workspace.windowActivated.connect(reportFocus);
reportFocus(workspace.activeWindow);
// Where the desk's pointer was when the phone took it onto this output: the agent puts it back
// when the phone lets go of an output that stands apart, which the pointer cannot leave by itself.
var deskPointer = null, pointerHere = false;
function pointerMoved() {
    var out = output(), p = workspace.cursorPos;
    var g = out ? out.geometry : null;
    if (!g || p.x < g.x || p.y < g.y || p.x >= g.x + g.width || p.y >= g.y + g.height) {
        deskPointer = {x: p.x, y: p.y};
        pointerHere = false;
        return;
    }
    if (pointerHere) return;
    pointerHere = true;
    if (deskPointer)
        callDBus("org.remora.HostAgent", "/Placement", "org.remora.HostAgent.Placement",
                 "pointerEntered", JSON.stringify({session: SESSION, x: deskPointer.x, y: deskPointer.y}));
}
workspace.cursorPosChanged.connect(pointerMoved);
pointerMoved();
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

// Gives windows back to the desk. Run once, after the session's script is unloaded. A window
// that was already on the desk goes back exactly as it was — where it was, then maximized and
// minimized again if it had been (maximized there, so its own size is the one it restores to);
// one opened for the phone is minimized rather than closed — closing could cost unsaved work,
// and a prompt about it would appear on a desk nobody is at.
const char kReleaseScript[] = R"JS(
var BACK = %1;
var ws = workspace.windowList();
for (var i = 0; i < ws.length; i++) {
    var w = ws[i], b = BACK[w.internalId.toString()];
    if (!b) continue;
    w.setMaximize(false, false);
    w.onAllDesktops = b.allDesktops;
    w.activities = b.activities;
    if (b.isNew) {
        w.noBorder = false;
        w.minimized = true;
    } else {
        w.noBorder = !b.border;
        w.frameGeometry = {x: b.x, y: b.y, width: b.width, height: b.height};
        if (b.maximized) w.setMaximize((b.maximized & 1) != 0, (b.maximized & 2) != 0);
        w.minimized = b.minimized;
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
                         Window window, QString *error) {
    windows_.insert(session, {});
    watched_.insert(session, {appId, output, window});
    return runScript(scriptName(session), placeScript(session, false), error);
}

QString WindowPlacer::placeScript(quint32 session, bool adopt, const QStringList &move) const {
    const Watched w = watched_.value(session);
    const QString moveJs = QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(move)).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(kPlaceScript)
        .arg(session)
        .arg(jsString(w.appId), jsString(w.output),
             adopt ? QStringLiteral("true") : QStringLiteral("false"), moveJs,
             QString::number(w.window.width), QString::number(w.window.height),
             w.window.desktop ? QStringLiteral("true") : QStringLiteral("false"));
}

void WindowPlacer::moveTo(quint32 session, const QString &output) {
    auto it = watched_.find(session);
    if (it == watched_.end()) return;
    it->output = output;
    replace(session);
}

void WindowPlacer::setWindow(quint32 session, Window window) {
    auto it = watched_.find(session);
    if (it == watched_.end() ||
        (it->window.width == window.width && it->window.height == window.height &&
         it->window.desktop == window.desktop))
        return;
    it->window = window;
    replace(session);
}

void WindowPlacer::replace(quint32 session) {
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
                                          {QStringLiteral("allDesktops"), it->allDesktops},
                                          {QStringLiteral("minimized"), it->minimized},
                                          {QStringLiteral("maximized"), it->maximized},
                                          {QStringLiteral("activities"),
                                           QJsonArray::fromStringList(it->activities)}});
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
    const QSize minimum(int(o.value(QStringLiteral("minWidth")).toDouble()),
                        int(o.value(QStringLiteral("minHeight")).toDouble()));
    if (minimum.width() > 0 && minimum.height() > 0) emit windowMinimum(session, minimum);
    // A window re-placed by a reflow keeps what was recorded when it first came to the phone:
    // that is the state the release must give back.
    if (it->contains(id)) return;
    const bool isNew = o.value(QStringLiteral("isNew")).toBool();
    QStringList activities;
    for (const QJsonValue &a : o.value(QStringLiteral("activities")).toArray())
        activities << a.toString();
    it->insert(id, {isNew, was, o.value(QStringLiteral("border")).toBool(true),
                    o.value(QStringLiteral("allDesktops")).toBool(false),
                    o.value(QStringLiteral("minimized")).toBool(false),
                    o.value(QStringLiteral("maximized")).toInt(), activities});
    emit windowPlaced(session, id, o.value(QStringLiteral("pid")).toInt(), isNew);
}

void WindowPlacer::focus(const QString &json) {
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const quint32 session = quint32(o.value(QStringLiteral("session")).toInt());
    if (!watched_.contains(session)) return;
    emit focusChanged(session, o.value(QStringLiteral("ours")).toBool());
}

void WindowPlacer::pointerEntered(const QString &json) {
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const quint32 session = quint32(o.value(QStringLiteral("session")).toInt());
    if (!watched_.contains(session)) return;
    emit deskPointer(session, QPointF(o.value(QStringLiteral("x")).toDouble(),
                                      o.value(QStringLiteral("y")).toDouble()));
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
