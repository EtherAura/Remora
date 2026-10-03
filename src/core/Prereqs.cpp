#include "core/Prereqs.h"

#include <QRegularExpression>
#include <QSet>

#include "core/Resolver.h"

namespace remora {

static bool flag(const std::optional<bool> &v) { return v.has_value() && *v; }

bool releaseNeedsAshmem(int androidVersion) { return androidVersion < 17; }

QVector<PrereqResult> checkReadiness(Backend target, const HostCapabilities &caps, bool gpuHost,
                                     bool macvlan, bool sharedInputs, bool needsAshmem) {
    QVector<PrereqResult> out;
    const auto add = [&](const QString &key, const QString &label, bool ok, const QString &remedy,
                         const QString &severity = QStringLiteral("required")) {
        out.append({key, label, ok, remedy, severity});
    };

    // Every target's connect launches the local adb client — a missing tool otherwise
    // surface only as a confusing late failure inside the connect step.
    add("adb", "adb present on the host", caps.adbPresent, "install android-tools (provides adb)");
    // No mirror-client prereq: the client lives inside the remora binary, and its device half is
    // the agent the image bakes — an image without it gets the client's own refusal at connect,
    // where the deployed image is known.

    if (target == Backend::Bare) {
        add("docker_group", "user in the 'docker' group", caps.inDockerGroup,
            "sudo usermod -aG docker $USER, then re-login");
        // Membership is not a daemon. With docker stopped, or DOCKER_HOST naming a socket that is
        // not there, every row above passed and `check` said READY — the deploy then failed at its
        // first docker call with a message about a socket, one step after being told all was well.
        add("docker_daemon", "docker daemon reachable", flag(caps.dockerDaemonOk),
            "start the docker service (systemctl start docker) — or unset a stale DOCKER_HOST");
        // Android 16 only — an Android 17 image runs on memfd and never needs the module.
        if (needsAshmem)
            add("ashmem", "IBT-fixed ashmem_linux.ko loaded (Android 16 images)", caps.modAshmem,
                "modprobe the IBT-fixed ashmem_linux (bare metal only)");
    } else {  // Backend::Remote
        add("ssh_remote", "remote host reachable over ssh", flag(caps.sshReachable),
            "confirm ssh access (key-based) to the remote host");
        add("remote_docker", "docker usable on the remote", flag(caps.remoteDockerOk),
            "install docker and add the remote user to the docker group");
    }

    // macvlan needs a parent NIC, a subnet and a gateway, and none of them is typed by hand — the
    // deploy reads them off the docker host and creates the network itself (bd remora-400). Report
    // WHAT was read, not just that something was: a macvlan on the wrong parent produces a
    // container that boots, gets an address, and is reachable from nowhere.
    if (macvlan) {
        add("host_lan",
            caps.hostLan.has_value()
                ? QStringLiteral("Host LAN for macvlan: %1 on %2 (gateway %3)")
                      .arg(caps.hostLan->subnet, caps.hostLan->parent, caps.hostLan->gateway)
                : QStringLiteral("Host LAN readable (macvlan parent/subnet/gateway)"),
            caps.hostLan.has_value(),
            "no default IPv4 route with an addressed interface on the docker host — set "
            "macvlan_parent=, macvlan_subnet= and macvlan_gateway=, or use network_mode=bridge");
    }

    // Only when the profile actually shares devices (the macvlan pattern: a per-profile choice,
    // asked of the resolved config). "optional" severity to match the step's own philosophy — a
    // deploy without the shares still boots and mirrors, so the step is advisory and the check
    // must not block harder than the deploy does. But it must be SAID here: without python3 every
    // deploy reports "could not run remora-input-share.py — no devices shared" and nothing names
    // the missing interpreter (bd remora-4ei.36).
    if (sharedInputs) {
        add("python3", "python3 on the docker host (runs the shared_inputs bridge)",
            caps.python3Present,
            "install python3 on the docker host — remora-input-share.py (stdlib-only) is what "
            "exposes shared input devices into the container and re-exposes them on replug",
            "optional");
    }

    // Name the probed GPU (bd remora-4ei.79): check/plan used to be silent about what the deploy
    // would actually render on, which is what let SwiftShader fallbacks pass as success. The ok
    // row carries node + driver in its label; the failing row means host mode has nothing to
    // point at and the guest's own scan is the only hope left — worth a line either way.
    if (gpuHost) {
        add("host_render_node",
            caps.hostRenderNode.has_value()
                ? QStringLiteral("Host GPU: %1 (%2)")
                      .arg(*caps.hostRenderNode,
                           caps.hostRenderDriver.value_or(QStringLiteral("unknown driver")))
                : QStringLiteral("Host GPU render node probed"),
            caps.hostRenderNode.has_value(),
            "no /dev/dri render node with a readable driver — host mode will rely on the image's "
            "own gpu_config.sh scan, and may land on SwiftShader",
            "gpu");
    }

    // HW video (bd remora-4ei.80). The c2-va HAL only works when LIBVA has a driver for the
    // probed GPU: iHD covers every Intel node and is the image default; amdgpu maps to radeonsi
    // (once the image ships its VA entrypoint); anything else falls back to SOFTWARE encode and
    // decode — silently, which is exactly how the NVIDIA gap went unnoticed. "gpu" severity: the
    // deploy still works, this line exists so the fallback is a stated fact instead of a
    // discovery. An explicit va_driver= override supersedes this row's judgement.
    if (gpuHost && caps.hostRenderDriver.has_value()) {
        // Shared with the resolver's h265 downgrade so the reported judgement and the acted-on one
        // are the same function, not two lists that drift (bd remora-e5x.4).
        const bool vaKnown = driverHasVaEncode(*caps.hostRenderDriver);
        add("va_driver", "VA-API driver for the probed GPU", vaKnown,
            QStringLiteral("no VA driver mapping for '%1' — video will fall back to software "
                           "encode/decode; set va_driver= to override if a driver exists")
                .arg(*caps.hostRenderDriver),
            "gpu");
    }

    // NVIDIA Venus, on the backends that deploy through the docker argv (bd remora-4ei.56).
    // "gpu" severity throughout: without these the xe/SwiftShader path is still correct, so these
    // lines exist to explain why Venus is or is not on — never to block a deploy that did not want
    // it. They are reported even when all three are missing, because otherwise there is no way to
    // discover the feature or see which piece is short.
    if (gpuHost) {
        add("venus_nvidia_node", "NVIDIA render node present",
            caps.hostNvidiaRenderNode.has_value(),
            "no /dev/dri/renderD* is driven by 'nvidia' — the proprietary driver must be loaded",
            "gpu");
        // Only when both sides are known: no loaded module or no found userspace library is
        // "nothing to compare" (and absence is already the row above's business). This line
        // exists because every OTHER row here stayed green through a real failure: a driver
        // update without a reboot broke nothing that is INSTALLED — it broke what is LOADED,
        // and the damage surfaced as SurfaceFlinger crash-looping after an image deploy, which
        // aimed a whole investigation at the image (bd remora-81cp).
        if (caps.nvidiaVersions.kernel && !caps.nvidiaVersions.userspace.isEmpty()) {
            const bool match = !nvidiaAbiMismatch(caps.nvidiaVersions);
            add("nvidia_abi_match",
                match ? QStringLiteral("NVIDIA kernel module matches userspace (%1)")
                            .arg(*caps.nvidiaVersions.kernel)
                      : QStringLiteral("NVIDIA kernel module matches userspace"),
                match,
                QStringLiteral(
                    "loaded module %1 vs installed userspace %2 — the driver was updated without "
                    "a reboot. Every NEW GPU client gets 'NVRM: API mismatch': the Venus render "
                    "server's per-client forks die, EGL never initialises and SurfaceFlinger "
                    "crash-loops, which looks exactly like a broken image. Reboot (or reload the "
                    "module) before deploying")
                    .arg(*caps.nvidiaVersions.kernel,
                         caps.nvidiaVersions.userspace.join(QLatin1String(", "))),
                "gpu");
        }
        add("venus_server", "Venus render server installed", caps.venusServerPath.has_value(),
            "run `remora venus-build` — it clones and patches virglrenderer, compiles it, and "
            "installs under $REMORA_VENUS_DIR (default ~/.local/share/remora/venus)",
            "gpu");
        // Reported as one line because a PARTIAL guest-libs dir is treated as absent: a missing
        // library boots to a black screen rather than to an error, so half-enabling is worse than
        // not enabling.
        // Host-side encode (bd remora-e5x.10). Reported at "gpu" severity like the Venus trio:
        // informational, never a blocker, because a machine that does not want host encode is not
        // broken for lacking it. Before these existed a missing piece surfaced only at connect,
        // as a non-fatal log line the CLI summarises away — the encoder simply never started and
        // nothing said why.
        add("host_encoder_built", "Host frame encoder built", caps.hostEncoderPath.has_value(),
            "the Remora build compiles it when it finds FFmpeg 7 or newer (libavfilter) and the "
            "Vulkan headers — install those and rebuild (cmake --build, then cmake --install for "
            "an installed Remora). A package built against an older FFmpeg, such as the Ubuntu "
            "24.04 .deb, does not include it",
            "gpu");
        add("host_encode_ffmpeg", "ffmpeg can encode HEVC on Vulkan", caps.ffmpegHevcVulkan,
            "this ffmpeg lists no hevc_vulkan encoder — host encode needs a build with Vulkan "
            "Video encode. NOTE this only proves ffmpeg SUPPORTS it: whether the GPU exposes a "
            "VIDEO_ENCODE queue is not visible to a shell probe, and a card without one fails at "
            "device creation instead",
            "gpu");
        add("host_encode_capture", "Venus render server publishes frames",
            caps.venusCapturePatched,
            "the installed virgl_render_server carries no frame publisher, so nothing will ever "
            "reach the encoder — rebuild with `remora venus-build` (patches 0009-0012 add it) and "
            "remember the render server is STATICALLY linked, so installing libvirglrenderer.so "
            "alone leaves the old code running",
            "gpu");
        add("venus_guest_libs", "Venus guest libraries complete (all five)",
            caps.venusLibDir.has_value(),
            "run `remora venus-build` — it fetches the five guest libraries from their pinned "
            "release and SHA256-verifies them (NB vulkan.virtio.so must be the ~29.7MB "
            "prebuilt, NOT the image's own — that one renders but cannot capture)",
            "gpu");
    }
    return out;
}

bool isReady(const QVector<PrereqResult> &results) {
    for (const PrereqResult &r : results)
        if (r.severity == QLatin1String("required") && !r.ok) return false;
    return true;
}

QVector<PrereqResult> blockers(const QVector<PrereqResult> &results) {
    QVector<PrereqResult> out;
    for (const PrereqResult &r : results)
        if (r.severity == QLatin1String("required") && !r.ok) out.append(r);
    return out;
}

QStringList archVariantRequiredFlags(const QString &variant) {
    // The x86-64-v3 core, shared by every variant at that level or above. These are what a
    // vectorizer actually emits without intrinsics, which is the only thing that can SIGILL a
    // build nobody hand-wrote assembly for.
    static const QStringList v3 = {QStringLiteral("avx2"),  QStringLiteral("bmi1"),
                                   QStringLiteral("bmi2"),  QStringLiteral("fma"),
                                   QStringLiteral("f16c"),  QStringLiteral("movbe"),
                                   QStringLiteral("abm")};
    if (variant == QLatin1String("haswell")) return v3;  // -march=core-avx2
    if (variant == QLatin1String("broadwell") || variant == QLatin1String("skylake")) {
        // -march=broadwell adds ADX/RDSEED/PREFETCHW over Haswell.
        //
        // RTM AND HLE ARE DELIBERATELY ABSENT, and this is the single most important omission in
        // this table. Both are in broadwell's ISA and NEITHER is on any Zen part, so listing them
        // would refuse a broadwell image on exactly the AMD hosts it was chosen FOR. They are safe
        // to omit because a compiler does not emit RTM/HLE from ordinary C++ — they require
        // intrinsics (_xbegin/_xend), and AOSP uses none. Measured on both AMD hosts:
        // adx avx2 bmi1 bmi2 f16c fma movbe abm rdseed 3dnowprefetch present, rtm/hle absent.
        return v3 + QStringList{QStringLiteral("adx"), QStringLiteral("rdseed")};
    }
    if (variant == QLatin1String("alderlake") || variant == QLatin1String("pantherlake")) {
        // The additions that make an alderlake build unrunnable on pre-Zen4 AMD. AVX-VNNI is the
        // one a vectorizer reaches for on its own; the rest are cheap to check and decisive by
        // their absence. Measured on both AMD hosts: none of these five present.
        //
        // CLDEMOTE IS DELIBERATELY NOT HERE, and it is the reason this list was measured against
        // real silicon instead of read out of the ISA manual. -march=alderlake enables -mcldemote,
        // but Intel fuses CLDEMOTE OFF on consumer Alder Lake: this project's own 12900K build host
        // does not report it in /proc/cpuinfo. Requiring it would refuse an alderlake image on THE
        // MACHINE THAT BUILT IT — the false refusal this whole mechanism exists to avoid, and it
        // would have shipped if the fixture had been written from documentation rather than from
        // `grep ^flags /proc/cpuinfo`.
        return v3 + QStringList{QStringLiteral("adx"),      QStringLiteral("rdseed"),
                                QStringLiteral("avx_vnni"), QStringLiteral("serialize"),
                                QStringLiteral("movdiri"),  QStringLiteral("movdir64b"),
                                QStringLiteral("waitpkg")};
    }
    if (variant == QLatin1String("stoneyridge")) {
        // soong's ONE AMD-named entry, and a trap: it maps to -march=bdver4 (Excavator), whose
        // XOP/FMA4/TBM Zen REMOVED. Picking the AMD-sounding variant for AMD hardware SIGILLs just
        // as surely as alderlake does — measured, all three absent on both Zen hosts.
        return QStringList{QStringLiteral("avx2"), QStringLiteral("fma"), QStringLiteral("bmi2"),
                           QStringLiteral("xop"),  QStringLiteral("fma4"), QStringLiteral("tbm")};
    }
    if (variant == QLatin1String("sandybridge")) return {QStringLiteral("avx")};
    if (variant == QLatin1String("ivybridge"))
        return {QStringLiteral("avx"), QStringLiteral("f16c")};
    // Everything else — the baseline "x86_64", the Atom line (silvermont/goldmont*/tremont), the
    // bare feature names (sse4*, ssse3, popcnt, aes_ni), and any variant added to soong after this
    // was written — asks for nothing. See the header: not knowing a name is not grounds to refuse.
    return {};
}

QStringList missingArchVariantFlags(const QString &variant, const QString &flagsLine) {
    const QStringList want = archVariantRequiredFlags(variant);
    if (want.isEmpty()) return {};
    // A cpuinfo line we could not read tells us nothing about the CPU. Refusing here would turn
    // "the probe failed" into "your hardware is wrong", which is the false refusal this whole
    // mechanism is written to avoid.
    if (flagsLine.trimmed().isEmpty()) return {};
    // Split on whitespace and match WHOLE tokens: "avx" is a prefix of "avx2" and of "avx512f", so
    // a contains() test would silently pass a CPU that has neither.
    const QSet<QString> have = [&] {
        QSet<QString> s;
        const QString body = flagsLine.section(QLatin1Char(':'), 1);
        for (const QString &t :
             (body.isEmpty() ? flagsLine : body).split(QRegularExpression(QStringLiteral("\\s+")),
                                                       Qt::SkipEmptyParts))
            s.insert(t);
        return s;
    }();
    QStringList missing;
    for (const QString &f : want)
        if (!have.contains(f)) missing << f;
    return missing;
}

QString archVariantRefusal(const QString &variant, const QString &flagsLine,
                           const QString &imageTag, const QString &where) {
    const QStringList missing = missingArchVariantFlags(variant, flagsLine);
    if (missing.isEmpty()) return QString();
    // Name the missing flags, because the failure they prevent names nothing at all: an image whose
    // codegen the CPU cannot execute does not report an unsupported anything — a process takes
    // SIGILL somewhere inside libart or a vendor HAL, init restarts it, and the log reads like a
    // broken image rather than a wrong machine.
    return QStringLiteral(
               "%1 was compiled for TARGET_ARCH_VARIANT=%2, and %3 cannot execute it — the CPU is "
               "missing %4. This is not a slow deploy, it is a crashing one: the build emits "
               "instructions this processor does not have, so processes take SIGILL with nothing in "
               "the log naming the CPU. Pick one:\n"
               "  deploy a portable image here: build with the arch variant left blank (Image panel "
               "→ Source build → CPU arch variant), which compiles at the x86_64 baseline and runs "
               "anywhere\n"
               "  tune for THIS machine: build with a variant this host satisfies, and give it its "
               "own tag so the two do not overwrite each other\n"
               "  run the image where it belongs: %1 is correct on a host that has %4")
        .arg(imageTag, variant, where, missing.join(QStringLiteral(", ")));
}

}  // namespace remora
