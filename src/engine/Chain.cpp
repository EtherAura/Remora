#include "engine/Engine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimeZone>

#include "core/Builders.h"
#include "core/Features.h"
#include "core/Parsers.h"
#include "core/Resolver.h"
#include "core/SourcePatches.h"
#include "engine/SourceBuild.h"  // readVendorPath — the concrete VendorReader over the local tree

#ifndef REMORA_VENDOR_DIR
#define REMORA_VENDOR_DIR "vendor"
#endif
#ifndef REMORA_SOURCE_VENDOR_DIR
#define REMORA_SOURCE_VENDOR_DIR "vendor"
#endif
#ifndef REMORA_INSTALLED_VENDOR_DIR
#define REMORA_INSTALLED_VENDOR_DIR "/usr/share/remora/vendor"
#endif
#ifndef REMORA_VENDOR_IS_INSTALLED
#define REMORA_VENDOR_IS_INSTALLED 0
#endif

namespace remora {

QString stepStateToString(StepState s) {
    switch (s) {
        case StepState::Pending: return QStringLiteral("pending");
        case StepState::Running: return QStringLiteral("running");
        case StepState::Ok: return QStringLiteral("ok");
        case StepState::Failed: return QStringLiteral("failed");
        case StepState::Skipped: return QStringLiteral("skipped");
    }
    return QStringLiteral("pending");
}

bool runChain(const QVector<Step> &steps, LogSink &sink) {
    bool aborted = false;
    for (const Step &step : steps) {
        if (aborted) {
            sink.state(step.name, StepState::Skipped);
            continue;
        }
        sink.state(step.name, StepState::Running);
        StepResult res = step.run(sink);
        sink.state(step.name, res.ok ? StepState::Ok : StepState::Failed, res.detail,
                   res.stderrTail);
        if (!res.ok) aborted = true;
    }
    return !aborted;
}

QString vendorScript(const QString &kind, const QString &name) {
    return QStringLiteral(REMORA_VENDOR_DIR) + "/" + kind + "/" + name;
}

// Raw inputs only — the version extraction is parseNvidiaVersionScan's, tested offline. The ls
// names the three layouts in the wild (lib64: Gentoo/Fedora; lib: Arch and Gentoo's 32-bit;
// x86_64-linux-gnu: Debian/Ubuntu); a miss is harmless, it just reads as "no userspace found"
// and the comparison declines to judge.
const QString kNvidiaVersionScan = QStringLiteral(
    "cat /proc/driver/nvidia/version 2>/dev/null; "
    "ls /usr/lib64/libnvidia-glcore.so.* /usr/lib/libnvidia-glcore.so.* "
    "/usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.* 2>/dev/null");

QVector<VersionAssetRow> auditVersionAssets() {
    // The core registry declares which versions Remora source-builds (a non-empty releaseConfig);
    // this is the filesystem half it cannot do — confirming those versions' build assets are
    // actually on disk, so a version offered in the UI with no manifest / no patch set is caught by
    // `remora check` rather than 40 minutes into a build (bd remora-82c.2). Prebuilt-only versions
    // (no releaseConfig) have no source assets to audit and are skipped.
    const QString sb = QStringLiteral(REMORA_VENDOR_DIR "/source-build");
    QVector<VersionAssetRow> rows;
    for (const AndroidRelease &r : androidReleases()) {
        if (r.releaseConfig.isEmpty()) continue;
        QStringList missing;
        // container-compat patch set directory
        if (r.containerPatchSet.isEmpty()
            || !QDir(sb + QStringLiteral("/container-patches/") + r.containerPatchSet).exists())
            missing << QStringLiteral("container patch set (container-patches/%1)")
                           .arg(r.containerPatchSet.isEmpty() ? QStringLiteral("<none>")
                                                              : r.containerPatchSet);
        // pinned manifest
        if (!QFileInfo::exists(sb + QStringLiteral("/pinned-manifest-a%1.xml").arg(r.version)))
            missing << QStringLiteral("pinned manifest (pinned-manifest-a%1.xml)").arg(r.version);
        // local manifest(s): one versioned pattern for every release (the A16 special case went
        // with the old untagged filename, — see repoBringupCmd, which must agree)
        const QString glob = QStringLiteral("remora-a%1*.xml").arg(r.version);
        if (QDir(sb + QStringLiteral("/local_manifests"))
                .entryList({glob}, QDir::Files)
                .isEmpty())
            missing << QStringLiteral("local manifest (local_manifests/%1)").arg(glob);
        rows << VersionAssetRow{r.version, missing.isEmpty(),
                                missing.isEmpty()
                                    ? QStringLiteral("container patches, pinned + local manifest")
                                    : missing.join(QStringLiteral(", "))};
    }
    return rows;
}

QVector<PatchAssetRow> auditPatchAssetsOnDisk() {
    // Audited per BUILDABLE version, the same set auditVersionAssets walks: the registry scopes
    // some entries to a release, and an override directory present for one version and absent for
    // another is exactly the shape that goes unnoticed. Prebuilt-only versions never run the apply
    // loop, so they have nothing to audit.
    QVector<PatchAssetRow> rows;
    for (const AndroidRelease &r : androidReleases()) {
        if (r.releaseConfig.isEmpty()) continue;
        for (const PatchAssetGap &g : auditPatchAssets(r.version, readVendorPath))
            rows << PatchAssetRow{r.version, g.key, g.dir,
                                  g.missing.join(QStringLiteral(", ")),
                                  g.unknown.join(QStringLiteral(", "))};
    }
    return rows;
}

VendorDriftReport auditVendorTreeDrift() {
    // Which tree is the counterpart depends on which one this binary was compiled to read. The
    // candidates for the installed copy are PROBED, not trusted from configure time: the baked
    // CMAKE_INSTALL_FULL_DATADIR froze the configure-time prefix (/usr/local) while remora-install
    // redirects with --prefix /usr — the exact mismatch that let the install prune test a
    // directory that does not exist for its whole life (bd remora-4ei.33). Probing the short list
    // of real locations answers what a frozen path cannot.
    const QString mineRoot = QStringLiteral(REMORA_VENDOR_DIR);
    QStringList candidates;
#if REMORA_VENDOR_IS_INSTALLED
    const bool stagesFromDrifted = true;  // builds stage from `mine`; drift means the repo's
                                          // edits are not what ships
    candidates << QStringLiteral(REMORA_SOURCE_VENDOR_DIR);
#else
    const bool stagesFromDrifted = false;  // the installed copy is inert to this binary
    candidates << QStringLiteral(REMORA_INSTALLED_VENDOR_DIR)
               << QStringLiteral("/usr/share/remora/vendor")
               << QStringLiteral("/usr/local/share/remora/vendor");
#endif
    VendorDriftReport report;
    report.binaryStagesFromDrifted = stagesFromDrifted;
    for (const QString &c : candidates) {
        if (c == mineRoot) continue;  // one tree is agreement by construction, not a comparison
        if (!QDir(c + QStringLiteral("/source-patches")).exists()) continue;
        report.otherRoot = c;
        break;
    }
    if (report.otherRoot.isEmpty()) return report;  // nothing to compare against — silence
    const auto readAt = [](const QString &root) {
        return [root](const QString &rel) { return readVendorTreeAt(root, rel); };
    };
    report.rows = compareSourcePatchTrees(readAt(mineRoot), readAt(report.otherRoot));
    return report;
}

QString vendorNativeBinary(const QString &name) {
    return QStringLiteral(REMORA_VENDOR_DIR) + "/native/" + name;
}

QString agentDex() {
    // agent/build.sh's deployable output. It lives under vendor/ so it follows the same
    // source-vs-installed tree every other device-side artefact does — but like the native
    // helpers it is BUILT, not committed prose, so its absence is a supported state rather than
    // a broken checkout (bd remora-28ix.3).
    return vendorScript(QStringLiteral("agent"), QStringLiteral("agent.dex"));
}

void applyVendorPaths(ResolvedConfig &rc, Backend backend) {
    // The netfix pair reaches the container as bind mounts, not as a post-boot `docker cp`:
    // Android's init remounts the container rootfs read-only once boot completes, and docker cp
    // has to create a .pivot_root dir at the rootfs ROOT, so it fails for every destination once
    // the container is up (bd remora-4ei.59). Engine-side because the vendor dir is a build-time
    // location and core must stay free of it.
    //
    // REMOTE binds the staging copies under the remote's own ~/.remora instead. The vendor paths
    // name files on THIS machine, and docker answers a -v whose source does not exist by creating
    // an empty DIRECTORY there and mounting that — so a remote container got empty dirs at
    // /remora/* while the real wiring fell to a post-boot docker cp, the exact mechanism 4ei.59
    // proved cannot work (bd remora-4ei.88). The remote deployer's host-prereqs pushes this same
    // set to ~/.remora before provision, and the remote SHELL expands the ~ in the bind source —
    // the run argv goes through `ssh sh -c`, the same contract the "~/remora-data" default
    // already stands on (Resolver.cpp).
    const bool remote = backend == Backend::Remote;
    rc.netfixFwdPath = remote ? QStringLiteral("~/.remora/fwd")
                              : vendorNativeBinary(QStringLiteral("fwd"));
    rc.netfixScriptPath = remote ? QStringLiteral("~/.remora/remora-netfix.sh")
                                 : vendorScript(QStringLiteral("container-scripts"),
                                                QStringLiteral("remora-netfix.sh"));
    // Same road again for the init .rc. It is what makes the forwarder survive a container
    // restart nobody told Remora about: init parses /vendor/etc/init/*.rc on every boot, starts
    // the service, and restarts it when it dies — supervision by PID 1 rather than by a shell
    // loop that whatever kills fwd could kill too (bd remora-yn4).
    rc.netfixInitRcPath = remote ? QStringLiteral("~/.remora/remora-fwd.rc")
                                 : vendorScript(QStringLiteral("container-scripts"),
                                                QStringLiteral("remora-fwd.rc"));
    // The androidboot.remora_* namespace adapter rides the identical road (bd remora-28ix.4):
    // a committed source file with no build step, so no existence gate — its absence IS a broken
    // checkout, and mounting nothing would let the remora_* argv below meet an old image that
    // cannot read it, the black-screen shape the whole migration is ordered around.
    rc.propsInitRcPath = remote ? QStringLiteral("~/.remora/remora.props.rc")
                                : vendorScript(QStringLiteral("container-scripts"),
                                               QStringLiteral("remora.props.rc"));
    // The mirror agent's dex and init service, gated on the dex actually having been built
    // (`agent/build.sh`, which needs the lineage tree). Same existence gate as uinput-kbd and for
    // the same docker reason — a -v whose source is missing becomes an empty DIRECTORY, so an
    // unbuilt agent would mount a directory over /remora/agent.dex and init would fail to start
    // it every 5s for the container's life. Absent is a supported state: an image that bakes the
    // agent needs no mounts, and one that does not gets the client's plain refusal. The rc is
    // gated on the dex rather than on its own existence — it is a committed source file, so it is
    // always there, and starting a service whose class path does not exist is the failure this
    // gate prevents.
    if (QFileInfo::exists(agentDex())) {
        rc.agentDexPath = remote ? QStringLiteral("~/.remora/agent.dex") : agentDex();
        rc.agentInitRcPath = remote ? QStringLiteral("~/.remora/remora-agent.rc")
                                    : vendorScript(QStringLiteral("container-scripts"),
                                                   QStringLiteral("remora-agent.rc"));
    }
    // The phantom keyboard travels the same road. Always mounted, whatever kbd_identity says:
    // "none" is the picker's "Generic — no Gboard toolbar", which still wants a keyboard (a
    // generic one), not the on-screen keyboard back.
    // Only when it has actually been built, though (the Remora build compiles it): docker answers a
    // -v whose SOURCE does not exist by creating a directory there, so an unbuilt helper would
    // both silently do nothing and leave a stray dir in the vendor tree. Absent means the deploy
    // step says so and the on-screen keyboard stays — which is the old behaviour, not a failure.
    // The LOCAL existence gate holds for remote too: host-prereqs stages exactly what exists
    // here, so a helper this machine has not built is absent from ~/.remora as well.
    if (QFileInfo::exists(vendorNativeBinary(QStringLiteral("uinput-kbd"))))
        rc.uinputKbdPath = remote ? QStringLiteral("~/.remora/uinput-kbd")
                                  : vendorNativeBinary(QStringLiteral("uinput-kbd"));
    // Guest timezone: unset means "follow THIS host" — the desktop the mirror lands on, so it is
    // the LOCAL zone even for a remote docker host. Filled here and not in the resolver because
    // the core cannot read the machine; a profile pin passes through untouched (bd remora-c8t).
    if (rc.timezone.isEmpty())
        rc.timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    // /vendor/build.prop overlay: the PATH only. The file itself is generated by the deployer's
    // host-prereqs step, which has a Spawner to ask docker for the image's own copy — the overlay
    // is that copy with our GPU keys replaced, so nothing else in it can go stale. Needed by venus
    // AND by gpu_mode=guest (bd remora-e5x.33: a baked ro.hardware.vulkan=intel is first-wins over
    // gpu_setup_guest's setprop, so guest mode kept the Intel ICD and SurfaceFlinger crash-looped
    // in chooseEglConfig). SINCE bd remora-xqt1 nothing BAKES that key — the native_vulkan pin is
    // retired and the host sends androidboot.remora_vulkan instead — but this overlay stays, and
    // it is the transition mechanism rather than a leftover: a PRE-RETIREMENT image still carries
    // the pin, and a baked value beats the shim's early-init setprop, so only stripping it makes
    // the boot arg effective on those. On a current image the strip finds nothing and the value
    // it re-adds agrees with the arg. Keyed by image tag so switching images cannot silently
    // reuse another image's props, and by MODE so a venus↔guest flip on one tag cannot reuse the
    // other mode's content. A literal `~` for remote — the bind source must exist on the machine
    // running docker, same form and reason as uinputKbdPath/cpufreqDir above and below.
    if (rc.venus || rc.gpuMode == GpuMode::Guest) {
        QString key = rc.imageTag;
        key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
        const QString name = QStringLiteral("%1-build.prop-%2")
                                 .arg(rc.venus ? QStringLiteral("venus") : QStringLiteral("guest"),
                                      key);
        rc.buildPropOverlay =
            (remote ? QStringLiteral("~/.cache/remora")
                    : QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                          QStringLiteral("/remora"))
            + QLatin1Char('/') + name;
    }
    // Where the trimmed cpufreq tree is generated (bd remora-6279). Keyed by CONTAINER, not by
    // image: it describes the HOST's clusters, and two containers on one host share that answer
    // while the same image on two hosts must not. Per-container anyway so a deploy rebuilding one
    // profile's tree can never pull the directory out from under another's running mount.
    //
    // A literal `~` for remote, expanded by the remote shell — the same form uinputKbdPath uses
    // above, and correct for the same reason: the directory has to exist on whichever machine runs
    // docker, and for backend=remote that is not this one.
    if (rc.cpufreqTopology)
        rc.cpufreqDir = (remote ? QStringLiteral("~/.cache/remora")
                                : QStandardPaths::writableLocation(
                                      QStandardPaths::GenericCacheLocation)
                                      + QStringLiteral("/remora"))
                        + QStringLiteral("/cpufreq-%1").arg(rc.containerName);
}

StepResult ensureCpufreqTopology(Spawner &sp, const ResolvedConfig &rc, const QString &sshHost,
                                 const LineSink &log) {
    if (rc.cpufreqDir.isEmpty()) return StepResult::good();
    const QString cmd = buildCpufreqTopologyCmd(rc.cpufreqDir);
    const ProcResult r = sshHost.isEmpty() ? sp.run({"sh", "-c", cmd}, {}, log)
                                           : sp.run(Spawner::sshArgv(sshHost, cmd), {}, log);
    if (r.rc != 0)
        // NOT FATAL. A host with no cpufreq at all (plenty of VMs) would otherwise fail every
        // deploy over a mitigation it does not need — and the fallback is the world before this
        // feature: the guest sees the host's topology, which is what it saw yesterday. Said out
        // loud rather than swallowed, because the profile asked for the trimmed view and a
        // silently untrimmed one is the shape of failure this project keeps re-learning: the
        // capability is gone and the step is green (bd remora-4ei.9's camera wording, same
        // lesson).
        return StepResult::fail(
            QStringLiteral("could not build a phone-like cpufreq view on the docker host — the "
                           "guest will see the host's own policies, which is what it saw before "
                           "this existed; apps with fixed per-cluster tables may crash on a host "
                           "with many policies (bd remora-6279)"),
            r.stderrTail);
    return StepResult::good(QStringLiteral("cpufreq topology at %1").arg(rc.cpufreqDir));
}

// The keys the overlay owns, shared between the venus transform below and the guest-overlay
// script so the two strips cannot drift (bd remora-e5x.33 pins their equality in tests).
const QStringList &overlayOwnedKeys() {
    static const QStringList owned{
        QStringLiteral("ro.hardware.vulkan"),      QStringLiteral("ro.hardware.egl"),
        QStringLiteral("mesa.vn.debug"),           QStringLiteral("mesa.vtest.socket.name"),
        QStringLiteral("debug.hwui.renderer"),     QStringLiteral("debug.renderengine.backend")};
    return owned;
}

QStringList venusBuildPropLines(QStringList lines) {
    // Drop the image's own copies of the keys we own before appending ours — build.prop is
    // first-wins for ro.* props, so leaving a baked ro.hardware.vulkan=intel in place would keep
    // the Intel ICD and the whole Venus path would never be taken. Current images bake no such
    // key (bd remora-xqt1 retired the pin); pre-retirement ones do, which is why this still runs.
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [](const QString &l) {
                                   for (const QString &k : overlayOwnedKeys())
                                       if (l.startsWith(k + QLatin1Char('='))) return true;
                                   return false;
                               }),
                lines.end());
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    lines << QStringLiteral("# Venus/NVIDIA stack — generated by Remora (bd remora-4ei.56)")
          // virtio selects the Venus ICD (vulkan.virtio.so); angle puts GL on top of it.
          << QStringLiteral("ro.hardware.vulkan=virtio")
          << QStringLiteral("ro.hardware.egl=angle")
          // no_abort is REQUIRED, not diagnostic: without it Venus aborts the process on the
          // first unsupported query instead of falling back.
          << QStringLiteral("mesa.vn.debug=vtest,no_abort")
          // Must match the container-side path of the socket-dir bind in buildDockerRunArgv().
          << QStringLiteral("mesa.vtest.socket.name=/dev/venus/venus.sock")
          << QStringLiteral("debug.hwui.renderer=skiavk")
          << QStringLiteral("debug.renderengine.backend=skiaglthreaded") << QString();
    return lines;
}

QString guestBuildPropScript(const QString &path, const QString &imageTag) {
    // One script, two carriages — bare runs it under a local `sh -c`, remote as an ssh payload —
    // so the two backends cannot disagree about what the overlay is. Content: the image's own
    // /vendor/build.prop with the owned GPU keys stripped, plus ONLY the SwiftShader Vulkan ICD.
    // pastel IS SwiftShader (verified live: ANGLE reports 'Vulkan 1.3.0 SwiftShader Device'),
    // and it is exactly what gpu_setup_guest's own setprop tries and, on a PRE-RETIREMENT image,
    // silently loses to the baked ro.hardware.vulkan=intel — so this makes the script's intent
    // effective, not new policy. Current images bake nothing and also receive
    // androidboot.remora_vulkan=pastel from the resolver, which agrees (bd remora-xqt1).
    // ro.hardware.egl deliberately NOT set: gpu_setup_guest picks angle by itself once nothing
    // bakes over it (bd remora-e5x.33). The leading rmdir heals docker's missing-bind-source
    // directory artefact (remote twin of clearDockerCreatedDir; an empty dir can only be that).
    QStringList keys = overlayOwnedKeys();
    return QStringLiteral(
               "P=%1; { [ ! -d $P ] || rmdir $P; } && mkdir -p $(dirname $P) && "
               "id=$(docker create %2) || exit 1; "
               "docker cp $id:/vendor/build.prop $P.tmp; rc=$?; docker rm $id >/dev/null; "
               "[ $rc -eq 0 ] && grep -vE \"^(%3)=\" $P.tmp > $P; rm -f $P.tmp; "
               "[ $rc -eq 0 ] && printf \"%4\" >> $P")
        .arg(path, imageTag, keys.join(QLatin1Char('|')),
             QStringLiteral("# gpu_mode=guest SwiftShader ICD — generated by Remora "
                            "(bd remora-e5x.33)\\nro.hardware.vulkan=pastel\\n"));
}

QString clearDockerCreatedDir(const QString &path) {
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isDir()) return {};
    if (QDir().rmdir(path)) return {};
    return QStringLiteral(
               "%1 exists as a non-empty DIRECTORY where a generated file belongs. An empty one "
               "would be docker's bind-mount artefact and is removed automatically; this one has "
               "content, so inspect and remove it by hand")
        .arg(path);
}

StepResult ensureVenusServer(Spawner &sp, const ResolvedConfig &rc, const LineSink &log) {
    if (!rc.venus) return StepResult::good();
    if (!rc.venusServerPath || !rc.venusSocketDir)
        return StepResult::fail(
            "venus is on but the render server is not installed — run `remora venus-build`, "
            "or set venus=false");
    // Refuse the one host state that makes this whole path unbootable while every piece looks
    // present (bd remora-81cp): a driver update without a reboot. The process/socket health
    // checks below CANNOT see it — the parent server keeps running on the old module, and it is
    // each per-client fork that dies with 'NVRM: API mismatch', so the deploy otherwise produces
    // a container whose SurfaceFlinger crash-loops and an investigation aimed at the image.
    const NvidiaVersions vers =
        parseNvidiaVersionScan(sp.run({"sh", "-c", kNvidiaVersionScan}, {}, log).out);
    if (nvidiaAbiMismatch(vers))
        return StepResult::fail(
            QStringLiteral("NVIDIA kernel module %1 does not match the installed userspace %2 — "
                           "the driver was updated without a reboot, and every client the render "
                           "server forks will die with 'NVRM: API mismatch'. Reboot the host (or "
                           "reload the nvidia module), then deploy again")
                .arg(vers.kernel.value_or(QStringLiteral("?")),
                     vers.userspace.join(QLatin1String(", "))));
    const QString sock = *rc.venusSocketDir + QStringLiteral("/venus.sock");
    // Health is the SOCKET plus a live process, not the process alone. A killed server leaves its
    // socket inode behind, and a container that connects to it gets ECONNREFUSED, fails Vulkan
    // init, and crash-loops SurfaceFlinger against the boot animation — with the stale file making
    // everything look present. Checking both means a half-dead stack is repaired rather than
    // mistaken for a healthy one.
    // `ps -C` matches the process NAME: `pgrep -f virgl_test_server` would also match the shell
    // running the check, so it could never report "not running".
    //
    // MATCHED ON THIS PROFILE'S SOCKET PATH, not merely on the name — the same correction the host
    // encoder needed for the same reason (bd remora-e5x.13). Bare `ps -C` answers "yes" for ANY
    // profile's server, so a second instance saw a server that was not serving it; combined with a
    // leftover socket inode it could also adopt a server on a different path entirely.
    const QString psMine =
        QStringLiteral("ps -C virgl_test_server -o pid=,args= 2>/dev/null | grep -F -- '--socket-path %1'")
            .arg(sock);
    const bool proc = sp.run({"sh", "-c", psMine + " >/dev/null 2>&1"}, {}, log).rc == 0;
    const bool sockOk = sp.run({"sh", "-c", QStringLiteral("[ -S %1 ]").arg(sock)}, {}, log).rc == 0;
    if (proc && sockOk) {
        // NO CAPTURE GUARD HERE ANY MORE (bd remora-emoe). It used to refuse a running server that
        // was not publishing frames for this profile, because frame capture was ONE PER SERVER:
        // REMORA_FRAME_SOCKET is read once at start, so a shared server fed whichever profile
        // started it and a second profile's encoder waited on a socket nothing wrote to — a mirror
        // that stayed black with no error anywhere (bd remora-4u4.5).
        //
        // The render server no longer publishes at all: the hwcomposer does, per container. So
        // "not publishing" is now the normal, correct state for every profile, and the guard could
        // only produce false refusals — it managed exactly one, refusing a healthy profile on the
        // first deploy after the switch with advice (give it its own venus_socket_dir) that would
        // not have helped. Rendering was always genuinely shared (--multi-clients); it was only
        // capture that could not be, and there is no capture here now.
        return StepResult::good(QStringLiteral("venus server already on %1").arg(sock));
    }

    sp.run({"mkdir", "-p", *rc.venusSocketDir}, {}, log);
    sp.run({"rm", "-f", sock}, {}, log);
    // <root>/bin/virgl_test_server → <root>; the render server and libvirglrenderer sit beside it
    // in the same install tree.
    const QString root = QFileInfo(QFileInfo(*rc.venusServerPath).absolutePath()).absolutePath();
    QMap<QString, QString> env{
        // The patch this whole path depends on: serve MAPPABLE buffers as GPU images instead of
        // udmabufs, which NVIDIA's RM cannot dmabuf-export at a non-zero offset.
        {QStringLiteral("VTEST_MAPPABLE_GPU"), QStringLiteral("1")},
        {QStringLiteral("RENDER_SERVER_EXEC_PATH"), root + QStringLiteral("/bin/virgl_render_server")},
        {QStringLiteral("LD_LIBRARY_PATH"), root + QStringLiteral("/lib")}};
    // Frame capture lives INSIDE the render server, so it is enabled here and nowhere else. It is
    // read once per process, so a server already running without it cannot start publishing — the
    // encoder step below reports that rather than waiting forever.
    if (rc.hostEncode) {
        env.insert(QStringLiteral("REMORA_VIDEO_CAPTURE"), QStringLiteral("1"));
        // THE RENDER SERVER NO LONGER PUBLISHES FRAMES, and REMORA_FRAME_SOCKET is deliberately
        // never set (bd remora-emoe). Its publisher is gated on that variable existing, so
        // withholding it unconditionally leaves Remora's own hwcomposer as the only publisher.
        // That is the whole point of owning the HAL: the composer is handed exactly one client
        // target per frame and IS the display, where the interception had to INFER which
        // render-server context was the screen — the heuristic that inference needed (pick_display)
        // is gone, along with the three separate bugs its comments recorded.
        //
        // Measured before switching (bd remora-emoe): the composer encodes ~1.0 frames per frame
        // the device composed, the interception ~1.6 — it published every composited surface, so
        // ~40% of its encoding was redundant and, at a fixed bitrate, spent on duplicates. Roughly
        // a third less host CPU for the same picture.
        //
        // Running both was tried and cannot work: pick_display kept its incumbent on ties, the two
        // advertised identical extent, and the composer publishes once per composed frame so it
        // never out-delivered the incumbent it would have had to displace.
        //
        // The interception PATCHES still ship (vendor/host-prereqs/venus-nvidia/) because the
        // rendering fixes there share a helper header with them; they are inert without this
        // variable. See bd remora-emoe.4.
        // WHICH display to publish. The server labels it by extent — largest target wins, and after
        // 240 submits with nothing at that size it follows whatever else is rendering. Both rules
        // assume ONE display, and an app window (--new-display) is a second: it renders
        // continuously while the main display sits on a still launcher, so the label moves to the
        // app and the MIRROR shows the app, measured as "relabelled 3760x1992 -> 736x1600". No
        // extent heuristic can separate that from a genuine resize — but the host does not have to
        // guess. The size is baked into the container at `docker run` (androidboot.remora_width/
        // height are first-write-wins ro props) and cannot change while it runs (bd remora-ljm).
        env.insert(QStringLiteral("REMORA_DISPLAY_SIZE"),
                   QStringLiteral("%1x%2").arg(rc.width).arg(rc.height));
    }
    // spawnDetached puts it in its own transient scope, which matters here for the same reason it
    // does for the mirror: a GUI launched from the desktop would otherwise leave the server in the app
    // unit's cgroup, and that unit's KillMode would reap it on teardown — taking the running
    // container's GPU with it (bd remora-4ei.29).
    if (sp.spawnDetached({*rc.venusServerPath, "--venus", "--multi-clients", "--socket-path", sock},
                         env)
            .pid == 0)
        return StepResult::fail("venus: could not start the render server");
    // The socket appears a moment after the process does; the container's Vulkan fails hard if it
    // connects first, so wait for the socket rather than for the pid.
    for (int i = 0; i < 40; ++i) {
        if (sp.run({"sh", "-c", QStringLiteral("[ -S %1 ]").arg(sock)}, {}, log).rc == 0) {
            // The server binds the socket with no mode of its own, so it lands at 0777 & ~umask —
            // it inherits the umask of WHOEVER STARTED REMORA. At the common 022 that is
            // srwxr-xr-x, and connect(2) on a unix socket needs write: only uid 1000 can reach it.
            // Inside the container SurfaceFlinger (system, uid 1000) then connects fine while every
            // app UID gets EPERM, so the guest half-works and each hardware-accelerated app dies on
            // "venus vtest: failed to connect: Permission denied" -> "Assertion failed: !gpuCount".
            // Nothing in that chain names a permission, and it only appears when the server is
            // (re)started from a shell whose umask differs from the last one's (bd remora-7qs).
            // The socket lives in this user's own cache dir, which is what actually guards it.
            sp.run({"chmod", "0777", sock}, {}, log);
            return StepResult::good(QStringLiteral("venus server started on %1").arg(sock));
        }
        sp.waitMs(250);
    }
    return StepResult::fail(
        QStringLiteral("venus: render server started but %1 never appeared").arg(sock));
}

QString imageBuildFingerprint(const QStringList &enabledPatches, const QStringList &features,
                              int androidVersion, const QString &sourceRef) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    QStringList f = features, p = enabledPatches;
    f.sort();
    p.sort();
    h.addData(QStringLiteral("%1|%2|%3|%4")
                  .arg(f.join(QLatin1Char(',')), p.join(QLatin1Char(',')),
                       QString::number(androidVersion), sourceRef)
                  .toUtf8());
    // enabled patch files, content-hashed in registry (= apply) order
    for (const SourcePatch &sp : sourcePatches()) {
        if (!p.contains(sp.key)) continue;
        for (const QString &name : sp.patches) {
            QFile file(QStringLiteral(REMORA_VENDOR_DIR "/source-patches/") + sp.dir +
                       QLatin1Char('/') + name);
            h.addData(name.toUtf8());
            if (file.open(QIODevice::ReadOnly)) h.addData(&file);
        }
    }
    // The source-build tree carries the assemble pipeline + feature payloads (~470 MB of blobs).
    // Hash ALL of it by content. This used to shortcut files over 1 MiB to path+size, reasoning
    // that a payload swap changes its size in practice — but that exception is exactly the shape
    // of a binary patch: a same-size edit of a vendored payload moved nothing, the build reported
    // "up-to-date", and the image kept the old bits baked (bd remora-82c.11 — proven with a
    // one-byte flip in features/magisk/magisk/busybox). Content is the only honest input.
    // The cost guard is a CACHE, not a cutoff: a full read measures ~0.3 s warm — too slow for
    // every GUI page rebuild, which is where this gets called — so the tree digest is recomputed
    // only when the sorted (path, size, mtime) stat vector changes. mtimes gate the CACHE and
    // never enter the fingerprint, so git/sync churning them costs one re-read and moves nothing
    // (the reason they were excluded before, kept).
    const QString root = QStringLiteral(REMORA_VENDOR_DIR "/source-build");
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    QStringList paths;
    while (it.hasNext()) paths << it.next();
    paths.sort();
    QByteArray statVec;
    for (const QString &fp : paths) {
        const QFileInfo fi(fp);
        statVec += fp.mid(root.size()).toUtf8() + '|' + QByteArray::number(fi.size()) + '|' +
                   QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + '\n';
    }
    static QMutex cacheMutex;
    static QByteArray cachedStatVec;
    static QByteArray cachedTreeDigest;
    QMutexLocker lock(&cacheMutex);
    if (statVec != cachedStatVec || cachedTreeDigest.isEmpty()) {
        QCryptographicHash th(QCryptographicHash::Sha256);
        for (const QString &fp : paths) {
            th.addData(fp.mid(root.size()).toUtf8());
            QFile file(fp);
            if (file.open(QIODevice::ReadOnly)) th.addData(&file);
        }
        cachedStatVec = statVec;
        cachedTreeDigest = th.result();
    }
    h.addData(cachedTreeDigest);
    return QString::fromLatin1(h.result().toHex().left(16));
}

RunContext makeContext(const RemoraConfig &cfg, Backend backend,
                       const std::optional<HostCapabilities> &caps, const QString &windowTitle,
                       bool fullscreenDisplay) {
    RunContext ctx;
    ctx.backend = backend;
    ctx.rc = resolve(cfg, backend, caps);
    applyVendorPaths(ctx.rc, backend);
    ctx.dockerRunArgv = buildDockerRunArgv(ctx.rc);
    auto [env, mirror] = buildMirrorArgv(ctx.rc, ctx.rc.target, fullscreenDisplay);
    ctx.mirrorEnv = env;
    ctx.mirrorArgv = mirror;
    // The builder emits the contract argv[0] "remora"; run the binary this engine IS, so a dev
    // build launches itself rather than whatever PATH resolves first.
    ctx.mirrorArgv[0] = QCoreApplication::applicationFilePath();
    if (!windowTitle.isEmpty())
        ctx.mirrorArgv << "--window-title" << windowTitle;
    // One-window boot splash: the MIRROR argv itself plus the splash flags. The window opens the
    // moment the deploy starts (status text from statusPath), plays the boot animation while
    // Android boots, then the mirroring session adopts the same window. Shaped like the device
    // (--boot-animation-size aspect-fits the resolution into the desktop) so the animation is
    // centered in the mirror's own dimensions from the first frame.
    // The per-target zip cache is what makes the animation start instantly: a warm the guest boot
    // is over in ~30s but adbd only answers ~20s in, so pulling at boot time leaves ~1s of
    // animation. Cached, playback starts at restart time; the cache refreshes after each boot.
    QString cacheKey = ctx.rc.target;
    cacheKey.replace(QLatin1Char(':'), QLatin1Char('-')).replace(QLatin1Char('/'), QLatin1Char('-'));
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                             + QStringLiteral("/remora");
    ctx.statusPath = QStringLiteral("%1/splash-status-%2.log").arg(cacheDir, cacheKey);
    ctx.splashArgv = ctx.mirrorArgv;
    ctx.splashArgv << QStringLiteral("--boot-animation")
                   << QStringLiteral("--boot-animation-cache=%1/bootanim-%2.zip")
                          .arg(cacheDir, cacheKey)
                   << QStringLiteral("--boot-animation-status=%1").arg(ctx.statusPath)
                   << QStringLiteral("--boot-animation-size=%1x%2")
                          .arg(ctx.rc.width)
                          .arg(ctx.rc.height);
    // Empty when unconfigured — the remote preflight rejects that by name rather than
    // substituting somebody else's host (bd remora-4ei.12).
    ctx.guest = ctx.rc.sshHost.value_or(QString());
    ctx.retiredVmBackend = cfg.backend.retiredVmBackend;
    ctx.unrecognisedGpuMode = cfg.gpu.unrecognisedMode;
    ctx.windowTitle = windowTitle;
    ctx.fullscreenDisplay = fullscreenDisplay;
    return ctx;
}

// Re-run the container's netfix script. The runtime net fixes (routes, rp_filter, the IPv4->IPv6
// adb `fwd` shim) applied at provision are lost when the container or Android restarts; `up`
// reapplies them in its netfix step, but `reconnect` goes straight to connect — so a dead fwd left
// adb retrying forever ("not connected" while every readiness probe passes). The scripts persist
// inside the container (/remora mount on bare, /data volume on remote) and are idempotent, so
// re-running on a failed adb attempt is cheap and safe.
static void reapplyNetfix(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    const QString c = ctx.rc.containerName;
    // /remora on BOTH backends, not /data/local/tmp — outside /data so the use_overlayfs init
    // overlay cannot shadow the bind (bd remora-4u4.3). Remote used to exec its docker-cp'd copy
    // under /data/local/tmp; the cp mechanism cannot work post-boot (bd remora-4ei.59) and the
    // remote argv now binds the staged ~/.remora pair at /remora like bare (bd remora-4ei.88).
    switch (ctx.backend) {
        case Backend::Bare:
            sp.run({"docker", "exec", c, "sh", "-c", "sh /remora/netfix.sh"}, {}, log);
            break;
        case Backend::Remote:
            sp.run(Spawner::sshArgv(ctx.guest,
                                    QStringLiteral("docker exec %1 sh /remora/netfix.sh").arg(c)),
                   {}, log);
            break;
    }
}

// Restart the codec2 VAAPI HAL in the container. Its VaapiDevice self-disables after a VF failure
// and stays disabled for the service's lifetime, so every later encode fails "No VAAPI device"
// even once the VF recovers. Killing it makes init respawn it, forcing a fresh VAAPI probe. This
// is the real recovery for the recurring encoder wedge (killing the mirror server does not
// release the HAL's VA state).
static void restartC2Hal(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    const QString c = ctx.rc.containerName;
    const char *inner = "pkill -f '[c]2@1.2-service-vaapi' 2>/dev/null; true";
    if (ctx.backend == Backend::Bare)
        sp.run({"docker", "exec", c, "sh", "-c", inner}, {}, log);
    else
        sp.run(Spawner::sshArgv(
                   ctx.guest,
                   QStringLiteral("docker exec %1 sh -c \"%2\"").arg(c, QLatin1String(inner))),
               {}, log);
    sp.waitMs(3000);  // let init respawn the service + re-probe the VF
}

// Wrap a script for a shell that will re-parse it (ssh's remote login shell). The blunt reap this
// file used to send carried no '$' and no '"', so interpolating it into "docker exec … sh -c \"%1\""
// happened to survive; the scoped one below is full of both and would be shredded.
static QString shSingleQuote(const QString &s) {
    return QLatin1Char('\'') + QString(s).replace(QLatin1String("'"), QLatin1String("'\\''"))
           + QLatin1Char('\'');
}

// THERE IS NO GUEST-SIDE SERVER REAP. The device half is the in-image agent, a long-running INIT
// SERVICE — `service remora_agent`, class late_start, started once on sys.boot_completed
// (features/remora_agent/remora-agent.rc). One process for the container's lifetime, not one per
// session, so there is nothing per-session to leak and nothing to reap; an image without the
// agent is refused outright. Killing it would disconnect every live session, PIP included.
//
// WHAT REAPS: reapSessionOrphans below, which is the HOST side — this profile's mirror clients
// and frame encoder that outlived their guest session.

void appendStatusLine(const QString &statusPath, const QString &text) {
    if (statusPath.isEmpty()) return;
    QDir().mkpath(QFileInfo(statusPath).dir().path());
    QFile f(statusPath);
    if (f.open(QIODevice::Append)) f.write((text + QLatin1Char('\n')).toUtf8());
}

// Alt+D toggles mirror ↔ desktop. Each mirror carries BOTH launch commands as env; the client
// launches the OTHER one and closes itself. REMORA_IS_DESKTOP marks which side a process is:
// the desktop launcher sets it, the mirror launcher clears it (env -u), and both commands
// background mirror so it survives the switch. Built with the resolved target (macvlan-safe).
QMap<QString, QString> mirrorSpawnEnv(const RunContext &ctx, const QString &target,
                                      const QStringList &mirrorArgv) {
    QMap<QString, QString> spawnEnv = ctx.mirrorEnv;
    // An APP window is not one side of this toggle, and handing it the toggle wired it to ITSELF:
    // mirrorArgv is that window's own argv, so a popup carried REMORA_MIRROR_CMD = "relaunch this
    // popup" with REMORA_IS_DESKTOP unset (i.e. "I am the mirror"), and Alt+D there spawned a
    // duplicate and closed the original. There is no sensible other side for one app's window, so
    // it gets no toggle at all and Alt+D says "not configured".
    const bool desktop = ctx.desktopWindow();
    if (ctx.extraWindow() && !desktop) return spawnEnv;
    auto shquote = [](const QStringList &argv) {
        QStringList q;
        for (const QString &a : argv)
            q << QLatin1Char('\'')
                     + QString(a).replace(QLatin1String("'"), QLatin1String("'\\''"))
                     + QLatin1Char('\'');
        return q.join(QLatin1Char(' '));
    };
    // desktop launch args (the --new-display session), with a distinct window title
    ResolvedConfig drc = ctx.rc;
    drc.fullscreenSeparateDisplay = true;
    QStringList dargv = buildMirrorArgv(drc, target, /*fullscreenDisplay=*/true).second;
    dargv[0] = QCoreApplication::applicationFilePath();
    QString inst = ctx.windowTitle;
    if (inst.startsWith(QLatin1String("Remora:"))) inst = inst.mid(7);
    if (inst.startsWith(kDesktopTitlePrefix)) inst = inst.mid(kDesktopTitlePrefix.size());
    if (inst.isEmpty()) inst = QStringLiteral("device");
    dargv << QStringLiteral("--window-title") << (kDesktopTitlePrefix + inst);
    // mirror launch args = exactly what the caller spawns (title appended, target resolved).
    // NOTE: deliberately the plain mirror argv, never the splash one — an Alt+D round-trip must
    // not re-run a boot splash against an already-booted device.
    //
    // …except when THIS window is the desktop side, where the caller's argv IS the desktop and
    // would send Alt+D straight back to another desktop. `remora desktop` starts on that side
    // without ever having been the mirror, so there is no mirror argv to have been handed; build
    // the profile's plain one. (A desktop window that Alt+D spawned from a mirror does not come
    // through here at all — it inherits the real mirror's command through the shell.)
    QStringList margv = mirrorArgv;
    if (desktop) {
        ResolvedConfig mrc = ctx.rc;
        mrc.fullscreenSeparateDisplay = false;
        margv = buildMirrorArgv(mrc, target, /*fullscreenDisplay=*/false).second;
        margv[0] = QCoreApplication::applicationFilePath();
        margv << QStringLiteral("--window-title") << inst;
        // This process is the desktop side, so Alt+D must read REMORA_MIRROR_CMD, not _DESKTOP_.
        spawnEnv.insert(QStringLiteral("REMORA_IS_DESKTOP"), QStringLiteral("1"));
    }
    const QString mirrorCmd =
        QStringLiteral("env -u REMORA_IS_DESKTOP %1 &").arg(shquote(margv));
    const QString desktopCmd = QStringLiteral("REMORA_IS_DESKTOP=1 %1 &").arg(shquote(dargv));
    spawnEnv.insert(QStringLiteral("REMORA_MIRROR_CMD"), mirrorCmd);
    spawnEnv.insert(QStringLiteral("REMORA_DESKTOP_CMD"), desktopCmd);
    // Default Alt+D switches (closes the current window); the checkbox keeps it (extra window).
    if (drc.desktopNewWindow)
        spawnEnv.insert(QStringLiteral("REMORA_DESKTOP_NEW_WINDOW"), QStringLiteral("1"));
    return spawnEnv;
}

// Wait out the first-boot module work before the splash is allowed to become the mirror.
//
// play-spoof deliberately BACKGROUNDS each module's service.sh (setsid ... &): they block on
// sys.boot_completed and only then do their real work — PIF's runs the whole sensitive-property
// pass (resetprop --compact over lockstate/vbmeta/verifiedbootstate/...). So the step reports ok
// while that work is still pending, it lands seconds later, and it takes the framework and adb
// down with it. If the splash has already handed off by then, the mirror sees "Device disconnected"
// and EXITS — the window drops and doConnect spawns a replacement. That is the visible
// drop-then-reappear on every first connect (bd remora-82x; splash log showed animation →
// hand-off → live demuxer → Device disconnected).
//
// Holding the hand-off is the cheap half of the fix: before it, the splash is only running adb
// probes, so a disconnect there costs nothing — it keeps animating and hands off once, cleanly.
// Fail-open in every direction: a device that cannot be asked, or work that outlasts the budget,
// must delay the mirror rather than block it.
//
// The probe deliberately spells the pattern 'service\.sh'. `pgrep -f` matches whole command lines,
// and the container-side `sh -c` running THIS probe carries the pattern in its own — the backslash
// makes the regex demand a literal '.' where the probe's own argv has a backslash, so it cannot
// match itself (killStaleServer's [c]ommand trick, in the form pgrep needs). Verified live, both
// directions, plus the case that matters: a module's late `{ … }&` block keeps its parent's argv,
// so it stays matched until the work is genuinely finished rather than until the wrapper exits.
//
// Returns a note for the connect step's DETAIL, empty when nothing had to be waited for. Whether
// this gate fired is the first question the next cold boot has to answer, and the detail is the
// only channel the CLI and the desktop launcher can see (ConsoleSink overrides state() and drops
// line()) — the same blind spot that made bd remora-eoy.10 take days to pin down.
static QString waitFirstBootSettled(Spawner &sp, const RunContext &ctx, const QString &target,
                                    const LineSink &log) {
    constexpr int kPollMs = 1000;
    constexpr int kBudgetMs = 90000;
    constexpr int kSettleBudgetMs = 30000;
    constexpr int kSettleHoldMs = 3000;
    const QString probe =
        QStringLiteral("pgrep -f 'service\\.sh' >/dev/null 2>&1 && echo busy || echo idle");
    const auto modulesBusy = [&]() {
        const ProcResult r =
            ctx.backend == Backend::Bare
                ? sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", probe}, {}, log)
                : sp.run(Spawner::sshArgv(ctx.guest,
                                          QStringLiteral("docker exec %1 sh -c \"%2\"")
                                              .arg(ctx.rc.containerName, probe)),
                         {}, log);
        // Anything other than a clear "busy" ends the wait: an unreachable container, a missing
        // pgrep or a mid-restart adb must not hold the mirror hostage.
        return r.out.contains(QLatin1String("busy"));
    };
    int held = 0;
    bool timedOut = false;
    while (modulesBusy()) {
        if (held == 0)
            log(QStringLiteral("stdout"),
                QStringLiteral("first-boot module setup still running — holding the mirror "
                               "hand-off so the window is not dropped by its restart"));
        if (held >= kBudgetMs) {
            timedOut = true;
            log(QStringLiteral("stdout"),
                QStringLiteral("first-boot module setup outlasted its budget — attaching anyway"));
            break;
        }
        sp.waitMs(kPollMs);
        held += kPollMs;
    }
    // Nothing was in flight, so nothing has invalidated the adb link the connect loop and the
    // boot-stability gate already established. Cost of the healthy path: one probe.
    if (held == 0) return QString();

    // Work WAS in flight, so that link is no longer a current fact — the property pass is exactly
    // what takes adbd (and the framework) down with it. The marker the caller writes next means
    // "adb is verified and the mirror is about to attach"; that is what the mirror's attach gate
    // trusts, and handing off into the tail of the outage is the same drop by a later name (the
    // mirror's whole log then reads "adb: error: device offline"). So re-establish it here, and
    // insist the device stay answerable for a moment rather than catch the eye of the outage.
    // Bounded and fail-open, like everything above it.
    int stable = 0;
    for (int waited = 0; waited < kSettleBudgetMs; waited += kPollMs) {
        const bool up =
            sp.run({"adb", "-s", target, "get-state"}, {}, log).out.trimmed()
                == QLatin1String("device")
            && sp.run({"adb", "-s", target, "shell", "getprop", "sys.boot_completed"}, {}, log)
                       .out.trimmed()
                   == QLatin1String("1");
        if (up) {
            stable += kPollMs;
            if (stable >= kSettleHoldMs) break;
        } else {
            stable = 0;
            sp.run({"adb", "connect", target}, {}, log);  // the outage drops the link
        }
        sp.waitMs(kPollMs);
    }
    return QStringLiteral("first-boot modules held the hand-off %1s%2")
        .arg(held / 1000)
        .arg(timedOut                ? QStringLiteral(" (budget)")
             : stable < kSettleHoldMs ? QStringLiteral(", adb never re-settled")
                                      : QString());
}

// One command inside the container, whichever backend owns it.
static ProcResult containerExec(Spawner &sp, const RunContext &ctx, const QString &inner,
                                const LineSink &log) {
    if (ctx.backend == Backend::Bare)
        return sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", inner}, {}, log);
    return sp.run(Spawner::sshArgv(ctx.guest, QStringLiteral("docker exec %1 sh -c %2")
                                                  .arg(ctx.rc.containerName, shSingleQuote(inner))),
                  {}, log);
}

// Has THIS /data ever carried a boot through to a working mirror?
//
// The boot-stability hold exists for one situation: a genuinely fresh /data reaches
// sys.boot_completed and then reboots itself seconds later (factory_reset -> ota), and a mirror
// attached to that doomed boot dies with it (bd remora-4ei, the "first-launch mirror-close"). That
// is a FIRST-BOOT property, not a per-deploy one — but the hold was unconditional, so every deploy
// for the life of the data dir paid for a hazard that can only occur while it is new.
//
// The marker is written only after a connect has fully succeeded, so its presence means this /data
// has already survived the window the hold defends. A factory reset removes it along with
// everything else, which is exactly right: the hazard returns and so does the hold.
static const char *const kBootStableMarker = "/data/adb/.remora-boot-stable";

static bool dataHasBootedBefore(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    return containerExec(sp, ctx, QStringLiteral("test -f %1").arg(kBootStableMarker), log).rc == 0;
}

// TERM every pid, give it a moment, then KILL whatever ignored the TERM. Shared by both reaps —
// the one that clears a rejected attempt and the one that enforces "exactly one mirror" after a
// good attempt — because a mirror worth reaping is usually one that is stuck, and a stuck client
// does not service signals it has to wake up to handle.
static void killMirrorPids(Spawner &sp, const QStringList &pids, const LineSink &log) {
    if (pids.isEmpty()) return;
    const QString list = pids.join(QLatin1Char(' '));
    sp.run({"sh", "-c", QStringLiteral("kill %1 2>/dev/null; true").arg(list)}, {}, log);
    sp.waitMs(1500);
    sp.run({"sh", "-c",
            QStringLiteral("for p in %1; do kill -0 $p 2>/dev/null && kill -9 $p 2>/dev/null; "
                           "done; true").arg(list)},
           {}, log);
}

// A mirror that has OUTLIVED ITS SERVER is not a mirror any more, and nothing used to reap it.
//
// The guest server can be killed out from under a LIVE client: killStaleServer falls back to a
// blunt container-wide pkill whenever its scid ownership check comes up empty, and that check
// reads the scid from a single host-side `adb -s <target> shell …` line — the only one of the
// three matching processes that carries the target at all. What survives that is a husk: the
// window is still open, the process is still alive, it is still an ESTAB consumer on the host
// encoder's video socket, and nothing is composing behind it. It never recovers and never exits.
//
// Left alone the encoder goes on fanning every packet at a client that cannot use them, bounded
// only by a 500 ms send timeout that a slowly-draining husk never actually trips — so it is not
// even reaped as broken, it just intermittently stalls the fan-out to the window someone IS
// looking at. That is judder with nothing to point at, and until now only destroying the
// container cleared it (bd remora-8d7, reproduced live).
//
// The discriminator is the TUNNEL, not the window: the mirror listens on its own port for the adb
// forward, so a live session owns TCP connections and a husk owns none. Measured on the pair that
// started this — the healthy client held two ESTAB on 127.0.0.1:27183, the husk held zero.
static QStringList findMirrorHusks(Spawner &sp, const QString &target, const QString &windowTitle,
                                   const LineSink &log) {
    const QString pat = mirrorTitlePattern(target, windowTitle);
    if (pat.isEmpty() || pat.contains(QLatin1Char('\''))) return {};
    // A mirror that has only just been spawned has no tunnel YET, and would answer this test
    // exactly like a husk. The age floor is what makes the difference; it is generous because
    // the cost of waiting is nothing and the cost of being wrong is killing a starting mirror.
    constexpr int kMinAgeSec = 30;
    // `; true` on the loop, deliberately: the body ends in a test that FAILS for every healthy
    // mirror, and a loop exits with the status of its last command — so without this the scan
    // returns non-zero exactly when it found nothing wrong.
    const QString script =
        QStringLiteral("command -v ss >/dev/null 2>&1 || exit 0; "
                       "for p in $(pgrep -f '%1' 2>/dev/null); do "
                       "age=$(ps -o etimes= -p $p 2>/dev/null | tr -d ' '); "
                       "[ -n \"$age\" ] || continue; "
                       "[ \"$age\" -ge %2 ] || continue; "
                       "n=$(ss -tnHp 2>/dev/null | grep -c \"pid=$p,\"); "
                       "[ \"$n\" = 0 ] && echo $p; "
                       "done; true")
            .arg(pat)
            .arg(kMinAgeSec);
    // No `ss` exits 0 with no output, and that is the point: absent evidence must never read as
    // "it is dead". Killing a healthy mirror because a diagnostic tool is missing would be a far
    // worse bug than the leak this closes.
    const ProcResult r = sp.run({QStringLiteral("sh"), QStringLiteral("-c"), script}, {}, log);
    if (r.rc != 0) return {};
    QStringList pids;
    static const QRegularExpression kPid(QStringLiteral("^[0-9]+$"));
    for (const QString &l : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        if (kPid.match(l.trimmed()).hasMatch()) pids << l.trimmed();
    return pids;
}

int reapMirrorHusks(Spawner &sp, const RunContext &ctx, const QString &target,
                    const LineSink &log) {
    // A --new-display app or desktop window is a session in its own right, not a mirror, and the
    // title pattern already excludes it; skipping here as well keeps that guarantee explicit
    // rather than inherited.
    if (ctx.fullscreenDisplay || target.isEmpty()) return 0;
    const QStringList husks = findMirrorHusks(sp, target, ctx.windowTitle, log);
    if (husks.isEmpty()) return 0;
    // Guarded: a LineSink is a std::function and several callers legitimately pass an empty one
    // (startSplash does), where calling it throws rather than doing nothing.
    if (log)
        log(QStringLiteral("stderr"),
            QStringLiteral("reaping %1 mirror(s) that outlived their guest server — still holding "
                           "the encoder's video socket with nothing composing behind them (pid %2)")
                .arg(husks.size())
                .arg(husks.join(QLatin1Char(' '))));
    killMirrorPids(sp, husks, log);
    return int(husks.size());
}

// Reap a mirror attempt that was rejected, and make sure it is actually gone.
//
// Two traps here, both proven live (bd remora-82x, and the flickering of bd remora-e5x.19 that it
// turned out to cause):
//
//  · `det.pid` is spawnDetached's pid — the pid of cmd[0] — and scopedArgv wraps the mirror in
//    systemd-run, so it is NOT reliably the mirror's. Killing it can leave the real process running.
//    Match the mirror's own argv instead, which is what actually identifies it.
//  · A rejected mirror is by definition a STUCK one — starved of video, blocked in its demuxer —
//    and that is precisely the state in which SIGTERM is not serviced. Observed: alive in
//    futex_wait 84 SECONDS after a TERM, gone instantly on a KILL.
//
// Left behind, the survivor is a SECOND client on the one host encoder, both pulling from it, and
// the user sees heavy black flickering — with nothing in the chain admitting a second window
// exists, because the connect step reported the pid of the replacement. TERM first (a graceful exit
// releases the guest VirtualDisplay), then KILL whatever ignored it: an ungraceful exit costs less
// than a second client does.
//
// `spare` is the set of pids that must survive — the mirrors that predate this connect. It is empty
// when the attempt being reaped IS one of those predecessors (an adopted splash), because then the
// thing to kill is exactly the thing the list would otherwise protect.
static void reapMirrorAttempt(Spawner &sp, const RunContext &ctx, const QString &target,
                              const QStringList &spare, const Detached &det, const LineSink &log) {
    QStringList victims;
    const QString pat = mirrorTitlePattern(target, ctx.windowTitle);
    if (!pat.contains(QLatin1Char('\''))) {
        const QStringList live =
            sp.run({"sh", "-c", QStringLiteral("pgrep -f '%1' || true").arg(pat)}, {}, log)
                .out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &p : live) {
            const QString t = p.trimmed();
            if (t.isEmpty()) continue;
            bool spared = false;
            for (const QString &s : spare)
                if (s.trimmed() == t) { spared = true; break; }
            if (!spared) victims << t;
        }
    }
    const QString own = QString::number(det.pid);
    if (det.pid > 0 && !victims.contains(own)) victims << own;
    killMirrorPids(sp, victims, log);
}

// Reap the host-side session of a container that is being TORN DOWN — the reap doConnect cannot
// provide, because after a `docker rm -f` there is no connect coming.
//
// The mirror is detached into its own transient scope precisely so Remora's exit cannot kill it,
// which also means nothing kills it when its container dies: observed live surviving a full
// destroy (docker kill -> stop -> die -> destroy) together with its frame encoder, both pointed
// at a device that no longer existed. Two clients later contended over the one encoder and the
// user saw heavy flickering; each orphan also held ~0.5 GiB of VRAM, and the encoder's socket
// inode outlived it, ready to make the next generation's adoption checks look healthy
// (bd remora-e5x.19.1).
//
// Unlike doConnect's reaps there is nothing to spare here: every mirror on this target — main window,
// splash, PIP, app and desktop windows alike — serves the device being destroyed. An empty
// target is a macvlan profile whose IP was never pinned (the serial is discovered at connect) or
// a takeover of a container whose profile this process cannot resolve; those clients are left to
// the next connect's husk reaper, but the encoder and its sockets are still cleaned, because
// both derive from the container NAME alone.
int reapSessionOrphans(Spawner &sp, const QString &containerName, const QString &target,
                       const LineSink &log) {
    static const QRegularExpression kPid(QStringLiteral("^[0-9]+$"));
    // Only bare pids reach the kill line — this output becomes a command line, and a stray word
    // there is an arbitrary-argument hole, not a cosmetic problem (same guard as the husk scan).
    const auto pidsOf = [&](const ProcResult &r) {
        QStringList pids;
        for (const QString &l : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (kPid.match(l.trimmed()).hasMatch()) pids << l.trimmed();
        return pids;
    };
    QStringList victims;
    if (!target.isEmpty() && !target.contains(QLatin1Char('\''))) {
        // `( |$)` closes the serial: without it a target of 10.0.0.7 would also match a
        // 10.0.0.78 client — a DIFFERENT container's live mirror reaped on a prefix.
        const QString pat = QStringLiteral("remora mirror.*-s %1( |$)")
                                .arg(QRegularExpression::escape(target));
        victims << pidsOf(
            sp.run({"sh", "-c", QStringLiteral("pgrep -f '%1' || true").arg(pat)}, {}, log));
    }
    // The encoder, matched on this container's frame socket exactly the way ensureHostEncoder
    // adopts one — grep -F because the path is a literal, not a pattern.
    const QString frameSock = hostEncodeFrameSocketFor(containerName);
    victims << pidsOf(sp.run(
        {"sh", "-c",
         QStringLiteral("ps -C remora-frame-encoder -o pid=,args= 2>/dev/null | "
                        "grep -F -- '%1' | awk '{print $1}'")
             .arg(frameSock)},
        {}, log));
    killMirrorPids(sp, victims, log);
    // The socket inodes outlive the encoder, and a leftover inode is what made a dead stack look
    // present to the adoption checks (bd remora-e5x.19) — the next generation starts from nothing.
    // The status file rides along: the encoder removes it itself on a clean exit, but a KILLed one
    // cannot, and a dead encoder's "ok" reading as current is the lie the file exists to end
    // (bd remora-e5x.18.3).
    sp.run({"rm", "-f", frameSock, hostEncodeVideoSocketFor(containerName),
            hostEncodeStatusFileFor(containerName)},
           {}, log);
    return int(victims.size());
}

// Seed the HOST's adb public key into the device's /data/misc/adb/adb_keys.
//
// A device whose /data is genuinely empty has no authorized keys, and the guest ships
// ro.adb.secure=1, so adb answers "device unauthorized" and asks for a confirmation dialog on
// the device — which cannot be clicked, because the mirror that would show it is precisely what
// adb is blocking. Nothing in Remora used to seed this: every device so far inherited keys from a
// /data that had been authorized at some point in its life, so a first boot with no history was
// never exercised until overlayfs became the default and the merged /data started out empty.
//
// Over docker exec, which needs no adb — that is the whole point. Idempotent (grep -qxF), and
// re-run on every connect so a device that loses its keys heals itself. adbd re-reads the file
// on each auth attempt, so no adbd restart is needed.
static void seedAdbKey(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    QFile f(QDir::homePath() + QStringLiteral("/.android/adbkey.pub"));
    if (!f.open(QIODevice::ReadOnly)) return;  // adb mints one on its first run; nothing to seed
    const QString key = QString::fromUtf8(f.readAll()).trimmed();
    // The key is base64 plus a "user@host" comment. Anything that could break out of the quoting
    // below is not a key we are willing to interpolate into a shell command.
    if (key.isEmpty() || key.contains(QLatin1Char('\'')) || key.contains(QLatin1Char('"'))
        || key.contains(QLatin1Char('$')) || key.contains(QLatin1Char('`')))
        return;
    const QString inner =
        QStringLiteral("mkdir -p /data/misc/adb; "
                       "grep -qxF '%1' /data/misc/adb/adb_keys 2>/dev/null || "
                       "echo '%1' >> /data/misc/adb/adb_keys; "
                       "chmod 640 /data/misc/adb/adb_keys; "
                       "chown system:shell /data/misc/adb/adb_keys")
            .arg(key);
    if (ctx.backend == Backend::Bare)
        sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", inner}, {}, log);
    else
        sp.run(Spawner::sshArgv(ctx.guest, QStringLiteral("docker exec %1 sh -c \"%2\"")
                                               .arg(ctx.rc.containerName, inner)),
               {}, log);
}

// Share dirs must look like the stock /sdcard dirs (setgid media_rw) or the MediaProvider FUSE
// creates device-side files as 600/<app-uid> — unreadable from the host (proven live: with the
// setgid group they land 66x/1023 and a gid-1023 member or a default ACL reads them back). Root
// inside the container (adb shell is uid 2000, can't chgrp); the OWNER is deliberately untouched
// so the user never loses their own host folder. Idempotent, every connect.
static void fixupSharedFolders(Spawner &sp, const RunContext &ctx, const LineSink &log) {
    if (ctx.rc.sharedFolders.isEmpty()) return;
    QStringList cmds;
    for (const QString &f : ctx.rc.sharedFolders) {
        const QString dir = sharedFolderBind(f).section(QLatin1Char(':'), 1);
        // Under the overlay (the default) the share is staged at /remora/share/<name> — docker
        // binds beneath /data are shadowed by the init-time overlay mount — and re-bound into
        // the merged /data here, after boot. Container restarts reset mounts; every connect
        // redoes them, and mountpoint -q keeps it idempotent.
        if (ctx.rc.useOverlayfs) {
            const QString stage = stagedShareBind(f).section(QLatin1Char(':'), 1);
            cmds << QStringLiteral("mountpoint -q %1 || { mkdir -p %1; mount --bind %2 %1; }")
                        .arg(dir, stage);
        }
        cmds << QStringLiteral("chgrp 1023 %1; chmod g+rws %1").arg(dir);
    }
    const QString inner = cmds.join(QStringLiteral("; ")) + QStringLiteral("; true");
    if (ctx.backend == Backend::Bare)
        sp.run({"docker", "exec", ctx.rc.containerName, "sh", "-c", inner}, {}, log);
    else
        sp.run(Spawner::sshArgv(ctx.guest, QStringLiteral("docker exec %1 sh -c \"%2\"")
                                               .arg(ctx.rc.containerName, inner)),
               {}, log);
}

QStringList dockerHostArgv(const QString &host, const QString &cmd) {
    if (host.isEmpty()) return {"sh", "-c", cmd};
    return Spawner::sshArgv(host, cmd);
}

NetworkEnsure ensureMacvlanNetwork(Spawner &sp, const ResolvedConfig &rc, const QString &host,
                                   const LineSink &log) {
    if (rc.networkMode != QLatin1String("macvlan")) return {};
    const QString name = rc.dockerNetwork.value_or(QString());
    if (name.isEmpty())
        return {false, QStringLiteral("macvlan mode with no network name — set docker_network=")};

    const QString where = host.isEmpty() ? QStringLiteral("the host") : host;
    if (sp.run(dockerHostArgv(host, QStringLiteral("docker network inspect %1 >/dev/null 2>&1")
                                        .arg(name)),
               {}, log)
            .rc == 0) {
        // Present. A pinned address on the wrong subnet is the one mismatch worth catching here:
        // docker's own refusal ("no available IPv4 addresses") names neither the address nor the
        // network, and the operator reads it as the pool being full.
        if (rc.macvlanIp && rc.macvlanSubnet && !ipv4InSubnet(*rc.macvlanIp, *rc.macvlanSubnet))
            return {false, QStringLiteral("macvlan_ip %1 is not inside %2 — the container could "
                                          "not be addressed on network '%3'")
                               .arg(*rc.macvlanIp, *rc.macvlanSubnet, name)};
        return {true, QStringLiteral("network '%1' present on %2").arg(name, where)};
    }

    // Missing. Create it from whatever the host itself reports, which is the whole point: the
    // operator picks macvlan, not a subnet. buildMacvlanCreateArgv returns empty when the shape is
    // incomplete, and that is a hard stop — a network created on a guessed subnet would come up
    // fine and route nowhere.
    const QStringList create = buildMacvlanCreateArgv(rc);
    if (create.isEmpty())
        return {false,
                QStringLiteral("macvlan network '%1' is missing on %2 and its shape could not be "
                               "read from that host (no default route with an address?) — set "
                               "macvlan_parent=, macvlan_subnet= and macvlan_gateway=, or create "
                               "the network by hand")
                    .arg(name, where)};
    const ProcResult r =
        sp.run(dockerHostArgv(host, create.join(QLatin1Char(' '))), {}, log);
    if (r.rc != 0)
        return {false, QStringLiteral("could not create macvlan network '%1' on %2: %3")
                           .arg(name, where, r.stderrTail.trimmed())};
    return {true, QStringLiteral("created network '%1' (%2 via %3 on %4)")
                      .arg(name, rc.macvlanSubnet.value_or(QString()),
                           rc.macvlanGateway.value_or(QString()),
                           rc.macvlanParent.value_or(QString()))};
}

HostRouteResult ensureMacvlanHostRoute(Spawner &sp, const ResolvedConfig &rc, const LineSink &log) {
    if (rc.networkMode != QLatin1String("macvlan") || !rc.macvlanHostRoute) return {};
    const QString shim = QString::fromUtf8(kMacvlanShimName);

    // Already up? Then the routes may still need re-asserting (a container recreated on a new
    // address), but the ADDRESS conflict check below has already been passed once and re-running
    // it would only cost a probe. Distinguish the two cases on the interface's existence.
    const bool present =
        sp.run({"sh", "-c", QStringLiteral("ip link show %1 >/dev/null 2>&1").arg(shim)}, {}, log)
            .rc == 0;

    const QStringList argv = buildMacvlanShimArgv(rc);
    if (argv.isEmpty())
        return {false,
                QStringLiteral("macvlan host route is on but its shape is unresolved (parent=%1, "
                               "shim address=%2) — set macvlan_shim_ip=, or macvlan_host_route="
                               "false to manage host routing yourself")
                    .arg(rc.macvlanParent.value_or(QStringLiteral("?")),
                         rc.macvlanShimIp.value_or(QStringLiteral("?")))};

    if (!present) {
        // The shim carries a REAL address on a REAL segment, so hand it one nobody answers to.
        // Checked only before the first creation: once the shim owns it, this probe would answer
        // itself and every later deploy would refuse to proceed.
        const QString ip = *rc.macvlanShimIp;
        if (sp.run({"sh", "-c", QStringLiteral("ping -c1 -W1 %1 >/dev/null 2>&1").arg(ip)}, {}, log)
                .rc == 0)
            return {false, QStringLiteral("macvlan host route: %1 is already answering on the LAN "
                                          "— set macvlan_shim_ip= to a free address")
                               .arg(ip)};
        // Say what is about to happen to host networking BEFORE it happens. This needs no sudo —
        // the docker group is root-equivalent — which makes it exactly the sort of change that
        // should be announced rather than discovered later in `ip link`.
        if (log)
            log(QStringLiteral("stdout"),
                QStringLiteral("macvlan: adding host interface %1 on %2 (%3) so this machine can "
                               "reach its own containers — a host cannot talk to its macvlan "
                               "children over the parent NIC")
                    .arg(shim, rc.macvlanParent.value_or(QString()), ip));
        // The helper image is tiny and cached after the first pull; a host that cannot reach a
        // registry falls back to the vendored script, which the failure message names.
        if (sp.run({"docker", "image", "inspect", QString::fromUtf8(kMacvlanShimImage)}, {}, log)
                .rc != 0)
            sp.run({"docker", "pull", QString::fromUtf8(kMacvlanShimImage)}, {}, log);
    }

    // Two owners for one interface is how it ends up in a state neither expects. Remora reconciles
    // the shim on every connect, so a boot-time unit doing the same thing is at best redundant and
    // at worst a race — say so rather than letting the interface flap silently.
    const bool rivalOwner =
        sp.run({"sh", "-c", "test -e /etc/systemd/system/remora-macvlan-shim.service"}, {}, log)
            .rc == 0;

    const ProcResult r = sp.run(argv, {}, log);
    if (r.rc != 0)
        return {false,
                QStringLiteral("macvlan host route: could not configure %1 (%2). Run it by hand: "
                               "sudo sh vendor/host-prereqs/macvlan-shim/remora-macvlan-shim "
                               "install %3 %4")
                    .arg(shim, r.stderrTail.trimmed(), rc.macvlanParent.value_or(QString()),
                         macvlanShimRoutes(rc).join(QLatin1Char(' '))),
                true};
    QString detail = QStringLiteral("host route via %1 (%2 -> %3)")
                         .arg(shim, *rc.macvlanShimIp,
                              macvlanShimRoutes(rc).join(QLatin1Char(' ')));
    if (rivalOwner)
        detail += QStringLiteral("; a remora-macvlan-shim systemd unit is installed too — Remora "
                                 "owns this interface now, so remove it (sudo systemctl disable "
                                 "--now remora-macvlan-shim.service) or set "
                                 "macvlan_host_route=false to hand ownership back");
    return {true, detail, !present || rivalOwner};
}

// "<network>=<ip> " per attachment. Both halves are needed: the NAME decides whether the container
// is on the right network at all, the ADDRESS whether a pin has since changed.
const char *const kNetworkInspectFormat =
    "{{range $k,$v := .NetworkSettings.Networks}}{{$k}}={{$v.IPAddress}} {{end}}";

bool containerNetworkStale(const QString &inspectOut, const ResolvedConfig &rc) {
    const QString out = inspectOut.trimmed();
    if (out.isEmpty()) return false;  // no container to be stale
    QMap<QString, QString> nets;
    for (const QString &tok : out.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        nets.insert(tok.section(QLatin1Char('='), 0, 0), tok.section(QLatin1Char('='), 1));

    if (rc.networkMode == QLatin1String("macvlan")) {
        const QString want = rc.dockerNetwork.value_or(QString());
        if (want.isEmpty() || !nets.contains(want)) return true;
        // A changed pin has to recreate too: --ip is fixed at create time, so a running container
        // keeps the old address no matter what the config now says.
        return rc.macvlanIp.has_value() && nets.value(want) != *rc.macvlanIp;
    }
    // Bridge mode: any attachment to a network Remora would have created for macvlan means this
    // container predates the switch back. The default bridge is unnamed in this sense — docker
    // calls it "bridge" — so that one alone is the match.
    return !(nets.size() == 1 && nets.contains(QStringLiteral("bridge")));
}

bool containerDevicesStale(const QStringList &devices, const ResolvedConfig &rc) {
    // Every device this resolution would pass must already be in the container…
    QStringList want = rc.cameraDevices;
    if (rc.gpuNode.has_value()) want << *rc.gpuNode;
    if (rc.venus && rc.venusNode.has_value()) want << *rc.venusNode;
    for (const QString &d : want)
        if (!devices.contains(d)) return true;
    // …and every camera node the container carries must still be resolved — an unplugged (or
    // un-configured) camera staying passed through is the same silent drift the other way. Only
    // video nodes get this reverse check: the GPU and venus nodes come from a live capability
    // probe and are legitimately absent from a resolution that skipped it, so demanding the
    // container carry nothing else would recreate on every warm reconnect.
    for (const QString &d : devices)
        if (d.startsWith(QLatin1String("/dev/video")) && !rc.cameraDevices.contains(d))
            return true;
    return false;
}

QString resolveAdbTarget(Spawner &sp, const RunContext &ctx, QString *err, const LineSink &log) {
    if (!ctx.rc.target.isEmpty()) return ctx.rc.target;
    const ProcResult ip = sp.run(
        dockerHostArgv(ctx.guest,
                       QStringLiteral("docker inspect -f '{{range .NetworkSettings.Networks}}"
                                      "{{.IPAddress}}{{end}}' %1")
                           .arg(ctx.rc.containerName)),
        {}, log);
    const QString addr = ip.out.trimmed();
    if (addr.isEmpty()) {
        if (err)
            *err = QStringLiteral("no macvlan IP was pinned and the container's assigned IP could "
                                  "not be read — is %1 deployed on %2?")
                       .arg(ctx.rc.containerName,
                            ctx.guest.isEmpty() ? QStringLiteral("this host") : ctx.guest);
        return {};
    }
    // The container's OWN listening port, not hostAdbPort — macvlan publishes nothing. See the
    // matching rule in resolve().
    return QStringLiteral("%1:%2").arg(addr).arg(ctx.rc.containerAdbPort);
}

int countMirrorClients(Spawner &sp, const QString &target) {
    // pgrep -c prints the count itself (0 included); it just exits 1 on no match — hence ||true.
    // [r]emora: the self-exclusion trick — the sh -c wrapper's own cmdline contains this pattern
    // and would otherwise count as a phantom client.
    const ProcResult r = sp.run(
        {"sh", "-c",
         QStringLiteral("pgrep -fc '[r]emora mirror.*-s %1' || true").arg(target)}, {}, {});
    bool ok = false;
    const int n = r.out.trimmed().toInt(&ok);
    return ok ? n : 0;
}

bool captureScreenshot(Spawner &sp, const QString &target, const QString &outPath, QString *err) {
    const ProcResult r = sp.run(
        {"sh", "-c",
         QStringLiteral("adb -s %1 exec-out screencap -p > '%2'").arg(target, outPath)},
        {}, {});
    if (r.rc != 0) {
        if (err)
            *err = r.stderrTail.isEmpty() ? QStringLiteral("screencap failed (adb rc %1)").arg(r.rc)
                                          : r.stderrTail;
        return false;
    }
    if (QFileInfo(outPath).size() < 8) {  // a real PNG is never this small — a dead capture
        QFile::remove(outPath);
        if (err) *err = QStringLiteral("screencap produced no image — is the device booted?");
        return false;
    }
    return true;
}

// Host-side encoder for the mirror (bd remora-e5x.6). Like the render server this is host state
// the mirror depends on but the container does not own, so it is ensured at connect rather than
// only in the deploy chain — reconnect and GUI runners skip that chain entirely.
StepResult wipeDataDir(Spawner &sp, const ResolvedConfig &rc, const LineSink &log) {
    if (rc.dataDir.isEmpty()) return StepResult::fail("this profile has no data dir to remove");
    // Refuse while the container is up: removing the tree under a live Android is how you get a
    // half-wiped instance that still boots. The caller gets to stop it and retry.
    // Two-field probe to match the shape used everywhere else in the engine (a lone
    // {{.State.Running}} would be the only single-field variant and reads as an oversight).
    const ProcResult st = sp.run(
        {"docker", "inspect", "-f", "{{.State.Running}} {{.State.Paused}}", rc.containerName}, {},
        log);
    if (st.out.simplified().split(QLatin1Char(' ')).value(0) == QLatin1String("true"))
        return StepResult::fail(
            QStringLiteral("%1 is still running — stop it before wiping its data")
                .arg(rc.containerName));

    // The contents belong to root and to Android's system uids, so the removal is delegated to a
    // container with the same privilege rather than attempted as this user.
    //
    // busybox, NOT the profile's own the guest image. The the guest image does ship /system/bin/sh,
    // but it cannot be exec'd as an entrypoint: its ELF interpreter lives in an apex that only
    // init mounts, so docker fails with "exec /system/bin/sh: no such file or directory" before
    // anything runs. Verified by trying it. busybox is a few hundred KB and is the conventional
    // image for exactly this job.
    //
    // The tree is mounted one level down (/wipe/target) so the container removes its CONTENTS and
    // the directory itself survives, still owned by the user — mounting it as the target and
    // deleting that would leave the next deploy to recreate it root-owned, inverting the problem.
    const QString sh = QStringLiteral("rm -rf /wipe/target/* /wipe/target/.[!.]* 2>/dev/null; true");
    const QStringList argv{"docker", "run", "--rm", "--privileged", "-v",
                           rc.dataDir + ":/wipe/target", "busybox", "sh", "-c", sh};
    if (sp.run(argv, {}, log).rc != 0)
        return StepResult::fail(
            QStringLiteral("could not remove the contents of %1 — the removal runs in a privileged "
                           "busybox container (that is what avoids needing sudo), so check that "
                           "docker works and that the busybox image can be pulled")
                .arg(rc.dataDir));
    return StepResult::good(QStringLiteral("wiped %1").arg(rc.dataDir));
}

// reboundStarved, when given, is set if the encoder was replaced because it was RECEIVING
// nothing. The caller reports it on the connect step: this function's own detail goes through
// line(), which ConsoleSink drops, and a silent self-heal is indistinguishable from never having
// been broken — which is the thing that cost a whole session to find (bd remora-d3t).
StepResult ensureHostDecoder(Spawner &sp, const ResolvedConfig &rc, const LineSink &log) {
    if (!rc.hostDecode) return StepResult::good();

    const QString bin = vendorNativeBinary(QStringLiteral("remora-frame-decoder"));
    if (sp.run({"sh", "-c", QStringLiteral("[ -x %1 ]").arg(bin)}, {}, log).rc != 0)
        return StepResult::fail(
            QStringLiteral("host_decode is on but %1 is not built — the Remora build compiles "
                           "it when it finds libva and FFmpeg's development files; install those "
                           "and rebuild, or set host_decode=false")
                .arg(bin));

    // GATE ON THE HELPER'S OWN --probe, not on our idea of what the host can do (bd remora-4ei.79's
    // rule). It answers the only question that matters — can this host decode AND export at all —
    // and it answers it with the same libva/ffmpeg stack the session will use, so a host that
    // probes clean and then fails at session setup is a real bug rather than a mismatch between
    // two opinions. Exit 0 yes, 1 no.
    QStringList probe = buildHostDecoderProbeArgv(rc);
    probe[0] = bin;
    if (sp.run(probe, {}, log).rc != 0)
        return StepResult::fail(
            QStringLiteral("host_decode is on but %1 --probe failed — this host cannot "
                           "decode-and-export (no VA decode entrypoints, or no usable render "
                           "node). Set host_decode=false").arg(bin));

    // Health is the socket PLUS a live process PLUS not-older-than-the-container, the same three
    // signals ensureHostEncoder uses and for the same reasons: a killed helper leaves its socket
    // inode behind so presence alone proves nothing; `ps -C` matches the process NAME so a pattern
    // match would also match the shell running the check; and the socket path is derived from the
    // container NAME, which survives a recreate, so a helper from a destroyed container matches
    // both other checks perfectly and would be adopted (bd remora-e5x.19, the same class).
    //
    // MATCHED ON THIS PROFILE'S SOCKET, not merely on the binary name, or a second profile's
    // decoder makes this one look up when it is not (bd remora-e5x.13).
    const QString psMine =
        QStringLiteral("ps -C remora-frame-decoder -o pid=,args= 2>/dev/null | grep -F -- '%1'")
            .arg(rc.hostDecodeSocket);
    const bool proc = sp.run({"sh", "-c", psMine + " >/dev/null 2>&1"}, {}, log).rc == 0;
    const bool sockOk =
        sp.run({"sh", "-c", QStringLiteral("[ -S %1 ]").arg(rc.hostDecodeSocket)}, {}, log).rc == 0;
    // The socket is created at startup, so its mtime IS the helper's start time — no ps(1) start
    // time parsing. Container missing or unparseable => treat as stale and rebind: restarting a
    // decoder is cheap, adopting a dead one costs every video the device plays.
    const QString freshEnough =
        QStringLiteral("cs=$(docker inspect -f '{{.State.StartedAt}}' %1 2>/dev/null); "
                       "[ -n \"$cs\" ] || exit 1; "
                       "ce=$(date -d \"$cs\" +%s 2>/dev/null) || exit 1; "
                       "es=$(stat -c %Y %2 2>/dev/null) || exit 1; "
                       "[ \"$es\" -ge \"$ce\" ]")
            .arg(rc.containerName, rc.hostDecodeSocket);
    const bool fresh = sp.run({"sh", "-c", freshEnough}, {}, log).rc == 0;

    if (proc && sockOk && fresh)
        return StepResult::good(QStringLiteral("host decoder already on %1").arg(rc.hostDecodeSocket));
    if (proc && sockOk && !fresh)
        log(QStringLiteral("host-decoder"),
            QStringLiteral("decoder on %1 predates the container — rebinding")
                .arg(rc.hostDecodeSocket));

    // Reap whatever is left before rebinding: a half-dead helper keeps the socket BOUND, so a
    // fresh one cannot listen while the old one keeps its NVDEC session. SIGKILL after SIGTERM
    // because the encoder's orphan ignored SIGTERM and there is no reason this one differs; kill
    // on a dead pid is a harmless no-op.
    const QString pidsMine = psMine + " | awk '{print $1}'";
    sp.run({"sh", "-c", pidsMine + " | xargs -r kill 2>/dev/null; true"}, {}, log);
    sp.waitMs(1000);
    sp.run({"sh", "-c", pidsMine + " | xargs -r kill -9 2>/dev/null; true"}, {}, log);
    sp.run({"rm", "-f", rc.hostDecodeSocket}, {}, log);
    // The DIRECTORY must exist before the helper binds in it, and root-owned is fatal: docker
    // creates a missing bind source as root, and the helper then cannot bind. Same second line of
    // defence as the encoder's — the deploy makes it in host-prereqs, this covers the paths that
    // reach here without a deploy (reconnect, a manual restart).
    sp.run({"mkdir", "-p", hostDecodeSocketDirFor(rc.containerName)}, {}, log);

    QStringList argv = buildHostDecoderArgv(rc);
    argv[0] = bin;
    // Detached in its own transient scope, for the same reason the encoder and the mirror are: a
    // GUI launched from the desktop would otherwise leave this in the app unit's cgroup, whose
    // KillMode reaps it on teardown (bd remora-4ei.29).
    if (sp.spawnDetached(argv, {}).pid <= 0)
        return StepResult::fail(QStringLiteral("could not start %1").arg(bin));
    return StepResult::good(QStringLiteral("host decoder on %1").arg(rc.hostDecodeSocket));
}

StepResult ensureHostEncoder(Spawner &sp, const ResolvedConfig &rc, const LineSink &log,
                             bool *reboundStarved) {
    if (reboundStarved) *reboundStarved = false;
    if (!rc.hostEncode) return StepResult::good();

    const QString bin = vendorNativeBinary(QStringLiteral("remora-frame-encoder"));
    if (sp.run({"sh", "-c", QStringLiteral("[ -x %1 ]").arg(bin)}, {}, log).rc != 0)
        return StepResult::fail(
            QStringLiteral("host_encode is on but %1 is not built — the Remora build compiles "
                           "it when it finds FFmpeg 7 or newer (libavfilter) and the Vulkan "
                           "headers; install those and rebuild, or set host_encode=false")
                .arg(bin));

    // Health is the socket plus a live process, for the same reason as the render server: a killed
    // encoder leaves its socket inode behind, and everything then looks present while nothing
    // publishes. `ps -C` matches the process NAME — a pattern match would also match the shell
    // running the check and could never report "not running".
    //
    // MATCHED ON THIS PROFILE'S FRAME SOCKET, not merely on the name. `ps -C` alone answers "yes"
    // for ANY profile's encoder, so a second instance made this one look up when it was not; and
    // combined with a leftover socket inode it also adopted a HALF-DEAD encoder, handing the mirror
    // a socket that would never answer while the real cause (that encoder was finished) stayed
    // invisible. That is what produced "the mirror exited immediately after 5 attempts" with a
    // render-server log line attached and nothing pointing at the encoder (bd remora-e5x.13).
    const QString psMine =
        QStringLiteral("ps -C remora-frame-encoder -o pid=,args= 2>/dev/null | grep -F -- '%1'")
            .arg(rc.hostEncodeFrameSocket);
    const bool proc = sp.run({"sh", "-c", psMine + " >/dev/null 2>&1"}, {}, log).rc == 0;
    const bool sockOk =
        sp.run({"sh", "-c", QStringLiteral("[ -S %1 ]").arg(rc.hostEncodeFrameSocket)}, {}, log)
            .rc == 0;
    // …AND NOT OLDER THAN THE CONTAINER. The frame-socket path is derived from the container
    // NAME, which survives a recreate, so an encoder left over from a destroyed container matches
    // both checks above perfectly and gets adopted — feeding the mirror from a container that no
    // longer exists. Observed as a connect that reports ok and then drops the moment the boot
    // animation ends, with six mirror clients and a two-containers-old encoder on the host
    // (bd remora-e5x.19). bd remora-4u4.5 fixed the sibling case, another PROFILE's server; this
    // is the same class one level down — same profile, older container generation — and no path
    // comparison can see it, because the path is identical.
    //
    // The socket is created by the encoder at startup, so its mtime IS the encoder's start time,
    // which avoids parsing ps(1) start times. Container missing or unparseable => treat as stale
    // and rebind: restarting an encoder is cheap, adopting a dead one costs the whole mirror.
    const QString freshEnough =
        QStringLiteral("cs=$(docker inspect -f '{{.State.StartedAt}}' %1 2>/dev/null); "
                       "[ -n \"$cs\" ] || exit 1; "
                       "ce=$(date -d \"$cs\" +%s 2>/dev/null) || exit 1; "
                       "es=$(stat -c %Y %2 2>/dev/null) || exit 1; "
                       "[ \"$es\" -ge \"$ce\" ]")
            .arg(rc.containerName, rc.hostEncodeFrameSocket);
    const bool fresh = sp.run({"sh", "-c", freshEnough}, {}, log).rc == 0;
    // A FOURTH staleness signal, and the only one that has to be MEASURED rather than inspected:
    // an encoder can be running, bound and newer than the container and still be receiving
    // nothing. The render server's publisher stops resolving submits after a display swap — its
    // own log shows `submits` climbing while `resolved` sits frozen — and the encoder falls back
    // to emitting its 300 ms keepalive. proc, sockOk and fresh are all still true, so adopting it
    // hands the next mirror the same dead link, which is why reconnecting never fixed this and
    // only killing the encoder did (bd remora-d3t, caught live at 29.9 fps composed against 3.2
    // encoded).
    //
    // Asked LAST and only when the other three would have said adopt: it is the one check that
    // costs wall-clock, and it answers nullopt immediately unless both a live mirror and a live
    // encoder are there to measure — i.e. it is free on every cold path and paid only on the warm
    // reconnect where the fault can actually exist.
    //
    // probeMirrorFlow gates itself: it returns after a few seconds once the encoder is plainly
    // carrying frames, and only spends its full window when the answer is genuinely in doubt. So
    // a busy mirror costs seconds here and an idle one the full window — never their sum, which
    // is what a separate pre-probe would have cost.
    //
    // What that gate gives up, stated rather than buried: a PARTIAL stall, where the publisher
    // resolves some submits and the encoder emits, say, 20 fps against 60 composed, is above the
    // threshold and would be waved through. The observed fault is binary — `resolved` fully
    // frozen — so it trades a failure mode nobody has seen for seconds on every warm reconnect.
    QString starvedNote;
    if (proc && sockOk && fresh) {
        if (const auto flow = probeMirrorFlow(sp, rc); flow && flow->starved)
            starvedNote = QStringLiteral("%1 fps composed but only %2 encoded (%3%)")
                              .arg(flow->composedFps, 0, 'f', 1)
                              .arg(flow->encodedFps, 0, 'f', 1)
                              .arg(flow->ratio * 100, 0, 'f', 0);
    }
    if (proc && sockOk && fresh && starvedNote.isEmpty())
        return StepResult::good(
            QStringLiteral("host encoder already on %1").arg(rc.hostEncodeFrameSocket));
    if (proc && sockOk && !fresh)
        log(QStringLiteral("host-encoder"),
            QStringLiteral("encoder on %1 predates the container — rebinding")
                .arg(rc.hostEncodeFrameSocket));
    else if (!starvedNote.isEmpty()) {
        if (reboundStarved) *reboundStarved = true;
        log(QStringLiteral("host-encoder"),
            QStringLiteral("encoder on %1 is bound but starved (%2) — rebinding rather than "
                           "handing the new mirror the same dead link")
                .arg(rc.hostEncodeFrameSocket, starvedNote));
    }

    // Reap whatever is left of this profile's encoder before rebinding. A half-dead one keeps the
    // sockets BOUND, so a fresh encoder cannot listen and the old one keeps its GPU memory — the
    // two failures compound. SIGKILL follows SIGTERM because the orphan found in the wild ignored
    // SIGTERM; kill on an already-dead pid is a harmless no-op, so this costs nothing when clean.
    const QString pidsMine = psMine + " | awk '{print $1}'";
    sp.run({"sh", "-c", pidsMine + " | xargs -r kill 2>/dev/null; true"}, {}, log);
    sp.waitMs(1000);
    sp.run({"sh", "-c", pidsMine + " | xargs -r kill -9 2>/dev/null; true"}, {}, log);

    sp.run({"rm", "-f", rc.hostEncodeFrameSocket}, {}, log);
    sp.run({"rm", "-f", rc.hostEncodeVideoSocket}, {}, log);
    // The frame socket lives in a per-container DIRECTORY now, because the composer reaches it
    // through a bind mount and a file bind cannot survive the encoder recreating its inode
    // (bd remora-emoe phase 2). The deploy chain creates it in HOST-PREREQS, before provision —
    // it has to, since docker creates a missing bind source root-owned and the encoder then cannot
    // bind in it. This mkdir is the second line of defence for the paths that reach the encoder
    // WITHOUT a deploy (reconnect, a manually restarted encoder), where no provision has run.
    sp.run({"mkdir", "-p", hostEncodeFrameSocketDirFor(rc.containerName)}, {}, log);
    // And the predecessor's status file: the encoder just killed cannot remove its own, and its
    // last word must not read as the NEW encoder's state (bd remora-e5x.18.3).
    sp.run({"rm", "-f", hostEncodeStatusFileFor(rc.containerName)}, {}, log);

    QStringList argv = buildHostEncoderArgv(rc);
    argv[0] = bin;
    // Detached in its own transient scope, for the same reason the mirror is: a GUI launched from the
    // desktop would otherwise leave the encoder in the app unit's cgroup, whose KillMode reaps it
    // on teardown (bd remora-4ei.29).
    //
    // GLIBC_TUNABLES IS A TRIPWIRE, not configuration (bd remora-e5x.18.9). Six encoder cores were
    // recovered from Aug 8; five were an NVIDIA blob walking a per-object table with a NULL slot
    // (not ours, and dead with the 610.43.03 driver), but ONE was in-allocator — a posix_memalign
    // crash, i.e. something had already corrupted the heap by the time libc noticed. The encoder's
    // degrade/cross-pool fix is the candidate mechanism and is unproven, and the cores can no
    // longer arbitrate: the Aug 10 world update made every library frame in them unsymbolizable.
    // malloc.check=3 asks glibc to validate on each malloc/free and abort AT the corrupting call
    // rather than leave an unattributable SEGV later.
    //
    // ITS INCREMENTAL VALUE HERE IS UNDEMONSTRATED, and saying so is the point of this paragraph.
    // The tunable IS recognised on this host (ld.so --list-tunables reports glibc.malloc.check,
    // max 3, on glibc 2.43), but three synthetic corruptions — a top-size clobber, a small
    // neighbour overflow, and a full overflow into an adjacent chunk header — abort IDENTICALLY
    // with and without it, because 2.43's default hardening already catches all three. So this is
    // belt-and-braces that costs nothing, not the guarantee bd remora-e5x.18.9 hoped for when it
    // prescribed it; the default allocator is already doing most of that job. Do not read a clean
    // soak WITH this set as proof the heap is sound.
    // Kept anyway because it is free, because a weaker-defaulting glibc or host may yet need it,
    // and because the crash is rare enough that an opt-in flag would never be set on the run that
    // mattered. The bead's own stronger instrument is unchanged and still the one to reach for if
    // signature B recurs: valgrind, which sees the blob and libav asm writers that ASan does not
    // (the bd remora-181t lesson).
    if (sp.spawnDetached(argv, {{QStringLiteral("GLIBC_TUNABLES"),
                                 QStringLiteral("glibc.malloc.check=3")}})
            .pid == 0)
        return StepResult::fail("host encoder: could not start");

    // Wait for the FRAME socket, which the encoder binds immediately. The video socket is not
    // waited on: the encoder only creates it once it has imported a frame, and frames only exist
    // once something is driving the display — waiting here would deadlock.
    for (int i = 0; i < 40; ++i) {
        if (sp.run({"sh", "-c", QStringLiteral("[ -S %1 ]").arg(rc.hostEncodeFrameSocket)}, {}, log)
                .rc == 0)
            return StepResult::good(
                QStringLiteral("host encoder listening on %1").arg(rc.hostEncodeFrameSocket));
        sp.waitMs(250);
    }
    return StepResult::fail("host encoder: started but never bound its frame socket");
}

StepResult doConnect(Spawner &sp, const RunContext &ctx, LogSink &sink, int retries,
                     int retryDelayMs, int livenessDelayMs, int mirrorRetries, int bootWaitMs,
                     bool waitLit, int bootStableMs, int mirrorHoldMs) {
    QString target = ctx.rc.target;
    const LineSink log = [&](const QString &stream, const QString &text) {
        sink.line(QStringLiteral("connect"), stream, text);
    };
    // No server-jar preflight since the v1 purge (bd remora-28ix.5): the device half is the
    // agent the image bakes, so there is no host-side file whose absence could fail a session —
    // an image that predates the bake gets the client's own plain refusal in the tail.
    // The Venus render server is host-side state the container DEPENDS ON but does not own, so it
    // has to be checked here and not only in the deploy chain's host-prereqs step. Connecting to an
    // already-running container — reconnect, a GUI runner, waking a paused device — skips that
    // chain entirely; a container whose server has since died then boot-loops SurfaceFlinger
    // against a dead socket and never leaves the boot animation, which is exactly what happened
    // once the server was killed out from under a running container. Non-fatal on failure: the
    // deploy chain reports the same condition properly, and a connect should still try.
    if (ctx.rc.venus) {
        const StepResult v = ensureVenusServer(sp, ctx.rc, log);
        if (!v.ok)
            log(QStringLiteral("stderr"), QStringLiteral("venus: %1").arg(v.detail));
        else if (!v.detail.isEmpty())
            log(QStringLiteral("stdout"), v.detail);
    }
    // Non-fatal on failure, like the render server above: a mirror that cannot host-encode should
    // still connect and mirror the ordinary way rather than refuse to start.
    bool encoderReboundStarved = false;
    if (ctx.rc.hostEncode) {
        const StepResult e = ensureHostEncoder(sp, ctx.rc, log, &encoderReboundStarved);
        log(e.ok ? QStringLiteral("stdout") : QStringLiteral("stderr"),
            e.ok ? e.detail : QStringLiteral("host encoder: %1").arg(e.detail));
        // No "running but unconsumed" warning any more: since the v1 purge the mirror follows
        // hostEncode unconditionally (the agent composes natively), so an encoder this step
        // started always has its consumer.
    }
    // Host DECODE, non-fatal for the same reason and more so: c2-va falls back to the local Intel
    // VA decoder on its own when the socket does not answer, so a failure here costs the NVDEC
    // optimisation and nothing else. Refusing to connect over it would trade working video for no
    // video (bd remora-e5x.37).
    if (ctx.rc.hostDecode) {
        const StepResult d = ensureHostDecoder(sp, ctx.rc, log);
        log(d.ok ? QStringLiteral("stdout") : QStringLiteral("stderr"),
            d.ok ? d.detail : QStringLiteral("host decoder: %1").arg(d.detail));
    }
    // Connect assumes a running container — verify that first. Without this a Connect against a
    // stopped/removed container burns every adb retry on "No route to host" and the honest cause
    // (nothing is running; a Deploy is needed) never appears. A PAUSED container (remora sleep)
    // reports Running=true but every adb packet stalls — wake it here, transparently: Connect on
    // a sleeping device just resumes it (~1s) and attaches.
    {
        const QString inspect =
            QStringLiteral("docker inspect -f '{{.State.Running}} {{.State.Paused}}' %1")
                .arg(ctx.rc.containerName);
        const ProcResult st =
            ctx.backend == Backend::Bare
                ? sp.run({"docker", "inspect", "-f", "{{.State.Running}} {{.State.Paused}}",
                          ctx.rc.containerName}, {}, log)
                : sp.run(Spawner::sshArgv(ctx.guest, inspect), {}, log);
        const QStringList state = st.out.simplified().split(QLatin1Char(' '));
        if (state.value(0) != QLatin1String("true"))
            return StepResult::fail(
                QStringLiteral("container %1 is not running%2 — it exited or was removed; "
                               "Connect again to redeploy it")
                    .arg(ctx.rc.containerName,
                         ctx.backend == Backend::Bare ? QString()
                                                      : QStringLiteral(" on %1").arg(ctx.guest)),
                st.stderrTail);
        if (state.value(1) == QLatin1String("true")) {
            log(QStringLiteral("stdout"),
                QStringLiteral("device is asleep (paused) — waking it"));
            const QString unpause =
                QStringLiteral("docker unpause %1").arg(ctx.rc.containerName);
            const ProcResult up =
                ctx.backend == Backend::Bare
                    ? sp.run({"docker", "unpause", ctx.rc.containerName}, {}, log)
                    : sp.run(Spawner::sshArgv(ctx.guest, unpause), {}, log);
            if (up.rc != 0)
                return StepResult::fail(
                    QStringLiteral("could not wake %1 (docker unpause failed)")
                        .arg(ctx.rc.containerName),
                    up.stderrTail);
        }
    }
    // Share dirs get their media perms before anything writes through them.
    fixupSharedFolders(sp, ctx, log);
    QStringList mirrorArgv = ctx.mirrorArgv;
    // macvlan without a pinned IP: docker IPAM assigned one at provision — discover it now.
    if (target.isEmpty()) {
        QString err;
        target = resolveAdbTarget(sp, ctx, &err, log);
        if (target.isEmpty()) return StepResult::fail(err);
        log(QStringLiteral("stdout"),
            QStringLiteral("auto-assigned macvlan IP: %1")
                .arg(target.section(QLatin1Char(':'), 0, 0)));
        const int s = mirrorArgv.indexOf(QStringLiteral("-s"));
        if (s >= 0 && s + 1 < mirrorArgv.size()) mirrorArgv[s + 1] = target;
    }
    // The host route the macvlan target rides. Here rather than in the deploy chain because
    // `reconnect` never runs the chain, and a reboot drops the interface — so the one place that
    // covers every path to an adb handshake is immediately before it (bd remora-400). Not fatal on
    // its own: a host with a second NIC on the same LAN reaches the container without it, so let
    // the connect attempt be the judge and keep the reason in the log for when it is not.
    QString hostRouteNote;
    if (ctx.backend == Backend::Bare) {
        const HostRouteResult hr = ensureMacvlanHostRoute(sp, ctx.rc, log);
        if (!hr.detail.isEmpty()) {
            log(hr.ok ? QStringLiteral("stdout") : QStringLiteral("stderr"), hr.detail);
            // Carried into the step's own detail when it is worth seeing: ConsoleSink prints step
            // details and nothing else, so a change to the operator's host networking that lived
            // only in the log would be invisible from the CLI.
            if (hr.notable) hostRouteNote = hr.detail;
        }
    }
    // Before the first adb handshake: a fresh /data has no authorized keys, and the dialog adb
    // would ask for cannot be answered without the mirror adb is gating.
    seedAdbKey(sp, ctx, log);
    bool connected = false;
    // Did adbd ever ANSWER? `adb connect` reporting "connected to" — or a state of "offline",
    // which only a device that replied can produce — proves the endpoint is reachable at layer 3.
    // Kept because the failure message below is otherwise free to blame the network for a device
    // that was merely still starting (bd remora-c3h).
    bool endpointAnswered = false;
    // Time spent waiting for an answering-but-offline device, on its own budget. play-spoof and a
    // first boot both restart adbd immediately before this step, so the opening seconds of a cold
    // connect legitimately look like this: the endpoint answers, the device says "offline", and it
    // comes good on its own. Measured once at 30s of that, then a failure that recommended a
    // macvlan shim to a host whose shim was working and whose ping was 0.03 ms. Retrying is right;
    // spending the REACHABILITY budget on it is not.
    constexpr int kOfflineBudgetMs = 90000;
    int offlineWaitedMs = 0;
    bool announcedOffline = false;
    for (int i = 0; i < retries; ++i) {
        const ProcResult conn = sp.run({"adb", "connect", target}, {}, log);
        const ProcResult state = sp.run({"adb", "-s", target, "get-state"}, {}, log);
        if (state.out.trimmed() == QLatin1String("device")) {
            connected = true;
            break;
        }
        const bool offline = state.out.contains(QLatin1String("offline"))
                             || state.stderrTail.contains(QLatin1String("offline"));
        if (offline || conn.out.contains(QLatin1String("connected to"))) endpointAnswered = true;
        sp.run({"adb", "disconnect", target}, {}, log);
        // STALE TRANSPORT. `adb connect` to an endpoint the server already holds returns "already
        // connected" WITHOUT re-establishing anything, so when that held transport is dead the
        // whole retry budget burns on it: connect → "already connected" → get-state "offline" →
        // disconnect → repeat, fifteen times, and the failure then blames the network. The
        // disconnect above does not settle it either — a live mirror still attached to the same
        // -s target re-registers the endpoint before the next attempt.
        //
        // `adb reconnect offline` is the escape: it forces the server to tear down and re-open
        // every offline transport, which is the one operation that gets a fresh handshake here.
        // Gated on the state actually reading "offline" (a missing device reports differently), so
        // an ordinary not-yet-booted device still takes the plain retry path.
        //
        // A pinned macvlan address makes this the NORMAL case rather than a rarity: the adb
        // endpoint is now stable across container recreations, so the transport for a container
        // that has just been destroyed and rebuilt looks identical to the live one (bd remora-400).
        if (offline) {
            log(QStringLiteral("stderr"),
                QStringLiteral("adb holds a stale transport for %1 (offline) — forcing a "
                               "re-handshake").arg(target));
            sp.run({"adb", "reconnect", "offline"}, {}, log);
            sp.run({"adb", "connect", target}, {}, log);
            if (sp.run({"adb", "-s", target, "get-state"}, {}, log).out.trimmed() ==
                QLatin1String("device")) {
                connected = true;
                break;
            }
        }
        // Still offline after the re-handshake: adbd is answering and simply is not ready. Wait it
        // out WITHOUT consuming a try — the tries are the reachability budget, and reachability is
        // exactly what is not in question here.
        // …but not before the loop's own remedies have had their turn. `--i` below skips the rest
        // of the body, so waiting from the first attempt meant the netfix reapply (i==0) and the
        // direct-adbd fallback (i==2) never ran — and a device that is offline BECAUSE the fwd shim
        // died then sat out the full budget instead of being fixed in two seconds. Measured: a
        // connect that took 48.9s where the remedies would have converged in single digits.
        // i >= 3 is exactly "both remedies have been tried".
        if (offline && i >= 3 && offlineWaitedMs < kOfflineBudgetMs) {
            if (!announcedOffline) {
                announcedOffline = true;
                log(QStringLiteral("stdout"),
                    QStringLiteral("adbd is answering but the device is still offline (it restarts "
                                   "with the framework) — waiting for it to come online"));
            }
            const int step = qMax(retryDelayMs, 1000);
            offlineWaitedMs += step;
            sp.waitMs(step);
            --i;
            continue;
        }
        // First failure: the usual culprit after a container/Android restart is a dead fwd shim —
        // reapply the netfix before burning the remaining retries.
        if (i == 0) {
            log(QStringLiteral("stderr"),
                QStringLiteral("adb not ready — reapplying netfix (fwd may have died)"));
            reapplyNetfix(sp, ctx, log);
        }
        // Third failure on bare: stop trusting the forwarder and go to adbd DIRECTLY. The
        // localhost target rides docker-proxy -> the fwd shim -> adbd, and the shim has proven
        // capable of dying in ways no probe sees coming — pgrep counts its orphaned relay
        // children as alive, the container's own `ss` misreports listeners, and one cold boot
        // stayed unreachable through six netfix re-runs and 280 s of retries while adbd itself
        // answered instantly on the container IP over plain IPv4 (bd remora-e5x.16). The shim
        // exists for images whose adbd binds IPv6-only; when adbd takes the direct connection,
        // this image is not one of them, so switch the session to the container IP and move on.
        // Port 5555 here is adbd's OWN port — containerAdbPort is the shim's.
        if (ctx.backend == Backend::Bare && i == 2 && target.startsWith(QLatin1String("localhost"))) {
            const QString cip =
                sp.run({"docker", "inspect", "-f",
                        "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}",
                        ctx.rc.containerName},
                       {}, log)
                    .out.trimmed();
            if (!cip.isEmpty()) {
                const QString direct = cip + QStringLiteral(":5555");
                sp.run({"adb", "connect", direct}, {}, log);
                if (sp.run({"adb", "-s", direct, "get-state"}, {}, log).out.trimmed() ==
                    QLatin1String("device")) {
                    log(QStringLiteral("stdout"),
                        QStringLiteral("forwarder unreachable but adbd answers directly — "
                                       "switching this session to %1")
                            .arg(direct));
                    target = direct;
                    const int si = mirrorArgv.indexOf(QStringLiteral("-s"));
                    if (si >= 0 && si + 1 < mirrorArgv.size()) mirrorArgv[si + 1] = target;
                    connected = true;
                    break;
                }
                sp.run({"adb", "disconnect", direct}, {}, log);
            }
        }
        sp.waitMs(retryDelayMs);
    }
    if (!connected) {
        // A macvlan container lives on the LAN behind the docker host's NIC, so whether THIS host
        // can reach it depends on this host's routing — something every readiness probe above is
        // blind to. `remora check` can be fully green, the container healthy with its IP up, and
        // the connect still die on "No route to host" because the host picked the wrong interface
        // onto the shared subnet. Diagnose it here rather than reporting a bare retry count: the
        // fix is a host route, and nothing else in the chain will ever say so (bd remora-4ei.13).
        const QString ip = target.section(QLatin1Char(':'), 0, 0);
        QString hint;
        if (!ip.isEmpty() && ip != QLatin1String("localhost")) {
            const QString subnet = ip.section(QLatin1Char('.'), 0, 2) + QStringLiteral(".0/24");
            const int nRoutes = sp.run({"sh", "-c",
                                        QStringLiteral("ip route show %1 2>/dev/null | wc -l")
                                            .arg(subnet)})
                                    .out.trimmed()
                                    .toInt();
            const QString via = sp.run({"sh", "-c", QStringLiteral("ip route get %1 2>/dev/null | "
                                                                   "head -1").arg(ip)})
                                    .out.trimmed();
            // The WHOLE table, not `head -1`. A host running the macvlan shim always has a FAILED
            // entry on the parent interface — a host cannot reach its own macvlan children over
            // the parent, which is the very limitation the shim exists to work around — and it
            // sorts first. Taking one line therefore diagnosed "layer 2 is dead" on a host whose
            // shim entry, two lines further down, was REACHABLE with the right lladdr, and printed
            // the full install-the-shim remedy at someone who already had it (bd remora-c3h).
            //
            // Layer 2 resolved if ANY entry resolved: an lladdr is an answer, whichever interface
            // produced it.
            const QString neigh =
                sp.run({"sh", "-c", QStringLiteral("ip neigh show %1 2>/dev/null").arg(ip)})
                    .out.trimmed();
            const bool arpDead = !neigh.contains(QLatin1String("lladdr"));
            // …and none of this is a diagnosis at all when adbd ANSWERED. A device that replied
            // "offline" is reachable by construction, so whatever went wrong is not the network.
            if (arpDead && !endpointAnswered) {
                hint = QStringLiteral(" — this host cannot reach %1 at layer 2 (ARP %2). Route: %3.")
                           .arg(ip, neigh.isEmpty() ? QStringLiteral("no entry") : neigh, via);
                if (nRoutes > 1)
                    hint += QStringLiteral(
                                " THIS HOST HAS %1 ROUTES to %2 — it is almost certainly using the "
                                "wrong one. Pin the container: sudo ip route add %3/32 dev <iface> "
                                "src <that iface's IP>, choosing the interface that physically "
                                "reaches the docker host.")
                                .arg(nRoutes)
                                .arg(subnet, ip);
                else
                    hint += QStringLiteral(" The container is on a macvlan behind the docker "
                                           "host's NIC; check that this host's path to %1 "
                                           "reaches it.")
                                .arg(subnet);
            }
            // BARE + macvlan is the one case with a known, structural cause, so name it instead of
            // leaving the operator with the generic routing advice above: a macvlan child and its
            // own parent interface cannot exchange traffic, and on bare the docker host IS this
            // host. It often works anyway — a second NIC on the same LAN reaches the container
            // through the switch — which is exactly why the failure is baffling when it does not
            // (bd remora-400).
            if (ctx.backend == Backend::Bare && !endpointAnswered &&
                ctx.rc.networkMode == QLatin1String("macvlan")) {
                // Same blocks the automatic shim would route — one definition, so the manual
                // remedy and the automatic path can never disagree about what needs a route.
                QStringList blocks = macvlanShimRoutes(ctx.rc);
                if (blocks.isEmpty()) blocks << subnet;
                hint += QStringLiteral(
                            " NOTE: %1 is a macvlan container on THIS host's own NIC (%2), and a "
                            "host cannot reach its own macvlan children over the parent interface. "
                            "Install the shim once — sudo sh vendor/host-prereqs/macvlan-shim/"
                            "remora-macvlan-shim install %2 %3 — or use network_mode=bridge.")
                            .arg(ip, ctx.rc.macvlanParent.value_or(QStringLiteral("<parent>")),
                                 blocks.join(QLatin1Char(' ')));
            }
        }
        if (endpointAnswered && hint.isEmpty())
            return StepResult::fail(
                QStringLiteral("adbd on %1 answered but the device never came online (waited %2s). "
                               "The endpoint is reachable — this is the device not finishing its "
                               "start, not a network problem. Check the container's logcat.")
                    .arg(target)
                    .arg(offlineWaitedMs / 1000));
        return StepResult::fail(QStringLiteral("adb device %1 not ready after %2 tries%3")
                                    .arg(target)
                                    .arg(retries)
                                    .arg(hint));
    }

    // adb up ≠ Android up: adbd answers long before system_server registers its services (a
    // first boot spends minutes in dex2oat). Launching the mirror into that window kills its server
    // ("Can't find service: settings", instant demuxer disconnects) and burns every retry, so
    // gate on sys.boot_completed here. After a watch-boot animation this passes on the first probe.
    {
        // Wait for boot_completed=1. When bootStableMs>0 (a deploy path, where a boot may have just
        // happened), don't stop at the FIRST =1: a fresh /data first boot reaches boot_completed=1,
        // then runs its setup and REBOOTS (factory_reset -> ota) seconds later, and a mirror attached
        // to that doomed first boot dies the moment it reboots ("connected but mirror immediately
        // closed on first launch"). So require =1 to HOLD for bootStableMs, and if it drops back
        // (reboot in progress — the prop resets and adb goes away) reset and resume waiting, re-
        // asserting the adb link. Warm reconnects pass bootStableMs=0 and stop on the first =1.
        //
        // 15s -> 8s -> 4s, and the shrinking is not impatience: this hold was the whole defence
        // against a late reboot, and it no longer is. play-spoof used to bounce the framework on
        // every cold deploy and that was by far the likeliest late restart — gone as of bd
        // remora-v8h, which arms the injector from a post-fs-data hook so the container boots once.
        // What remains is first-boot module work, and waitFirstBootSettled holds the HAND-OFF for
        // exactly that, measuring the work itself rather than guessing at a duration. 4s is what is
        // left over for a genuine factory-reset -> ota bounce; below that it stops being a hold at
        // all (bd remora-tdz).
        // Polled every second, not every three. The interval is not a safety margin — the BUDGET
        // is — and on a healthy boot every bit of it is overshoot: a 3 s step also made the
        // stability hold land on a 3 s multiple, so an 8 s hold cost 9 s.
        constexpr int kBootPollMs = 1000;
        // …and skipped entirely once this /data has proved it does not do that. See
        // dataHasBootedBefore: the hazard is a property of a NEW data dir, so an established one
        // should stop into paying for it. Costs one `test -f`; saves the whole hold.
        int holdMs = bootStableMs;
        if (holdMs > 0 && dataHasBootedBefore(sp, ctx, log)) {
            holdMs = 0;
            log(QStringLiteral("stdout"),
                QStringLiteral("this /data has booted cleanly before — skipping the boot-stability "
                               "hold"));
        }
        bool booted = false;
        int stable = 0;
        for (int waited = 0; waited <= bootWaitMs; waited += kBootPollMs) {
            const ProcResult bc = sp.run(
                {"adb", "-s", target, "shell", "getprop", "sys.boot_completed"}, {}, log);
            const bool up = bc.out.trimmed() == QLatin1String("1");
            if (up && holdMs <= 0) {
                booted = true;
                break;
            }
            if (up) {
                stable += kBootPollMs;
                if (stable >= holdMs) {
                    booted = true;
                    break;
                }
            } else if (stable > 0) {
                log(QStringLiteral("stderr"),
                    QStringLiteral("device rebooted during first-boot setup — waiting for it to "
                                   "settle before attaching the mirror"));
                stable = 0;
                sp.run({"adb", "connect", target}, {}, log);  // the reboot drops the adb link
            }
            if (waited >= bootWaitMs) break;
            if (waited % 15000 == 0 && stable == 0)
                log(QStringLiteral("stdout"),
                    QStringLiteral("Android still booting (%1s / up to %2s — a first boot optimizes "
                                   "apps and takes minutes)…")
                        .arg(waited / 1000)
                        .arg(bootWaitMs / 1000));
            sp.waitMs(kBootPollMs);
        }
        if (!booted)
            return StepResult::fail(
                QStringLiteral("Android did not finish booting within %1s — the container is up and "
                               "adb answers, but system_server never came ready. Check the "
                               "container's logcat.")
                    .arg(bootWaitMs / 1000));

            // Device tune (animation scales) — idempotent, and deliberately not checked so a failed
        // settings write can never block the connect.
        sp.run(buildDeviceTuneArgv(ctx.rc, target), {}, log);
    }

    // sys.boot_completed ≠ pixels on screen: on the xe VF the launcher's first composited frame
    // lands ~10-15s later, so a mirror attached at boot_completed opens onto black (and yanks the
    // hand-off out from under the still-animating boot-anim viewer). Gate on what SurfaceFlinger
    // actually outputs: a PNG screencap of an all-black screen is a few KB, the drawn home screen
    // hundreds of KB — sized on-device (wc -c), nothing transferred. Bounded: a legitimately-dark
    // screen must delay the connect, not block it. The boot-anim viewer polls the same signal, so
    // it exits right as the mirror below launches.
    if (waitLit) {
        constexpr int kLitMinBytes = 100000;
        constexpr int kLitWaitMs = 60000;
        // 1s, and NOT finer — tried, measured, reverted. This probe is a full screencap of a
        // 3760x1992 screen, PNG-encoded on the device, so it is nothing like the cheap host-side
        // probes elsewhere in this file: at 250ms the polls stack up faster than they complete and
        // the gate got SLOWER, 2.3s -> 3.1-4.2s, while loading the very device it is waiting on.
        // The interval here is not overshoot to be trimmed; it is what keeps the probe from
        // competing with the thing it measures.
        constexpr int kLitPollMs = 1000;
        // Each probe is bounded, because the 60s budget above counts LOOP ITERATIONS and assumes
        // every call returns. screencap can wedge indefinitely on some GPU stacks (observed on the
        // Venus/NVIDIA path, bd remora-4ei.65), and one hung call then blocks `remora up` forever
        // at the connect step rather than delaying it — the exact failure this gate promises not to
        // cause.
        constexpr int kLitCallTimeoutS = 8;
        for (int waited = 0; waited < kLitWaitMs; waited += kLitPollMs) {
            const ProcResult sc =
                sp.run({"timeout", QString::number(kLitCallTimeoutS), "adb", "-s", target, "shell",
                        "screencap -p 2>/dev/null | wc -c"},
                       {}, log);
            bool isNum = false;
            const int bytes = sc.out.trimmed().toInt(&isNum);
            if (isNum && bytes > kLitMinBytes) break;
            // 124 = `timeout` killed it. Polling a wedged screencap again cannot help, and the
            // mirror does not depend on it, so stop gating and let the window open.
            if (sc.rc == 124) {
                log(QStringLiteral("stdout"),
                    QStringLiteral("screencap probe timed out — connecting without the lit-screen "
                                   "gate"));
                break;
            }
            if (waited == 0)
                log(QStringLiteral("stdout"),
                    QStringLiteral("booted — waiting for the first drawn frame (launcher warmup)…"));
            sp.waitMs(kLitPollMs);
        }
    }

    // Launch the mirror, relaunching if it dies immediately. The first launch frequently exits during
    // guest codec/buffer setup (a transient gralloc/encoder-startup race on the the guest side) — a
    // relaunch almost always succeeds, so retry instead of failing the whole connect. Re-assert the
    // adb link before each retry in case a dropped connection (e.g. a died fwd shim) is what killed
    // the mirror.
    // When the deploy pre-spawned the one-window boot splash and it is still alive, ADOPT it as
    // the mirror: it has been showing this very boot in the window the mirror will use, and its
    // mirroring session starts the moment its animation hands off. Spawning another mirror here
    // would be a second window AND a second client sharing the VF encoder's bandwidth.
    // Recorded, not just computed: when this comes out false while the splash is in fact alive and
    // mirroring, the window visibly drops and reopens, and until now nothing said which of the
    // three conditions decided it (bd remora-eoy.10). Surfaced in the step detail because
    // ConsoleSink overrides state() only and drops line() — a log-only note is invisible from the
    // CLI and the desktop launcher, which is exactly where the cold path runs.
    // Predecessors, captured BEFORE anything is spawned. Reaping by "everything except the pid
    // we started" looked simpler but is not sound: spawnDetached returns the pid of cmd[0], and
    // scopedArgv wraps the mirror in systemd-run, so that pid is not reliably the mirror's — an
    // exclusion built on it can kill the very window it just opened. A pid that already existed
    // here is a predecessor by definition, whatever the launcher does with pids.
    // Asked as a FUNCTION, not captured once. The snapshot below is taken on entry, but a cold
    // connect then spends minutes in boot-wait, and any Remora process can open a mirror for this
    // target in that window — the desktop launcher, a second GUI Connect, a `remora up`. A
    // predecessor list frozen at entry cannot see those, so the post-spawn reap walked straight
    // past them and left two clients pulling from the one host encoder: the black flickering of
    // bd remora-e5x.19, arriving by a route that bug's fix did not cover (bd remora-51j —
    // confirmed live, two mirror pids from two different Remora processes, 2 ESTAB on the
    // encoder's video socket).
    // Husks first, before anything is counted or spawned. One is provably dead — no tunnel, no
    // server, nothing composing — so unlike every other reap here this one cannot cost a visible
    // drop, and doing it now keeps husks out of the predecessor snapshot below AND frees the
    // encoder consumer slot before the new mirror attaches (bd remora-8d7).
    const int huskCount = ctx.fullscreenDisplay ? 0 : reapMirrorHusks(sp, ctx, target, log);
    const auto liveMirrors = [&]() -> QStringList {
        if (ctx.fullscreenDisplay) return {};
        const QString pat = mirrorTitlePattern(target, ctx.windowTitle);
        if (pat.contains(QLatin1Char('\''))) return {};
        QStringList pids;
        for (const QString &p :
             sp.run({"sh", "-c", QStringLiteral("pgrep -f '%1' || true").arg(pat)}, {}, log)
                 .out.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (!p.trimmed().isEmpty()) pids << p.trimmed();
        return pids;
    };
    const QStringList priorMirrors = liveMirrors();
    // Before deciding anything about the splash: let first-boot module work finish, so the
    // hand-off does not land moments before a framework restart kills the window.
    const QString settleNote =
        ctx.fullscreenDisplay ? QString() : waitFirstBootSettled(sp, ctx, target, log);
    const bool splashAlive = ctx.splash && sp.detachedAlive(*ctx.splash);
    const bool adoptSplash = !ctx.fullscreenDisplay && splashAlive;
    const QString adoptWhyNot =
        adoptSplash            ? QString()
        : ctx.fullscreenDisplay ? QStringLiteral("fullscreen/new-display")
        : !ctx.splash           ? QStringLiteral("no splash was spawned")
                                : QStringLiteral("splash pid %1 not alive").arg(ctx.splash->pid);
    // Kill both ends of any stale session before launching: the local mirror (it is
    // detached and survives Remora — a second client shares the VF encoder's bandwidth) AND
    // the guest-side server (which leaks the VF encoder context if it lingers).
    // Skip both for a desktop/app launch (--new-display): it's an extra window, so it must not
    // reap the running mirror's local process or its guest server.
    if (!ctx.fullscreenDisplay) {
        // The pattern matches the SPLASH's own argv — the splash is the mirror argv plus the
        // boot-animation flags, same -s and same --window-title. Gating this on adoptSplash was
        // therefore only ever as correct as that test, and on a cold path the test comes out false
        // while the splash is alive and mirroring: Remora SIGTERMs the window it just opened, and
        // the user sees it vanish and reopen (bd remora-eoy.10, captured — "Terminated"
        // in the splash's own log 21s into a healthy session).
        // Gate on the splash EXISTING instead. startSplash reaps true stales immediately before
        // spawning it, so once a splash exists the only thing this pattern can still match is that
        // splash. A wrong adoption test now costs a redundant spawn instead of a visible drop.
        if (!ctx.splash)
            sp.run({"pkill", "-f", mirrorTitlePattern(target, ctx.windowTitle)}, {}, log);
        // The guest-side half of this reap is gone with killStaleServer — the agent
        // is one init service, not a per-session server, so there is no guest process here to
        // leak. Its history is worth keeping because it is why the HOST-side reap above is gated
        // on `!ctx.splash` rather than on an adoption test: on a cold path the splash owns a live
        // session by the time doConnect reaches here, and reaping it logged "WARN: Device
        // disconnected" on a healthy 20s-old session and cost two replacement spawns (bd
        // remora-eoy.10, captured).
    }
    const QMap<QString, QString> spawnEnv = mirrorSpawnEnv(ctx, target, mirrorArgv);
    // Tell a splash still in its status phase (a deploy that booted nothing) to attach now —
    // without this it would wait forever for a boot marker that never comes. A splash already
    // animating ignores the file; there the probe's booted+lit signal drives the hand-off.
    if (adoptSplash) appendStatusLine(ctx.statusPath, QStringLiteral("REMORA_ATTACH"));
    // A running mirror process is not proof of a working session — the guest encoder can fail
    // (MediaCodec.start), the demuxer can drop, the codec can be unopenable. These leave the
    // process alive but blank, so "connected" was reported while nothing showed. Treat them as
    // a failed attempt: kill this instance and retry (encoder contention usually clears).
    // The server-side strings pass through the client's [server] relay verbatim; "mirror: fatal"
    // is the in-house client's own unrecoverable-error prefix (first-frame timeout, framing
    // error, hw download failure) — one entry covers the whole family.
    static const char *const kFatal[] = {"Capture/encoding error", "Could not create",
                                         "Demuxer error", "Server connection failed",
                                         "could not open codec", "MediaCodec",
                                         "mirror: fatal"};
    // When the mirror takes its video from the host encoder, a dead encoder is the likeliest reason
    // the mirror found nothing — and NOTHING in its own output says so. The tail captured below is
    // whatever last wrote to the shared detached log, which in the observed failure was an
    // unrelated render-server line ("vtest_client_dispatch_commands: client context created."):
    // it sent the reader at Venus and at the mirror while the actual cause — the encoder had died out
    // of VRAM — went unmentioned entirely (bd remora-e5x.13). Checked at failure time rather than
    // cached, because the encoder can die DURING the retries.
    // How many consumers the host encoder is actually SERVING, or -1 when that cannot be told.
    // Measured from the socket rather than from a log line: it is the real condition, and the
    // detached log is shared by every process in the launch so a match there proves less than it
    // appears to. -1 (no `ss`) deliberately reads as "unknown" and never as a failure — refusing a
    // working mirror because a diagnostic tool is missing would be a worse bug than the one this
    // closes.
    const auto servedConsumers = [&]() -> int {
        const QString cmd =
            QStringLiteral("command -v ss >/dev/null 2>&1 || { echo -1; exit 0; }; "
                           "ss -xH 2>/dev/null | grep -F -- '%1' | grep -c ESTAB")
                .arg(ctx.rc.hostEncodeVideoSocket);
        bool ok = false;
        const int n = sp.run({"sh", "-c", cmd}, {}, log).out.trimmed().toInt(&ok);
        return ok ? n : -1;
    };
    const auto encoderNote = [&]() -> QString {
        if (!ctx.rc.hostEncode || !ctx.rc.hostEncodeDrivesMirror) return QString();
        const bool alive =
            sp.run({"sh", "-c",
                    QStringLiteral("ps -C remora-frame-encoder -o args= 2>/dev/null | "
                                   "grep -qF -- '%1'").arg(ctx.rc.hostEncodeFrameSocket)},
                   {}, log)
                .rc == 0;
        if (!alive)
            return QStringLiteral(
                "\nNOTE: the host encoder for this profile is NOT running, and with host_encode on "
                "it is where the mirror's video comes from — so this is an encoder failure, not a "
                "mirror one. It most often dies for want of GPU memory; check `nvidia-smi` for free "
                "VRAM and for abandoned remora-frame-encoder processes holding it.");
        // Running but serving nobody is the OTHER shape, and it looks identical from the mirror's side.
        // Naming it separately matters because the remedy is different: nothing is wrong with the
        // GPU, the mirror simply never got a slot.
        if (servedConsumers() == 0)
            return QStringLiteral(
                "\nNOTE: the host encoder is running but is serving NO consumer, so the mirror "
                "would have received no video however long it waited. If several windows are open "
                "against this profile, an encoder predating the fan-out change serves only the "
                "first — check that the installed remora-frame-encoder is current.");
        return QString();
    };
    // The encoder's OWN log, resolved through /proc while it is demonstrably alive (the step
    // above just ensured it). Never guessed from /tmp mtimes: the newest remora-detached-*.log
    // is whatever spawned LAST — usually the client this very step starts — while the encoder
    // may be hours older; that is the shared-log trap of bd remora-e5x.13 wearing a new hat.
    // Resolved ONCE, up front, because after an encoder death readlink has nothing to read and
    // the path is needed precisely then, to tail the evidence (bd remora-7ps). The baseline
    // counts served-first-packet lines BEFORE anything is spawned, once per connect: any
    // attempt that gets served bumps the count past it, and attempts rejected earlier were by
    // definition not served.
    QString encoderLog;
    int packetBaseline = -1;
    if (ctx.rc.hostEncode && ctx.rc.hostEncodeDrivesMirror) {
        encoderLog =
            sp.run({"sh", "-c",
                    QStringLiteral("pid=$(ps -C remora-frame-encoder -o pid=,args= 2>/dev/null | "
                                   "grep -F -- '%1' | awk '{print $1; exit}'); "
                                   "[ -n \"$pid\" ] && readlink /proc/$pid/fd/2")
                        .arg(ctx.rc.hostEncodeFrameSocket)},
                   {}, log)
                .out.trimmed();
        if (!encoderLog.isEmpty()) {
            bool ok = false;
            const int n = sp.run({"sh", "-c",
                                  QStringLiteral("grep -c 'got its first decodable packet' %1 "
                                                 "2>/dev/null")
                                      .arg(encoderLog)},
                                 {}, log)
                              .out.trimmed()
                              .toInt(&ok);
            if (ok) packetBaseline = n;
        }
    }
    // The publisher gate (bd remora-r6au). An encoder can pass every ensure-check — process
    // alive, socket bound, fresh, even state=ok — with NOTHING composing behind it: the
    // composer's reconnect only ticks per COMPOSED frame, so a device whose screen slept while
    // the encoder restarted never re-attaches on its own, and every mirror spawned below would
    // starve to its ~30 s first-frame timeout with "connected" already printed. The encoder
    // publishes its attach counts precisely for this probe; one that predates them parses as -1
    // and passes untouched. Fail-open (the bd remora-82x lesson): a wake composes within ~1 s
    // when the screen was off — the observed cascade — while a screen that is on-but-static
    // self-heals on the user's first input, so expiry warns rather than refuses.
    if (ctx.rc.hostEncode && ctx.rc.hostEncodeDrivesMirror) {
        const auto encoderStatus = [&]() -> std::optional<EncoderStatus> {
            const ProcResult st =
                sp.run({"sh", "-c",
                        QStringLiteral("cat %1 2>/dev/null")
                            .arg(hostEncodeStatusFileFor(ctx.rc.containerName))},
                       {}, log);
            return parseEncoderStatus(st.out);
        };
        const auto publishers = [&]() -> int {
            const auto es = encoderStatus();
            return es ? es->publishers : -1;
        };
        // Heal the black stream BEFORE spawning anything (bd remora-pobz item 2). The state: the
        // guest's RenderEngine lost its Vulkan device at the VRAM ceiling, SurfaceFlinger kept
        // PRESENTING client targets it no longer renders into, and the composer publishes them
        // faithfully — so every gate below passes and the window is black. Handing the user a
        // mirror in that state is what left this to be diagnosed by hand, twice.
        //
        // TWO independent signals, and the restart needs BOTH. black_stream is the encoder saying
        // the pixels are flat NOW; the guest's mCreateDevice assertion storm is the reason, and is
        // what separates this from a legitimately dark screen — a game's black loading scene trips
        // the first signal honestly and must not cost the user their session. Either alone only
        // warns.
        const auto es = encoderStatus();
        if (es && es->blackStream == 1) {
            // Recent lines only, and a run rather than one: the ring buffer keeps old aborts from
            // an episode already recovered, and a single assert is not the storm this looks for.
            const int asserts =
                containerExec(sp, ctx,
                              QStringLiteral("logcat -d -t 3000 2>/dev/null | grep -c mCreateDevice"),
                              log)
                    .out.trimmed()
                    .toInt();
            if (asserts >= 3) {
                log(QStringLiteral("stdout"),
                    QStringLiteral("the stream is flat and the guest shows %1 RenderEngine device "
                                   "failures — SurfaceFlinger is presenting targets it no longer "
                                   "renders into. Restarting the guest framework (bd remora-pobz)")
                        .arg(asserts));
                containerExec(sp, ctx, QStringLiteral("stop && start"), log);
                // The framework restart is the documented ~15 s recovery; wait for the composer to
                // come back rather than racing the mirror against a booting SurfaceFlinger.
                constexpr int kHealWaitMs = 30000, kHealPollMs = 1000;
                for (int waited = 0; waited < kHealWaitMs; waited += kHealPollMs) {
                    sp.waitMs(kHealPollMs);
                    const auto now = encoderStatus();
                    if (now && now->publishers > 0 && now->blackStream != 1) break;
                }
            } else {
                log(QStringLiteral("stderr"),
                    QStringLiteral("the encoder reports a flat/black stream, but the guest shows no "
                                   "RenderEngine failure — if the window is black rather than the "
                                   "content, restart the framework: docker exec %1 sh -c 'stop && "
                                   "start'")
                        .arg(ctx.rc.containerName));
            }
        }
        if (publishers() == 0) {
            log(QStringLiteral("stdout"),
                QStringLiteral("host encoder has no frame publisher — waking the device so the "
                               "composer re-attaches (bd remora-r6au)"));
            // A composed frame is both the retry tick and the proof, so force one: KEYCODE_WAKEUP
            // (224) lights a sleeping screen (a compose) and is a no-op on one already lit.
            containerExec(sp, ctx, QStringLiteral("input keyevent 224"), log);
            constexpr int kPublisherWaitMs = 10000;  // a composed frame away, not a boot away
            constexpr int kPublisherPollMs = 500;
            int pubs = 0;
            for (int waited = 0; waited < kPublisherWaitMs; waited += kPublisherPollMs) {
                sp.waitMs(kPublisherPollMs);
                if ((pubs = publishers()) != 0) break;
            }
            if (pubs == 0)
                log(QStringLiteral("stderr"),
                    QStringLiteral(
                        "still no frame publisher after the wake — the mirror will show nothing "
                        "until the device composes a frame (touch it, or `docker restart %1`)")
                        .arg(ctx.rc.containerName));
        }
    }
    const int attempts = mirrorRetries > 0 ? mirrorRetries : 1;
    for (int a = 1; a <= attempts; ++a) {
        // Mirrors that appeared SINCE this connect started. They are ours by no definition:
        // nothing has been spawned yet on this attempt, and an adopted splash predates the entry
        // snapshot, so it is never in here. Re-asked per attempt so a window opened during the
        // retries is caught too.
        QStringList intruders;
        for (const QString &p : liveMirrors())
            if (!priorMirrors.contains(p)) intruders << p;
        // First attempt adopts the live splash; a splash that died (or later attempts after a
        // broken adoption was killed) falls back to spawning a plain mirror.
        const Detached det = (a == 1 && adoptSplash)
                                 ? *ctx.splash
                                 : sp.spawnDetached(mirrorArgv, spawnEnv);
        if (a == 1 && adoptSplash)
            log(QStringLiteral("stdout"),
                QStringLiteral("boot splash pid %1 is taking over as the mirror").arg(det.pid));
        sp.waitMs(livenessDelayMs);
        const bool alive = sp.detachedAlive(det);  // once — the fake counts calls
        const QString said =
            sp.run({"sh", "-c",
                    "tail -n 8 $(ls -t /tmp/remora-detached-*.log 2>/dev/null | head -1) "
                    "2>/dev/null"},
                   {}, log)
                .out.trimmed();
        bool broken = false;
        // WHY this attempt was rejected, in Remora's own words. Without it the retry line below
        // reported "the mirror exited immediately" for a process Remora had just killed itself, and
        // appended the last line of the SHARED detached log as if it were the reason — which on a
        // host-encode profile is whatever the frame encoder happened to print (bd remora-82x: a
        // starved-mirror kill was reported as "mirror exited immediately: [encoder] 2 display
        // frames dropped", pointing at VRAM when nothing was wrong with the GPU at all).
        QString brokenWhy;
        for (const char *sig : kFatal)
            if (said.contains(QLatin1String(sig))) {
                broken = true;
                brokenWhy = QStringLiteral("its log carries \"%1\"").arg(QLatin1String(sig));
                break;
            }
        // A STARVED MIRROR IS ALIVE. When the video comes from the host encoder, a client that
        // connected but is not being served sits blocked waiting for a stream header — so it passes
        // `alive`, logs nothing, and the connect reports ok while no window ever appears. That is
        // the whole false-ok (bd remora-e5x.14): the check answered "is the process running" when
        // the question was "is it receiving video". Treat it as a failed attempt so the retry loop
        // gets a chance — a consumer that lost a race for a slot may well win the next one.
        int handoffWaitedMs = 0;
        if (alive && !broken && ctx.rc.hostEncode && ctx.rc.hostEncodeDrivesMirror) {
            int served = servedConsumers();
            // An ADOPTED SPLASH is not starved — it is not connected YET, and the difference is
            // invisible to this probe. It was told to attach one livenessDelayMs ago and still has
            // to notice the marker, finish its outro, tear down its renderer, start its device
            // session and open the video socket: seconds, not milliseconds. Sampled once at t+1s it therefore
            // ALWAYS reads as serving nobody, so this check killed the very window the hand-off
            // had just been arranged for, and the retry spawned a replacement — the other half of
            // the drop-then-reappear in bd remora-82x, confirmed on a cold run whose splash log
            // stops dead after "Boot animation: pulled" with the marker already written.
            // Wait for it to arrive instead of judging it on arrival.
            // Its OWN budget, deliberately not mirrorHoldMs. The two answer different questions —
            // "did the window survive?" versus "has the mirror finished arriving?" — and they have
            // different natural sizes: arriving means finishing the animation, tearing down the
            // splash renderer, starting the device session and opening the video socket, measured
            // at ~14.5 s on a cold connect. Sharing the liveness hold's number meant that shortening
            // the hold to 3 s silently started killing every adopted splash for being "starved"
            // three seconds into a fourteen-second arrival.
            constexpr int kConsumerArrivalMs = 30000;
            // Polled at 200ms, not 1s. The probe is a `ss` grep on the HOST — microseconds — so a
            // one-second interval bought nothing and cost the difference between the consumer
            // arriving and us noticing. Measured across four cold connects, the post-attach phase
            // came out at 4.0 / 5.0 / 6.0 / 6.0s: quantised to whole seconds, and that quantisation
            // WAS the deploy's entire run-to-run variance (every other phase held to within 80ms).
            constexpr int kArrivalPollMs = 200;
            if (served == 0 && a == 1 && adoptSplash) {
                for (; handoffWaitedMs < kConsumerArrivalMs && served == 0;
                     handoffWaitedMs += kArrivalPollMs) {
                    sp.waitMs(kArrivalPollMs);
                    served = servedConsumers();
                }
            }
            if (served == 0) {
                broken = true;
                brokenWhy = QStringLiteral("the host encoder is serving nobody %1s after it "
                                           "attached")
                                .arg((livenessDelayMs + handoffWaitedMs) / 1000);
                log(QStringLiteral("stderr"),
                    QStringLiteral("mirror attached but the host encoder is serving nobody — no video "
                                   "would reach the window; retrying"));
            }
        }
        if (alive && !broken) {
            // alive and no fatal error: surface any benign ERROR (audio, icon) but succeed
            if (said.contains(QLatin1String("ERROR")))
                for (const QString &ln : said.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                    if (ln.contains(QLatin1String("ERROR")))
                        log(QStringLiteral("stderr"), QStringLiteral("mirror: %1").arg(ln));
            // The adopted boot-anim splash clears the 1s liveness check while still playing its
            // cached animation, then dies at the hand-off to live mirroring (the guest session
            // server / VAAPI encoder isn't ready the instant boot_completed lands) — the recurring
            // "first connect closes, reconnect holds". Only that adopted first attempt is fragile
            // (a plain spawn is stable), so confirm it survives past the hand-off; if it drops,
            // clear the half-started guest server and fall through to relaunch a plain mirror.
            // Time already spent waiting for the consumer to arrive is time the window demonstrably
            // survived, so it counts against this hold rather than being added to it.
            if (mirrorHoldMs > 0 && a == 1 && adoptSplash && a < attempts) {
                bool held = true;
                for (int w = handoffWaitedMs; w < mirrorHoldMs; w += 1000) {
                    // Ask BEFORE sleeping. By the time the arrival wait above has seen a served
                    // consumer, the mirror is a live session pulling frames — sleeping a further
                    // second to re-confirm it is pure latency on the cold path, paid after the
                    // user can already see the device. Measured: the consumer attaches ~6.0s into
                    // an 8.2s connect, so the whole tail after it is this loop and the liveness
                    // delay above it.
                    if (ctx.rc.hostEncode && ctx.rc.hostEncodeDrivesMirror
                        && servedConsumers() > 0)
                        break;
                    sp.waitMs(1000);
                    if (!sp.detachedAlive(det)) { held = false; break; }
                    // Evidence beats the clock. This hold asks "did the window survive the
                    // hand-off?", and a mirror the host encoder is actively SERVING has answered
                    // it — that is a live session pulling frames, not a process that merely has
                    // not exited yet. The fixed wait was only ever a stand-in for a signal we did
                    // not have; the served-consumer probe (bd remora-90y) is that signal, so stop
                    // spending seconds re-asking a question already answered. Nothing to lean on
                    // when the device does its own encoding — there the timer still rules.
                }
                if (!held) {
                    log(QStringLiteral("stderr"),
                        QStringLiteral("boot-anim mirror dropped at hand-off — relaunching a plain "
                                       "mirror (the stable reconnect, now automatic)"));
                    sp.run({"adb", "connect", target}, {}, log);
                    if (retryDelayMs > 0) sp.waitMs(qMax(retryDelayMs, 2500));
                    continue;
                }
            }
            // Exactly one mirror survives this step. Whatever the adoption test decided, a
            // predecessor that is still alive here is a SECOND client on the same device: both
            // pull from the one host encoder, which shows up as heavy black flickering and as
            // frames neither window can import (bd remora-e5x.19 / e5x.19.1 — confirmed live,
            // twice, by two mirror pids on one target). Reaped AFTER the replacement is up, not
            // before, so the window is never absent — that ordering is what made the earlier
            // pre-emptive reap read as a vanish-and-reopen (bd remora-eoy.10).
            // Reap only pids that predate this spawn, and only if the splash was NOT adopted
            // (an adopted splash IS this mirror). After the replacement is up, never before, so
            // the window is never absent — the ordering bd remora-eoy.10 was about.
            // Intruders are reaped WHATEVER the adoption test decided — that is the half bd
            // remora-51j was missing. An adopted splash is safe from this: it lives in
            // priorMirrors, and the adopt branch reaps nothing from that list.
            if (!ctx.fullscreenDisplay) {
                QStringList stale = intruders;
                if (!adoptSplash)
                    for (const QString &pid : priorMirrors)
                        if (!stale.contains(pid)) stale << pid;
                stale.removeAll(QString::number(det.pid));
                // TERM then KILL, not TERM alone. A predecessor that is now sharing the encoder is
                // a STARVED client — blocked in its demuxer waiting for frames it will never get —
                // and that is exactly the state in which SIGTERM goes unserviced (observed in
                // reapMirrorAttempt's case: alive in futex_wait 84s after a TERM). A TERM-only reap
                // left the flicker running for as long as the survivor felt like ignoring it.
                if (!stale.isEmpty()) killMirrorPids(sp, stale, log);
            }
            // bd remora-7ps: the false success. Everything above judged CONNECTION-level
            // evidence — the client is alive and an ESTAB sits on the video socket — and both
            // were true seconds before the encoder SIGSEGV'd under the predecessor teardown and
            // the old client's no-first-frame guard took the fresh client with it: this step
            // printed ✓ while neither a mirror nor an encoder survived it. The guard makes that
            // outcome detectable, so detect it: require DATA-level evidence — the encoder's own
            // 'consumer N got its first decodable packet' line, which a healthy attach produces
            // in ~1.2 s and which is usually in the log before this gate even asks — and read
            // the two death signals while waiting. Sits AFTER the reap on purpose: the teardown
            // it performs is exactly what the encoder died under in the wild. Fail-open per the
            // attach-gate lesson (bd remora-82x): with both processes alive this gate only ever
            // DELAYS the verdict — grace expiry is a pass with a note, and only a death, which
            // no healthy mirror exhibits, fails it.
            QString serveNote;
            if (ctx.rc.hostEncode && ctx.rc.hostEncodeDrivesMirror) {
                constexpr int kServeGraceMs = 6000;  // comfortably above the ~1.2 s healthy serve
                constexpr int kServePollMs = 200;    // kArrivalPollMs' reasoning: the probes are
                                                     // local and cheap; a coarse poll only adds
                                                     // latency between the event and noticing
                for (int waited = 0;;) {
                    if (packetBaseline >= 0) {
                        bool ok = false;
                        const int n =
                            sp.run({"sh", "-c",
                                    QStringLiteral("grep -c 'got its first decodable packet' %1 "
                                                   "2>/dev/null")
                                        .arg(encoderLog)},
                                   {}, log)
                                .out.trimmed()
                                .toInt(&ok);
                        if (ok && n > packetBaseline) break;  // served — the healthy exit
                    }
                    const bool clientAlive = sp.detachedAlive(det);
                    const bool encoderAlive =
                        sp.run({"sh", "-c",
                                QStringLiteral("ps -C remora-frame-encoder -o args= 2>/dev/null "
                                               "| grep -qF -- '%1'")
                                    .arg(ctx.rc.hostEncodeFrameSocket)},
                               {}, log)
                            .rc == 0;
                    if (!clientAlive || !encoderAlive) {
                        const QString tail =
                            encoderLog.isEmpty()
                                ? QString()
                                : sp.run({"sh", "-c",
                                          QStringLiteral("tail -n 12 %1 2>/dev/null")
                                              .arg(encoderLog)},
                                         {}, log)
                                      .out.trimmed();
                        return StepResult::fail(
                            !encoderAlive
                                ? QStringLiteral(
                                      "the host encoder DIED before serving the new mirror — the "
                                      "window this step spawned received no video and %1. Its log "
                                      "was %2; check `coredumpctl list remora-frame-encoder` and "
                                      "free VRAM, then reconnect — a fresh connect respawns both "
                                      "halves")
                                      .arg(clientAlive
                                               ? QStringLiteral("will exit by the no-first-frame "
                                                                "guard")
                                               : QStringLiteral("already exited by the "
                                                                "no-first-frame guard"),
                                           encoderLog.isEmpty() ? QStringLiteral("not resolvable")
                                                                : encoderLog)
                                : QStringLiteral(
                                      "the mirror exited before the host encoder served its first "
                                      "packet — reconnect to respawn it (encoder still up on %1)")
                                      .arg(ctx.rc.hostEncodeFrameSocket),
                            tail);
                    }
                    // No log to count lines in: the death checks above are all this gate has, so
                    // ask them once and move on rather than idling out the whole grace window.
                    if (packetBaseline < 0) break;
                    if (waited >= kServeGraceMs) {
                        serveNote = QStringLiteral(
                                        "serve unconfirmed: no first-packet line within %1s")
                                        .arg(kServeGraceMs / 1000);
                        break;
                    }
                    sp.waitMs(kServePollMs);
                    waited += kServePollMs;
                }
            }
            // SERVED is not SURVIVED (bd remora-10bt). Twice in one evening a mirror cleared
            // every gate above — alive, ESTAB, first packet served — and was dead 1.5-2 s
            // later: under an active desktop stream (Sunshine capture + NVENC on the same GPU)
            // the client's first damage burst can stall it past the encoder's 500 ms send
            // deadline, and the encoder evicts it BY DESIGN as a broken consumer. The death is
            // silent on both sides — EOF reads as stream end (clean exit 0), the encoder logs
            // a neutral "consumer disconnected" — so without this hold the step prints ✓ for a
            // corpse and the user's report is "says its connected". Three seconds covers both
            // observed deaths with margin; paid only on host-encode profiles, after the serve
            // gate's ~1.2 s fast path, and it buys the retry loop (or an honest failure) in
            // place of a silent no-window connect.
            if (ctx.rc.hostEncode && ctx.rc.hostEncodeDrivesMirror) {
                constexpr int kSurviveHoldMs = 3000;
                bool survived = true;
                for (int w = 0; w < kSurviveHoldMs; w += 1000) {
                    sp.waitMs(1000);
                    if (!sp.detachedAlive(det)) {
                        survived = false;
                        break;
                    }
                }
                if (!survived) {
                    log(QStringLiteral("stderr"),
                        QStringLiteral(
                            "mirror pid %1 died within 3 s of being served — with a desktop "
                            "stream active this is the encoder evicting a consumer that stalled "
                            "past its 500 ms send deadline (bd remora-10bt)")
                            .arg(det.pid));
                    if (a < attempts) continue;
                    return StepResult::fail(QStringLiteral(
                        "every mirror died seconds after being served. With host encode this is "
                        "the encoder evicting stalled consumers under GPU contention — free VRAM "
                        "(close GPU-heavy apps, lighten the desktop stream) and reconnect "
                        "(bd remora-10bt)"));
                }
            }
            QString detail = QStringLiteral("mirror pid %1 on %2").arg(det.pid).arg(target);
            // Say whether the pre-spawned window was reused or abandoned, and if abandoned, which
            // condition decided it. This goes in the SUCCESS DETAIL rather than the step log
            // because ConsoleSink overrides state() only and drops line() on the floor — a
            // log-only note is invisible from the CLI and the desktop launcher, the very places a
            // cold start happens. A deploy that declines its own live splash is the signature of
            // bd remora-eoy.10, and finding that took a log-snapshotting reproduction.
            // In the DETAIL, not the log, for the reason the adoption note is: ConsoleSink
            // overrides state() only and drops line(), so a log-only note is invisible from the
            // CLI and the desktop launcher. A reaped husk is exactly the thing worth seeing —
            // it means a previous session had been quietly degrading this one.
            if (encoderReboundStarved)
                detail += QStringLiteral(" [rebound a starved host encoder]");
            // The encoder's published shape (bd remora-e5x.18.3). In the DETAIL for the
            // ConsoleSink reason above: the encoder degrades AUTONOMOUSLY under VRAM pressure,
            // and a silently quarter-sized mirror was diagnosed as everything except the encoder.
            // Read at success time rather than at the encoder step, so a degradation during this
            // very connect's settle is already visible. Always a local read — the encoder only
            // exists where the venus stack does.
            if (ctx.rc.hostEncode) {
                const ProcResult st =
                    sp.run({"sh", "-c",
                            QStringLiteral("cat %1 2>/dev/null")
                                .arg(hostEncodeStatusFileFor(ctx.rc.containerName))},
                           {}, log);
                if (const auto es = parseEncoderStatus(st.out)) {
                    const QString note = encoderDegradationNote(*es);
                    if (!note.isEmpty()) detail += QStringLiteral(" [%1]").arg(note);
                    // The one failure every other gate certifies as healthy (bd remora-pobz): the
                    // guest presents client targets it no longer renders into, so publishers,
                    // consumers, packet rate and this very step's serve proof all stay green while
                    // the picture is black. The encoder is the only party holding the pixels, so
                    // it is the only one that can say so. A NOTE and never a failure — the flag is
                    // a size heuristic, and a genuinely dark scene trips it honestly; the remedy
                    // travels with it because it is not obvious from the symptom.
                    if (es->blackStream == 1)
                        detail += QStringLiteral(
                            " [stream looks BLACK: keyframes too small for %1x%2 while frames "
                            "flow — if the window is black, restart the guest framework: "
                            "docker exec %3 sh -c 'stop && start']")
                                      .arg(es->encodeW)
                                      .arg(es->encodeH)
                                      .arg(ctx.rc.containerName);
                }
            }
            if (huskCount > 0)
                detail += QStringLiteral(" [reaped %1 husk mirror(s) that outlived their server]")
                              .arg(huskCount);
            if (ctx.splash && !adoptSplash) detail += QStringLiteral(" [splash NOT adopted: %1]").arg(adoptWhyNot);
            else if (adoptSplash && a == 1) detail += QStringLiteral(" [adopted splash]");
            // Same reasoning, for the gate that decides WHEN the adoption is allowed to happen
            // (bd remora-82x): a hand-off that lands mid-restart looks identical to one that was
            // never held, and only this line separates them after the fact.
            if (!settleNote.isEmpty()) detail += QStringLiteral(" [%1]").arg(settleNote);
            // The fail-open residue of the serve gate above: both processes alive but no
            // data-level proof within grace. Worth a bracket rather than silence — if the
            // window then shows no video, this note is the difference between "connect lied"
            // and "connect said it could not confirm".
            if (!serveNote.isEmpty()) detail += QStringLiteral(" [%1]").arg(serveNote);
                    if (!hostRouteNote.isEmpty()) detail += QStringLiteral(" [%1]").arg(hostRouteNote);
            // Here, and nowhere earlier: the marker's whole meaning is "a boot on this /data
            // carried all the way through to a working mirror". Written after the liveness hold, so
            // it cannot be claimed by a boot that was still about to reboot out from under us —
            // the one thing it is asked to rule out. Only when a hold was actually requested; a
            // warm reconnect proves nothing about a first boot.
            if (bootStableMs > 0)
                containerExec(sp, ctx, QStringLiteral("touch %1").arg(kBootStableMarker), log);
            return StepResult::good(detail);
        }
        // alive-but-broken: get this blank instance out of the way before retrying, so it cannot
        // hold an encoder context against the next attempt — or, worse, survive to become a second
        // client on the same encoder. `kill det.pid` alone did neither reliably; see
        // reapMirrorAttempt for why both halves of that were wrong.
        // An adopted splash is itself a predecessor, so nothing is spared when reaping it.
        if (alive)
            reapMirrorAttempt(sp, ctx, target, (a == 1 && adoptSplash) ? QStringList() : priorMirrors,
                              det, log);
        if (a < attempts) {
            log(QStringLiteral("stderr"),
                QStringLiteral("%1 (attempt %2/%3)%4 — relaunching")
                    .arg(alive ? QStringLiteral("mirror rejected and killed")
                               : QStringLiteral("the mirror exited immediately"))
                    .arg(a)
                    .arg(attempts)
                    .arg(!brokenWhy.isEmpty() ? QStringLiteral(": %1").arg(brokenWhy)
                         : said.isEmpty()
                             ? QString()
                             // Only reached when nothing Remora tested decided it, so this line is
                             // a hint and is labelled as one: the detached log is shared by every
                             // process in the launch (bd remora-e5x.13).
                             : QStringLiteral(": last line of the shared detached log (may be "
                                              "another process') was \"%1\"")
                                   .arg(said.section('\n', -1))));
            // an encoder/MediaCodec failure means the c2-va HAL self-disabled VAAPI — restart it
            // (once) so the next attempt gets a freshly-probed encoder
            if (a == 1
                && (said.contains(QLatin1String("MediaCodec"))
                    || said.contains(QLatin1String("Could not create"))
                    || said.contains(QLatin1String("encoding error")))) {
                log(QStringLiteral("stderr"),
                    QStringLiteral("encoder failed — restarting the guest c2-va HAL"));
                restartC2Hal(sp, ctx, log);
            }
            sp.run({"adb", "connect", target}, {}, log);
            // a dying mirror is usually the guest encoder still warming up (first boot,
            // dex2oat storm) — give it a real pause instead of hammering
            if (retryDelayMs > 0) sp.waitMs(qMax(retryDelayMs, 2500));
        } else if (!said.isEmpty()) {
            for (const QString &ln : said.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                log(QStringLiteral("stderr"), QStringLiteral("mirror: %1").arg(ln));
            return StepResult::fail(
                QStringLiteral("the mirror exited immediately after %1 attempts — %2%3")
                    .arg(attempts)
                    .arg(said.section('\n', -1), encoderNote()),
                said);
        }
    }
    return StepResult::fail(
        QStringLiteral("the mirror exited immediately after %1 attempts — no window appeared%2")
            .arg(attempts)
            .arg(encoderNote()));
}

}  // namespace remora
