// Desktop app-menu integration: one launcher folder per profile, one .desktop per installed app
// (remora-app-menu.sh does the file work; the agent's IconDump renders real icons on the device).
// The generate/remove work runs through the injectable Spawner; the read-only entry scan is plain
// filesystem.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

#include "core/Resolver.h"
#include "engine/Engine.h"

namespace remora {

// Must match slugify() in vendor/host-scripts/remora-app-menu.sh.
QString appMenuSlug(const QString &instance) {
    QString s = instance.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    s.remove(QRegularExpression(QStringLiteral("^-|-$")));
    return s;
}

static QString dataHome() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
}

// "<pkg>=desktop|freeform" list helpers — the stored answer of the first-launch prompt.
QString appModeFor(const RemoraConfig &cfg, const QString &pkg) {
    const QString prefix = pkg + QLatin1Char('=');
    for (const QString &m : cfg.integration.appModes)
        if (m.startsWith(prefix)) return m.mid(prefix.size());
    return {};
}

void setAppMode(RemoraConfig &cfg, const QString &pkg, const QString &mode) {
    const QString prefix = pkg + QLatin1Char('=');
    QStringList &modes = cfg.integration.appModes;
    for (int i = modes.size() - 1; i >= 0; --i)
        if (modes[i].startsWith(prefix)) modes.removeAt(i);
    if (!mode.isEmpty()) modes << prefix + mode;
}

// ---- arm64 pins ----------------------------------------------------------
// An app with no working x86_64 build is installed as arm64 and runs under the native bridge.
// Play does not know that: it sees an x86_64 device and re-delivers the x86_64 split at the next
// update, which puts the app back to whatever was broken about it (TikTok: dies at spawn;
// Flutter apps: the split omits libflutter.so). Pinning records "this one is arm64 on purpose"
// so the guard can undo that silently, instead of the user rediscovering it as a crash.

// Same file-static as Spawner.cpp/Chain.cpp keep: the probe script below is full of quotes and
// '$', so it cannot ride the plain "sh -c \"%1\"" interpolation those files warn about.
static QString shSingleQuote(const QString &s) {
    return QLatin1Char('\'') + QString(s).replace(QLatin1String("'"), QLatin1String("'\\''"))
           + QLatin1Char('\'');
}

bool isArmPinned(const RemoraConfig &cfg, const QString &pkg) {
    return cfg.integration.armApps.contains(pkg);
}

void setArmPinned(RemoraConfig &cfg, const QString &pkg, bool pinned) {
    QStringList &pins = cfg.integration.armApps;
    pins.removeAll(pkg);
    if (pinned) pins << pkg;
}

QStringList armPinsNeedingRepair(Spawner &sp, const RemoraConfig &cfg, Backend backend) {
    const QStringList &pins = cfg.integration.armApps;
    if (pins.isEmpty()) return {};
    const ResolvedConfig rc = resolve(cfg, backend);
    // Ask the device in ONE exec: for each pin, print the package iff it is installed and its
    // primaryCpuAbi is no longer arm64. A package that is not installed at all is NOT a repair
    // candidate — reinstalling something the user removed would be worse than the crash.
    QString script;
    for (const QString &pkg : pins) {
        // Pins come from package names the device itself reported; guard anyway, since a
        // hand-edited remorarc reaches this shell.
        static const QRegularExpression kPkg(QStringLiteral("^[A-Za-z0-9_.]+$"));
        if (!kPkg.match(pkg).hasMatch()) continue;
        script += QStringLiteral("d=$(dumpsys package %1 2>/dev/null); "
                                 "case \"$d\" in *primaryCpuAbi*) "
                                 "echo \"$d\" | grep -q 'primaryCpuAbi=arm64-v8a' || echo %1;; "
                                 "esac; ")
                      .arg(pkg);
    }
    if (script.isEmpty()) return {};
    const QString c = rc.containerName;
    const ProcResult r =
        backend == Backend::Bare
            ? sp.run({"docker", "exec", c, "sh", "-c", script}, {}, {})
            : sp.run(Spawner::sshArgv(rc.sshHost.value_or(QString()),
                                      QStringLiteral("docker exec %1 sh -c %2")
                                          .arg(c, shSingleQuote(script))),
                     {}, {});
    if (r.rc != 0) return {};  // device down / docker gone — nothing to repair right now
    QStringList out;
    for (const QString &l : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString p = l.trimmed();
        if (pins.contains(p)) out << p;
    }
    return out;
}

QVector<AppMenuEntry> appMenuEntries(const QString &slug) {
    QVector<AppMenuEntry> out;
    const QDir apps(dataHome() + QStringLiteral("/applications"));
    // ONE ARTIFACT SCHEME (bd remora-28ix.4 step 4). This used to scan the pre-rename
    // names too; the next refresh regenerated under remora-* and swept the old names, so the window
    // was always meant to close, and it has. A leftover pre-rename artifact is now invisible here
    // and has to be deleted by hand from ~/.local/share/{applications,desktop-directories}.
    const QString cat = QStringLiteral("Categories=X-Remora-") + slug + QLatin1Char(';');
    const QStringList files = apps.entryList({QStringLiteral("remora-*.desktop")}, QDir::Files);
    for (const QString &f : files) {
        QFile af(apps.filePath(f));
        if (!af.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString body = QString::fromUtf8(af.readAll());
        if (!body.contains(QLatin1Char('\n') + cat + QLatin1Char('\n'))) continue;
        AppMenuEntry e;
        for (const QString &line : body.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("Name=")) && e.name.isEmpty())
                e.name = line.mid(5);
            else if (line.startsWith(QStringLiteral("X-Remora-App=")))
                e.pkg = line.mid(int(qstrlen("X-Remora-App=")));
        }
        if (!e.pkg.isEmpty()) out << e;  // app entries only (mirror/desktop links carry no pkg)
    }
    std::sort(out.begin(), out.end(),
              [](const AppMenuEntry &a, const AppMenuEntry &b) { return a.name < b.name; });
    return out;
}

bool appMenuSync(Spawner &sp, const RemoraConfig &cfg, Backend backend, const QString &instance,
                 LogSink &sink) {
    const QString step = QStringLiteral("app-menu");
    sink.state(step, StepState::Running, QStringLiteral("refreshing the '%1' app-menu folder")
                                             .arg(instance));
    const RunContext ctx = makeContext(cfg, backend, std::nullopt, QString());
    const QString target = resolveAdbTarget(sp, ctx);
    // Each entry launches `<this-binary> app "<profile>" <pkg> --title "<Name>"` — quoted, profile
    // and app names have spaces. The folder also gets plain-device entries: mirror, desktop mode
    // and the PIP toggle — all three are per-profile, so each folder drives its OWN device.
    const QString self = QCoreApplication::applicationFilePath();
    const QString execTmpl =
        QStringLiteral("\"%1\" app \"%2\" %PKG% --title \"%NAME%\"").arg(self, instance);
    const QString execMirror = QStringLiteral("\"%1\" up \"%2\"").arg(self, instance);
    const QString execDesktop = QStringLiteral("\"%1\" desktop \"%2\"").arg(self, instance);
    const QString execPip = QStringLiteral("\"%1\" pip \"%2\"").arg(self, instance);
    const QString script =
        vendorScript(QStringLiteral("host-scripts"), QStringLiteral("remora-app-menu.sh"));

    // No device: refresh the DEVICE-LEVEL entries anyway and leave the app list alone. Only the app
    // list needs adb — mirror/desktop/PIP are pure profile metadata. Bailing here meant a profile
    // got its buttons only while its container happened to be running, which is precisely when a
    // launcher entry is not what you reach for; with two profiles and one container at a time, the
    // idle profile could never be given a PIP entry at all.
    if (target.isEmpty()) {
        const ProcResult p = sp.run(
            {QStringLiteral("bash"), script, QStringLiteral("--plain"), instance, execMirror,
             execDesktop, execPip},
            {}, [&](const QString &stream, const QString &text) { sink.line(step, stream, text); });
        sink.state(step, p.rc == 0 ? StepState::Ok : StepState::Failed,
                   p.rc == 0 ? QStringLiteral("'%1' is not running — refreshed its device entries; "
                                              "the app list needs the device")
                                   .arg(instance)
                             : QStringLiteral("could not resolve the device's adb address"),
                   p.stderrTail);
        return p.rc == 0;
    }
    sp.run({QStringLiteral("adb"), QStringLiteral("connect"), target}, {}, {});
    QMap<QString, QString> env;
    // The fallback renderer, for an image whose agent predates IconDump; absent in a public
    // checkout, where the script says so and the entries stay generic.
    env.insert(QStringLiteral("ICONDUMP_DEX"),
               vendorScript(QStringLiteral("host-scripts"), QStringLiteral("icondump.dex")));
    // The script's app enumeration runs the in-house client (remora mirror --list-apps): hand it
    // the binary this engine IS. Nothing is pushed to the device for it — the in-image agent
    // answers.
    env.insert(QStringLiteral("REMORA_BIN"), self);
    const ProcResult r = sp.run(
        {QStringLiteral("bash"), script, target, execTmpl, instance, execMirror, execDesktop,
         execPip},
        env, [&](const QString &stream, const QString &text) { sink.line(step, stream, text); });
    // rc 3 is the script's boot-settling guard KEEPING the existing menu because the device is
    // not answering with a full app list yet — its designed outcome on a cold deploy, where this
    // step runs moments after boot. Reported as a skip with the reason, not a failure: a step
    // that fails on every single run trains the eye to ignore step states (bd remora-5sd).
    const bool kept = r.rc == 3;
    sink.state(step,
               r.rc == 0 ? StepState::Ok : (kept ? StepState::Skipped : StepState::Failed),
               r.rc == 0
                   ? QStringLiteral("menu folder '%1' refreshed").arg(instance)
                   : (kept ? QStringLiteral("device still settling — kept the existing '%1' "
                                            "menu (it refreshes on the next Apps visit)")
                                 .arg(instance)
                           : QStringLiteral("menu refresh failed (rc %1)").arg(r.rc)),
               r.stderrTail);
    return r.rc == 0 || kept;
}

bool appMenuRemove(Spawner &sp, const QString &instance, LogSink &sink) {
    const QString step = QStringLiteral("app-menu");
    const ProcResult r = sp.run(
        {QStringLiteral("bash"),
         vendorScript(QStringLiteral("host-scripts"), QStringLiteral("remora-app-menu.sh")),
         QStringLiteral("--remove"), instance},
        {}, [&](const QString &stream, const QString &text) { sink.line(step, stream, text); });
    sink.state(step, r.rc == 0 ? StepState::Ok : StepState::Failed,
               r.rc == 0 ? QStringLiteral("menu folder '%1' removed").arg(instance)
                         : QStringLiteral("menu removal failed (rc %1)").arg(r.rc),
               r.stderrTail);
    return r.rc == 0;
}

}  // namespace remora
