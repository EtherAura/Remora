#include "ui/Workers.h"

#include <QDir>

#include "core/Builders.h"
#include "engine/AppTransfer.h"
#include "store/Store.h"
#include "core/Features.h"
#include "core/ImageBuild.h"
#include "core/Prereqs.h"
#include "core/Resolver.h"

namespace remora {

EngineWorker::EngineWorker(Backend backend, RemoraConfig cfg, QString windowTitle, Mode mode,
                           QObject *parent)
    : QThread(parent), backend_(backend), cfg_(std::move(cfg)),
      windowTitle_(std::move(windowTitle)), mode_(mode) {}

void EngineWorker::line(const QString &step, const QString &, const QString &text) {
    emit sigLine(step + QStringLiteral(": ") + text);
}

void EngineWorker::state(const QString &step, StepState st, const QString &detail,
                         const QString &stderrTail) {
    emit sigState(step, stepStateToString(st), detail, stderrTail);
}

void EngineWorker::run() {
    RealSpawner sp;
    bool ok;
    const auto onConflict = [this](const Conflict &c) {
        // The signal carries the holder AND what it holds: with per-profile containers a bare
        // name no longer tells the user why their profile was refused (bd remora-4u4.1).
        emit sigConflict(QStringLiteral("%1 (using %2)").arg(c.holder, c.reason));
        return false;  // MVP: decline; the dashboard offers Stop
    };
    // Every one of these used to pass std::nullopt, so the GUI resolved a DIFFERENT config from the
    // CLI for the same profile: gpu_mode=host silently fell back to SwiftShader, and an explicit
    // venus=true failed its preflight claiming the render server and guest libs were missing when
    // they were installed and `remora up` used them fine (bd remora-4ei.85). Probed once here and
    // shared by every mode — deployCapabilities() is a no-op for profiles that do not need it.
    const std::optional<HostCapabilities> caps = deployCapabilities(backend_, cfg_, sp);
    if (mode_ == Reconnect)
        ok = reconnectRun(backend_, cfg_, *this, sp, caps, windowTitle_);
    else if (mode_ == Smart)
        ok = smartRun(backend_, cfg_, *this, sp, caps, windowTitle_, onConflict);
    else if (mode_ == Fullscreen)
        ok = smartRun(backend_, cfg_, *this, sp, caps, windowTitle_, onConflict,
                      /*fullscreenDisplay=*/true);
    else  // Sleep
        ok = sleepRun(backend_, cfg_, *this, sp, caps, windowTitle_);
    emit sigDone(ok);
    // Post-connect app-menu refresh: after sigDone (the mirror is already up — this is passive
    // housekeeping), skipped for Fullscreen (an additional window, not a fresh connect) and for
    // Sleep (the device is frozen — adb would stall against it). The windowTitle IS the instance
    // name (legacy callers prefixed it with "Remora:" — still accepted).
    QString inst = windowTitle_;
    if (inst.startsWith(QStringLiteral("Remora:"))) inst = inst.mid(int(qstrlen("Remora:")));
    if (ok && mode_ != Fullscreen && mode_ != Sleep && resolve(cfg_, backend_).desktopMenu &&
        !inst.isEmpty())
        emit sigMenuSynced(appMenuSync(sp, cfg_, backend_, inst, *this));
}

AppMenuWorker::AppMenuWorker(Op op, Backend backend, RemoraConfig cfg, QString instance,
                             QObject *parent)
    : QThread(parent), op_(op), backend_(backend), cfg_(std::move(cfg)),
      instance_(std::move(instance)) {}

void AppMenuWorker::line(const QString &, const QString &, const QString &text) {
    emit sigLine(text);
}

void AppMenuWorker::state(const QString &, StepState, const QString &detail, const QString &) {
    if (!detail.isEmpty()) lastDetail_ = detail;
}

void AppMenuWorker::run() {
    RealSpawner sp;
    const bool ok = op_ == Remove ? appMenuRemove(sp, instance_, *this)
                                  : appMenuSync(sp, cfg_, backend_, instance_, *this);
    emit sigDone(ok, lastDetail_);
}

ReadinessWorker::ReadinessWorker(Backend backend, QString sshHost, bool gpuHost, QObject *parent)
    : QThread(parent), backend_(backend), sshHost_(std::move(sshHost)), gpuHost_(gpuHost) {}

void ReadinessWorker::run() {
    RealSpawner sp;
    const HostCapabilities caps = probeCapabilities(backend_, sshHost_, sp);
    const auto results = checkReadiness(backend_, caps, gpuHost_);
    QStringList lines;
    for (const PrereqResult &r : results) {
        QString line = (r.ok ? QStringLiteral("✓ ") : QStringLiteral("✗ ")) + r.label;
        if (!r.ok && !r.remedy.isEmpty()) line += "   → " + r.remedy;
        lines << line;
    }
    emit sigReady(isReady(results), lines);
}

ShotWorker::ShotWorker(Backend backend, RemoraConfig cfg, QString outPath, QObject *parent)
    : QThread(parent), backend_(backend), cfg_(std::move(cfg)), outPath_(std::move(outPath)) {}

void ShotWorker::run() {
    RealSpawner sp;
    const RunContext ctx = makeContext(cfg_, backend_);
    QString err;
    const QString target = resolveAdbTarget(sp, ctx, &err);
    if (target.isEmpty()) {
        emit sigDone(false, err);
        return;
    }
    sp.run({"adb", "connect", target}, {}, {});
    const bool ok = captureScreenshot(sp, target, outPath_, &err);
    emit sigDone(ok, ok ? outPath_ : err);
}

CertificationWorker::CertificationWorker(Backend backend, RemoraConfig cfg, bool recheck,
                                         QString instance, bool restore, QObject *parent)
    : QThread(parent), backend_(backend), cfg_(std::move(cfg)), recheck_(recheck),
      instance_(std::move(instance)), restore_(restore) {}

void CertificationWorker::run() {
    RealSpawner sp;
    const ResolvedConfig rc = resolve(cfg_, backend_);
    const QString host = rc.sshHost.value_or(QString());
    // Restore first, so the state reported afterwards is the state it produced — same order as the
    // CLI's `certify --restore`.
    QString restoreDetail;
    if (restore_) {
        const CheckinRestore r =
            restoreCheckinIdentity(sp, backend_, rc.containerName, host, instance_);
        restoreDetail = (r.ok ? QStringLiteral("restored: ") : QStringLiteral("restore failed: ")) +
                        r.detail;
    }
    // Passing instance_ is what makes this read KEEP the identity it just saw.
    const PlayCertification c = probePlayCertification(sp, backend_, rc.containerName, host,
                                                       recheck_ || restore_, instance_);
    emit sigDone(c.certified.has_value() || c.androidId.has_value(), c.certified.value_or(false),
                 c.androidId.value_or(QString()), checkinSnapshotNote(instance_, c), restoreDetail,
                 c.pendingEvaluation ? pendingEvaluationNote(c.identityAgeSec.value_or(0))
                                     : QString());
}

StopWorker::StopWorker(Backend backend, QString sshHost, QString containerName, QString target,
                       QObject *parent)
    : QThread(parent), backend_(backend), sshHost_(std::move(sshHost)),
      containerName_(std::move(containerName)), target_(std::move(target)) {}

ArmAppInstallWorker::ArmAppInstallWorker(Backend backend, RemoraConfig cfg, QString package,
                                         QObject *parent)
    : QThread(parent), backend_(backend), cfg_(std::move(cfg)), package_(std::move(package)) {}

void ArmAppInstallWorker::run() {
    RealSpawner sp;
    const ResolvedConfig rc = resolve(cfg_, backend_);
    const QString container = rc.containerName;
    const QString script = vendorScript(QStringLiteral("guest-scripts"),
                                        QStringLiteral("remora-arm-fetch.sh"));
    const LineSink onLine = [this](const QString &, const QString &text) { emit sigLine(text); };
    ProcResult r;
    if (backend_ == Backend::Bare) {  // local docker host — run the script here
        r = sp.run({QStringLiteral("bash"), script, container, package_}, {}, onLine);
    } else {  // remote — stage the script onto the docker host, then run it there
        const QString guest = rc.sshHost.value_or(QString());
        if (guest.isEmpty()) {
            emit sigDone(false, QStringLiteral("no ssh host configured for this profile — set "
                                               "it on the Device page"));
            return;
        }
        emit sigLine(QStringLiteral("staging remora-arm-fetch.sh → ") + guest);
        const ProcResult scp =
            sp.run({QStringLiteral("scp"), script, guest + QStringLiteral(":remora-arm-fetch.sh")},
                   {}, onLine);
        if (scp.rc != 0) {
            emit sigDone(false, QStringLiteral("could not copy the fetch script to ") + guest);
            return;
        }
        r = sp.run(Spawner::sshArgv(guest, QStringLiteral("bash ~/remora-arm-fetch.sh %1 %2")
                                               .arg(container, package_)),
                   {}, onLine);
    }
    const bool ok = r.rc == 0;
    const bool sweep = package_ == QLatin1String("--update-all");
    emit sigDone(ok, ok ? (sweep ? QStringLiteral("arm64 app update check finished — see the Log "
                                                  "pane for versions")
                                 : QStringLiteral("installed %1 (arm64)").arg(package_))
                        : (r.stderrTail.trimmed().isEmpty()
                               ? QStringLiteral("ARM install failed — see the Log pane")
                               : r.stderrTail.trimmed()));
}

TransferAppsWorker::TransferAppsWorker(Op op, Backend srcBackend, RemoraConfig srcCfg,
                                       QString srcName, Backend dstBackend, RemoraConfig dstCfg,
                                       QString dstName, QStringList packages, QString dir,
                                       bool dryRun, bool withData, QObject *parent)
    : QThread(parent), op_(op), srcBackend_(srcBackend), dstBackend_(dstBackend),
      srcCfg_(std::move(srcCfg)), dstCfg_(std::move(dstCfg)), srcName_(std::move(srcName)),
      dstName_(std::move(dstName)), packages_(std::move(packages)), dir_(std::move(dir)),
      dryRun_(dryRun), withData_(withData) {}

QString TransferAppsWorker::endpoint(Spawner &sp, Backend backend, const RemoraConfig &cfg,
                                     const QString &name, QString *err) {
    const RunContext ctx = makeContext(cfg, backend);
    QString rerr;
    const QString target = resolveAdbTarget(sp, ctx, &rerr);
    if (target.isEmpty()) {
        if (err) *err = QStringLiteral("'%1': %2").arg(name, rerr);
        return {};
    }
    if (assertAdbEndpoint(sp, target).isEmpty()) {
        if (err)
            *err = QStringLiteral("'%1' (%2) is not answering adb — is the profile running?")
                       .arg(name, target);
        return {};
    }
    return target;
}

void TransferAppsWorker::run() {
    RealSpawner sp;
    const LineSink onLine = [this](const QString &, const QString &text) { emit sigLine(text); };

    // Resolve every endpoint the operation writes to or reads from BEFORE touching anything, and
    // name the side that failed. A dry run needs no writable side at all.
    QString err, src, dst;
    const bool needSrc = op_ != Op::Import;
    const bool needDst = (op_ == Op::Transfer || op_ == Op::Import) && !dryRun_;
    if (needSrc && (src = endpoint(sp, srcBackend_, srcCfg_, srcName_, &err)).isEmpty()) {
        emit sigDone(false, QStringLiteral("source %1").arg(err));
        return;
    }
    if (needDst && (dst = endpoint(sp, dstBackend_, dstCfg_, dstName_, &err)).isEmpty()) {
        emit sigDone(false, QStringLiteral("target %1").arg(err));
        return;
    }

    QStringList pkgs = packages_;
    if (pkgs.isEmpty() && op_ != Op::Import) pkgs = listThirdPartyPackages(sp, src);

    if (op_ == Op::Transfer) {
        if (pkgs.isEmpty()) {
            emit sigDone(true, QStringLiteral("no third-party apps on '%1' — nothing to transfer")
                                   .arg(srcName_));
            return;
        }
        emit sigLine(QStringLiteral("%1 app(s): '%2'  →  '%3'%4")
                         .arg(pkgs.size())
                         .arg(srcName_, dstName_,
                              dryRun_ ? QStringLiteral("   (dry run)") : QString()));
        // Both sides are profiles in the GUI, so both always resolve a container for the data
        // carry's root route (bd remora-4ei.73) — the adb `su` fallback is CLI-only.
        const AppOpCounts c =
            transferApps(sp, src, dst, pkgs, dryRun_, withData_, rootExecFor(srcCfg_, srcBackend_),
                         rootExecFor(dstCfg_, dstBackend_), onLine);
        // Both sides are profiles in the GUI, so the window-mode preferences ride along for the
        // packages that landed (bd remora-4ei.73); the target's own answers win. MainWindow
        // reloads the active profile's modes on sigDone so a later autosave cannot clobber this.
        if (!c.transferred.isEmpty()) {
            RemoraConfig merged = dstCfg_;
            const int carried = carryAppModes(srcCfg_, merged, c.transferred);
            if (carried > 0) {
                saveInstance(defaultRemorarcPath(), dstName_, merged);
                emit sigLine(QStringLiteral("%1 window-mode preference(s) carried over to '%2'")
                                 .arg(carried)
                                 .arg(dstName_));
            }
        }
        QString detail = QStringLiteral("%1 %2, %3 failed, %4 skipped")
                             .arg(c.done)
                             .arg(dryRun_ ? QStringLiteral("would be transferred")
                                          : QStringLiteral("transferred"))
                             .arg(c.failed)
                             .arg(c.skipped);
        if (withData_)
            detail += QStringLiteral("; app data: %1 %2, %3 failed")
                          .arg(c.dataDone)
                          .arg(dryRun_ ? QStringLiteral("would be carried")
                                       : QStringLiteral("carried"))
                          .arg(c.dataFailed);
        else if (!dryRun_ && c.done > 0)
            detail += QStringLiteral(" — app data does not move; apps arrive logged out");
        emit sigDone(c.failed == 0 && c.dataFailed == 0, detail);
        return;
    }
    if (op_ == Op::Export) {
        if (pkgs.isEmpty()) {
            emit sigDone(true, QStringLiteral("no third-party apps on '%1' — nothing to export")
                                   .arg(srcName_));
            return;
        }
        if (!QDir().mkpath(dir_)) {
            emit sigDone(false, QStringLiteral("cannot create %1").arg(dir_));
            return;
        }
        emit sigLine(QStringLiteral("%1 app(s): '%2'  →  %3").arg(pkgs.size()).arg(srcName_, dir_));
        const AppOpCounts c = exportApks(sp, src, pkgs, dir_, onLine);
        emit sigDone(c.failed == 0, QStringLiteral("%1 exported, %2 failed  →  %3")
                                        .arg(c.done)
                                        .arg(c.failed)
                                        .arg(dir_));
        return;
    }
    const QList<ApkJob> jobs = importJobs(dir_);
    if (jobs.isEmpty()) {
        emit sigDone(true, QStringLiteral("no APKs found under %1").arg(dir_));
        return;
    }
    emit sigLine(QStringLiteral("%1 app(s): %2  →  '%3'%4")
                     .arg(jobs.size())
                     .arg(dir_, dstName_, dryRun_ ? QStringLiteral("   (dry run)") : QString()));
    const AppOpCounts c = importApks(sp, dst, jobs, dryRun_, onLine);
    emit sigDone(c.failed == 0,
                 QStringLiteral("%1 %2, %3 failed")
                     .arg(c.done)
                     .arg(dryRun_ ? QStringLiteral("would be installed")
                                  : QStringLiteral("installed"))
                     .arg(c.failed));
}

void StopWorker::run() {
    RealSpawner sp;
    // Stop exactly the profile's own container. The teardown (docker rm -f, locally or over ssh)
    // lives in ExclusionService::stop, which is the same path the conflict takeover uses — one
    // rule, not two copies of it.
    //
    // There is no longer a name-less fallback. It used to force-remove a hardcoded historical
    // name, which is unreachable (both call sites pass resolve().containerName, and the resolver
    // always yields one) and, since exclusion became resource-based, actively wrong: with no name
    // there is nothing we could honestly identify as this profile's container (bd remora-4u4.1).
    if (containerName_.isEmpty()) {
        emit sigDone(false);
        return;
    }
    ExclusionService(sp, sshHost_).stop(backend_, containerName_, target_);
    emit sigDone(true);
}

}  // namespace remora
