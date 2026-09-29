#include "ui/MainWindow.h"
#include "engine/AppTransfer.h"
#include <algorithm>
#ifdef Q_OS_UNIX
#include <csignal>   // ::killpg
#include <unistd.h>  // ::setpgid, ::getuid
#endif

#include <QAction>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QCursor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileSystemWatcher>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QIconEngine>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QEventLoop>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QFile>
#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>

#include "core/Builders.h"  // buildStorageAclArgv — the storage ACL grant behind pkexec
#include "core/Features.h"
#include "core/Gating.h"
#include "core/ImageBuild.h"
#include "core/MirrorInput.h"  // the SDL key-name table the mirror client parses key_bind with
#include "core/Parsers.h"
#include "engine/SourceBuild.h"
#include "core/SourcePatches.h"
#include "store/Store.h"
#include "ui/RedditLoginWindow.h"
#include "ui/Workers.h"

namespace remora {
namespace {

// The desktop accent. QPalette::Accent is Qt 6.6+; older Qt has no such role, and Highlight is
// the nearest thing it does have.
QColor paletteAccent(const QPalette &pal) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return pal.color(QPalette::Accent);
#else
    return pal.color(QPalette::Highlight);
#endif
}

// Paint-time accent tinting: the engine reads the CURRENT palette accent on every render, so
// a wallpaper-driven accent change recolors every glyph on the next repaint — no baked
// pixmaps to go stale. Disabled renders as the same accent, faded.
class AccentIconEngine : public QIconEngine {
public:
    explicit AccentIconEngine(QIcon base) : base_(std::move(base)) {}
    static QColor accent() {
        QColor ac = paletteAccent(qApp->palette());
        if (qApp->palette().color(QPalette::Window).lightness() < 128) ac = ac.lighter(150);
        return ac;
    }
    void paint(QPainter *p, const QRect &r, QIcon::Mode mode, QIcon::State state) override {
        p->drawPixmap(r, pixmap(r.size(), mode, state));
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QColor ac = accent();
        if (mode == QIcon::Disabled) ac.setAlpha(105);
        const quint64 key = (quint64(size.width()) << 40) ^ (quint64(int(mode)) << 32) ^ ac.rgba();
        const auto it = cache_.constFind(key);
        if (it != cache_.constEnd()) return *it;
        QPixmap px = base_.pixmap(size, QIcon::Normal, state);
        if (!px.isNull()) {
            QPainter t(&px);
            t.setCompositionMode(QPainter::CompositionMode_SourceIn);
            t.fillRect(px.rect(), ac);
        }
        cache_.insert(key, px);
        return px;
    }
    QIconEngine *clone() const override { return new AccentIconEngine(base_); }

private:
    QIcon base_;
    QHash<quint64, QPixmap> cache_;
};

// Themed icon with fallbacks (first theme match wins) — Breeze names only, no emoji.
// Tinted + cached: themeIcon is called from per-second progress updates.
QIcon themeIcon(std::initializer_list<const char *> names) {
    static QHash<QString, QIcon> cache;
    QString key;
    for (const char *n : names) key += QLatin1String(n) + QLatin1Char('|');
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    QIcon found;
    for (const char *n : names) {
        const QIcon ic = QIcon::fromTheme(QLatin1String(n));
        if (!ic.isNull()) {
            found = QIcon(new AccentIconEngine(ic));  // engine reads the live accent per paint
            break;
        }
    }
    cache.insert(key, found);
    return found;
}

// ── image sources ──
// No built-ins at all (bd remora-2ylt). The upstream-image / erstt catalogs are RETIRED —
// dead upstreams, and the Image-page rework had already removed the selector that consumed
// their tables. 'Self-built' went with them: it was a checkbox that gated nothing, because the
// library lists the build host's archives and docker images UNCONDITIONALLY — self-built images
// always show, no source entry required. What remains is purely user-configured: the mirror and
// friends, added as location sources. Stale [Source-…] records the old built-ins left in
// remorarc are dropped by loadImageSources (tagless, locationless records describe nothing).

// script(1) runs its -c command through $SHELL — on a fish login shell that breaks every
// POSIX construct. Pin the child's SHELL to /bin/sh for the pty-wrapped spawns.


// Wait for a QProcess WITHOUT wedging the GUI when we happen to be on the main thread.
//
// runShell/runShellRc are documented "worker thread only" and fourteen call sites invoke them from
// the GUI thread regardless. onBuildFromSource is the worst: it runs `df`, then `du -sk <tree>/out`,
// BEFORE it prompts — and that `du` measures 7.0s on the A16 tree's 110 GiB out/ with a warm cache
// and an idle disk, against a 60s timeout. QProcess::waitForFinished polls its OWN fd set, not the
// application's, so for that entire window the main loop dispatches nothing: no repaints, no D-Bus,
// and the compositor marks the window "not responding" (bd remora-51z).
//
// Off the main thread nothing is at stake, so block plainly. On the main thread run a nested event
// loop instead, so repaints and D-Bus keep flowing and the window stays alive to the compositor.
// ExcludeUserInputEvents is what makes the nesting safe — input is queued rather than delivered, so
// the handler that called us cannot be re-entered by a second click. This is strictly MORE
// restrictive than what these same handlers already do: every QMessageBox in them nests an event
// loop that does not exclude input.
static bool waitForProcess(QProcess &p, int timeoutMs) {
    if (QThread::currentThread() != QApplication::instance()->thread())
        return p.waitForFinished(timeoutMs);
    if (p.state() == QProcess::NotRunning) return true;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&p, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (timeoutMs > 0) timer.start(timeoutMs);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return p.state() == QProcess::NotRunning;
}

// Run a shell command on the build host (ssh) or locally, returning stdout. Blocking, but the main
// loop keeps turning when called from the GUI thread (see waitForProcess). Empty host = local.
// Remote commands are POSIX sh — but "ssh host cmd" hands cmd to the login shell, which may
// be fish (ours is) and reject $(...)/[..]/{..} syntax. Run a remote `sh` and feed the command
// over stdin instead: no login-shell parsing, no quoting games.
QString runShell(const QString &host, const QString &cmd, int timeoutMs = 120000) {
    QProcess p;
    if (host.isEmpty()) {
        p.start(QStringLiteral("sh"), {QStringLiteral("-c"), cmd});
    } else {
        p.start(QStringLiteral("ssh"),
                {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                 QStringLiteral("-o"), QStringLiteral("ConnectTimeout=6"), host,
                 QStringLiteral("sh")});
        p.write(cmd.toUtf8() + '\n');
        p.closeWriteChannel();
    }
    if (!waitForProcess(p, timeoutMs) || p.exitCode() != 0) return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
}

// A killed or failed 'repo sync' leaves three kinds of wreckage that break the next sync:
// stale git lock files (index.lock left by a checkout that died holding it), worktrees that
// were mid-checkout (files on disk but no .git link), and half-created project gitdirs
// (config but no HEAD → "fatal: not a git repository"). Sweep all three — fetched objects live under .repo/projects/, so nothing is lost; the next
// sync re-checks-out. Only call when no sync is running: a live sync legitimately holds locks.
// Returns how many worktrees were removed (0 also when not a repo client or the host is down).
int cleanupStaleWorktrees(const QString &host, const QString &tree) {
    if (tree.isEmpty()) return 0;
    const QString out = runShell(
        host,
        QStringLiteral(
            "cd %1 2>/dev/null || exit 0; [ -d .repo/projects ] || exit 0; "
            "find .repo/projects -type f -name '*.lock' -delete 2>/dev/null; "
            "find .repo/projects -type d -name '*.git' -prune -print | while read -r g; do "
            "p=\"${g#.repo/projects/}\"; p=\"${p%.git}\"; "
            "if [ ! -f \"$g/HEAD\" ]; then rm -rf \"$g\" \"$p\" && echo \"$p\"; "
            "elif [ -d \"$p\" ] && [ ! -e \"$p/.git\" ]; then rm -rf \"$p\" && echo \"$p\"; fi; "
            "done | wc -l")
            .arg(tree),
        60000);
    return out.trimmed().toInt();
}

// runShell with the truth attached: exit code + merged output. runShell() returns an empty
// string on failure, which callers must never mistake for "no errors printed".
struct ShellResult {
    int rc;
    QString out;
};
ShellResult runShellRc(const QString &host, const QString &cmd, int timeoutMs = 120000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    if (host.isEmpty()) {
        p.start(QStringLiteral("sh"), {QStringLiteral("-c"), cmd});
    } else {
        p.start(QStringLiteral("ssh"),
                {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                 QStringLiteral("-o"), QStringLiteral("ConnectTimeout=6"), host,
                 QStringLiteral("sh")});
        p.write(cmd.toUtf8() + '\n');
        p.closeWriteChannel();
    }
    if (!waitForProcess(p, timeoutMs)) {
        p.kill();
        return {124, QString::fromUtf8(p.readAll())};
    }
    const int rc = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 128;
    return {rc, QString::fromUtf8(p.readAll())};
}

// Total size in bytes of a directory tree, on the build host or locally. -1 if it doesn't exist.
// Ten minutes, not one: every caller is off the GUI thread (the library scan, reclaimDir), and a
// cold-cache `du` over the 1.59 M entries of an AOSP out/ tree can outrun 60 s — at which point
// -1 read as "no build output" while 285 GB sat there (bd remora-h691's twin).
qint64 dirSizeBytes(const QString &host, const QString &dir) {
    const QString out =
        runShell(host, QStringLiteral("du -sb %1 2>/dev/null | cut -f1").arg(dir), 600000);
    bool ok = false;
    const qint64 n = out.trimmed().toLongLong(&ok);
    return ok ? n : -1;
}

// Docker image archives (docker save output) in the output dir on the build host (or locally).
// The repo tag comes from the archive's manifest.json (RepoTags[0]); reading it streams the whole
// multi-GB tar, so it's cached by (host, path, size, mtime).
struct ImageArchive {
    QString path;
    QString tag;  // empty if the manifest was unreadable
    qint64 size = 0;
    qint64 mtime = 0;  // epoch seconds (the .tar mtime = when it was saved)
};
// Remora docker images on the build host (or locally): repo:tag + human size + build date.
// These are directly deployable (the backend runs them), unlike .tar archives which must be
// docker-loaded.
struct DockerImage {
    QString tag;
    QString size;
    QString created;    // "YYYY-MM-DD HH:MM" (docker's CreatedAt, truncated)
    QString hostLabel;  // which docker host this was scanned from (empty = local)
};
QList<DockerImage> scanDockerImages(const QString &buildHost, QString *dockerError = nullptr) {
    QList<DockerImage> out;
    if (dockerError) dockerError->clear();
    // Ask whether docker can be asked at all, first. The listing pipeline below ends in a `while`
    // loop whose exit status is 0 whatever `docker images` said, so a daemon that is down came back
    // as an empty list — and the library then read "(no archives or images on this host)" over a
    // host holding sixty.
    {
        const ShellResult ping = runShellRc(
            buildHost, QStringLiteral("docker version --format '{{.Server.Version}}' 2>&1"), 20000);
        if (ping.rc != 0) {
            if (dockerError) {
                *dockerError = ping.out.trimmed().section(QLatin1Char('\n'), 0, 0);
                if (dockerError->isEmpty())
                    *dockerError = QStringLiteral("docker version exited %1").arg(ping.rc);
            }
            return out;
        }
    }
    // docker images {{.Size}} double-counts overlay2 layers (esp. after a save/load push), so
    // sum the per-layer history deltas instead — the real content size. docker prints SI units
    // (kB/MB/GB); the awk converts + re-humanizes to match docker's own format. CreatedAt gives
    // the build date. The builder image (remora-builder) is toolchain, not a deployable image —
    // exclude it so it can't be picked in the image selector.
    const QString listing = runShell(
        buildHost,
        QStringLiteral(
            "docker images --format '{{.Repository}}:{{.Tag}}|{{.CreatedAt}}' 2>/dev/null "
            "| grep -Ei 'remora|lineage|erstt' | grep -v '<none>' "
            "| grep -v 'remora-builder' | while IFS='|' read -r t c; do "
            "s=$(docker history \"$t\" --format '{{.Size}}' 2>/dev/null | awk "
            "'function b(x, n,u){n=x+0;u=x;gsub(/[0-9.]/,\"\",u);"
            "if(u==\"kB\")return n*1e3;if(u==\"MB\")return n*1e6;"
            "if(u==\"GB\")return n*1e9;if(u==\"TB\")return n*1e12;return n}"
            "{tot+=b($1)}END{if(tot>=1e9)printf \"%.2fGB\",tot/1e9;"
            "else if(tot>=1e6)printf \"%.0fMB\",tot/1e6;else printf \"%.0fkB\",tot/1e3}'); "
            "echo \"$t|$s|$c\"; done"),
        30000);
    for (const QString &line : listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() >= 3) {
            DockerImage im;
            im.tag = f[0].trimmed();
            im.size = f[1].trimmed();
            im.created = f[2].trimmed().left(16);  // "2026-07-12 14:01:49 -0400 EDT" → "…14:01"
            out << im;
        }
    }
    return out;
}

// Parse docker's SI size strings ("2.845GB", "618.6MB", "0B", "1.5kB") to bytes. Multi-char
// units are matched before the bare "B" (every unit ends in B). 0 on an unrecognized string.
qint64 parseDockerSize(const QString &s) {
    const QString t = s.trimmed();
    static const struct { const char *u; double mult; } units[] = {
        {"TB", 1e12}, {"GB", 1e9}, {"MB", 1e6}, {"kB", 1e3}, {"KB", 1e3}, {"B", 1.0}};
    for (const auto &u : units)
        if (t.endsWith(QLatin1String(u.u))) {
            bool ok = false;
            const double n = t.left(t.size() - int(qstrlen(u.u))).toDouble(&ok);
            return ok ? qint64(n * u.mult) : 0;
        }
    return 0;
}

// What a `docker image prune -f` on this host would reclaim: count + summed UNIQUE size of the
// dangling (untagged, container-free) images. UniqueSize — not Size — is the real figure: nominal
// Size double-counts shared base layers (observed 28GB nominal vs 7GB actually reclaimed). The
// JSON is parsed here in C++ so no jq/python is needed on the (possibly remote) host.
struct PrunableStat { int count = 0; qint64 bytes = 0; };
PrunableStat scanPrunable(const QString &host) {
    PrunableStat st;
    const QString json = runShell(
        host, QStringLiteral("docker system df -v --format '{{json .Images}}' 2>/dev/null"), 30000);
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) return st;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        // dangling = no repo tag AND not referenced by any container (prune -f skips in-use images)
        if (o.value(QStringLiteral("Repository")).toString() != QLatin1String("<none>")) continue;
        if (o.value(QStringLiteral("Containers")).toString() != QLatin1String("0")) continue;
        st.count++;
        st.bytes += parseDockerSize(o.value(QStringLiteral("UniqueSize")).toString());
    }
    return st;
}

// Custom, human-friendly image names (display only — never the docker tag). Stored per settings
// file in the "ImageNames" group, keyed by tag, so the same build gets the same label everywhere.
QString imageCustomName(const QString &settingsPath, const QString &tag) {
    QSettings s(settingsPath, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("ImageNames"));
    return s.value(tag).toString();
}
void setImageCustomName(const QString &settingsPath, const QString &tag, const QString &name) {
    QSettings s(settingsPath, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("ImageNames"));
    if (name.trimmed().isEmpty())
        s.remove(tag);
    else
        s.setValue(tag, name.trimmed());
}

QList<ImageArchive> scanImageArchives(const QString &outputDir, const QString &settingsPath,
                                      const QString &buildHost) {
    QList<ImageArchive> out;
    // one round-trip lists every archive with its size + mtime; concatenated (not arg()) so the
    // stat format's % specifiers don't collide with QString::arg placeholders.
    const QString listing = runShell(
        buildHost,
        QStringLiteral("for f in ") + outputDir +
            QStringLiteral("/*.tar; do [ -e \"$f\" ] && stat -c '%n|%s|%Y' \"$f\"; done"),
        30000);
    QSettings cache(settingsPath, QSettings::IniFormat);
    cache.beginGroup(QStringLiteral("ArchiveTagCache"));
    for (const QString &line : listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() != 3) continue;
        ImageArchive a;
        a.path = f[0];
        a.size = f[1].toLongLong();
        a.mtime = f[2].toLongLong();
        const QString key = QString::fromLatin1(
            QCryptographicHash::hash((buildHost + QLatin1Char('|') + a.path + QLatin1Char('|')
                                      + f[1] + QLatin1Char('|') + f[2])
                                         .toUtf8(),
                                     QCryptographicHash::Md5)
                .toHex());
        // Empty cached value = miss, retried (old 2s-timeout code cached failures).
        if (!cache.value(key).toString().isEmpty()) {
            a.tag = cache.value(key).toString();
        } else {
            const QString manifest = runShell(
                buildHost, QStringLiteral("tar -xOf %1 --occurrence=1 manifest.json").arg(a.path),
                120000);
            static const QRegularExpression rx(
                QStringLiteral("\"RepoTags\"\\s*:\\s*\\[\\s*\"([^\"]+)\""));
            const auto m = rx.match(manifest);
            if (m.hasMatch()) a.tag = m.captured(1);
            if (!a.tag.isEmpty()) cache.setValue(key, a.tag);
        }
        out << a;
    }
    return out;
}

// One image a location-backed source offers (bd remora-2ylt). `file` is the archive name when
// the source names one (directories always do; a mirror's index may), empty otherwise.
struct SourceImage {
    QString source;    // ImageSource::name
    QString location;  // the source's location, carried so a row is a self-contained fetch target
    QString tag;       // empty = unreadable manifest / index row without a tag
    QString file;
    qint64 size = 0;   // 0 = unknown (a mirror index without the bytes column)
    qint64 mtime = 0;  // 0 = unknown (mirrors don't say; directories do)
};

// List what a source's location offers. http(s):// → GET <location>/index.txt, the contract bd
// remora-2ylt froze so the mirror can be built against it: one image per line,
// '<docker-tag> [<archive-file> [<bytes>]]', '#' starts a comment. Anything else is a directory
// of docker-save archives — remote when it carries a user@host: prefix — walked by the same
// scan the library already runs on the build host's output dir, so tags resolve through the
// same ArchiveTagCache instead of streaming multi-GB tars twice.
QList<SourceImage> scanSourceLocation(const ImageSource &s, const QString &settingsPath) {
    QList<SourceImage> out;
    const QString loc = s.location;
    if (loc.isEmpty()) return out;
    if (loc.startsWith(QLatin1String("http://")) || loc.startsWith(QLatin1String("https://"))) {
        // -f: a 404 must read as "no index", never as an index of one HTML error page.
        const QString idx = runShell(
            QString(),
            QStringLiteral("curl -fsSL -m 15 %1").arg(shellQuote(loc + QStringLiteral("/index.txt"))),
            20000);
        for (const QString &raw : idx.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QString line = raw.section(QLatin1Char('#'), 0, 0).trimmed();
            if (line.isEmpty()) continue;
            static const QRegularExpression ws(QStringLiteral("\\s+"));
            const QStringList f = line.split(ws, Qt::SkipEmptyParts);
            SourceImage im;
            im.source = s.name;
            im.location = s.location;
            im.tag = f[0];
            if (f.size() > 1) im.file = f[1];
            if (f.size() > 2) im.size = f[2].toLongLong();
            out << im;
        }
        return out;
    }
    QString host, dir = loc;  // user@host:/dir, or a plain local directory
    const int colon = loc.indexOf(QLatin1Char(':'));
    if (colon > 0 && !loc.startsWith(QLatin1Char('/'))) {
        host = loc.left(colon);
        dir = loc.mid(colon + 1);
    }
    for (const ImageArchive &a : scanImageArchives(dir, settingsPath, host)) {
        SourceImage im;
        im.source = s.name;
        im.location = s.location;
        im.tag = a.tag;
        im.file = QFileInfo(a.path).fileName();
        im.size = a.size;
        im.mtime = a.mtime;
        out << im;
    }
    return out;
}

// Android major version encoded in a tag: exact gating-table match first, then the
// "repo:13.0.0_..." convention. -1 = unknown (such entries are never version-filtered).
int tagAndroidVersion(const QString &tag) {
    for (const ImageTag &t : imageTags())
        if (t.tag == tag) return t.android;
    static const QRegularExpression rx(QStringLiteral(":(\\d+)\\."));
    const auto m = rx.match(tag);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

// Everything a feature row used to say in colour, folded into the one tooltip the checkbox already
// had. `gateReason` is the gating engine's live objection to the current selection, empty when it
// has none. badgeStyle() — the tinted OK/CAVEAT/PARKED pill this replaced — is gone with the
// badges; the maturity word survives here, because a PARKED feature that reads exactly like an OK
// one is the one thing the row genuinely could not afford to lose.
QString featureTooltip(const Feature &f, const QString &gateReason) {
    QString t = QStringLiteral("%1 — %2").arg(featureStatusToString(f.status).toUpper(), f.note);
    if (!gateReason.isEmpty())
        t += QStringLiteral("\n\nUnavailable for this profile: %1").arg(gateReason);
    return t;
}

QString optInt(const std::optional<int> &v) { return v ? QString::number(*v) : QString(); }
QString optStr(const std::optional<QString> &v) { return v.value_or(QString()); }

// Read an int field: empty → nullopt (keep the resolver default), else the parsed value.
std::optional<int> readInt(QLineEdit *e) {
    const QString t = e->text().trimmed();
    bool ok = false;
    const int v = t.toInt(&ok);
    return (ok && !t.isEmpty()) ? std::optional<int>(v) : std::nullopt;
}
std::optional<QString> readStr(QLineEdit *e) {
    const QString t = e->text().trimmed();
    return t.isEmpty() ? std::nullopt : std::optional<QString>(t);
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    path_ = defaultRemorarcPath();
    instance_ = activeInstance(path_);
    cfg_ = loadInstance(path_, instance_);
    features_ = QSet<QString>(cfg_.image.features.begin(), cfg_.image.features.end());
    if (features_.isEmpty()) features_ = defaultBuildSet(cfg_.image.androidVersion.value_or(16));
    sources_ = loadImageSources(path_);
    buildUi();
    loadConfig();
    wireAutosave();  // after loadConfig, so the initial widget population doesn't trigger saves
    // Watch remorarc so an edit made outside this window — by hand, by the CLI (which reads the
    // file fresh on every invocation), or by another GUI — is picked up instead of being clobbered
    // by the next autosave writing this window's stale copy back over it (bd remora-4ei.18).
    rcWatcher_ = new QFileSystemWatcher(this);
    // The directory as well as the file: an atomic replace unlinks the path, and a watch on a path
    // that stops existing is gone for good, so the file watch alone would survive exactly one save.
    const QString rcDir = QFileInfo(path_).absolutePath();
    if (QFileInfo::exists(rcDir)) rcWatcher_->addPath(rcDir);
    noteRemorarcWritten();  // seed the fingerprint and arm the file watch
    connect(rcWatcher_, &QFileSystemWatcher::fileChanged, this, &MainWindow::onRemorarcChanged);
    connect(rcWatcher_, &QFileSystemWatcher::directoryChanged, this,
            &MainWindow::onRemorarcChanged);
    onRefreshArchives();  // needs the loaded output_dir — buildUi runs before the fields are set
    recompute();
    // Deploy-target readiness, probed once at startup into the log pane. This replaces the
    // Device page's Check-readiness button: the answer only changes with config/host changes,
    // so it belongs in the startup log, not behind a button on a form. Skipped for REMORA_SHOT
    // renders (deterministic, must not depend on docker state) and unparented so a fast window
    // close can't destroy the probe mid-run.
    if (!qEnvironmentVariableIsSet("REMORA_SHOT")) {
        const Backend b = currentBackend();
        // gpu-host comes from the RESOLVED profile, not from the backend — the same correction
        // `remora check` already carries. It used to be `b == Vm`, so the GPU/Venus readiness rows
        // could never appear in the GUI on bare or remote however the profile was configured.
        auto *w = new ReadinessWorker(b, cfg_.backend.sshHost.value_or(QString()).trimmed(),
                                      resolve(cfg_, b).gpuMode == GpuMode::Host, nullptr);
        connect(w, &ReadinessWorker::sigReady, this, [this](bool ready, QStringList lines) {
            appendLog(QStringLiteral("── deploy-target readiness ──"));
            for (const QString &l : lines) appendLog(l);
            if (!ready)
                statusBar()->showMessage(
                    QStringLiteral("deploy target not ready — details in the log"), 8000);
        });
        connect(w, &ReadinessWorker::finished, w, &QObject::deleteLater);
        w->start();
    }
    setWindowTitle(QStringLiteral("Remora"));
    // window icon: set app-wide in App.cpp from the raw themed SVG — deliberately NOT via
    // themeIcon(), whose accent tinting would repaint the brand fish in the wallpaper color
    // Sized to the widest page, MEASURED (minimumSizeHint of each page's content):
    // Device 887 px and Viewer 876 px — two columns of cards, each with a label gutter — against
    // the 844 px a 1060-wide window left after the sidebar. Both pages scrolled sideways at that
    // width, and at the 1100 the docs shot used, the Device page still did by 3 px. There is no
    // saved geometry, so this IS the everyday window; REMORA_SHOT renders at the same size, so
    // the docs show what a launch shows. The height stays where every page fit on screen.
    resize(1140, 740);
}

// A one-line muted hint; a longer explainer (when given) lives in a tooltip behind an ⓘ mark,
// so cards stay short enough that every page fits on screen without scrolling.
static QLabel *hintLabel(const QString &text, const QString &details = {}) {
    auto *s = new QLabel(details.isEmpty() ? text : text + QStringLiteral("  ⓘ"));
    s->setStyleSheet("color:#777;");
    s->setWordWrap(true);
    if (!details.isEmpty()) s->setToolTip(details);
    return s;
}

// Home-relative display for path fields: the UI shows ~ where the config stores $HOME (the
// stored value stays absolute — docker -v mounts don't expand tildes).
static QString tildify(QString p) {
    const QString home = QDir::homePath();
    if (p.startsWith(home)) p.replace(0, home.size(), QStringLiteral("~"));
    return p;
}

// A titled card holding a QFormLayout; returns the form so the caller adds rows.
// A titleless card (folded=true) trims the QSS title padding — used under disclosures,
// where the disclosure button already carries the title.
static QFormLayout *card(QVBoxLayout *col, const QString &title, const QString &subtitle = {},
                         const QString &details = {}) {
    auto *box = new QGroupBox(title);
    if (title.isEmpty()) box->setProperty("folded", true);
    auto *v = new QVBoxLayout(box);
    if (!subtitle.isEmpty()) v->addWidget(hintLabel(subtitle, details));
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    v->addLayout(form);
    col->addWidget(box);
    return form;
}

// A collapsed-by-default disclosure: an arrow button that reveals `content` on demand.
// Returns the button so a caller whose summary changes at runtime can retitle it — the collapsed
// state is the only thing most users read, so a stale count there is worse than no count.
static QToolButton *disclosure(QVBoxLayout *col, const QString &title, QWidget *content) {
    auto *btn = new QToolButton();
    btn->setText(title);
    btn->setCheckable(true);
    btn->setArrowType(Qt::RightArrow);
    btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn->setAutoRaise(true);
    content->setVisible(false);
    QObject::connect(btn, &QToolButton::toggled, content, [btn, content](bool on) {
        btn->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        content->setVisible(on);
    });
    col->addWidget(btn);
    col->addWidget(content);
    return btn;
}

// (foldedCard — a card behind a collapsed disclosure — went with its last user, the Play
// certification card, which is a Profiles-table column now; disclosure() itself stays in use.)

// A sidebar page: scrollable column of cards with a bold context title, Kartend-style.
// Content is capped at a readable width and centered — on a wide/tiled monitor, full-bleed
// form fields stretch into unreadable 2000px lines.
QVBoxLayout *MainWindow::newPage(const QString &title,
                                 std::initializer_list<const char *> icons) {
    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *root = new QWidget();
    root->setMaximumWidth(1120);  // wide enough for the 2-col pages (Viewer/Source) to breathe
    auto *col = new QVBoxLayout(root);
    col->setSpacing(12);
    col->setContentsMargins(16, 14, 16, 12);
    // no page heading: the sidebar's selected row already names the page
    auto *wrap = new QWidget();
    auto *wl = new QHBoxLayout(wrap);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->addStretch(1);
    wl->addWidget(root, 100);  // root wins the space until its width cap, then the sides share
    wl->addStretch(1);
    scroll->setWidget(wrap);
    pages_->addWidget(scroll);
    auto *item = new QListWidgetItem(themeIcon(icons), title, sidebar_);
    item->setToolTip(title);
    item->setData(Qt::UserRole, pages_->count() - 1);  // header rows carry no page index
    sidebarLabels_ << title;
    return col;
}

// Sentinels for the GPU combo's two non-driver entries. Neither is a DRM driver name, and
// gpu_driver= must stay empty for both: Venus selects the NVIDIA render-server stack, and
// SwiftShader selects no GPU at all (gpu_mode's Guest arm).
static const QString kGpuVenus = QStringLiteral("__venus__");
static const QString kGpuSoftware = QStringLiteral("__swiftshader__");

// video_bit_rate is a mirror unit string ("30M", "8000K"); the Viewer's field is a number with the
// M standing beside it. These two are the whole translation, and they are deliberately narrow —
// only the Mbps spelling is unwrapped and re-wrapped, so every other unit round-trips verbatim.
static const QRegularExpression kBitRateMbps(QStringLiteral("^(\\d+)M$"));
static const QRegularExpression kBitRateDigits(QStringLiteral("^\\d+$"));

// ── the Viewer's Navigation card ──────────────────────────────────────────────────────────────
// One row per Android navigation action, each naming what triggers it. The card is the ACTION's
// point of view; the mirror's two knobs underneath are both the other way round, so the card reads
// and writes them rather than exposing them:
//   --mouse-bind=WXYZ:wxyz  a vector over BUTTONS (right / middle / 4th / 5th, then the same four
//                           with Shift held), whose value is the action character below
//   --shortcut-mod=…        the modifier that arms the mirror's fixed MOD+letter chords
// Positional strings punish a typo silently — a wrong character rebinds or kills a button and
// nothing says so until you click it — which is why neither is ever typed here.
struct NavAction {
    const char *label;
    char bindChar;  // --mouse-bind action character
    char key;       // the mirror's fixed shortcut letter for the same action (MOD+<key>)
};
static const QList<NavAction> kNavActions = {
    {"Back", 'b', 'B'},
    {"Home", 'h', 'H'},
    {"Recents", 's', 'S'},
    {"Notifications", 'n', 'N'},
};
// Index = position in the --mouse-bind vector. 4th/5th are the thumb pair; Linux calls them
// BTN_SIDE/BTN_EXTRA and mice label them back/forward, so name them the way the hand knows them.
// QString, not const char *: these carry an em dash, and QLatin1String would read its UTF-8 bytes
// one per character.
static const QList<QString> kNavButtons = {
    QStringLiteral("Right click"),
    QStringLiteral("Middle click"),
    QStringLiteral("Side button — back"),
    QStringLiteral("Side button — forward"),
};
// A button carrying no action forwards its click to Android ('+'). Any other non-action character
// a hand-written mouse_bind used — '-' (ignore the click) is the only other one the mirror defines —
// is left alone rather than normalised away, so this card cannot quietly disarm a button it has
// no row for.
static const QChar kNavUnmapped = QLatin1Char('+');
// What an unset mouse_bind actually runs with (Resolver.cpp). The card shows the LIVE bindings for
// a profile that never mentioned them, rather than four empty rows for a mirror where right-click
// already goes Back.
static const QString kResolvedMouseBind = QStringLiteral("bhsn:++++");

// The direct-key half (--key-bind, bd remora-pdu): a single arbitrary key per action, no
// modifier, consumed by the mirror. The card captures Qt keys but the mirror wants SDL key names;
// QKeySequence's portable names already match SDL's for almost everything (SDL compares
// case-insensitively), so only the divergent spellings are tabled. A key this pair of functions
// cannot round-trip is refused at capture and preserved verbatim from a hand-written key_bind.
// Both directions come from core/MirrorInput's table, which is also what the CLIENT parses — so a
// key this card can capture is always a key the mirror honours. They used to be a private pair
// here backed by QKeySequence, which meant the card could write a binding the client silently
// ignored (bd remora-28ix.2.6).
static QString sdlKeyName(int qtKey) { return remora::mirror::sdlKeyName(qtKey); }
static int qtKeyFromSdlName(const QString &sdlName) {
    return remora::mirror::qtKeyFromSdlName(sdlName).value_or(0);
}

// Modifier choices for the keyboard half. Values are --shortcut-mod vectors; empty keeps
// the mirror's own default (and emits no argument at all). It accepts only lctrl/rctrl/lalt/ralt/
// lsuper/rsuper here — Shift is not a shortcut modifier, it is what selects a --mouse-bind's
// second sequence — so it is not offered.
struct ShortcutMod {
    const char *label, *value, *chordPrefix;
};
static const QList<ShortcutMod> kShortcutMods = {
    {"Alt or Super    (default)", "", "Alt"},
    {"Alt", "lalt", "Alt"},
    {"Super", "lsuper", "Super"},
    {"Ctrl", "lctrl", "Ctrl"},
    {"Ctrl+Alt", "lctrl+lalt", "Ctrl+Alt"},
    {"Ctrl+Super", "lctrl+lsuper", "Ctrl+Super"},
};

// Size a value field to the value it holds. QFormLayout's AllNonFixedFieldsGrow stretches every
// field to the card width, so a 4-digit resolution box arrives 700px wide and reads as "type
// anything here" — the field's width is the clearest hint about what belongs in it. Measured off
// the widget's own font so it survives a theme or DPI change, unlike an eyeballed pixel count.
static void setFieldChars(QWidget *w, int chars) {
    // '0' is the widest digit in most UI fonts; a mixed-text field gets 'n' as an average glyph.
    const QChar sample = chars <= 6 ? QLatin1Char('0') : QLatin1Char('n');
    const int text = w->fontMetrics().horizontalAdvance(QString(chars, sample));
    // frame + padding, plus the drop-down arrow when there is one. The combo allowance is the
    // arrow and its own frame and nothing more: at 44 a 4-character combo came out half again as
    // wide as the 4-character boxes beside it, so "same number of characters" stopped looking like
    // the same size — which is the whole point of sizing a field to its value.
    const int chrome = qobject_cast<QComboBox *>(w) ? 30 : 18;
    w->setFixedWidth(text + chrome);
}

// One label gutter for a whole page: every form's label column gets the width of the widest
// label across ALL of them, so value fields line up between cards instead of each QFormLayout
// computing its own indent.
static void alignFormLabels(std::initializer_list<QFormLayout *> forms) {
    int w = 0;
    for (QFormLayout *f : forms) {
        if (!f) continue;
        for (int i = 0; i < f->rowCount(); ++i)
            if (auto *it = f->itemAt(i, QFormLayout::LabelRole); it && it->widget())
                w = qMax(w, it->widget()->sizeHint().width());
    }
    for (QFormLayout *f : forms) {
        if (!f) continue;
        for (int i = 0; i < f->rowCount(); ++i)
            if (auto *it = f->itemAt(i, QFormLayout::LabelRole); it && it->widget())
                it->widget()->setMinimumWidth(w);
    }
}

void MainWindow::setLogVisible(bool visible) {
    if (logAction_) logAction_->setChecked(visible);
}

// REMORA_SHOT_PAGE support: jump to a page by its sidebar title (e.g. for per-page screenshots).
bool MainWindow::selectPage(const QString &title) {
    const int row = sidebarLabels_.indexOf(title);
    if (row < 0 || !sidebar_->item(row)->data(Qt::UserRole).isValid()) return false;
    sidebar_->setCurrentRow(row);
    return true;
}

// REMORA_SHOT companion: say so when the shown page is wider than its viewport. A page that scrolls
// sideways is a layout defect, and the docs screenshots are the one place every page is rendered
// at the default width on a schedule — so the render is where the regression gets caught.
void MainWindow::warnIfPageOverflows() const {
    auto *scroll = qobject_cast<QScrollArea *>(pages_->currentWidget());
    if (!scroll || !scroll->widget()) return;
    const int need = scroll->widget()->minimumSizeHint().width();
    const int have = scroll->viewport()->width();
    if (need > have)
        fprintf(stderr, "REMORA_SHOT: page '%s' needs %d px but the viewport is %d — it scrolls sideways\n",
                qPrintable(sidebarLabels_.value(sidebar_->currentRow())), need, have);
}

void MainWindow::setSidebarCollapsed(bool collapsed) {
    // Row paddings are IDENTICAL in both states, so no icon moves in X or Y across the toggle —
    // the pane just slides. Texts strip before the collapse animation and return only after the
    // expand finishes, so labels never elide mid-slide.
    const auto applyTexts = [this, collapsed] {
        if (profileNameLabel_) profileNameLabel_->setVisible(!collapsed);
        if (profileSwitchBtn_) profileSwitchBtn_->setVisible(!collapsed);
        if (profileAddBtn_) profileAddBtn_->setVisible(!collapsed);
        for (int i = 0; i < sidebar_->count(); ++i)
            sidebar_->item(i)->setText(collapsed ? QString() : sidebarLabels_.value(i));
    };
    const int target = collapsed ? 56 : 216;
    if (sidebarAnim_) {
        sidebarAnim_->stop();  // DeleteWhenStopped reaps it
        sidebarAnim_ = nullptr;
    }
    if (!sidebarPane_ || !sidebarPane_->isVisible()) {  // initial buildUi call — nothing to slide
        applyTexts();
        if (sidebarPane_) sidebarPane_->setFixedWidth(target);
        return;
    }
    if (collapsed) applyTexts();
    auto *anim = new QVariantAnimation(this);
    sidebarAnim_ = anim;
    anim->setStartValue(sidebarPane_->width());
    anim->setEndValue(target);
    anim->setDuration(170);
    anim->setEasingCurve(QEasingCurve::InOutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) { sidebarPane_->setFixedWidth(v.toInt()); });
    connect(anim, &QVariantAnimation::finished, this, [this, applyTexts, collapsed, anim] {
        if (!collapsed) applyTexts();
        if (sidebarAnim_ == anim) sidebarAnim_ = nullptr;
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::buildUi() {
    auto *central = new QWidget(this);
    auto *split = new QHBoxLayout(central);
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);

    // ── Sidebar pane: profile switcher + nav list + collapse toggle ──
    // Deliberately no app name/icon header: the titlebar already carries both.
    sidebarPane_ = new QWidget();
    sidebarPane_->setObjectName(QStringLiteral("sidebarPane"));
    auto *sideCol = new QVBoxLayout(sidebarPane_);
    sideCol->setContentsMargins(0, 10, 0, 4);
    sideCol->setSpacing(4);
    {
        // Profile entry — reads like a nav row: icon + the ACTIVE profile's name, then a switch
        // (▾) and an add (+) button. The icon and ▾ both open the profile menu (pick or create);
        // collapsed, the labelled parts hide and the icon alone keeps profiles manageable. One
        // profile per [Instance-<name>] in remorarc; switching saves the current profile, loads
        // the selected one, and persists it as active.
        profileRow_ = new QWidget();
        auto *pl = new QHBoxLayout(profileRow_);
        pl->setContentsMargins(8, 2, 8, 2);
        pl->setSpacing(4);
        auto *profileIconBtn = new QToolButton();
        // an OS-ish glyph, deliberately not a person and not Android-specific: a profile is a
        // machine configuration, not an identity
        profileIconBtn->setIcon(themeIcon({"computer", "computer-symbolic", "cpu"}));
        profileIconBtn->setIconSize(QSize(22, 22));
        profileIconBtn->setAutoRaise(true);
        profileIconBtn->setToolTip(QStringLiteral("Profile — switch or create"));
        connect(profileIconBtn, &QToolButton::clicked, this, &MainWindow::showProfileMenu);
        profileNameLabel_ = new QLabel(instance_);
        {
            QFont pf = profileNameLabel_->font();
            pf.setBold(true);
            profileNameLabel_->setFont(pf);
        }
        profileSwitchBtn_ = new QToolButton();
        profileSwitchBtn_->setIcon(themeIcon({"system-switch-user", "go-down", "arrow-down"}));
        profileSwitchBtn_->setAutoRaise(true);
        profileSwitchBtn_->setToolTip(QStringLiteral("Switch profile"));
        connect(profileSwitchBtn_, &QToolButton::clicked, this, &MainWindow::showProfileMenu);
        profileAddBtn_ = new QToolButton();
        profileAddBtn_->setIcon(themeIcon({"list-add", "document-new"}));
        profileAddBtn_->setAutoRaise(true);
        profileAddBtn_->setToolTip(
            QStringLiteral("New profile — pick OS + Android version; starts from clean defaults"));
        connect(profileAddBtn_, &QToolButton::clicked, this, &MainWindow::onNewProfile);
        pl->addWidget(profileIconBtn);
        pl->addWidget(profileNameLabel_, 1);
        pl->addWidget(profileSwitchBtn_);
        pl->addWidget(profileAddBtn_);
        // Zero-stretch trailing spacer: invisible while the name label (stretch 1) absorbs the
        // slack, but once the labelled parts hide on collapse it is the only expanding item —
        // so the icon stays pinned left instead of drifting to the centre.
        pl->addStretch(0);
        sideCol->addWidget(profileRow_);
        // an accent rule under the profile entry — 80% of the pane, centered (the 1:8:1
        // stretches keep the proportion at both the expanded and collapsed widths)
        auto *divider = new QFrame();
        divider->setObjectName(QStringLiteral("profileDivider"));
        divider->setFixedHeight(1);
        auto *divRow = new QWidget();
        auto *dl = new QHBoxLayout(divRow);
        dl->setContentsMargins(0, 1, 0, 3);
        dl->addStretch(1);
        dl->addWidget(divider, 8);
        dl->addStretch(1);
        sideCol->addWidget(divRow);
    }
    sidebar_ = new QListWidget();
    sidebar_->setObjectName(QStringLiteral("sidebarList"));
    sidebar_->setIconSize(QSize(22, 22));
    sidebar_->setSpacing(2);
    sidebar_->setFrameShape(QFrame::NoFrame);
    // never a horizontal scrollbar: collapsed, a fraction-too-wide row must clip, not scroll
    sidebar_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideCol->addWidget(sidebar_, 1);
    {
        auto *collapseBtn = new QToolButton();
        collapseBtn->setIcon(themeIcon({"sidebar-collapse-left", "view-left-close",
                                        "format-justify-left"}));
        collapseBtn->setCheckable(true);
        collapseBtn->setAutoRaise(true);
        collapseBtn->setToolTip(QStringLiteral("Collapse/expand the sidebar"));
        connect(collapseBtn, &QToolButton::toggled, this, &MainWindow::setSidebarCollapsed);
        auto *foot = new QWidget();
        auto *fl = new QHBoxLayout(foot);
        fl->setContentsMargins(8, 0, 8, 0);
        fl->addWidget(collapseBtn);
        fl->addStretch(1);
        sideCol->addWidget(foot);
    }
    pages_ = new QStackedWidget();
    // Opaque: Breeze's tools-area separator is painted onto the QMainWindow at the very top of
    // the content, and a transparent page stack lets that 1px header-colored line show through
    // (the opaque sidebar pane covers its share, so the line looked misaligned).
    pages_->setAutoFillBackground(true);
    split->addWidget(sidebarPane_);
    split->addWidget(pages_, 1);
    // rows map to pages via UserRole — group-header rows carry no page and are unselectable
    connect(sidebar_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        const QVariant page = sidebar_->item(row)->data(Qt::UserRole);
        if (!page.isValid()) return;
        pages_->setCurrentIndex(page.toInt());
        // the Apps page refreshes itself on every visit (there is no Refresh button)
        if (page.toInt() == appsPageIndex_) refreshAppsPage();
        // the device may have been started or stopped since this page was last looked at
        if (page.toInt() == devicePageIndex_) refreshDataDirLock();
        // so does the Runners page — and while it stays open, a timer re-probes the statuses
        if (runnersPoll_) {
            if (page.toInt() == runnersPageIndex_
                && !qEnvironmentVariableIsSet("REMORA_SHOT")) {
                rebuildRunTable();
                runnersPoll_->start();
            } else {
                runnersPoll_->stop();
            }
        }
    });

    // ── Log pane (status-bar toggled, resizable, never steals the current page) ──
    // A titled "Activity" strip over a steps checklist (left) + terminal-style output (right),
    // so the pane reads as a distinct region instead of bleeding into the page above it.
    checklist_ = new QTreeWidget();
    checklist_->setColumnCount(2);
    checklist_->setHeaderLabels({QStringLiteral("step"), QStringLiteral("detail")});
    checklist_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    checklist_->header()->setStretchLastSection(true);
    checklist_->setRootIsDecorated(false);
    logView_ = new QPlainTextEdit();
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(5000);  // bounded scrollback
    {
        QFont mono = logView_->font();
        mono.setFamilies({QStringLiteral("monospace")});
        mono.setStyleHint(QFont::Monospace);
        logView_->setFont(mono);
    }
    // Ctrl+Q closes the workspace. Routed through close() rather than qApp->quit() so it goes
    // through closeEvent — a source pull or build still running gets its "stop it?" question and
    // its process-group cleanup, which quit() would skip and leave orphaned worktrees behind.
    // Each sequence registered ONCE. QKeySequence::Quit is resolved by the platform theme, and
    // under Breeze/KDE it resolves to Ctrl+Q — so listing it *and* Ctrl+Q outright bound the same
    // key twice on the same action, every press logged "QAction::event: Ambiguous shortcut
    // overload: Ctrl+Q", and Qt fires nothing on an ambiguous match. The binding therefore did
    // nothing on the desktop it was meant for while testing fine offscreen, where the stub theme
    // maps Quit to the Exit media key and the two sequences do not collide (bd remora-4ei.29).
    {
        auto *quitAction = new QAction(QStringLiteral("Quit"), this);
        QList<QKeySequence> quitKeys{QKeySequence(QStringLiteral("Ctrl+Q"))};
        // keep the platform's own quit sequence too, but only where it is a genuinely different key
        const QKeySequence platformQuit(QKeySequence::Quit);
        if (!platformQuit.isEmpty() && !quitKeys.contains(platformQuit)) quitKeys << platformQuit;
        quitAction->setShortcuts(quitKeys);
        quitAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(quitAction, &QAction::triggered, this, &MainWindow::close);
        addAction(quitAction);
    }
    logAction_ = new QAction(
        themeIcon({"utilities-terminal-symbolic", "dialog-scripts", "viewlog", "text-x-log"}),
        QStringLiteral("Log"), this);
    logAction_->setCheckable(true);
    logAction_->setToolTip(QStringLiteral(
        "Show/hide the log pane — step states + command output, without leaving this page"));
    auto *logSplit = new QSplitter(Qt::Horizontal);
    logSplit->addWidget(checklist_);
    logSplit->addWidget(logView_);
    logSplit->setStretchFactor(0, 1);
    logSplit->setStretchFactor(1, 2);
    {
        auto *pane = new QWidget();
        pane->setObjectName(QStringLiteral("logPane"));
        auto *pv = new QVBoxLayout(pane);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(0);
        auto *head = new QWidget();
        head->setObjectName(QStringLiteral("logHead"));
        auto *hl = new QHBoxLayout(head);
        hl->setContentsMargins(10, 4, 6, 4);
        hl->setSpacing(6);
        auto *icon = new QLabel();
        const QIcon term = themeIcon({"utilities-terminal-symbolic", "dialog-scripts"});
        if (!term.isNull()) icon->setPixmap(term.pixmap(QSize(14, 14)));
        auto *title = new QLabel(QStringLiteral("Activity"));
        QFont tf = title->font();
        tf.setBold(true);
        title->setFont(tf);
        auto *clearBtn = new QToolButton();
        clearBtn->setIcon(themeIcon({"edit-clear-history", "edit-clear-all", "edit-clear"}));
        clearBtn->setAutoRaise(true);
        clearBtn->setToolTip(QStringLiteral("Clear the log output (step states stay)"));
        connect(clearBtn, &QToolButton::clicked, this, [this] {
            logView_->clear();
            statusBar()->clearMessage();  // a lingering failure line is part of what's cleared
        });
        auto *closeBtn = new QToolButton();
        closeBtn->setIcon(themeIcon({"window-close-symbolic", "window-close", "dialog-close"}));
        closeBtn->setAutoRaise(true);
        closeBtn->setToolTip(QStringLiteral("Hide the log pane"));
        connect(closeBtn, &QToolButton::clicked, this,
                [this] { logAction_->setChecked(false); });
        hl->addWidget(icon);
        hl->addWidget(title);
        hl->addStretch(1);
        hl->addWidget(clearBtn);
        hl->addWidget(closeBtn);
        pv->addWidget(head);
        pv->addWidget(logSplit, 1);
        logPane_ = pane;
    }
    logPane_->setMinimumHeight(140);
    connect(logAction_, &QAction::toggled, logPane_, &QWidget::setVisible);
    auto *vsplit = new QSplitter(Qt::Vertical);
    vsplit->addWidget(central);
    vsplit->addWidget(logPane_);
    vsplit->setStretchFactor(0, 3);
    vsplit->setStretchFactor(1, 1);
    vsplit->setCollapsible(0, false);
    logPane_->hide();
    setCentralWidget(vsplit);

    QVBoxLayout *col = newPage(QStringLiteral("Runners"), {"media-playback-start", "system-run"});
    runnersPageIndex_ = pages_->count() - 1;  // for the visit-triggered refresh + status polling

    // ── Runners: orchestrate every profile's deployment ──────────────
    // No card title or explainer: the table's own headers say it, and the per-row tooltips
    // carry the verbs.
    {
        auto *box = new QGroupBox();
        auto *v = new QVBoxLayout(box);
        runTree_ = new QTreeWidget();
        runTree_->setColumnCount(5);
        runTree_->setHeaderLabels({QStringLiteral("profile"), QStringLiteral("image"),
                                   QStringLiteral("docker host"), QStringLiteral("status"),
                                   QStringLiteral("actions")});
        runTree_->setRootIsDecorated(false);
        runTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        // the image column yields (elides) rather than forcing a horizontal scrollbar —
        // row tooltips carry the full tag when it matters (pinned/diverged)
        runTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        runTree_->header()->setStretchLastSection(false);
        runTree_->setMinimumHeight(200);
        v->addWidget(runTree_);
        auto *rowBtns = new QWidget();
        auto *rbl = new QHBoxLayout(rowBtns);
        rbl->setContentsMargins(0, 0, 0, 0);
        auto *addDepBtn = new QPushButton(themeIcon({"list-add"}), QStringLiteral("Add"));
        auto *rmDepBtn = new QPushButton(themeIcon({"list-remove", "edit-delete"}),
                                         QStringLiteral("Remove"));
        auto *editDepBtn = new QPushButton(themeIcon({"document-edit", "edit-rename"}),
                                           QStringLiteral("Edit"));
        editDepBtn->setToolTip(
            QStringLiteral("Change the selected deployment's profile, image, name or host "
                           "(double-clicking a row works too)"));
        connect(addDepBtn, &QPushButton::clicked, this, &MainWindow::onAddDeployment);
        connect(editDepBtn, &QPushButton::clicked, this, &MainWindow::onEditDeployment);
        connect(rmDepBtn, &QPushButton::clicked, this, &MainWindow::onRemoveDeployment);
        connect(runTree_, &QTreeWidget::itemDoubleClicked, this,
                &MainWindow::onEditDeployment);
        rbl->addWidget(addDepBtn);
        rbl->addWidget(editDepBtn);
        rbl->addWidget(rmDepBtn);
        rbl->addStretch(1);
        v->addWidget(rowBtns);
        col->addWidget(box, 1);  // the deployments table gets the whole page
        rebuildRunTable();
        // No Refresh button: entering the page re-reads everything, and while it stays open a
        // timer re-probes the statuses in place (rows aren't rebuilt — that would eat a host
        // edit mid-typing).
        runnersPoll_ = new QTimer(this);
        runnersPoll_->setInterval(8000);
        connect(runnersPoll_, &QTimer::timeout, this, [this] {
            if (!running_) probeRunStatuses();
        });
    }

    // ── Apps: what runs inside the instance (menu integration + ARM sideloads) ────
    // view-list-icons: Breeze's 2×2 tile grid — reads as an app drawer ("applications-all" is
    // just a blank rounded square at sidebar size).
    col = newPage(QStringLiteral("Apps"),
                  {"view-list-icons", "view-grid", "applications-all"});
    appsPageIndex_ = pages_->count() - 1;  // for the visit-triggered auto-refresh

    // ── The active profile's launcher-menu entries — no card title or hint banners: the
    // per-entry icons and the bar's tooltips carry the explanation.
    {
        auto *box = new QGroupBox();
        auto *v = new QVBoxLayout(box);
        integrationsTree_ = new QTreeWidget();
        integrationsTree_->setColumnCount(5);
        integrationsTree_->setHeaderLabels({QStringLiteral("entry"), QStringLiteral("package"),
                                            QStringLiteral("mode"), QStringLiteral("account"),
                                            QStringLiteral("hide root")});
        integrationsTree_->setRootIsDecorated(false);  // a flat list, like the Runners table
        integrationsTree_->setUniformRowHeights(true);  // mode toggles must not vary row height
        // entry names are short, package ids are long — size the columns accordingly
        integrationsTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        integrationsTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        integrationsTree_->header()->setStretchLastSection(false);
        integrationsTree_->setMinimumHeight(160);
        rebuildIntegrationsList();
        v->addWidget(integrationsTree_);
        // Flat, icon-led action bar: bulk window-mode ops on the left, app transfer on the right.
        auto *btns = new QWidget();
        auto *bl = new QHBoxLayout(btns);
        bl->setContentsMargins(0, 2, 0, 0);
        bl->setSpacing(4);
        const auto barBtn = [](const QIcon &icon, const QString &text, const QString &tip) {
            auto *b = new QToolButton();
            b->setIcon(icon);
            b->setText(text);
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setAutoRaise(true);
            b->setToolTip(tip);
            return b;
        };
        auto *allBtn = barBtn(themeIcon({"video-display", "computer"}),
                              QStringLiteral("All desktop"),
                              QStringLiteral("Set every entry to a desktop-resolution window."));
        auto *noneBtn = barBtn(
            themeIcon({"smartphone", "phone", "window"}),  // the same phone the mode column shows
            QStringLiteral("All freeform"),
            QStringLiteral("Set every entry to a freeform popup (phone-sized)."));
        auto *updateBtn = barBtn(
            themeIcon({"cloud-download", "download", "system-software-update"}),
            QStringLiteral("Update ARM apps"),
            QStringLiteral(
                "Re-fetch the ARM-injected apps in place from Google Play, using the device's "
                "own login. Only those: this is their only update path, because Remora's "
                "ownership claim blocks Play's auto-update. Every other app is a normal Play "
                "install that Play updates on the device by itself. App data and permissions "
                "are kept. Auto-updating after Connect can be enabled on the Settings page."));
        connect(allBtn, &QToolButton::clicked, this, [this] { setAllAppModes(true); });
        connect(noneBtn, &QToolButton::clicked, this, [this] { setAllAppModes(false); });
        connect(updateBtn, &QToolButton::clicked, this, [this] { runAppUpdate(instance_, true); });
        auto *transferBtn = new QPushButton(themeIcon({"document-send", "send-to", "system-run"}),
                                            QStringLiteral("Import / export…"));
        transferBtn->setToolTip(QStringLiteral(
            "Install an app's ARM64 build from Google Play, or move the apps users installed — "
            "including split APKs, which must travel together — between profiles' devices, or to "
            "and from a local folder (one subdirectory per app; loose APKs import too). "
            "APP DATA DOES NOT MOVE on transfers — apps arrive as fresh installs, logged out. "
            "Both endpoints are checked before anything is touched. CLI equivalents: remora "
            "transfer-apps / export-apks / import-apks."));
        connect(transferBtn, &QPushButton::clicked, this, &MainWindow::onTransferApps);
        runButtons_ << transferBtn;  // gated while any deploy/connect/install is running
        bl->addWidget(allBtn);
        bl->addWidget(noneBtn);
        bl->addSpacing(10);
        bl->addWidget(updateBtn);
        bl->addSpacing(10);
        // Root-hiding MODE, in the bar rather than the table: unlike the denylist beside it, this
        // is one value for the whole profile — off/shamiko/denylist/full describes how Magisk
        // hides, not which app it hides from — so it has no per-row meaning and cannot be a
        // column. It sits here because it is the master switch for the column: with mode=off the
        // denylist persists but does nothing, which is what the greyed toggles say.
        bl->addWidget(new QLabel(QStringLiteral("Hide root:")));
        rootHidingCombo_ = new QComboBox();
        for (const QString &m : {QStringLiteral("off"), QStringLiteral("shamiko"),
                                 QStringLiteral("denylist"), QStringLiteral("full")})
            rootHidingCombo_->addItem(m, m);
        rootHidingCombo_->setToolTip(QStringLiteral(
            "off = do nothing. denylist = Magisk's own DenyList for the apps ticked in the table. "
            "shamiko = unmount Magisk's modifications from denylisted processes (needs the shamiko "
            "build feature in the image). full = both.\n\n"
            "Applied by the deploy, so it takes effect on the next bringup — not a rebuild."));
        // The per-row toggles are enabled/greyed by this, so redraw the table when it moves.
        connect(rootHidingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this] { QMetaObject::invokeMethod(this, &MainWindow::rebuildIntegrationsList,
                                                   Qt::QueuedConnection); });
        bl->addWidget(rootHidingCombo_);
        bl->addStretch(1);
        bl->addWidget(transferBtn);
        v->addWidget(btns);
        // Denylisted packages the table has no row for (see rebuildIntegrationsList).
        denylistOrphanLabel_ = hintLabel(QString());
        denylistOrphanLabel_->hide();
        v->addWidget(denylistOrphanLabel_);
        col->addWidget(box, 1);  // the entries tree gets the spare page height
    }

    // (Install ARM app lives in the Install/transfer/back-up dialog; updates in the action bar;
    // Google Play certification moved to the Settings page — an account fix-it, not app work.)


    // (Move & back up apps lives in the Desktop-integration action bar; Device storage moved
    // to the Device page — it is storage plumbing, not app management.)

    devicePageIndex_ = pages_->count();  // for the visit-triggered data-dir lock re-probe
    col = newPage(QStringLiteral("Device"),
                  {"smartphone", "computer", "preferences-desktop-display"});

    // ── Device hardware & runtime (container side) ────────────────
    // No backend picker: the deploy target is inferred from the docker host (blank = this
    // machine, user@host = that machine over ssh — see inferBackend). Remora does not start
    // VMs; a KVM guest is just another docker host once it is running.
    {
        // Two columns of named sections, as on the Viewer page. One untitled card ran twelve
        // unrelated rows — a resolution, a sleep timer and five macvlan fields — down a single
        // list, so finding anything meant reading all of it. Left: what the device IS (display,
        // storage, power). Right: how it is REACHED (network), which is also where the five
        // macvlan rows live, so their appearing and disappearing stays inside one card.
        auto *rowW = new QWidget();
        auto *h = new QHBoxLayout(rowW);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);
        auto *lcol = new QVBoxLayout();
        auto *rcol = new QVBoxLayout();
        h->addLayout(lcol, 1);
        h->addLayout(rcol, 1);
        col->addWidget(rowW);

        // ── Storage: the data dir, and what else appears under /sdcard ──
        // (No separate /sdcard row: the data dir IS the /sdcard backing store, so opening it and
        // choosing it belong on the same line as the path they act on — see "Data dir" below.)
        auto *storageForm = card(lcol, QStringLiteral("Storage"));
        auto *form = storageForm;
        sharedFoldersEdit_ = new QLineEdit();
        sharedFoldersEdit_->setToolTip(QStringLiteral(
            "Docker-host folders to appear under /sdcard: <hostDir> or <hostDir>:<name>, "
            "space/comma separated. Empty = no extra shares. Applied on the next Connect "
            "(container recreate)."));
        // The same split button the data dir below wears, so the two rows of this card are one
        // shape: a path field and one control at its end. Icon-only — the label these carried was
        // eating the width of the path they were about.
        auto *sharedBrowse = new QToolButton();
        sharedBrowse->setPopupMode(QToolButton::MenuButtonPopup);
        sharedBrowse->setToolButtonStyle(Qt::ToolButtonIconOnly);
        sharedBrowse->setIcon(themeIcon({"folder-open", "document-open-folder"}));
        auto *sharedMenu = new QMenu(sharedBrowse);
        sharedMenu->setToolTipsVisible(true);
        // APPEND, not replace. This field is a LIST, so a picker that overwrote it would throw
        // away every other share to add one — and the row gives no hint that it would.
        auto *appendAct = sharedMenu->addAction(
            themeIcon({"list-add", "folder-new", "document-open-folder"}),
            QStringLiteral("Append a folder…"));
        appendAct->setToolTip(QStringLiteral(
            "Pick a folder and add it to the shares. The ones already listed stay."));
        connect(appendAct, &QAction::triggered, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Share a folder into /sdcard"), QDir::homePath());
            if (dir.isEmpty()) return;
            const QString cur = sharedFoldersEdit_->text().trimmed();
            sharedFoldersEdit_->setText(cur.isEmpty() ? dir
                                                      : cur + QLatin1Char(' ') + dir);
            // setText is programmatic — autosave needs the nudge (bd remora-4ei.28)
            emit sharedFoldersEdit_->editingFinished();
        });
        auto *clearSharesAct = sharedMenu->addAction(
            themeIcon({"edit-clear-all", "edit-clear", "edit-delete"}),
            QStringLiteral("Remove all shares"));
        clearSharesAct->setToolTip(QStringLiteral(
            "Empty the list. Nothing on the host is touched — the folders simply stop appearing "
            "under /sdcard on the next Connect."));
        connect(clearSharesAct, &QAction::triggered, this, [this] {
            if (sharedFoldersEdit_->text().trimmed().isEmpty()) return;
            sharedFoldersEdit_->clear();
            emit sharedFoldersEdit_->editingFinished();
        });
        sharedBrowse->setMenu(sharedMenu);
        sharedBrowse->setToolTip(appendAct->toolTip());
        // Primary click is the everyday one, as on the data dir: adding a share.
        connect(sharedBrowse, &QToolButton::clicked, appendAct, &QAction::trigger);
        auto *sharedRow = new QWidget();
        auto *srl = new QHBoxLayout(sharedRow);
        srl->setContentsMargins(0, 0, 0, 0);
        srl->addWidget(sharedFoldersEdit_, 1);
        srl->addWidget(sharedBrowse);
        form->addRow(QStringLiteral("Shared folders"), sharedRow);

        // ── Display: the panel the device believes it has ──
        auto *displayForm = card(lcol, QStringLiteral("Display"));
        // Placeholders MUST mirror the resolver's baked defaults (Resolver.cpp:28-30). They read
        // 1080/1920/420 — the old upstream-era values, never updated when the
        // resolver moved to 3760x1992/320, so the form told you blank meant something it did not.
        // Same failure mode that hid the 30 Hz refresh rate for weeks (bd remora-bm7.9): a default
        // stated in the UI and implemented differently. If you change one side, change both.
        widthEdit_ = new QLineEdit();  widthEdit_->setPlaceholderText(QStringLiteral("3760"));
        heightEdit_ = new QLineEdit(); heightEdit_->setPlaceholderText(QStringLiteral("1992"));
        dpiEdit_ = new QLineEdit();    dpiEdit_->setPlaceholderText(QStringLiteral("320"));
        // numeric fields sized for their values — full-width boxes for 4 digits read badly
        for (QLineEdit *e : {widthEdit_, heightEdit_, dpiEdit_}) setFieldChars(e, 4);
        // Refresh rate: editable so power users can still type an arbitrary panel rate, but the
        // dropdown surfaces the proven rungs (60 default → 240 validated). Blank keeps
        // the resolver default. Above 60 the resolver auto-caps the encoder (--max-fps 60) and the
        // device-tune pass pins peak/min refresh; >120 needs the clamp-patched hwc (baked since
        // the 240Hz work); >240 is untested. See bd remora-mzw / remora-c9y.
        fpsCombo_ = new QComboBox();
        fpsCombo_->setEditable(true);
        fpsCombo_->setInsertPolicy(QComboBox::NoInsert);
        fpsCombo_->lineEdit()->setPlaceholderText(QStringLiteral("60"));
        fpsCombo_->addItem(QString());  // blank = proven default
        for (const int hz : {60, 90, 120, 240})
            fpsCombo_->addItem(QString::number(hz), hz);
        fpsCombo_->setCurrentIndex(0);
        fpsCombo_->setToolTip(QStringLiteral(
            "Display refresh rate (Hz). Blank = the proven default (60).\n"
            "Above 60 the encoder is auto-capped and the refresh rate is pinned; "
            "120 and 240 are validated, >240 is untested."));
        setFieldChars(fpsCombo_, 4);
        // Width, height, DPI and refresh rate describe ONE thing — the panel — so they read as one
        // line. The four-row form this replaced spent four label gutters on four short numbers,
        // and put the rate far enough from the resolution to look like a separate subject.
        auto *resRow = new QWidget();
        auto *resl = new QHBoxLayout(resRow);
        resl->setContentsMargins(0, 0, 0, 0);
        resl->setSpacing(6);
        resl->addWidget(widthEdit_);
        resl->addWidget(new QLabel(QStringLiteral("×")));
        resl->addWidget(heightEdit_);
        resl->addSpacing(14);
        resl->addWidget(new QLabel(QStringLiteral("DPI")));
        resl->addWidget(dpiEdit_);
        resl->addSpacing(14);
        resl->addWidget(fpsCombo_);
        resl->addWidget(new QLabel(QStringLiteral("Hz")));
        resl->addStretch(1);  // the fields keep their own width; the slack goes to the right
        displayForm->addRow(QStringLiteral("Resolution"), resRow);
        // No animation-scale rows: they are Developer options' job, and Remora writing them on
        // every connect meant a change made there survived exactly until the next one. A new
        // image/profile now gets Android's own 1/1/1 (bd remora-4ei).
        autoSleepEdit_ = new QLineEdit();
        autoSleepEdit_->setPlaceholderText(QStringLiteral("off"));
        autoSleepEdit_->setToolTip(QStringLiteral(
            "Freeze the device (docker pause) after this many minutes with no mirror/app/desktop "
            "window attached — it stops burning host CPU/GPU, and any Connect wakes it in ~1s "
            "with the session intact. Blank/0 = off. Enforced by the auto-sleep watcher that "
            "`remora service` installs; changes apply on its next poll."));
        setFieldChars(autoSleepEdit_, 4);  // minutes

        // ── Power: the idle freeze ──
        auto *powerForm = card(lcol, QStringLiteral("Power"));
        powerForm->addRow(QStringLiteral("Auto-sleep (min)"), autoSleepEdit_);
        lcol->addStretch(1);

        // ── Network: how the device is reached, and the macvlan shape when it is asked for.
        // deviceForm_ is THIS card — syncConditionalRows hides the five macvlan rows through it,
        // so the rows that come and go must be the ones this pointer owns.
        deviceForm_ = card(rcol, QStringLiteral("Network"));
        form = deviceForm_;
        // overlayfs /data is ON by default and not user-facing: profiles share one base
        // with private diff layers. use_overlayfs=false in remorarc opts out.
        networkModeCombo_ = new QComboBox();
        networkModeCombo_->addItem(QStringLiteral("(default — port-forward)"),
                                   QString());
        networkModeCombo_->addItem(QStringLiteral("macvlan (container gets its own LAN IP)"),
                                   QStringLiteral("macvlan"));
        networkModeCombo_->addItem(QStringLiteral("port-forward (docker bridge)"),
                                   QStringLiteral("bridge"));
        networkModeCombo_->setToolTip(QStringLiteral(
            "macvlan puts the container on the LAN with its own address (needed for LAN-visible "
            "adb/casting); port-forward publishes adb through the docker host instead."));
        form->addRow(QStringLiteral("Networking"), networkModeCombo_);
        connect(networkModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                &MainWindow::syncConditionalRows);
        macvlanEdit_ = new QLineEdit();
        setFieldChars(macvlanEdit_, 16);  // an IPv4 address
        macvlanEdit_->setPlaceholderText(QStringLiteral("auto-assigned"));
        macvlanEdit_->setToolTip(QStringLiteral(
            "Optional pinned LAN IP for macvlan mode. Blank lets docker assign a free one — no "
            "conflicts when several deployments run; Remora discovers it at connect time."));
        form->addRow(QStringLiteral("macvlan IP"), macvlanEdit_);
        // The macvlan network's shape. Every one of these is blank by default and stays blank: the
        // deploy reads the docker host's own primary interface and creates the network from it, so
        // these rows exist for the host the automatic answer is wrong for — a second NIC, a VLAN,
        // a subnet the router does not own end-to-end (bd remora-400).
        macvlanSubnetEdit_ = new QLineEdit();
        macvlanSubnetEdit_->setMaximumWidth(320);
        macvlanSubnetEdit_->setPlaceholderText(
            QStringLiteral("auto — the docker host's own subnet"));
        macvlanSubnetEdit_->setToolTip(QStringLiteral(
            "Subnet the macvlan network is created with, e.g. 192.168.0.0/24. Blank uses the "
            "network of the docker host's default-route interface."));
        form->addRow(QStringLiteral("macvlan subnet"), macvlanSubnetEdit_);
        macvlanGatewayEdit_ = new QLineEdit();
        macvlanGatewayEdit_->setMaximumWidth(320);
        macvlanGatewayEdit_->setPlaceholderText(QStringLiteral("auto — the host's default gateway"));
        form->addRow(QStringLiteral("macvlan gateway"), macvlanGatewayEdit_);
        macvlanParentEdit_ = new QLineEdit();
        macvlanParentEdit_->setMaximumWidth(320);
        macvlanParentEdit_->setPlaceholderText(
            QStringLiteral("auto — the host's default-route NIC"));
        macvlanParentEdit_->setToolTip(QStringLiteral(
            "Host interface the macvlan rides. Blank uses the interface the docker host's default "
            "route leaves by."));
        form->addRow(QStringLiteral("macvlan parent NIC"), macvlanParentEdit_);
        macvlanRangeEdit_ = new QLineEdit();
        macvlanRangeEdit_->setMaximumWidth(320);
        macvlanRangeEdit_->setPlaceholderText(QStringLiteral("auto — the top /28 of the subnet"));
        macvlanRangeEdit_->setToolTip(QStringLiteral(
            "Slice of the subnet docker may auto-assign from. The default keeps assigned addresses "
            "clear of a router's DHCP pool; a pinned macvlan IP is not restricted by it."));
        form->addRow(QStringLiteral("macvlan auto-IP range"), macvlanRangeEdit_);
        // DNS belongs here, not on the Image page's Advanced card where it used to live: it is
        // network.dns in the config, it is written in remorarc's network group beside the macvlan
        // keys, and it is a docker-run boot arg — so it costs a container recreate, never a
        // rebuild. Behind "expert image knobs" it implied a rebuild it does not need.
        dnsEdit_ = new QLineEdit();
        dnsEdit_->setMaximumWidth(320);
        dnsEdit_->setPlaceholderText(QStringLiteral("1.1.1.1, 8.8.8.8"));
        dnsEdit_->setToolTip(QStringLiteral(
            "Resolvers the device boots with, comma-separated (four at most) — "
            "androidboot.remora_net_dns*. "
            "Blank uses the image's own default. Applies to both networking modes; takes effect "
            "on the next container recreate ('remora up'), not a setprop."));
        form->addRow(QStringLiteral("DNS"), dnsEdit_);
        // Proxy sits beside DNS for the same reason DNS sits here: network.proxy* keys in the
        // same remorarc group, the same boot-arg block in the builder, the same recreate-not-
        // rebuild cost (bd remora-w02j). Only the vocabulary ipconfigstore actually reads is
        // offered — static and none; pac is real too but needs a PAC URL the config does not
        // carry, so it stays a hand edit until someone asks for it.
        proxyTypeCombo_ = new QComboBox();
        proxyTypeCombo_->addItem(QStringLiteral("(no proxy configured)"), QString());
        proxyTypeCombo_->addItem(QStringLiteral("static — explicit host + port"),
                                 QStringLiteral("static"));
        proxyTypeCombo_->addItem(QStringLiteral("none — explicitly no proxy"),
                                 QStringLiteral("none"));
        proxyTypeCombo_->setToolTip(QStringLiteral(
            "Ethernet proxy the device boots with — androidboot.remora_net_proxy_*. "
            "'static' publishes the host and port below to every app that honours the system "
            "proxy; 'none' writes an explicit no-proxy record. Takes effect on the next "
            "container recreate ('remora up')."));
        form->addRow(QStringLiteral("Proxy"), proxyTypeCombo_);
        connect(proxyTypeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                &MainWindow::syncConditionalRows);
        proxyHostEdit_ = new QLineEdit();
        proxyHostEdit_->setMaximumWidth(320);
        proxyHostEdit_->setPlaceholderText(QStringLiteral("192.168.0.1"));
        form->addRow(QStringLiteral("Proxy host"), proxyHostEdit_);
        proxyPortEdit_ = new QLineEdit();
        setFieldChars(proxyPortEdit_, 5);  // a port number
        proxyPortEdit_->setPlaceholderText(QStringLiteral("3128"));
        proxyPortEdit_->setToolTip(
            QStringLiteral("Blank uses the image's default, 3128 (ipconfigstore)."));
        form->addRow(QStringLiteral("Proxy port"), proxyPortEdit_);
        // Data dir: the container's /data volume, and therefore the /sdcard backing store — so
        // this one row carries the path, opening what is inside it, and choosing a different one.
        dataDirEdit_ = new QLineEdit();
        dataDirEdit_->setPlaceholderText(QStringLiteral("~/.remora-bm-data"));
        dataDirEdit_->setToolTip(QStringLiteral(
            "Host path mounted at /data — the instance's Android state (apps, accounts). "
            "The path is on the docker host: this machine, or the ssh host for a remote "
            "profile.\n\n"
            "Read-only while the profile is running: the container is bound to this path, so "
            "repointing it under a live device changes nothing and describes something untrue."));
        // One split button carrying both things you can want from a data dir. The primary click
        // is the everyday one — show me what is on the device — and the drop-down holds the rare
        // one. Naming both in the menu beats a button that silently means different things
        // depending on state you cannot see from the row.
        dataDirBtn_ = new QToolButton();
        dataDirBtn_->setPopupMode(QToolButton::MenuButtonPopup);
        dataDirBtn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        auto *dataDirMenu = new QMenu(dataDirBtn_);
        // Qt hides QAction tooltips in menus by default, and the disabled picker's tooltip is
        // where "stop the device first" is written — without this it explains nothing.
        dataDirMenu->setToolTipsVisible(true);
        // A FOLDER, not drive-harddisk: themeIcon silhouettes every icon in the accent
        // (AccentIconEngine, SourceIn), and a disk is a filled slab — flattened to one colour it
        // is an accent-coloured rectangle with nothing left to read. A folder's shape survives
        // being one colour, which is also why the shares button beside it uses the same one.
        auto *openAct = dataDirMenu->addAction(
            themeIcon({"folder-open", "document-open-folder"}),
            QStringLiteral("Open in file manager"));
        openAct->setToolTip(QStringLiteral(
            "Open this profile's /sdcard (<data dir>/media/0) — a local path for a local deploy, "
            "an sftp:// URL for a remote one. If the tree is not yours to read yet (Android's "
            "media_rw owns it after first boot), this grants your user access first."));
        connect(openAct, &QAction::triggered, this, &MainWindow::onOpenDeviceStorage);
        // A different glyph from the row above it — with both flattened to the accent, two
        // identical silhouettes down one menu tell you nothing about which is which.
        dataDirPickAct_ = dataDirMenu->addAction(
            themeIcon({"document-open-folder", "folder-open"}),
            QStringLiteral("Select new location…"));
        connect(dataDirPickAct_, &QAction::triggered, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Select data directory"),
                dataDirEdit_->text().isEmpty() ? QDir::homePath() : dataDirEdit_->text());
            if (dir.isEmpty()) return;
            dataDirEdit_->setText(tildify(dir));
            // Same trap as addBrowseDir() below, and this handler is hand-rolled rather than going
            // through it, so the fix there does not reach this field: setText() is programmatic, Qt
            // emits no editingFinished, and autosave() therefore never writes the chosen data dir to
            // remorarc — it would silently revert to the on-disk value on the next GUI start, while
            // the running instance kept using the widget's value (bd remora-4ei.28).
            emit dataDirEdit_->editingFinished();
        });
        dataDirBtn_->setMenu(dataDirMenu);
        // Icon only, with the menu keeping the full wording — in a half-width column even the
        // short "Open" left the path itself little room, and both actions are named one click
        // away. Deliberately not setDefaultAction(), which would drag the long text back on.
        dataDirBtn_->setIcon(themeIcon({"folder-open", "document-open-folder"}));
        dataDirBtn_->setToolTip(openAct->toolTip());
        connect(dataDirBtn_, &QToolButton::clicked, this, &MainWindow::onOpenDeviceStorage);
        dataDirRow_ = new QWidget();
        auto *drl = new QHBoxLayout(dataDirRow_);
        drl->setContentsMargins(0, 0, 0, 0);
        drl->addWidget(dataDirEdit_, 1);
        drl->addWidget(dataDirBtn_);
        // Storage's FIRST row, though it is built last: the data dir is what that card is about,
        // and shared folders are the extra thing layered on top of it.
        storageForm->insertRow(0, QStringLiteral("Data dir"), dataDirRow_);
        // Locked state is asked of docker, not assumed: start locked so a running device can
        // never be handed an editable path in the gap before the first probe answers.
        setDataDirLocked(true);
        refreshDataDirLock();
        // The rest of the per-profile identity (bd remora-4u4.8). These three are what two
        // concurrently-running profiles must not share, and a new profile has all three pinned
        // at creation — so showing only the data dir left the other two invisible and, for the
        // adb port, unguessable. Blank still means "resolver default", as everywhere else.
        // container name: derived from the profile name at creation
        // (allocateInstanceIdentity) — remorarc-only, `remora plan` shows it
        hostAdbPortEdit_ = new QLineEdit();
        setFieldChars(hostAdbPortEdit_, 5);  // a port number
        hostAdbPortEdit_->setPlaceholderText(QStringLiteral("5555"));
        hostAdbPortEdit_->setToolTip(QStringLiteral(
            "Host port adb connects on — this is the port in `adb connect localhost:<port>`. "
            "Each simultaneously-running profile needs its own; a new profile is given the "
            "lowest free one at or above 5555. Not used with network_mode=macvlan, where the "
            "container has its own LAN address and nothing is published on the host."));
        form->addRow(QStringLiteral("Host adb port"), hostAdbPortEdit_);
        rcol->addStretch(1);

        // One label gutter PER COLUMN: cards stacked above each other line up, and the two
        // columns stay independent — a wide "macvlan auto-IP range" label on the right must not
        // indent every field on the left.
        alignFormLabels({storageForm, displayForm, powerForm});
        alignFormLabels({deviceForm_});
    }

    // ── System properties ─────────────────────────────────────────
    // In the open, and not called "Advanced". ro overrides become androidboot.ro.* on the docker
    // run argv, which makes them the same KIND of thing as resolution, fps and DNS above:
    // properties of the container this page configures, baked at create time and costing a
    // recreate to change. A collapsed disclosure implied they were a different, riskier class of
    // setting than the rows they sit beside, which they are not.
    {
        auto *form = card(col, QStringLiteral("System properties"),
                          QStringLiteral("Read-only properties baked onto the container at "
                                         "create time. Comma-separate the list."));
        roEdit_ = new QLineEdit();
        roEdit_->setPlaceholderText(QStringLiteral("product.first_api_level=25, …"));
        roEdit_->setToolTip(QStringLiteral(
            "Each entry becomes androidboot.ro.<key>=<value> on the container's boot args, landing "
            "as a ro.* system property. Baked at docker run, so changes need a container recreate "
            "('remora up') — a setprop on the running device will not do it."));
        form->addRow(QStringLiteral("ro overrides"), roEdit_);
    }

    col->addStretch(1);
    col = newPage(QStringLiteral("Viewer"), {"video-display", "view-fullscreen", "camera-video"});

    // ── Viewer (mirror client) — video/audio beside window/fullscreen ─────────────
    {
        auto *rowW = new QWidget();
        auto *h = new QHBoxLayout(rowW);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);
        auto *lcol = new QVBoxLayout();
        auto *rcol = new QVBoxLayout();
        h->addLayout(lcol, 1);
        h->addLayout(rcol, 1);
        col->addWidget(rowW);

        auto *form = card(lcol, QStringLiteral("Video && audio"));
        auto *videoUnit = new QLabel(QStringLiteral("M"));
        // Codec and bit-rate are one decision — the rate that is right for h264 is not the rate
        // that is right for h265 — so they share a row rather than being read a line apart.
        bitrateEdit_ = new QLineEdit();
        bitrateEdit_->setPlaceholderText(QStringLiteral("30"));  // mirrors Resolver.cpp:36
        bitrateEdit_->setToolTip(QStringLiteral(
            "Video bit-rate in Mbps. Blank = 30.\n\n"
            "A unit suffix written by hand in remorarc (8000K, or a bare bit count) is kept and "
            "shown as it stands — the M beside the box is what a plain number here means, not a "
            "conversion applied to one you already set."));
        setFieldChars(bitrateEdit_, 4);
        videoCodecCombo_ = new QComboBox();
        for (const QString &c : {QStringLiteral("h264"), QStringLiteral("h265"),
                                 QStringLiteral("av1")})
            videoCodecCombo_->addItem(c, c);
        // The caveats live here rather than in the item labels: a dropdown is a list of codecs,
        // and three names that read like three names are easier to choose between than three
        // sentences. Nothing is lost — the resolver states the same thing where it acts on it.
        videoCodecCombo_->setToolTip(QStringLiteral(
            "h265 is the default: the -hwc2 image's hardware HEVC encoder. It needs an image "
            "carrying that encoder — the resolver picks the -hwc2 variant for a feature-based "
            "profile — and a host GPU with VA-API encode; without one, the deploy falls back to "
            "h264 and says so.\n\n"
            "h264 works on every image.\n\n"
            "av1 has no hardware encoder in any image yet, so it encodes in software."));
        {
            auto *vid = new QWidget();
            auto *vl = new QHBoxLayout(vid);
            vl->setContentsMargins(0, 0, 0, 0);
            vl->setSpacing(6);
            vl->addWidget(videoCodecCombo_, 1);
            vl->addWidget(bitrateEdit_);
            // The unit sits outside the box: a field holding "30M" reads as free text to be
            // spelled right, and a field holding "30" reads as a number.
            vl->addWidget(videoUnit);
            form->addRow(QStringLiteral("Video"), vid);
        }
        // No hardware-decode row: client-side GPU decode is on for every profile now and falls
        // back to software on its own when the host has no usable decoder, so the checkbox only
        // ever offered a slower deploy. hw_decode=false in remorarc still forces it off on a host
        // whose decoder misbehaves.
        // No codec2 prompt on codec change either: HEVC/AV1 are codec2-only encoders, and the
        // pipeline they need is now on for every profile, so picking one can no longer leave the
        // profile in the state this modal existed to announce. What h265 still needs is an image
        // carrying the encoder — the resolver bumps an A16 gapps image to its -hwc2 variant on its
        // own, and the codec tooltip says so.
        // Audio is one switch — the mirror plays the device's audio (opus) or it does not. The
        // codec and buffer knobs that used to sit here reached nothing: the mirror takes neither.
        audioCheck_ = new QCheckBox(QStringLiteral("Play device audio"));
        audioCheck_->setToolTip(QStringLiteral(
            "Play the device's audio through the mirror window (--audio). Off by default.\n\n"
            "The PIP window never plays it: the main window already does, and a second stream "
            "would double every sound."));
        form->addRow(QStringLiteral("Audio"), audioCheck_);
        // Window size joins the two rows above it: all three describe the picture the mirror puts on
        // this desktop, and it was the one of them filed under the card about Android's own
        // displays. The suffixed unit is the row's own — px is not a knob you switch off.
        winWidthEdit_ = new QLineEdit();  winWidthEdit_->setPlaceholderText(QStringLiteral("auto"));
        winHeightEdit_ = new QLineEdit(); winHeightEdit_->setPlaceholderText(QStringLiteral("auto"));
        for (QLineEdit *e : {winWidthEdit_, winHeightEdit_}) setFieldChars(e, 4);
        {
            auto *win = new QWidget();
            auto *wl = new QHBoxLayout(win);
            wl->setContentsMargins(0, 0, 0, 0);
            wl->setSpacing(6);
            wl->addWidget(winWidthEdit_);
            wl->addWidget(new QLabel(QStringLiteral("×")));
            wl->addWidget(winHeightEdit_);
            wl->addStretch(1);
            form->addRow(QStringLiteral("Window size"), win);
        }
        // No audio-gain row: the quiet upstream capture it compensated for is fixed, so the boost
        // is dead weight. No independent-audio row either — multi audio focus is on for every
        // profile now (see buildDeviceTuneArgv); nothing here wants apps silencing each other.
        // ── GPU (relocated from the Device page): what renders the device ──
        auto *gpuForm = card(lcol, QStringLiteral("GPU"));
        gpuDriverCombo_ = new QComboBox();
        gpuDriverCombo_->setEditable(true);
        gpuDriverCombo_->addItem(QStringLiteral("(auto — first probed GPU)"), QString());
        // SwiftShader is an ANSWER to "what renders this", not a mode that suspends the question —
        // so it is an entry here rather than a checkbox that greys the list out. Its own row was
        // the last of three spellings of one choice (auto / guest / the box), and every one of
        // them was really just picking the renderer this list already names.
        gpuDriverCombo_->addItem(QStringLiteral("SwiftShader  —  software (CPU)"), kGpuSoftware);
        // NVIDIA belongs in the list of GPUs even though the guest cannot open its node: to anyone
        // choosing a GPU, "which card renders this" is one question, and splitting it across a
        // driver combo that hides NVIDIA and a separate Venus tri-state made the only NVIDIA
        // option invisible on an NVIDIA machine. The mechanism differs, so the label says so.
        gpuDriverCombo_->addItem(QStringLiteral("NVIDIA  —  via Venus"), kGpuVenus);
        gpuDriverCombo_->setToolTip(QStringLiteral(
            "What renders the device. Stored by DRIVER name and "
            "resolved to a live node at each deploy — node numbers rotate across boots, drivers "
            "do not, so this choice survives where a baked /dev/dri path would silently move to "
            "the wrong card. An explicit gpu_node= in remorarc still wins for exact control.\n\n"
            "The probed entries are the local machine's; for a remote backend type the driver "
            "name (xe, i915, amdgpu, ...).\n\n"
            "'SwiftShader — software' renders on the CPU: portable, deterministic and it leaves "
            "the GPU alone, but a fraction of the speed. It is what an unset profile gets.\n\n"
            "'NVIDIA — via Venus' is a different mechanism, not just another node: the guest cannot "
            "open the NVIDIA node directly, so guest Vulkan goes over a unix socket to a host "
            "virglrenderer render server driving the proprietary driver, with ANGLE supplying GL "
            "on top. It needs an image built with the minigbm_gbm_mesa_vtest patch; install the "
            "stack with `remora venus-build` and `remora check` names anything missing.\n\n"
            "Leaving this on auto leaves Venus on auto too — it engages only when an NVIDIA render "
            "node, the render server and all five guest libraries are present, so auto can never "
            "switch a machine onto a stack whose pieces are missing. Picking a specific GPU turns "
            "Venus off."));
        // Fill the probed entries off-thread: one sh scan, but the Device page must not stall on
        // profile switches.
        {
            QPointer<QComboBox> combo(gpuDriverCombo_);
            auto *scan = QThread::create([combo] {
                RealSpawner sp;
                const QList<RenderNode> nodes = probeRenderNodes(sp);
                QMetaObject::invokeMethod(qApp, [combo, nodes] {
                    if (!combo) return;
                    for (const RenderNode &n : nodes) {
                        if (n.driver == QLatin1String("nvidia") ||
                            n.driver == QLatin1String("vgem") ||
                            combo->findData(n.driver) >= 0)
                            continue;
                        combo->addItem(QStringLiteral("%1  —  %2").arg(n.driver, n.node),
                                       n.driver);
                    }
                });
            });
            connect(scan, &QThread::finished, scan, &QObject::deleteLater);
            scan->start();
        }
        gpuForm->addRow(QStringLiteral("GPU"), gpuDriverCombo_);
        // Selecting NVIDIA/Venus is what makes the gbm_mesa vtest gralloc a real option — its
        // note is blunt that it cannot allocate without a vtest server listening.
        connect(gpuDriverCombo_, &QComboBox::currentTextChanged, this,
                [this] { refreshPatchApplicability(); });
        // Kept, hidden, and only shown when the profile is in a state the GPU list cannot
        // express — venus explicitly OFF with no GPU chosen. Folding that into "(auto)" would
        // silently re-enable Venus for someone who deliberately turned it off.
        venusCombo_ = new QComboBox();
        venusCombo_->addItem(QStringLiteral("auto — on when the stack is installed"), QString());
        venusCombo_->addItem(QStringLiteral("on (force)"), QStringLiteral("true"));
        venusCombo_->addItem(QStringLiteral("off"), QStringLiteral("false"));
        venusCombo_->setToolTip(QStringLiteral(
            "NVIDIA acceleration for the bare/remote backends via Mesa Venus: guest Vulkan goes "
            "over a unix socket to a host virglrenderer render server driving the proprietary "
            "NVIDIA driver, with ANGLE supplying GL on top.\n\n"
            "Auto turns it on only when an NVIDIA render node, the render server and all five "
            "guest libraries are present, so it can never switch a machine onto a stack whose "
            "pieces are missing. Install them with `remora venus-build`; `remora check` names "
            "whatever is short.\n\n"
            "Needs a GPU selected rather than SwiftShader, and an image built with the "
            "minigbm_gbm_mesa_vtest patch."));
        venusRowLabel_ = new QLabel(QStringLiteral("NVIDIA Venus"));
        gpuForm->addRow(venusRowLabel_, venusCombo_);
        venusCombo_->hide();
        venusRowLabel_->hide();

        // Window size moved out to Video & audio, next to the codec and the bit-rate it belongs
        // with. What is left here is the hardware the guest is handed and the Android displays it
        // is asked for — hence the name.
        auto *winForm = card(rcol, QStringLiteral("Devices"));
        // Which of THIS host's keyboards the guest impersonates. Gboard gates its
        // physical-keyboard toolbar on the identity, so a guest advertising generic ids gets the
        // full on-screen keyboard instead (bd remora-a41). Auto cannot tell which keyboard the
        // human types on — a wireless MOUSE receiver enumerates a keyboard endpoint too — so the
        // list is shown rather than decided silently.
        kbdIdentityCombo_ = new QComboBox();
        kbdIdentityCombo_->setToolTip(QStringLiteral(
            "The guest's passthrough keyboard advertises this identity. Gboard only offers its "
            "physical-keyboard toolbar for a keyboard that looks real, so pick the one you "
            "actually type on. 'Generic' keeps the old behaviour: a working keyboard, no toolbar."));
        populateKbdIdentities();
        winForm->addRow(QStringLiteral("Keyboard identity"), kbdIdentityCombo_);
        // Share host input devices with Android. Ticked devices are exposed into the container
        // and keep working on this host at the same time — Android opens the same evdev node the
        // desktop reads, and nothing grabs it (bd remora-4ei.36).
        sharedInputsList_ = new QListWidget();
        sharedInputsList_->setToolTip(QStringLiteral(
            "Tick a device to share it with Android. It keeps working here too — Android reads "
            "the same device node without taking it over. The device reaches Android with its "
            "real bus, vendor and product, so a Bluetooth controller arrives as Bluetooth and "
            "games recognise it."));
        sharedInputsList_->setMaximumHeight(110);
        populateSharedInputs();
        connect(sharedInputsList_, &QListWidget::itemChanged, this, &MainWindow::autosave);
        winForm->addRow(QStringLiteral("Share devices"), sharedInputsList_);

        // ── Desktop mode: the Runners page's Desktop action, and the display it opens ──
        // Its own card because it is the only subject on this page that is not about the mirror:
        // four rows describing a SECOND Android display, which is why they read as strays wherever
        // they were parked. Mirrored fullscreen always stretches to fill — letterboxing is not a
        // concept Remora keeps.
        auto *fsForm = card(lcol, QStringLiteral("Desktop mode"));
        // Opt-in separate display: Desktop mode opens one instead of mirroring.
        fsSeparateCheck_ = new QCheckBox(QStringLiteral("Separate display (desktop mode)"));
        fsSeparateCheck_->setToolTip(QStringLiteral(
            "Make the Runners page's Desktop mode action open a SEPARATE Android display at the "
            "resolution below (--new-display) instead of mirroring the main one. Useful for a "
            "dedicated fullscreen app at a different resolution; the windowed mirror keeps "
            "running independently."));
        fsForm->addRow(QString(), fsSeparateCheck_);
        fsDisplayEdit_ = new QLineEdit();
        fsDisplayEdit_->setPlaceholderText(QStringLiteral("e.g. 3840x2160/240 — blank = main size"));
        fsDisplayEdit_->setToolTip(QStringLiteral(
            "Desktop-mode display resolution (--new-display \"<W>x<H>[/<dpi>]\"). Only used "
            "when 'Separate display' is checked."));
        fsForm->addRow(QStringLiteral("Desktop res"), fsDisplayEdit_);
        fsAppEdit_ = new QLineEdit();
        fsAppEdit_->setPlaceholderText(QStringLiteral("e.g. tv.pluto.android — optional"));
        fsAppEdit_->setToolTip(QStringLiteral(
            "Desktop mode: app to auto-launch on the separate display (otherwise blank). Only used "
            "when 'Separate display' is checked."));
        fsForm->addRow(QStringLiteral("Desktop app"), fsAppEdit_);
        fsNewWindowCheck_ = new QCheckBox(QStringLiteral("Desktop mode opens a new window"));
        fsNewWindowCheck_->setToolTip(QStringLiteral(
            "The Alt+D desktop shortcut: off (default) replaces the current desktop window each "
            "press (one at a time); on opens an additional new window every press."));
        fsForm->addRow(QString(), fsNewWindowCheck_);
        // desktop res/app apply only in separate-display mode — hide the rows outright (stretch
        // stays live either way: it governs the mirrored fullscreen AND the keyboard-fullscreen
        // shortcut from a windowed run).
        auto syncFsRows = [this, fsForm] {
            const bool sep = fsSeparateCheck_->isChecked();
            fsForm->setRowVisible(fsDisplayEdit_, sep);
            fsForm->setRowVisible(fsAppEdit_, sep);
        };
        connect(fsSeparateCheck_, &QCheckBox::toggled, this, [syncFsRows](bool) { syncFsRows(); });
        syncFsRows();
        // One label gutter down the whole column: its three cards are read as one stack, so
        // their value fields have to start at the same x or the later ones read as asides.
        // "Desktop res" is the widest label, so the two cards above widen to match rather than
        // each card indenting to its own longest word.
        alignFormLabels({form, gpuForm, fsForm});
        lcol->addStretch(1);

        // ── Navigation: what triggers Back / Home / Recents inside the mirror ──
        auto *navForm = card(rcol, QStringLiteral("Navigation"));
        for (const NavAction &act : kNavActions) {
            auto *combo = new QComboBox();
            combo->addItem(QStringLiteral("— none —"), -1);
            for (int b = 0; b < kNavButtons.size(); ++b) combo->addItem(kNavButtons.at(b), b);
            combo->setToolTip(QStringLiteral(
                "Which mouse button triggers %1. A button is worth one action, so picking one "
                "already spoken for takes it from the other row; 'none' hands that button's click "
                "straight to Android instead.\n\n"
                "Holding Shift while clicking always forwards the raw click, whatever is set here.")
                                  .arg(QLatin1String(act.label)));
            auto *keyHint = new QLabel();  // filled by syncNavKeyHints() once the modifier is known
            keyHint->setToolTip(QStringLiteral(
                "The keyboard route to the same action. The letter is fixed and only the "
                "modifier is ours to choose — set it in 'Shortcut key' below."));
            // The direct-key route: one arbitrary key, no modifier (--key-bind). This is the
            // only way an unmodified key can mean Recents — no SDL key maps to APP_SWITCH, so no
            // host-side remapping could do it (bd remora-pdu).
            auto *keyEdit = new QKeySequenceEdit();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            keyEdit->setMaximumSequenceLength(1);  // below 6.5 the handler truncates instead
#endif
            keyEdit->setClearButtonEnabled(true);
            setFieldChars(keyEdit, 9);
            keyEdit->setToolTip(QStringLiteral(
                "A single key that triggers %1 on its own, no modifier. The mirror swallows it, so "
                "Android never sees the key itself — function and navigation keys (F1…, PageUp, "
                "Pause) cost nothing, while a letter bound here becomes untypable in the mirror.\n\n"
                "Any modifier pressed while capturing is dropped: the binding is always the bare "
                "key. Empty = chord only.")
                                    .arg(QLatin1String(act.label)));
            auto *rowW = new QWidget();
            auto *rowL = new QHBoxLayout(rowW);
            rowL->setContentsMargins(0, 0, 0, 0);
            rowL->setSpacing(8);
            rowL->addWidget(combo, 1);
            rowL->addWidget(keyEdit);
            rowL->addWidget(keyHint);
            navForm->addRow(QLatin1String(act.label), rowW);
            navActionCombos_ << combo;
            navKeyEdits_ << keyEdit;
            navKeyRawNames_ << QString();
            navKeyHints_ << keyHint;
        }
        // A captured key is normalised to its bare keycode, refused outright when it cannot be
        // spelled for the mirror, and taken from any other row that held it — the fork refuses a
        // duplicate key at launch, which would cost the whole mirror for one stale row.
        for (int i = 0; i < navKeyEdits_.size(); ++i) {
            connect(navKeyEdits_.at(i), &QKeySequenceEdit::keySequenceChanged, this,
                    [this, i](const QKeySequence &seq) {
                if (suppressAutosave_) return;
                auto *edit = navKeyEdits_.at(i);
                int key = 0;
                if (seq.count() >= 1) {
                    key = seq[0].key();
                    if (sdlKeyName(key).isEmpty()) {
                        const QSignalBlocker b(edit);  // unbindable: put back what the row had
                        const int last = edit->property("lastGoodKey").toInt();
                        edit->setKeySequence(last ? QKeySequence(last) : QKeySequence());
                        return;
                    }
                    // The binding is always ONE bare key. A second key only arrives on Qt < 6.5,
                    // which has no maximumSequenceLength to stop the edit recording it.
                    if (seq.count() > 1 || seq[0].keyboardModifiers() != Qt::NoModifier) {
                        const QSignalBlocker b(edit);
                        edit->setKeySequence(QKeySequence(key));
                    }
                    for (int j = 0; j < navKeyEdits_.size(); ++j) {
                        auto *other = navKeyEdits_.at(j);
                        if (j != i && !other->keySequence().isEmpty()
                                && other->keySequence()[0].key() == key) {
                            const QSignalBlocker b(other);
                            other->clear();
                            other->setProperty("lastGoodKey", 0);
                            navKeyRawNames_[j].clear();
                        }
                    }
                }
                edit->setProperty("lastGoodKey", key);
                navKeyRawNames_[i].clear();  // the row now says exactly what the field shows
                autosave();
            });
        }
        // One button, one action: taking a button that another row holds must release it there,
        // or the vector below would carry the action twice and this card could not show it.
        for (int i = 0; i < navActionCombos_.size(); ++i) {
            connect(navActionCombos_.at(i), &QComboBox::currentIndexChanged, this, [this, i] {
                if (suppressAutosave_) return;
                const int btn = navActionCombos_.at(i)->currentData().toInt();
                if (btn >= 0)
                    for (int j = 0; j < navActionCombos_.size(); ++j)
                        if (j != i && navActionCombos_.at(j)->currentData().toInt() == btn)
                            navActionCombos_.at(j)->setCurrentIndex(0);  // "— none —"
                autosave();
            });
        }
        shortcutModCombo_ = new QComboBox();
        for (const ShortcutMod &m : kShortcutMods)
            shortcutModCombo_->addItem(QLatin1String(m.label), QLatin1String(m.value));
        shortcutModCombo_->setToolTip(QStringLiteral(
            "The modifier that arms the mirror's navigation chords. The letters are fixed — b, "
            "h, s, n are back, home, recents and notifications and cannot be reassigned — so this "
            "is the whole of the keyboard binding.\n\n"
            "Pick a modifier the guest never needs: the chord is swallowed by the mirror and never "
            "reaches Android."));
        connect(shortcutModCombo_, &QComboBox::currentIndexChanged, this, [this] {
            syncNavKeyHints();
            autosave();
        });
        navForm->addRow(QStringLiteral("Shortcut key"), shortcutModCombo_);
        syncNavKeyHints();
        alignFormLabels({winForm, navForm});
        rcol->addStretch(1);
    }

    col->addStretch(1);
    col = newPage(QStringLiteral("Image"), {"package-x-generic", "application-x-archive", "drive-harddisk"});

    // ── Image: Android version + à-la-carte features ─────────────
    {
        // No Android-version control at all: the version is chosen when the profile is created
        // (onNewProfile writes fresh.image.androidVersion) and every other thing on this page is
        // scoped BY it — the feature set, the patch set, the ref list. Offering it as an editable
        // combo here invited changing a live profile's Android release from the same box you tick
        // features in, which is a new profile, not an edit. The sidebar header already names it.
        // No "Prebuilt image" row: the feature selection below picks the image, always. A profile
        // that must pin one names it as image_tag in remorarc — still honoured by the resolver, and
        // still announced by the source-disabled note, which is the only place it now shows.
        // No "Resolves to" row either: it narrated a tag the user does not choose any more.
        // No "Build image" button: the overlay-recipe build and the registry download it fronted
        // were the OTHER of two build buttons; "Build from source" is the one that remains.

        // Titleless: the box sits behind a disclosure now, and the button carries the title.
        auto *featBox = new QGroupBox();
        featBox->setProperty("folded", true);
        auto *fv = new QVBoxLayout(featBox);
        featureGrid_ = new QGridLayout();  // two columns, under section headings
        featureGrid_->setHorizontalSpacing(16);
        featureGrid_->setVerticalSpacing(4);
        // One header widget per section, built once and re-parented into the grid on every repack.
        // Rebuilding them per repack would leak a label per pass and lose their fonts.
        for (const auto &sec : featureSections()) {
            auto *head = new QLabel(sec.second, featBox);
            QFont hf = head->font();
            hf.setBold(true);
            head->setFont(hf);
            head->hide();
            sectionHeaders_.insert(sec.first, head);
        }
        // One checkbox per row, nothing beside it. The maturity pill (OK/CAVEAT/PARKED/MANDATORY)
        // and the red ⚠ gate marker both went: thirteen rows each carrying a coloured chip read as
        // thirteen things demanding attention, when the honest answer for most of them is "fine".
        // Neither signal is lost — featureTooltip() folds the maturity word and, when the gating
        // engine has an objection to the current selection, its reason into the checkbox's own
        // tooltip, which recompute() refreshes. Gating itself is untouched: it still runs and the
        // build still refuses a gated combination.
        for (const Feature &f : buildFeatures()) {
            auto *cb = new QCheckBox(f.label);
            cb->setChecked(features_.contains(f.key));
            if (f.status == FeatureStatus::Mandatory) {
                cb->setChecked(true);
                cb->setEnabled(false);
            }
            cb->setToolTip(featureTooltip(f, QString()));
            cb->setParent(featBox);  // positioned by repackFeatureGrid(), like the patch rows
            featureChecks_.insert(f.key, cb);
            featureOrder_ << f.key;
            const QString key = f.key;
            connect(cb, &QCheckBox::toggled, this, [this, key](bool on) {
                if (on) features_.insert(key); else features_.remove(key);
                // Turning a feature ON pulls in the source patches it declares. ONE-WAY: turning
                // it off leaves them alone, because a patch can serve more than one purpose and
                // nothing here knows whether the user wanted it independently.
                // This is featurePatchGaps' warning turned into the action it was asking for.
                // The warning shipped because prose nobody reads let `sensors` reach a live A17
                // image with neither of its patches — dumpsys answering "No Sensors on the
                // device", which is also a textbook emulator signal (bd remora-4ei.53). A gap that
                // can be closed automatically should not be a modal before a multi-hour build.
                if (on) {
                    if (const Feature *f = findFeature(key))
                        for (const QString &pk : f->requiresPatches)
                            if (QCheckBox *pc = patchChecks_.value(pk)) pc->setChecked(true);
                }
                refreshPatchApplicability();  // features gate patch rows
                recompute();
            });
        }
        fv->addLayout(featureGrid_);
        // (The hint line that once explained the greying is gone with the greying itself — the
        // Active image card above now says what deploys and why, which is the fact it was for.)
        // ── Source patches, folded in under the features that pull them in ──────
        // Not its own group box any more: features and patches are one decision seen from
        // two heights — a feature is what you want, a patch is what delivering it costs in
        // the tree — and Feature::requiresPatches already ticks the second from the first.
        // ONE LIST. Features and optional patches were two grids with a rule and a paragraph of
        // grey between them; with the golden stack retired there are three optional entries left,
        // and the distinction they were separated on — "what I want" versus "what the tree needs"
        // — was never one a reader had to act on. Both are now cells in the same grid, packed by
        // repackFeatureGrid().
        auto makeRow = [&](const SourcePatch &p) -> QWidget * {
            auto *cb = new QCheckBox(p.label);  // project path lives in the tooltip
            cb->setToolTip(QStringLiteral("%1\n\n%2").arg(p.projectPath, p.note));
            patchChecks_.insert(p.key, cb);
            // A patch proven on some releases but not this one. Shown, never hidden, for the
            // reason above refreshPatchApplicability — but shown with the caveat visible, since
            // the list previously offered nine A17-only entries on A16 with no signal at all, one
            // of which (no_lineage_sepolicy) would REMOVE working Lineage sepolicy there.
            QLabel *badge = nullptr;  // (the "A17 only" mark went with the golden stack)
            // soong_gomemlimit is the one patch that carries a NUMBER rather than just a toggle —
            // the patch opens the channel, SOONG_GOMEMLIMIT decides what goes through it. Keep the
            // value next to the checkbox that enables it: a heap bound parked in a distant field
            // reads as unrelated, and it is inert whenever this box is unticked.
            QWidget *companion = nullptr;
            // soong_gomemlimit's heap spinbox used to ride here, beside the patch that opens the
            // channel. That patch is golden, so it no longer has a row — and the value is a build
            // resource limit, which is what the Build throttle row is. It moved there.
            // A bare checkbox is its own row; anything with a badge or a companion control needs a
            // wrapper, or hiding the checkbox alone would leave the extras orphaned on screen.
            if (!badge && !companion) {
                patchRows_.insert(p.key, cb);
                return cb;
            }
            auto *row = new QWidget();
            auto *rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->addWidget(cb);
            if (companion) {
                rl->addSpacing(8);
                rl->addWidget(new QLabel(QStringLiteral("heap")));
                rl->addWidget(companion);
            }
            if (badge) {
                rl->addSpacing(8);
                rl->addWidget(badge);
            }
            rl->addStretch(1);
            patchRows_.insert(p.key, row);
            return row;
        };
        for (const SourcePatch &p : sourcePatches()) {
            if (p.defaultOn) continue;  // golden: carried in config, never a widget
            auto *row = makeRow(p);
            row->setParent(featBox);  // owned by the box; the grid only positions it
            // The group goes in the TOOLTIP, not the label. Five entries across three groups would
            // be more section headers than rows, and an inline "· Graphics, media and codecs"
            // suffix pushed the card past the window width the whole page is sized to fit in.
            if (auto *cb = patchChecks_.value(p.key)) {
                for (const auto &g : patchGroups())
                    if (g.first == p.group)
                        cb->setToolTip(QStringLiteral("%1  ·  %2\n\n%3")
                                           .arg(p.projectPath, g.second, p.note));
            }
        }
        repackFeatureGrid();
        // The baseline set — Remora's proven patches, applied by default. Behind a disclosure
        // because it is long and rarely touched, but EDITABLE: it was briefly read-only, and a
        // list you can see but not act on just relocates the question. Two columns, grouped the
        // way the patch registry groups them.
        // Not called "golden stack" any more: the name described how the maintainers feel about
        // the set, not what it is or what unticking one does.
        {
            auto *body = new QWidget();
            auto *bv = new QVBoxLayout(body);
            bv->setContentsMargins(16, 0, 0, 0);
            bv->setSpacing(2);
            for (const auto &g : patchGroups()) {
                QVector<const SourcePatch *> members;
                for (const SourcePatch &sp : sourcePatches())
                    if (sp.defaultOn && sp.group == g.first) members << &sp;
                if (members.isEmpty()) continue;
                auto *head = new QLabel(g.second);
                QFont hf = head->font();
                hf.setBold(true);
                head->setFont(hf);
                bv->addWidget(head);
                auto *grid = new QGridLayout();
                grid->setHorizontalSpacing(16);
                grid->setVerticalSpacing(2);
                grid->setContentsMargins(0, 0, 0, 4);
                for (int n = 0; n < members.size(); ++n) {
                    const SourcePatch *sp = members[n];
                    auto *cb = new QCheckBox(sp->label);
                    cb->setToolTip(QStringLiteral("%1\n\n%2").arg(sp->projectPath, sp->note));
                    goldenChecks_.insert(sp->key, cb);
                    // Editing one re-runs the gate filter (a needs.patch dependency reads the
                    // enabled set) and re-titles the disclosure with the new count.
                    connect(cb, &QCheckBox::toggled, this, [this] {
                        refreshPatchApplicability();
                        refreshBaselineTitle();
                    });
                    grid->addWidget(cb, n / 2, n % 2);
                }
                bv->addLayout(grid);
            }
            goldenDisclosure_ = disclosure(fv, QStringLiteral("Baseline patches"), body);
        }
        // A needs.patch dependency reads the enabled set, so any tick can change what is gated —
        // re-run the whole filter, not just the group totals.
        for (const SourcePatch &p : sourcePatches())
            if (auto *cb = patchChecks_.value(p.key))
                connect(cb, &QCheckBox::toggled, this, [this] { refreshPatchApplicability(); });
        // Collapsed by default: the feature set is decided once per profile and then left alone,
        // and expanded it is the tallest thing on the page.
        // "&&", not "&": QToolButton reads a single ampersand as a mnemonic marker and swallows
        // it, titling the button "Build features _source patches" with the s underlined.
        disclosure(col, QStringLiteral("Build features && source patches"), featBox);
    }

    // ── Source, folded in from its own page ───────────────────────
    // Picking an image and building one were two sidebar rows that could not be reasoned about
    // apart: the Prebuilt-image combo above decides whether ANY of this is live, and the old
    // layout expressed that by dimming a sidebar row you had to navigate to in order to see what
    // it had done. Same page, no dimming, the cause directly above the effect.

    // ── Active image ──────────────────────────────────────────────
    // Which tag this profile actually deploys, and whether that is a pin or a resolution. The pin
    // was INVISIBLE before this card: image_tag= in remorarc greyed out the whole source half, and
    // the sentence that used to name it went with the prebuilt selector that set it (bd
    // remora-2ylt) — leaving a page of dead controls whose cause was stated nowhere in the UI, and
    // reversible only by editing remorarc with the GUI closed. A state that decides what deploys
    // has to be readable, and undoable, on the page it changes.
    // ONE COMBO, NOT A LABEL AND TWO BUTTONS. Pin and unpin are the same decision read in two
    // directions, so they are one control: the first entry is "no pin, resolve from the features"
    // and every other entry is a built image to pin. A label would state the cause without fixing
    // it; a Pin button beside Promote would put half the decision on a different card.
    {
        auto *form = card(col, QStringLiteral("Active image"));
        activeImageCombo_ = new QComboBox();
        activeImageCombo_->setToolTip(QStringLiteral(
            "Which image this profile deploys. 'from features' is the default — the feature list "
            "picks the tag and arch_variant appends the CPU-tuning suffix. Choosing a tag instead "
            "writes image_tag= to the profile and fixes it to that one image; the features then "
            "still decide what a source build produces and still drive the runtime features, but "
            "no longer choose what runs.\n\nNot the same as Promote, which retags an image to the "
            "active name so EVERY profile on that family follows it. Pinning moves this profile "
            "only."));
        connect(activeImageCombo_, &QComboBox::currentIndexChanged, this,
                &MainWindow::onActiveImageChanged);
        form->addRow(QStringLiteral("Deploys"), activeImageCombo_);
    }


    // Full width, like every card above it. This was a two-column split with the patch list on
    // the right; the patches folded into the features box, and a half-width card beside an empty
    // half read as a layout that had lost something.

    // ── Source build (full AOSP rebuild) ──────────────────────────
    // No Source host row: builds run on the machine running Remora. source_host= in remorarc is
    // still honoured (sourceHostStr reads the config) for the beefy-remote-builder setup, but it
    // is expert plumbing now, not a field — a remote machine you DEPLOY to is a Runner, and that
    // stays on the Runners page.
    {
        auto *form = card(col, QStringLiteral("Source build"));
        sourceKindCombo_ = new QComboBox();
        sourceKindCombo_->addItem(QStringLiteral("LineageOS base (do-build.sh)"),
                                  QStringLiteral("lineage"));
        sourceKindCombo_->addItem(QStringLiteral("AOSP base (envsetup + lunch + m)"),
                                  QStringLiteral("aosp"));
        form->addRow(QStringLiteral("Source kind"), sourceKindCombo_);
        connect(sourceKindCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::recompute);
        // An AOSP tree never syncs the LineageOS projects, so the nine entries that patch them
        // stop being choices the moment the kind changes.
        connect(sourceKindCombo_, &QComboBox::currentIndexChanged, this,
                [this] { refreshPatchApplicability(); });
        // No "Source repo" row: it offered two URLs per kind and the first was always the right
        // one, so the kind above already answers it — defaultSourceRepo() derives it for the CLI
        // and the GUI alike. A tree that needs a different manifest sets source_repo= in remorarc.
        sourceRefCombo_ = new QComboBox();
        sourceRefCombo_->setEditable(true);
        // The ref list and the patch list are both version-filtered views. They used to re-filter
        // on the version combo's currentIndexChanged; with no combo the version only moves when a
        // profile is loaded, so loadConfig drives both explicitly instead.
        auto *refRow = new QWidget();
        auto *rrl = new QHBoxLayout(refRow);
        rrl->setContentsMargins(0, 0, 0, 0);
        rrl->addWidget(sourceRefCombo_, 1);
        auto *pullBtn = new QPushButton(themeIcon({"cloud-download", "download", "git"}),
                                        QStringLiteral("Pull source…"));
        pullBtn->setToolTip(QStringLiteral(
            "Clone/checkout into the Source directory at the given ref. "
            "Lineage/AOSP kinds use 'repo init/sync'; a plain git URL is cloned."));
        connect(pullBtn, &QPushButton::clicked, this, &MainWindow::onPullSource);
        sourceButtons_ << pullBtn;
        rrl->addWidget(pullBtn);
        form->addRow(QStringLiteral("Source ref"), refRow);
        // No pin checkbox: pinning is on unless remorarc says source_pinned=false. Unticking it
        // synced the ref's tip instead of the manifest the patches were made against, so upstream
        // drift broke the apply — an expert act whose usual outcome is a failed multi-hour build,
        // which is the same reason the golden-stack patches sit behind a disclosure.
        auto *throttleRow = new QWidget();
        auto *thl = new QHBoxLayout(throttleRow);
        thl->setContentsMargins(0, 0, 0, 0);
        buildNiceSpin_ = new QSpinBox();
        buildNiceSpin_->setRange(0, 19);
        buildNiceSpin_->setValue(15);
        buildNiceSpin_->setToolTip(QStringLiteral(
            "CPU priority for the source build: 0 = compete with everything, 19 = maximum "
            "politeness. Default 15 keeps the desktop responsive."));
        buildJobsSpin_ = new QSpinBox();
        buildJobsSpin_->setRange(0, 128);
        buildJobsSpin_->setSpecialValueText(QStringLiteral("all cores"));
        buildJobsSpin_->setToolTip(QStringLiteral(
            "Parallel build jobs (ninja -j). 'all cores' uses everything; lower it to cap CPU "
            "and RAM pressure during long builds."));
        buildMemSpin_ = new QSpinBox();
        buildMemSpin_->setRange(0, 512);
        buildMemSpin_->setSuffix(QStringLiteral(" GiB"));
        buildMemSpin_->setSpecialValueText(QStringLiteral("no limit"));
        buildMemSpin_->setToolTip(QStringLiteral(
            "Optional hard RAM ceiling for the build container (docker --memory, swap disabled). "
            "'no limit' is the default: the container is still isolated in its own cgroup slice, "
            "which is what stops a big build throttle-deadlocking bluetoothd and journald — the "
            "ceiling is a separate, stricter thing.\n\nSet a value only if you want the build "
            "KILLED rather than allowed to grow into swap. An automatic ceiling was tried and "
            "removed: it OOM-killed four builds that were otherwise progressing, because siso "
            "alone holds ~20 GiB before any compilation starts."));
        thl->addWidget(new QLabel(QStringLiteral("niceness")));
        thl->addWidget(buildNiceSpin_);
        thl->addSpacing(12);
        thl->addWidget(new QLabel(QStringLiteral("jobs")));
        thl->addWidget(buildJobsSpin_);
        thl->addSpacing(12);
        thl->addWidget(new QLabel(QStringLiteral("memory cap")));
        thl->addWidget(buildMemSpin_);
        thl->addSpacing(12);
        // Relocated from the golden-stack patch list: every number that bounds what the build may
        // consume now sits on one row. Inert unless the soong_gomemlimit patch is applied, which
        // is what its tooltip and the enabled state say.
        soongMemSpin_ = new QSpinBox();
        soongMemSpin_->setRange(0, 512);
        soongMemSpin_->setSuffix(QStringLiteral(" GiB"));
        soongMemSpin_->setSpecialValueText(QStringLiteral("auto"));
        soongMemSpin_->setToolTip(QStringLiteral(
            "Go heap ceiling for soong_build (SOONG_GOMEMLIMIT). 'auto' uses 65% of the container "
            "memory cap.\n\nIt must sit ABOVE the live set or the GC thrashes against a flat "
            "heap, and BELOW twice the live set or it never binds — with the ~19.4 GiB live set "
            "measured here that window is roughly 22-35 GiB.\n\nOnly has an effect while the "
            "soong_gomemlimit patch is applied (it is golden, so on by default): soong_build is "
            "launched via 'env -i' and that patch is what forwards the value into its "
            "environment."));
        thl->addWidget(new QLabel(QStringLiteral("soong heap")));
        thl->addWidget(soongMemSpin_);
        thl->addStretch(1);
        form->addRow(QStringLiteral("Build throttle"), throttleRow);
        archVariantCombo_ = new QComboBox();
        archVariantCombo_->setEditable(true);
        archVariantCombo_->addItem(QStringLiteral("(generic x86-64 — runs on any host)"),
                                   QString());
        for (const QString &v : knownArchVariants()) archVariantCombo_->addItem(v, v);
        archVariantCombo_->setToolTip(QStringLiteral(
            "TARGET_ARCH_VARIANT: compile every in-tree native library for a chosen CPU class — "
            "Skia, framework native and codecs all gain real vector codegen. The built image is "
            "tagged with the variant (…-broadwell-src), so tuned builds coexist with the portable "
            "baseline and can be published side by side.\n\n"
            "broadwell = x86-64-v3 (AVX2/FMA/BMI2): Intel 2014+ and every AMD Zen. skylake adds "
            "little beyond it. alderlake matches 12th-gen Intel exactly but needs VAES/GFNI — "
            "pre-Zen4 AMD hosts SIGILL, so pick it only for a build pinned to such a machine. "
            "silvermont/goldmont/tremont target Atom-class boxes. The list is editable: any "
            "variant this tree's soong knows is accepted, and an unknown name fails fast at "
            "config parse, not mid-build. A tuned image refuses nothing at install time — it "
            "simply crashes on a host below its level, so keep the generic build around for "
            "anything you might deploy elsewhere."));
        form->addRow(QStringLiteral("CPU tuning"), archVariantCombo_);
        // The ref list is a view of the repo, and the repo now follows the kind — so the kind is
        // what invalidates it. This used to hang off the repo combo's own edits.
        connect(sourceKindCombo_, &QComboBox::currentIndexChanged, this,
                &MainWindow::onRefreshSourceRefs);
        sourceTreeEdit_ = addBrowseDir(form, QStringLiteral("Source directory"),
                                       QStringLiteral("~/remora-android"));
        sourceTreeEdit_->setToolTip(QStringLiteral(
            "The AOSP/Lineage checkout used for full source rebuilds — the destination of Pull "
            "source. One directory per Android version/branch is strongly recommended: re-syncing "
            "one tree to another version invalidates the whole build output anyway."));
        connect(sourceTreeEdit_, &QLineEdit::editingFinished, this, &MainWindow::recompute);
        connect(sourceTreeEdit_, &QLineEdit::editingFinished, this, &MainWindow::onRefreshArchives);
        connect(sourceTreeEdit_, &QLineEdit::editingFinished, this,
                &MainWindow::refreshSourceState);
        outputDirEdit_ = addBrowseDir(form, QStringLiteral("Output dir"),
                                      QStringLiteral("~/remora-images"));
        outputDirEdit_->setToolTip(QStringLiteral(
            "Where docker-save archives land — what the image library below scans."));
        connect(outputDirEdit_, &QLineEdit::editingFinished, this, &MainWindow::onRefreshArchives);
        // The build-output size and its Clean button live in the image library card's
        // maintenance bar below, beside the docker-context size and prunable-layers counters.
        // Reclaiming 100+ GB is storage work, and that bar is where the other two live.
        // No source-status line: "source (manifest): <ref> @ <sha> · 0↑ 0↓ · build up-to-date"
        // was grey too. refreshSourceStatus() is gone with it; the tree's real state is what git
        // in the source directory says.
        // The build action, moved down from the features box: everything it consumes — the tree,
        // the ref, the throttle — is configured in THIS card, and it was previously the
        // one control in the box above that acted on none of that box's contents.
        {
            auto *buildSrcBtn = new QPushButton(themeIcon({"run-build", "system-run"}),
                                                QStringLiteral("Build from source…"));
            buildSrcBtn->setProperty("primary", true);
            buildSrcBtn->setToolTip(QStringLiteral(
                "Apply the ticked patches, then run do-build.sh — the full reproducible pipeline "
                "(multi-hour). Pull the tree first."));
            connect(buildSrcBtn, &QPushButton::clicked, this, &MainWindow::onBuildFromSource);
            sourceButtons_ << buildSrcBtn;
            form->addRow(QString(), buildSrcBtn);
        }
    }

    // No Build locations card at all any more. The docker-context dir and the docker host both
    // lost their widgets — everything builds and runs on the machine running Remora, and the
    // remaining path (Output dir) moved into the Source build card beside the build that fills
    // it. build_dir= and build_host= in remorarc are still honoured (buildDirPath/buildHostStr
    // read the config); a remote machine you deploy to is a Runner, set on the Runners page.

    // No Advanced disclosure here any more — ro overrides moved to the Device page, which is where
    // the rest of the boot args live. Nothing image-shaped was left to keep it company.

    // ── Built images — the image library, folded in from its own page ────────────
    // The Library page held exactly this one card, and a one-card page is a tab's worth of
    // navigation for zero grouping (user direction, bd remora-2ylt): the images live where the
    // build that produces them and the sources that offer them are configured. The old Build
    // locations card is gone: its output dir lives in the Source build card above, and the
    // docker-context/host fields retired to remorarc keys.
    {
        auto *box = new QGroupBox(QStringLiteral("Built images"));
        auto *v = new QVBoxLayout(box);
        v->addWidget(hintLabel(QStringLiteral(
            "docker-save archives in the output dir (set above) + local docker "
            "images. They also appear in the image selector. These get large — rename to keep, "
            "delete to reclaim.")));
        archivesTree_ = new QTreeWidget();
        archivesTree_->setColumnCount(4);
        archivesTree_->setHeaderLabels({QStringLiteral("archive"), QStringLiteral("image tag"),
                                        QStringLiteral("built"), QStringLiteral("size")});
        archivesTree_->setRootIsDecorated(false);
        archivesTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        archivesTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        archivesTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        archivesTree_->header()->setStretchLastSection(false);
        archivesTree_->sortByColumn(2, Qt::DescendingOrder);  // newest build first
        archivesTree_->setMinimumHeight(120);
        v->addWidget(archivesTree_);
        auto *btns = new QWidget();
        auto *bl = new QHBoxLayout(btns);
        bl->setContentsMargins(0, 0, 0, 0);
        auto *refreshBtn = new QPushButton(themeIcon({"view-refresh"}), QStringLiteral("Refresh"));
        auto *renameBtn = new QPushButton(themeIcon({"edit-rename", "document-edit"}),
                                          QStringLiteral("Rename"));
        auto *deleteBtn = new QPushButton(themeIcon({"edit-delete"}), QStringLiteral("Delete"));
        auto *promoteBtn = new QPushButton(themeIcon({"go-up", "arrow-up", "vcs-update-cvs-cervisia"}),
                                           QStringLiteral("Promote to active"));
        promoteBtn->setToolTip(QStringLiteral(
            "Retag a fresh -src source build to its active tag (drops the -src suffix) so the next "
            "Connect/Up deploys it — the reproducible promote step. The old image is freed when the "
            "container recreates onto the new one; use Prune to reclaim its layers."));
        auto *pushBtn = new QPushButton(themeIcon({"cloud-upload", "go-up", "network-server"}),
                                        QStringLiteral("Push to host…"));
        pushBtn->setToolTip(QStringLiteral(
            "Copy the selected docker image to another host (docker save | ssh docker load) — "
            "e.g. ship a locally source-built image to the deploy host. A -src build offers to "
            "promote itself to the active tag on arrival, so the next Connect/Up deploys it."));
        auto *fetchBtn = new QPushButton(themeIcon({"cloud-download", "go-down", "download"}),
                                         QStringLiteral("Fetch"));
        fetchBtn->setToolTip(QStringLiteral(
            "Fetch the selected source row's image onto the build host (bd remora-pht9): the "
            "archive streams straight into docker load — curl for a published-image mirror, ssh "
            "for a remote directory, cat for a local one — and the row flips to local on the next "
            "refresh. Nothing is written to the archive dir."));
        auto *pruneBtn = new QPushButton(themeIcon({"edit-clear-history", "trash-empty"}),
                                         QStringLiteral("Prune"));
        pruneBtn->setToolTip(QStringLiteral(
            "Reclaim orphaned docker layers left by deleted/rebuilt images (rmi only untags; "
            "the layers linger until pruned). The count/size to its right is what pruning frees."));
        prunableLabel_ = new QLabel();  // "N orphaned · ~X" — populated by onRefreshArchives
        prunableLabel_->setStyleSheet("color:#777;");
        auto *cleanBtn = new QPushButton(themeIcon({"edit-clear-all", "edit-clear"}),
                                         QStringLiteral("Clean docker context…"));
        buildDirSizeLabel_ = new QLabel();
        buildDirSizeLabel_->setStyleSheet("color:#777;");
        connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshArchives);
        connect(renameBtn, &QPushButton::clicked, this, &MainWindow::onRenameArchive);
        connect(deleteBtn, &QPushButton::clicked, this, &MainWindow::onDeleteArchive);
        connect(promoteBtn, &QPushButton::clicked, this, &MainWindow::onPromoteImage);
        connect(pushBtn, &QPushButton::clicked, this, &MainWindow::onPushImage);
        connect(fetchBtn, &QPushButton::clicked, this, &MainWindow::onFetchSourceImage);
        connect(pruneBtn, &QPushButton::clicked, this, &MainWindow::onPruneImages);
        connect(cleanBtn, &QPushButton::clicked, this, &MainWindow::onCleanBuildDir);
        sourceButtons_ << cleanBtn;  // a build stages into this dir — not while it is being deleted
        bl->addWidget(refreshBtn);
        bl->addWidget(renameBtn);
        bl->addWidget(deleteBtn);
        bl->addWidget(promoteBtn);
        bl->addWidget(pushBtn);
        bl->addWidget(fetchBtn);
        bl->addWidget(pruneBtn);
        bl->addWidget(prunableLabel_);
        bl->addStretch(1);
        v->addWidget(btns);

        // A SECOND row for the reclaim-disk controls, not the button bar above: six archive actions
        // plus three size readouts and their two Clean buttons overran the page width and scrolled
        // sideways. They are also a different kind of thing — the bar acts on the selected archive,
        // this row acts on what the build pipeline left lying around.
        auto *diskRow = new QWidget();
        auto *dl = new QHBoxLayout(diskRow);
        dl->setContentsMargins(0, 2, 0, 0);
        dl->setSpacing(4);
        // Relocated from the Source build card: the compiled AOSP tree is often 100+ GB and is by
        // far the biggest thing Remora leaves on disk, so it belongs with the other size readouts
        // rather than at the bottom of a build-configuration form.
        sourceOutSizeLabel_ = new QLabel(QStringLiteral("build output: —"));
        sourceOutSizeLabel_->setStyleSheet("color:#777;");
        auto *cleanOutBtn = new QPushButton(themeIcon({"edit-clear-all", "edit-clear"}),
                                            QStringLiteral("Clean build output…"));
        cleanOutBtn->setToolTip(QStringLiteral(
            "Delete <source dir>/out (the compiled AOSP output — often 100+ GB). The source tree "
            "itself is kept; the next source build recompiles from scratch."));
        connect(cleanOutBtn, &QPushButton::clicked, this, &MainWindow::onCleanSourceOutput);
        sourceButtons_ << cleanOutBtn;
        dl->addWidget(sourceOutSizeLabel_);
        dl->addWidget(cleanOutBtn);
        dl->addSpacing(12);
        dl->addWidget(buildDirSizeLabel_);
        dl->addWidget(cleanBtn);
        dl->addStretch(1);
        v->addWidget(diskRow);
        col->addWidget(box, 1);  // the archives tree gets the spare page height
    }

    col->addStretch(1);
    col = newPage(QStringLiteral("Settings"), {"configure", "preferences-system"});

    // ── Image sources ─────────────────────────────────────────────
    {
        auto *box = new QGroupBox(QStringLiteral("Image sources"));
        auto *v = new QVBoxLayout(box);
        v->addWidget(hintLabel(QStringLiteral(
            "Where extra images come from. A source is a name plus a location — an https:// "
            "mirror of published images, an ssh user@host:/dir of docker-save archives, or a "
            "local directory — and/or pinned image tags. Every enabled location's images appear "
            "in the image library on the Image page, marked local or needing a fetch. Self-built "
            "images and the build host's docker images always appear there; they need no source "
            "entry.")));
        sourcesTree_ = new QTreeWidget();
        sourcesTree_->setColumnCount(2);
        sourcesTree_->setHeaderLabels({QStringLiteral("source"), QStringLiteral("images")});
        sourcesTree_->setRootIsDecorated(false);
        sourcesTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        sourcesTree_->header()->setStretchLastSection(true);
        sourcesTree_->setMinimumHeight(140);
        rebuildSourcesTree();
        connect(sourcesTree_, &QTreeWidget::itemChanged, this, &MainWindow::onSourceToggled);
        v->addWidget(sourcesTree_);
        auto *btns = new QWidget();
        auto *bl = new QHBoxLayout(btns);
        bl->setContentsMargins(0, 0, 0, 0);
        auto *addBtn = new QPushButton(themeIcon({"list-add"}), QStringLiteral("Add source"));
        auto *rmBtn = new QPushButton(themeIcon({"list-remove", "edit-delete"}),
                                      QStringLiteral("Remove source"));
        connect(addBtn, &QPushButton::clicked, this, &MainWindow::onAddSource);
        connect(rmBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSource);
        bl->addWidget(addBtn);
        bl->addWidget(rmBtn);
        bl->addStretch(1);
        v->addWidget(btns);
        col->addWidget(box);
    }


    // ── Profiles — the whole per-profile surface in one table ─────
    // The Desktop-integration card and the Google-Play-certification card (bd remora-4ei.83) are
    // columns and bar actions of this list now, Apps-page style. They were always per-profile
    // state, but as cards they could only see the ACTIVE profile — flipping a switch for another
    // profile meant loading it first, and its certification state was invisible until you did.
    {
        auto *box = new QGroupBox(QStringLiteral("Profiles"));
        auto *v = new QVBoxLayout(box);
        v->addWidget(hintLabel(QStringLiteral(
            "Each profile is an independent config (backend, image, hosts…). The sidebar's "
            "switcher picks the active one; new profiles are named for the selected OS + Android "
            "version. The switches apply per row — no need to load a profile to change them.")));
        profilesTree_ = new QTreeWidget();
        profilesTree_->setColumnCount(5);
        profilesTree_->setHeaderLabels({QStringLiteral("profile"), QStringLiteral("desktop menu"),
                                        QStringLiteral("update ARM apps"),
                                        QStringLiteral("ARM guard"),
                                        QStringLiteral("Play certified")});
        profilesTree_->setRootIsDecorated(false);  // a flat list, like the Apps entries table
        profilesTree_->setUniformRowHeights(true);  // cell widgets must not vary row height
        // The header carries the explanations the two cards' checkboxes used to hold.
        auto *head = profilesTree_->headerItem();
        head->setToolTip(1, QStringLiteral(
            "On: a launcher-menu folder with one entry per installed Android app, kept fresh on "
            "every Connect and whenever the Apps page is opened. Off: the folder is removed from "
            "the desktop menu and not regenerated.\n\nTurning a non-active profile ON needs its "
            "device, so it takes effect at that profile's next Connect; OFF acts immediately."));
        head->setToolTip(2, QStringLiteral(
            "After a successful Connect, re-fetch the ARM-injected apps from Google Play (the "
            "Apps page's Update-apps sweep: keeps app data; other apps are left to Play's own "
            "updater), at most as often as selected."));
        head->setToolTip(3, QStringLiteral(
            "Keep ARM-only apps working. Some apps have no working x86_64 build (TikTok, and "
            "Flutter apps whose x86_64 split omits libflutter.so). Remora installs those as "
            "arm64, where they run under the native bridge — but Play re-delivers the broken "
            "x86_64 build at its next update.\n\nWith this on, Remora checks after each Connect "
            "and quietly re-fetches the arm64 build of any such app that got overwritten (marked "
            "'arm64 (auto-repair)' on the Apps page). On by default: unlike the opt-in update "
            "sweep it normally costs one docker exec that finds nothing."));
        head->setToolTip(4, QStringLiteral(
            "Google keeps a list of certified devices, keyed on the device's GSF android_id; an "
            "id that is not on it makes GMS refuse Google sign-in, so a Play Store login failure "
            "and an uncertified device are one problem, not two. Every /data reset mints a "
            "brand-new android_id, which silently de-certifies the instance — the usual reason a "
            "device that worked yesterday stops today.\n\nCheck reads the verdict off the "
            "running device; the fix-it actions sit in the bar below. CLI equivalent: remora "
            "certify [--recheck]."));
        // profile names vary most; the switch columns size to their headers
        profilesTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        profilesTree_->header()->setStretchLastSection(false);
        for (int c = 1; c <= 4; ++c)
            profilesTree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        profilesTree_->setMinimumHeight(140);
        rebuildProfilesList();
        connect(profilesTree_, &QTreeWidget::itemDoubleClicked, this,
                [this] { onLoadProfile(); });
        v->addWidget(profilesTree_);
        // Flat, icon-led action bar, like the Apps page: profile ops left, the certification
        // fix-it flow right. Certification is an account repair, not a daily control — and every
        // one of its buttons acts on the SELECTED row (falling back to the active profile), so a
        // fleet's verdicts are reachable without a single profile switch.
        auto *btns = new QWidget();
        auto *bl = new QHBoxLayout(btns);
        bl->setContentsMargins(0, 2, 0, 0);
        bl->setSpacing(4);
        const auto barBtn = [](const QIcon &icon, const QString &text, const QString &tip) {
            auto *b = new QToolButton();
            b->setIcon(icon);
            b->setText(text);
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setAutoRaise(true);
            b->setToolTip(tip);
            return b;
        };
        auto *loadBtn = barBtn(themeIcon({"document-open"}), QStringLiteral("Load"),
                               QStringLiteral("Make the selected profile the active one."));
        auto *addBtn = barBtn(themeIcon({"list-add"}), QStringLiteral("Add"),
                              QStringLiteral("Create a new profile."));
        auto *renameBtn = barBtn(themeIcon({"edit-rename", "document-edit"}),
                                 QStringLiteral("Rename"),
                                 QStringLiteral("Rename the selected profile."));
        auto *delBtn = barBtn(themeIcon({"edit-delete"}), QStringLiteral("Delete"),
                              QStringLiteral("Delete the selected profile (built images and "
                                             "containers are untouched)."));
        connect(loadBtn, &QToolButton::clicked, this, &MainWindow::onLoadProfile);
        connect(addBtn, &QToolButton::clicked, this, &MainWindow::onNewProfile);
        connect(renameBtn, &QToolButton::clicked, this, &MainWindow::onRenameProfile);
        connect(delBtn, &QToolButton::clicked, this, &MainWindow::onDeleteProfile);
        auto *checkBtn = barBtn(themeIcon({"view-refresh"}), QStringLiteral("Check"),
            QStringLiteral(
                "Read the selected profile's Play-certification state off its running device. "
                "Needs the container up — a stopped device reads as unknown, not as "
                "uncertified."));
        connect(checkBtn, &QToolButton::clicked, this, [this] { refreshCertification(false); });
        auto *recheckBtn = barBtn(themeIcon({"view-refresh", "system-run"}),
                                  QStringLiteral("Re-check"),
            QStringLiteral(
                "Force the GMS checkin that re-evaluates a fresh registration, then re-read the "
                "verdict — without it the verdict only refreshes on GMS's own ~11h schedule or "
                "at the next container restart. Takes a few seconds. Registration can need a "
                "few minutes to propagate — if it still reads uncertified, wait and try again, "
                "or Connect to restart the device."));
        connect(recheckBtn, &QToolButton::clicked, this, [this] { refreshCertification(true); });
        auto *registerBtn = barBtn(themeIcon({"internet-web-browser", "web-browser"}),
                                   QStringLiteral("Register…"),
            QStringLiteral(
                "Open Google's uncertified-device form with the selected profile's android_id on "
                "the clipboard ready to paste. The page needs your Google login, so this step "
                "happens in the browser, not in Remora. Run Check first — that is what reads "
                "the id."));
        connect(registerBtn, &QToolButton::clicked, this, [this] {
            const QString id = profileCert_.value(selectedProfileName()).androidId;
            if (!id.isEmpty()) QApplication::clipboard()->setText(id);
            QDesktopServices::openUrl(QUrl(playCertRegistrationUrl()));
            statusBar()->showMessage(
                id.isEmpty()
                    ? QStringLiteral("opened Google's registration page — Check reads the device "
                                     "id it asks for")
                    : QStringLiteral("android_id copied — paste it into the page, then Re-check"),
                8000);
        });
        auto *restoreBtn = barBtn(themeIcon({"document-revert", "edit-undo"}),
                                  QStringLiteral("Restore ID"),
            QStringLiteral(
                "Put the selected profile's SAVED checkin identity back on its device.\n\n"
                "A /data rebuild makes the device mint a brand-new identity, and the old one's "
                "registration does not carry over — that is what silently de-certifies an "
                "instance. The identity is a PAIR — the android_id Google registers plus the "
                "token that authenticates it — and Remora keeps a copy every time it reads one, "
                "so if the saved one was already registered this fixes the device without "
                "another trip to Google's form.\n\nIt force-stops Google Play services, writes "
                "the identity back, and drops the caches that would otherwise re-seed the new "
                "one."));
        connect(restoreBtn, &QToolButton::clicked, this,
                [this] { refreshCertification(/*recheck=*/false, /*restore=*/true); });
        auto *copyBtn = barBtn(themeIcon({"edit-copy"}), QStringLiteral("Copy ID"),
            QStringLiteral(
                "Copy the selected profile's GSF android_id — the 19-digit number Google's form "
                "registers, which has to arrive there EXACTLY. Empty until a Check has read it.\n"
                "NB this is NOT `settings get secure android_id`, which is a per-app SSAID and "
                "registers nothing."));
        connect(copyBtn, &QToolButton::clicked, this, [this] {
            const QString id = profileCert_.value(selectedProfileName()).androidId;
            if (id.isEmpty()) {
                statusBar()->showMessage(
                    QStringLiteral("no id read yet — Check reads it off the running device"),
                    6000);
                return;
            }
            QApplication::clipboard()->setText(id);
            statusBar()->showMessage(QStringLiteral("android_id copied to the clipboard"), 4000);
        });
        bl->addWidget(loadBtn);
        bl->addWidget(addBtn);
        bl->addWidget(renameBtn);
        bl->addWidget(delBtn);
        bl->addSpacing(10);
        bl->addWidget(checkBtn);
        bl->addWidget(recheckBtn);
        bl->addWidget(registerBtn);
        bl->addWidget(restoreBtn);
        bl->addWidget(copyBtn);
        bl->addStretch(1);
        v->addWidget(btns);
        col->addWidget(box);
    }

    // (Desktop integration is columns of the Profiles table above — desktop menu, ARM-app
    // updates and the ARM guard are per-row switches now. The entries themselves still live on
    // the Apps page.)

    // (Google Play certification is the Profiles table's last column + the bar's Check /
    // Re-check / Register… / Restore ID / Copy ID actions. The saved-identity answer — "would a
    // /data rebuild cost a re-registration, or just a restore?" — rides the verdict tooltip.)

    col->addStretch(1);
    setSidebarCollapsed(false);

    applyAccentStyle();
    sidebar_->setCurrentRow(0);  // Runners is the first page (no group headers)

    // ── No page scrolls sideways at the default window width ──────
    // A QComboBox's MINIMUM width is its widest item (the default AdjustToContentsOnFirstShow), so
    // one long entry — "macvlan (container gets its own LAN IP)", a keyboard's product name — set
    // the floor of its whole column, and two such columns plus a label gutter each came to 887 px
    // against the 844 px the default resize(1060) leaves a page: Device and Viewer both wore a
    // horizontal scrollbar (measured). The PREFERRED width is left alone — a combo in a
    // row with slack still shows its full text, and under AllNonFixedFieldsGrow it fills the row
    // regardless — only the floor moves: an explicit minimum is what the layout consults instead
    // of minimumSizeHint (qSmartMinSize), so a squeezed combo clips its text rather than pushing
    // the page wider. Fixed-width combos (setFieldChars) already carry a minimum and are skipped,
    // as is anything a page sized by hand.
    for (QComboBox *cb : pages_->findChildren<QComboBox *>())
        if (cb->minimumWidth() == 0)
            cb->setMinimumWidth(
                cb->fontMetrics().horizontalAdvance(QString(12, QLatin1Char('n'))) + 30);

    // ── Status bar: busy indicator + the log-pane toggle, reachable from every page ──
    // A self-driven glyph spinner, not a QProgressBar: with an app stylesheet active Qt swaps
    // in QStyleSheetStyle, whose indeterminate progress bars simply don't animate — the busy
    // bar sat frozen. A QTimer flipping braille frames cannot be broken by any style.
    busySpinner_ = new QLabel();
    busySpinner_->setObjectName(QStringLiteral("busySpinner"));
    busySpinner_->hide();
    spinnerTimer_ = new QTimer(this);
    spinnerTimer_->setInterval(90);
    connect(spinnerTimer_, &QTimer::timeout, this, [this] {
        static const QString frames = QStringLiteral("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏");
        spinnerFrame_ = (spinnerFrame_ + 1) % frames.size();
        busySpinner_->setText(frames.at(spinnerFrame_));
    });
    // Source state, in the bar rather than on the Image page: it is a fact about the checkout that
    // matters while you are anywhere in the app (about to Connect, about to build), and the page
    // it used to sit on has no grey status lines left. Terse by design — the tooltip carries the
    // branch, sha and counts. Hidden entirely when no source tree is configured.
    sourceStateLabel_ = new QLabel();
    sourceStateLabel_->hide();
    statusBar()->addPermanentWidget(sourceStateLabel_);
    statusBar()->addPermanentWidget(busySpinner_);
    auto *logBtn = new QToolButton();
    logBtn->setDefaultAction(logAction_);
    logBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);  // the tooltip carries the words
    logBtn->setAutoRaise(true);
    statusBar()->addPermanentWidget(logBtn);
    // No permanent instance/path message: the profile is visible in the sidebar switcher and
    // the remorarc path on the Settings page. The bar still carries transient status messages.
}


// Re-label each Navigation row's keyboard chord for the modifier now selected. The LETTER is not
// ours: the mirror hard-codes MOD+b/h/s/n, so the chord is the only honest thing to show — a row that
// let you type a key here would promise a binding it has no option for.
void MainWindow::syncNavKeyHints() {
    if (!shortcutModCombo_ || navKeyHints_.size() != kNavActions.size()) return;
    const QString value = shortcutModCombo_->currentData().toString();
    QString prefix;
    for (const ShortcutMod &m : kShortcutMods)
        if (value == QLatin1String(m.value)) { prefix = QLatin1String(m.chordPrefix); break; }
    // A hand-written shortcut_mod= this list has no row for: name its first alternative rather
    // than showing a chord that is wrong, since only the first is guaranteed to be one key.
    if (prefix.isEmpty() && !value.isEmpty()) prefix = value.section(QLatin1Char(','), 0, 0);
    for (int i = 0; i < kNavActions.size(); ++i)
        navKeyHints_.at(i)->setText(
            QStringLiteral("or %1+%2").arg(prefix).arg(QChar(kNavActions.at(i).key)));
}

// Fill the identity picker from the host's own /proc/bus/input/devices. Synthetic uinput devices
// (Sunshine's passthrough, our own persist-kbd) are listed too but flagged, because impersonating
// one is almost never what is wanted and silently hiding them would be confusing on a host where
// they are all that shows up.
void MainWindow::populateKbdIdentities() {
    if (!kbdIdentityCombo_) return;
    kbdIdentityCombo_->clear();
    kbdIdentityCombo_->addItem(QStringLiteral("Auto — first real keyboard"), QString());
    kbdIdentityCombo_->addItem(QStringLiteral("Generic — no Gboard toolbar"),
                               QStringLiteral("none"));
    QFile f(QStringLiteral("/proc/bus/input/devices"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QList<KbdIdentity> ks = parseInputDevicesKeyboards(QString::fromUtf8(f.readAll()));
    for (const KbdIdentity &k : ks) {
        const bool synthetic = k.phys.isEmpty()
                               || k.sysfs.startsWith(QLatin1String("/devices/virtual/input"));
        kbdIdentityCombo_->addItem(
            QStringLiteral("%1  (%2:%3)%4")
                .arg(k.name)
                .arg(k.vendor, 4, 16, QLatin1Char('0'))
                .arg(k.product, 4, 16, QLatin1Char('0'))
                .arg(synthetic ? QStringLiteral("  — virtual") : QString()),
            k.name);
    }
}

// Put the boundaries back into a legacy space-joined shared_inputs value. Device names contain
// spaces, so the old format ran them together ("Logitech USB Receiver Mouse Keychron K8 Pro
// Keyboard") and the string alone cannot say where one name ends; the devices the host reports
// right now can. Longest name first, so "Logitech USB Receiver Mouse" wins over the prefix
// "Logitech USB Receiver". Only a complete tiling by exact names is accepted — a partial match
// would mean guessing, and a wrong guess silently shares a device the operator did not pick.
static std::optional<QStringList> splitJoinedNames(const QString &joined, const QStringList &known) {
    if (joined.isEmpty()) return std::nullopt;
    QStringList byLength = known;
    std::sort(byLength.begin(), byLength.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    QStringList out;
    for (int pos = 0; pos < joined.size();) {
        const QStringView rest = QStringView(joined).mid(pos);
        auto hit = std::find_if(byLength.cbegin(), byLength.cend(), [&](const QString &n) {
            return !n.isEmpty() && rest.startsWith(n);
        });
        if (hit == byLength.cend()) return std::nullopt;  // not a clean tiling — leave it alone
        out << *hit;
        pos += hit->size();
        while (pos < joined.size() && joined.at(pos) == QLatin1Char(' ')) ++pos;
    }
    return out;
}

// Every host input device, ticked if shared. Shows the bus so USB and Bluetooth are distinguishable
// at a glance — two controllers of the same model on different transports are otherwise identical
// in this list, and the bus is what Android ends up seeing (bd remora-4ei.36).
void MainWindow::populateSharedInputs() {
    if (!sharedInputsList_) return;
    sharedInputsList_->clear();
    QFile f(QStringLiteral("/proc/bus/input/devices"));
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const KbdIdentity &k : parseInputDevices(QString::fromUtf8(f.readAll()))) {
        // The container's own synthetic devices are not shareable in any meaningful sense:
        // sharing a device that exists to feed the container would be a loop.
        if (k.phys.isEmpty() || k.sysfs.startsWith(QLatin1String("/devices/virtual/input")))
            continue;
        // Only things a person would actually share: a keyboard, a pointer, or something with
        // axes. Power/Sleep buttons, consumer-control endpoints and PC speakers all present as
        // input devices with a real phys, and listing them is pure noise.
        if (!k.alphabetic && !k.hasRel && !k.hasAbs) continue;
        const QString bus = k.bus == 0x5    ? QStringLiteral("Bluetooth")
                            : k.bus == 0x3  ? QStringLiteral("USB")
                                            : QStringLiteral("bus %1").arg(k.bus, 4, 16,
                                                                           QLatin1Char('0'));
        auto *item = new QListWidgetItem(QStringLiteral("%1  — %2").arg(k.name, bus));
        item->setData(Qt::UserRole, k.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        sharedInputsList_->addItem(item);
    }
}

void MainWindow::loadConfig() {
    // Programmatic widget population must not trip autosave (which would re-persist mid-load, and
    // on a profile switch could write half-loaded state back over the profile being loaded).
    suppressAutosave_ = true;
    dataDirEdit_->setText(tildify(optStr(cfg_.backend.dataDir)));
    hostAdbPortEdit_->setText(optInt(cfg_.backend.hostAdbPort));
    sourceTreeEdit_->setText(optStr(cfg_.image.sourceTree));
    sourceKindCombo_->setCurrentIndex(qMax(
        0, sourceKindCombo_->findData(cfg_.image.sourceKind.value_or(QStringLiteral("lineage")))));
    {
        const QStringList on =
            cfg_.image.sourcePatches.isEmpty()
                ? defaultSourcePatchSet(cfg_.image.androidVersion.value_or(16))
                : cfg_.image.sourcePatches;
        // Split the stored set in two. The optional tier drives checkboxes; the golden tier has
        // no widgets at all, so it is parked in goldenSelection_ and written back verbatim. Doing
        // this by p.defaultOn — the registry's own flag — rather than by "did we build a row for
        // it" keeps the split independent of what gating happened to hide.
        // Seed the baseline checkboxes, and keep a copy of anything stored that has no widget at
        // all (an entry from a future registry, say) so saving cannot drop what it cannot draw.
        goldenSelection_.clear();
        for (const QString &k : on)
            if (!goldenChecks_.contains(k) && !patchChecks_.contains(k)) goldenSelection_ << k;
        for (auto it = goldenChecks_.constBegin(); it != goldenChecks_.constEnd(); ++it) {
            QSignalBlocker b2(it.value());
            it.value()->setChecked(on.contains(it.key()));
        }
        refreshBaselineTitle();
        for (auto it = patchChecks_.constBegin(); it != patchChecks_.constEnd(); ++it) {
            QSignalBlocker b(it.value());
            it.value()->setChecked(on.contains(it.key()));
        }
        // and re-filter for THIS profile's version — switching profiles moves the version without
        // touching the combo's index, so the currentIndexChanged hook alone would miss it.
        refreshPatchApplicability();
        populateSourceRefCombo();  // the other version-filtered view — same reason
    }
    sourceRefCombo_->setCurrentText(optStr(cfg_.image.sourceRef));
    outputDirEdit_->setText(optStr(cfg_.image.outputDir));
    widthEdit_->setText(optInt(cfg_.display.width));
    heightEdit_->setText(optInt(cfg_.display.height));
    dpiEdit_->setText(optInt(cfg_.display.dpi));
    fpsCombo_->setCurrentText(optInt(cfg_.display.fps));
    autoSleepEdit_->setText(optInt(cfg_.advanced.autoSleepMinutes));
    // Unset resolves to Guest (Resolver.cpp), so an unset profile IS software-rendering — the box
    // has to read that way rather than showing an unticked promise of a GPU nobody selected.
    if (gpuDriverCombo_) {
        // SwiftShader outranks every other reading: gpu_mode not being host means nothing on this
        // host renders it, so which card the profile names is moot until that changes.
        // Otherwise: Venus forced ON is the NVIDIA entry; a named driver is that driver; anything
        // else is auto. The one state the list cannot say is "Venus off with no GPU chosen", so
        // the old tri-state is revealed for exactly that profile rather than silently rewritten.
        const bool software = cfg_.gpu.mode.value_or(GpuMode::Guest) != GpuMode::Host;
        const bool venusOn = cfg_.gpu.venus.value_or(false);
        const QString v = cfg_.gpu.gpuDriver.value_or(QString());
        const bool needsLegacy =
            !software && cfg_.gpu.venus.has_value() && !*cfg_.gpu.venus && v.isEmpty();
        if (venusCombo_) venusCombo_->setVisible(needsLegacy);
        if (venusRowLabel_) venusRowLabel_->setVisible(needsLegacy);

        if (software)
            gpuDriverCombo_->setCurrentIndex(qMax(0, gpuDriverCombo_->findData(kGpuSoftware)));
        else if (venusOn)
            gpuDriverCombo_->setCurrentIndex(qMax(0, gpuDriverCombo_->findData(kGpuVenus)));
        else if (v.isEmpty())
            gpuDriverCombo_->setCurrentIndex(0);
        else if (gpuDriverCombo_->findData(v) >= 0)
            gpuDriverCombo_->setCurrentIndex(gpuDriverCombo_->findData(v));
        else
            gpuDriverCombo_->setCurrentText(v);  // async probe may not have listed it (yet)
    }
    if (venusCombo_)
        venusCombo_->setCurrentIndex(qMax(
            0, venusCombo_->findData(cfg_.gpu.venus ? (*cfg_.gpu.venus ? QStringLiteral("true")
                                                                       : QStringLiteral("false"))
                                                    : QString())));
    if (buildNiceSpin_) buildNiceSpin_->setValue(cfg_.image.buildNice.value_or(15));
    if (archVariantCombo_) {
        const QString v = cfg_.image.archVariant.value_or(QString());
        archVariantCombo_->setCurrentText(v.isEmpty() ? archVariantCombo_->itemText(0) : v);
    }
    if (buildJobsSpin_) buildJobsSpin_->setValue(cfg_.image.buildJobs.value_or(0));
    if (buildMemSpin_) buildMemSpin_->setValue(cfg_.image.buildMemGiB.value_or(0));
    if (soongMemSpin_) soongMemSpin_->setValue(cfg_.image.soongMemGiB.value_or(0));
    macvlanEdit_->setText(optStr(cfg_.network.macvlanIp));
    macvlanSubnetEdit_->setText(optStr(cfg_.network.macvlanSubnet));
    macvlanGatewayEdit_->setText(optStr(cfg_.network.macvlanGateway));
    macvlanParentEdit_->setText(optStr(cfg_.network.macvlanParent));
    macvlanRangeEdit_->setText(optStr(cfg_.network.macvlanRange));
    networkModeCombo_->setCurrentIndex(qMax(
        0, networkModeCombo_->findData(cfg_.network.networkMode.value_or(QString()))));
    {
        // The M lives beside the box, so the box holds the number. Only the Mbps spelling is
        // unwrapped: 8000K or a bare bit count is a mirror unit somebody chose by hand, and it is
        // shown as it stands rather than converted — the suffix beside it says what a PLAIN number
        // there means, and rewriting an existing value to match would be answering a different
        // question than the one asked.
        const QString br = cfg_.mirror.videoBitRate.value_or(QString());
        const QRegularExpressionMatch m = kBitRateMbps.match(br);
        bitrateEdit_->setText(m.hasMatch() ? m.captured(1) : br);
    }
    videoCodecCombo_->setCurrentIndex(
        qMax(0, videoCodecCombo_->findData(cfg_.mirror.videoCodec.value_or(QStringLiteral("h265")))));
    audioCheck_->setChecked(cfg_.mirror.audio.value_or(false));
    if (!navActionCombos_.isEmpty()) {
        // Read the button-indexed --mouse-bind vector back out action-first, which is how the card
        // is laid out. kResolvedMouseBind is what an unset profile actually runs with, so an
        // untouched Navigation card shows the live bindings rather than an empty one.
        navBindRaw_ = cfg_.input.mouseBind.value_or(QString());
        const QString primary = (navBindRaw_.isEmpty() ? kResolvedMouseBind : navBindRaw_)
                                    .section(QLatin1Char(':'), 0, 0);
        for (int i = 0; i < kNavActions.size(); ++i) {
            // A hand-written vector may put one action on two buttons; the card can only show it
            // on one. First wins, and the other slot is left untouched until this row is edited.
            const int btn = primary.indexOf(QLatin1Char(kNavActions.at(i).bindChar));
            const int idx = navActionCombos_.at(i)->findData(btn < kNavButtons.size() ? btn : -1);
            navActionCombos_.at(i)->setCurrentIndex(qMax(0, idx));
        }
    }
    if (!navKeyEdits_.isEmpty()) {
        // key_bind, read action-first like the mouse vector. First entry per action wins, matching
        // the mouse rows; an SDL name the card cannot translate is kept aside verbatim so a save
        // cannot quietly unbind a hand-written key it has no picture of.
        for (int i = 0; i < navKeyEdits_.size(); ++i) {
            navKeyEdits_.at(i)->clear();
            navKeyEdits_.at(i)->setProperty("lastGoodKey", 0);
            navKeyRawNames_[i].clear();
        }
        const QStringList entries = cfg_.input.keyBind.value_or(QString())
                                        .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &entry : entries) {
            const int colon = entry.lastIndexOf(QLatin1Char(':'));
            if (colon < 1 || colon + 2 != entry.size()) continue;  // not <key>:<action char>
            for (int i = 0; i < kNavActions.size(); ++i) {
                if (entry.at(colon + 1) != QLatin1Char(kNavActions.at(i).bindChar)) continue;
                if (navKeyEdits_.at(i)->keySequence().isEmpty()
                        && navKeyRawNames_.at(i).isEmpty()) {
                    const int qtKey = qtKeyFromSdlName(entry.left(colon));
                    if (qtKey) {
                        navKeyEdits_.at(i)->setKeySequence(QKeySequence(qtKey));
                        navKeyEdits_.at(i)->setProperty("lastGoodKey", qtKey);
                    } else {
                        navKeyRawNames_[i] = entry.left(colon);
                    }
                }
                break;
            }
        }
    }
    if (shortcutModCombo_) {
        const QString want = cfg_.input.shortcutMod.value_or(QString());
        int idx = shortcutModCombo_->findData(want);
        // A hand-written shortcut_mod= is a valid modifier vector even when this list has no row for
        // it — keep it selectable so opening the page cannot quietly reset the modifier.
        if (idx < 0 && !want.isEmpty()) {
            shortcutModCombo_->addItem(QStringLiteral("%1  (custom)").arg(want), want);
            idx = shortcutModCombo_->count() - 1;
        }
        shortcutModCombo_->setCurrentIndex(qMax(0, idx));
        syncNavKeyHints();
    }
    if (kbdIdentityCombo_) {
        const QString want = cfg_.input.kbdIdentity.value_or(QString());
        int idx = kbdIdentityCombo_->findData(want);
        // A configured keyboard that is not plugged in right now must not be silently downgraded
        // to Auto — keep it selectable so saving the page cannot quietly change it.
        if (idx < 0 && !want.isEmpty()) {
            kbdIdentityCombo_->addItem(QStringLiteral("%1  (not connected)").arg(want), want);
            idx = kbdIdentityCombo_->count() - 1;
        }
        kbdIdentityCombo_->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    if (sharedInputsList_) {
        // Rebuild from the live hardware FIRST. This used to run once, at construction, while the
        // "not connected" rows below are appended on every load — so each profile switch stacked
        // its own leftovers on top of the previous profile's and the list only ever grew.
        populateSharedInputs();
        QStringList want = cfg_.input.sharedInputs;
        // One entry that is really several names run together is a remorarc left by the old
        // space-joined format — split it back where the host's own device list says so.
        if (want.size() == 1) {
            QStringList present;
            for (int i = 0; i < sharedInputsList_->count(); ++i)
                present << sharedInputsList_->item(i)->data(Qt::UserRole).toString();
            if (const auto recovered = splitJoinedNames(want.constFirst(), present))
                want = *recovered;
        }
        QStringList missing = want;
        for (int i = 0; i < sharedInputsList_->count(); ++i) {
            auto *it = sharedInputsList_->item(i);
            const QString name = it->data(Qt::UserRole).toString();
            const bool on = want.contains(name);
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
            missing.removeAll(name);
        }
        // A shared device that is unplugged right now stays listed and ticked, so simply opening
        // this page cannot quietly drop it from the config.
        for (const QString &name : missing) {
            auto *it = new QListWidgetItem(QStringLiteral("%1  — not connected").arg(name));
            it->setData(Qt::UserRole, name);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Checked);
            sharedInputsList_->addItem(it);
        }
    }
    winWidthEdit_->setText(optInt(cfg_.mirror.windowWidth));
    winHeightEdit_->setText(optInt(cfg_.mirror.windowHeight));
    fsSeparateCheck_->setChecked(cfg_.mirror.fullscreenSeparateDisplay.value_or(false));
    fsDisplayEdit_->setText(optStr(cfg_.mirror.fullscreenDisplay));
    fsAppEdit_->setText(optStr(cfg_.mirror.fullscreenApp));
    fsNewWindowCheck_->setChecked(cfg_.mirror.desktopNewWindow.value_or(false));
    // No integration widgets to populate: desktop menu, ARM updates and the ARM guard are cells
    // of the Profiles table, which reads every profile's saved config in rebuildProfilesList.
    sharedFoldersEdit_->setText(cfg_.integration.sharedFolders.join(QStringLiteral(" ")));
    // desktop-mode res/app rows show only when separate display is on — the setChecked above
    // fires the visibility sync on change; an unchanged state was already synced at build time
    if (cfg_.advanced.rootHiding)
        rootHidingCombo_->setCurrentIndex(qMax(0, rootHidingCombo_->findData(*cfg_.advanced.rootHiding)));
    dnsEdit_->setText(cfg_.network.dns.join(QStringLiteral(", ")));
    proxyTypeCombo_->setCurrentIndex(
        qMax(0, proxyTypeCombo_->findData(cfg_.network.proxyType.value_or(QString()))));
    proxyHostEdit_->setText(optStr(cfg_.network.proxyHost));
    proxyPortEdit_->setText(optInt(cfg_.network.proxyPort));
    roEdit_->setText(cfg_.advanced.roOverrides.join(QStringLiteral(", ")));
    lastRefRepo_.clear();
    onRefreshSourceRefs();
    // A different profile is a different container: lock first (the safe answer for a device that
    // may be up) and let the probe unlock it, rather than inheriting the last profile's verdict.
    setDataDirLocked(true);
    refreshDataDirLock();
    refreshSourceState();  // the status-bar token is per-profile: tree, ref and built commit all move
    suppressAutosave_ = false;
}

// Persist every config change the instant it happens — no Save button, no "forgot to save" class
// of bug (a stale checkbox once silently produced an image without its patch). Idempotent: writes
// the current widget state to this profile's remorarc section.
void MainWindow::autosave() {
    if (suppressAutosave_) return;
    applyToConfig();
    saveInstance(path_, instance_, cfg_);
    noteRemorarcWritten();
    statusBar()->showMessage(QStringLiteral("saved"), 1500);
}

// Content hash, not mtime: autosave rewrites remorarc constantly, so mtime alone cannot tell this
// window's own write from an external one — which is the entire question a watcher event asks.
QByteArray MainWindow::remorarcFingerprint() const {
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

// Call after every write this window makes, so the resulting watcher event is recognised as ours.
void MainWindow::noteRemorarcWritten() {
    rcFingerprint_ = remorarcFingerprint();
    watchRemorarc();
}

// QSettings writes atomically — temp file plus rename — so the watch follows the replaced inode
// and is dropped after every save, ours and everyone else's. Re-arm it each time; addPath() on an
// already-watched path is a harmless no-op.
void MainWindow::watchRemorarc() {
    if (!rcWatcher_ || path_.isEmpty()) return;
    if (!rcWatcher_->files().contains(path_) && QFileInfo::exists(path_))
        rcWatcher_->addPath(path_);
}

void MainWindow::onRemorarcChanged() {
    // Debounce: an atomic replace fires directoryChanged and fileChanged in quick succession, and
    // the file is briefly absent in between — reading it too early yields an empty fingerprint and
    // a spurious "changed" verdict.
    if (rcCheckPending_) return;
    rcCheckPending_ = true;
    QTimer::singleShot(250, this, [this] {
        rcCheckPending_ = false;
        watchRemorarc();
        const QByteArray now = remorarcFingerprint();
        if (now.isEmpty() || now == rcFingerprint_) return;  // unreadable, or our own write
        rcFingerprint_ = now;

        // A QLineEdit the user has typed into but not committed (no Return, no focus-out) has no
        // counterpart on disk yet, so reloading would silently discard it — the same class of
        // silent loss this bead is about, just pointed the other way.
        bool dirty = false;
        for (QLineEdit *w : findChildren<QLineEdit *>())
            if (w->isModified()) { dirty = true; break; }

        if (!dirty || qEnvironmentVariableIsSet("REMORA_SHOT")) {
            reloadRemorarc();
            statusBar()->showMessage(
                QStringLiteral("remorarc changed on disk — reloaded"), 5000);
            return;
        }
        // Genuine conflict: both sides changed. Never resolve it silently in either direction.
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("remorarc changed on disk"));
        box.setText(QStringLiteral("<b>remorarc was modified outside this window.</b>"));
        box.setInformativeText(QStringLiteral(
            "This window also has an edit you haven't committed yet.\n\n"
            "Reload — take the file's values and discard your uncommitted edit.\n"
            "Keep mine — your next change overwrites the file's version."));
        QPushButton *reload =
            box.addButton(QStringLiteral("Reload from disk"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Keep my edits"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == reload) {
            reloadRemorarc();
            statusBar()->showMessage(QStringLiteral("reloaded from disk"), 5000);
        }
    });
}

// Re-read this profile from disk and repopulate. loadConfig() suppresses autosave while it
// populates, so the reload cannot immediately write the values straight back out.
void MainWindow::reloadRemorarc() {
    cfg_ = loadInstance(path_, instance_);
    loadConfig();
}

// Wire every config widget's "value changed" signal to autosave(). Called once after the first
// loadConfig so the initial population doesn't fire it.
void MainWindow::wireAutosave() {
    for (QComboBox *w : findChildren<QComboBox *>()) {
        connect(w, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                &MainWindow::autosave);
        if (w->isEditable())
            connect(w, &QComboBox::currentTextChanged, this, &MainWindow::autosave);
    }
    for (QLineEdit *w : findChildren<QLineEdit *>())
        connect(w, &QLineEdit::editingFinished, this, &MainWindow::autosave);
    for (QCheckBox *w : findChildren<QCheckBox *>())
        connect(w, &QCheckBox::toggled, this, &MainWindow::autosave);
    for (QSpinBox *w : findChildren<QSpinBox *>())
        connect(w, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::autosave);
}

static QStringList splitList(const QString &s) {
    QStringList out;
    for (const QString &p : s.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts))
        out << p.trimmed();
    return out;
}

void MainWindow::applyToConfig() {
    // The backend is inferred from the docker host (inferBackend) — the GUI never writes the
    // legacy key, so a GUI save drops it.
    cfg_.backend.backend = std::nullopt;
    // sshHost + containerName are deliberately untouched: the host lives on
    // the Runners rows now, the name is pinned at profile creation.
    cfg_.backend.dataDir = readStr(dataDirEdit_);
    // expand the display-only ~ back to $HOME — the stored path must stay absolute
    if (cfg_.backend.dataDir && cfg_.backend.dataDir->startsWith(QLatin1Char('~')))
        cfg_.backend.dataDir->replace(0, 1, QDir::homePath());
    cfg_.backend.hostAdbPort = readInt(hostAdbPortEdit_);
    // android_version is deliberately NOT written: it is creation-time state (onNewProfile),
    // has no widget here, and rewriting it from one would be the only way this page could
    // change which Android release a live profile is.
    cfg_.image.features = QStringList(features_.begin(), features_.end());
    cfg_.image.features.sort();
    // image_tag is deliberately NOT written here — not because it is unreachable (the Active image
    // combo sets it, and autosave() runs THIS function on the way to persisting it), but because it
    // is not a widget value to be read back. onActiveImageChanged mutates cfg_ directly and this
    // pass must leave that result alone; re-deriving it from the page is how a hand-set pin gets
    // quietly rewritten. Same rule hw_decode and use_codec2 follow.
    // buildHost, sourceHost and buildDir are deliberately NOT written: none has a widget any
    // more, so a hand-set build_host=/source_host=/build_dir= in remorarc survives every save
    // instead of being blanked by it.
    cfg_.image.sourceTree = readStr(sourceTreeEdit_);
    {
        // The UNION of the two tiers (selectedPatchKeys). Writing patchChecks_ alone would drop
        // the entire golden stack from source_patches the first time this page saved — the page
        // silently unpicking twenty-odd patches it had stopped drawing.
        QStringList on = selectedPatchKeys();
        on.sort();  // stored sorted, so a remorarc diff shows real edits and not reordering
        cfg_.image.sourcePatches = on;
    }
    {
        const QString ref = sourceRefCombo_->currentText().trimmed();
        cfg_.image.sourceRef = ref.isEmpty() ? std::nullopt : std::optional<QString>(ref);
    }
    {
        const QString k = sourceKindCombo_->currentData().toString();
        cfg_.image.sourceKind =
            k == QLatin1String("lineage") ? std::nullopt : std::optional<QString>(k);
    }
    cfg_.image.outputDir = readStr(outputDirEdit_);
    cfg_.display.width = readInt(widthEdit_);
    cfg_.display.height = readInt(heightEdit_);
    cfg_.display.dpi = readInt(dpiEdit_);
    cfg_.display.fps = readInt(fpsCombo_->lineEdit());
    cfg_.advanced.autoSleepMinutes = readInt(autoSleepEdit_);
    if (gpuDriverCombo_) {
        // A picked item stores the raw driver as DATA (the display text carries the node too);
        // free text in the editable combo IS the driver name. Parenthesized = the auto row.
        const int i = gpuDriverCombo_->currentIndex();
        const QString v =
            (i >= 0 && gpuDriverCombo_->itemText(i) == gpuDriverCombo_->currentText())
                ? gpuDriverCombo_->itemData(i).toString()
                : gpuDriverCombo_->currentText().trimmed();
        // This one row now answers gpu_mode as well as which card. SwiftShader leaves the whole
        // group UNSET — it is what the resolver bakes, and defaults belong there — while every
        // other entry is a GPU, so it writes gpu_mode=host and lets the arms below name the card.
        cfg_.gpu.mode =
            v == kGpuSoftware ? std::nullopt : std::optional<GpuMode>(GpuMode::Host);
        if (v == kGpuSoftware) {
            cfg_.gpu.gpuDriver = std::nullopt;
            cfg_.gpu.venus = std::nullopt;
        } else if (v == kGpuVenus) {
            // NVIDIA renders through Venus, so there is no DRM driver to name.
            cfg_.gpu.gpuDriver = std::nullopt;
            cfg_.gpu.venus = true;
        } else {
            const bool isAuto = v.isEmpty() || v.startsWith(QLatin1Char('('));
            cfg_.gpu.gpuDriver = isAuto ? std::nullopt : std::make_optional(v);
            // Naming a GPU means rendering on THAT one, so Venus goes off; auto leaves Venus on
            // auto, which only engages when the whole stack is installed. The legacy tri-state
            // still wins while it is visible, so an explicit "off" survives being re-saved.
            if (venusCombo_ && venusCombo_->isVisible()) {
                const QString vn = venusCombo_->currentData().toString();
                cfg_.gpu.venus = vn.isEmpty() ? std::nullopt
                                              : std::optional<bool>(vn == QLatin1String("true"));
            } else {
                cfg_.gpu.venus = isAuto ? std::nullopt : std::optional<bool>(false);
            }
        }
    }
    // hw_decode and use_codec2 are deliberately NOT touched here: neither has a widget any
    // more, and rewriting either from one would be the only way a page with no such row could
    // still clear a hand-set hw_decode=false / use_codec2=false. Leaving them alone is what
    // keeps the remorarc opt-out an opt-out.
    // source_pinned and source_repo are deliberately NOT written: neither has a widget any more,
    // and writing them from one would be the only way this page could clear a hand-set value.
    if (archVariantCombo_) {
        // The placeholder row reads "(generic …)" — parenthesized text means unset, and unset
        // means the tree's baseline (the config-nullopt convention: defaults live in one place).
        const QString v = archVariantCombo_->currentText().trimmed();
        cfg_.image.archVariant = (v.isEmpty() || v.startsWith(QLatin1Char('(')))
                                     ? std::nullopt
                                     : std::make_optional(v);
    }
    if (buildNiceSpin_)
        cfg_.image.buildNice = buildNiceSpin_->value() == 15
                                   ? std::nullopt
                                   : std::make_optional(buildNiceSpin_->value());
    if (buildJobsSpin_)
        cfg_.image.buildJobs = buildJobsSpin_->value() == 0
                                   ? std::nullopt
                                   : std::make_optional(buildJobsSpin_->value());
    if (buildMemSpin_)
        cfg_.image.buildMemGiB = buildMemSpin_->value() == 0
                                     ? std::nullopt
                                     : std::make_optional(buildMemSpin_->value());
    if (soongMemSpin_)
        cfg_.image.soongMemGiB = soongMemSpin_->value() == 0
                                     ? std::nullopt
                                     : std::make_optional(soongMemSpin_->value());
    cfg_.network.macvlanIp = readStr(macvlanEdit_);
    cfg_.network.macvlanSubnet = readStr(macvlanSubnetEdit_);
    cfg_.network.macvlanGateway = readStr(macvlanGatewayEdit_);
    cfg_.network.macvlanParent = readStr(macvlanParentEdit_);
    cfg_.network.macvlanRange = readStr(macvlanRangeEdit_);
    {
        const QString mode = networkModeCombo_->currentData().toString();
        cfg_.network.networkMode = mode.isEmpty() ? std::nullopt : std::make_optional(mode);
    }
    {
        // A plain number is Mbps, per the suffix next to the field; anything else is already a
        // mirror unit and goes through untouched, which is what keeps a hand-written 8000K alive.
        const QString t = bitrateEdit_->text().trimmed();
        cfg_.mirror.videoBitRate =
            t.isEmpty() ? std::nullopt
                        : std::optional<QString>(kBitRateDigits.match(t).hasMatch()
                                                     ? t + QLatin1Char('M')
                                                     : t);
    }
    {
        // Unset means the resolver's default, h265 (Resolver.cpp) — so h265 is the one choice that
        // is not stored. Comparing against h264 here once made h264 unselectable: choosing it
        // saved "unset", which deployed h265, while an unset profile displayed h264.
        const QString vc = videoCodecCombo_->currentData().toString();
        cfg_.mirror.videoCodec = vc == QLatin1String("h265") ? std::nullopt
                                                             : std::optional<QString>(vc);
    }
    // Audio defaults off → store only when on.
    cfg_.mirror.audio = audioCheck_->isChecked() ? std::optional<bool>(true) : std::nullopt;
    if (!navActionCombos_.isEmpty()) {
        // Back to the mirror's button-indexed vector. Slots this card did not claim keep whatever the
        // profile arrived with — that is how a hand-written '-' (ignore the click) survives a save
        // from a card whose rows only speak of actions.
        const QString wasPrimary = navBindRaw_.section(QLatin1Char(':'), 0, 0);
        QString primary(kNavButtons.size(), kNavUnmapped);
        for (int b = 0; b < primary.size(); ++b)
            if (b < wasPrimary.size() && !QStringLiteral("bhsn").contains(wasPrimary.at(b)))
                primary[b] = wasPrimary.at(b);
        for (int i = 0; i < kNavActions.size(); ++i) {
            const int btn = navActionCombos_.at(i)->currentData().toInt();
            if (btn >= 0 && btn < primary.size()) primary[btn] = QLatin1Char(kNavActions.at(i).bindChar);
        }
        // The second sequence (Shift held) always forwards the raw click, so a click Android needs
        // is never more than a modifier away whatever the rows say. Preserve a hand-written one.
        const QString secondary = navBindRaw_.contains(QLatin1Char(':'))
                                      ? navBindRaw_.section(QLatin1Char(':'), 1, 1)
                                      : QStringLiteral("++++");
        const QString mb = primary + QLatin1Char(':') + secondary;
        // Matching the resolver's default stays UNSET — defaults live in the resolver, never in
        // the config (and a profile written before the default moved then follows it).
        cfg_.input.mouseBind =
            mb == kResolvedMouseBind ? std::nullopt : std::optional<QString>(mb);
        navBindRaw_ = mb;
    }
    if (!navKeyEdits_.isEmpty()) {
        // Rebuilt whole from the rows: every key_bind entry belongs to an action this card has a
        // row for, so unlike mouse_bind there is nothing outside it to preserve — except a
        // hand-written name the card could not translate, which rides along verbatim until its
        // row is edited. A second key on the same action collapses to the shown one, the same
        // first-wins the mouse rows document.
        QStringList entries;
        for (int i = 0; i < navKeyEdits_.size(); ++i) {
            const QKeySequence seq = navKeyEdits_.at(i)->keySequence();
            QString name = seq.isEmpty() ? QString() : sdlKeyName(seq[0].key());
            if (name.isEmpty()) name = navKeyRawNames_.at(i);
            if (!name.isEmpty())
                entries << name + QLatin1Char(':') + QLatin1Char(kNavActions.at(i).bindChar);
        }
        // Empty = no bindings, which is the resolver default and so stays unset.
        cfg_.input.keyBind = entries.isEmpty()
                                 ? std::nullopt
                                 : std::optional<QString>(entries.join(QLatin1Char(',')));
    }
    if (shortcutModCombo_) {
        // Empty = the mirror's own lalt,lsuper → stays unset, so no --shortcut-mod is emitted.
        const QString sm = shortcutModCombo_->currentData().toString();
        cfg_.input.shortcutMod = sm.isEmpty() ? std::nullopt : std::optional<QString>(sm);
    }
    if (kbdIdentityCombo_) {
        const QString sel = kbdIdentityCombo_->currentData().toString();
        cfg_.input.kbdIdentity = sel.isEmpty() ? std::optional<QString>() : sel;
    }
    if (sharedInputsList_) {
        QStringList shared;
        for (int i = 0; i < sharedInputsList_->count(); ++i)
            if (sharedInputsList_->item(i)->checkState() == Qt::Checked)
                shared << sharedInputsList_->item(i)->data(Qt::UserRole).toString();
        cfg_.input.sharedInputs = shared;
    }
    cfg_.mirror.windowWidth = readInt(winWidthEdit_);
    cfg_.mirror.windowHeight = readInt(winHeightEdit_);
    // Separate defaults false → store only when on.
    cfg_.mirror.fullscreenSeparateDisplay =
        fsSeparateCheck_->isChecked() ? std::optional<bool>(true) : std::nullopt;
    cfg_.mirror.fullscreenDisplay = readStr(fsDisplayEdit_);
    cfg_.mirror.fullscreenApp = readStr(fsAppEdit_);
    cfg_.mirror.desktopNewWindow =
        fsNewWindowCheck_->isChecked() ? std::optional<bool>(true) : std::nullopt;
    // desktopMenu, autoUpdateApps/Hours and armGuard are deliberately NOT written: they have no
    // page widgets any more — the Profiles table's row handlers write each profile's copy (and
    // mirror the active one into cfg_), so a full-section save must not clobber them.
    // app_modes: the disk copy is authoritative — the Integration tree AND the first-launch
    // prompt (an external `remora app` process) both write it directly; re-read so a full-section
    // save can't clobber a remembered prompt answer.
    cfg_.integration.appModes = loadInstance(path_, instance_).integration.appModes;
    // Same for arm_apps: the pins are written by the ARM-install path and the guard, both of
    // which can land while this page is open.
    cfg_.integration.armApps = loadInstance(path_, instance_).integration.armApps;
    cfg_.integration.sharedFolders = splitList(sharedFoldersEdit_->text());
    const QString rh = rootHidingCombo_->currentData().toString();
    cfg_.advanced.rootHiding = rh == QStringLiteral("off") ? std::nullopt : std::optional<QString>(rh);
    // denylist_packages is written by the per-app toggle (setEntryDenylisted), which edits
    // ONE entry at a time — see the note there for why it must not be rebuilt from the table.
    cfg_.network.dns = splitList(dnsEdit_->text());
    const QString proxyType = proxyTypeCombo_->currentData().toString();
    cfg_.network.proxyType =
        proxyType.isEmpty() ? std::nullopt : std::optional<QString>(proxyType);
    cfg_.network.proxyHost = readStr(proxyHostEdit_);
    cfg_.network.proxyPort = readInt(proxyPortEdit_);
    cfg_.advanced.roOverrides = splitList(roEdit_->text());
}

Backend MainWindow::currentBackend() const {
    // inferred, never chosen: blank docker host = this machine, anything else = remote ssh
    return inferBackend(cfg_);
}

// The single writer of every feature checkbox's tooltip. Two callers contribute to it and neither
// knows what the other found: recompute() supplies the gating engine's objection to the current
// selection, refreshPatchApplicability() supplies "this version has no assets for the patch it
// needs". Each stashes its half in a map and calls this; whoever ran last no longer erases the
// other's work, which is what happened while they both wrote setToolTip directly.
void MainWindow::applyFeatureTooltips() {
    for (auto it = featureChecks_.constBegin(); it != featureChecks_.constEnd(); ++it) {
        const Feature *f = findFeature(it.key());
        if (!f) continue;
        QString t = featureTooltip(*f, featureGateReason_.value(it.key()));
        const QString bad = featureInapplicable_.value(it.key());
        if (!bad.isEmpty()) t += QStringLiteral("\n\n%1").arg(bad);
        it.value()->setToolTip(t);
    }
}

// Live: recompute the resolved image tag + per-feature gate reasons whenever a toggle changes.
// The profile's Android release. Creation-time state with no widget on any page, so it is read
// from the config rather than a combo — see the note where the combo used to be.
int MainWindow::androidVersion() const { return cfg_.image.androidVersion.value_or(16); }

void MainWindow::recompute() {
    // Drop keys for features that no longer exist (e.g. a removed feature like play_cert left stale
    // in the saved config) — otherwise an orphan key shows in the recipe label and looks like an
    // un-satisfiable request forcing a source rebuild.
    for (auto it = features_.begin(); it != features_.end();)
        it = findFeature(*it) ? std::next(it) : features_.erase(it);
    // A PIN NO LONGER BLANKS THIS LIST. Under an explicit image_tag every checkbox used to be
    // disabled AND unchecked here, on the reasoning that features are build-time and a fixed image
    // has already been built. Two thirds of that is false. Features are read at DEPLOY time too —
    // camera_v4l2, usb_audio and mirror_agent are resolver inputs (Resolver.cpp:311, :322, :527)
    // and apply to a pinned image exactly as to a resolved one — and features_ itself was never
    // cleared (the QSignalBlocker saw to that), so a profile carrying fifteen features rendered as
    // carrying none: the page showed a state the config did not hold. The pin decides ONE thing,
    // which tag deploys, and the Active image card is where it now says so.
    for (auto it = featureChecks_.constBegin(); it != featureChecks_.constEnd(); ++it) {
        QSignalBlocker block(it.value());
        it.value()->setEnabled(true);
        it.value()->setChecked(features_.contains(it.key()));
    }
    // Gates no longer render on the page — no ⚠ column, no red summary. They are still COMPUTED,
    // and each feature's live objection rides in its checkbox tooltip; the build's own preflight is
    // what actually refuses a gated combination.
    featureGateReason_.clear();
    for (const Gate &g : featureGates(features_))
        if (!g.ok) featureGateReason_.insert(g.key, g.reason);
    applyFeatureTooltips();

    syncConditionalRows();
    refreshImageGating();
}

// Show only the rows that apply to the current backend/network mode. Hidden (not greyed) so the
// pages stay short; the fields keep their persisted values and reappear when applicable again.
void MainWindow::syncConditionalRows() {
    if (!deviceForm_) return;
    const Backend b = currentBackend();
    // "(default)" resolves to bridge on every backend now, so the macvlan rows show only when the
    // profile asks for macvlan explicitly.
    const QString mode = networkModeCombo_->currentData().toString();
    const bool macvlan = mode == QLatin1String("macvlan");
    for (QLineEdit *e : {macvlanEdit_, macvlanSubnetEdit_, macvlanGatewayEdit_,
                         macvlanParentEdit_, macvlanRangeEdit_})
        deviceForm_->setRowVisible(e, macvlan);
    // Host and port only mean anything to a static proxy — same conditioning as the macvlan
    // rows above.
    const bool staticProxy =
        proxyTypeCombo_->currentData().toString() == QLatin1String("static");
    deviceForm_->setRowVisible(proxyHostEdit_, staticProxy);
    deviceForm_->setRowVisible(proxyPortEdit_, staticProxy);
    // bare and remote have DIFFERENT baked data-dir defaults (Resolver.cpp:125 vs :181), so a
    // static placeholder is wrong for one of them. It used to read the bare path unconditionally
    // (fixed). Same class as bd remora-bm7.9: a default stated in the UI and implemented
    // differently elsewhere.
    dataDirEdit_->setPlaceholderText(b == Backend::Remote
                                         ? QStringLiteral("~/remora-data")
                                         : QStringLiteral("~/.remora-bm-data"));
}

// Everything on this page that depends on which image the profile deploys: the Active image row,
// and the HW codecs an image cannot deliver.
//
// THE PIN GREYS NOTHING NOW. It used to disable the Source build card and the whole
// feature/patch box outright. That made sense while image_tag was set by a catalog of third-party
// prebuilts — you could not know what someone else's image shipped, so you could not offer to
// change it. The catalogs retired with the selector (bd remora-2ylt) and a pin became, in
// practice, one of this project's own builds; the greying outlived its reason and turned into a
// dead page:
//   · A source build PRODUCES an artifact and deploys nothing. Its tag comes from features_ plus
//     arch_variant through sourceBuiltTag(), never from the pin, so a pin cannot make building
//     incoherent — and this profile's pin is precisely the promoted form of what its own tree
//     builds, i.e. the greying blocked the one operation that maintains the pinned image.
//   · The status bar tells you "source: rebuild pending" off built_commit while the controls that
//     would do it were inert. One window, two contradictory instructions.
//   · Feature checkboxes are live under a pin (see the note in recompute()).
// What replaces it is a stated cause and an Unpin, which is what the greying was standing in for.
void MainWindow::refreshImageGating() {
    const QString tag = cfg_.image.imageTag.value_or(QString());
    const bool explicitTag = !tag.isEmpty();
    if (activeImageCombo_) {
        // Rebuilt from the live image list on every recompute — onRefreshArchives ends in one, so
        // a build that just finished is selectable without reopening the page.
        QSignalBlocker block(activeImageCombo_);
        activeImageCombo_->clear();
        // Entry 0 is the unpinned state, and it SPELLS OUT the tag it resolves to rather than
        // saying "(default)". Unpinning is not always a no-op: arch_variant is appended to a
        // derived tag and never to an explicit one (Resolver.cpp:439), so a CPU-tuned profile
        // changes image family the moment the pin goes. Better read here than discovered as a
        // missing image at the next Connect.
        activeImageCombo_->addItem(QStringLiteral("from features — %1").arg(derivedImageTag()),
                                   QString());
        QStringList tags;
        for (int i = 0; archivesTree_ && i < archivesTree_->topLevelItemCount(); ++i) {
            const QString t =
                archivesTree_->topLevelItem(i)->data(0, Qt::UserRole + 1).toString();
            if (!t.isEmpty() && !tags.contains(t)) tags << t;
        }
        // A pin naming an image this host does not have is still the profile's pin — list it, or
        // selecting anything else would be the only way to leave a state the page could not show.
        if (explicitTag && !tags.contains(tag)) tags << tag;
        tags.sort();
        for (const QString &t : tags) activeImageCombo_->addItem(t, t);
        activeImageCombo_->setCurrentIndex(qMax(0, activeImageCombo_->findData(tag)));
    }
    // Grey the HW-encoder codecs when this profile's image can't deliver them: an explicit tag
    // must ship the encoder itself (-hwc2); feature-based A16 qualifies because the resolver
    // auto-bumps gapps images to -hwc2 when a HW codec is selected. Everything else → h264 only.
    if (videoCodecCombo_) {
        // Ask the resolver the question directly, rather than hard-coding which Android
        // versions can encode. The honest test is "if h265 were selected, would the image that
        // actually deploys ship the encoder?" — so resolve a copy with h265 set and look at the
        // tag it lands on. Asking about the CURRENT config instead would be circular: the
        // resolver only bumps a gapps image to -hwc2 BECAUSE a hardware codec is selected, so
        // h265 could never be enabled while h264 is chosen.
        //
        // This used to be `version == 16`, which was true when A16 gapps was the only image the
        // resolver bumped. A17 source builds are -hwc2 too (the pre-rename tag family:...-gapps-wv-hwc2-*),
        // so the rule silently greyed out h265 on a profile whose image ships the encoder, reset
        // the selection to h264, and left no way to choose it.
        RemoraConfig hevcProbe = cfg_;
        hevcProbe.mirror.videoCodec = QStringLiteral("h265");
        const bool hevcCapable =
            resolve(hevcProbe, currentBackend()).imageTag.contains(QLatin1String("hwc2"));
        // h265 rides the -hwc2 image's VAAPI encoder; av1 has NO hardware encoder in any
        // Remora image (the software AV1 encoder dies instantly at full resolution) — the
        // c2-va component is HEVC-only, so av1 stays disabled everywhere for now.
        const auto codecOk = [hevcCapable](const QString &c) {
            return c == QLatin1String("h264")
                   || (c == QLatin1String("h265") && hevcCapable);
        };
        if (auto *model = qobject_cast<QStandardItemModel *>(videoCodecCombo_->model())) {
            for (int i = 0; i < videoCodecCombo_->count(); ++i) {
                const QString dc = videoCodecCombo_->itemData(i).toString();
                if (auto *it = model->item(i)) {
                    it->setEnabled(codecOk(dc));
                    it->setToolTip(
                        codecOk(dc) ? QString()
                        : dc == QLatin1String("av1")
                            ? QStringLiteral("no Remora image ships an AV1 hardware encoder — "
                                             "the software one fails at full resolution")
                            : QStringLiteral("needs the -hwc2 image's hardware encoder — not "
                                             "available for this image / Android version"));
                }
            }
        }
        if (!codecOk(videoCodecCombo_->currentData().toString())) {
            const QString fallback =
                hevcCapable ? QStringLiteral("h265") : QStringLiteral("h264");
            QSignalBlocker block(videoCodecCombo_);
            videoCodecCombo_->setCurrentIndex(
                qMax(0, videoCodecCombo_->findData(fallback)));
            statusBar()->showMessage(
                QStringLiteral("video codec reset to %1 — the selected codec has no working "
                               "encoder on this image").arg(fallback),
                6000);
        }
    }
}


// Tint the chrome with the desktop accent color: card borders + titles, selection fills,
// splitter handles, toolbar rule. QPalette::Accent tracks the Plasma accent (Highlight is
// often a darker selection shade that vanishes on dark themes); titles use a lightened
// accent on dark schemes so the tint stays legible either way. Re-applied on palette
// changes so a wallpaper-driven accent switch restyles the app live.
// The whole visual identity, derived from the LIVE palette (wallpaper accent + light/dark) so
// a theme change restyles everything on the next PaletteChange — nothing is baked. Cards are
// filled rounded panels (one shade off the window), the sidebar is a distinct darker pane, and
// statuses render as pill chips.
void MainWindow::applyAccentStyle() {
    const QColor ac = paletteAccent(palette());
    const QColor w = palette().color(QPalette::Window);
    const bool dark = w.lightness() < 128;
    const QColor title = dark ? ac.lighter(150) : ac.darker(115);
    const QColor card = dark ? w.lighter(114) : w.darker(103);        // filled panel
    const QColor cardEdge = dark ? w.lighter(132) : w.darker(112);    // its faint outline
    const QColor side = dark ? w.darker(114) : w.darker(106);         // sidebar pane
    const QColor paneBg = dark ? w.darker(108) : w.darker(104);       // log pane
    // legible on the accent: dark text over a light accent, white over a dark one
    const QColor onAccent = ac.lightness() > 160 ? QColor(28, 28, 28) : QColor(255, 255, 255);
    const auto rgb = [](const QColor &c) {
        return QStringLiteral("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue());
    };
    setStyleSheet(
        QStringLiteral(
            // ── filled rounded cards; title as a bold accent label inside ──
            // margin-top 0 EVERYWHERE: titled, untitled and folded cards must all start at the
            // same Y, or switching pages makes the first card visibly hop (the title floats
            // within the padding, so it needs no outer margin)
            "QGroupBox { background: rgb(%2); border: 1px solid rgb(%3);"
            "  border-radius: 10px; margin-top: 0;"
            "  padding: 26px 10px 8px 10px; }"
            "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left;"
            "  left: 12px; top: 8px; color: rgb(%4); font-weight: 600; }"
            // folded cards live under a disclosure button that already carries the title
            "QGroupBox[folded=\"true\"] { padding-top: 10px; }"
            // ── sidebar pane: distinct background, roomy rounded nav rows ──
            "#sidebarPane { background: rgb(%5); }"
            "#sidebarList { background: transparent; }"
            // one padding for both sidebar states: icons must not shift when collapsing
            "#sidebarList::item { padding: 7px 8px; border-radius: 6px; margin: 0px 6px; }"
            // background only — an accent edge would inset the content, shifting icon+text
            // on selection
            "#sidebarList::item:selected { background: rgba(%1,95); }"
            "#sidebarList::item:hover:!selected { background: rgba(%1,40); }"
            // ── log pane: accent top edge + stepped background = a real boundary ──
            "#logPane { border-top: 2px solid rgb(%1); background: rgb(%6); }"
            "#logHead { background: rgb(%6); }"
            // ── status pill chips (Qt QSS has no bare-attribute selector, so per-variant) ──
            "QLabel[pill=\"up\"] { color: %7; background: rgba(46,125,50,55);"
            "  border-radius: 9px; padding: 1px 10px; font-weight: 600; }"
            "QLabel[pill=\"down\"] { color: %8; background: rgba(128,128,128,45);"
            "  border-radius: 9px; padding: 1px 10px; font-weight: 600; }"
            // docker could not be asked — amber, neither the green of up nor the grey of down
            "QLabel[pill=\"unknown\"] { color: %8; background: rgba(230,160,0,70);"
            "  border-radius: 9px; padding: 1px 10px; font-weight: 600; }"
            // transitory verbs ("starting…") — accent-tinted so they read as in-motion
            "QLabel[pill=\"busy\"] { color: %8; background: rgba(%1,55);"
            "  border-radius: 9px; padding: 1px 10px; font-weight: 600; }"
            // the status-bar busy spinner glyph
            "#busySpinner { color: rgb(%1); font-size: 14pt; font-weight: bold; "
            "  padding: 0 6px; }"
            // the accent rule under the sidebar's profile entry — hairline, translucent
            "#profileDivider { background: rgba(%1,90); }"
            // ── the accent-primary actions (Connect, Build image) ──
            "QToolButton[primary=\"true\"], QPushButton[primary=\"true\"] {"
            "  background: rgb(%1); color: rgb(%9);"
            "  border: none; border-radius: 6px; padding: 5px 14px; font-weight: 600; }"
            "QToolButton[primary=\"true\"]:hover, QPushButton[primary=\"true\"]:hover {"
            "  background: rgba(%1,215); }"
            "QToolButton[primary=\"true\"]:pressed, QPushButton[primary=\"true\"]:pressed {"
            "  background: rgba(%1,170); }"
            "QToolButton[primary=\"true\"]:disabled, QPushButton[primary=\"true\"]:disabled {"
            "  background: rgba(%1,80); }"
            // ── generic accents kept from the old style ──
            "QListWidget::item:selected { background: rgba(%1,95); border-radius: 4px; }"
            "QTreeWidget::item:selected { background: rgba(%1,90); }"
            "QToolButton:checked { background: rgba(%1,80); border-radius: 4px; }"
            "QSplitter::handle:vertical { background: transparent; height: 4px; }"
            "QSplitter::handle:horizontal { background: rgba(%1,50); width: 3px; }"
            "QStatusBar { border-top: 1px solid rgb(%3); }"
            "QScrollArea { background: transparent; border: none; }"
            // ── overlay-style scrollbars: a rounded accent pill on a track of no colour ──
            // Scrollbars must still be styled explicitly. An app stylesheet swaps every descendant
            // onto QStyleSheetStyle, and a sub-control this sheet never mentions stops being drawn
            // by Breeze — the groove then paints as bare black instead of the page colour, which is
            // what a window resize exposed.
            // The track cannot be `transparent` either, which is the same bug wearing the other
            // hat: transparent means "paint nothing", and NOTHING else paints the 12px gutter —
            // the scroll area's frame excludes it and the pane behind it is already covered — so
            // the band stayed at the backing store's zero and read as a black rail down the
            // sidebar and under every page. Each rule below therefore names the opaque surface its
            // bar actually floats on, which is what `transparent` was reaching for: a track with
            // no colour of its own.
            // The pill is inset by its handle margins, so the bar reserves 12px but only 6px is ink.
            "QScrollBar, QAbstractScrollArea::corner { background: palette(base);"
            "  border: none; margin: 0; }"                              // views: tables, lists, log
            // descendant, not child: Qt matches a scroll area's own bars as DESCENDANTS of it
            // (QAbstractScrollArea inserts them under an internal container), so `>` silently
            // matches nothing and the bar keeps the view colour above
            "QScrollArea QScrollBar, QScrollArea::corner { background: rgb(%10); }"  // page scroll
            "#sidebarList QScrollBar { background: rgb(%5); }"          // the sidebar pane
            "QScrollBar:vertical { width: 12px; }"
            "QScrollBar:horizontal { height: 12px; }"
            // The pill is the accent, in the same scheme-corrected shade the card titles use (%4):
            // a raw accent is a mid-tone and this app's surfaces are near it at both ends of the
            // light/dark axis, so the bare colour would sink into the track it sits on.
            "QScrollBar::handle { background: rgba(%4,150); border-radius: 3px; border: none; }"
            "QScrollBar::handle:vertical { min-height: 32px; margin: 2px 3px; }"
            "QScrollBar::handle:horizontal { min-width: 32px; margin: 3px 2px; }"
            "QScrollBar::handle:hover { background: rgba(%4,205); }"
            "QScrollBar::handle:pressed { background: rgb(%4); }"
            // arrows and the page-gap regions: no chrome of their own — transparent is right here,
            // because the bar's own track (above) is what they let through
            "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none;"
            "  background: transparent; }"
            "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")
            .arg(rgb(ac))                                        // %1 accent
            .arg(rgb(card))                                      // %2 card fill
            .arg(rgb(cardEdge))                                  // %3 card edge
            .arg(rgb(title))                                     // %4 card title
            .arg(rgb(side))                                      // %5 sidebar bg
            .arg(rgb(paneBg))                                    // %6 log pane bg
            .arg(dark ? QStringLiteral("#81c784") : QStringLiteral("#2e7d32"))   // %7 pill up
            .arg(dark ? QStringLiteral("#bdbdbd") : QStringLiteral("#616161"))   // %8 pill down
            .arg(rgb(onAccent))                                  // %9 on-accent text
            .arg(rgb(w)));                                       // %10 window (page scroll track)
}

void MainWindow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyAccentStyle();
        refreshImageGating();  // re-bakes the dimmed Source sidebar icon in the new accent
        update();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::setRunning(bool running) {
    running_ = running;
    // Deliberately NO auto-open of the log pane here: it opens only from its status-bar toggle.
    // A failure mid-chain still surfaces on its own — the failure dialog handles that.
    // Gate every source-tree operation while one is in flight: two concurrent Pull/Apply/Build
    // workers write the same tree and the same log and race on the same Progress row.
    for (QPushButton *b : sourceButtons_)
        if (b) b->setEnabled(!running);
    // Run-row Start/Connect/Stop: launching a second chain against the same target races the
    // first (a Connect retrying adb while a Deploy recreates the container).
    for (QPushButton *b : runButtons_)
        if (b) b->setEnabled(!running);
    if (busySpinner_) {
        busySpinner_->setVisible(running);
        if (running)
            spinnerTimer_->start();
        else
            spinnerTimer_->stop();
    }
    // A chain that just finished is exactly what starts or stops the container, so the data dir's
    // editability has to be re-asked here — this is the only moment it changes without the user
    // leaving the page.
    if (!running) refreshDataDirLock();
}

// A source pull runs in a detached worker; without this it reparents to init and keeps fetching
// after the window is gone (repo sync is resumable, so stopping it loses nothing but time).
void MainWindow::closeEvent(QCloseEvent *event) {
    const qint64 pid = pullChildPid_.load();
    if (pid > 0) {
        if (QMessageBox::question(
                this, QStringLiteral("Source operation running"),
                QStringLiteral("A source pull or build is still running. Close Remora and stop "
                               "it? (repo sync and ninja both resume where they left off.)"))
            != QMessageBox::Yes) {
            event->ignore();
            return;
        }
#ifdef Q_OS_UNIX
        ::killpg(static_cast<pid_t>(pid), SIGTERM);  // whole group: repo + workers + git fetches
        // wait (bounded) for the group to die, then sweep the half-checked-out worktrees it left —
        // otherwise the next sync fails with "Cannot initialize work tree" on those projects
        for (int i = 0; i < 25 && ::killpg(static_cast<pid_t>(pid), 0) == 0; ++i)
            QThread::msleep(200);
        if (::killpg(static_cast<pid_t>(pid), 0) == 0)
            ::killpg(static_cast<pid_t>(pid), SIGKILL);
#endif
        cleanupStaleWorktrees(sourceHostStr(), sourceTreePath());
    }
    QMainWindow::closeEvent(event);
}

// find-or-create the checklist row for a step, so we don't need the step list up front
static QTreeWidgetItem *rowFor(QTreeWidget *tree, const QString &step) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == step)
            return tree->topLevelItem(i);
    auto *item = new QTreeWidgetItem(tree);
    item->setData(0, Qt::UserRole, step);
    return item;
}

void MainWindow::onState(QString step, QString state, QString detail, QString stderrTail) {
    auto *item = rowFor(checklist_, step);
    QIcon icon;
    if (state == QLatin1String("running"))
        icon = themeIcon({"view-refresh", "chronometer"});
    else if (state == QLatin1String("ok"))
        icon = themeIcon({"dialog-ok-apply", "emblem-checked", "dialog-ok"});
    else if (state == QLatin1String("failed"))
        icon = themeIcon({"dialog-error", "data-error", "emblem-error"});
    else if (state == QLatin1String("skipped"))
        icon = themeIcon({"media-skip-forward", "go-next"});
    item->setIcon(0, icon);
    item->setText(0, step);
    item->setText(1, stderrTail.isEmpty() ? detail : detail + "  —  " + stderrTail.section('\n', -1));
    if (state == QStringLiteral("failed")) item->setForeground(0, QBrush(QColor("#c62828")));
    if (state == QStringLiteral("ok")) item->setForeground(0, QBrush(QColor("#2e7d32")));
    checklist_->scrollToItem(item);
    appendLog(QStringLiteral("%1 \u00b7 %2 \u00b7 %3").arg(step, state, detail));
    if (!stderrTail.isEmpty()) appendLog(stderrTail);
    if (state == QLatin1String("failed")) showFailure(step, detail, stderrTail);
}

// onState always lands on the GUI thread \u2014 workers reach it through sigState (queued), and the
// direct callers inside build/pull threads all go through QMetaObject::invokeMethod \u2014 so it is
// safe to open a dialog from here.
void MainWindow::showFailure(const QString &step, const QString &detail, const QString &stderrTail) {
    // REMORA_SHOT renders the window and quits on a timer with nobody at the keyboard — a modal
    // exec() there would spin a nested event loop that never gets an OK, hanging the capture.
    if (qEnvironmentVariableIsSet("REMORA_SHOT")) return;
    // A failing chain can report more than one dead step; the first is the one that matters and a
    // stack of modal dialogs would just be in the way.
    if (failureDialogOpen_) return;
    failureDialogOpen_ = true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Remora \u2014 %1 failed").arg(step));
    box.setText(QStringLiteral("<b>%1</b> failed.").arg(step.toHtmlEscaped()));
    box.setInformativeText(detail.isEmpty() ? QStringLiteral("No detail was reported.") : detail);
    // stderr goes behind "Show Details" rather than into the body: it is routinely dozens of lines
    // of git/repo output, and the useful part is usually the first failure, not the volume.
    if (!stderrTail.isEmpty()) box.setDetailedText(stderrTail);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
    failureDialogOpen_ = false;
}

// Terminal-style line into the log pane, timestamped and auto-scrolled. The pane never opens
// itself \u2014 the toolbar Log toggle is the only thing that shows it.
void MainWindow::appendLog(const QString &text) {
    if (!logView_) return;
    const QString stamp = QTime::currentTime().toString(QStringLiteral("hh:mm:ss"));
    for (const QString &ln : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        logView_->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, ln));
}

// Probe the device's Play Protect state into the Apps-page card. An unreachable device reads as
// UNKNOWN and keeps whatever id was last read: a stopped container has not become uncertified, and
// blanking the id would take away the one thing the user needs in order to go and register it.
void MainWindow::refreshCertification(bool recheck, bool restore) {
    const QString prof = selectedProfileName();
    if (prof.isEmpty()) return;
    // The worker takes the row's own config and name — `instance` keys the identity snapshot —
    // so a non-active profile is checked exactly like the active one, no switch needed.
    RemoraConfig pc;
    if (prof == instance_) {
        applyToConfig();  // the active profile's widgets may be ahead of remorarc
        pc = cfg_;
    } else {
        pc = loadInstance(path_, prof);
    }
    ProfileCertState busy;
    busy.cell = recheck ? QStringLiteral("checkin…") : QStringLiteral("checking…");
    busy.tip = recheck ? QStringLiteral("forcing a GMS checkin, then re-reading…")
                       : QStringLiteral("reading the verdict off the device…");
    busy.androidId = profileCert_.value(prof).androidId;
    busy.color = QColor(0x99, 0x99, 0x99);
    profileCert_.insert(prof, busy);
    if (QTreeWidgetItem *item = profileItemFor(prof)) applyCertCell(item);
    auto *w = new CertificationWorker(inferBackend(pc), pc, recheck, prof, restore, this);
    connect(w, &CertificationWorker::sigDone, this,
            [this, prof](bool known, bool certified, QString androidId, QString snapshotNote,
                         QString restoreDetail, QString pendingNote) {
                ProfileCertState st;
                st.androidId =
                    androidId.isEmpty() ? profileCert_.value(prof).androidId : androidId;
                if (!known) {
                    st.cell = QStringLiteral("unknown");
                    st.color = QColor(0x99, 0x99, 0x99);
                    st.tip = QStringLiteral(
                        "unknown — the device is not running, or is unreachable");
                } else if (!pendingNote.isEmpty()) {
                    // The third state renders amber, and wins over the true/false pair: a young
                    // identity with no posted verdict is neither certified nor uncertified, and
                    // painting it red would send the user to re-register an id that may be
                    // minutes from passing (bd remora-4ei.90).
                    st.cell = QStringLiteral("pending");
                    st.color = QColor(0xb2, 0x6a, 0x00);
                    st.tip = pendingNote;
                } else if (certified) {
                    st.cell = QStringLiteral("certified");
                    st.color = QColor(0x2e, 0x7d, 0x32);
                    st.tip = QStringLiteral("certified — Google sign-in is not blocked");
                } else {
                    st.cell = QStringLiteral("NOT certified");
                    st.color = QColor(0xc6, 0x28, 0x28);
                    st.tip = QStringLiteral(
                        "NOT certified — Google sign-in is blocked, so Play Store login will "
                        "fail.\nRegister… puts the id on the clipboard and opens Google's form; "
                        "Re-check afterwards.");
                }
                if (!restoreDetail.isEmpty()) st.tip = restoreDetail + QLatin1Char('\n') + st.tip;
                // The snapshot note rides every verdict's tooltip: it answers "would a /data
                // rebuild cost me a re-registration, or just a Restore ID?", which is worth
                // knowing most while things are FINE.
                if (!snapshotNote.isEmpty())
                    st.tip += QStringLiteral("\n\nSaved ID: ") + snapshotNote;
                if (!st.androidId.isEmpty())
                    st.tip += QStringLiteral("\nandroid_id: ") + st.androidId +
                              QStringLiteral("  (Copy ID puts it on the clipboard)");
                profileCert_.insert(prof, st);
                if (QTreeWidgetItem *item = profileItemFor(prof)) applyCertCell(item);
            });
    connect(w, &CertificationWorker::finished, w, &QObject::deleteLater);
    w->start();
}



void MainWindow::rebuildSourcesTree() {
    QSignalBlocker block(sourcesTree_);
    sourcesTree_->clear();
    for (const ImageSource &s : sources_) {
        auto *item = new QTreeWidgetItem(sourcesTree_);
        item->setText(0, s.name);
        item->setCheckState(0, s.enabled ? Qt::Checked : Qt::Unchecked);
        // location first — it is what the library lists; pinned tags trail it
        QStringList desc;
        if (!s.location.isEmpty()) desc << s.location;
        if (!s.tags.isEmpty()) desc << s.tags.join(QStringLiteral("  "));
        item->setText(1, desc.join(QStringLiteral("  ·  ")));
        item->setData(0, Qt::UserRole, s.name);
    }
}

void MainWindow::onSourceToggled() {
    for (int i = 0; i < sourcesTree_->topLevelItemCount(); ++i) {
        const auto *item = sourcesTree_->topLevelItem(i);
        const QString name = item->data(0, Qt::UserRole).toString();
        for (ImageSource &s : sources_)
            if (s.name == name) s.enabled = item->checkState(0) == Qt::Checked;
    }
    saveImageSources(path_, sources_);
    recompute();
}

void MainWindow::onAddSource() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Add image source"),
                                               QStringLiteral("Source name:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    const QString location = QInputDialog::getText(
        this, QStringLiteral("Add image source"),
        QStringLiteral("Location — https:// mirror, ssh user@host:/dir, or a local directory\n"
                       "(blank = a plain tag list; a mirror serves <url>/index.txt, one\n"
                       "'<tag> [<file> [<bytes>]]' per line):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    const QString tags = QInputDialog::getText(
        this, QStringLiteral("Add image source"),
        QStringLiteral("Pinned image tags (space-separated, e.g. repo/image:tag — optional when "
                       "a location is set):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    ImageSource src;
    src.name = name.trimmed();
    src.location = location.trimmed();
    src.tags = tags.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (src.location.isEmpty() && src.tags.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("a source needs a location or at least one tag — nothing added"), 5000);
        return;
    }
    sources_ << src;
    saveImageSources(path_, sources_);
    rebuildSourcesTree();
    onRefreshArchives();  // a new location should appear in the library right away
}

void MainWindow::onRemoveSource() {
    auto *item = sourcesTree_->currentItem();
    if (!item) return;
    const QString name = item->data(0, Qt::UserRole).toString();
    for (int i = 0; i < sources_.size(); ++i) {
        if (sources_[i].name == name) {
            sources_.removeAt(i);
            break;
        }
    }
    saveImageSources(path_, sources_);
    rebuildSourcesTree();
    recompute();
}

static QString humanSize(qint64 bytes) {
    if (bytes >= 1LL << 30) return QStringLiteral("%1 GiB").arg(bytes / double(1LL << 30), 0, 'f', 1);
    if (bytes >= 1LL << 20) return QStringLiteral("%1 MiB").arg(bytes / double(1LL << 20), 0, 'f', 1);
    return QStringLiteral("%1 KiB").arg(bytes / double(1LL << 10), 0, 'f', 1);
}

// The build-side paths (docker context, output dir, source tree) all live on the build host —
// so does the GUI when build_host is unset. Empty = operate locally, which is the default and
// the only thing the UI offers: both hosts read remorarc-only keys, their widgets are retired.
QString MainWindow::buildHostStr() const {
    return cfg_.image.buildHost.value_or(QString()).trimmed();
}
QString MainWindow::sourceHostStr() const {
    return cfg_.image.sourceHost.value_or(QString()).trimmed();
}
QString MainWindow::buildDirPath() const {
    // From the config, not a widget — the field was retired; build_dir= in remorarc still wins.
    const QString t = cfg_.image.buildDir.value_or(QString()).trimmed();
    return t.isEmpty() ? QStringLiteral("~/.cache/remora/image-build") : t;
}
QString MainWindow::archivesDir() const {
    const QString t = outputDirEdit_ ? outputDirEdit_->text().trimmed() : QString();
    return t.isEmpty() ? QStringLiteral("~/remora-images") : t;
}
// The manifest this profile pulls from: remorarc's source_repo when set, else the one that goes
// with the source kind. There is no widget — see the note where the row used to be.
QString MainWindow::sourceRepoStr() const {
    if (cfg_.image.sourceRepo.has_value() && !cfg_.image.sourceRepo->trimmed().isEmpty())
        return cfg_.image.sourceRepo->trimmed();
    const bool lineage =
        !(sourceKindCombo_ && sourceKindCombo_->currentData().toString() == QLatin1String("aosp"));
    return defaultSourceRepo(lineage);
}

QString MainWindow::sourceTreePath() const {
    const QString t = sourceTreeEdit_ ? sourceTreeEdit_->text().trimmed() : QString();
    return t.isEmpty() ? QStringLiteral("~/remora-android") : t;
}

// A library row's tag column: the custom name first when one is set (display only — the raw tag
// stays in the item data).
static QString imageLabel(const QString &settings, const QString &tag) {
    const QString n = imageCustomName(settings, tag);
    return n.isEmpty() ? tag : n + QStringLiteral("  ·  ") + tag;
}

void MainWindow::onRefreshArchives() {
    if (!archivesTree_ || archiveScanBusy_) return;
    archiveScanBusy_ = true;
    const QString dir = archivesDir();
    const QString settings = path_;
    archivesTree_->clear();
    auto *busy = new QTreeWidgetItem(archivesTree_);
    busy->setText(0, QStringLiteral("scanning %1 …").arg(dir));
    busy->setFlags(Qt::NoItemFlags);
    // Reading a tag streams the whole multi-GB tar on a cache miss (manifest.json is at the end
    // of a docker-save archive) — do it off the GUI thread; results marshal back queued.
    const QString host = buildHostStr();
    const QString shost = sourceHostStr();
    const QString bdir = buildDirPath();
    const QString streeOut = sourceTreePath() + QStringLiteral("/out");
    // Snapshot on the GUI thread — the worker must not read sources_ while the card edits it.
    const QList<ImageSource> srcs = sources_;
    // THREE DELIVERIES, NOT ONE. Everything below used to land in a single invoke at
    // the end of the worker, so the list of images waited on every mirror's index fetch AND on a
    // `du -sb` of the source tree's out/ (2.5 s warm over a 285 GB AOSP tree, far longer cold) AND
    // a `docker system df` (1.5 s) — for two figures that decorate a status line. "scanning …"
    // sat on the library for 4-15 s on every refresh, and Promote/Delete/the Active image combo
    // were unavailable for exactly that long. Now: the listing the moment the local scans are back;
    // the source rows as each mirror answers; the sizes last. A generation number is what lets a
    // Refresh mid-scan start over: the old worker's later stages see the number moved on and drop
    // their results instead of appending mirror rows to a tree a newer scan just filled.
    const int gen = ++archiveScanGen_;
    auto *thread = QThread::create([this, gen, dir, settings, host, shost, bdir, streeOut, srcs] {
        const QList<ImageArchive> found = scanImageArchives(dir, settings, host);
        QString dockerError;
        QList<DockerImage> images = scanDockerImages(host, &dockerError);
        for (DockerImage &im : images) im.hostLabel = host;
        // Source builds assemble their docker image on the SOURCE host, which is often a
        // different (beefier) machine than the overlay build host. Scan it too, so a fresh
        // source-built image actually shows up — deduped when the hosts coincide.
        if (shost != host) {
            QList<DockerImage> srcImages = scanDockerImages(shost, nullptr);
            for (DockerImage &im : srcImages) {
                bool dup = false;
                for (const DockerImage &e : images)
                    if (e.tag == im.tag && e.hostLabel == shost) { dup = true; break; }
                if (!dup) { im.hostLabel = shost; images << im; }
            }
        }
        // ── 1. The listing ──
        QMetaObject::invokeMethod(
            this,
            [this, gen, settings, found, images, host, dockerError] {
                if (gen != archiveScanGen_) return;
                archiveScanBusy_ = false;
                archivesTree_->clear();
                // Say whose disk this is — profiles can point at different build hosts, so the
                // same panel legitimately shows different images per profile.
                archivesTree_->headerItem()->setText(
                    0, QStringLiteral("archive — on %1")
                           .arg(host.isEmpty() ? QStringLiteral("this machine") : host));
                qint64 total = 0;
                for (const ImageArchive &a : found) {
                    auto *item = new QTreeWidgetItem(archivesTree_);
                    item->setText(0, QFileInfo(a.path).fileName());
                    item->setText(1, a.tag.isEmpty() ? QStringLiteral("(unreadable manifest)")
                                                     : imageLabel(settings, a.tag));
                    const QString built =
                        a.mtime > 0 ? QDateTime::fromSecsSinceEpoch(a.mtime).toString(
                                          QStringLiteral("yyyy-MM-dd HH:mm"))
                                    : QString();
                    item->setText(2, built);
                    item->setText(3, humanSize(a.size));
                    item->setData(0, Qt::UserRole, a.path);
                    total += a.size;
                }
                for (const DockerImage &im : images) {
                    auto *item = new QTreeWidgetItem(archivesTree_);
                    item->setText(0, im.hostLabel.isEmpty()
                                         ? QStringLiteral("(docker image · local)")
                                         : QStringLiteral("(docker image · %1)").arg(im.hostLabel));
                    item->setText(1, imageLabel(settings, im.tag));
                    item->setText(2, im.created);
                    item->setText(3, im.size);
                    item->setData(0, Qt::UserRole, QString());       // no file
                    item->setData(0, Qt::UserRole + 1, im.tag);      // docker rmi target
                    item->setData(0, Qt::UserRole + 2, im.hostLabel);  // where it lives (push src)
                }
                const QString hostName = host.isEmpty() ? QStringLiteral("this host") : host;
                if (!dockerError.isEmpty()) {
                    // Docker's images are not "none", they are unlisted — say which, and why.
                    auto *item = new QTreeWidgetItem(archivesTree_);
                    item->setText(0, QStringLiteral("(docker is not reachable on %1 — its images "
                                                    "are not listed)").arg(hostName));
                    item->setText(1, dockerError);
                    item->setToolTip(1, dockerError);
                    item->setFlags(Qt::NoItemFlags);
                } else if (archivesTree_->topLevelItemCount() == 0) {
                    auto *item = new QTreeWidgetItem(archivesTree_);
                    item->setText(0, QStringLiteral("(no archives or images on %1)").arg(hostName));
                    item->setFlags(Qt::NoItemFlags);
                }
                archivesTree_->headerItem()->setText(
                    3, total > 0 ? QStringLiteral("size (archives %1)").arg(humanSize(total))
                                 : QStringLiteral("size"));
                // The recipe label detects an existing <tag>-src build to say "promote it" instead
                // of "needs a rebuild" — re-run it now that the image list is loaded (the scan is
                // async, so the first recompute() often ran before it finished).
                recompute();
            },
            Qt::QueuedConnection);
        // ── 2. Location-backed sources (bd remora-2ylt) ──
        // Every enabled one, listed so the library can show what it offers. After the listing on
        // purpose, and each delivered as it answers: a dead mirror costs its own timeout and
        // nothing else — not the local listing, not the next mirror's rows.
        QSet<QString> here;
        for (const ImageArchive &a : found)
            if (!a.tag.isEmpty()) here << a.tag;
        for (const DockerImage &im : images) here << im.tag;
        for (const ImageSource &src : srcs) {
            if (!src.enabled || src.location.isEmpty()) continue;
            const QList<SourceImage> offered = scanSourceLocation(src, settings);
            QMetaObject::invokeMethod(
                this,
                [this, gen, settings, here, offered] {
                    if (gen != archiveScanGen_) return;
                    // The "(no archives or images …)" row gives way to the first real one.
                    if (!offered.isEmpty() && archivesTree_->topLevelItemCount() == 1 &&
                        archivesTree_->topLevelItem(0)->flags() == Qt::NoItemFlags)
                        delete archivesTree_->takeTopLevelItem(0);
                    // Marked by whether the tag is already HERE — a docker image or archive the
                    // scans above found — or would need a fetch. A tag appearing twice (its local
                    // row and its source row saying "local") is the point: it says the mirror
                    // carries what this machine runs. Selectable for Fetch (bd remora-pht9);
                    // delete/rename/push ignore these rows because their own data roles (path,
                    // docker tag) stay empty here.
                    for (const SourceImage &si : offered) {
                        auto *item = new QTreeWidgetItem(archivesTree_);
                        item->setText(0, QStringLiteral("(source · %1)").arg(si.source));
                        item->setText(1, si.tag.isEmpty()
                                             ? si.file + QStringLiteral("  (unreadable manifest)")
                                             : imageLabel(settings, si.tag));
                        const bool local = !si.tag.isEmpty() && here.contains(si.tag);
                        item->setText(2, local ? QStringLiteral("local ✓")
                                               : QStringLiteral("needs fetch ↓"));
                        if (si.size > 0) item->setText(3, humanSize(si.size));
                        item->setData(0, Qt::UserRole + 3, si.tag);
                        item->setData(0, Qt::UserRole + 4, si.file);
                        item->setData(0, Qt::UserRole + 5, si.location);
                        item->setData(0, Qt::UserRole + 6, si.source);
                        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                    }
                },
                Qt::QueuedConnection);
        }
        // ── 3. The figures: what the build dirs weigh, what a Prune would reclaim ──
        const qint64 buildDirSize = dirSizeBytes(host, bdir);
        const qint64 srcOutSize = dirSizeBytes(shost, streeOut);
        // Dangling images, across the same hosts prune runs on.
        PrunableStat prunable = scanPrunable(host);
        if (shost != host) {
            const PrunableStat s2 = scanPrunable(shost);
            prunable.count += s2.count;
            prunable.bytes += s2.bytes;
        }
        QMetaObject::invokeMethod(
            this,
            [this, gen, buildDirSize, srcOutSize, prunable] {
                if (gen != archiveScanGen_) return;
                sourceOutSizeBytes_ = srcOutSize;  // the build preflight reads this, not a du
                if (prunableLabel_)
                    prunableLabel_->setText(
                        prunable.count == 0
                            ? QStringLiteral("no orphaned layers")
                            : QStringLiteral("%1 orphaned · ~%2")
                                  .arg(prunable.count)
                                  .arg(humanSize(prunable.bytes)));
                if (buildDirSizeLabel_)
                    buildDirSizeLabel_->setText(
                        QStringLiteral("context: %1")
                            .arg(buildDirSize < 0 ? QStringLiteral("—")
                                 : buildDirSize == 0 ? QStringLiteral("empty")
                                                     : humanSize(buildDirSize)));
                if (sourceOutSizeLabel_)
                    sourceOutSizeLabel_->setText(
                        QStringLiteral("build output: %1")
                            .arg(srcOutSize < 0 ? QStringLiteral("(none)") : humanSize(srcOutSize)));
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::onRenameArchive() {
    auto *item = archivesTree_ ? archivesTree_->currentItem() : nullptr;
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) {  // a docker image row — set/clear its display-only custom name
        const QString tag = item->data(0, Qt::UserRole + 1).toString();
        if (tag.isEmpty()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, QStringLiteral("Name image"),
            QStringLiteral("A name to identify this image in the list (blank = none):\n\nTag: %1")
                .arg(tag),
            QLineEdit::Normal, imageCustomName(path_, tag), &ok);
        if (!ok) return;
        setImageCustomName(path_, tag, name);
        onRefreshArchives();
        return;
    }
    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, QStringLiteral("Rename archive"), QStringLiteral("New file name:"),
        QLineEdit::Normal, QFileInfo(path).fileName(), &ok);
    if (!ok || newName.trimmed().isEmpty()) return;
    QString name = newName.trimmed();
    if (!name.endsWith(QLatin1String(".tar")))  // the scanner only lists *.tar — keep it findable
        name += QLatin1String(".tar");
    const QString dst = QFileInfo(path).absolutePath() + QLatin1Char('/') + name;
    if (runShell(buildHostStr(), QStringLiteral("mv -n %1 %2 && echo ok").arg(path, dst), 30000)
            .trimmed()
        != QLatin1String("ok"))
        QMessageBox::warning(this, QStringLiteral("Rename failed"),
                             QStringLiteral("Could not rename to %1").arg(dst));
    onRefreshArchives();
}

void MainWindow::onDeleteArchive() {
    auto *item = archivesTree_ ? archivesTree_->currentItem() : nullptr;
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const QString imageTag = item->data(0, Qt::UserRole + 1).toString();
    // archives live on the build host; docker images live on whatever host the row was scanned
    // from (build host OR source host) — stored in UserRole+2. rmi must target that host, not
    // always the build host, or a local image "deletes" against the wrong machine and survives.
    const bool isDockerImage = path.isEmpty() && !imageTag.isEmpty();
    const QString host =
        isDockerImage ? item->data(0, Qt::UserRole + 2).toString() : buildHostStr();
    if (!path.isEmpty()) {  // a .tar archive on disk
        if (QMessageBox::question(this, QStringLiteral("Delete archive"),
                                  QStringLiteral("Delete %1 (%2)?")
                                      .arg(QFileInfo(path).fileName(), item->text(3)))
            != QMessageBox::Yes)
            return;
        // Checked, like the rename beside it: an rm that fails (permissions, a host that dropped
        // off ssh) used to pass silently, and the only sign was the row still being there after
        // the refresh — which reads as "the delete is slow", not "the delete failed".
        const ShellResult rm =
            runShellRc(host, QStringLiteral("rm -f %1 2>&1 && echo ok").arg(path), 30000);
        if (rm.rc != 0 || !rm.out.trimmed().endsWith(QLatin1String("ok")))
            QMessageBox::warning(this, QStringLiteral("Delete failed"),
                                 rm.out.trimmed().isEmpty() ? QStringLiteral("rm exited %1").arg(rm.rc)
                                                            : rm.out.trimmed());
    } else if (!imageTag.isEmpty()) {  // a docker image on the build/source host
        const QString where = host.isEmpty() ? QStringLiteral("local") : host;
        if (QMessageBox::question(this, QStringLiteral("Remove docker image"),
                                  QStringLiteral("docker rmi %1 on %2 (%3)?")
                                      .arg(imageTag, where, item->text(3)))
            != QMessageBox::Yes)
            return;
        QString out = runShell(host, QStringLiteral("docker rmi %1 2>&1").arg(imageTag), 30000);
        const bool conflict = out.contains(QLatin1String("conflict"))
                              || out.contains(QLatin1String("must be forced"))
                              || out.contains(QLatin1String("cannot"));
        if (conflict) {
            // A plain rmi refuses when ANY container references the image — even stopped ones
            // (`rmi -f` would only untag, leaving the layers pinned by those containers). Split
            // the cases: a RUNNING container means the user must stop the deployment first; only
            // STOPPED leftovers can be cleared, so offer to remove them and the image together.
            const QString running =
                runShell(host,
                         QStringLiteral("docker ps -q --filter ancestor=%1").arg(imageTag), 30000)
                    .trimmed();
            if (!running.isEmpty()) {
                // Actionable instead of a dead-end: offer to stop+remove the deployment and delete
                // in one step (docker rm -f stops then removes), so the user isn't stuck in a
                // stop → wait → retry loop that races the "stopping…" transition.
                if (QMessageBox::warning(
                        this, QStringLiteral("Image is in use"),
                        QStringLiteral(
                            "A running container is using %1. Stop and remove that deployment, then "
                            "delete the image?\n\nThis ends the current session; your Android data is "
                            "safe on the mounted volume (not in the image), and `remora up` recreates "
                            "the container.").arg(imageTag),
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    == QMessageBox::Yes) {
                    out = runShell(
                        host,
                        QStringLiteral("cids=$(docker ps -aq --filter ancestor=%1); "
                                       "[ -n \"$cids\" ] && docker rm -f $cids >/dev/null 2>&1; "
                                       "docker rmi -f %1 2>&1").arg(imageTag),
                        30000);
                    if (out.contains(QLatin1String("conflict"))
                        || out.contains(QLatin1String("cannot")))
                        QMessageBox::warning(this, QStringLiteral("Remove failed"), out.trimmed());
                }
            } else {
                const QString stopped =
                    runShell(host,
                             QStringLiteral("docker ps -aq --filter ancestor=%1").arg(imageTag),
                             30000).trimmed();
                const int n = stopped.isEmpty()
                                  ? 0
                                  : int(stopped.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size());
                const QString why =
                    n > 0 ? QStringLiteral("%1 is pinned by %2 stopped container(s) — no deployment "
                                           "is running. Remove them and delete the image?\n\n(Your "
                                           "Android data is on the mounted volume, not the image; "
                                           "`remora up` recreates the container.)").arg(imageTag).arg(n)
                          : QStringLiteral("%1 is referenced by a child image:\n\n%2\n\nForce "
                                           "remove?").arg(imageTag, out.trimmed());
                if (QMessageBox::warning(this, QStringLiteral("Remove image?"), why,
                                         QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    == QMessageBox::Yes) {
                    // remove the stopped containers (if any), then the image
                    out = runShell(
                        host,
                        QStringLiteral("cids=$(docker ps -aq --filter ancestor=%1); "
                                       "[ -n \"$cids\" ] && docker rm $cids >/dev/null 2>&1; "
                                       "docker rmi -f %1 2>&1").arg(imageTag),
                        30000);
                    if (out.contains(QLatin1String("conflict"))
                        || out.contains(QLatin1String("cannot")))
                        QMessageBox::warning(this, QStringLiteral("Remove failed"), out.trimmed());
                }
            }
        } else if (out.contains(QLatin1String("No such image"))) {
            QMessageBox::warning(this, QStringLiteral("docker rmi failed"),
                                 out.trimmed().isEmpty() ? QStringLiteral("no output")
                                                         : out.trimmed());
        }
    } else {
        return;
    }
    onRefreshArchives();
}

// Promote a fresh source build to its active tag: RENAME `<base>-src` → `<base>` (move the tag, not
// copy — so the image isn't left showing under two tags) on the host where the image lives, so the
// next Connect/Up deploys the new build. This is the reproducible "activate the build" step (a source
// build lands as -src precisely so it never clobbers the active/golden tag until you choose to). The
// old <base> image stays pinned by the running container until it recreates onto the new tag (smartRun
// detects the stale image on the next Up); Prune reclaims it afterward.
// The tag this profile would deploy with NO pin: the feature list resolved through the same
// Resolver the deploy uses, rather than a formula copied here. Live features_ and not
// cfg_.image.features, since the checkboxes may have moved since the last save.
QString MainWindow::derivedImageTag() const {
    RemoraConfig probe = cfg_;
    probe.image.imageTag.reset();
    probe.image.features = QStringList(features_.begin(), features_.end());
    probe.image.features.sort();
    return resolve(probe, currentBackend()).imageTag;
}

// Set or clear image_tag= from the Active image combo. No confirmation dialog: the control states
// what it does, one click undoes it, and nothing is destroyed — it redirects the next deploy. What
// it must NOT do is go through applyToConfig, which deliberately never writes image_tag (a pin is
// not a widget value to be read back off the page), so the pin is set on cfg_ here and autosave()
// persists it on the way past.
void MainWindow::onActiveImageChanged() {
    if (!activeImageCombo_ || suppressAutosave_) return;
    const QString tag = activeImageCombo_->currentData().toString();
    if (cfg_.image.imageTag.value_or(QString()) == tag) return;
    if (tag.isEmpty()) cfg_.image.imageTag.reset();  // wStr() removes the key on a nullopt
    else cfg_.image.imageTag = tag;
    autosave();
    statusBar()->showMessage(
        tag.isEmpty()
            ? QStringLiteral("unpinned — this profile resolves to %1; press Connect to deploy it")
                  .arg(derivedImageTag())
            : QStringLiteral("pinned %1 — press Connect to deploy it").arg(tag),
        8000);
    recompute();  // relabels entry 0 and re-runs the codec gate against the new image
}

void MainWindow::onPromoteImage() {
    auto *item = archivesTree_ ? archivesTree_->currentItem() : nullptr;
    if (!item) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const QString tag = item->data(0, Qt::UserRole + 1).toString();
    if (!path.isEmpty() || tag.isEmpty()) {  // must be a docker image row, not a .tar archive
        QMessageBox::information(this, QStringLiteral("Promote to active"),
                                 QStringLiteral("Select a source-built docker image row (its tag "
                                                "ends in -src)."));
        return;
    }
    if (!tag.endsWith(QLatin1String("-src"))) {
        QMessageBox::information(
            this, QStringLiteral("Nothing to promote"),
            QStringLiteral("%1 is already an active tag, not a -src source build. Promote retags a "
                           "fresh -src build to its active tag so a deploy picks it up.").arg(tag));
        return;
    }
    const QString host = item->data(0, Qt::UserRole + 2).toString();
    const QString base = tag.left(tag.size() - 4);  // strip the "-src" suffix
    const QString where = host.isEmpty() ? QStringLiteral("local") : host;
    if (QMessageBox::question(
            this, QStringLiteral("Promote to active"),
            QStringLiteral(
                "Rename %1 → %2 on %3?\n\nThis makes the new build the active image; the next Connect "
                "recreates the container onto it (your Android data lives on the mounted volume, not "
                "the image). The old %2 image is then freed — use Prune to reclaim its layers.")
                .arg(tag, base, where),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    // MOVE the tag, don't copy it: add <base> then drop the now-redundant <base>-src, so the list
    // doesn't show the same image twice. `docker rmi <src>` only untags (the image survives via
    // <base>). If <base> already pointed at a different (old) image, tagging reassigns it — that old
    // image is freed once the container recreates onto the new one (Prune reclaims it).
    // "ok" follows the TAG, not the line: this read `… && docker rmi …; echo ok`, so the echo ran
    // whatever docker tag said, and a failed tag ("No such image", a daemon that was down) came
    // back as "<error>\nok" — endsWith("ok") — and was announced as promoted. The untag is
    // cleanup and may fail on its own without unmaking the promotion, so it stays inside the
    // success branch but does not gate the signal. The trailing `|| true` keeps the exit code zero
    // on failure so runShell hands back the daemon's message for the dialog instead of nothing.
    const QString out = runShell(
        host,
        QStringLiteral(
            "docker tag %1 %2 2>&1 && { docker rmi %1 >/dev/null 2>&1; echo ok; } || true")
            .arg(tag, base),
        30000);
    if (out.trimmed().endsWith(QLatin1String("ok")))
        statusBar()->showMessage(
            QStringLiteral("promoted %1 → %2 — press Connect to deploy the new build").arg(tag, base),
            8000);
    else
        QMessageBox::warning(this, QStringLiteral("Promote failed"),
                             out.trimmed().isEmpty() ? QStringLiteral("no output") : out.trimmed());
    onRefreshArchives();
}

// Reclaim dangling/orphaned docker layers (e.g. what a deleted image leaves behind — rmi only
// untags; the layers linger until pruned) on both the build host and the source host.
void MainWindow::onPruneImages() {
    const QString bhost = buildHostStr();
    const QString shost = sourceHostStr();
    QStringList hosts{bhost};
    if (shost != bhost) hosts << shost;
    // Tell the operator up front what pruning will actually free — no more reclaiming blind.
    PrunableStat prunable;
    for (const QString &h : hosts) {
        const PrunableStat s = scanPrunable(h);
        prunable.count += s.count;
        prunable.bytes += s.bytes;
    }
    if (prunable.count == 0) {
        statusBar()->showMessage(
            QStringLiteral("nothing to prune — no orphaned (dangling) images on %1")
                .arg(hosts.join(QStringLiteral(", "))),
            6000);
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Prune orphaned layers"),
            QStringLiteral("Remove %1 dangling docker image(s) (untagged leftovers from deleted/"
                           "rebuilt images), reclaiming about %2 on the build and source hosts? "
                           "Tagged images are kept; only orphaned layers are reclaimed.")
                .arg(prunable.count)
                .arg(humanSize(prunable.bytes)))
        != QMessageBox::Yes)
        return;
    statusBar()->showMessage(QStringLiteral("pruning orphaned layers…"));
    auto *thread = QThread::create([this, hosts] {
        QString summary;
        for (const QString &h : hosts) {
            const QString out = runShell(h, QStringLiteral("docker image prune -f 2>&1"), 120000);
            QString reclaimed = QStringLiteral("0B");
            for (const QString &ln : out.split(QLatin1Char('\n')))
                if (ln.contains(QLatin1String("reclaimed")))
                    reclaimed = ln.section(QLatin1Char(':'), -1).trimmed();
            summary += QStringLiteral("%1: %2\n")
                           .arg(h.isEmpty() ? QStringLiteral("local") : h, reclaimed);
        }
        QMetaObject::invokeMethod(this, [this, summary] {
            statusBar()->showMessage(
                QStringLiteral("pruned — %1").arg(summary.trimmed().replace(QLatin1Char('\n'),
                                                                            QStringLiteral(", "))),
                8000);
            onRefreshArchives();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::onCleanBuildDir() {
    const QString host = buildHostStr();
    const QString dir = buildDirPath();
    reclaimDir(host, dir, QStringLiteral("Clean docker context"),
               QStringLiteral("Remove the docker build context at %1 (%2)? Staged payloads (e.g. "
                              "wv/vendor) will need re-staging before the next build."),
               QStringLiteral("docker context %1 is already empty").arg(dir));
}

// Measure, ask, delete — with the GUI thread free throughout. Both Clean buttons used
// to run their `du` AND their `rm -rf` inline in the click handler, under runShell's nested event
// loop: the window repainted but took no input, with nothing on screen saying why, for as long as
// the two commands took. The `du` alone is 2.5 s warm over the 285 GB out/ tree (bd remora-s6ma)
// and the delete of its 1.59 million entries is minutes, not seconds — longer than the 120 s the
// handler allowed, at which point runShell gave up, the QProcess destructor KILLED the rm
// half-way, and the status bar announced "removed … (265.8 GiB reclaimed)" regardless, because
// nothing looked at the result. Now each step runs in a worker, the delete has no timeout, and
// "removed" is said only when rm says so. running_ gates the source buttons meanwhile, so a
// build cannot start into a tree that is being deleted.
void MainWindow::reclaimDir(const QString &host, const QString &dir, const QString &title,
                            const QString &question, const QString &emptyMsg) {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before cleaning"), 5000);
        return;
    }
    const QString where = host.isEmpty() ? dir : host + QLatin1Char(':') + dir;
    setRunning(true);
    statusBar()->showMessage(QStringLiteral("measuring %1 …").arg(where));
    auto *measure = QThread::create([this, host, dir, where, title, question, emptyMsg] {
        const qint64 total = dirSizeBytes(host, dir);
        QMetaObject::invokeMethod(
            this,
            [this, host, dir, where, title, question, emptyMsg, total] {
                if (total <= 0) {
                    setRunning(false);
                    statusBar()->showMessage(emptyMsg, 4000);
                    return;
                }
                // One .arg call for both fields: a path containing "%2" must not be rescanned.
                if (QMessageBox::question(this, title, question.arg(where, humanSize(total)))
                    != QMessageBox::Yes) {
                    setRunning(false);
                    statusBar()->clearMessage();
                    return;
                }
                statusBar()->showMessage(
                    QStringLiteral("removing %1 (%2) …").arg(where, humanSize(total)));
                auto *remove = QThread::create([this, host, dir, where, total] {
                    // No timeout: a delete this size is done when it is done. The trailing echo
                    // is the only success signal — runShell returns empty on any failure.
                    const bool ok =
                        runShell(host, QStringLiteral("rm -rf %1 && echo ok").arg(dir), -1)
                            .trimmed() == QLatin1String("ok");
                    QMetaObject::invokeMethod(
                        this,
                        [this, where, total, ok] {
                            setRunning(false);
                            statusBar()->showMessage(
                                ok ? QStringLiteral("removed %1 (%2 reclaimed)")
                                         .arg(where, humanSize(total))
                                   : QStringLiteral("removing %1 did not finish — part of it may "
                                                    "be gone; the size readout below shows what "
                                                    "is left")
                                         .arg(where),
                                ok ? 6000 : 10000);
                            onRefreshArchives();
                        },
                        Qt::QueuedConnection);
                });
                connect(remove, &QThread::finished, remove, &QObject::deleteLater);
                remove->start();
            },
            Qt::QueuedConnection);
    });
    connect(measure, &QThread::finished, measure, &QObject::deleteLater);
    measure->start();
}

// Populate the Source-ref dropdown from the selected repo's branches/tags (git ls-remote on the
// source host, async — AOSP manifests have thousands of tags, capped to the newest ~200).
void MainWindow::onRefreshSourceRefs() {
    const QString repo = sourceRepoStr();
    if (repo.isEmpty() || repo == lastRefRepo_) return;
    lastRefRepo_ = repo;
    const QString host = sourceHostStr();
    auto *thread = QThread::create([this, host, repo] {
        const QString listing = runShell(
            host,
            QStringLiteral("git ls-remote --heads --tags ") + shellQuote(repo) +
                QStringLiteral(" 2>/dev/null | awk '{print $2}' "
                               "| sed 's|refs/heads/||; s|refs/tags/||; s|\\^{}$||' "
                               "| sort -uVr | head -200"),
            60000);
        const QStringList refs = listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QMetaObject::invokeMethod(
            this,
            [this, refs] {
                sourceRefsAll_ = refs;
                populateSourceRefCombo();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// Show only the refs that build the selected Android version (lineage-24 ↔ A17, android-17.* /
// android17-*) plus unversioned branches (master/main). A typed custom ref survives
// unless it names a *different* version — then it flips to the matching Lineage branch.
// Show only the patches that have something to apply on the selected Android version
// (bd remora-82c.3). Applicability is derived from the ASSETS — the same <dir>-a<N> / <dir> pair
// the apply loop resolves — so this widens automatically as per-version series are added, rather
// than tracking a hand-maintained list that would go stale and shrink what is offerable.
//
// A hidden patch is NOT unticked. Its remorarc entry is a pin, so clearing it here would silently
// drop the user's choice the moment they glanced at another version, and switching back would not
// restore it. Leaving the value alone costs nothing on the build side: a series absorbed upstream
// carries SKIP and no-ops, and one with no directory at all hard-fails the apply with the file
// named. Neither can produce the silent green build that bd remora-82c.1 is about.
bool MainWindow::venusSelected() const {
    if (!gpuDriverCombo_) return cfg_.gpu.venus.value_or(false);
    // Same read as the write path in applyToConfig: the combo is editable, so item DATA only
    // counts while the text still matches that item — typed text is a driver name, never the
    // Venus sentinel.
    const int i = gpuDriverCombo_->currentIndex();
    if (i >= 0 && gpuDriverCombo_->itemText(i) == gpuDriverCombo_->currentText())
        return gpuDriverCombo_->itemData(i).toString() == kGpuVenus;
    return false;
}

// Title the baseline disclosure with its live count. Scoped to THIS version (patchIsDefaultFor),
// not the registry's whole defaultOn set — an A17-only entry counted on A16 would read as drift on
// a perfectly stock profile. Reads the checkboxes, which are the state now that they are editable.
void MainWindow::refreshBaselineTitle() {
    if (!goldenDisclosure_) return;
    const int av = androidVersion();
    int applied = 0, expected = 0;
    for (const SourcePatch &p : sourcePatches()) {
        if (!p.defaultOn || !patchIsDefaultFor(p, av)) continue;
        ++expected;
        QCheckBox *cb = goldenChecks_.value(p.key);
        if (cb && cb->isChecked()) ++applied;
    }
    goldenDisclosure_->setText(
        applied == expected
            ? QStringLiteral("Baseline patches — all %1 applied").arg(expected)
            : QStringLiteral("Baseline patches — %1 of %2 applied").arg(applied).arg(expected));
}

// Pack the list into two columns, under section headings. Re-packed rather than positioned once
// because the patch half comes and goes — the version filter, the profile gate and the
// feature-implied rule each hide entries, and a QGridLayout keeps a hidden widget's cell, so fixed
// positions would grow holes and empty headings. A heading is emitted only once its section is
// known to have a visible member, and spans both columns.
// Ask the source tree where it stands, off-thread, and fold the answer into one status-bar token.
// Reuses the probe the Image page's status line used to run: HEAD, the tracked branch, and
// ahead/behind vs @{upstream}. A repo/AOSP tree has no top-level .git — its version is the
// manifest repo, so fall through to .repo/manifests. VALIDATE the repo rather than stat .git: a
// husk .git (info/attributes only, manufactured by the union-merge step) made the old probe report
// "no git checkout" for a fully synced tree and invite a re-sync that would have wiped it
// (bd remora-eoy.11).
void MainWindow::refreshSourceState() {
    if (!sourceStateLabel_) return;
    // A deterministic screenshot must not touch the tree, and a probe in flight at quit would be
    // destroyed mid-run — the same reason refreshAppsPage() bails here.
    if (qEnvironmentVariableIsSet("REMORA_SHOT")) return;
    const QString host = sourceHostStr();
    const QString dir = sourceTreePath();
    const QString built = cfg_.image.builtCommit.value_or(QString());
    if (dir.trimmed().isEmpty()) { sourceStateLabel_->hide(); return; }
    auto *thread = QThread::create([this, host, dir, built] {
        const QString probe = runShell(
            host,
            QStringLiteral("g=%1; if git -C \"$g\" rev-parse --git-dir >/dev/null 2>&1; then :; "
                           "elif [ -d \"$g/.repo/manifests\" ]; then g=\"$g/.repo/manifests\"; "
                           "else g=; fi; [ -n \"$g\" ] || exit 0; "
                           "echo HEAD=$(git -C \"$g\" rev-parse --short HEAD 2>/dev/null); "
                           "lb=$(git -C \"$g\" rev-parse --abbrev-ref HEAD 2>/dev/null); "
                           "br=$(git -C \"$g\" config --get branch.$lb.merge 2>/dev/null "
                           "| sed 's|refs/heads/||; s|refs/tags/||'); "
                           "echo BR=${br:-$lb}; "
                           "echo AB=$(git -C \"$g\" rev-list --left-right --count "
                           "HEAD...@{upstream} 2>/dev/null | tr '\\t' '/')")
                .arg(dir),
            30000);
        QString head, branch, ab;
        for (const QString &l : probe.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (l.startsWith(QLatin1String("HEAD="))) head = l.mid(5).trimmed();
            else if (l.startsWith(QLatin1String("BR="))) branch = l.mid(3).trimmed();
            else if (l.startsWith(QLatin1String("AB="))) ab = l.mid(3).trimmed();
        }
        int ahead = 0, behind = 0;
        const QStringList abp = ab.split(QLatin1Char('/'));
        if (abp.size() == 2) { ahead = abp[0].toInt(); behind = abp[1].toInt(); }
        QMetaObject::invokeMethod(
            this,
            [this, head, branch, built, ahead, behind] {
                if (!sourceStateLabel_) return;
                if (head.isEmpty()) {  // configured, but nothing checked out there yet
                    sourceStateLabel_->setText(QStringLiteral("source: no checkout"));
                    sourceStateLabel_->setToolTip(QStringLiteral(
                        "The source directory has no git checkout — 'Pull source…' on the Image "
                        "page creates one."));
                    sourceStateLabel_->show();
                    return;
                }
                // Two independent facts, and the label says whichever is worth acting on. BUILD
                // STATE first: being behind upstream matters less than not having built what you
                // already have.
                QStringList bits;
                if (built.isEmpty())
                    bits << QStringLiteral("not built");
                else if (!head.startsWith(built) && !built.startsWith(head))
                    bits << QStringLiteral("rebuild pending");
                if (behind > 0) bits << QStringLiteral("↓%1").arg(behind);
                sourceStateLabel_->setText(bits.isEmpty()
                                               ? QStringLiteral("source: up to date")
                                               : QStringLiteral("source: %1")
                                                     .arg(bits.join(QStringLiteral(" · "))));
                sourceStateLabel_->setToolTip(
                    QStringLiteral("%1 @ %2 · %3 ahead, %4 behind upstream\n\n%5")
                        .arg(branch.isEmpty() ? QStringLiteral("(detached)") : branch, head)
                        .arg(ahead).arg(behind)
                        .arg(built.isEmpty()
                                 ? QStringLiteral("Never built through Remora — 'Build from "
                                                  "source…' records the commit it builds.")
                             : (!head.startsWith(built) && !built.startsWith(head))
                                 ? QStringLiteral("The tree has moved since the last build (%1) — "
                                                  "rebuild to match.").arg(built)
                                 : QStringLiteral("Built at this commit.")));
                sourceStateLabel_->show();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::repackFeatureGrid() {
    if (!featureGrid_) return;
    while (QLayoutItem *item = featureGrid_->takeAt(0)) delete item;  // items only, not widgets
    for (QLabel *h : sectionHeaders_) h->hide();
    int row = 0;
    for (const auto &sec : featureSections()) {
        QVector<QWidget *> members;
        for (const QString &key : featureOrder_) {
            const Feature *f = findFeature(key);
            QCheckBox *cb = featureChecks_.value(key);
            if (f && cb && featureSectionFor(f->category) == sec.first) members << cb;
        }
        for (const SourcePatch &sp : sourcePatches()) {
            if (sp.defaultOn) continue;
            QWidget *w = patchRows_.value(sp.key);
            if (w && !w->isHidden() && featureSectionForPatchGroup(sp.group) == sec.first)
                members << w;
        }
        if (members.isEmpty()) continue;
        QLabel *head = sectionHeaders_.value(sec.first);
        featureGrid_->addWidget(head, row, 0, 1, 2);  // spans both columns
        head->show();
        ++row;
        for (int n = 0; n < members.size(); ++n) {
            featureGrid_->addWidget(members[n], row + n / 2, n % 2);
        }
        row += (members.size() + 1) / 2;
    }
}

QStringList MainWindow::selectedPatchKeys() const {
    QStringList on = goldenSelection_;  // stored entries with no widget — never dropped
    for (auto it = goldenChecks_.constBegin(); it != goldenChecks_.constEnd(); ++it)
        if (it.value()->isChecked()) on << it.key();
    for (auto it = patchChecks_.constBegin(); it != patchChecks_.constEnd(); ++it)
        if (it.value()->isChecked()) on << it.key();
    on.removeDuplicates();
    return on;
}

void MainWindow::refreshPatchApplicability() {
    if (patchRows_.isEmpty()) return;
    const int av = androidVersion();
    const QStringList ok = applicableSourcePatchKeys(av, readVendorPath);
    // Two independent filters, deliberately kept apart (see irrelevantPatches): `ok` answers "are
    // there assets for this Android version", the gate answers "does this profile want it". A row
    // needs both to appear, but conflating them would make the two causes indistinguishable.
    PatchProfile profile;
    profile.features = features_;
    // BOTH tiers (selectedPatchKeys). Asking patchChecks_ alone would tell irrelevantPatches()
    // that every golden patch is off — and any entry gated on `needs.patch` against a golden one
    // would vanish from the page for a profile that has it applied.
    profile.enabledPatches = selectedPatchKeys();
    profile.lineageTree =
        !(sourceKindCombo_ && sourceKindCombo_->currentData().toString() == QLatin1String("aosp"));
    profile.venus = venusSelected();
    const QMap<QString, QString> gated = irrelevantPatches(profile);
    // Patches an ENABLED feature already declares in requiresPatches: sensors pulls sensors_hal +
    // sensors_product, system_updater pulls settings_hide_stock_updater. Ticking the feature ticks
    // them, so offering them again is two controls for one decision. Hidden, and deliberately NOT
    // added to `gated` — the gate label is for entries a profile CANNOT use, and these are the
    // opposite: on, correct, and implied by something visible one box up.
    QSet<QString> impliedByFeature;
    for (const QString &fk : features_)
        if (const Feature *f = findFeature(fk))
            for (const QString &pk : f->requiresPatches) impliedByFeature.insert(pk);
    for (auto it = patchRows_.constBegin(); it != patchRows_.constEnd(); ++it)
        it.value()->setVisible(ok.contains(it.key()) && !gated.contains(it.key())
                               && !impliedByFeature.contains(it.key()));
    repackFeatureGrid();  // the grid must close the gaps the line above just opened
    // Features whose required patches have no assets for this version cannot be built here at all
    // (bd remora-82c.3). Mark them: a tooltip naming the inapplicable patch, and — so an
    // un-buildable feature cannot be freshly ticked — disabled UNLESS it is already on, which keeps
    // a pin made on another version un-tickable off (the same "never silently untick" rule the
    // patch rows follow). Derived from the same `ok` set, so features and patches agree.
    const QSet<QString> okSet(ok.begin(), ok.end());
    // This half owns the ENABLED state; the tooltip text is composed in one place
    // (applyFeatureTooltips) because recompute() has a claim on it too. They used to both write
    // it directly and the last one to run won: recompute set maturity + gate reason, this cleared
    // it to empty on the very next version change or patch tick.
    featureInapplicable_.clear();
    for (auto it = featureChecks_.constBegin(); it != featureChecks_.constEnd(); ++it) {
        const Feature *f = findFeature(it.key());
        QStringList inapplicable;
        if (f)
            for (const QString &pk : f->requiresPatches)
                if (!okSet.contains(pk)) inapplicable << pk;
        QCheckBox *cb = it.value();
        if (inapplicable.isEmpty()) {
            cb->setEnabled(true);
        } else {
            featureInapplicable_.insert(
                it.key(), QStringLiteral("Not buildable on Android %1 — requires source patch(es) "
                                         "with no assets for this version: %2. Your choice is "
                                         "preserved; it applies again on a version where it fits.")
                              .arg(av)
                              .arg(inapplicable.join(QStringLiteral(", "))));
            cb->setEnabled(cb->isChecked());  // togglable off if pinned on, not on if off
        }
    }
    applyFeatureTooltips();
    // The "N enabled patch(es) hidden as irrelevant here — still applied" line went with the
    // other grey notes. Gating still hides without unticking, so that remains true — it is just
    // no longer said on the page; source_patches= in remorarc is the record.
}


void MainWindow::populateSourceRefCombo() {
    if (!sourceRefCombo_) return;
    const int version = androidVersion();
    const bool aosp = sourceKindCombo_
                      && sourceKindCombo_->currentData().toString() == QLatin1String("aosp");
    QStringList filtered;
    for (const QString &r : sourceRefsAll_) {
        // Under the AOSP kind the lineage-<NN> branches are not choices at all — an AOSP tree
        // cannot sync them, and offering them is how a stray kind flip still LOOKED like a
        // lineage build while the aosp arm skipped everything lineage (bd remora-82c.4).
        if (aosp && r.startsWith(QLatin1String("lineage-"))) continue;
        const std::optional<int> v = refAndroidVersion(r);
        if ((v && *v == version) || r == QLatin1String("master") || r == QLatin1String("main"))
            filtered << r;
    }
    QString keep = sourceRefCombo_->currentText().trimmed();
    const std::optional<int> keepVer = refAndroidVersion(keep);
    if (keepVer && *keepVer != version) {
        if (aosp) {
            // ls-remote output is version-sorted newest-first → first versioned hit = latest
            keep.clear();
            for (const QString &r : filtered)
                if (refAndroidVersion(r)) { keep = r; break; }
        } else {
            keep = lineageBranchForAndroid(version);
        }
        statusBar()->showMessage(
            QStringLiteral("source ref followed the Android version change → %1")
                .arg(keep.isEmpty() ? QStringLiteral("(none)") : keep), 5000);
    }
    QSignalBlocker block(sourceRefCombo_);
    sourceRefCombo_->clear();
    sourceRefCombo_->addItems(filtered);
    sourceRefCombo_->setCurrentText(keep);
}

// Ensure the vendored source assets (patches, local_manifest, do-build.sh) exist on the source
// host; returns the root path to use there. Local host → the install's vendor dir unchanged.
QString MainWindow::stageSourceAssets(const QString &host) {
    const QString localVendor = QStringLiteral(REMORA_VENDOR_DIR);
    if (host.isEmpty()) return localVendor + QStringLiteral("/source-patches");
    const QString remoteRoot = QStringLiteral("~/.cache/remora/source-assets");
    // create the remote dir, then scp the source-build + source-patches trees into it
    runShell(host, QStringLiteral("mkdir -p %1").arg(remoteRoot), 20000);
    QProcess scp;
    scp.start(QStringLiteral("scp"),
              {QStringLiteral("-r"), QStringLiteral("-q"),
               localVendor + QStringLiteral("/source-build"),
               localVendor + QStringLiteral("/source-patches"),
               host + QStringLiteral(":") + remoteRoot + QStringLiteral("/")});
    waitForProcess(scp, 120000);
    return remoteRoot + QStringLiteral("/source-patches");
}


void MainWindow::onPullSource() {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"), 5000);
        return;
    }
    const QString host = sourceHostStr();
    const QString tree = sourceTreePath();
    const QString repo = sourceRepoStr();
    const QString ref = sourceRefCombo_ ? sourceRefCombo_->currentText().trimmed() : QString();
    if (repo.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Pull source"),
                                 QStringLiteral("Set a Source repo (git URL or repo manifest) first."));
        return;
    }
    const bool lineage =
        sourceKindCombo_ && sourceKindCombo_->currentData().toString() != QLatin1String("aosp");
    // repo (lineage/aosp manifest) installs the vendored local manifest before sync (an empty
    // overlay since the upstream projects left both manifests); else a plain git clone.
    const QString assets = stageSourceAssets(host);  // returns <root>/source-patches
    const QString sbuild = assets.left(assets.lastIndexOf(QLatin1Char('/'))) +
                           QStringLiteral("/source-build");
    const bool pinned = lineage && cfg_.image.sourcePinned.value_or(true);
    // RESET THE PATCH PROJECTS FIRST — the same pass the build runs, and for the same reason its
    // own comment gives: a dirty or conflicted patch project blocks repo's checkout, so the sync
    // dies part-way with "Your local changes would be overwritten by checkout". Pull did not run
    // it, so pulling onto a tree that had ever been patched failed every time; the two paths that
    // sync the tree disagreed about who cleans up. git-apply patches leave working-tree edits
    // (the old device tree's product .mk and BoardConfig.mk are the ones that bit); reset drops
    // those. Committed (git am) patches survive the reset — repo discards them itself when it
    // moves the project, and re-applying is what a build does next anyway.
    // EVERY patch project, not just the enabled ones, and no project marks. A pull means "bring
    // this tree to the manifest", so anything Remora has ever written into has to be pristine
    // first. Scoping the reset to the ENABLED set leaves a hole that reappears as the same failure:
    // untick a patch, pull, and the project it dirtied last time is still dirty and still blocks
    // the checkout — with nothing on the page connecting the two. The build scopes its reset
    // narrowly on purpose (it passes marks so the incremental pass can skip untouched projects);
    // a pull has no such pass to protect.
    QStringList allPatchKeys;
    for (const SourcePatch &sp : sourcePatches()) allPatchKeys << sp.key;
    const QStringList reset = lineage ? buildResetCommands(allPatchKeys, tree) : QStringList{};
    const QString cmd =
        gitIdentityGuard() + QStringLiteral(" && ") +
        (reset.isEmpty() ? QString() : reset.join(QStringLiteral(" ; ")) + QStringLiteral(" ; ")) +
        (lineage
             ? repoBringupCmd(tree, repo, ref, pinned, sbuild,
                              cfg_.image.androidVersion.value_or(16))
             : QStringLiteral("git clone --progress %1 %2 %3")
                   .arg(ref.isEmpty() ? QString() : QStringLiteral("-b ") + ref, repo, tree));
    // Summary, not the command. The reset pass prepends one guarded shell block PER PATCHABLE
    // PROJECT, so the literal command is ~15 blocks of git incantation — it filled the screen and
    // buried the one sentence that matters. The full text is still one click away in Details,
    // which is where a command you are being asked to authorise belongs.
    {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("Pull source"));
        box.setText(QStringLiteral("Sync the source tree on %1?")
                        .arg(host.isEmpty() ? QStringLiteral("this host") : host));
        QStringList lines;
        lines << QStringLiteral("• %1").arg(tree);
        if (lineage)
            lines << QStringLiteral("• %1%2%3")
                         .arg(repo, ref.isEmpty() ? QString() : QStringLiteral("  @ ") + ref,
                              pinned ? QStringLiteral("  (Remora's pinned manifest)") : QString());
        if (!reset.isEmpty())
            lines << QStringLiteral("• %1 patchable projects reset to pristine first — Remora "
                                    "re-applies its own patches on the next source build; hand "
                                    "edits in them are lost").arg(reset.size());
        lines << QStringLiteral("• Can take a long time and a lot of disk");
        box.setInformativeText(lines.join(QLatin1Char('\n')));
        box.setDetailedText(cmd);
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes) return;
    }
    // Stream progress on the Progress page: a single 'pull-source' step whose detail is the latest
    // output line, so the user sees it working and a clear ok/failed at the end (a full repo sync
    // is hours). Runs in a QThread; QProcess is polled for output there.
    checklist_->clear();
    onState(QStringLiteral("pull-source"), QStringLiteral("running"),
            QStringLiteral("starting on %1…").arg(host.isEmpty() ? QStringLiteral("local") : host),
            QString());
    setRunning(true);
    // repo gates its consolidated progress line — "Fetching: 34% (390/1137) …" — on having a TTY;
    // through a plain pipe it stays silent and only raw git/curl noise comes out. Run the child
    // under a pseudo-TTY (ptyRunArgv: script(1) locally, ssh -tt remotely) so the real % streams.
    // -tt also means killing the local ssh HUPs the remote sync — a remote pull can't orphan
    // either. Verdict comes from the sentinel the wrap appends, not the wrapper's exit code,
    // which reads 0 when the wrapper itself is killed (bd remora-c7y).
    const QStringList argv = ptyRunArgv(cmd, host);
    auto *thread = QThread::create([this, argv, host, tree] {
        // heal leftovers from any earlier killed/failed sync before starting a new one
        cleanupStaleWorktrees(host, tree);
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        posixShellEnv(p);
#ifdef Q_OS_UNIX
        // Run the child in its own process group so closeEvent can kill the whole sync
        // (repo + its -j workers + git fetches) in one killpg without touching remora itself.
        p.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
        QFile log(QStringLiteral("/tmp/remora-pull.log"));
        const bool logOk =
            log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        if (!logOk) log.setFileName(QString());  // stream continues; only the file copy is lost
        QElapsedTimer clock;
        clock.start();
        p.start(argv.first(), argv.mid(1));
        if (!p.waitForStarted(10000)) {
            QMetaObject::invokeMethod(this, [this] {
                onState(QStringLiteral("pull-source"), QStringLiteral("failed"),
                        QStringLiteral("could not start"), QString());
                setRunning(false);
            }, Qt::QueuedConnection);
            return;
        }
        pullChildPid_.store(p.processId());  // group leader == child pid (see setpgid above)
        QString lastLine, progressLine;
        qint64 lastEmit = -1;
        const auto elapsed = [&clock] {
            const qint64 s = clock.elapsed() / 1000;
            return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
        };
        const QRegularExpression lineSplit(QStringLiteral("[\\r\\n]"));
        const QRegularExpression ansi(QStringLiteral("\\x1b\\[[0-9;?]*[A-Za-z]"));  // pty escapes
        // curl's clone.bundle transfer meter — two numeric columns ("100  6.53M  30 …")
        const QRegularExpression curlMeter(QStringLiteral("^\\d+\\s+[\\d.]+[kMG]?(\\s|$)"));
        // a percent-with-count progress line from repo or git ("34% (390/1137)")
        const QRegularExpression pctCount(QStringLiteral("\\d+% \\(\\d+/\\d+\\)"));
        QString vTail;  // for the exit sentinel — kept off lastLine so the row shows real output
        while (p.state() != QProcess::NotRunning) {
            if (p.waitForReadyRead(1000)) {
                QString chunk = QString::fromUtf8(p.readAll());
                chunk.remove(ansi);
                log.write(chunk.toUtf8());
                log.flush();
                vTail = (vTail + chunk).right(2048);
                // repo/git update progress with \r; split on both so the live % shows
                for (const QString &ln : chunk.split(lineSplit, Qt::SkipEmptyParts)) {
                    const QString t = ln.trimmed();
                    if (t.isEmpty() || t.startsWith(QLatin1String(kPtyExitSentinel))) continue;
                    // curl clone.bundle probes: transfer meters and expected 404s (repo probes
                    // each mirror for a bundle; most miss) — log-only noise, keep off the row
                    if (t.startsWith(QLatin1String("curl:")) ||
                        t.contains(QLatin1String("% Total")) ||
                        t.contains(QLatin1String("Dload")) || curlMeter.match(t).hasMatch())
                        continue;
                    lastLine = t;
                    // repo's consolidated status ("Fetching: 34% (390/1137) …") — pin it so tag
                    // spam between refreshes doesn't push the % off the row
                    if (t.startsWith(QLatin1String("Fetching:")) ||
                        t.startsWith(QLatin1String("Checking out:")) ||
                        t.startsWith(QLatin1String("Syncing:")) ||
                        t.startsWith(QLatin1String("Receiving objects:")) ||
                        t.startsWith(QLatin1String("Resolving deltas:")) ||
                        pctCount.match(t).hasMatch())
                        progressLine = t;
                }
            }
            // emit at most ~1/sec so the UI shows a live clock even during a quiet \r spinner
            if (clock.elapsed() / 1000 != lastEmit) {
                lastEmit = clock.elapsed() / 1000;
                const QString detail =
                    QStringLiteral("[%1] %2")
                        .arg(elapsed(),
                             progressLine.isEmpty() ? lastLine : progressLine);
                QMetaObject::invokeMethod(this, [this, detail] {
                    onState(QStringLiteral("pull-source"), QStringLiteral("running"), detail,
                            QString());
                    statusBar()->showMessage(QStringLiteral("Pull: %1").arg(detail));
                }, Qt::QueuedConnection);
            }
        }
        {  // drain any remaining buffered output — the sentinel is the last line of all
            QString chunk = QString::fromUtf8(p.readAll());
            chunk.remove(ansi);
            log.write(chunk.toUtf8());
            vTail = (vTail + chunk).right(2048);
            for (const QString &ln : chunk.split(lineSplit, Qt::SkipEmptyParts)) {
                const QString t = ln.trimmed();
                if (!t.isEmpty() && !t.startsWith(QLatin1String(kPtyExitSentinel))) lastLine = t;
            }
        }
        log.close();
        pullChildPid_.store(0);  // process ended — nothing left for closeEvent to kill
        // Judged by the chain's sentinel, wrapper code as fallback only (bd remora-c7y).
        const int wrapperRc = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 128;
        const PtyExit ex = judgePtyExit(wrapperRc, vTail);
        const int rc = ex.rc;
        const bool interrupted = ex.interrupted;
        // a failed sync leaves half-checked-out worktrees — clean them now so the retry is clean
        // (the canceled-at-close case is cleaned by closeEvent instead; this thread dies with it)
        if (rc != 0 && p.exitStatus() == QProcess::NormalExit) cleanupStaleWorktrees(host, tree);
        const QString took = elapsed();
        QMetaObject::invokeMethod(this, [this, rc, lastLine, took, interrupted] {
            onState(QStringLiteral("pull-source"),
                    rc == 0 ? QStringLiteral("ok") : QStringLiteral("failed"),
                    rc == 0 ? QStringLiteral("source pull complete in %1 · "
                                             "log: /tmp/remora-pull.log")
                                  .arg(took)
                    : interrupted
                        ? QStringLiteral("pull INTERRUPTED after %1 (exit %2) — the pty closed "
                                         "before the sync reported a status · "
                                         "log: /tmp/remora-pull.log").arg(took).arg(rc)
                        : QStringLiteral("pull failed after %1 (exit %2): %3 · "
                                         "log: /tmp/remora-pull.log")
                              .arg(took).arg(rc).arg(lastLine),
                    QString());
            setRunning(false);
            onRefreshArchives();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// Chain the full reproducible pipeline on the source host: apply the selected patches, then run
// the vendored do-build.sh. (Pull the tree first with 'Pull source'.) Streamed to Progress.
// Exit 137 is 128+SIGKILL. For a multi-hour AOSP build that is almost always systemd-oomd, a
// PSI-driven userspace OOM killer that fires on combined RAM+swap pressure and kills the heaviest
// cgroup — the docker scope. The trap worth naming in the UI: oomd logs to the JOURNAL, not to
// dmesg, so `dmesg | grep oom-kill` returns nothing while it is killing your builds. That false
// negative cost a day of misdiagnosis (bd remora-4ei.23), so say it outright rather than leaving a
// bare exit code. Silent when journalctl is unreadable or shows no kill — a guess would be worse
// than nothing.
static QString oomdKillNote(int rc) {
    if (rc != 137) return QString();
    QProcess j;
    j.start(QStringLiteral("journalctl"),
            {QStringLiteral("-u"), QStringLiteral("systemd-oomd"), QStringLiteral("--since"),
             QStringLiteral("-6 hours"), QStringLiteral("--no-pager"), QStringLiteral("-q")});
    if (!j.waitForStarted(2000) || !waitForProcess(j, 5000)) return QString();
    // parseOomdKilled, not a local contains(): oomd never logs the word "killed" — it logs
    // "Marked <cgroup> for killing due to ..." — so the obvious check cannot fire on a real kill.
    // Pinned to verbatim journal text in tests/test_parsers.cpp.
    if (!parseOomdKilled(QString::fromUtf8(j.readAllStandardOutput()))) return QString();
    return QStringLiteral(" — KILLED BY systemd-oomd (exit 137 = SIGKILL). It logs to the journal, "
                          "not dmesg: journalctl -u systemd-oomd. Raise SwapUsedLimit in "
                          "/etc/systemd/oomd.conf — see bd remora-4ei.23");
}

void MainWindow::onBuildFromSource() {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"), 5000);
        return;
    }
    // BOTH tiers — this list is the build's actual patch set (SourceBuildPlan::enabledPatches),
    // not just what the preflight warnings read.
    const QStringList enabled = selectedPatchKeys();
    // A HARD stop, and first, because it is the cheapest question here (a local directory read —
    // no ssh, no `df`) and the only one whose answer is "this build cannot succeed". A patch file
    // the registry names but the tree does not carry already fails the apply loop by design
    // (bd remora-fgj.11); asking now saves the repo bring-up in front of it. Scoped to this
    // version's TICKED entries — an opt-in entry this build would never touch must not block it.
    {
        // Same two-direction refusal as the CLI: lacking files would fail the apply loop (tree
        // older — reinstall it), unnamed files would never reach it and ship a green build
        // WITHOUT them (binary older — rebuild remora; bd remora-y9y9's five-of-six trap).
        QStringList lacking, unnamed;
        for (const PatchAssetRow &g : auditPatchAssetsOnDisk()) {
            if (g.version != cfg_.image.androidVersion.value_or(16) || !enabled.contains(g.key))
                continue;
            if (!g.detail.isEmpty())
                lacking << QStringLiteral("%1 — %2 lacks %3").arg(g.key, g.dir, g.detail);
            if (!g.unknownDetail.isEmpty())
                unnamed << QStringLiteral("%1 — %2 carries %3")
                               .arg(g.key, g.dir, g.unknownDetail);
        }
        if (!lacking.isEmpty() || !unnamed.isEmpty()) {
            QString msg;
            if (!lacking.isEmpty())
                msg += QStringLiteral(
                           "The patch tree does not carry what this build needs, so the build "
                           "would fail at the patch step:\n\n  %1\n\nThe installed vendor tree is "
                           "older than this binary. Reinstall it (cmake --install) and try again.")
                           .arg(lacking.join(QStringLiteral("\n  ")));
            if (!unnamed.isEmpty()) {
                if (!msg.isEmpty()) msg += QStringLiteral("\n\n");
                msg += QStringLiteral(
                           "The patch tree carries files this binary's registry does not name, "
                           "and a build would silently SKIP them:\n\n  %1\n\nThis binary is older "
                           "than its patch tree. Rebuild remora and try again.")
                           .arg(unnamed.join(QStringLiteral("\n  ")));
            }
            QMessageBox::critical(this, QStringLiteral("Patch tree is out of sync"), msg);
            return;
        }
    }
    // A default-on patch this profile never picked up is very nearly always a doomed build, so it
    // is worth a modal BEFORE the multi-hour run rather than only the line the apply chain logs.
    // Once source_patches is in remorarc it is an explicit pin and a newly registered patch never
    // joins it; twice in one day that meant a build that failed exactly as if the patch did not
    // work (bd remora-4ei.38). Same shape as the ENOSPC guard below, for the same reason — and
    // ahead of it, because this needs no `df`/`du` round trip over ssh to answer. Deliberately not
    // Not gated on anything any more — this is the only route into a source build.
    // FEATURES have the identical hole (bd remora-4ei.44) — features= is pinned the same way and a
    // feature registered later never joins it — so they share the dialog rather than queueing a
    // second one in front of the same build. Milder on its own (an absent capability, not a build
    // that dies at 78%), but the cause and the remedy are the same sentence.
    const QStringList missedPatches =
        unappliedDefaultPatches(enabled, cfg_.image.androidVersion.value_or(16));
    const QStringList missedFeatures = unappliedDefaultFeatures(features_);
    // A ticked feature whose required patches are NOT ticked. Different cause from the two above —
    // nothing went stale, the patches are opt-in and were never turned on — so it is worded as its
    // own paragraph rather than folded in: this build succeeds and ships the feature inert, which
    // is how A17 got an image whose sensors feature was listed and whose HAL did not exist
    // (bd remora-4ei.53). Shares the dialog for the same reason features and patches already do —
    // one stop before the multi-hour run, not a queue of modals in front of it.
    const QVector<FeaturePatchGap> patchGaps =
        featurePatchGaps(features_, enabled, cfg_.image.androidVersion.value_or(16));
    if (!missedPatches.isEmpty() || !missedFeatures.isEmpty() || !patchGaps.isEmpty()) {
        QString body;
        if (!missedPatches.isEmpty() || !missedFeatures.isEmpty())
            body = QStringLiteral(
                "This profile pinned its list before these were added, and a pinned list never "
                "picks up new ones.\n");
        if (!missedPatches.isEmpty())
            body += QStringLiteral("\n%1 patch group(s) that ship enabled by default are NOT "
                                   "ticked, so the build runs without them:\n  %2\n"
                                   "Nothing later in the build will mention them again — it will "
                                   "simply fail as though the patch did not work.\nTick them on "
                                   "the Image page's source half.\n")
                        .arg(missedPatches.size())
                        .arg(missedPatches.join(QStringLiteral("\n  ")));
        if (!missedFeatures.isEmpty())
            body += QStringLiteral("\n%1 feature(s) that ship enabled by default are NOT "
                                   "selected, so the image is built without them:\n  %2\n"
                                   "Tick them on the Image page.\n")
                        .arg(missedFeatures.size())
                        .arg(missedFeatures.join(QStringLiteral("\n  ")));
        for (const FeaturePatchGap &g : patchGaps)
            body += QStringLiteral("\nThe feature \"%1\" is enabled but the source patches it "
                                   "needs are NOT ticked:\n  %2\nThe build will SUCCEED and ship "
                                   "\"%1\" doing nothing at all.\nTick them in Source patches.\n")
                        .arg(g.feature, g.missingPatches.join(QStringLiteral("\n  ")));
        const QString title = (missedPatches.isEmpty() && missedFeatures.isEmpty())
                                  ? QStringLiteral("A feature is missing its source patches")
                                  : QStringLiteral("Defaults are not enabled");
        if (QMessageBox::warning(this, title,
                                 body + QStringLiteral("\nFix them first, or Build Anyway."),
                                 QMessageBox::Ignore | QMessageBox::Cancel, QMessageBox::Cancel)
            != QMessageBox::Ignore)
            return;
    }
    // à-la-carte features that bake into the source build via product-config toggles (WITH_*)
    const bool withGapps = features_.contains(QStringLiteral("gapps"));
    const bool withMicrog = features_.contains(QStringLiteral("microg"));
    const bool withNdk = features_.contains(QStringLiteral("arm_translate"));
    const bool withMagisk = features_.contains(QStringLiteral("magisk_root"));
    const bool withUpdater = features_.contains(QStringLiteral("system_updater"));
    const bool withPlaySpoof = features_.contains(QStringLiteral("play_spoof"));
    const bool withShamiko = features_.contains(QStringLiteral("shamiko"));
    const bool withCamera = features_.contains(QStringLiteral("camera_v4l2"));
    const bool withWv = features_.contains(QStringLiteral("widevine_l3"));
    // Whether the BUILD ships the c2-va codecs — a different question from the boot arg that
    // activates them, which is now unconditional. The 'HW video decode (VA Codec2)' feature is the
    // one control for it; it used to be OR'd with the Advanced codec2 checkbox, so two unrelated-
    // looking rows both silently decided the image variant.
    const bool withHwc2 = features_.contains(QStringLiteral("hw_video_decode"));
    // The docker tag this build will produce (feature-derived; -src so it never clobbers a golden
    // tag). Compute it up front so the naming prompt can pre-fill the current name for that tag.
    const bool lineageKind = !(sourceKindCombo_ && sourceKindCombo_->currentData().toString()
                                                       == QLatin1String("aosp"));
    // Kind=aosp with lineage evidence in the profile is refused HERE, before the naming prompt
    // and the disk checks: the aosp arm would skip the manifest sync and every container-compat
    // patch and build the wrong image without saying so (see aospKindConflict).
    if (!lineageKind) {
        const QString clash = aospKindConflict(
            sourceRefCombo_ ? sourceRefCombo_->currentText().trimmed() : QString(),
            selectedPatchKeys());
        if (!clash.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Source kind mismatch"), clash);
            return;
        }
    }
    // One tag composer for both callers — this used to be a hand-rolled copy of the same format
    // string, which is exactly how the arch-variant suffix would have drifted between the GUI
    // and `remora build --source`. The family/version/promote reasoning lives with
    // sourceBuiltTag() in engine/SourceBuild.cpp.
    const int builtVer = cfg_.image.androidVersion.value_or(16);
    const QString archVariant = cfg_.image.archVariant.value_or(QString());
    const QString builtTag = sourceBuiltTag(builtVer, lineageKind, withGapps, withMicrog, withWv,
                                            withHwc2, archVariant);
    // Prompt to name this build so it's identifiable in the (often cluttered) image list. Blank =
    // no custom name; Cancel aborts the build. The name is display-only, prepended to the tag.
    {
        bool nameOk = false;
        const QString name = QInputDialog::getText(
            this, QStringLiteral("Name this image"),
            QStringLiteral("A name to identify this build in the image list (blank = none):\n\n"
                           "Tag: %1").arg(builtTag),
            QLineEdit::Normal, imageCustomName(path_, builtTag), &nameOk);
        if (!nameOk) return;  // cancelled
        setImageCustomName(path_, builtTag, name);
    }
    const QString host = sourceHostStr();
    const QString tree = sourceTreePath();
    const QString kindLabel = (sourceKindCombo_ && sourceKindCombo_->currentData().toString()
                                                        == QLatin1String("aosp"))
                                  ? QStringLiteral("AOSP")
                                  : QStringLiteral("LineageOS");
    const bool lineage =
        !(sourceKindCombo_ && sourceKindCombo_->currentData().toString() == QLatin1String("aosp"));
    const bool pinned = lineage && cfg_.image.sourcePinned.value_or(true);
    const QString repoUrl = sourceRepoStr();
    const QString ref = sourceRefCombo_ ? sourceRefCombo_->currentText().trimmed() : QString();
    {
        // out/ for a full A16 build lands around 300–400 GB; catching ENOSPC an hour in is
        // the worst outcome — check the tree filesystem's headroom first
        const ShellResult free = runShellRc(
            host,
            QStringLiteral("df -Pk %1 2>/dev/null | tail -1 | awk '{print $4}'").arg(tree),
            15000);
        const qint64 availG = free.out.trimmed().toLongLong() / (1024 * 1024);
        // How big out/ already is: the library scan measured it (off-thread, bd remora-s6ma), so
        // take that figure when there is one. The `du` here ran on the GUI thread on every Build
        // click — 2.5 s warm, far longer cold — and when it hit its 60 s cap the size read as 0,
        // which turned into a "needs ~350 GiB more" warning over a tree that already held 265.
        qint64 outG = sourceOutSizeBytes_ / (1024 * 1024 * 1024);
        if (sourceOutSizeBytes_ < 0) {
            const ShellResult outSz = runShellRc(
                host, QStringLiteral("du -sk %1/out 2>/dev/null | cut -f1").arg(tree), 60000);
            outG = outSz.out.trimmed().toLongLong() / (1024 * 1024);
        }
        const qint64 needG = qMax<qint64>(0, 350 - outG);
        if (free.rc == 0 && availG > 0 && availG < needG
            && QMessageBox::warning(
                   this, QStringLiteral("Probably not enough disk space"),
                   QStringLiteral("%1 has %2 GiB free, but the build output needs roughly %3 GiB "
                                  "more (out/ grows to ~350 GiB total; currently %4 GiB). The "
                                  "build will likely die with 'no space left on device'.\n\n"
                                  "Free up space first, or Build Anyway.")
                       .arg(tree)
                       .arg(availG)
                       .arg(needG)
                       .arg(outG),
                   QMessageBox::Ignore | QMessageBox::Cancel, QMessageBox::Cancel)
                   != QMessageBox::Ignore)
            return;
    }
    // Always asked now. This used to be skippable when onBuildImage had routed a source-only
    // feature set here and already prompted; that button is gone, so this is the only entry point
    // and the only prompt.
    if (QMessageBox::question(
               this, QStringLiteral("Build from source"),
               QStringLiteral("On %1: sync %3 to the %5 manifest, reset the patch projects to "
                              "pristine, apply %2 patch group(s), then run do-build.sh. This is a "
                              "multi-hour %4 build. Proceed?")
                   .arg(host.isEmpty() ? QStringLiteral("local") : host)
                   .arg(enabled.size())
                   .arg(tree, kindLabel,
                        pinned ? QStringLiteral("pinned (exact tested SHAs)")
                               : QStringLiteral("branch-tip")))
               != QMessageBox::Yes)
        return;
    checklist_->clear();
    setRunning(true);
    onState(QStringLiteral("build-from-source"), QStringLiteral("running"),
            QStringLiteral("staging + applying patches…"), QString());
    const int buildNice = buildNiceSpin_ ? buildNiceSpin_->value() : 15;
    const int buildJobs = buildJobsSpin_ ? buildJobsSpin_->value() : 0;
    const int buildMemGiB = buildMemSpin_ ? buildMemSpin_->value() : 0;
    const int soongMemGiB = soongMemSpin_ ? soongMemSpin_->value() : 0;
    const int androidVer = cfg_.image.androidVersion.value_or(16);
    const QMap<QString, QString> requiredOwners = featureRequiredPatchOwners(features_);
    auto *thread = QThread::create([this, host, tree, enabled, lineage, pinned, repoUrl, ref,
                                    buildNice, buildJobs, buildMemGiB, soongMemGiB, androidVer,
                                    withGapps, withMicrog,
                                    withNdk, withMagisk, withUpdater, withPlaySpoof, withShamiko,
                                    withCamera,
                                    withWv,
                                    withHwc2, builtTag, archVariant, requiredOwners] {
        cleanupStaleWorktrees(host, tree);  // heal killed-sync wreckage before repo touches it
        const QString patchRoot = stageSourceAssets(host);
        const QString sbuild = patchRoot.left(patchRoot.lastIndexOf(QLatin1Char('/'))) +
                               QStringLiteral("/source-build");
        // INCREMENTAL PATCH STATE — the reset / sync / re-apply cycle below exists to put the tree
        // in one specific patched state, and re-running it wholesale when only one patch changed
        // costs ~7 minutes of bootstrap + kati + Soong analysis before ninja even starts, because
        // `repo sync --force-sync` rewrites every source file's mtime and Soong can then reuse
        // nothing. Measured: 40:48 of a 42:46 build was `bootstrap blueprint`, reaching a
        // compile error 30 seconds in; repo sync itself is 2.5s on this host.
        //
        // So the work is scoped by fingerprint, at two granularities (see core/SourcePatches.h for
        // the correctness constraint that forces the split, and why "just skip the sync" is wrong):
        //   repoFp     — the manifest/url/ref/version and the container-compat patches. A change
        //                here means a global sync, which wipes every project, so the prologue also
        //                deletes every per-project record: the dangerous case is made safe by
        //                construction rather than by reasoning.
        //   projectFp  — per project, its own patch content. A change means `repo sync --force-sync
        //                <that project>` and a re-apply of that project alone; the other ~1180
        //                projects keep their mtimes and Soong keeps its analysis.
        // The prologue writes mark files; every reset/apply step is guarded by the mark of the
        // project it touches, so a project's steps still all run or all skip together — which is
        // what keeps the shell variables they pass between them flowing.
        SourceBuildPlan plan;
        plan.tree = tree;
        plan.patchRoot = patchRoot;
        plan.sourceBuildDir = sbuild;
        plan.enabledPatches = enabled;
        plan.lineage = lineage;
        plan.pinned = pinned;
        plan.repoUrl = repoUrl;
        plan.ref = ref;
        plan.builtTag = builtTag;
        plan.androidVersion = androidVer;
        plan.buildNice = buildNice;
        plan.buildJobs = buildJobs;
        plan.buildMemGiB = buildMemGiB;
        plan.soongMemGiB = soongMemGiB;
        plan.withGapps = withGapps;
        plan.withMicrog = withMicrog;
        plan.withNdk = withNdk;
        plan.withMagisk = withMagisk;
        plan.withUpdater = withUpdater;
        plan.withPlaySpoof = withPlaySpoof;
        plan.withShamiko = withShamiko;
        plan.withCamera = withCamera;
        plan.withUsbAudio = features_.contains(QStringLiteral("usb_audio"));
        plan.withMesaSource = features_.contains(QStringLiteral("mesa_source"));
        plan.withAgent = features_.contains(QStringLiteral("mirror_agent"));
        plan.archVariant = archVariant;
        plan.requiredPatchOwners = requiredOwners;
        // The chain itself lives in engine/SourceBuild so `remora build --source` runs the SAME
        // one (bd remora-25u): a second copy would drift, and this ordering is load-bearing.
        const QStringList steps = sourceBuildSteps(plan, readVendorPath);
        const QString cmd = sourceBuildCommand(steps);
        // Stream like the source pull: pty for soong/ninja progress, everything to a log file.
        // Success is judged by the SENTINEL the wrapped chain prints, never by grepping build
        // output for errors — and no longer by the wrapper's exit code either, which lies when
        // the wrapper itself is killed (`script` exits 0 having killed the shell mid-build,
        // bd remora-c7y).
        const QStringList argv = ptyRunArgv(cmd, host);
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        posixShellEnv(p);
#ifdef Q_OS_UNIX
        p.setChildProcessModifier([] { ::setpgid(0, 0); });  // closeEvent can kill the group
#endif
        QFile log(QStringLiteral("/tmp/remora-build.log"));
        const bool blogOk =
            log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        if (!blogOk) log.setFileName(QString());
        QElapsedTimer clock;
        clock.start();
        p.start(argv.first(), argv.mid(1));
        if (!p.waitForStarted(10000)) {
            QMetaObject::invokeMethod(this, [this] {
                onState(QStringLiteral("build-from-source"), QStringLiteral("failed"),
                        QStringLiteral("could not start"), QString());
                setRunning(false);
            }, Qt::QueuedConnection);
            return;
        }
        pullChildPid_.store(p.processId());
        QString lastLine, progressLine;
        qint64 lastEmit = -1;
        const auto elapsed = [&clock] {
            const qint64 s = clock.elapsed() / 1000;
            return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10,
                                                                QLatin1Char('0'))
                .arg(s % 60, 2, 10, QLatin1Char('0'));
        };
        const QRegularExpression lineSplit(QStringLiteral("[\\r\\n]"));
        const QRegularExpression ansi(QStringLiteral("\\x1b\\[[0-9;?]*[A-Za-z]"));
        const QRegularExpression ninjaPct(QStringLiteral("^\\[\\s*\\d+% "));  // "[ 45% 12000/27000]"
        const QRegularExpression syncPct(QStringLiteral("\\d+% \\(\\d+/\\d+\\)"));  // repo sync
        QString vTail;  // for the exit sentinel — kept off lastLine so the row shows real output
        while (p.state() != QProcess::NotRunning) {
            if (p.waitForReadyRead(1000)) {
                QString chunk = QString::fromUtf8(p.readAll());
                chunk.remove(ansi);
                log.write(chunk.toUtf8());
                log.flush();
                vTail = (vTail + chunk).right(2048);
                for (const QString &ln : chunk.split(lineSplit, Qt::SkipEmptyParts)) {
                    const QString t = ln.trimmed();
                    if (t.isEmpty() || t.startsWith(QLatin1String(kPtyExitSentinel))) continue;
                    lastLine = t;
                    if (ninjaPct.match(t).hasMatch() || syncPct.match(t).hasMatch()
                        || t.startsWith(QLatin1String("Fetching:"))
                        || t.startsWith(QLatin1String("Checking out:")))
                        progressLine = t;
                }
            }
            if (clock.elapsed() / 1000 != lastEmit) {
                lastEmit = clock.elapsed() / 1000;
                const QString detail =
                    QStringLiteral("[%1] %2").arg(elapsed(),
                                                  progressLine.isEmpty() ? lastLine : progressLine);
                QMetaObject::invokeMethod(this, [this, detail] {
                    onState(QStringLiteral("build-from-source"), QStringLiteral("running"), detail,
                            QString());
                }, Qt::QueuedConnection);
            }
        }
        {   // drain what landed between the last read and exit — the sentinel is the LAST line,
            // so without this every finished build would read as interrupted
            QString chunk = QString::fromUtf8(p.readAll());
            chunk.remove(ansi);
            log.write(chunk.toUtf8());
            vTail = (vTail + chunk).right(2048);
            for (const QString &ln : chunk.split(lineSplit, Qt::SkipEmptyParts)) {
                const QString t = ln.trimmed();
                if (!t.isEmpty() && !t.startsWith(QLatin1String(kPtyExitSentinel))) lastLine = t;
            }
        }
        log.close();
        pullChildPid_.store(0);
        // Judged by the chain's sentinel, with the wrapper's code only as the fallback for a run
        // that never got to report (bd remora-c7y — a killed `script` exits 0 mid-build).
        const int wrapperRc = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 128;
        const PtyExit ex = judgePtyExit(wrapperRc, vTail);
        int rc = ex.rc;
        const bool interrupted = ex.interrupted;
        // OK must also mean the tag exists — the chain ends in assemble-image.sh, so a truthful 0
        // always leaves the image behind; a 0 without the image is a verdict we refuse.
        QString noImage;
        if (!interrupted && rc == 0) {
            QProcess chk;
            if (host.isEmpty())
                chk.start(QStringLiteral("docker"),
                          {QStringLiteral("image"), QStringLiteral("inspect"), builtTag});
            else
                chk.start(QStringLiteral("ssh"),
                          {host, QStringLiteral("docker image inspect ") + builtTag});
            if (!chk.waitForStarted(10000) || !chk.waitForFinished(60000)
                || chk.exitStatus() != QProcess::NormalExit || chk.exitCode() != 0) {
                rc = 1;
                noImage = QStringLiteral(" — chain exited 0 but image %1 does not exist")
                              .arg(builtTag);
            }
        }
        const QString took = elapsed();
        // Ask the journal HERE, on the worker thread — the GUI thread must not block on it.
        QString oomd = oomdKillNote(rc);
        // An action killed by the CONTAINER's cgroup cap leaves the container alive, so ninja fails
        // and the outer code is 1 — oomdKillNote cannot fire, and the log line reads like a
        // toolchain error (bd remora-4ei.30). Say what it actually is, and name the timezone trap
        // that made the first investigation conclude there had been no OOM at all.
        if (rc != 0 && oomd.isEmpty()) {
            QFile lf(QStringLiteral("/tmp/remora-build.log"));
            if (lf.open(QIODevice::ReadOnly)) {
                // tail only: the signature is at the failure, and these logs run to hundreds of MB
                const qint64 tail = 512 * 1024;
                if (lf.size() > tail) lf.seek(lf.size() - tail);
                const QString tailText = QString::fromUtf8(lf.readAll());
                if (parseReadOnlyTree(tailText))
                    oomd = QStringLiteral(
                        " — THE SOURCE TREE WENT READ-ONLY mid-build. Not a build error, and "
                        "retrying will not help: if it is btrfs it latched after a failed write "
                        "barrier. Check `btrfs device stats` first (a lone flush timeout with "
                        "wr/rd/corrupt at 0 is NOT a failing drive), then unmount fully and mount "
                        "again — remount,rw will not clear it (bd remora-4ei.20)");
                else if (parseInnerOomKill(tailText))
                    oomd = QStringLiteral(
                        " — OOM-KILLED INSIDE THE CONTAINER (a build action took SIGKILL; the "
                        "container itself survived, which is why this is exit %1 and not 137). This "
                        "is the --memory cap, NOT systemd-oomd: check `journalctl -k` for "
                        "\"Memory cgroup out of memory\". NOTE the build log stamps UTC while the "
                        "journal stamps local time, so searching it with a build-log timestamp "
                        "finds nothing and looks like there was no OOM. Raise build_mem_gib on the "
                        "Image page, or lower NINJA_HIGHMEM_NUM_JOBS — see bd remora-4ei.30")
                            .arg(rc);
            }
        }
        QMetaObject::invokeMethod(this, [this, rc, lastLine, took, oomd, interrupted, noImage] {
            onState(QStringLiteral("build-from-source"),
                    rc == 0 ? QStringLiteral("ok") : QStringLiteral("failed"),
                    rc == 0 ? QStringLiteral("source build complete in %1 · "
                                             "log: /tmp/remora-build.log").arg(took)
                    : interrupted
                        ? QStringLiteral("build INTERRUPTED after %1 (exit %2) — the pty closed "
                                         "before the build reported a status; the detached "
                                         "builder may still be compiling (`docker ps`) · "
                                         "log: /tmp/remora-build.log").arg(took).arg(rc)
                        : QStringLiteral("build failed after %1 (exit %2): %3%4%5 · "
                                         "log: /tmp/remora-build.log")
                              .arg(took).arg(rc).arg(lastLine, oomd, noImage),
                    QString());
            setRunning(false);
            if (rc == 0) onMarkBuilt();
            refreshSourceState();
            onRefreshArchives();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::onCleanSourceOutput() {
    const QString host = sourceHostStr();
    const QString out = sourceTreePath() + QStringLiteral("/out");
    reclaimDir(host, out, QStringLiteral("Clean build output"),
               QStringLiteral("Remove the source build output at %1 (%2)? The source tree is kept — "
                              "only the compiled output/cache is deleted; the next source build is "
                              "a full rebuild."),
               QStringLiteral("no build output at %1").arg(out));
}

// One row per profile: resolved image + editable docker host + Start/Connect/Stop. Actions load
// that profile's config fresh (without switching the active profile) and drive it through the same
// engine workers as the toolbar, streaming step states to the Progress page.
// Shared add/edit dialog for a deployment. Prefilled from `d` when editing; writes the user's
// choices back into `d` on accept.
bool MainWindow::deploymentDialog(Deployment &d, bool isNew) {
    const QStringList profiles = listInstances(path_);
    if (profiles.isEmpty()) return false;
    QDialog dlg(this);
    dlg.setWindowTitle(isNew ? QStringLiteral("Add deployment")
                             : QStringLiteral("Edit deployment"));
    auto *form = new QFormLayout(&dlg);
    auto *profCombo = new QComboBox(&dlg);
    profCombo->addItems(profiles);
    auto *nameEdit = new QLineEdit(&dlg);
    nameEdit->setToolTip(QStringLiteral("A label; reuse a profile on several hosts."));
    auto *hostEdit = new QLineEdit(&dlg);
    hostEdit->setPlaceholderText(QStringLiteral("blank = the profile's own ssh host"));
    auto *imgCombo = new QComboBox(&dlg);
    imgCombo->setToolTip(QStringLiteral(
        "The image this deployment boots — only images already present on the deployment's "
        "docker host are offered. '(profile default)' follows the profile's Image page "
        "selection; picking one here overrides it for this deployment only."));
    form->addRow(QStringLiteral("Profile"), profCombo);
    form->addRow(QStringLiteral("Name"), nameEdit);
    form->addRow(QStringLiteral("Docker host"), hostEdit);
    form->addRow(QStringLiteral("Image"), imgCombo);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    // image choices + suggested name track the selected profile; name stops tracking once edited
    // (or immediately when editing an existing deployment — its name is the user's label)
    bool nameEdited = !isNew;
    connect(nameEdit, &QLineEdit::textEdited, &dlg, [&nameEdited] { nameEdited = true; });
    const QString keepImage = d.image;  // reselect after the async scan fills the combo
    const auto refresh = [this, profCombo, nameEdit, imgCombo, &nameEdited, keepImage] {
        const QString prof = profCombo->currentText();
        if (!nameEdited) nameEdit->setText(prof);
        const RemoraConfig cfg = loadInstance(path_, prof);
        imgCombo->clear();
        imgCombo->addItem(QStringLiteral("(profile default — the Image page selection)"),
                          QString());
        if (!keepImage.isEmpty()) {  // current override stays selectable before the scan lands
            const QString nm = imageCustomName(path_, keepImage);
            const QString disp = nm.isEmpty() ? keepImage : nm + QStringLiteral("  ·  ") + keepImage;
            imgCombo->addItem(disp + QStringLiteral("  (current)"), keepImage);
            imgCombo->setCurrentIndex(1);
        }
        // only images actually present on the deployment's docker host (live async scan) —
        // registry offers that were never pulled don't belong in a "boot this now" picker
        const Backend backend = inferBackend(cfg);
        const QString host =
            backend == Backend::Bare ? QString() : cfg.backend.sshHost.value_or(QString());
        const int ver = cfg.image.androidVersion.value_or(16);
        QPointer<QComboBox> combo(imgCombo);
        const QString settings = path_;
        auto *scan = QThread::create([this, combo, host, ver, settings] {
            const QList<DockerImage> images = scanDockerImages(host);
            QMetaObject::invokeMethod(this, [combo, images, ver, settings] {
                if (!combo) return;  // dialog already closed
                for (const DockerImage &im : images) {
                    if (im.tag.contains(QLatin1String("builder"))) continue;
                    const int v = tagAndroidVersion(im.tag);
                    if (v > 0 && v != ver) continue;
                    if (combo->findData(im.tag) >= 0) continue;  // e.g. the current override
                    // show any custom name prepended (matches the Built-images list); the raw tag
                    // stays the item's data, so the deployment stores the tag, not the label
                    const QString nm = imageCustomName(settings, im.tag);
                    const QString disp =
                        nm.isEmpty() ? im.tag : nm + QStringLiteral("  ·  ") + im.tag;
                    combo->addItem(QStringLiteral("%1  (%2)").arg(disp, im.size), im.tag);
                }
            }, Qt::QueuedConnection);
        });
        connect(scan, &QThread::finished, scan, &QObject::deleteLater);
        scan->start();
    };
    if (!isNew) {
        profCombo->setCurrentIndex(qMax(0, profCombo->findText(d.profile)));
        nameEdit->setText(d.name);
        hostEdit->setText(d.host);
    } else {
        // A new runner defaults to the ACTIVE profile, not list-index 0. Otherwise adding a runner
        // while working on 'Android 17' silently creates one bound to 'Android 16' (the first
        // profile alphabetically) — the runner then deploys A16 no matter its label. The name
        // auto-tracks this profile via refresh() below, and the run row shows "name [profile]"
        // whenever they diverge, so a mismatch is always visible.
        profCombo->setCurrentIndex(qMax(0, profCombo->findText(instance_)));
    }
    refresh();
    connect(profCombo, &QComboBox::currentIndexChanged, &dlg, refresh);
    if (dlg.exec() != QDialog::Accepted) return false;
    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) return false;
    d.name = name;
    d.profile = profCombo->currentText();
    d.host = hostEdit->text().trimmed();
    d.image = imgCombo->currentData().toString();
    return true;
}

void MainWindow::onAddDeployment() {
    Deployment d;
    if (!deploymentDialog(d, true)) return;
    QList<Deployment> deps = loadDeployments(path_);
    deps << d;
    saveDeployments(path_, deps);
    rebuildRunTable();
}

void MainWindow::onEditDeployment() {
    auto *item = runTree_ ? runTree_->currentItem() : nullptr;
    const QString name = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (name.isEmpty()) return;
    QList<Deployment> deps = loadDeployments(path_);
    for (Deployment &d : deps) {
        if (d.name != name) continue;
        if (deploymentDialog(d, false)) {  // edits in place; rename included
            saveDeployments(path_, deps);
            rebuildRunTable();
        }
        return;
    }
}

void MainWindow::onRemoveDeployment() {
    auto *item = runTree_ ? runTree_->currentItem() : nullptr;
    const QString name = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (name.isEmpty()) return;
    QList<Deployment> deps = loadDeployments(path_);
    deps.erase(std::remove_if(deps.begin(), deps.end(),
                              [&](const Deployment &d) { return d.name == name; }),
               deps.end());
    saveDeployments(path_, deps);
    rebuildRunTable();
}

void MainWindow::rebuildRunTable() {
    if (!runTree_) return;
    runTree_->clear();  // deletes the row widgets, so the button list must be rebuilt with them
    runButtons_.clear();
    // Seed one deployment per profile the first time (so the table isn't empty), then persist.
    // 'First time' means never-saved — a user who deleted every runner keeps an empty table.
    QList<Deployment> deps = loadDeployments(path_);
    const bool initialized = QSettings(path_, QSettings::IniFormat)
                                 .value(QStringLiteral("Remora/deployments_initialized"))
                                 .toBool();
    if (deps.isEmpty() && !initialized) {
        for (const QString &n : listInstances(path_)) {
            RemoraConfig cfg = loadInstance(path_, n);
            deps << Deployment{n, n, cfg.backend.sshHost.value_or(QString()), {}};
        }
        saveDeployments(path_, deps);
    }
    // ONE-TIME: adopt each profile's stored ssh host into rows that never had one of their own,
    // now that the row is authoritative. Guarded by a flag rather than re-run per rebuild —
    // otherwise clearing a fan-out row's host would silently inherit it straight back.
    {
        QSettings s(path_, QSettings::IniFormat);
        const QString key = QStringLiteral("Remora/deployment_hosts_migrated");
        if (!s.value(key).toBool()) {
            bool changed = false;
            for (Deployment &d : deps) {
                if (!d.host.isEmpty()) continue;
                const QString ph =
                    loadInstance(path_, d.profile).backend.sshHost.value_or(QString()).trimmed();
                if (ph.isEmpty()) continue;
                d.host = ph;
                changed = true;
            }
            if (changed) saveDeployments(path_, deps);
            s.setValue(key, true);
            noteRemorarcWritten();
        }
    }
    for (const Deployment &d : deps) {
        RemoraConfig cfg = loadInstance(path_, d.profile);
        const Backend backend = inferBackend(cfg);
        const ResolvedConfig rc = resolve(cfg, backend);
        auto *item = new QTreeWidgetItem(runTree_);
        item->setText(0, d.name + (d.name == d.profile ? QString()
                                                       : QStringLiteral(" [%1]").arg(d.profile)));
        // An image OVERRIDE must not look like the profile's own resolution. Connecting with a
        // tag the running container was not created from makes provision `docker rm -f` it and
        // rebuild — so a pin left over from an older resolution silently destroys the container
        // and boots a different image, which reads as "connect didn't work".
        const bool pinned = !d.image.isEmpty();
        const bool diverged = pinned && d.image != rc.imageTag;
        item->setText(1, pinned ? (diverged ? d.image + QStringLiteral("  (override)") : d.image)
                                : rc.imageTag);
        // the image column elides under Stretch — keep the full tag reachable on every row
        item->setToolTip(1, item->text(1));
        if (diverged) {
            item->setToolTip(1, QStringLiteral(
                "This deployment PINS image '%1', but profile '%2' now resolves to '%3'.\n\n"
                "Connecting from this row deploys the pinned image, and because the running "
                "container was created from a different one it will be removed and recreated — a "
                "full reboot, not a connect.\n\nEdit → Image → '(profile default)' to follow the "
                "profile instead.").arg(d.image, d.profile, rc.imageTag));
            item->setForeground(1, QBrush(QColor(0xb0, 0x60, 0x00)));
        }
        item->setText(3, QStringLiteral("—"));
        // Verbatim, with NO fallback to the profile's stored ssh host: runProfile treats this
        // cell as authoritative including empty, so showing an inherited host here would both
        // lie about where Connect goes and silently undo a clear (bd remora-60x).
        auto *hostEdit = new QLineEdit(d.host);
        // Always editable: the host IS the deploy target now — blank runs on this machine,
        // user@host deploys the same profile there.
        hostEdit->setPlaceholderText(QStringLiteral("(local)"));
        hostEdit->setToolTip(QStringLiteral(
            "Where this deployment runs: blank = this machine's docker, user@host = that "
            "machine's docker over ssh."));
        // persist a host edit back onto the deployment
        const QString depName = d.name;
        connect(hostEdit, &QLineEdit::editingFinished, this, [this, depName, hostEdit] {
            const QString h = hostEdit->text().trimmed();
            QList<Deployment> ds = loadDeployments(path_);
            QString prof;
            for (Deployment &x : ds)
                if (x.name == depName) {
                    x.host = h;
                    prof = x.profile;
                }
            saveDeployments(path_, ds);
            int rows = 0;
            for (const Deployment &x : ds)
                if (x.profile == prof) ++rows;
            // With a single deployment the row IS that profile's host, so mirror it into the
            // profile — otherwise the profile's own operations (app menu, device storage,
            // certification, updates) keep targeting the machine the user just cleared.
            // Fan-out rows (same profile, several hosts) deliberately stay row-local.
            if (rows == 1 && !prof.isEmpty()) {
                RemoraConfig pc = loadInstance(path_, prof);
                pc.backend.sshHost = h.isEmpty() ? std::nullopt : std::optional<QString>(h);
                saveInstance(path_, prof, pc);
                noteRemorarcWritten();
                if (prof == instance_) cfg_.backend.sshHost = pc.backend.sshHost;
            }
        });
        runTree_->setItemWidget(item, 2, hostEdit);
        auto *actions = new QWidget();
        auto *al = new QHBoxLayout(actions);
        al->setContentsMargins(0, 0, 0, 0);
        al->setSpacing(2);
        // The full session-action set, per deployment: ▶ 🖵 📷 ⏺ ⏸ ⏹. Everything the old
        // toolbar offered, but aimed at THIS row's profile/host/image instead of the active one.
        auto *connBtn = new QPushButton(themeIcon({"media-playback-start", "network-connect"}),
                                        QString());
        connBtn->setToolTip(QStringLiteral(
            "Connect — attaches to the running container, or deploys + boots it first if needed"));
        auto *desktopBtn = new QPushButton(themeIcon({"video-display", "view-fullscreen"}),
                                           QString());
        desktopBtn->setToolTip(QStringLiteral(
            "Desktop mode — opens this deployment's fullscreen session (a separate "
            "desktop-resolution display when the profile's Viewer page opts in)"));
        // An OUTLINED image frame, not a camera: accent tinting fills the whole silhouette, and
        // camera-photo's solid body flattened into the same filled rectangle as Stop. Outline vs
        // solid is what keeps these two apart at 16px.
        auto *shotBtn = new QPushButton(themeIcon({"view-preview", "image-crop-symbolic",
                                                   "camera-photo"}),
                                        QString());
        shotBtn->setToolTip(QStringLiteral(
            "Screenshot — save what this device shows to Pictures (works with or without the "
            "mirror)"));
        auto *recBtn = new QPushButton(themeIcon({"media-record", "media-record-symbolic"}),
                                       QString());
        recBtn->setCheckable(true);
        recBtn->setChecked(d.name == recordingDep_);
        recBtn->setToolTip(QStringLiteral(
            "Record — relaunch this deployment's mirror recording the session to Videos (the "
            "recorded session replaces the running mirror). Toggle off — or close the mirror "
            "window — to finish the mp4."));
        auto *sleepBtn = new QPushButton(themeIcon({"media-playback-pause", "system-suspend"}),
                                         QString());
        sleepBtn->setToolTip(QStringLiteral(
            "Sleep — freeze the device in place (zero CPU, session preserved); "
            "Connect wakes it in ~1s instead of a full boot"));
        auto *stopBtn = new QPushButton(themeIcon({"media-playback-stop", "process-stop"}),
                                        QString());
        stopBtn->setToolTip(QStringLiteral("Stop the container"));
        const QString prof = d.profile;
        const QString img = d.image;
        // depName was declared above for the host-edit persistence lambda; reused here
        connect(connBtn, &QPushButton::clicked, this, [this, prof, hostEdit, img] {
            runProfile(prof, hostEdit->text().trimmed(), RunConnect, img);
        });
        connect(desktopBtn, &QPushButton::clicked, this, [this, prof, hostEdit, img] {
            runProfile(prof, hostEdit->text().trimmed(), RunDesktop, img);
        });
        connect(shotBtn, &QPushButton::clicked, this, [this, prof, hostEdit] {
            shotProfile(prof, hostEdit->text().trimmed());
        });
        connect(recBtn, &QPushButton::clicked, this,
                [this, depName, prof, hostEdit, img](bool on) {
                    recordProfile(depName, prof, hostEdit->text().trimmed(), img, on);
                });
        connect(sleepBtn, &QPushButton::clicked, this, [this, prof, hostEdit, img] {
            runProfile(prof, hostEdit->text().trimmed(), RunSleep, img);
        });
        connect(stopBtn, &QPushButton::clicked, this, [this, prof, hostEdit, img] {
            runProfile(prof, hostEdit->text().trimmed(), RunStop, img);
        });
        // compact icon squares — six of them per row at Breeze's default padding would
        // out-width the table
        for (QPushButton *b : {connBtn, desktopBtn, shotBtn, recBtn, sleepBtn, stopBtn}) {
            b->setFixedSize(30, 28);
            al->addWidget(b);
        }
        // Screenshot stays clickable during a run (a read-only capture can't disturb a deploy);
        // everything else is gated with the rest of the run buttons.
        for (QPushButton *b : {connBtn, desktopBtn, recBtn, sleepBtn, stopBtn}) {
            b->setEnabled(!running_);
            runButtons_ << b;
        }
        runTree_->setItemWidget(item, 4, actions);
        item->setData(0, Qt::UserRole, d.name);
        item->setData(0, Qt::UserRole + 1, d.profile);
    }
    // Size the actions column to the six buttons: ResizeToContents ignores item widgets, and
    // the default column width crushes them into slivers.
    if (runTree_->topLevelItemCount() > 0) {
        if (QWidget *aw = runTree_->itemWidget(runTree_->topLevelItem(0), 4))
            runTree_->setColumnWidth(4, aw->sizeHint().width() + 12);
    }
    probeRunStatuses();
}

// Set one deployment row's status chip (styled via the [pill=…] rules in applyAccentStyle).
void MainWindow::setRunPill(QTreeWidgetItem *it, const QString &text, const QString &kind,
                            const QString &tooltip) {
    auto *pill = new QLabel(text);
    pill->setProperty("pill", kind);
    pill->setToolTip(tooltip);  // the "unknown" chip carries docker's own line here
    pill->setAlignment(Qt::AlignCenter);
    auto *wrap = new QWidget();
    auto *wl = new QHBoxLayout(wrap);
    wl->setContentsMargins(2, 1, 2, 1);
    wl->addWidget(pill);
    wl->addStretch(1);
    it->setText(3, QString());
    runTree_->setItemWidget(it, 3, wrap);
    // The column was Qt's 100 px default, sized by luck for "● running"/"○ stopped"; the third
    // chip ("? docker unreachable") clipped to "cker unreach". Grow to the widest chip seen, as the
    // actions column already does for its buttons.
    runTree_->setColumnWidth(3, qMax(runTree_->columnWidth(3), wrap->sizeHint().width() + 6));
}

// A transitory chip ("starting…", "stopping…") on every row of a profile, shown the moment an
// action is clicked — the truth arrives with the next probe, which overwrites it.
void MainWindow::markDeploymentsBusy(const QString &profile, const QString &verb) {
    if (!runTree_) return;
    for (int i = 0; i < runTree_->topLevelItemCount(); ++i) {
        auto *it = runTree_->topLevelItem(i);
        if (it->data(0, Qt::UserRole + 1).toString() == profile)
            setRunPill(it, verb, QStringLiteral("busy"));
    }
}

// Async status sweep (docker ps per deployment host), updating the chips IN PLACE — no row
// rebuild, so it is safe to run on a timer while the user is mid-edit in a host field.
void MainWindow::probeRunStatuses() {
    if (!runTree_) return;
    const QList<Deployment> deps = loadDeployments(path_);
    const QString settingsPath = path_;
    auto *thread = QThread::create([this, deps, settingsPath] {
        // Three answers, not two: up, down, and "docker could not be asked" — which used to be
        // reported as ○ stopped, on the reasoning that an empty `docker ps` has no such container.
        // A daemon that is down, a socket that is gone or a host that dropped off ssh all produced
        // that empty output, and the row said the device was stopped while it ran.
        struct RowState { bool up = false; QString dockerError; };
        QHash<QString, RowState> states;
        for (const Deployment &d : deps) {
            RemoraConfig cfg = loadInstance(settingsPath, d.profile);
            // the row's host decides the target, exactly as runProfile applies it — empty
            // means local, so it must clear the profile's stored host rather than inherit it
            cfg.backend.sshHost = d.host.trimmed().isEmpty()
                                      ? std::nullopt
                                      : std::optional<QString>(d.host.trimmed());
            const Backend backend = inferBackend(cfg);
            const ResolvedConfig rc = resolve(cfg, backend);
            const QString host =
                backend == Backend::Bare ? QString() : cfg.backend.sshHost.value_or(QString());
            const ShellResult ps = runShellRc(
                host, QStringLiteral("docker ps --filter name=^%1$ --format '{{.Names}}' 2>&1")
                          .arg(rc.containerName),
                12000);
            RowState st;
            if (ps.rc != 0) {
                st.dockerError = ps.out.trimmed().section(QLatin1Char('\n'), 0, 0);
                if (st.dockerError.isEmpty())
                    st.dockerError = QStringLiteral("docker ps exited %1").arg(ps.rc);
            } else {
                st.up = ps.out.trimmed() == rc.containerName;
            }
            states.insert(d.name, st);
        }
        QMetaObject::invokeMethod(
            this,
            [this, states] {
                // a chain in flight owns the chips — its transitory verbs must not be
                // overwritten with a pre-action snapshot
                if (running_) return;
                for (int i = 0; i < runTree_->topLevelItemCount(); ++i) {
                    auto *it = runTree_->topLevelItem(i);
                    const QString n = it->data(0, Qt::UserRole).toString();
                    if (!states.contains(n)) continue;
                    const RowState st = states.value(n);
                    if (!st.dockerError.isEmpty())
                        setRunPill(it, QStringLiteral("? docker unreachable"),
                                   QStringLiteral("unknown"), st.dockerError);
                    else
                        setRunPill(it, st.up ? QStringLiteral("● running")
                                             : QStringLiteral("○ stopped"),
                                   st.up ? QStringLiteral("up") : QStringLiteral("down"));
                }
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::runProfile(const QString &name, const QString &hostOverride, int action,
                            const QString &imageOverride) {
    // The Run-table row buttons stay clickable while a chain runs (setRunning only disables
    // the session actions) — without this guard a Connect can race a Deploy on the same target
    // (adb retries against a container mid-recreate) and both chains interleave in the log.
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"), 5000);
        return;
    }
    RemoraConfig cfg = loadInstance(path_, name);
    // the override is authoritative INCLUDING empty: a cleared host cell means local,
    // exactly as its "(local)" placeholder promises
    cfg.backend.sshHost = hostOverride.trimmed().isEmpty()
                              ? std::nullopt
                              : std::optional<QString>(hostOverride.trimmed());
    if (!imageOverride.isEmpty()) cfg.image.imageTag = imageOverride;
    const Backend backend = inferBackend(cfg);  // AFTER the override: the host decides the type
    // only the actions that launch a mirror client need an encoder in the image
    const bool launchesViewer =
        action == RunStart || action == RunConnect || action == RunDesktop;
    if (launchesViewer && !gateVideoCodec(cfg, backend)) return;
    const QString title = name;  // the mirror window is titled with just the profile name
    checklist_->clear();
    setRunning(true);
    statusBar()->showMessage(QStringLiteral("%1 '%2'…")
                                 .arg(action == RunStop      ? QStringLiteral("stopping")
                                      : action == RunConnect ? QStringLiteral("connecting")
                                      : action == RunSleep   ? QStringLiteral("freezing")
                                      : action == RunDesktop
                                          ? QStringLiteral("opening desktop display for")
                                          : QStringLiteral("starting"),
                                      name));
    // the row's chip flips to the verb immediately; the next status probe writes the truth
    markDeploymentsBusy(name, action == RunStop    ? QStringLiteral("stopping…")
                              : action == RunSleep ? QStringLiteral("sleeping…")
                                                   : QStringLiteral("starting…"));
    if (action == RunStop) {
        const ResolvedConfig rc = resolve(cfg, backend);
        auto *w = new StopWorker(backend, hostOverride, rc.containerName, rc.target, this);
        connect(w, &StopWorker::sigDone, this, [this](bool) {
            setRunning(false);
            rebuildRunTable();
        });
        connect(w, &StopWorker::finished, w, &QObject::deleteLater);
        w->start();
        return;
    }
    auto *w = new EngineWorker(backend, cfg, title,
                               action == RunDesktop  ? EngineWorker::Fullscreen
                               : action == RunSleep  ? EngineWorker::Sleep
                                                     : EngineWorker::Smart,
                               this);
    // After a successful Connect/Start, run the app-update sweep if the profile opts in and it
    // is due (Settings → Desktop integration). Queued deliberately: by the time the hook runs,
    // onDone (connected below) has already cleared running_, so the sweep isn't refused.
    if (action == RunConnect || action == RunStart) {
        const QString prof = name;
        connect(w, &EngineWorker::sigDone, this, [this, prof](bool ok) {
            if (ok) QMetaObject::invokeMethod(this,
                                              [this, prof] {
                                                  runArmGuard(prof);
                                                  maybeAutoUpdateApps(prof);
                                              },
                                              Qt::QueuedConnection);
        });
    }
    connect(w, &EngineWorker::sigState, this, &MainWindow::onState);
    connect(w, &EngineWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &EngineWorker::sigConflict, this, &MainWindow::onConflict);
    connect(w, &EngineWorker::sigMenuSynced, this, [this](bool) { rebuildIntegrationsList(); });
    connect(w, &EngineWorker::sigDone, this, [this](bool ok) {
        onDone(ok);
        rebuildRunTable();
    });
    connect(w, &EngineWorker::finished, w, &QObject::deleteLater);
    w->start();
}


// Record the source dir's current HEAD as the built commit (called after a source build so a
// later pull correctly flags PENDING). Async: read HEAD on the source host, save, refresh.
void MainWindow::onMarkBuilt() {
    const QString host = sourceHostStr();
    const QString dir = sourceTreePath();
    auto *thread = QThread::create([this, host, dir] {
        const QString head = runShell(
            host,
            // VALIDATE the repo, don't just stat .git. A source tree can carry a HUSK .git —
            // a directory holding only info/attributes, manufactured by the union-merge step
            // (git rev-parse --git-path returns a relative ".git/info/attributes" outside a repo
            // and the following mkdir -p creates it). `[ -d .git ]` was true for that husk, so
            // this never fell through to .repo/manifests, HEAD came back empty, and the caller
            // silently recorded nothing (bd remora-eoy.11).
            QStringLiteral("g=%1; if git -C \"$g\" rev-parse --git-dir >/dev/null 2>&1; then :; "
                           "elif [ -d \"$g/.repo/manifests\" ]; then g=\"$g/.repo/manifests\"; "
                           "else exit 0; fi; git -C \"$g\" rev-parse --short HEAD 2>/dev/null")
                .arg(dir),
            20000).trimmed();
        QMetaObject::invokeMethod(
            this,
            [this, head] {
                // The two markers have DIFFERENT prerequisites, and treating them as one is what
                // left built_recipe stale through a whole successful build+package (bd
                // remora-eoy.11): builtCommit needs a resolvable git HEAD, builtRecipe does not —
                // it fingerprints patches, features, version and ref, none of which involve git.
                // So record the recipe unconditionally and gate only the commit.
                if (!head.isEmpty()) cfg_.image.builtCommit = head;
                // Fingerprint the build inputs (patch files, feature payloads, recipe knobs):
                // updateBuildImageButton() hides the Build button while the current inputs still
                // match — a rebuild would be identical.
                {
                    QStringList pats = cfg_.image.sourcePatches;
                    if (pats.isEmpty())
                        pats = defaultSourcePatchSet(cfg_.image.androidVersion.value_or(16));
                    QSet<QString> feats(cfg_.image.features.begin(), cfg_.image.features.end());
                    if (feats.isEmpty()) feats = defaultBuildSet();
                    cfg_.image.builtRecipe = imageBuildFingerprint(
                        pats, QStringList(feats.begin(), feats.end()),
                        cfg_.image.androidVersion.value_or(16),
                        cfg_.image.sourceRef.value_or(QString()));
                }
                saveInstance(path_, instance_, cfg_);
                noteRemorarcWritten();
                statusBar()->showMessage(
                    head.isEmpty()
                        ? QStringLiteral("recorded build inputs (no git HEAD to mark)")
                        : QStringLiteral("marked %1 as built").arg(head),
                    4000);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}


QLineEdit *MainWindow::addBrowseDir(QFormLayout *form, const QString &label,
                                    const QString &placeholder) {
    auto *edit = new QLineEdit();
    edit->setPlaceholderText(placeholder);
    auto *browse = new QPushButton(themeIcon({"folder-open", "document-open-folder"}), QString());
    browse->setToolTip(QStringLiteral("Browse…"));
    connect(browse, &QPushButton::clicked, this, [this, edit] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select directory"),
            edit->text().isEmpty() ? QDir::homePath() : edit->text());
        if (dir.isEmpty()) return;
        edit->setText(dir);
        // setText() is a PROGRAMMATIC change and Qt does not emit editingFinished for it — that
        // signal fires only on Return/Enter or focus-out after a USER edit. Every listener on
        // these fields hangs off editingFinished: autosave() via wireAutosave()'s
        // findChildren<QLineEdit*>, plus recompute() and onRefreshArchives() on the source dir.
        // So a directory chosen with Browse updated the widget, was used by the running build,
        // and was NEVER written to remorarc — it silently reverted on the next GUI restart. That
        // cost a full build against the old source tree (bd remora-4ei.27). Emitting it here is
        // literally true — editing did finish — and keeps Browse and typing on one code path for
        // every browse field, not just the source directory.
        emit edit->editingFinished();
    });
    auto *row = new QWidget();
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(edit, 1);
    rl->addWidget(browse);
    form->addRow(label, row);
    return edit;
}

void MainWindow::rebuildProfilesList() {
    if (!profilesTree_) return;
    profilesTree_->clear();
    for (const QString &name : listInstances(path_)) {
        // Each row reads ITS profile's saved config — the disk copy is authoritative for every
        // row including the active one (autosave keeps it current), same as the Runners rows.
        const RemoraConfig pc = loadInstance(path_, name);
        auto *item = new QTreeWidgetItem(profilesTree_);
        item->setText(0, name == instance_ ? name + QStringLiteral("  (active)") : name);
        item->setData(0, Qt::UserRole, name);
        if (name == instance_) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        // Cell widgets are recreated per rebuild (like the Runners and Apps trees). State is set
        // BEFORE connect, so population never fires the write-back; each handler edits the ROW's
        // profile on disk and mirrors into cfg_ only when the row is the active one.
        auto *menuCb = new QCheckBox();
        menuCb->setChecked(pc.integration.desktopMenu.value_or(true));
        connect(menuCb, &QCheckBox::toggled, this, [this, name](bool on) {
            RemoraConfig c = loadInstance(path_, name);
            // On is the default → store only the opt-out.
            c.integration.desktopMenu = on ? std::nullopt : std::optional<bool>(false);
            saveInstance(path_, name, c);
            noteRemorarcWritten();
            if (name == instance_) {
                cfg_.integration.desktopMenu = c.integration.desktopMenu;
                // acts immediately: off tears the folder down, on regenerates it
                if (on)
                    refreshAppsPage();
                else
                    onRemoveAppMenu();
            } else if (!on) {
                // Off works for ANY row — the folder is plain files keyed on the profile name.
                // On for a non-active profile needs its device up to enumerate apps, so it waits
                // for that profile's next Connect (what the column header says).
                removeAppMenuFolder(name);
            }
        });
        profilesTree_->setItemWidget(item, 1, menuCb);
        auto *updCombo = new QComboBox();
        updCombo->addItem(QStringLiteral("off"), -1);
        updCombo->addItem(QStringLiteral("every Connect"), 0);
        updCombo->addItem(QStringLiteral("at most daily"), 24);
        updCombo->addItem(QStringLiteral("at most weekly"), 168);
        const int hours = pc.integration.autoUpdateApps.value_or(false)
                              ? pc.integration.autoUpdateHours.value_or(24)
                              : -1;
        updCombo->setCurrentIndex(qMax(0, updCombo->findData(hours)));
        connect(updCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, name, updCombo] {
                    const int h = updCombo->currentData().toInt();
                    RemoraConfig c = loadInstance(path_, name);
                    // Off is the default → store only the opt-in, and the frequency only when
                    // it differs from the daily default (as the two-widget card stored it).
                    c.integration.autoUpdateApps = h < 0 ? std::nullopt : std::optional<bool>(true);
                    c.integration.autoUpdateHours =
                        (h < 0 || h == 24) ? std::nullopt : std::optional<int>(h);
                    saveInstance(path_, name, c);
                    noteRemorarcWritten();
                    if (name == instance_) {
                        cfg_.integration.autoUpdateApps = c.integration.autoUpdateApps;
                        cfg_.integration.autoUpdateHours = c.integration.autoUpdateHours;
                    }
                });
        profilesTree_->setItemWidget(item, 2, updCombo);
        auto *guardCb = new QCheckBox();
        guardCb->setChecked(pc.integration.armGuard.value_or(true));
        connect(guardCb, &QCheckBox::toggled, this, [this, name](bool on) {
            RemoraConfig c = loadInstance(path_, name);
            // On is the default → store only the opt-out.
            c.integration.armGuard = on ? std::nullopt : std::optional<bool>(false);
            saveInstance(path_, name, c);
            noteRemorarcWritten();
            if (name == instance_) cfg_.integration.armGuard = c.integration.armGuard;
        });
        profilesTree_->setItemWidget(item, 3, guardCb);
        applyCertCell(item);
    }
}

// Paint a row's certification column from the profileCert_ cache; "—" until a Check has run.
void MainWindow::applyCertCell(QTreeWidgetItem *item) {
    const QString name = item->data(0, Qt::UserRole).toString();
    const auto it = profileCert_.constFind(name);
    if (it == profileCert_.constEnd()) {
        item->setText(4, QStringLiteral("—"));
        item->setForeground(4, QBrush(QColor(0x99, 0x99, 0x99)));
        item->setToolTip(4, QStringLiteral(
            "not checked — select the row and press Check (the container must be running)"));
        return;
    }
    item->setText(4, it->cell);
    item->setForeground(4, QBrush(it->color));
    item->setToolTip(4, it->tip);
}

QTreeWidgetItem *MainWindow::profileItemFor(const QString &name) const {
    if (!profilesTree_) return nullptr;
    for (int i = 0; i < profilesTree_->topLevelItemCount(); ++i)
        if (profilesTree_->topLevelItem(i)->data(0, Qt::UserRole).toString() == name)
            return profilesTree_->topLevelItem(i);
    return nullptr;
}

QString MainWindow::selectedProfileName() const {
    auto *item = profilesTree_ ? profilesTree_->currentItem() : nullptr;
    const QString name = item ? item->data(0, Qt::UserRole).toString() : QString();
    return name.isEmpty() ? instance_ : name;
}

void MainWindow::onLoadProfile() {
    auto *item = profilesTree_ ? profilesTree_->currentItem() : nullptr;
    switchProfile(item ? item->data(0, Qt::UserRole).toString() : QString());
}

// Remove a profile's app-menu folder SYNCHRONOUSLY. The worker used by the active profile's
// menu toggle is async, which is wrong here: rename/delete rewrite remorarc immediately
// afterwards, and a worker still reading the profile it is removing would race that write. This
// is a handful of file deletions, so blocking briefly is the simpler correctness story.
void MainWindow::removeAppMenuFolder(const QString &profile) {
    if (profile.isEmpty()) return;
    RealSpawner sp;
    struct : LogSink {
    } quiet;  // the outcome is visible in the Apps list; a failed removal must not block the rename
    appMenuRemove(sp, profile, quiet);
}

void MainWindow::onRenameProfile() {
    auto *item = profilesTree_ ? profilesTree_->currentItem() : nullptr;
    const QString name = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (name.isEmpty()) return;
    bool ok = false;
    const QString neu = QInputDialog::getText(this, QStringLiteral("Rename profile"),
                                              QStringLiteral("New name:"), QLineEdit::Normal, name,
                                              &ok);
    if (!ok || neu.trimmed().isEmpty() || neu.trimmed() == name) return;
    const QString clean = neu.trimmed();
    RemoraConfig cfg = loadInstance(path_, name);
    // Drop the OLD name's menu folder first. The folder, its per-app .desktop entries and its
    // applications-merged fragment are all keyed on the profile name, so a rename would otherwise
    // strand them — and `apps <profile> --remove` cannot reach them afterwards because the profile
    // it needs no longer exists (bd remora-4ei.77). Done before the rename while `name` still
    // resolves; the new folder is created by the next refresh/connect.
    removeAppMenuFolder(name);
    saveInstance(path_, clean, cfg);
    noteRemorarcWritten();
    deleteInstance(path_, name);
    noteRemorarcWritten();
    // the certification verdict cache is keyed on the name — carry it across the rename
    if (profileCert_.contains(name)) profileCert_.insert(clean, profileCert_.take(name));
    if (instance_ == name) {
        instance_ = clean;
        setActiveInstance(path_, instance_);
        noteRemorarcWritten();
        setWindowTitle(QStringLiteral("Remora"));
    }
    syncProfileWidgets();
}

void MainWindow::onDeleteProfile() {
    auto *item = profilesTree_ ? profilesTree_->currentItem() : nullptr;
    const QString name = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (name.isEmpty()) return;
    if (listInstances(path_).size() <= 1) {
        statusBar()->showMessage(QStringLiteral("can't delete the only profile"), 4000);
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Delete profile"),
                              QStringLiteral("Delete profile '%1'? Its settings are removed from "
                                             "remorarc (built images/containers are untouched).")
                                  .arg(name))
        != QMessageBox::Yes)
        return;
    // Same reason as the rename: once the profile is gone its menu folder is unreachable.
    removeAppMenuFolder(name);
    deleteInstance(path_, name);
    noteRemorarcWritten();
    profileCert_.remove(name);
    if (instance_ == name) {
        instance_ = listInstances(path_).value(0);
        cfg_ = loadInstance(path_, instance_);
        features_ = QSet<QString>(cfg_.image.features.begin(), cfg_.image.features.end());
        if (features_.isEmpty()) features_ = defaultBuildSet(cfg_.image.androidVersion.value_or(16));
        for (auto it = featureChecks_.constBegin(); it != featureChecks_.constEnd(); ++it) {
            QSignalBlocker block(it.value());
            it.value()->setChecked(features_.contains(it.key()));
        }
        loadConfig();
        recompute();
        setActiveInstance(path_, instance_);
        noteRemorarcWritten();
        setWindowTitle(QStringLiteral("Remora"));
    }
    syncProfileWidgets();
}

void MainWindow::syncProfileWidgets() {
    if (profileNameLabel_) profileNameLabel_->setText(instance_);
    rebuildProfilesList();
}

// The profile menu: every profile as a checkable entry (the active one ticked) plus "New
// profile…". Opened from the sidebar profile entry's icon and its ▾ switch button — the whole
// switcher when collapsed to 56px, where a combo box cannot live.
void MainWindow::showProfileMenu() {
    QMenu menu(this);
    for (const QString &n : listInstances(path_)) {
        QAction *a = menu.addAction(n);
        a->setCheckable(true);
        a->setChecked(n == instance_);
        connect(a, &QAction::triggered, this, [this, n] { switchProfile(n); });
    }
    menu.addSeparator();
    QAction *add = menu.addAction(themeIcon({"list-add", "document-new"}),
                                  QStringLiteral("New profile…"));
    connect(add, &QAction::triggered, this, &MainWindow::onNewProfile);
    menu.exec(QCursor::pos());
}

void MainWindow::switchProfile(const QString &name) {
    if (name.isEmpty() || name == instance_) return;
    // save the profile we're leaving, then load the selected one
    applyToConfig();
    saveInstance(path_, instance_, cfg_);
    noteRemorarcWritten();
    instance_ = name;
    cfg_ = loadInstance(path_, instance_);
    features_ = QSet<QString>(cfg_.image.features.begin(), cfg_.image.features.end());
    if (features_.isEmpty()) features_ = defaultBuildSet(cfg_.image.androidVersion.value_or(16));
    for (auto it = featureChecks_.constBegin(); it != featureChecks_.constEnd(); ++it) {
        QSignalBlocker block(it.value());
        it.value()->setChecked(features_.contains(it.key()));
    }
    loadConfig();
    // the library rows belong to the previous profile's hosts — rescan now instead of showing
    // stale entries until a manual Refresh
    onRefreshArchives();
    recompute();
    setActiveInstance(path_, instance_);  // startup resumes the last loaded profile
    noteRemorarcWritten();
    setWindowTitle(QStringLiteral("Remora"));
    syncProfileWidgets();       // sidebar name label + Settings profiles list
    rebuildRunTable();          // re-probes the deployment states for the new active profile
    rebuildIntegrationsList();  // the Apps list shows only the active profile's entries
}

// A new profile starts from the chosen OS + Android version and nothing else — a fresh config
// whose unset fields resolve to the proven per-backend defaults (never a copy of the profile
// currently in use, whose tweaks would silently leak into the new one).
void MainWindow::onNewProfile() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("New profile"));
    auto *form = new QFormLayout(&dlg);
    // OS stays coarse on purpose: 'Android' covers lineage- and aosp-based ROMs alike (the
    // lineage/aosp split lives in Source kind / the prebuilt choice). Linux mobile OS entries
    // will join this list later.
    auto *osCombo = new QComboBox(&dlg);
    osCombo->addItem(QStringLiteral("Android"), QStringLiteral("android"));
    auto *verCombo = new QComboBox(&dlg);
    for (int v : supportedAndroidVersions()) {
        const QString label = v == 8 ? QStringLiteral("8.1") : QString::number(v);
        verCombo->addItem(QStringLiteral("Android %1").arg(label), v);
    }
    verCombo->setCurrentIndex(qMax(0, verCombo->findData(16)));
    auto *nameEdit = new QLineEdit(&dlg);
    form->addRow(QStringLiteral("OS"), osCombo);
    form->addRow(QStringLiteral("Android version"), verCombo);
    form->addRow(QStringLiteral("Profile name"), nameEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    // suggest "Android 13" (deduped) and keep suggesting until the user types their own
    const QStringList existing = listInstances(path_);
    const auto suggest = [&existing, osCombo, verCombo] {
        const int v = verCombo->currentData().toInt();
        const QString base = QStringLiteral("%1 %2").arg(
            osCombo->currentText(),
            v == 8 ? QStringLiteral("8.1") : QString::number(v));
        if (!existing.contains(base)) return base;
        for (int i = 2;; ++i) {
            const QString cand = QStringLiteral("%1 (%2)").arg(base).arg(i);
            if (!existing.contains(cand)) return cand;
        }
    };
    nameEdit->setText(suggest());
    bool edited = false;
    connect(nameEdit, &QLineEdit::textEdited, &dlg, [&edited] { edited = true; });
    const auto refresh = [&edited, &suggest, nameEdit] {
        if (!edited) nameEdit->setText(suggest());
    };
    connect(osCombo, &QComboBox::currentIndexChanged, &dlg, refresh);
    connect(verCombo, &QComboBox::currentIndexChanged, &dlg, refresh);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString clean = nameEdit->text().trimmed();
    if (clean.isEmpty()) return;
    if (existing.contains(clean)) {
        statusBar()->showMessage(QStringLiteral("profile '%1' already exists").arg(clean), 4000);
        return;
    }
    // save the profile we're leaving, then create + switch to the fresh one
    applyToConfig();
    saveInstance(path_, instance_, cfg_);
    noteRemorarcWritten();
    RemoraConfig fresh;
    fresh.image.androidVersion = verCombo->currentData().toInt();
    // sourceKind stays unset — 'Android' spans lineage- and aosp-based ROMs; Source build
    // picks the rebuild flavor if/when the user goes down the source-build path.
    // Container identity is per-profile, pinned at creation (the resolver can't know the profile
    // name): sharing /data across profiles corrupts both Androids, and sharing a docker container
    // name makes concurrent deploys collide. Of the three, only the data dir has a GUI field; the
    // container name and adb port are remorarc-only for now (bd remora-4u4.8). All three are
    // written explicitly, so `remora plan` shows what each profile actually got.
    //
    // The host adb PORT is allocated the same way and for the same reason (bd remora-4u4.2). It
    // used to be left unset, so every profile ever created resolved to 5555 and the second one
    // to deploy failed on the docker port bind. Ports already spoken for are collected from the
    // other profiles; a profile that publishes NO host port is skipped, because counting it would
    // burn a port number for nothing. That is every macvlan profile — the container is addressed
    // on the LAN and nothing is bound here — which used to be spelled "vm" back when macvlan was
    // a vm-only mode (bd remora-400).
    QSet<int> takenPorts;
    for (const QString &other : listInstances(path_)) {
        if (other == clean) continue;
        const RemoraConfig oc = loadInstance(path_, other);
        const Backend ob = inferBackend(oc);
        const ResolvedConfig orc = resolve(oc, ob);
        if (orc.networkMode == QLatin1String("macvlan")) continue;
        takenPorts.insert(orc.hostAdbPort);
    }
    const InstanceIdentity id = allocateInstanceIdentity(clean, QDir::homePath(), takenPorts);
    fresh.backend.dataDir = id.dataDir;
    fresh.backend.containerName = id.containerName;
    fresh.backend.hostAdbPort = id.hostAdbPort;
    saveInstance(path_, clean, fresh);
    noteRemorarcWritten();
    instance_ = clean;
    cfg_ = fresh;
    features_ = defaultBuildSet(cfg_.image.androidVersion.value_or(16));
    for (auto it = featureChecks_.constBegin(); it != featureChecks_.constEnd(); ++it) {
        QSignalBlocker block(it.value());
        it.value()->setChecked(features_.contains(it.key()));
    }
    loadConfig();
    recompute();
    setActiveInstance(path_, instance_);
    noteRemorarcWritten();
    setWindowTitle(QStringLiteral("Remora"));
    syncProfileWidgets();
    statusBar()->showMessage(QStringLiteral("profile '%1' created").arg(clean), 4000);
}


// A non-h264 mirror codec needs an on-device HW encoder, which only the -hwc2 image ships —
// stock/prebuilt images have just OMX.google.h264.encoder, so the mirror server dies with
// "Could not create video encoder" and the demuxers report connection errors. Catch it here
// with a clear choice instead of letting the connect fail cryptically.
bool MainWindow::gateVideoCodec(RemoraConfig &cfg, Backend backend) {
    const ResolvedConfig rc = resolve(cfg, backend);
    if (rc.videoCodec == QLatin1String("h264")
        || (rc.videoCodec == QLatin1String("h265")
            && rc.imageTag.contains(QLatin1String("hwc2"))))
        return true;
    const auto ret = QMessageBox::question(
        this, QStringLiteral("No %1 encoder in this image").arg(rc.videoCodec),
        QStringLiteral("This profile asks the mirror for %1, but image '%2' has no hardware video "
                       "encoder (stock images only ship the software h264 encoder; Remora's "
                       "-hwc2 image adds HEVC). The session would fail with \u201cCould not create "
                       "video encoder\u201d.\n\nConnect with h264 instead?")
            .arg(rc.videoCodec, rc.imageTag),
        QMessageBox::Yes | QMessageBox::Cancel);
    if (ret != QMessageBox::Yes) return false;
    cfg.mirror.videoCodec = QStringLiteral("h264");  // this run only \u2014 profile unchanged
    return true;
}

// One-click device screenshot → Pictures/remora-shot-<profile>-<ts>.png. Deliberately not gated
// on running_: it is a read-only adb capture that can't disturb a deploy, and it works with or
// without the mirror open.
void MainWindow::shotProfile(const QString &profile, const QString &hostOverride) {
    RemoraConfig cfg = loadInstance(path_, profile);
    // the override is authoritative INCLUDING empty: a cleared host cell means local,
    // exactly as its "(local)" placeholder promises
    cfg.backend.sshHost = hostOverride.trimmed().isEmpty()
                              ? std::nullopt
                              : std::optional<QString>(hostOverride.trimmed());
    const Backend backend = inferBackend(cfg);  // AFTER the override: the host decides
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QDir().mkpath(dir);
    const QString path =
        dir + QStringLiteral("/remora-shot-%1-%2.png")
                  .arg(appMenuSlug(profile), QDateTime::currentDateTime().toString(
                                                 QStringLiteral("yyyyMMdd-HHmmss")));
    statusBar()->showMessage(QStringLiteral("capturing screenshot…"));
    auto *w = new ShotWorker(backend, cfg, path, this);
    connect(w, &ShotWorker::sigDone, this, [this](bool ok, QString detail) {
        statusBar()->showMessage(ok ? QStringLiteral("screenshot saved: %1").arg(detail)
                                    : QStringLiteral("screenshot failed: %1").arg(detail),
                                 10000);
    });
    connect(w, &ShotWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// Record toggle, per deployment: ON relaunches that deployment's mirror with --record —
// the recorded session REPLACES the running mirror (a second client shares the encoder and
// costs it bandwidth). OFF (or closing the mirror window) relaunches the plain mirror; either
// way the mirror finalizes the mp4 on exit. Every exit path resyncs the row buttons with
// rebuildRunTable(), which re-derives each checked state from recordingDep_ — so a refused or
// failed toggle snaps its button back instead of lying.
void MainWindow::recordProfile(const QString &depName, const QString &profile,
                               const QString &hostOverride, const QString &imageOverride,
                               bool on) {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"), 5000);
        rebuildRunTable();
        return;
    }
    RemoraConfig cfg = loadInstance(path_, profile);
    // the override is authoritative INCLUDING empty: a cleared host cell means local,
    // exactly as its "(local)" placeholder promises
    cfg.backend.sshHost = hostOverride.trimmed().isEmpty()
                              ? std::nullopt
                              : std::optional<QString>(hostOverride.trimmed());
    const Backend backend = inferBackend(cfg);  // AFTER the override: the host decides
    if (!imageOverride.isEmpty()) cfg.image.imageTag = imageOverride;
    if (on && !gateVideoCodec(cfg, backend)) {
        rebuildRunTable();
        return;
    }
    QString doneMsg;
    if (on) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        QDir().mkpath(dir);
        recordPath_ =
            dir + QStringLiteral("/remora-rec-%1-%2.mp4")
                      .arg(appMenuSlug(profile), QDateTime::currentDateTime().toString(
                                                     QStringLiteral("yyyyMMdd-HHmmss")));
        cfg.mirror.extra << QStringLiteral("--record=%1").arg(recordPath_);
        recordingDep_ = depName;
        doneMsg = QStringLiteral("recording to %1 — toggle Record off (or close the mirror) to "
                                 "finish").arg(recordPath_);
    } else {
        recordingDep_.clear();
        doneMsg = QStringLiteral("recording saved: %1").arg(recordPath_);
    }
    checklist_->clear();
    setRunning(true);
    statusBar()->showMessage(on ? QStringLiteral("starting the recorded mirror session…")
                                : QStringLiteral("finishing the recording…"));
    auto *w = new EngineWorker(backend, cfg, profile, EngineWorker::Reconnect, this);
    connect(w, &EngineWorker::sigState, this, &MainWindow::onState);
    connect(w, &EngineWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &EngineWorker::sigDone, this, [this, doneMsg, on](bool ok) {
        setRunning(false);
        if (!ok && on) recordingDep_.clear();  // never started — don't pretend it's rolling
        statusBar()->showMessage(
            ok ? doneMsg
               : QStringLiteral("mirror relaunch failed — is the instance running? (see the log)"),
            12000);
        rebuildRunTable();
    });
    connect(w, &EngineWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// A flat list of the ACTIVE profile's app entries (Runners-table style) — other profiles'
// folders are managed by switching profiles, not by scrolling past them here. States render
// as icon toggles, not checkboxes: mode (desktop window ⟷ freeform popup) and, for freeform
// entries, unstretch (letterbox instead of stretch-to-fill).
void MainWindow::rebuildIntegrationsList() {
    if (!integrationsTree_) return;
    integrationsTree_->clear();
    // Per-entry window mode from the profile's app_modes (the list writes to disk
    // immediately, so disk is the authority — see applyToConfig()).
    RemoraConfig pcfg = loadInstance(path_, instance_);
    denylistPkgs_ = pcfg.advanced.denylistPackages;
    const bool rootHidingOn =
        pcfg.advanced.rootHiding.value_or(QStringLiteral("off")) != QLatin1String("off");
    bool defaulted = false;
    for (const AppMenuEntry &e : appMenuEntries(appMenuSlug(instance_))) {
        // arm64-injected apps read differently: they run under the native bridge, and their
        // UPDATE OWNER decides what Play can do to them (bd remora-4ei.91). Shell-claimed is
        // PROTECTED — Play cannot silently push the broken x86_64 split, updates come only from
        // Update ARM apps. Play-owned or unclaimed is only REPAIRED AFTER THE FACT — Play may
        // overwrite it at any update, and the guard puts the arm64 build back at the next
        // Connect. Say which, because one is preventive and the other leaves a broken window.
        // Without an owner answer yet (probe pending or stale), fall back to the pin alone.
        const bool arm = armApps_.contains(e.pkg);
        const bool pinned = isArmPinned(pcfg, e.pkg);
        const bool ownerKnown = armOwners_.contains(e.pkg);
        const QString owner = armOwners_.value(e.pkg);
        QString tag, tip;
        if (ownerKnown && armUpdateOwnerProtects(owner)) {
            tag = QStringLiteral("   · arm64 (protected)");
            tip = QStringLiteral(
                "This app has no working x86_64 build, so Remora keeps it on arm64 (it runs "
                "under the native bridge).\n\nPROTECTED: Remora holds its update-ownership "
                "claim, so Play cannot silently push the broken x86_64 split over it. New "
                "versions come only from Update ARM apps, which keeps it arm64.");
        } else if (ownerKnown) {
            const bool play = owner == QLatin1String("com.android.vending");
            tag = play ? QStringLiteral("   · arm64 (Play-owned)")
                       : QStringLiteral("   · arm64 (unclaimed)");
            tip = (play ? QStringLiteral(
                       "This app runs as arm64 (native bridge), but PLAY still owns its "
                       "updates — it can overwrite it with the broken x86_64 build at any "
                       "update. ")
                        : QStringLiteral(
                       "This app runs as arm64 (native bridge), but no update-ownership claim "
                       "is recorded — Play may silently auto-update it onto the broken x86_64 "
                       "build. "))
                  + QStringLiteral(
                      "The arm guard puts the arm64 build back at the next Connect; the app "
                      "may be broken in between.\n\nTo claim it for Remora now, reinstall it "
                      "via Import / export… → Install ARM app from Google Play — app data is "
                      "kept, and the fetch reports the claim as it finishes. A guard repair "
                      "after Play's first strike claims it the same way.");
        } else if (pinned) {
            tag = QStringLiteral("   · arm64 (auto-repair)");
            tip = QStringLiteral(
                "This app has no working x86_64 build, so Remora keeps it on arm64 (it runs "
                "under the native bridge).\n\nPlay re-delivers the broken x86_64 split at every "
                "update; the arm guard checks after each Connect and silently re-fetches the "
                "arm64 build when that happens, so the app just works.\n\nTurn the guard off on "
                "the Apps page to stop the repairs.");
        } else if (arm) {
            tag = QStringLiteral("   · arm64");
            tip = QStringLiteral(
                "Installed as arm64 (runs under the native bridge). Remora owns its updates — "
                "Play cannot silently auto-update it; use Update ARM apps.");
        }
        auto *item = new QTreeWidgetItem(QStringList{e.name, e.pkg + tag});
        if (!tip.isEmpty()) item->setToolTip(1, tip);
        item->setData(0, Qt::UserRole, e.pkg);
        QString mode = appModeFor(pcfg, e.pkg);
        // A newly added app has no recorded mode — default it to a freeform popup, PERSISTED,
        // so the list and the launcher agree (cmdApp applies the same default).
        if (mode.isEmpty()) {
            mode = QStringLiteral("freeform");
            setAppMode(pcfg, e.pkg, mode);
            defaulted = true;
        }
        integrationsTree_->addTopLevelItem(item);
        const bool desktop = mode == QLatin1String("desktop");
        const QString pkg = e.pkg;
        // mode toggle: one desktop icon, lit when the entry opens as a desktop-resolution
        // window; unlit = the freeform-popup default. Click to flip.
        auto *modeBtn = new QToolButton();
        modeBtn->setAutoRaise(true);
        // the icon IS the state: a monitor for a desktop-resolution window, a phone for the
        // freeform popup — click to flip. One explicit icon size for both states: a mode
        // toggle must never change the ROW height (the 22px ghost pixmap used to).
        modeBtn->setIconSize(QSize(16, 16));
        modeBtn->setIcon(desktop ? themeIcon({"video-display", "computer"})
                                 : themeIcon({"smartphone", "phone", "window"}));
        modeBtn->setToolTip(desktop
                                ? QStringLiteral("Opens as a desktop-resolution window — click "
                                                 "for a freeform popup")
                                : QStringLiteral("Opens as a freeform popup (phone-sized) — "
                                                 "click for a desktop-resolution window"));
        connect(modeBtn, &QToolButton::clicked, this, [this, pkg, desktop] {
            setEntryMode(pkg, desktop ? QStringLiteral("freeform") : QStringLiteral("desktop"));
            // rebuild swaps the row widgets — queued, so the clicked button isn't deleted
            // out from under its own signal
            QMetaObject::invokeMethod(this, &MainWindow::rebuildIntegrationsList,
                                      Qt::QueuedConnection);
        });
        integrationsTree_->setItemWidget(item, 2, modeBtn);
        // Root-hiding denylist, per app. It was a comma-separated line edit in its own card: you
        // typed package names at a text box while the list of package names sat right above it.
        // Same icon-is-the-state idiom as the mode toggle — lit means this app is on the denylist.
        // Greyed out (not hidden) when the mode is off, because the entry still PERSISTS then and
        // a row that vanished would read as "not denylisted".
        const bool denied = denylistPkgs_.contains(e.pkg);
        auto *denyBtn = new QToolButton();
        denyBtn->setAutoRaise(true);
        denyBtn->setIconSize(QSize(16, 16));
        denyBtn->setIcon(denied ? themeIcon({"view-hidden", "hint", "security-high"})
                                : themeIcon({"view-visible", "visibility", "security-low"}));
        denyBtn->setEnabled(rootHidingOn);
        denyBtn->setToolTip(
            !rootHidingOn
                ? QStringLiteral("Root hiding is off for this profile — set a mode in the bar "
                                 "below and this becomes live. The list is kept either way.")
            : denied ? QStringLiteral("On the root-hiding denylist — Magisk's changes are hidden "
                                      "from this app. Click to remove it.")
                     : QStringLiteral("Not on the denylist — this app can see root. Click to hide "
                                      "root from it."));
        connect(denyBtn, &QToolButton::clicked, this, [this, pkg, denied] {
            setEntryDenylisted(pkg, !denied);
            QMetaObject::invokeMethod(this, &MainWindow::rebuildIntegrationsList,
                                      Qt::QueuedConnection);
        });
        integrationsTree_->setItemWidget(item, 4, denyBtn);
        // The Reddit app keeps login state the app's own login flow can't establish in the
        // container; offer the embedded web-login-and-inject flow right on its entry. Only the
        // Reddit entry gets the button — it is app-specific (bd remora-6zj).
        if (e.pkg == QLatin1String("com.reddit.frontpage")) {
            auto *loginBtn = new QToolButton();
            loginBtn->setAutoRaise(true);
            loginBtn->setIcon(themeIcon({"im-user", "user-identity", "internet-web-browser"}));
            loginBtn->setToolTip(QStringLiteral(
                "Log into Reddit — sign into reddit.com in a window (your password stays with "
                "reddit); Remora reads the session and logs the container app in — no rooted "
                "device."));
            const QString profile = instance_;
            connect(loginBtn, &QToolButton::clicked, this,
                    [this, profile] { openRedditLogin(profile); });
            integrationsTree_->setItemWidget(item, 3, loginBtn);
        }
    }
    // the icon columns hold item widgets, which ResizeToContents cannot measure
    integrationsTree_->setColumnWidth(2, 70);
    integrationsTree_->setColumnWidth(3, 70);
    integrationsTree_->setColumnWidth(4, 80);
    // Denylisted packages with no launcher entry — a system package like com.google.android.gms
    // has none, and it is the placeholder the old text box shipped with. The column cannot show
    // them, so name them rather than let the fold silently swallow half the list.
    if (denylistOrphanLabel_) {
        QStringList orphans;
        for (const QString &d : denylistPkgs_) {
            bool listed = false;
            for (int i = 0; i < integrationsTree_->topLevelItemCount() && !listed; ++i)
                listed = integrationsTree_->topLevelItem(i)->data(0, Qt::UserRole).toString() == d;
            if (!listed) orphans << d;
        }
        denylistOrphanLabel_->setVisible(!orphans.isEmpty());
        denylistOrphanLabel_->setText(
            QStringLiteral("also denylisted, with no app entry to show it against: %1")
                .arg(orphans.join(QStringLiteral(", "))));
    }
    if (defaulted) {
        saveInstance(path_, instance_, pcfg);
        noteRemorarcWritten();
        cfg_.integration.appModes = pcfg.integration.appModes;
    }
}

// Which of the active profile's apps run as arm64 — asked of the live device, quietly, so the
// list can badge them. A stopped device just keeps the last known set (usually empty at start).
void MainWindow::probeArmApps() {
    if (armProbeInFlight_) return;
    armProbeInFlight_ = true;
    RemoraConfig cfg = loadInstance(path_, instance_);
    const Backend b = inferBackend(cfg);
    const ResolvedConfig rc = resolve(cfg, b);
    const QString host = b == Backend::Bare ? QString() : rc.sshHost.value_or(QString());
    const QString container = rc.containerName;
    // Whose device this probe is asking about. Carried through because the answer now WRITES
    // (the arm pins below): a profile switch while the probe is in flight would otherwise record
    // one device's arm64 apps against another profile's config.
    const QString probed = instance_;
    auto *thread = QThread::create([this, host, container, probed] {
        // One dumpsys per package answers BOTH questions: does it run as arm64, and who may push
        // its next update (updateOwnerPackageName). The owner separates an app Play cannot touch
        // from one it can silently overwrite with the broken x86_64 split (bd remora-4ei.91), so
        // the probe carries it out alongside the name, "pkg=owner" per line. The trailing
        // `exit 0` stays load-bearing: the loop exits with the status of its LAST command, and
        // runShell discards the output of anything non-zero — that once cost every arm app but a
        // PINNED one its badge (bd remora-h0o).
        const ShellResult r = runShellRc(
            host,
            QStringLiteral("docker exec %1 sh -c 'for p in $(pm list packages -3 | sed "
                           "s/^package://); do d=$(dumpsys package $p 2>/dev/null); "
                           "case \"$d\" in *primaryCpuAbi=arm64-v8a*) o=; "
                           "case \"$d\" in *updateOwnerPackageName=*) "
                           "o=${d#*updateOwnerPackageName=}; o=${o%%[!A-Za-z0-9_.]*};; esac; "
                           "echo \"$p=$o\";; esac; done; exit 0' 2>/dev/null")
                .arg(container),
            30000);
        // runShellRc merges stderr, so the parser keeps only lines shaped like an answer.
        const QHash<QString, QString> owners = parseArmAppOwners(r.out);
        const QSet<QString> pkgs(owners.keyBegin(), owners.keyEnd());
        const bool reached = r.rc == 0;
        QMetaObject::invokeMethod(
            this,
            [this, pkgs, owners, reached, probed] {
                armProbeInFlight_ = false;
                // Answer for a profile that is no longer shown: drop it rather than badge or pin
                // another device's apps. The next visit re-probes.
                if (probed != instance_) return;
                // The exit code is the reachability signal, not emptiness: a running device with
                // nothing on arm must be able to CLEAR the badges, while an unreachable one keeps
                // the last known set rather than blanking the list. Owners compare separately:
                // a repair flips vending → shell without changing the SET, and the badge must
                // follow it.
                if (!reached || (pkgs == armApps_ && owners == armOwners_)) return;
                armApps_ = pkgs;
                armOwners_ = owners;
                // AN APP RUNNING AS ARM64 *IS* THE EVIDENCE THAT IT SHOULD BE, so record it —
                // the pin is what the guard reads, and it is needed precisely when the live
                // probe can no longer help: once Play overwrites the app with its x86_64 split
                // the device stops reporting arm64, and without a pin nothing remembers it ever
                // was. Two of this machine's three arm64 apps sat unpinned AND Play-owned
                // (updateOwnerPackageName=com.android.vending), i.e. Play was free to break them
                // with nothing arranged to repair it. Same auto-pin the successful-fetch path
                // already does, applied to the apps that got there some other way.
                RemoraConfig pcfg = loadInstance(path_, probed);
                QStringList newlyPinned;
                for (const QString &pkg : pkgs)
                    if (!isArmPinned(pcfg, pkg)) {
                        setArmPinned(pcfg, pkg, true);
                        newlyPinned << pkg;
                    }
                if (!newlyPinned.isEmpty()) {
                    saveInstance(path_, probed, pcfg);
                    noteRemorarcWritten();
                    cfg_.integration.armApps = pcfg.integration.armApps;
                    appendLog(QStringLiteral("arm guard: now watching %1 — running as arm64, so "
                                             "Remora will put the arm64 build back if Play "
                                             "replaces it with x86_64")
                                  .arg(newlyPinned.join(QStringLiteral(", "))));
                }
                rebuildIntegrationsList();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// Persist one entry's window mode into the active profile (menu launches read it from disk).
// Add/remove ONE package on the root-hiding denylist. Surgical on purpose: rebuilding the list
// from the tree's ticked rows would drop every denylisted package that has no launcher entry, and
// system packages — the exact ones worth hiding root from — never have one.
void MainWindow::setEntryDenylisted(const QString &pkg, bool on) {
    RemoraConfig pcfg = loadInstance(path_, instance_);
    QStringList d = pcfg.advanced.denylistPackages;
    d.removeAll(pkg);
    if (on) d << pkg;
    d.sort();
    pcfg.advanced.denylistPackages = d;
    saveInstance(path_, instance_, pcfg);
    noteRemorarcWritten();
    cfg_.advanced.denylistPackages = d;
}

void MainWindow::setEntryMode(const QString &pkg, const QString &mode) {
    RemoraConfig pcfg = loadInstance(path_, instance_);
    setAppMode(pcfg, pkg, mode);
    saveInstance(path_, instance_, pcfg);
    noteRemorarcWritten();
    cfg_.integration.appModes = pcfg.integration.appModes;
}

// Every visit to the Apps page: show the on-disk truth immediately, then quietly re-generate
// the menu folder from the device (this replaces the old Refresh-now button). Failure is
// deliberately quiet — with the instance stopped, the local list is the best available truth.
void MainWindow::refreshAppsPage() {
    rebuildIntegrationsList();
    if (running_ || appsRefreshInFlight_) return;
    // menu disabled (the Profiles table's desktop-menu column, Settings page)
    if (!cfg_.integration.desktopMenu.value_or(true)) return;
    // REMORA_SHOT renders quit ~500ms in — a device probe mid-flight would be destroyed while
    // running (QThread abort), and a deterministic screenshot must not touch the device anyway
    if (qEnvironmentVariableIsSet("REMORA_SHOT")) return;
    probeArmApps();
    appsRefreshInFlight_ = true;
    // Unparented on purpose: fire-and-forget. A parented worker aborts the whole process if the
    // window is closed while the refresh is mid-adb; unparented it leaks only on that exit path.
    auto *w = new AppMenuWorker(AppMenuWorker::Refresh, currentBackend(), cfg_, instance_,
                                nullptr);
    connect(w, &AppMenuWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &AppMenuWorker::sigDone, this, [this](bool ok, QString) {
        appsRefreshInFlight_ = false;
        if (ok) rebuildIntegrationsList();
    });
    connect(w, &AppMenuWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// Open the embedded reddit.com login window for a profile. Runs inside the workspace's own
// QApplication (no separate process); the window owns the transplant worker and self-deletes.
void MainWindow::openRedditLogin(const QString &profileName) {
    RemoraConfig cfg = loadInstance(path_, profileName);
    const Backend backend = inferBackend(cfg);
    auto *w = new RedditLoginWindow(cfg, backend);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setWindowTitle(QStringLiteral("Remora — Reddit login (%1)").arg(profileName));
    w->resize(920, 840);
    w->show();
}

// All desktop → every entry a desktop window; all freeform → every entry a popup.
// Active profile only; the rebuild redraws the icons.
void MainWindow::setAllAppModes(bool desktop) {
    if (!integrationsTree_) return;
    RemoraConfig pcfg = loadInstance(path_, instance_);
    for (int i = 0; i < integrationsTree_->topLevelItemCount(); ++i) {
        const QString pkg = integrationsTree_->topLevelItem(i)->data(0, Qt::UserRole).toString();
        setAppMode(pcfg, pkg,
                   desktop ? QStringLiteral("desktop") : QStringLiteral("freeform"));
    }
    saveInstance(path_, instance_, pcfg);
    noteRemorarcWritten();
    cfg_.integration.appModes = pcfg.integration.appModes;
    rebuildIntegrationsList();
    statusBar()->showMessage(desktop ? QStringLiteral("all entries set to desktop mode.")
                                     : QStringLiteral("all entries set to freeform mode."),
                             4000);
}

void MainWindow::onRemoveAppMenu() {
    // the list shows only the active profile — Remove always targets its folder
    auto *w = new AppMenuWorker(AppMenuWorker::Remove, currentBackend(), cfg_, instance_, this);
    connect(w, &AppMenuWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &AppMenuWorker::sigDone, this, [this](bool ok, QString detail) {
        rebuildIntegrationsList();
        statusBar()->showMessage(ok ? detail : QStringLiteral("app-menu removal failed"),
                                 ok ? 6000 : 0);
    });
    connect(w, &AppMenuWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// The data dir is the path the running container is bound to. Repointing it under a live device
// changes nothing about that device — it only makes the form describe something untrue, and the
// next Connect then silently moves the instance. So while the profile is up the field is
// read-only, and the row's one button becomes the action that IS available: open the storage.
void MainWindow::setDataDirLocked(bool locked) {
    if (!dataDirEdit_) return;
    dataDirEdit_->setReadOnly(locked);
    // Say it visually too. A read-only QLineEdit is pixel-identical to an editable one in most
    // styles, so the field looked typeable and simply ignored the keystrokes — worse than being
    // told no. Muted, not disabled: the path still has to be readable, it just stops inviting a
    // cursor. Theme-aware for the same reason badgeStyle is — one fixed grey vanishes on one of
    // the two backgrounds.
    const bool dark = qApp->palette().color(QPalette::Window).lightness() < 128;
    dataDirEdit_->setStyleSheet(
        locked ? QStringLiteral("color:%1;")
                     .arg(dark ? QStringLiteral("#bdbdbd") : QStringLiteral("#757575"))
               : QString());
    if (!dataDirPickAct_) return;
    // Greyed rather than removed: a menu whose entries come and go teaches you nothing, while a
    // disabled entry that says why is the answer to "how do I move this?" — stop the device.
    dataDirPickAct_->setEnabled(!locked);
    dataDirPickAct_->setToolTip(
        locked ? QStringLiteral("The device is running: it is bound to this path, so it can only "
                                "be repointed while stopped.")
               : QStringLiteral("Move this profile's /data elsewhere. The next Start uses "
                                "whatever path is here — the existing folder is not moved for "
                                "you."));
}

// Ask docker whether THIS profile's container is up. Off-thread: the Device page must not stall
// on a docker call, and on a remote backend this goes over ssh.
void MainWindow::refreshDataDirLock() {
    if (!dataDirEdit_) return;
    // REMORA_SHOT renders must not depend on docker state. Show the unlocked row: it is the one
    // that has every control on it, so a screenshot documents the feature rather than a snapshot
    // of whether something happened to be running.
    if (qEnvironmentVariableIsSet("REMORA_SHOT")) {
        setDataDirLocked(false);
        return;
    }
    const RemoraConfig cfg = cfg_;
    const Backend backend = currentBackend();
    const ResolvedConfig rc = resolve(cfg, backend);
    const QString container = rc.containerName;
    const QString host =
        backend == Backend::Bare ? QString() : cfg.backend.sshHost.value_or(QString());
    const QString profile = instance_;
    QPointer<MainWindow> self(this);
    auto *thread = QThread::create([self, container, host, profile] {
        const QString out = runShell(
            host,
            QStringLiteral("docker ps --filter name=^%1$ --format '{{.Names}}' 2>/dev/null")
                .arg(container),
            12000);
        const bool up = out.trimmed() == container;
        QMetaObject::invokeMethod(
            qApp,
            [self, up, profile] {
                // The profile may have been switched while the probe was in flight — an answer
                // about the old one must not lock the new one's field.
                if (!self || self->instance_ != profile) return;
                self->setDataDirLocked(up);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// Show a FOLDER in the user's file manager.
//
// Deliberately not QDesktopServices/xdg-open, which resolve a directory through the mime database
// as inode/directory — and plenty of applications claim that association without being anything
// like a file manager. VLC ships inode/directory in its MimeType list, so on a desktop where it
// won the association, "open device storage" opened a media player on 5 GB of Android /data
// (reported against this very button). Nothing was broken: xdg-open did exactly what the system
// told it to, which is why asking a different question is the fix.
//
// org.freedesktop.FileManager1 IS that different question — "show me this folder", not "open
// this mime type" — and Dolphin, Nautilus, Thunar and Nemo all implement it. It also takes URIs,
// so the remote backend's sftp:// path goes through the same call. Fall back to the mime route
// only when nobody implements the interface, where a wrong-app open still beats no open at all.
static void showFolder(const QUrl &url) {
    QDBusInterface fm(QStringLiteral("org.freedesktop.FileManager1"),
                      QStringLiteral("/org/freedesktop/FileManager1"),
                      QStringLiteral("org.freedesktop.FileManager1"),
                      QDBusConnection::sessionBus());
    if (fm.isValid()) {
        const QDBusMessage r = fm.call(QStringLiteral("ShowFolders"),
                                       QStringList{url.toString(QUrl::FullyEncoded)}, QString());
        if (r.type() != QDBusMessage::ErrorMessage) return;
    }
    QDesktopServices::openUrl(url);
}

// Open <dataDir>/media/0 — the backing store of the device's /sdcard (the /data bind makes them
// the same files). Local path on bare; sftp:// for remote, where the data dir lives on the
// docker host (KIO/gvfs file managers browse sftp natively). Under overlayfs the writable copy
// lives in the diff's upper layer instead (dataDir is the /data-diff mount).
void MainWindow::onOpenDeviceStorage() {
    applyToConfig();
    const ResolvedConfig rc = resolve(cfg_, currentBackend());
    const QString media = rc.dataDir
                          + (rc.useOverlayfs ? QStringLiteral("/upper/media/0")
                                             : QStringLiteral("/media/0"));
    QUrl url;
    if (currentBackend() == Backend::Bare) {
        QDir().mkpath(media);  // browsable even before the first deploy
        // Once the device has booted once, init hands /data/media to Android's media_rw (gid
        // 1023) at mode 0550 and this desktop session is not in that group — so the folder is
        // there and simply not ours to open, which every file manager reports as "does not
        // exist". Offer the one-time grant rather than the path, an excuse, or a workaround.
        if (!QDir(media).isReadable()) {
            offerDeviceStorageAccess(media, QUrl::fromLocalFile(media));
            return;
        }
        url = QUrl::fromLocalFile(media);
    } else {
        url = QUrl(QStringLiteral("sftp://%1%2").arg(rc.sshHost.value_or(QString()), media));
    }
    statusBar()->showMessage(QStringLiteral("opening %1").arg(url.toString()), 5000);
    showFolder(url);
}

// The desktop half of "Open device storage": grant this uid a POSIX ACL over the profile's
// /sdcard tree, including a default ACL so the device's own future files inherit it. Runs behind
// pkexec, so the authorisation happens in polkit's own prompt and no password ever reaches
// Remora. Nothing about the user's ACCOUNT changes — the grant lives on this data dir — and it
// takes effect at once, so the open it was blocking can just proceed.
void MainWindow::offerDeviceStorageAccess(const QString &media, const QUrl &pending) {
    // No confirmation step: the user pressed a button that says "open device storage", and the
    // only thing between them and it is an authorisation the system is about to ask for in its
    // own dialog. A Remora dialog explaining that a dialog is coming is one click of nothing.
    // <dataDir>[/upper]/media — the parent, so one recursive pass covers it and the /0 below it.
    const QString mediaRoot = QFileInfo(media).absolutePath();
    const QStringList argv = buildStorageAclArgv(mediaRoot, getuid());
    auto *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    connect(p, &QProcess::finished, this, [this, p, media, pending](int rc, QProcess::ExitStatus) {
        const QString err = QString::fromLocal8Bit(p->readAll()).trimmed();
        p->deleteLater();
        if (rc != 0) {
            // 126 is pkexec's "dismissed / not authorised" — a decision, not a fault.
            statusBar()->showMessage(QStringLiteral("storage access not granted"), 6000);
            if (rc != 126)
                QMessageBox::warning(
                    this, QStringLiteral("Could not grant storage access"),
                    QStringLiteral("Setting the ACL failed (exit %1).%2")
                        .arg(rc)
                        .arg(err.isEmpty() ? QString() : QStringLiteral("\n\n") + err));
            return;
        }
        // The grant is live in this process the moment setfacl returns, so finish the job the
        // user actually asked for rather than making them press the button a second time.
        if (!QDir(media).isReadable()) {
            QMessageBox::warning(this, QStringLiteral("Storage is still not readable"),
                                 QStringLiteral("The ACL was applied but %1 is still closed to "
                                                "this user.%2")
                                     .arg(media)
                                     .arg(err.isEmpty() ? QString()
                                                        : QStringLiteral("\n\n") + err));
            return;
        }
        statusBar()->showMessage(QStringLiteral("access granted — opening %1").arg(media), 5000);
        showFolder(pending);
    });
    statusBar()->showMessage(QStringLiteral("waiting for authorisation…"));
    p->start(argv.first(), argv.mid(1));
}

// Fetch an app's ARM64 build from Play — reusing the device's own Google login — and sideload
// it into the profile's container (remora-arm-fetch.sh). The fix for apps whose x86_64 build
// is broken. Reached from the Install/transfer/back-up dialog.
void MainWindow::installArmApp(const QString &profile, const QString &pkg) {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"),
                                 5000);
        return;
    }
    const RemoraConfig cfg = loadInstance(path_, profile);
    const Backend b = inferBackend(cfg);
    setRunning(true);
    if (logAction_) logAction_->setChecked(true);  // reveal the log pane for the streamed output
    statusBar()->showMessage(QStringLiteral("fetching the ARM build of %1…").arg(pkg));
    appendLog(QStringLiteral("=== fetching ARM build of %1 into '%2' ===").arg(pkg, profile));
    auto *w = new ArmAppInstallWorker(b, cfg, pkg, this);
    connect(w, &ArmAppInstallWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &ArmAppInstallWorker::sigDone, this, [this, profile, pkg](bool ok, QString detail) {
        setRunning(false);
        statusBar()->showMessage(detail, ok ? 8000 : 0);
        if (!ok) {
            // Say it in the log too, not just a status bar message that scrolls away — a failed
            // repair is exactly the thing a user needs to be able to read back later.
            appendLog(QStringLiteral("=== ARM install FAILED for %1: %2 ===").arg(pkg, detail));
            return;
        }
        // Trust the DEVICE, not the exit code. The fetch script can return 0 having installed
        // nothing that took effect, and a pin recorded against an app that is still x86_64 makes
        // the guard re-fetch it forever while the app stays broken. Ask what the ABI actually is
        // before recording anything (bd remora-e5x.31). Off the UI thread — this talks to the
        // device, the same reason probeArmApps() does.
        RemoraConfig probe = loadInstance(path_, profile);
        probe.integration.armApps = QStringList{pkg};
        const Backend pb = inferBackend(probe);
        auto *verify = QThread::create([this, profile, pkg, probe, pb] {
            RealSpawner sp;
            const bool stillWrong = !armPinsNeedingRepair(sp, probe, pb).isEmpty();
            QMetaObject::invokeMethod(
                this,
                [this, profile, pkg, stillWrong] {
                    if (stillWrong) {
                        appendLog(QStringLiteral(
                                      "=== %1: the fetch reported success but the app is STILL "
                                      "not arm64 — not pinning it, because a pin here would make "
                                      "the guard retry forever against an app that never "
                                      "changed ===")
                                      .arg(pkg));
                        statusBar()->showMessage(
                            QStringLiteral("%1 is still not arm64 — see the Log pane").arg(pkg), 0);
                        return;
                    }
                    // A successful ARM fetch IS the statement that this app has no working x86_64
                    // build — record it so the guard puts it back when Play overwrites it, instead
                    // of the user rediscovering the same crash weeks later.
                    RemoraConfig pcfg = loadInstance(path_, profile);
                    if (!isArmPinned(pcfg, pkg)) {
                        setArmPinned(pcfg, pkg, true);
                        saveInstance(path_, profile, pcfg);
                        noteRemorarcWritten();
                        if (profile == instance_)
                            cfg_.integration.armApps = pcfg.integration.armApps;
                        appendLog(QStringLiteral(
                                      "pinned %1 to arm64 — the arm guard will re-fetch it if "
                                      "Play replaces it with an x86_64 build")
                                      .arg(pkg));
                    }
                    if (profile == instance_) refreshAppsPage();  // new entry + arm64 badge
                },
                Qt::QueuedConnection);
        });
        connect(verify, &QThread::finished, verify, &QObject::deleteLater);
        verify->start();
    });
    connect(w, &ArmAppInstallWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// Re-fetch the INJECTED ARM64 apps from Play (remora-arm-fetch.sh --update-all), keeping their
// data and permissions. Only those: their ownership claim blocks Play's silent auto-update, so
// this IS their update path, while every other app is a Play-owned x86_64 install that Play
// updates on the device by itself. The sweep used to cover those too and could not — the only
// x86_64 device profile apkeep offers returns an empty delivery (bd remora-rvf).
// manual=false is the quiet after-Connect auto-update (Settings page).
void MainWindow::runAppUpdate(const QString &profile, bool manual) {
    if (running_) {
        if (manual)
            statusBar()->showMessage(QStringLiteral("an operation is already running — wait for "
                                                    "it to finish before starting another"),
                                     5000);
        return;
    }
    if (manual && profile == instance_) {
        applyToConfig();  // pending edits travel with the sweep
        saveInstance(path_, instance_, cfg_);
        noteRemorarcWritten();
    }
    const RemoraConfig cfg = loadInstance(path_, profile);
    const Backend b = inferBackend(cfg);
    setRunning(true);
    // the manual button reveals the streamed output; the auto-update stays out of the way
    if (manual && logAction_) logAction_->setChecked(true);
    statusBar()->showMessage(QStringLiteral("checking '%1' for app updates…").arg(profile));
    appendLog(QStringLiteral("=== checking '%1' for app updates ===").arg(profile));
    auto *w = new ArmAppInstallWorker(b, cfg, QStringLiteral("--update-all"), this);
    connect(w, &ArmAppInstallWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &ArmAppInstallWorker::sigDone, this, [this, profile](bool ok, QString detail) {
        setRunning(false);
        statusBar()->showMessage(detail, ok ? 8000 : 0);
        if (ok) {
            QSettings s(path_, QSettings::IniFormat);
            s.setValue(QStringLiteral("Remora/app_update_last_%1").arg(profile),
                       QDateTime::currentSecsSinceEpoch());
            noteRemorarcWritten();  // our own write — don't let the watcher reload over edits
        }
    });
    connect(w, &ArmAppInstallWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// After a successful Connect: run the update sweep when the profile opts in (Settings) and the
// configured interval has passed. The last-run stamp lives in the shared Remora section, not
// the instance section, so profile saves never carry it.
void MainWindow::maybeAutoUpdateApps(const QString &profile) {
    const RemoraConfig cfg = loadInstance(path_, profile);
    if (!cfg.integration.autoUpdateApps.value_or(false)) return;
    const int hours = cfg.integration.autoUpdateHours.value_or(24);
    const qint64 last = QSettings(path_, QSettings::IniFormat)
                            .value(QStringLiteral("Remora/app_update_last_%1").arg(profile))
                            .toLongLong();
    if (hours > 0 && last > 0
        && QDateTime::currentSecsSinceEpoch() - last < qint64(hours) * 3600)
        return;
    runAppUpdate(profile, /*manual=*/false);
}

// The arm guard (bd remora-e5x.23): apps with no working x86_64 build are pinned to arm64 when
// they are installed that way, and Play re-delivers the broken x86_64 split at its next update.
// After every Connect, ask the device which pins have lost their arm64 ABI and re-fetch those —
// so an ARM-only app just keeps working instead of dying at spawn until someone notices.
//
// Deliberately quiet: the probe is one docker exec and normally finds nothing, so a healthy
// device pays a few milliseconds and prints nothing. Repairs DO announce themselves in the log
// and the status bar, because a silent reinstall would be worse than a noisy one.
void MainWindow::runArmGuard(const QString &profile) {
    const RemoraConfig cfg = loadInstance(path_, profile);
    if (cfg.integration.armApps.isEmpty()) return;
    if (!cfg.integration.armGuard.value_or(true)) return;  // pins kept, repairs off
    const Backend b = inferBackend(cfg);
    // The probe talks to the device, so it cannot run on the UI thread.
    auto *thread = QThread::create([this, profile, cfg, b] {
        RealSpawner sp;
        const QStringList broken = armPinsNeedingRepair(sp, cfg, b);
        if (broken.isEmpty()) return;
        QMetaObject::invokeMethod(
            this,
            [this, profile, broken] {
                appendLog(QStringLiteral("=== arm guard: %1 lost its arm64 build (Play pushed the "
                                         "x86_64 split) — re-fetching ===")
                              .arg(broken.join(QStringLiteral(", "))));
                // One at a time: installArmApp refuses while another operation runs, and each
                // fetch is a full Play download. The first repair is the one the user is most
                // likely waiting on; the rest ride the next Connect.
                installArmApp(profile, broken.first());
                if (broken.size() > 1)
                    appendLog(QStringLiteral("arm guard: %1 also need repair — they follow on the "
                                             "next Connect, or use Install ARM build now")
                                  .arg(broken.mid(1).join(QStringLiteral(", "))));
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// The transfer/export/import dialog (bd remora-4ei.75). The heavy lifting is
// engine/AppTransfer.cpp via TransferAppsWorker — this only collects WHAT to move, from WHERE to
// WHERE, and streams the per-app lines into the log pane.
void MainWindow::onTransferApps() {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"),
                                 5000);
        return;
    }
    const QStringList profiles = listInstances(path_);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Install, move & back up apps"));
    auto *form = new QFormLayout(&dlg);

    auto *opCombo = new QComboBox();
    opCombo->addItem(QStringLiteral("Transfer to another profile"));
    opCombo->addItem(QStringLiteral("Export APKs to a folder"));
    opCombo->addItem(QStringLiteral("Import APKs from a folder"));
    opCombo->addItem(QStringLiteral("Install ARM app from Google Play"));
    opCombo->setItemData(
        3,
        QStringLiteral("Fetch an app's ARM64 build from Play — reusing the device's own Google "
                       "login — and sideload it as arm64 (native bridge). The fix for apps whose "
                       "x86_64 build is broken, e.g. Flutter apps that crash on launch. The "
                       "install claims update ownership, so Play cannot silently deliver the "
                       "broken build back — Update ARM apps re-fetches it instead. Needs apkeep on "
                       "the deploy host and a signed-in Google account on the device."),
        Qt::ToolTipRole);
    form->addRow(QStringLiteral("Operation"), opCombo);

    // Default both combos to the ACTIVE profile, not list-index 0 — the deployment dialog learned
    // this the hard way. The target defaults to the first OTHER profile, since transferring a
    // profile onto itself is never what anyone means.
    auto *fromCombo = new QComboBox();
    fromCombo->addItems(profiles);
    fromCombo->setCurrentText(instance_);
    form->addRow(QStringLiteral("From profile"), fromCombo);
    auto *toCombo = new QComboBox();
    toCombo->addItems(profiles);
    for (const QString &p : profiles)
        if (p != fromCombo->currentText()) {
            toCombo->setCurrentText(p);
            break;
        }
    form->addRow(QStringLiteral("To profile"), toCombo);

    auto *dirRow = new QWidget();
    auto *dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    auto *dirEdit = new QLineEdit();
    dirEdit->setPlaceholderText(QStringLiteral("folder of APKs — one subdirectory per app"));
    auto *browseBtn = new QPushButton(QStringLiteral("Browse…"));
    connect(browseBtn, &QPushButton::clicked, &dlg, [&dlg, dirEdit] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, QStringLiteral("APK folder"),
                                                            dirEdit->text());
        if (!d.isEmpty()) dirEdit->setText(d);
    });
    dirLay->addWidget(dirEdit, 1);
    dirLay->addWidget(browseBtn);
    form->addRow(QStringLiteral("Folder"), dirRow);

    // The checklist is OPTIONAL — nothing checked means every third-party app, which is the
    // normal migration. Listing needs the source reachable, so it is a button, not an open stall.
    auto *appList = new QListWidget();
    appList->setMinimumHeight(140);
    auto *listBtn = new QPushButton(QStringLiteral("List apps on the source"));
    form->addRow(QString(), listBtn);
    form->addRow(QStringLiteral("Only these"), appList);
    auto *hint = new QLabel(QStringLiteral("nothing checked = every third-party app"));
    hint->setEnabled(false);
    form->addRow(QString(), hint);
    const QString settings = path_;
    connect(listBtn, &QPushButton::clicked, &dlg, [fromCombo, appList, listBtn, settings] {
        listBtn->setEnabled(false);
        listBtn->setText(QStringLiteral("listing…"));
        const QString name = fromCombo->currentText();
        QPointer<QListWidget> list(appList);
        QPointer<QPushButton> btn(listBtn);
        auto *scan = QThread::create([list, btn, name, settings] {
            RealSpawner sp;
            const RemoraConfig cfg = loadInstance(settings, name);
            const RunContext ctx =
                makeContext(cfg, inferBackend(cfg));
            QString err;
            QString target = resolveAdbTarget(sp, ctx, &err);
            if (!target.isEmpty()) target = assertAdbEndpoint(sp, target);
            const QStringList pkgs =
                target.isEmpty() ? QStringList() : listThirdPartyPackages(sp, target);
            const bool reachable = !target.isEmpty();
            QMetaObject::invokeMethod(qApp, [list, btn, pkgs, reachable, name] {
                if (btn) {
                    btn->setEnabled(true);
                    btn->setText(reachable
                                     ? QStringLiteral("List apps on the source")
                                     : QStringLiteral("'%1' is not reachable — is it running?")
                                           .arg(name));
                }
                if (!list) return;
                list->clear();
                for (const QString &p : pkgs) {
                    auto *item = new QListWidgetItem(p, list);
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(Qt::Unchecked);
                }
            });
        });
        connect(scan, &QThread::finished, scan, &QObject::deleteLater);
        scan->start();
    });

    auto *dryRunCheck = new QCheckBox(QStringLiteral("dry run — only report what would move"));
    form->addRow(QString(), dryRunCheck);

    auto *dataCheck =
        new QCheckBox(QStringLiteral("also carry app data (experimental, needs root)"));
    dataCheck->setToolTip(QStringLiteral(
        "Tar each app's /data/data on the source and restore it after the install on the "
        "target, chowned to the target's uid with the SELinux label recomputed (bd "
        "remora-4ei.73). Needs magisk root on BOTH profiles. A data failure leaves that app "
        "installed but logged out — the same result as an ordinary transfer."));
    form->addRow(QString(), dataCheck);

    // The ARM-install operation's package field (hidden for the move/back-up operations).
    auto *armEdit = new QLineEdit();
    armEdit->setPlaceholderText(QStringLiteral("com.example.app"));
    form->addRow(QStringLiteral("Package"), armEdit);

    // Per-operation relevance: transfer = two profiles; export = source + folder;
    // import = target + folder (the folder decides the app set, so no checklist);
    // ARM install = one profile + a package name.
    const auto refresh = [form, opCombo, fromCombo, toCombo, dirRow, appList, listBtn,
                          dryRunCheck, dataCheck, armEdit] {
        const int op = opCombo->currentIndex();
        fromCombo->setEnabled(op != 2);
        toCombo->setEnabled(op == 0 || op == 2);
        dirRow->setEnabled(op == 1 || op == 2);
        appList->setEnabled(op == 0 || op == 1);
        listBtn->setEnabled(op == 0 || op == 1);
        dryRunCheck->setEnabled(op == 0 || op == 2);  // export/install are non-destructive
        dataCheck->setEnabled(op == 0);  // data can only ride a device-to-device transfer
        armEdit->setEnabled(op == 3);
        // the source combo IS the install target for the ARM operation — say so
        if (auto *lbl = qobject_cast<QLabel *>(form->labelForField(fromCombo)))
            lbl->setText(op == 3 ? QStringLiteral("Into profile")
                                 : QStringLiteral("From profile"));
    };
    connect(opCombo, &QComboBox::currentIndexChanged, &dlg, refresh);
    refresh();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&dlg, opCombo, fromCombo, toCombo,
                                                         dirEdit, armEdit] {
        const int op = opCombo->currentIndex();
        if (op == 0 && fromCombo->currentText() == toCombo->currentText()) {
            QMessageBox::warning(&dlg, QStringLiteral("Transfer apps"),
                                 QStringLiteral("Source and target are the same profile."));
            return;
        }
        if ((op == 1 || op == 2) && dirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("Transfer apps"),
                                 QStringLiteral("Pick the APK folder first."));
            return;
        }
        if (op == 3 && !armEdit->text().trimmed().contains(QLatin1Char('.'))) {
            QMessageBox::warning(&dlg, QStringLiteral("Install ARM app"),
                                 QStringLiteral("Enter an Android package name, e.g. "
                                                "com.example.app."));
            return;
        }
        dlg.accept();
    });
    if (dlg.exec() != QDialog::Accepted) return;

    const int op = opCombo->currentIndex();
    if (op == 3) {
        installArmApp(fromCombo->currentText(), armEdit->text().trimmed());
        return;
    }
    const TransferAppsWorker::Op wop = op == 0   ? TransferAppsWorker::Op::Transfer
                                       : op == 1 ? TransferAppsWorker::Op::Export
                                                 : TransferAppsWorker::Op::Import;
    const QString from = fromCombo->currentText(), to = toCombo->currentText();
    const QString dir = dirEdit->text().trimmed();
    const bool dry = dryRunCheck->isEnabled() && dryRunCheck->isChecked();
    const bool withData = dataCheck->isEnabled() && dataCheck->isChecked();
    QStringList pkgs;
    for (int i = 0; i < appList->count(); ++i)
        if (appList->item(i)->checkState() == Qt::Checked) pkgs << appList->item(i)->text();

    // Fresh configs from disk for BOTH sides — the worker must see the saved profiles, not the
    // active page's unsaved edits applied to somebody else's name.
    const RemoraConfig srcCfg = loadInstance(path_, from);
    const RemoraConfig dstCfg = loadInstance(path_, to);
    setRunning(true);
    if (logAction_) logAction_->setChecked(true);  // reveal the log pane for the streamed output
    const QString title = op == 0
                              ? QStringLiteral("transferring apps '%1' → '%2'").arg(from, to)
                              : op == 1 ? QStringLiteral("exporting APKs from '%1'").arg(from)
                                        : QStringLiteral("importing APKs into '%1'").arg(to);
    statusBar()->showMessage(title + QStringLiteral("…"));
    appendLog(QStringLiteral("=== %1%2 ===").arg(title,
                                                 dry ? QStringLiteral(" (dry run)") : QString()));
    auto *w = new TransferAppsWorker(wop, inferBackend(srcCfg), srcCfg,
                                     from, inferBackend(dstCfg), dstCfg,
                                     to, pkgs, dir, dry, withData, this);
    connect(w, &TransferAppsWorker::sigLine, this, &MainWindow::appendLog);
    connect(w, &TransferAppsWorker::sigDone, this, [this, wop, to](bool ok, QString detail) {
        setRunning(false);
        // A transfer into the ACTIVE profile may have carried window modes into its saved
        // config; refresh the in-memory copy or the next autosave writes the stale list back
        // (the bd remora-4ei.73 carry, same pattern as the app-menu handlers above).
        if (wop == TransferAppsWorker::Op::Transfer && to == instance_)
            cfg_.integration.appModes = loadInstance(path_, instance_).integration.appModes;
        appendLog(detail);
        statusBar()->showMessage(detail, ok ? 10000 : 0);
    });
    connect(w, &TransferAppsWorker::finished, w, &QObject::deleteLater);
    w->start();
}

// docker-pull a registry prebuilt onto the docker host (bare = local), streaming the layer
// progress to the Progress page. Idempotent: an up-to-date image is a fast no-op.
// Copy a docker image between hosts: `docker save <tag>` on the source host piped into
// `docker load` on the target. The local machine orchestrates the pipe, so it works for any
// src/target combination (local↔remote, remote↔remote). Streamed to the Log pane.
void MainWindow::onPushImage() {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"), 5000);
        return;
    }
    auto *item = archivesTree_ ? archivesTree_->currentItem() : nullptr;
    const QString tag = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
    if (tag.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("select a docker image row first"), 4000);
        return;
    }
    const QString srcHost = item->data(0, Qt::UserRole + 2).toString();  // where it lives now
    // default target: the current profile's deploy/ssh host (where you'd run it)
    const QString defTarget =
        currentBackend() == Backend::Bare
            ? QString()
            : cfg_.backend.sshHost.value_or(QString()).trimmed();
    bool ok = false;
    const QString target = QInputDialog::getText(
        this, QStringLiteral("Push image to host"),
        QStringLiteral("Copy '%1' to which docker host?\n(user@host — blank = this machine)")
            .arg(tag),
        QLineEdit::Normal, defTarget, &ok).trimmed();
    if (!ok) return;
    if (target == srcHost) {
        statusBar()->showMessage(QStringLiteral("image is already on %1")
                                     .arg(target.isEmpty() ? QStringLiteral("this machine")
                                                           : target), 5000);
        return;
    }
    // A -src build is inert on a deploy target — profiles reference the base tag, so a pushed
    // -src image sits unused until someone retags it by hand on the guest. Offer to promote it
    // on arrival (default Yes) so the next Connect/Up deploys it; No keeps the staging behavior
    // (nothing is activated anywhere, the source keeps its -src archive copy). Promoting must
    // move the SOURCE's plain tag too: left on the old build, it fails every later Connect's
    // ensure-image layer-compare against the target (bd remora-blo).
    QString promoteBase;
    if (tag.endsWith(QLatin1String("-src"))) {
        const QString base = tag.left(tag.size() - 4);
        if (QMessageBox::question(
                this, QStringLiteral("Promote on target"),
                QStringLiteral(
                    "After the copy, rename %1 → %2 on %3 so the next Connect/Up deploys it?\n\n"
                    "The same rename happens on %4 — both hosts' %2 must stay in step or the "
                    "next Connect fails its image check. The old %2 image stays pinned by the "
                    "running container until it recreates. Choose No to stage the -src image "
                    "without activating it.")
                    .arg(tag, base,
                         target.isEmpty() ? QStringLiteral("this machine") : target,
                         srcHost.isEmpty() ? QStringLiteral("this machine") : srcHost),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
            == QMessageBox::Yes)
            promoteBase = base;
    }
    // build the save|load pipeline; the orchestrating machine (local) runs it via sh -c, so it
    // works for any src/target combination. Each remote side is a single quoted ssh command.
    const QString saveCmd = srcHost.isEmpty()
                                ? QStringLiteral("docker save %1").arg(shellQuote(tag))
                                : QStringLiteral("ssh %1 %2").arg(srcHost,
                                      shellQuote(QStringLiteral("docker save %1").arg(tag)));
    const QString loadCmd = target.isEmpty()
                                ? QStringLiteral("docker load")
                                : QStringLiteral("ssh %1 %2").arg(target,
                                      shellQuote(QStringLiteral("docker load")));
    QString cmd = saveCmd + QStringLiteral(" | ") + loadCmd;
    if (!promoteBase.isEmpty()) {
        // MOVE the tag like onPromoteImage does (tag then untag -src); the subshell keeps a
        // benign rmi failure from failing the whole push, while a failed `docker tag` still does.
        const QString retag = QStringLiteral("docker tag %1 %2 && (docker rmi %1 >/dev/null 2>&1 || true)")
                                  .arg(tag, promoteBase);
        cmd += QStringLiteral(" && ")
               + (target.isEmpty() ? retag
                                   : QStringLiteral("ssh %1 %2").arg(target, shellQuote(retag)));
        // ...and the same move on the source, so its plain tag tracks the promoted build
        // instead of going stale (bd remora-blo).
        cmd += QStringLiteral(" && ")
               + (srcHost.isEmpty() ? retag
                                    : QStringLiteral("ssh %1 %2").arg(srcHost, shellQuote(retag)));
    }
    // The multi-GB load fills the target's page cache, which forces the running Android
    // container's memory into zram and its file working set out of cache — minutes of A/V
    // stutter while it faults back (bd remora-8xq). Drop the now-useless cache via the
    // guest helper. Best-effort: sudo -n only succeeds where the one-time root install
    // (remora-drop-caches-install.sh) has been run; anywhere else this is a silent no-op.
    {
        const QString drop = QStringLiteral(
            "(sudo -n /usr/local/sbin/remora-drop-caches >/dev/null 2>&1 || true)");
        cmd += QStringLiteral(" && ")
               + (target.isEmpty() ? drop
                                   : QStringLiteral("ssh %1 %2").arg(target, shellQuote(drop)));
    }
    checklist_->clear();
    onState(QStringLiteral("push-image"), QStringLiteral("running"),
            QStringLiteral("%1 → %2 …").arg(srcHost.isEmpty() ? QStringLiteral("local") : srcHost,
                                            target.isEmpty() ? QStringLiteral("local") : target),
            QString());
    setRunning(true);
    auto *thread = QThread::create([this, cmd, tag, target, promoteBase] {
        QElapsedTimer clock;
        clock.start();
        const ShellResult r = runShellRc(QString(), cmd, 1800000);  // 30 min, multi-GB transfer
        const qint64 s = clock.elapsed() / 1000;
        const QString took = QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10,
                                                                     QLatin1Char('0'));
        QMetaObject::invokeMethod(this, [this, r, tag, target, took, promoteBase] {
            const bool okk = r.rc == 0;
            const QString where = target.isEmpty() ? QStringLiteral("local") : target;
            onState(QStringLiteral("push-image"),
                    okk ? QStringLiteral("ok") : QStringLiteral("failed"),
                    okk ? (promoteBase.isEmpty()
                               ? QStringLiteral("%1 copied to %2 in %3").arg(tag, where, took)
                               : QStringLiteral("%1 copied to %2 and promoted to %3 on both hosts "
                                                "in %4 — press Connect to deploy it")
                                     .arg(tag, where, promoteBase, took))
                        : QStringLiteral("push failed (exit %1): %2")
                              .arg(r.rc).arg(r.out.trimmed().section(QLatin1Char('\n'), -1)),
                    r.out);
            setRunning(false);
            onRefreshArchives();  // the target now has the image
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// Fetch a source row's image onto the build host (bd remora-pht9): stream the source's archive
// straight into `docker load` — curl for an http(s) mirror, `ssh … cat` for a remote directory,
// `cat` for a local one. The local machine orchestrates the pipe like onPushImage, so every
// source/build-host combination works; no archive is written anywhere, and the refresh at the
// end flips the row to "local ✓".
void MainWindow::onFetchSourceImage() {
    if (running_) {
        statusBar()->showMessage(QStringLiteral("an operation is already running — wait for it to "
                                                "finish before starting another"), 5000);
        return;
    }
    auto *item = archivesTree_ ? archivesTree_->currentItem() : nullptr;
    const QString loc = item ? item->data(0, Qt::UserRole + 5).toString() : QString();
    if (loc.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("select a source row (marked ‘needs fetch ↓’) first"), 4000);
        return;
    }
    const QString tag = item->data(0, Qt::UserRole + 3).toString();
    const QString file = item->data(0, Qt::UserRole + 4).toString();
    const QString sourceName = item->data(0, Qt::UserRole + 6).toString();
    if (item->text(2).startsWith(QLatin1String("local"))) {
        statusBar()->showMessage(QStringLiteral("%1 is already local").arg(tag), 4000);
        return;
    }
    const bool http = loc.startsWith(QLatin1String("http://"))
                      || loc.startsWith(QLatin1String("https://"));
    // A mirror index row may name only a tag — then there is no archive URL to fetch. The
    // contract (bd remora-2ylt) makes the file column optional, so this is the mirror's way of
    // saying "exists, not downloadable"; surface that instead of guessing a filename.
    if (file.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("the mirror's index names no archive file for %1 — nothing to fetch")
                .arg(tag), 6000);
        return;
    }
    const QString host = buildHostStr();
    const QString what = tag.isEmpty() ? file : tag;
    const QString size = item->text(3);
    if (QMessageBox::question(
            this, QStringLiteral("Fetch image"),
            QStringLiteral("Fetch %1%2 from ‘%3’ onto %4?\n\nThe archive streams straight into "
                           "docker load — nothing is saved to disk.")
                .arg(what, size.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(size),
                     sourceName,
                     host.isEmpty() ? QStringLiteral("this machine") : host))
        != QMessageBox::Yes)
        return;
    // Producer side of the pipe, by location shape (the same three scanSourceLocation reads).
    QString catCmd;
    if (http) {
        catCmd = QStringLiteral("curl -fsSL %1").arg(shellQuote(loc + QLatin1Char('/') + file));
    } else {
        QString shost, dir = loc;  // user@host:/dir, or a plain local directory
        const int colon = loc.indexOf(QLatin1Char(':'));
        if (colon > 0 && !loc.startsWith(QLatin1Char('/'))) {
            shost = loc.left(colon);
            dir = loc.mid(colon + 1);
        }
        const QString cat =
            QStringLiteral("cat %1").arg(shellQuote(dir + QLatin1Char('/') + file));
        catCmd = shost.isEmpty() ? cat : QStringLiteral("ssh %1 %2").arg(shost, shellQuote(cat));
    }
    const QString loadCmd = host.isEmpty()
                                ? QStringLiteral("docker load")
                                : QStringLiteral("ssh %1 %2").arg(host,
                                      shellQuote(QStringLiteral("docker load")));
    QString cmd = catCmd + QStringLiteral(" | ") + loadCmd;
    // Same page-cache eviction as onPushImage: the multi-GB load forces a running container's
    // working set into zram (bd remora-8xq). Best-effort where the root helper is installed.
    {
        const QString drop = QStringLiteral(
            "(sudo -n /usr/local/sbin/remora-drop-caches >/dev/null 2>&1 || true)");
        cmd += QStringLiteral(" && ")
               + (host.isEmpty() ? drop
                                 : QStringLiteral("ssh %1 %2").arg(host, shellQuote(drop)));
    }
    checklist_->clear();
    onState(QStringLiteral("fetch-image"), QStringLiteral("running"),
            QStringLiteral("%1 from %2 → %3 …")
                .arg(what, sourceName,
                     host.isEmpty() ? QStringLiteral("local") : host),
            QString());
    setRunning(true);
    auto *thread = QThread::create([this, cmd, what, sourceName, host] {
        QElapsedTimer clock;
        clock.start();
        const ShellResult r = runShellRc(QString(), cmd, 1800000);  // 30 min, multi-GB transfer
        const qint64 s = clock.elapsed() / 1000;
        const QString took = QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10,
                                                                     QLatin1Char('0'));
        QMetaObject::invokeMethod(this, [this, r, what, sourceName, host, took] {
            const bool okk = r.rc == 0;
            onState(QStringLiteral("fetch-image"),
                    okk ? QStringLiteral("ok") : QStringLiteral("failed"),
                    okk ? QStringLiteral("%1 fetched from %2 to %3 in %4")
                              .arg(what, sourceName,
                                   host.isEmpty() ? QStringLiteral("local") : host, took)
                        : QStringLiteral("fetch failed (exit %1): %2")
                              .arg(r.rc).arg(r.out.trimmed().section(QLatin1Char('\n'), -1)),
                    r.out);
            setRunning(false);
            onRefreshArchives();  // the row flips to "local ✓"
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::onDone(bool ok) {
    setRunning(false);
    rebuildRunTable();  // reflect the new container state without a manual Refresh
    statusBar()->showMessage(ok ? QStringLiteral("connected — the mirror window is up.")
                                : QStringLiteral("not connected — open the Log pane (toolbar) for details."),
                             ok ? 6000 : 0);
}

void MainWindow::onConflict(QString holder) {
    statusBar()->showMessage(
        QStringLiteral("‘%1’ already holds this target — press Stop, then Deploy again.").arg(holder),
        0);
}

}  // namespace remora
