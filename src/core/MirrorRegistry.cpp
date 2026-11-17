#include "core/MirrorRegistry.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace remora {

namespace {
struct KindName {
    MirrorKind kind;
    const char *name;
};
constexpr KindName kKinds[] = {{MirrorKind::Main, "main"},
                               {MirrorKind::Splash, "splash"},
                               {MirrorKind::Pip, "pip"},
                               {MirrorKind::App, "app"},
                               {MirrorKind::Desktop, "desktop"}};
constexpr int kEntryVersion = 1;
}  // namespace

QString mirrorKindName(MirrorKind k) {
    for (const KindName &kn : kKinds)
        if (kn.kind == k) return QLatin1String(kn.name);
    return QStringLiteral("main");
}

std::optional<MirrorKind> parseMirrorKind(const QString &name) {
    for (const KindName &kn : kKinds)
        if (name == QLatin1String(kn.name)) return kn.kind;
    return std::nullopt;
}

MirrorKind mirrorKindFor(bool newDisplay, bool bootAnimation, const QString &appId,
                         const QString &title) {
    if (appId == kPipAppId) return MirrorKind::Pip;
    if (newDisplay)
        return title.startsWith(kDesktopTitlePrefix) ? MirrorKind::Desktop : MirrorKind::App;
    return bootAnimation ? MirrorKind::Splash : MirrorKind::Main;
}

QString mirrorSessionName(MirrorSessionState s) {
    switch (s) {
        case MirrorSessionState::Pending: return QStringLiteral("pending");
        case MirrorSessionState::Live: return QStringLiteral("live");
        case MirrorSessionState::Lost: return QStringLiteral("lost");
        case MirrorSessionState::Unknown: break;
    }
    return QStringLiteral("unknown");
}

static MirrorSessionState parseMirrorSession(const QString &s) {
    if (s == QLatin1String("pending")) return MirrorSessionState::Pending;
    if (s == QLatin1String("live")) return MirrorSessionState::Live;
    if (s == QLatin1String("lost")) return MirrorSessionState::Lost;
    return MirrorSessionState::Unknown;
}

QByteArray encodeMirrorEntry(const MirrorEntry &e) {
    QJsonObject o;
    o.insert(QStringLiteral("v"), kEntryVersion);
    o.insert(QStringLiteral("pid"), double(e.pid));
    o.insert(QStringLiteral("serial"), e.serial);
    o.insert(QStringLiteral("title"), e.title);
    o.insert(QStringLiteral("kind"), mirrorKindName(e.kind));
    o.insert(QStringLiteral("started_ms"), double(e.startedMs));
    o.insert(QStringLiteral("session"), mirrorSessionName(e.session));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

std::optional<MirrorEntry> decodeMirrorEntry(const QByteArray &json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject o = doc.object();
    MirrorEntry e;
    e.pid = qint64(o.value(QStringLiteral("pid")).toDouble());
    e.serial = o.value(QStringLiteral("serial")).toString();
    if (e.pid <= 0 || e.serial.isEmpty()) return std::nullopt;
    e.title = o.value(QStringLiteral("title")).toString();
    // An unknown kind from a newer client is still a mirror on that target; Main is the kind
    // every target-wide lookup treats like the rest, and the instance lookups need a title match
    // on top, so the guess costs nothing it should not.
    e.kind = parseMirrorKind(o.value(QStringLiteral("kind")).toString()).value_or(MirrorKind::Main);
    e.startedMs = qint64(o.value(QStringLiteral("started_ms")).toDouble());
    e.session = parseMirrorSession(o.value(QStringLiteral("session")).toString());
    e.registered = true;
    return e;
}

std::optional<MirrorEntry> mirrorEntryFromArgv(const QStringList &argv) {
    // `remora mirror …`, by argv[0]'s file name rather than by a substring of the joined command
    // line: that is what kept a `sh -c "… remora mirror …"` wrapper — pgrep's own, famously —
    // from matching itself.
    if (argv.size() < 2 || argv[1] != QLatin1String("mirror")) return std::nullopt;
    // The file name by hand, not by QFileInfo: the core stays off the filesystem, and argv[0] may
    // be either platform's spelling of a path.
    const QString exe = argv[0].section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
    if (!exe.endsWith(QLatin1String("remora")) && !exe.endsWith(QLatin1String("remora.exe")))
        return std::nullopt;
    MirrorEntry e;
    e.registered = false;
    bool newDisplay = false, bootAnimation = false;
    QString appId;
    bool titled = false;
    // Both spellings QCommandLineParser takes, `--opt value` and `--opt=value`: the builders use
    // the first for -s and --window-title and the second for nearly everything else.
    const auto value = [&](int &i, const QString &a, const QString &longName,
                           const QString &shortName, QString *out) {
        if (a == longName || (!shortName.isEmpty() && a == shortName)) {
            if (i + 1 < argv.size()) *out = argv[++i];
            return true;
        }
        if (a.startsWith(longName + QLatin1Char('='))) {
            *out = a.mid(longName.size() + 1);
            return true;
        }
        return false;
    };
    for (int i = 2; i < argv.size(); ++i) {
        const QString &a = argv[i];
        QString v;
        if (a == QLatin1String("--list-apps")) return std::nullopt;  // a one-shot, not a client
        if (value(i, a, QStringLiteral("--serial"), QStringLiteral("-s"), &v)) {
            e.serial = v;
        } else if (value(i, a, QStringLiteral("--window-title"), QString(), &v)) {
            e.title = v;
            titled = true;
        } else if (value(i, a, QStringLiteral("--app-id"), QString(), &v)) {
            appId = v;
        } else if (a == QLatin1String("--new-display")
                   || a.startsWith(QLatin1String("--new-display="))) {
            newDisplay = true;
        } else if (a == QLatin1String("--boot-animation")) {
            bootAnimation = true;
        }
    }
    if (e.serial.isEmpty()) return std::nullopt;
    // The title the mirror itself falls back to (MirrorApp), so an untitled legacy session reads
    // the same whether it registered or was found by its command line.
    if (!titled) e.title = QStringLiteral("Remora mirror — %1").arg(e.serial);
    e.kind = mirrorKindFor(newDisplay, bootAnimation, appId, e.title);
    return e;
}

bool isInstanceMirrorTitle(const QString &entryTitle, const QString &windowTitle) {
    static const QLatin1String kLegacy("Remora:");
    QString inst = windowTitle;
    if (inst.startsWith(kLegacy)) inst = inst.mid(kLegacy.size());
    if (inst.isEmpty()) return entryTitle.startsWith(kLegacy);
    return entryTitle == inst || entryTitle == kLegacy + inst;
}

QVector<MirrorEntry> selectMirrors(const QVector<MirrorEntry> &all, const MirrorQuery &q) {
    QVector<MirrorEntry> out;
    for (const MirrorEntry &e : all) {
        if (!q.target.isEmpty() && e.serial != q.target) continue;
        if (!q.kinds.isEmpty() && !q.kinds.contains(e.kind)) continue;
        if (q.instanceTitle && !isInstanceMirrorTitle(e.title, *q.instanceTitle)) continue;
        if (!q.exactTitle.isEmpty() && e.title != q.exactTitle) continue;
        out << e;
    }
    return out;
}

QVector<MirrorEntry> instanceMirrors(const QVector<MirrorEntry> &all, const QString &target,
                                     const QString &windowTitle) {
    // No target, no instance: the old pattern with an empty serial matched every mirror on the
    // machine, which no caller ever meant.
    if (target.isEmpty()) return {};
    MirrorQuery q;
    q.target = target;
    q.kinds = {MirrorKind::Main, MirrorKind::Splash};
    q.instanceTitle = windowTitle;
    return selectMirrors(all, q);
}

QList<qint64> mirrorPids(const QVector<MirrorEntry> &es) {
    QList<qint64> pids;
    for (const MirrorEntry &e : es)
        if (e.pid > 0 && !pids.contains(e.pid)) pids << e.pid;
    return pids;
}

QVector<MirrorEntry> selfReportedHusks(const QVector<MirrorEntry> &es, qint64 nowMs,
                                       qint64 minAgeMs) {
    QVector<MirrorEntry> out;
    for (const MirrorEntry &e : es) {
        if (!e.registered || e.session != MirrorSessionState::Lost) continue;
        if (e.startedMs <= 0 || nowMs - e.startedMs < minAgeMs) continue;
        out << e;
    }
    return out;
}

QString mirrorStopEventName(qint64 pid) {
    return QStringLiteral("Local\\remora-mirror-stop-%1").arg(pid);
}

}  // namespace remora
