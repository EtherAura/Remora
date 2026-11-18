#include <QFile>
#include <QProcess>
#include <QtTest>

#include "TestPlatform.h"
#include "core/Builders.h"
#include "core/Prereqs.h"
#include "core/Resolved.h"
#include "core/Resolver.h"

// The host encoder's own --codec gate, compiled here as it is there (it carries no ffmpeg).
#include "../vendor/native/remora-encoder-names.h"

using namespace remora;

// every target's connect needs the local adb client (the mirror client is the remora binary
// itself since the cutover, bd remora-28ix.2 — no external tool to require)
static HostCapabilities withTools() {
    HostCapabilities c;
    c.adbPresent = true;
    return c;
}

// Bare readiness asks about this machine's docker, which only a Linux client has. The client's OS
// is an input (HostCapabilities::localOs), so these rows hold on a Windows build as well; what
// Windows answers instead is windowsBareIsOneLinuxRefusal.
static HostCapabilities linuxClient() {
    HostCapabilities c = withTools();
    c.localOs = LocalOs::Unix;
    return c;
}

class TestPrereqs : public QObject {
    Q_OBJECT
private slots:
    // bd remora-7oih.2 — a Windows client cannot run bare, so its readiness says that ONCE instead
    // of listing the docker group, a local daemon and ashmem as missing with usermod / systemctl /
    // modprobe remedies it could never act on. Remote readiness is the same rows as on Linux.
    void windowsBareIsOneLinuxRefusal() {
        HostCapabilities c = withTools();
        c.localOs = LocalOs::Windows;
        const auto r = checkReadiness(Backend::Bare, c, false, false, false, true);
        QVERIFY(!isReady(r));
        QStringList keys;
        for (const auto &row : r) keys << row.key;
        QCOMPARE(keys, (QStringList{"adb", "bare_linux"}));
        QCOMPARE(r.at(1).remedy, bareNeedsLinuxMessage());
        c.sshReachable = true;
        c.remoteDockerOk = true;
        QVERIFY(isReady(checkReadiness(Backend::Remote, c)));
        // adb's remedy names what a Windows user installs
        c.adbPresent = false;
        QVERIFY(checkReadiness(Backend::Remote, c).at(0).remedy.contains(
            QLatin1String("Platform-Tools")));
        const HostCapabilities onLinux = linuxClient();
        QStringList linuxKeys;
        for (const auto &row : checkReadiness(Backend::Bare, onLinux, false, false, false, true))
            linuxKeys << row.key;
        QCOMPARE(linuxKeys, (QStringList{"adb", "docker_group", "docker_daemon", "ashmem"}));
    }

    // A daemon that cannot be asked blocks a bare deploy, and names itself.
    void dockerDaemonDownBlocksBare() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.modAshmem = true;
        c.dockerDaemonOk = false;
        const auto r = checkReadiness(Backend::Bare, c);
        QVERIFY(!isReady(r));
        bool named = false;
        for (const auto &b : blockers(r))
            if (b.key == QLatin1String("docker_daemon")) named = true;
        QVERIFY(named);
        c.dockerDaemonOk = true;
        QVERIFY(isReady(checkReadiness(Backend::Bare, c)));
    }
    // bd remora-oky7. The ashmem row belongs to the release, not the target: an Android 17 image
    // runs on memfd, so a bare host without the module is READY for it, and the row is not shown
    // at all — while an Android 16 profile on the same host is blocked on it, by name.
    void ashmemRowOnlyForReleasesThatNeedIt() {
        QVERIFY(releaseNeedsAshmem(16));
        QVERIFY(releaseNeedsAshmem(13));
        QVERIFY(!releaseNeedsAshmem(17));
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = false;
        const auto a17 = checkReadiness(Backend::Bare, c, false, false, false,
                                        /*needsAshmem=*/releaseNeedsAshmem(17));
        QVERIFY(isReady(a17));
        for (const PrereqResult &r : a17) QVERIFY(r.key != QLatin1String("ashmem"));
        const auto a16 = checkReadiness(Backend::Bare, c, false, false, false,
                                        /*needsAshmem=*/releaseNeedsAshmem(16));
        QVERIFY(!isReady(a16));
        QCOMPARE(blockers(a16).size(), 1);
        QCOMPARE(blockers(a16).first().key, QStringLiteral("ashmem"));
    }
    // bd remora-4ei.36. The bridge script's interpreter, asked only of a profile that shares
    // devices (the macvlan pattern), and never a blocker — the deploy step is advisory, so the
    // check must not block harder than the deploy does. Without this row, a docker host missing
    // python3 fails every expose with "could not run remora-input-share.py" and nothing names why.
    void python3RowAppearsOnlyWhenSharingInputs() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        const auto find = [](const QVector<PrereqResult> &v, const QString &key) {
            for (const PrereqResult &r : v)
                if (r.key == key) return r;
            return PrereqResult{};
        };
        // Not sharing: no row, however the host looks.
        QVERIFY(find(checkReadiness(Backend::Bare, c), QStringLiteral("python3")).key.isEmpty());
        // Sharing, python3 missing: named with the remedy, and still READY.
        const QVector<PrereqResult> missing = checkReadiness(
            Backend::Bare, c, /*gpuHost=*/false, /*macvlan=*/false, /*sharedInputs=*/true);
        const PrereqResult bad = find(missing, QStringLiteral("python3"));
        QVERIFY(!bad.ok);
        QCOMPARE(bad.severity, QStringLiteral("optional"));
        QVERIFY(bad.remedy.contains(QStringLiteral("remora-input-share.py")));
        QVERIFY(isReady(missing));
        // Sharing, python3 present: ok.
        c.python3Present = true;
        QVERIFY(find(checkReadiness(Backend::Bare, c, false, false, true),
                     QStringLiteral("python3"))
                    .ok);
    }

    // bd remora-kcjd. macvlan's parent/subnet/gateway are read off the docker host, never typed,
    // so the row is asked only of a macvlan profile, blocks when nothing was read, and when
    // something was it names all three: a macvlan on the wrong parent is reachable from nowhere.
    void hostLanRowIsRequiredAndNamesWhatWasRead() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        const auto find = [](const QVector<PrereqResult> &v, const QString &key) {
            for (const PrereqResult &r : v)
                if (r.key == key) return r;
            return PrereqResult{};
        };
        // No macvlan: no row, and the fixture is otherwise ready.
        const QVector<PrereqResult> bridge =
            checkReadiness(Backend::Bare, c, /*gpuHost=*/false, /*macvlan=*/false);
        QVERIFY(find(bridge, QStringLiteral("host_lan")).key.isEmpty());
        QVERIFY(isReady(bridge));
        // macvlan, nothing read: a required row, and the only blocker.
        const QVector<PrereqResult> unread =
            checkReadiness(Backend::Bare, c, /*gpuHost=*/false, /*macvlan=*/true);
        const PrereqResult missing = find(unread, QStringLiteral("host_lan"));
        QCOMPARE(missing.key, QStringLiteral("host_lan"));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.severity, QStringLiteral("required"));
        QVERIFY(!isReady(unread));
        QCOMPARE(blockers(unread).size(), 1);
        QCOMPARE(blockers(unread).first().key, QStringLiteral("host_lan"));
        // macvlan, read: ok, and the label carries every value that was read.
        c.hostLan = LanInterface{QStringLiteral("eno1"), QStringLiteral("192.168.0.0/24"),
                                 QStringLiteral("192.168.0.1")};
        const QVector<PrereqResult> read =
            checkReadiness(Backend::Bare, c, /*gpuHost=*/false, /*macvlan=*/true);
        const PrereqResult ok = find(read, QStringLiteral("host_lan"));
        QVERIFY(ok.ok);
        QCOMPARE(ok.severity, QStringLiteral("required"));
        QVERIFY(ok.label.contains(QLatin1String("eno1")));
        QVERIFY(ok.label.contains(QLatin1String("192.168.0.0/24")));
        QVERIFY(ok.label.contains(QLatin1String("192.168.0.1")));
        QVERIFY(isReady(read));
    }

    // bd remora-81cp. Every other GPU row stayed green through the real failure — a driver update
    // without a reboot breaks nothing that is INSTALLED, only what is LOADED — so this row is the
    // one that names it. Reported only when both sides are known: absence of either is the
    // venus_nvidia_node row's business, and a non-NVIDIA host must not grow a row about NVIDIA.
    void nvidiaAbiMismatchIsNamedButNeverBlocks() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        const auto find = [](const QVector<PrereqResult> &v, const QString &key) {
            for (const PrereqResult &r : v)
                if (r.key == key) return r;
            return PrereqResult{};
        };
        // No NVIDIA at all: no row.
        QVERIFY(find(checkReadiness(Backend::Bare, c, /*gpuHost=*/true),
                     QStringLiteral("nvidia_abi_match"))
                    .key.isEmpty());
        // Mismatch: the row fails, carries BOTH versions and the reboot remedy — and stays a
        // "gpu" line, because a deploy that never wanted Venus is not broken by a stale module.
        c.nvidiaVersions.kernel = QStringLiteral("610.43.03");
        c.nvidiaVersions.userspace = QStringList{QStringLiteral("610.57.04")};
        const PrereqResult bad = find(checkReadiness(Backend::Bare, c, /*gpuHost=*/true),
                                      QStringLiteral("nvidia_abi_match"));
        QVERIFY(!bad.ok);
        QCOMPARE(bad.severity, QStringLiteral("gpu"));
        QVERIFY(bad.remedy.contains(QStringLiteral("610.43.03")));
        QVERIFY(bad.remedy.contains(QStringLiteral("610.57.04")));
        QVERIFY(bad.remedy.contains(QStringLiteral("Reboot")));
        QVERIFY(isReady(checkReadiness(Backend::Bare, c, /*gpuHost=*/true)));
        // Matched: ok, and the label names the version so the check output states what is loaded.
        c.nvidiaVersions.kernel = QStringLiteral("610.57.04");
        const PrereqResult good = find(checkReadiness(Backend::Bare, c, /*gpuHost=*/true),
                                       QStringLiteral("nvidia_abi_match"));
        QVERIFY(good.ok);
        QVERIFY(good.label.contains(QStringLiteral("610.57.04")));
    }

    // Host encode is reported so it can be DISCOVERED, and never blocks: a machine that does not
    // want it is not broken for lacking it. Before these checks a missing piece surfaced only at
    // connect, as a log line the CLI summarises away.
    void hostEncodeIsReportedButNeverBlocks() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        const QVector<PrereqResult> none = checkReadiness(Backend::Bare, c, /*gpuHost=*/true);
        const auto find = [](const QVector<PrereqResult> &v, const QString &key) {
            for (const PrereqResult &r : v)
                if (r.key == key) return r;
            return PrereqResult{};
        };
        QCOMPARE(find(none, QStringLiteral("host_encoder_built")).key,
                 QStringLiteral("host_encoder_built"));
        QVERIFY(!find(none, QStringLiteral("host_encoder_built")).ok);
        QVERIFY(!find(none, QStringLiteral("host_encode_ffmpeg")).ok);
        QVERIFY(!find(none, QStringLiteral("host_encode_capture")).ok);
        // ...and none of them is a blocker.
        QVERIFY(isReady(none));

        c.hostEncoderPath = QStringLiteral("/opt/remora/vendor/native/remora-frame-encoder");
        c.ffmpegHevcVulkan = true;
        c.venusCapturePatched = true;
        const QVector<PrereqResult> all = checkReadiness(Backend::Bare, c, /*gpuHost=*/true);
        QVERIFY(find(all, QStringLiteral("host_encoder_built")).ok);
        QVERIFY(find(all, QStringLiteral("host_encode_ffmpeg")).ok);
        QVERIFY(find(all, QStringLiteral("host_encode_capture")).ok);
    }

    void bareReady() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        QVERIFY(isReady(checkReadiness(Backend::Bare, c)));
    }

    void bareMissingAshmem() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        QVERIFY(!isReady(checkReadiness(Backend::Bare, c)));
    }

    // bd remora-4ei.80 — the software-video fallback must be a STATED fact, not a discovery.
    // A probed driver with no VA mapping gets a failing "gpu"-severity row (advisory: it must
    // never block a deploy); a mapped driver passes; no probe, no row.
    void bareUnmappedVaDriverIsNamedNotSilent() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        c.hostRenderNode = QStringLiteral("/dev/dri/renderD128");
        c.hostRenderDriver = QStringLiteral("nouveau");
        const auto rows = checkReadiness(Backend::Bare, c, /*gpuHost=*/true);
        bool found = false;
        for (const PrereqResult &r : rows)
            if (r.key == QLatin1String("va_driver")) {
                found = true;
                QVERIFY(!r.ok);
                QCOMPARE(r.severity, QStringLiteral("gpu"));
                QVERIFY(r.remedy.contains(QLatin1String("nouveau")));
            }
        QVERIFY(found);
        QVERIFY(isReady(rows));  // advisory only — never a blocker
        c.hostRenderDriver = QStringLiteral("amdgpu");
        for (const PrereqResult &r : checkReadiness(Backend::Bare, c, true))
            if (r.key == QLatin1String("va_driver")) QVERIFY(r.ok);
    }

    // bd remora-4ei.79 — host mode names the GPU it will render on, so a SwiftShader fallback
    // cannot pass as success. A node with no readable driver still names the node; no node at all
    // is a failing "gpu" line, never a blocker, since the image's own scan may still find one.
    void hostRenderNodeRowNamesTheProbedGpu() {
        HostCapabilities c = linuxClient();
        c.inDockerGroup = true;
        c.dockerDaemonOk = true;
        c.modAshmem = true;
        const auto find = [](const QVector<PrereqResult> &v, const QString &key) {
            for (const PrereqResult &r : v)
                if (r.key == key) return r;
            return PrereqResult{};
        };
        // Node and driver probed: ok, and the label carries both.
        c.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        c.hostRenderDriver = QStringLiteral("i915");
        const PrereqResult named = find(checkReadiness(Backend::Bare, c, /*gpuHost=*/true),
                                        QStringLiteral("host_render_node"));
        QCOMPARE(named.key, QStringLiteral("host_render_node"));
        QVERIFY(named.ok);
        QCOMPARE(named.label, QStringLiteral("Host GPU: /dev/dri/renderD129 (i915)"));
        // Node probed, driver not: still ok, the label says so, and there is no VA judgement to
        // make without a driver.
        c.hostRenderDriver.reset();
        const QVector<PrereqResult> noDriver = checkReadiness(Backend::Bare, c, /*gpuHost=*/true);
        const PrereqResult unknown = find(noDriver, QStringLiteral("host_render_node"));
        QCOMPARE(unknown.key, QStringLiteral("host_render_node"));
        QVERIFY(unknown.ok);
        QCOMPARE(unknown.label, QStringLiteral("Host GPU: /dev/dri/renderD129 (unknown driver)"));
        QVERIFY(find(noDriver, QStringLiteral("va_driver")).key.isEmpty());
        // No node: a failing "gpu" line, and the host is still ready.
        c.hostRenderNode.reset();
        const QVector<PrereqResult> noNode = checkReadiness(Backend::Bare, c, /*gpuHost=*/true);
        const PrereqResult missing = find(noNode, QStringLiteral("host_render_node"));
        QCOMPARE(missing.key, QStringLiteral("host_render_node"));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.severity, QStringLiteral("gpu"));
        QVERIFY(isReady(noNode));
    }

    void remoteReady() {
        HostCapabilities c = withTools();
        c.sshReachable = true;
        c.remoteDockerOk = true;
        QVERIFY(isReady(checkReadiness(Backend::Remote, c)));
    }

    // bd remora-7oih.3 — what preflight refuses, check reports as a required failure, in the same
    // words; and the Venus/host-encode rows are bare-only, because the remote probe no longer
    // scans for pieces nothing on remote would use.
    void remoteBareOnlyGpuSettingsAreNotReady() {
        const auto find = [](const QVector<PrereqResult> &v, const QString &key) {
            for (const PrereqResult &r : v)
                if (r.key == key) return r;
            return PrereqResult{};
        };
        HostCapabilities c = withTools();
        c.sshReachable = true;
        c.remoteDockerOk = true;
        const QStringList asked{QStringLiteral("host_encode=true")};
        const QVector<PrereqResult> rows =
            checkReadiness(Backend::Remote, c, /*gpuHost=*/true, false, false, true, asked);
        QVERIFY(!isReady(rows));
        const PrereqResult row = find(rows, QStringLiteral("bare_only_gpu"));
        QVERIFY(row.label.contains(QStringLiteral("host_encode=true")));
        QCOMPARE(row.remedy, bareOnlyFeaturesRefusal(asked));
        QVERIFY(find(rows, QStringLiteral("venus_server")).key.isEmpty());
        QVERIFY(find(rows, QStringLiteral("host_encoder_built")).key.isEmpty());
        QVERIFY(isReady(checkReadiness(Backend::Remote, c, true)));
        // Bare keeps the advisory rows.
        QVERIFY(!find(checkReadiness(Backend::Bare, c, true), QStringLiteral("venus_server"))
                     .key.isEmpty());
    }

    void remoteNoDocker() {
        HostCapabilities c = withTools();
        c.sshReachable = true;
        c.remoteDockerOk = false;
        QVERIFY(!isReady(checkReadiness(Backend::Remote, c)));
    }

    // bd remora-bm7.11. A TARGET_ARCH_VARIANT image SIGILLs on a CPU without the ISA — it does not
    // degrade — and delivering an image between machines is a `docker save | ssh docker load` with
    // no step that would notice. These pin the table against the REAL CPUs involved, because the
    // whole value of this check is being right in both directions: a miss ships a device that will
    // not boot, and a false refusal is the failure mode that got an earlier preflight deleted.
    //
    // The two flag lines below are the MEASURED /proc/cpuinfo sets, trimmed to the
    // tokens this table looks at, not invented.
    static QString zen1Flags() {  // EPYC 7551P / Ryzen 7 2700X — both AMD hosts
        return QStringLiteral(
            "flags\t: fpu vme de pse tsc msr sse sse2 ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt "
            "aes xsave avx f16c rdrand lahf_lm abm sse4a misalignsse 3dnowprefetch bmi1 avx2 "
            "smep bmi2 rdseed adx clflushopt sha_ni xsaveopt clzero");
    }
    static QString alderLakeFlags() {  // the 12900K this project builds on
        return QStringLiteral(
            "flags\t: fpu vme de pse tsc msr sse sse2 ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt "
            "aes xsave avx f16c rdrand lahf_lm abm 3dnowprefetch bmi1 avx2 smep bmi2 rdseed adx "
            "clflushopt sha_ni xsaveopt serialize waitpkg movdiri movdir64b avx_vnni");
    }

    void archVariantBroadwellRunsOnZen() {
        // The point of choosing broadwell for the AMD hosts: everything it needs is present.
        QVERIFY(missingArchVariantFlags(QStringLiteral("broadwell"), zen1Flags()).isEmpty());
        QVERIFY(missingArchVariantFlags(QStringLiteral("haswell"), zen1Flags()).isEmpty());
        // ...and RTM/HLE must NOT be demanded, or this exact case would refuse. Zen has neither,
        // and a compiler emits neither without intrinsics.
        const QStringList want = archVariantRequiredFlags(QStringLiteral("broadwell"));
        QVERIFY(!want.contains(QStringLiteral("rtm")));
        QVERIFY(!want.contains(QStringLiteral("hle")));
    }

    void archVariantAlderlakeRefusedOnZen() {
        const QStringList missing =
            missingArchVariantFlags(QStringLiteral("alderlake"), zen1Flags());
        QVERIFY(!missing.isEmpty());
        // Name them, so the refusal can say WHY rather than "unsupported".
        for (const char *f : {"avx_vnni", "serialize", "movdiri", "movdir64b", "waitpkg"})
            QVERIFY2(missing.contains(QLatin1String(f)), f);
        // ...and it runs on the machine it was built for.
        QVERIFY(missingArchVariantFlags(QStringLiteral("alderlake"), alderLakeFlags()).isEmpty());
    }

    void archVariantStoneyridgeIsATrapForAmd() {
        // soong's only AMD-named variant maps to -march=bdver4, whose XOP/FMA4/TBM Zen REMOVED.
        // Picking it because it sounds like the right vendor must be refused, not rewarded.
        const QStringList missing =
            missingArchVariantFlags(QStringLiteral("stoneyridge"), zen1Flags());
        for (const char *f : {"xop", "fma4", "tbm"})
            QVERIFY2(missing.contains(QLatin1String(f)), f);
    }

    void archVariantNeverRefusesOnIgnorance() {
        // An unknown variant, the baseline, and an unreadable probe must all produce NO refusal.
        // Refusing on ignorance is how a preflight earns its deletion (bd remora-4ei.19).
        QVERIFY(archVariantRequiredFlags(QStringLiteral("x86_64")).isEmpty());
        QVERIFY(archVariantRequiredFlags(QStringLiteral("some-future-variant")).isEmpty());
        QVERIFY(archVariantRequiredFlags(QString()).isEmpty());
        QVERIFY(missingArchVariantFlags(QStringLiteral("alderlake"), QString()).isEmpty());
        QVERIFY(missingArchVariantFlags(QStringLiteral("alderlake"), QStringLiteral("  ")).isEmpty());
    }

    void archVariantNeverRefusesAnEmptyFlagsBody() {
        // A flags line whose label survived but whose body did not carries no flag tokens: it is
        // the same not-knowing as an unreadable line, and must not read as "the CPU lacks avx2".
        for (const QString &line : {QStringLiteral("flags\t:"), QStringLiteral("flags\t: ")}) {
            QVERIFY2(missingArchVariantFlags(QStringLiteral("alderlake"), line).isEmpty(),
                     qPrintable(line));
            QVERIFY2(archVariantRefusal(QStringLiteral("alderlake"), line,
                                        QStringLiteral("remora:test"), QStringLiteral("this host"))
                         .isEmpty(),
                     qPrintable(line));
        }
    }

    // THE REGRESSION THAT ALMOST SHIPPED, and the reason the fixtures above are measured rather
    // than derived from the ISA. -march=alderlake enables -mcldemote, so an ISA-manual reading of
    // the requirement list includes CLDEMOTE — but Intel fuses CLDEMOTE OFF on consumer Alder Lake,
    // and this project's own 12900K build host does not report it. Requiring it refused an
    // alderlake image on the machine that built it. Reads its flags from THIS host at run time, so
    // it cannot be satisfied by a fixture that agrees with a mistake.
    void archVariantNeverRefusesTheHostThatBuiltIt() {
        SKIP_ON_WINDOWS("reads this host's /proc/cpuinfo, which only Linux has");
        QFile f(QStringLiteral("/proc/cpuinfo"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) QSKIP("no /proc/cpuinfo");
        // readAll(), NOT a while(!atEnd()) loop: QFile::atEnd() consults size(), procfs reports
        // size 0, so the loop exits on the first test and the check SKIPS on a host that has the
        // data. It did exactly that here before this comment existed — a silent false pass.
        const QString all = QString::fromUtf8(f.readAll());
        QString line;
        for (const QString &l : all.split(QLatin1Char('\n')))
            if (l.startsWith(QLatin1String("flags"))) { line = l; break; }
        if (line.isEmpty()) QSKIP("no flags line in /proc/cpuinfo");
        // Whatever this machine is, every variant it could plausibly be asked to build for must
        // either be satisfied or be a variant it genuinely is not. The load-bearing case is that a
        // host is never refused a build tuned for its OWN uarch — so check the variant this tree
        // actually uses when the host reports the ISA for it.
        const bool isAlderLake = line.contains(QLatin1String(" avx_vnni"))
                                 && line.contains(QLatin1String(" serialize"));
        if (isAlderLake)
            QVERIFY2(missingArchVariantFlags(QStringLiteral("alderlake"), line).isEmpty(),
                     qPrintable(QStringLiteral("alderlake refused on an Alder Lake host, missing: ")
                                + missingArchVariantFlags(QStringLiteral("alderlake"), line)
                                      .join(QLatin1Char(' '))));
        // x86-64-v3 is the portable target and must pass on any host new enough to run this build.
        if (line.contains(QLatin1String(" avx2")))
            QVERIFY2(missingArchVariantFlags(QStringLiteral("broadwell"), line).isEmpty(),
                     qPrintable(QStringLiteral("broadwell refused on an AVX2 host, missing: ")
                                + missingArchVariantFlags(QStringLiteral("broadwell"), line)
                                      .join(QLatin1Char(' '))));
    }

    void archVariantMatchesWholeTokens() {
        // "avx" is a prefix of "avx2" and "avx_vnni": a substring test would pass a CPU that has
        // neither of the things actually required.
        const QString avxOnly = QStringLiteral("flags\t: fpu sse sse2 avx");
        const QStringList missing = missingArchVariantFlags(QStringLiteral("broadwell"), avxOnly);
        QVERIFY(missing.contains(QStringLiteral("avx2")));
        // sandybridge asks only for avx, and this line genuinely has it.
        QVERIFY(missingArchVariantFlags(QStringLiteral("sandybridge"), avxOnly).isEmpty());
    }

    // bd remora-bm7.13 — the wiring's half of the contract. The refusal TEXT is tested because it is
    // the only thing a person sees at the moment of refusal, and a message that does not name the
    // flags is indistinguishable from the "unsupported, somehow" failure this replaces.
    void archVariantRefusalNamesTheFlagsAndTheImage() {
        const QString r = archVariantRefusal(QStringLiteral("alderlake"), zen1Flags(),
                                             QStringLiteral("remora24:x86_64-gapps-alderlake"),
                                             QStringLiteral("user@amd-host-a"));
        QVERIFY(!r.isEmpty());
        QVERIFY(r.contains(QLatin1String("remora24:x86_64-gapps-alderlake")));
        QVERIFY(r.contains(QLatin1String("alderlake")));
        QVERIFY(r.contains(QLatin1String("user@amd-host-a")));
        for (const char *f : {"avx_vnni", "serialize", "waitpkg"})
            QVERIFY2(r.contains(QLatin1String(f)), f);
    }

    // The three ways of not knowing, each of which used to be a real image on a real host: an image
    // built before the stamp existed, a variant added to soong after this table, and a host whose
    // /proc/cpuinfo the probe could not read (an ssh hiccup reads exactly like this). All three must
    // deploy, unchanged — bd remora-4ei.19 deleted a preflight for doing otherwise.
    void archVariantRefusalIsSilentOnEveryUnknown() {
        const QString tag = QStringLiteral("remora24:x86_64-gapps");
        QVERIFY(archVariantRefusal(QString(), zen1Flags(), tag, QStringLiteral("h")).isEmpty());
        QVERIFY(archVariantRefusal(QStringLiteral("some-future-variant"), zen1Flags(), tag,
                                   QStringLiteral("h"))
                    .isEmpty());
        QVERIFY(archVariantRefusal(QStringLiteral("x86_64"), zen1Flags(), tag,
                                   QStringLiteral("h"))
                    .isEmpty());
        QVERIFY(
            archVariantRefusal(QStringLiteral("alderlake"), QString(), tag, QStringLiteral("h"))
                .isEmpty());
        // ...and the case the whole mechanism is FOR still refuses, so the four above are not
        // passing because the function does nothing.
        QVERIFY(!archVariantRefusal(QStringLiteral("alderlake"), zen1Flags(), tag,
                                    QStringLiteral("h"))
                     .isEmpty());
    }

    // The shared image fetch (vendor/release/catalog.py: `remora images fetch`, the workspace's
    // Official downloads, Android's update-watch) releases its downloaded archive once the image is
    // imported and promoted, and only then. The lifecycle is the helper's own Python, so is its
    // regression; it runs from here, under this suite's isolated HOME.
    void imageFetchReleasesItsArchiveOnlyAfterImport() {
        SKIP_ON_WINDOWS("the helper is Linux tooling: python3 driving docker on the docker host");
        if (QStringLiteral(REMORA_PYTHON).isEmpty())
            QSKIP("no Python 3 interpreter was found at configure time");
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(QStringLiteral(REMORA_PYTHON),
                {sourcePath(QStringLiteral("tests/test_public_images.py")),
                 QStringLiteral("ImportCache")});
        QVERIFY2(p.waitForFinished(120000), "the image-cache regression did not finish");
        const QByteArray out = p.readAll();
        QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0, out.constData());
    }

    // The receipt preflight any release prune would need first: a receipt that lacks its
    // rollback fields is unknown rollback state, never a first install that holds nothing back.
    void imageReceiptRetentionRefusesMissingRollbackFields() {
        SKIP_ON_WINDOWS("the helper is Linux tooling: python3 driving docker on the docker host");
        if (QStringLiteral(REMORA_PYTHON).isEmpty())
            QSKIP("no Python 3 interpreter was found at configure time");
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(QStringLiteral(REMORA_PYTHON),
                {sourcePath(QStringLiteral("tests/test_public_images.py")),
                 QStringLiteral("ReleaseReceiptRetention")});
        QVERIFY2(p.waitForFinished(120000), "the receipt-retention regression did not finish");
        const QByteArray out = p.readAll();
        QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0, out.constData());
    }

    // The release-tag decision a prune would act on: only an image fetch replaced, by its own
    // rollback alias, that no receipt and no container holds; anything unknown selects nothing.
    void imageReleaseRetentionSelectsOnlyOwnedObsoleteTags() {
        SKIP_ON_WINDOWS("the helper is Linux tooling: python3 driving docker on the docker host");
        if (QStringLiteral(REMORA_PYTHON).isEmpty())
            QSKIP("no Python 3 interpreter was found at configure time");
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(QStringLiteral(REMORA_PYTHON),
                {sourcePath(QStringLiteral("tests/test_public_images.py")),
                 QStringLiteral("ReleaseRetention")});
        QVERIFY2(p.waitForFinished(120000), "the release-retention regression did not finish");
        const QByteArray out = p.readAll();
        QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0, out.constData());
    }

    // bd remora-8tpz: --codec reached avcodec_find_encoder_by_name with any explicit name, so the
    // host's ffmpeg could open an encoder Remora never uses — ffv1_vulkan among them. The encoder's
    // main() runs this gate before it opens any device; this drives the same function.
    void hostEncoderRefusesCodecsOutsideItsAllowList() {
        QVERIFY(!remora_encoder_name_allowed("ffv1_vulkan"));
        QVERIFY(!remora_encoder_name_allowed("ffv1"));
        QVERIFY(!remora_encoder_name_allowed(""));
        QVERIFY(!remora_encoder_name_allowed(nullptr));
        // exact match only: neither a prefix nor a case variant of an allowed name gets through
        QVERIFY(!remora_encoder_name_allowed("hevc_vulkan "));
        QVERIFY(!remora_encoder_name_allowed("HEVC"));
        // ...and every backend it already drove still passes, so the gate is not refusing all
        for (const char *n : {"hevc", "h265", "h264", "avc", "hevc_vulkan", "hevc_nvenc",
                              "h264_vulkan", "h264_nvenc", "av1_vulkan", "av1_nvenc"})
            QVERIFY2(remora_encoder_name_allowed(n), n);
    }

    // Whatever a profile's video_codec says, the argv Remora builds for the encoder names a codec
    // the gate accepts: a bad setting must not become an encoder that refuses to start.
    void hostEncoderArgvCodecPassesTheGate() {
        for (const char *vc : {"h264", "h265", "av1", "ffv1_vulkan", ""}) {
            ResolvedConfig c;
            c.hostEncode = true;
            c.videoCodec = QString::fromLatin1(vc);
            const QStringList a = buildHostEncoderArgv(c);
            const qsizetype i = a.indexOf(QStringLiteral("--codec"));
            QVERIFY2(i >= 0 && i + 1 < a.size(), vc);
            QVERIFY2(remora_encoder_name_allowed(a.at(i + 1).toUtf8().constData()), vc);
        }
    }
};

QTEST_MAIN(TestPrereqs)
#include "test_prereqs.moc"
