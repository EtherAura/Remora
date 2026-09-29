#include "engine/AppTransfer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/Parsers.h"
#include "core/Resolver.h"

namespace remora {

static void say(const LineSink &log, const QString &text) {
    if (log) log(QStringLiteral("stdout"), text);
}

QStringList listThirdPartyPackages(Spawner &sp, const QString &target) {
    return parsePmPackages(
        sp.run({"adb", "-s", target, "shell", "pm list packages -3"}, {}, {}).out);
}

QStringList pullApks(Spawner &sp, const QString &target, const QString &pkg,
                     const QString &destDir, QString *err) {
    const QStringList apks =
        parsePmPaths(sp.run({"adb", "-s", target, "shell", "pm path " + pkg}, {}, {}).out);
    if (apks.isEmpty()) {
        if (err) *err = QStringLiteral("no APK path (not installed?)");
        return {};
    }
    const QString dir = destDir + QLatin1Char('/') + pkg;
    QDir().mkpath(dir);
    QStringList local;
    for (const QString &apk : apks) {
        const QString lp = dir + QLatin1Char('/') + apk.section(QLatin1Char('/'), -1);
        if (sp.run({"adb", "-s", target, "pull", apk, lp}, {}, {}).rc != 0) {
            if (err) *err = QStringLiteral("could not pull %1").arg(apk);
            return {};
        }
        local << lp;
    }
    return local;
}

bool installApks(Spawner &sp, const QString &target, const QStringList &apks, QString *err) {
    if (apks.isEmpty()) return false;
    QStringList argv{"adb", "-s", target, "install-multiple", "-r", "-g"};
    argv << apks;
    const ProcResult r = sp.run(argv, {}, {});
    if (r.rc != 0 && err) *err = r.stderrTail.trimmed().left(120);
    return r.rc == 0;
}

QStringList apksIn(const QString &dir) {
    QStringList out = QDir(dir).entryList({QStringLiteral("*.apk")}, QDir::Files, QDir::Name);
    for (QString &f : out) f = dir + QLatin1Char('/') + f;
    return out;
}

QString assertAdbEndpoint(Spawner &sp, const QString &target) {
    if (target.isEmpty()) return {};
    sp.run({"adb", "connect", target}, {}, {});
    return sp.run({"adb", "-s", target, "get-state"}, {}, {}).out.trimmed() ==
                   QLatin1String("device")
               ? target
               : QString();
}

QList<ApkJob> importJobs(const QString &dir) {
    QList<ApkJob> jobs;
    for (const QString &sub : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QStringList apks = apksIn(dir + QLatin1Char('/') + sub);
        if (!apks.isEmpty()) jobs << qMakePair(sub, apks);
    }
    for (const QString &loose : apksIn(dir))
        jobs << qMakePair(QFileInfo(loose).fileName(), QStringList{loose});
    return jobs;
}

// Same file-static as Spawner.cpp/AppMenu.cpp/Chain.cpp keep: the scripts below are full of
// quotes and '$', so they cannot ride a plain "sh -c \"%1\"" interpolation.
static QString shSingleQuote(const QString &s) {
    return QLatin1Char('\'') + QString(s).replace(QLatin1String("'"), QLatin1String("'\\''"))
           + QLatin1Char('\'');
}

RootExec rootExecFor(const RemoraConfig &cfg, Backend backend) {
    const ResolvedConfig rc = resolve(cfg, backend);
    RootExec r;
    r.container = rc.containerName;
    if (backend != Backend::Bare) r.sshHost = rc.sshHost;
    return r;
}

// Run one script as uid 0 on a side. docker exec when we know the container — the same route
// Deployers uses for every other privileged bit of container work — and the adb `su` route only
// as the no-profile fallback (see RootExec for why that route cannot work on a container).
// `adbTarget` is used by the fallback alone.
static ProcResult runAsRoot(Spawner &sp, const RootExec &root, const QString &adbTarget,
                            const QString &script) {
    if (root.container.isEmpty())
        // `su` IS NOT ON AN APP-VISIBLE PATH and must not be put back on one (bd
        // remora-4ei.101), so this has to say where it actually lives. Same prefix and order as
        // the root probe in Deployers.cpp: /debug_ramdisk is current MAGISKTMP, /sbin is where
        // images built before bd remora-4ei.94 put it.
        return sp.run({"adb", "-s", adbTarget, "shell",
                       QStringLiteral("export PATH=/debug_ramdisk:/sbin:$PATH; su -c %1")
                           .arg(shSingleQuote(script))},
                      {}, {});
    if (root.sshHost && !root.sshHost->isEmpty())
        return sp.run(Spawner::sshArgv(*root.sshHost, QStringLiteral("docker exec %1 sh -c %2")
                                                          .arg(root.container,
                                                               shSingleQuote(script))),
                      {}, {});
    return sp.run({"docker", "exec", root.container, "sh", "-c", script}, {}, {});
}

// The staged tar's device-side path. /data/local/tmp because both `adb pull` and `adb push` run
// unprivileged — the rooted halves chmod it readable and clean it up.
static QString dataTarPath(const QString &pkg) {
    return QStringLiteral("/data/local/tmp/remora-appdata-%1.tar").arg(pkg);
}

// force-stop first for a consistent snapshot (';' — a package that is not running must not fail
// the pack). The two excludes are defensive rather than load-bearing on a container; see the
// header for what was measured.
static QString packScript(const QString &pkg, const QString &tar) {
    return QStringLiteral("am force-stop %1; tar -C /data/data -cf %2 --exclude=%1/lib "
                          "--exclude=%1/cache --exclude=%1/code_cache %1 && chmod 644 %2")
        .arg(pkg, tar);
}

// One script so the ordering is not negotiable: read the uid/gid the TARGET's install assigned
// BEFORE the untar clobbers the directory, untar, chown back to that owner, then restorecon —
// after the chown, because on a device that HAS SELinux the per-app MLS categories are derived
// from the owning uid (seapp_contexts levelFrom), which is exactly why the source's contexts
// could never be copied. The tar is removed whichever way the chain went; the chain's own rc
// survives the cleanup.
static QString restoreScript(const QString &pkg, const QString &tar) {
    // %u/%g are stat's, not arg()'s — only %<digit> is a place marker.
    return QStringLiteral("am force-stop %1; U=$(stat -c %u /data/data/%1) && "
                          "G=$(stat -c %g /data/data/%1) && tar -C /data/data -xf %2 && "
                          "chown -R $U:$G /data/data/%1 && restorecon -RF /data/data/%1; "
                          "rc=$?; rm -f %2; exit $rc")
        .arg(pkg, tar);
}

bool transferAppData(Spawner &sp, const QString &src, const QString &dst, const RootExec &srcRoot,
                     const RootExec &dstRoot, const QString &pkg, const QString &stageDir,
                     QString *err) {
    const QString tar = dataTarPath(pkg);
    const QString local = stageDir + QLatin1Char('/') + pkg + QStringLiteral("-data.tar");
    const QString rm = QStringLiteral("rm -f %1").arg(tar);
    const ProcResult pack = runAsRoot(sp, srcRoot, src, packScript(pkg, tar));
    if (pack.rc != 0) {
        if (err) *err = QStringLiteral("source pack failed: %1").arg(pack.stderrTail.trimmed().left(120));
        runAsRoot(sp, srcRoot, src, rm);
        return false;
    }
    const bool pulled = sp.run({"adb", "-s", src, "pull", tar, local}, {}, {}).rc == 0;
    runAsRoot(sp, srcRoot, src, rm);
    if (!pulled) {
        if (err) *err = QStringLiteral("could not pull %1").arg(tar);
        return false;
    }
    if (sp.run({"adb", "-s", dst, "push", local, tar}, {}, {}).rc != 0) {
        if (err) *err = QStringLiteral("could not push the data tar to the target");
        return false;
    }
    const ProcResult restore = runAsRoot(sp, dstRoot, dst, restoreScript(pkg, tar));
    if (restore.rc != 0) {
        if (err)
            *err = QStringLiteral("target restore failed: %1")
                       .arg(restore.stderrTail.trimmed().left(120));
        return false;
    }
    return true;
}

AppOpCounts transferApps(Spawner &sp, const QString &src, const QString &dst,
                         const QStringList &pkgs, bool dryRun, bool withData,
                         const RootExec &srcRoot, const RootExec &dstRoot, const LineSink &log) {
    AppOpCounts c;
    QTemporaryDir stage;
    if (!dryRun && !stage.isValid()) {
        say(log, QStringLiteral("could not create a staging dir"));
        c.failed = pkgs.size();
        return c;
    }
    for (const QString &pkg : pkgs) {
        if (dryRun) {
            // Report the split count without pulling anything, so a dry run stays read-only.
            const int n =
                parsePmPaths(sp.run({"adb", "-s", src, "shell", "pm path " + pkg}, {}, {}).out)
                    .size();
            if (n == 0) {
                say(log, QStringLiteral("  skip %1 — no APK path (not installed?)").arg(pkg));
                ++c.skipped;
                continue;
            }
            say(log, QStringLiteral("  would transfer %1  (%2 apk%3)%4")
                         .arg(pkg)
                         .arg(n)
                         .arg(n > 1 ? QStringLiteral("s, split") : QString())
                         .arg(withData ? QStringLiteral("  + app data") : QString()));
            ++c.done;
            if (withData) ++c.dataDone;
            continue;
        }
        QString err;
        const QStringList local = pullApks(sp, src, pkg, stage.path(), &err);
        if (local.isEmpty()) {
            say(log, QStringLiteral("  FAIL %1 — %2").arg(pkg, err));
            ++c.failed;
            continue;
        }
        if (installApks(sp, dst, local, &err)) {
            say(log, QStringLiteral("  ok   %1%2").arg(
                         pkg, local.size() > 1 ? QStringLiteral("  (split)") : QString()));
            ++c.done;
            c.transferred << pkg;
            // Data rides only behind a real install: the install is what created the target
            // dir and assigned the uid the restore chowns to. Its failure is tallied apart —
            // the app itself DID transfer, it is merely logged out like a default transfer.
            if (withData) {
                QString derr;
                if (transferAppData(sp, src, dst, srcRoot, dstRoot, pkg, stage.path(), &derr)) {
                    say(log, QStringLiteral("  data ok   %1").arg(pkg));
                    ++c.dataDone;
                } else {
                    say(log, QStringLiteral("  data FAIL %1 — %2 (app installed, logged out)")
                                 .arg(pkg, derr));
                    ++c.dataFailed;
                }
            }
        } else {
            say(log, QStringLiteral("  FAIL %1 — %2").arg(pkg, err));
            ++c.failed;
        }
        QDir(stage.path() + QLatin1Char('/') + pkg).removeRecursively();
        QFile::remove(stage.path() + QLatin1Char('/') + pkg + QStringLiteral("-data.tar"));
    }
    return c;
}

int carryAppModes(const RemoraConfig &src, RemoraConfig &dst, const QStringList &pkgs) {
    int carried = 0;
    for (const QString &pkg : pkgs) {
        const QString mode = appModeFor(src, pkg);
        if (mode.isEmpty()) continue;               // source never answered the prompt
        if (!appModeFor(dst, pkg).isEmpty()) continue;  // target's own answer wins
        setAppMode(dst, pkg, mode);
        ++carried;
    }
    return carried;
}

AppOpCounts exportApks(Spawner &sp, const QString &src, const QStringList &pkgs,
                       const QString &dir, const LineSink &log) {
    AppOpCounts c;
    for (const QString &pkg : pkgs) {
        QString err;
        const QStringList local = pullApks(sp, src, pkg, dir, &err);
        if (local.isEmpty()) {
            say(log, QStringLiteral("  FAIL %1 — %2").arg(pkg, err));
            ++c.failed;
            continue;
        }
        say(log, QStringLiteral("  ok   %1  (%2 apk%3)")
                     .arg(pkg)
                     .arg(local.size())
                     .arg(local.size() > 1 ? QStringLiteral("s, split") : QString()));
        ++c.done;
    }
    return c;
}

AppOpCounts importApks(Spawner &sp, const QString &dst, const QList<ApkJob> &jobs, bool dryRun,
                       const LineSink &log) {
    AppOpCounts c;
    for (const ApkJob &job : jobs) {
        if (dryRun) {
            say(log, QStringLiteral("  would install %1  (%2 apk%3)")
                         .arg(job.first)
                         .arg(job.second.size())
                         .arg(job.second.size() > 1 ? QStringLiteral("s, split") : QString()));
            ++c.done;
            continue;
        }
        QString err;
        if (installApks(sp, dst, job.second, &err)) {
            say(log, QStringLiteral("  ok   %1%2").arg(
                         job.first,
                         job.second.size() > 1 ? QStringLiteral("  (split)") : QString()));
            ++c.done;
        } else {
            say(log, QStringLiteral("  FAIL %1 — %2").arg(job.first, err));
            ++c.failed;
        }
    }
    return c;
}

}  // namespace remora
