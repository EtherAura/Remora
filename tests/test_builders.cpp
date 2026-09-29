#include <QtTest>

#include "core/Backend.h"
#include "core/Builders.h"
#include "core/Config.h"
#include "core/Resolved.h"
#include "core/Resolver.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include "core/Gating.h"
#include "core/SourcePatches.h"

using namespace remora;

// A fake vendor tree for the patch fingerprints: vendor-relative path → content, honouring
// readVendorPath's contract (a directory yields every file under it, sorted; a miss yields nothing).
// QMap iterates in key order, which is the sort.
struct FakeVendor {
    QMap<QString, QByteArray> files;
    QList<QPair<QString, QByteArray>> operator()(const QString &rel) const {
        QList<QPair<QString, QByteArray>> out;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            if (it.key() == rel || it.key().startsWith(rel + QLatin1Char('/')))
                out << qMakePair(it.key(), it.value());
        return out;
    }
};

// Names Remora does not describe itself by. Spelled from parts so that this file — which the
// source-tree guard scans along with everything else — does not carry them itself.
static const QStringList &forbiddenNames() {
    static const QStringList names{QStringLiteral("red") + QStringLiteral("roid"),
                                   QStringLiteral("way") + QStringLiteral("droid")};
    return names;
}

// The mark file a guarded command tests, or empty when the command carries no guard.
static QString guardOf(const QString &cmd) {
    static const QRegularExpression re(QStringLiteral("^\\[ -f (\\S+) \\] \\|\\| \\{ "));
    const QRegularExpressionMatch m = re.match(cmd);
    return m.hasMatch() ? m.captured(1) : QString();
}

// A concrete bare ResolvedConfig with literal paths (deterministic on any machine).
static ResolvedConfig bareCfg() {
    ResolvedConfig r;
    r.backend = Backend::Bare;
    r.imageTag = "remora23:x86_64-gapps";
    r.containerName = "remora-bm";
    r.dataDir = "/home/user/.remora-bm-data";
    r.width = 3760;
    r.height = 1992;
    r.dpi = 320;
    r.useMemfd = true;
    r.gpuMode = GpuMode::Guest;
    r.hostAdbPort = 5555;
    r.containerAdbPort = 5556;
    r.mouseBind = "bhsn:++++";
    r.hwDecode = true;  // what the resolver bakes — client-side GPU decode has no off switch in the UI
    r.videoBitRate = "30M";
    r.maxSize = 0;
    r.target = "localhost:5555";
    return r;
}

// A macvlan, host-GPU deployment on an ssh docker host — the shape the old vm profile had,
// which several argv vectors below are written against.
static ResolvedConfig macvlanCfg() {
    ResolvedConfig r = bareCfg();
    r.backend = Backend::Remote;
    r.imageTag = "remora23:x86_64-gapps-wv";
    r.containerName = "remora-lineage";
    r.dataDir = "/home/user/remora-lineage-data";
    r.rezmodsDir = QString("/home/user/rezmods");
    r.gpuMode = GpuMode::Host;
    r.networkMode = "macvlan";
    r.dockerNetwork = QString("lan");
    r.macvlanIp = QString("192.168.0.77");
    r.containerAdbPort = 5555;
    r.target = "192.168.0.77:5555";
    return r;
}

class TestBuilders : public QObject {
    Q_OBJECT
private slots:
    void bareDefaultDockerVector() {
        const QStringList expected{
            "docker", "run", "-d", "--privileged", "--name", "remora-bm",
            "-p", "5555:5556", "-v", "/home/user/.remora-bm-data:/data",
            "remora23:x86_64-gapps",
            "androidboot.remora_gpu_mode=guest", "androidboot.use_memfd=true",
            "androidboot.remora_width=3760", "androidboot.remora_height=1992",
            "androidboot.remora_dpi=320"};
        QCOMPARE(buildDockerRunArgv(bareCfg()), expected);
    }

    // With the host encoder driving the mirror, scrcpy reads the stream from a socket and the
    // device-encoder options must NOT be emitted — they would configure a codec2 encoder this
    // session never starts.
    void hostEncodeScrcpyTakesTheSocket() {
        ResolvedConfig c = bareCfg();
        c.videoCodec = QStringLiteral("h265");
        c.imageTag = QStringLiteral("remora24:x86_64-gapps-wv-hwc2");
        c.hostEncode = true;
        c.hostEncodeDrivesMirror = true;
        c.hostEncodeVideoSocket = QStringLiteral("/tmp/remora-video-remora-bm.sock");
        auto [env, argv] = buildMirrorArgv(c, QStringLiteral("localhost:5555"), false);
        QVERIFY(argv.contains(QStringLiteral("--external-video-socket=/tmp/remora-video-remora-bm.sock")));
        for (const QString &a : argv) {
            QVERIFY2(!a.startsWith(QStringLiteral("--video-codec")), qPrintable(a));
            QVERIFY2(!a.startsWith(QStringLiteral("--video-encoder")), qPrintable(a));
        }
        QVERIFY(env.isEmpty());
    }

    // mouse_bind reaches the client, which is the whole point of the knob: the UI writes a vector
    // and the mirror runs it. Emitted only when it differs from the client's built-in default, so
    // the argv every other profile produces is unchanged (bd remora-28ix.3.3).
    void mouseBindReachesTheMirror() {
        ResolvedConfig c = bareCfg();
        c.mouseBind = QStringLiteral("shbn:++++");
        QVERIFY(buildMirrorArgv(c, QStringLiteral("localhost:5555"), false)
                    .second.contains(QStringLiteral("--mouse-bind=shbn:++++")));

        // The default is what the client already does — saying it again would only add noise to
        // every argv golden.
        ResolvedConfig d = bareCfg();
        d.mouseBind = QStringLiteral("bhsn:++++");
        for (const QString &a : buildMirrorArgv(d, QStringLiteral("localhost:5555"), false).second)
            QVERIFY2(!a.startsWith(QStringLiteral("--mouse-bind")), qPrintable(a));

        ResolvedConfig e = bareCfg();
        e.mouseBind.clear();
        for (const QString &a : buildMirrorArgv(e, QStringLiteral("localhost:5555"), false).second)
            QVERIFY2(!a.startsWith(QStringLiteral("--mouse-bind")), qPrintable(a));
    }

    // The keyboard half of the Navigation card reaches the client too — key_bind was as inert as
    // mouse_bind was (bd remora-28ix.2.6).
    void keyBindingsReachTheMirror() {
        ResolvedConfig c = bareCfg();
        c.keyBind = QStringLiteral("Escape:b");
        c.shortcutMod = QStringLiteral("lctrl+lalt");
        const auto argv = buildMirrorArgv(c, QStringLiteral("localhost:5555"), false).second;
        QVERIFY(argv.contains(QStringLiteral("--key-bind=Escape:b")));
        QVERIFY(argv.contains(QStringLiteral("--shortcut-mod=lctrl+lalt")));

        // Unset stays unset: an empty shortcut_mod means the fork's default, which the client
        // already runs, so saying it would only add noise to every argv golden.
        ResolvedConfig d = bareCfg();
        d.keyBind.clear();
        d.shortcutMod.clear();
        for (const QString &a : buildMirrorArgv(d, QStringLiteral("localhost:5555"), false).second) {
            QVERIFY2(!a.startsWith(QStringLiteral("--key-bind")), qPrintable(a));
            QVERIFY2(!a.startsWith(QStringLiteral("--shortcut-mod")), qPrintable(a));
        }
    }

    // host_encode alone must NOT change the mirror: without a server that can compose, pointing
    // scrcpy at the encoder yields a black window. The device path stays exactly as it was.
    void hostEncodeWithoutForkServerLeavesTheMirrorAlone() {
        ResolvedConfig c = bareCfg();
        c.videoCodec = QStringLiteral("h265");
        c.imageTag = QStringLiteral("remora24:x86_64-gapps-wv-hwc2");
        c.hostEncode = true;
        c.hostEncodeDrivesMirror = false;
        auto [env, argv] = buildMirrorArgv(c, QStringLiteral("localhost:5555"), false);
        QVERIFY(!argv.join(QLatin1Char(' ')).contains(QStringLiteral("--external-video-socket")));
        QVERIFY(argv.contains(QStringLiteral("--video-codec=h265")));
        QVERIFY(argv.contains(QStringLiteral("--video-encoder=c2.remora.vaapi.hevc.encoder")));
        // pinned gone-for-good: the v1 purge (bd remora-28ix.5) deleted --server-path entirely
        for (const QString &t : argv) QVERIFY(!t.startsWith(QStringLiteral("--server-path")));
        Q_UNUSED(env);
    }

    // A --new-display window is not the mirror. The host encoder publishes the MAIN display and
    // serves one consumer, so handing that socket to an app/desktop window showed the launcher at
    // the main display's size, unable to reflow, and killed it shortly after (bd remora-0xh).
    void hostEncodeNeverFeedsANewDisplayWindow() {
        ResolvedConfig c = bareCfg();
        c.videoCodec = QStringLiteral("h265");
        c.imageTag = QStringLiteral("remora24:x86_64-gapps-wv-hwc2");
        c.hostEncode = true;
        c.hostEncodeDrivesMirror = true;
        c.hostEncodeVideoSocket = QStringLiteral("/tmp/remora-video-remora-bm.sock");
        c.fullscreenSeparateDisplay = true;
        c.fullscreenDisplay = QStringLiteral("369x800/143");
        c.fullscreenApp = QStringLiteral("com.anthropic.claude");
        auto [env, argv] = buildMirrorArgv(c, QStringLiteral("localhost:5555"), true);
        QVERIFY(!argv.join(QLatin1Char(' ')).contains(QStringLiteral("--external-video-socket")));
        QVERIFY(argv.contains(QStringLiteral("--new-display=369x800/143")));
        QVERIFY(argv.contains(QStringLiteral("--start-app=com.anthropic.claude")));
        // dropping the socket restores the device-encoder options the host path deliberately omits
        QVERIFY(argv.contains(QStringLiteral("--video-codec=h265")));
        QVERIFY(argv.contains(QStringLiteral("--video-encoder=c2.remora.vaapi.hevc.encoder")));
        Q_UNUSED(env);
        // ...and the same profile's MIRROR is untouched
        auto [menv, margv] = buildMirrorArgv(c, QStringLiteral("localhost:5555"), false);
        QVERIFY(margv.contains(
            QStringLiteral("--external-video-socket=/tmp/remora-video-remora-bm.sock")));
        for (const QString &a : margv)
            QVERIFY2(!a.startsWith(QStringLiteral("--video-codec")), qPrintable(a));
    }

    // The cap is an escape hatch for a GPU that cannot fit the full pipeline, so it must be
    // absent unless asked for — a silently downscaled mirror would be worse than a clear failure.
    void hostEncoderSizeCapIsOptOut() {
        ResolvedConfig c;
        c.hostEncodeFrameSocket = QStringLiteral("/f.sock");
        c.hostEncodeVideoSocket = QStringLiteral("/v.sock");
        c.videoCodec = QStringLiteral("h265");
        QVERIFY(!buildHostEncoderArgv(c).contains(QStringLiteral("--max-size")));
        c.hostEncodeMaxSize = 1280;
        const QStringList a = buildHostEncoderArgv(c);
        QCOMPARE(a.mid(a.indexOf(QStringLiteral("--max-size"))),
                 (QStringList{"--max-size", "1280"}));
    }

    void hostEncoderVector() {
        ResolvedConfig c;
        c.hostEncode = true;
        c.hostEncodeFrameSocket = QStringLiteral("/tmp/remora-frames-remora-bm.sock");
        c.hostEncodeVideoSocket = QStringLiteral("/tmp/remora-video-remora-bm.sock");
        c.videoCodec = QStringLiteral("h265");
        c.videoBitRate = QStringLiteral("30M");
        c.fps = 60;
        const QStringList expected{"remora-frame-encoder",
                                   "--frame-socket", "/tmp/remora-frames-remora-bm.sock",
                                   "--video-socket", "/tmp/remora-video-remora-bm.sock",
                                   "--codec", "hevc",
                                   "--idle-timeout", "600",
                                   "--bit-rate", "30000000",
                                   "--fps", "60"};
        QCOMPARE(buildHostEncoderArgv(c), expected);
    }

    // The idle timeout is not decoration: without it an abandoned encoder held ~2.9 GB of VRAM for
    // seven hours and starved every later one (bd remora-e5x.13). Pinned separately so a future
    // argv edit cannot quietly drop it, and pinned as >= the 240s cold-boot wait because the
    // encoder starts BEFORE that gate — a shorter window would reap healthy encoders mid-boot.
    void hostEncoderAlwaysCarriesIdleTimeout() {
        ResolvedConfig c;
        c.hostEncodeFrameSocket = QStringLiteral("/f.sock");
        c.hostEncodeVideoSocket = QStringLiteral("/v.sock");
        const QStringList a = buildHostEncoderArgv(c);
        const int i = a.indexOf(QStringLiteral("--idle-timeout"));
        QVERIFY(i >= 0);
        QVERIFY(i + 1 < a.size());
        QVERIFY(a.at(i + 1).toInt() >= 240);
    }

    // scrcpy spells bit rates "30M"; ffmpeg wants an integer, so the conversion is part of the
    // contract rather than something the caller is expected to do.
    void hostEncoderBitRateSuffixes() {
        ResolvedConfig c;
        c.hostEncodeFrameSocket = QStringLiteral("/f.sock");
        c.hostEncodeVideoSocket = QStringLiteral("/v.sock");
        c.videoCodec = QStringLiteral("h264");
        c.videoBitRate = QStringLiteral("800K");
        // Read RELATIVE to --bit-rate rather than at a fixed offset. These were .mid(6)/.mid(7)
        // until --idle-timeout was inserted ahead of them, at which point a positional assertion
        // does not fail loudly so much as start checking a different element.
        const auto rateAfter = [](const QStringList &a) {
            const int i = a.indexOf(QStringLiteral("--bit-rate"));
            return i >= 0 && i + 1 < a.size() ? a.at(i + 1) : QString();
        };
        QCOMPARE(buildHostEncoderArgv(c).at(6), QStringLiteral("h264"));
        QCOMPARE(rateAfter(buildHostEncoderArgv(c)), QStringLiteral("800000"));
        c.videoBitRate = QStringLiteral("2500000");
        QCOMPARE(rateAfter(buildHostEncoderArgv(c)), QStringLiteral("2500000"));
        // Unparseable rates are DROPPED rather than passed through: ffmpeg would reject the
        // argument and the encoder would not start at all.
        c.videoBitRate = QStringLiteral("fast");
        QVERIFY(!buildHostEncoderArgv(c).contains(QStringLiteral("--bit-rate")));
    }

    // macvlan is not a vm feature — it is a networking MODE, and bare has to build the same argv
    // for it. The resolver used to overwrite the mode with "bridge" on this backend and the
    // builder had no macvlan branch outside the vm arm, so a bare profile asking for a LAN address
    // silently got a port-mapped bridge container (bd remora-400).
    void bareMacvlanReplacesPortPublish() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.dockerNetwork = QString("lan");
        c.macvlanIp = QString("192.168.0.77");
        const QStringList argv = buildDockerRunArgv(c);
        QCOMPARE(argv[argv.indexOf("--network") + 1], QString("lan"));
        QCOMPARE(argv[argv.indexOf("--ip") + 1], QString("192.168.0.77"));
        // There is no published port in macvlan mode: the container is addressed directly, and a
        // -p here would bind a host port that nothing ever connects to.
        QVERIFY(!argv.contains("-p"));
        QVERIFY(!argv.contains("5555:5556"));
        // …and everything else about the bare argv is untouched by the networking choice.
        QVERIFY(argv.contains("-v"));
        QCOMPARE(argv.last(), QString("androidboot.remora_dpi=320"));
    }

    // bd remora-e5x.37. The mount and the property MOVE TOGETHER — that pairing is the contract,
    // not a coincidence of ordering. A container that mounted the socket without being told its
    // path is indistinguishable from one with no helper at all, and a container told a path it has
    // not mounted fails every connect; both states are unreachable only while one resolved flag
    // emits both.
    void hostDecodeEmitsTheMountAndThePropertyTogether() {
        ResolvedConfig c = bareCfg();
        c.hostDecode = true;
        c.hostDecodeSocket = hostDecodeSocketFor(c.containerName);
        const QStringList argv = buildDockerRunArgv(c);
        const int mount = argv.indexOf(QStringLiteral("/tmp/remora-decode-%1:/dev/remora-decode")
                                               .arg(c.containerName));
        QVERIFY(mount > 0);
        QCOMPARE(argv[mount - 1], QString("-v"));  // it really is a bind mount, not a stray token
        QVERIFY(argv.contains("androidboot.remora_decoder_socket=/dev/remora-decode/decode.sock"));
        // The DIRECTORY is mounted, never the socket file: the helper recreates that inode on every
        // restart, so a file mount would leave the container holding a deleted one.
        QVERIFY(!argv.contains(QStringLiteral("%1:/dev/remora-decode/decode.sock")
                                       .arg(c.hostDecodeSocket)));
    }

    // Off by default, and that is what keeps the guest decoder inert: with no property emitted,
    // RemoteVideoDecoder::configured() is false and every stream uses the local Intel VA decoder.
    // Host decode is also INDEPENDENT of host encode — they are opposite directions through
    // different helpers, and decode going remote is exactly what frees the local VADisplay for
    // encode, so enabling one must never imply the other.
    void hostDecodeIsOffByDefaultAndIndependentOfHostEncode() {
        const QStringList plain = buildDockerRunArgv(bareCfg());
        QVERIFY(!plain.contains("androidboot.remora_decoder_socket=/dev/remora-decode/decode.sock"));
        QVERIFY(!plain.contains("-v") || !plain.join(' ').contains("/dev/remora-decode"));

        ResolvedConfig enc = bareCfg();
        enc.hostEncode = true;
        enc.hostEncodeFrameSocket = QStringLiteral("/tmp/remora-frames-x/frame.sock");
        const QStringList encArgv = buildDockerRunArgv(enc);
        QVERIFY(encArgv.contains("androidboot.remora_frame_socket=/dev/remora-frames/frame.sock"));
        QVERIFY(!encArgv.join(' ').contains("/dev/remora-decode"));
    }

    // bd remora-e5x.37. THE PROBE AND THE SESSION MUST NAME THE SAME DEVICE. Measured on the
    // NVIDIA host: `--probe --drm-node renderD128` alone exits 1, because libva defaults to iHD
    // and iHD cannot drive that node, while the same probe WITH `--driver nvidia` exits 0. A probe
    // that answered for a different device than the session opens would have gated host decode OFF
    // on precisely the hardware the feature exists for — which is what the first version of this
    // did. One shared rule, pinned here.
    void hostDecoderProbeAndSessionNameTheSameDevice() {
        ResolvedConfig c = bareCfg();
        c.hostDecode = true;
        c.hostDecodeSocket = hostDecodeSocketFor(c.containerName);
        c.venusNode = QString("/dev/dri/renderD128");
        c.vaDriver = QString("nvidia");

        const QStringList session = buildHostDecoderArgv(c);
        const QStringList probe = buildHostDecoderProbeArgv(c);
        for (const QString &flag : {QStringLiteral("--drm-node"), QStringLiteral("--driver")}) {
            const int si = session.indexOf(flag), pi = probe.indexOf(flag);
            QVERIFY2(si > 0 && pi > 0, qPrintable(flag));
            QCOMPARE(probe[pi + 1], session[si + 1]);
        }
        QVERIFY(probe.contains("--probe"));
        QVERIFY(!probe.contains("--decode-socket"));  // a probe binds nothing

        // The NVIDIA node is chosen over the xe one: decode on the Intel node would forfeit the
        // same-GPU win this whole feature exists for.
        QCOMPARE(session[session.indexOf("--drm-node") + 1], QString("/dev/dri/renderD128"));
    }

    // No driver named => none passed, which is what lets the Intel node probe clean with libva's
    // own default and the AMD hosts run through the same helper with no second code path.
    void hostDecoderOmitsTheDriverWhenUnset() {
        ResolvedConfig c = bareCfg();
        c.hostDecode = true;
        c.hostDecodeSocket = hostDecodeSocketFor(c.containerName);
        c.gpuNode = QString("/dev/dri/renderD129");
        QVERIFY(!buildHostDecoderProbeArgv(c).contains("--driver"));
        QCOMPARE(buildHostDecoderArgv(c)[buildHostDecoderArgv(c).indexOf("--drm-node") + 1],
                 QString("/dev/dri/renderD129"));
    }

    void bareMacvlanNoIpOmitsIpFlag() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.dockerNetwork = QString("lan");
        const QStringList argv = buildDockerRunArgv(c);
        QCOMPARE(argv[argv.indexOf("--network") + 1], QString("lan"));
        QVERIFY(!argv.contains("--ip"));
    }

    void macvlanCreateArgvGolden() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.dockerNetwork = QString("lan");
        c.macvlanParent = QString("UMN");
        c.macvlanSubnet = QString("192.168.0.0/24");
        c.macvlanGateway = QString("192.168.0.1");
        c.macvlanRange = QString("192.168.0.240/28");
        const QStringList expected{"docker", "network", "create", "-d", "macvlan",
                                   "--subnet", "192.168.0.0/24", "--gateway", "192.168.0.1",
                                   "--ip-range", "192.168.0.240/28", "-o", "parent=UMN", "lan"};
        QCOMPARE(buildMacvlanCreateArgv(c), expected);
        // No range resolved -> docker IPAM gets the whole subnet, and --ip-range is absent
        // entirely rather than emitted empty.
        c.macvlanRange = std::nullopt;
        QVERIFY(!buildMacvlanCreateArgv(c).contains("--ip-range"));
    }

    // An incomplete shape yields NO command at all. The alternative — creating the network on a
    // partially guessed subnet — produces a container that boots, gets an address, and is routable
    // from nowhere, which is far harder to diagnose than a refusal.
    void macvlanCreateArgvEmptyWhenShapeIncomplete() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.dockerNetwork = QString("lan");
        c.macvlanParent = QString("UMN");
        c.macvlanGateway = QString("192.168.0.1");
        QVERIFY(buildMacvlanCreateArgv(c).isEmpty());  // no subnet
        c.macvlanSubnet = QString("192.168.0.0/24");
        c.macvlanParent = std::nullopt;
        QVERIFY(buildMacvlanCreateArgv(c).isEmpty());  // no parent
        c.macvlanParent = QString("UMN");
        c.dockerNetwork = std::nullopt;
        QVERIFY(buildMacvlanCreateArgv(c).isEmpty());  // no name
        // …and bridge mode never builds one, however complete the shape is.
        c.dockerNetwork = QString("lan");
        c.networkMode = "bridge";
        QVERIFY(buildMacvlanCreateArgv(c).isEmpty());
    }

    // The host shim: one privileged container in the host netns, no sudo anywhere. Golden because
    // it is a command that reconfigures the OPERATOR'S machine — every token in it should have to
    // be changed on purpose.
    void macvlanShimArgvGolden() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.dockerNetwork = QString("lan");
        c.macvlanIp = QString("192.168.0.78");
        c.macvlanParent = QString("UMN");
        c.macvlanSubnet = QString("192.168.0.0/24");
        c.macvlanRange = QString("192.168.0.240/28");
        c.macvlanHostRoute = true;
        c.macvlanShimIp = QString("192.168.0.239");
        const QStringList argv = buildMacvlanShimArgv(c);
        // The chroot is NOT decoration. busybox's own `ip` accepts "mode bridge" and silently
        // builds a VEPA interface, where sibling macvlan devices cannot reach each other — the
        // shim comes up looking perfect and routes nothing. Only real iproute2, reached through
        // the host filesystem, honours the mode. Asserted token-for-token so it cannot be
        // "simplified" back into a plain busybox call.
        QCOMPARE(argv.mid(0, 11),
                 QStringList({"docker", "run", "--rm", "--privileged", "--network", "host", "-v",
                              "/:/host:ro", "busybox", "chroot", "/host"}));
        QCOMPARE(argv.at(11), QString("/bin/sh"));
        QCOMPARE(argv.at(12), QString("-c"));
        const QString script = argv.last();
        // Remora OWNS this interface, so the script reconciles rather than creates. The two
        // structural properties cannot be changed on a live device and are what force a rebuild:
        // the parent it hangs off, and the mode that decides whether siblings can talk at all.
        QVERIFY(script.contains(QStringLiteral("case \"$cur\" in *remora-shim0@UMN:*)")));
        QVERIFY(script.contains(QStringLiteral("case \"$cur\" in *'macvlan mode bridge'*)")));
        QVERIFY(script.contains(QStringLiteral("ip link del remora-shim0")));
        QVERIFY(script.contains(
            QStringLiteral("ip link add remora-shim0 link UMN type macvlan mode bridge")));
        QVERIFY(script.contains(QStringLiteral("ip addr replace 192.168.0.239/32")));
        // Wanted routes asserted…
        QVERIFY(script.contains(QStringLiteral("want='192.168.0.240/28 192.168.0.78'")));
        QVERIFY(script.contains(
            QStringLiteral("for w in $want; do ip route replace $w dev remora-shim0")));
        // …and unwanted ones withdrawn, or a re-pinned container leaves its old /32 routed.
        QVERIFY(script.contains(QStringLiteral("ip route del")));
    }

    // The wanted-route list is compared against `ip route show` output, which prints a host route
    // BARE. Spelling it "/32" here would match nothing, and every connect would delete the route it
    // had just installed — a shim that flaps once per deploy.
    void macvlanShimRoutesUseThePrintedFormOfAHostRoute() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.macvlanRange = QString("192.168.0.240/28");
        c.macvlanIp = QString("192.168.0.78");
        QCOMPARE(macvlanShimRoutes(c), QStringList({"192.168.0.240/28", "192.168.0.78"}));
    }

    // A pin INSIDE the pool needs no extra /32 — the range already covers it. A pin outside does,
    // and that is the case a shim built from the range alone would leave stranded.
    void macvlanShimRoutesCoverPinnedAddressesOutsideThePool() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.macvlanRange = QString("192.168.0.240/28");
        QCOMPARE(macvlanShimRoutes(c), QStringList({"192.168.0.240/28"}));
        c.macvlanIp = QString("192.168.0.245");  // inside the pool
        QCOMPARE(macvlanShimRoutes(c), QStringList({"192.168.0.240/28"}));
        c.macvlanIp = QString("192.168.0.78");   // outside it
        QCOMPARE(macvlanShimRoutes(c), QStringList({"192.168.0.240/28", "192.168.0.78"}));
    }

    void macvlanShimArgvEmptyWhenOptedOutOrUnresolvable() {
        ResolvedConfig c = bareCfg();
        c.networkMode = "macvlan";
        c.macvlanParent = QString("UMN");
        c.macvlanRange = QString("192.168.0.240/28");
        c.macvlanShimIp = QString("192.168.0.239");
        c.macvlanHostRoute = false;
        QVERIFY(buildMacvlanShimArgv(c).isEmpty());  // opted out
        c.macvlanHostRoute = true;
        c.macvlanShimIp = std::nullopt;
        QVERIFY(buildMacvlanShimArgv(c).isEmpty());  // no address to give the shim
        c.macvlanShimIp = QString("192.168.0.239");
        c.macvlanParent = std::nullopt;
        QVERIFY(buildMacvlanShimArgv(c).isEmpty());  // no parent to hang it off
        c.macvlanParent = QString("UMN");
        c.macvlanRange = std::nullopt;
        QVERIFY(buildMacvlanShimArgv(c).isEmpty());  // nothing to route
        // …and bridge mode never builds one.
        c.macvlanRange = QString("192.168.0.240/28");
        c.networkMode = "bridge";
        QVERIFY(buildMacvlanShimArgv(c).isEmpty());
    }

    void vmNoCapsOmitsGpuNode() {
        const QStringList argv = buildDockerRunArgv(macvlanCfg());
        QVERIFY(argv.contains("androidboot.remora_gpu_mode=host"));
        for (const QString &a : argv)
            QVERIFY(!a.startsWith("androidboot.remora_gpu_node="));
        QCOMPARE(argv[argv.indexOf("--network") + 1], QString("lan"));
        QCOMPARE(argv[argv.indexOf("--ip") + 1], QString("192.168.0.77"));
    }

    void vmMacvlanNoIpOmitsIpFlag() {
        // optional macvlan IP: unset -> docker IPAM assigns; --ip must be absent entirely
        ResolvedConfig c = macvlanCfg();
        c.macvlanIp = std::nullopt;
        const QStringList argv = buildDockerRunArgv(c);
        QCOMPARE(argv[argv.indexOf("--network") + 1], QString("lan"));
        QVERIFY(!argv.contains("--ip"));
    }

    // bd remora-4ei.57 — bare host GPU mode: the resolved node must reach the container both as a
    // --device and as the boot arg, and the gralloc override must be emitted alongside it.
    void bareHostGpuPassesNodeAndGralloc() {
        ResolvedConfig c = bareCfg();
        c.gpuMode = GpuMode::Host;
        c.gpuNode = QString("/dev/dri/renderD129");
        c.gralloc = QString("minigbm_intel");
        const QStringList argv = buildDockerRunArgv(c);
        QCOMPARE(argv[argv.indexOf("--device") + 1],
                 QString("/dev/dri/renderD129:/dev/dri/renderD129"));
        QVERIFY(argv.contains("androidboot.remora_gpu_mode=host"));
        QVERIFY(argv.contains("androidboot.remora_gpu_node=/dev/dri/renderD129"));
        QVERIFY(argv.contains("androidboot.remora_gralloc=minigbm_intel"));
        // the vm path's whole-directory passthrough must NOT leak into bare
        QVERIFY(!argv.contains("/dev/dri"));
    }

    // bd remora-4ei.80 — VA driver selection rides the probe. amdgpu maps to radeonsi and emits
    // the boot arg; Intel (xe) maps to NOTHING because iHD is the c2-va .rc's own default; an
    // unmapped driver also emits nothing, so `check` reports the software fallback instead of a
    // wrong driver name hiding it; an explicit va_driver= wins over every mapping.
    // Since the v1 purge (bd remora-28ix.5) host encode DRIVES the mirror by itself: the agent
    // composes natively, so there is no compose-capable jar to gate on. PIP still opts out on
    // its own ResolvedConfig copy (buildPipMirrorArgv clears the flag).
    void hostEncodeDrivesTheMirrorByItself() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        c.mirror.hostEncode = false;
        QVERIFY(!resolve(c, Backend::Bare, caps).hostEncodeDrivesMirror);
        c.mirror.hostEncode = true;
        const auto composed = resolve(c, Backend::Bare, caps);
        QVERIFY(composed.hostEncode);
        QVERIFY(composed.hostEncodeDrivesMirror);
    }

    // bd remora-28ix.2.2 — opt-in audio reaches the mirror argv, and ONLY the main mirror argv:
    // a PIP with its own audio stream would double-play everything the main window plays.
    void mirrorAudioIsOptInAndMainWindowOnly() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");

        QVERIFY(!resolve(c, Backend::Bare, caps).mirrorAudio);  // off unless asked

        c.mirror.audio = true;
        ResolvedConfig rc = resolve(c, Backend::Bare, caps);
        QVERIFY(rc.mirrorAudio);
        QVERIFY(buildMirrorArgv(rc, QStringLiteral("t"), false).second.contains("--audio"));
        QVERIFY(!buildPipMirrorArgv(rc, QStringLiteral("t")).second.contains("--audio"));
    }

    void resolverVaDriverFollowsProbedDriver() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("amdgpu");
        QCOMPARE(*resolve(c, Backend::Bare, caps).vaDriver, QStringLiteral("radeonsi"));
        caps.hostRenderDriver = QStringLiteral("xe");
        QVERIFY(!resolve(c, Backend::Bare, caps).vaDriver.has_value());
        caps.hostRenderDriver = QStringLiteral("nouveau");
        QVERIFY(!resolve(c, Backend::Bare, caps).vaDriver.has_value());
        c.gpu.vaDriver = QStringLiteral("radeonsi");
        QCOMPARE(*resolve(c, Backend::Bare, caps).vaDriver, QStringLiteral("radeonsi"));
        // guest mode is SwiftShader: nothing GPU-shaped resolves, the VA override included
        RemoraConfig guest;
        QVERIFY(!resolve(guest, Backend::Bare, caps).vaDriver.has_value());
    }

    // bd remora-e5x.1 — the multi-GPU selector. gpu_driver= picks the LIVE node with that driver
    // (node numbers rotate across boots, drivers do not), and every per-driver rule follows the
    // SELECTION: picking amdgpu on a mixed box gets amdgpu's node, the cros gralloc (the image
    // default renders but SIGFPEs on the encoder's YUV allocations, bd remora-4ei.69), and the
    // radeonsi VA driver — picking xe gets minigbm_intel. gpu_node= exact path still wins.
    // bd remora-wlun — the namespace fold behind the smart-connect drift compare. androidboot.
    // strips, remora_ folds INSIDE the key (use_remora_c2 folds to use_c2), values stay untouched,
    // and non-androidboot tokens drop — the container's .Args section arrives interleaved with
    // binds/devs lines the caller never has to strip.
    // ONE NAMESPACE (bd remora-28ix.4 step 4): remora_ is stripped, a key in any other namespace
    // is left ALONE (so it compares different and triggers the recreate — this builder did not
    // bake it), and the namespace-neutral keys are untouched either way.
    void foldBootArgsNormalizesTheCurrentNamespaceOnly() {
        const QStringList folded = foldBootArgs(
            QStringList{QStringLiteral("androidboot.remora_gpu_mode=host"),
                        QStringLiteral("androidboot.legacy_gpu_mode=host"),
                        QStringLiteral("androidboot.use_remora_c2=1"),
                        QStringLiteral("androidboot.va_driver=radeonsi"),
                        QStringLiteral("androidboot.use_memfd=true"),
                        QStringLiteral("binds=/x:/y"), QStringLiteral("devs=/dev/foo")});
        QCOMPARE(folded,
                 (QStringList{QStringLiteral("gpu_mode=host"),
                              QStringLiteral("legacy_gpu_mode=host"),
                              QStringLiteral("use_c2=1"),
                              QStringLiteral("va_driver=radeonsi"),
                              QStringLiteral("use_memfd=true")}));
        // the load-bearing consequence, spelled out: the two spellings no longer compare equal
        QVERIFY(foldBootArgs({QStringLiteral("androidboot.remora_gpu_mode=host")})
                != foldBootArgs({QStringLiteral("androidboot.legacy_gpu_mode=host")}));
        // …and buildBootArgs is the docker argv's own tail, verbatim: the gate compares exactly
        // what a deploy would emit.
        RemoraConfig c;
        const ResolvedConfig r = resolve(c, Backend::Remote);
        const QStringList argv = buildDockerRunArgv(r);
        const QStringList boot = buildBootArgs(r);
        QVERIFY(!boot.isEmpty());
        QCOMPARE(argv.mid(argv.size() - boot.size()), boot);
    }

    // bd remora-n4qx — on a multi-GPU host, every NON-selected GPU's nodes are masked with
    // /dev/null binds: --device only states the selection, the privileged container sees all of
    // /dev/dri, and the in-container stack discovers devices itself (amd-host-b rendered on a
    // TeraScale relic while everything Remora controls named the RX 550). Single-GPU hosts must
    // produce an empty list and a byte-identical argv.
    void resolverMasksNonSelectedGpuNodes() {
        HostCapabilities caps;
        caps.renderNodes = {RenderNode{QStringLiteral("/dev/dri/renderD128"),
                                       QStringLiteral("amdgpu"), QStringLiteral("/dev/dri/card0")},
                            RenderNode{QStringLiteral("/dev/dri/renderD129"),
                                       QStringLiteral("radeon"), QStringLiteral("/dev/dri/card1")}};
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD128");
        caps.hostRenderDriver = QStringLiteral("amdgpu");
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        const ResolvedConfig r = resolve(c, Backend::Remote, caps);
        QCOMPARE(r.maskedGpuNodes,
                 (QStringList{QStringLiteral("/dev/dri/renderD129"),
                              QStringLiteral("/dev/dri/card1")}));
        // …and the argv carries the binds, after the --device statement of intent.
        const QStringList argv = buildDockerRunArgv(r);
        QVERIFY(argv.contains(QStringLiteral("/dev/null:/dev/dri/renderD129")));
        QVERIFY(argv.contains(QStringLiteral("/dev/null:/dev/dri/card1")));
        QVERIFY(!argv.contains(QStringLiteral("/dev/null:/dev/dri/renderD128")));
        // A card sibling the scan could not pair masks the render node alone.
        caps.renderNodes[1].card.clear();
        QCOMPARE(resolve(c, Backend::Remote, caps).maskedGpuNodes,
                 QStringList{QStringLiteral("/dev/dri/renderD129")});
        // Single GPU: nothing masked, argv untouched.
        caps.renderNodes = {RenderNode{QStringLiteral("/dev/dri/renderD128"),
                                       QStringLiteral("amdgpu"), QStringLiteral("/dev/dri/card0")}};
        const ResolvedConfig one = resolve(c, Backend::Remote, caps);
        QVERIFY(one.maskedGpuNodes.isEmpty());
        QVERIFY(buildDockerRunArgv(one).filter(QStringLiteral("/dev/null:")).isEmpty());
        // Venus: the NVIDIA node is deliberately a second GPU — exempt from masking; a THIRD
        // GPU still masks.
        caps.renderNodes = {RenderNode{QStringLiteral("/dev/dri/renderD129"),
                                       QStringLiteral("xe"), QStringLiteral("/dev/dri/card0")},
                            RenderNode{QStringLiteral("/dev/dri/renderD128"),
                                       QStringLiteral("nvidia"), QStringLiteral("/dev/dri/card1")},
                            RenderNode{QStringLiteral("/dev/dri/renderD130"),
                                       QStringLiteral("radeon"), QStringLiteral("/dev/dri/card2")}};
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        caps.hostNvidiaRenderNode = QStringLiteral("/dev/dri/renderD128");
        RemoraConfig v;
        v.gpu.mode = GpuMode::Host;
        v.gpu.venus = true;
        const ResolvedConfig rv = resolve(v, Backend::Bare, caps);
        QVERIFY(!rv.maskedGpuNodes.contains(QStringLiteral("/dev/dri/renderD128")));
        QVERIFY(rv.maskedGpuNodes.contains(QStringLiteral("/dev/dri/renderD130")));
        QVERIFY(rv.maskedGpuNodes.contains(QStringLiteral("/dev/dri/card2")));
    }

    void resolverGpuDriverSelectsAmongNodes() {
        HostCapabilities caps;
        caps.renderNodes = {RenderNode{QStringLiteral("/dev/dri/renderD129"), QStringLiteral("xe")},
                            RenderNode{QStringLiteral("/dev/dri/renderD130"),
                                       QStringLiteral("amdgpu")}};
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");  // the probe's first pick
        caps.hostRenderDriver = QStringLiteral("xe");
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        // auto: first pick, xe rules
        QCOMPARE(*resolve(c, Backend::Bare, caps).gpuNode, QStringLiteral("/dev/dri/renderD129"));
        QCOMPARE(*resolve(c, Backend::Bare, caps).gralloc, QStringLiteral("minigbm_intel"));
        // select amdgpu: its node, the cros gralloc, radeonsi VA — the rules follow the pick
        c.gpu.gpuDriver = QStringLiteral("amdgpu");
        const ResolvedConfig amd = resolve(c, Backend::Bare, caps);
        QCOMPARE(*amd.gpuNode, QStringLiteral("/dev/dri/renderD130"));
        QCOMPARE(*amd.gralloc, QStringLiteral("cros"));
        QCOMPARE(*amd.vaDriver, QStringLiteral("radeonsi"));
        // exact node still beats the driver pick — and gets ITS driver's rules via the node list
        c.gpu.gpuNode = QStringLiteral("/dev/dri/renderD129");
        const ResolvedConfig exact = resolve(c, Backend::Bare, caps);
        QCOMPARE(*exact.gpuNode, QStringLiteral("/dev/dri/renderD129"));
        QCOMPARE(*exact.gralloc, QStringLiteral("minigbm_intel"));
        QVERIFY(!exact.vaDriver.has_value());  // xe = iHD = the image default, nothing emitted
        // a requested driver with no live node: node stays unset (preflight names it), and the
        // rules still follow the REQUEST rather than silently reverting to another GPU's
        RemoraConfig gone;
        gone.gpu.mode = GpuMode::Host;
        gone.gpu.gpuDriver = QStringLiteral("amdgpu");
        HostCapabilities xeOnly;
        xeOnly.renderNodes = {
            RenderNode{QStringLiteral("/dev/dri/renderD129"), QStringLiteral("xe")}};
        xeOnly.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        xeOnly.hostRenderDriver = QStringLiteral("xe");
        const ResolvedConfig g = resolve(gone, Backend::Bare, xeOnly);
        QVERIFY(!g.gpuNode.has_value());
        QVERIFY(!g.gralloc.has_value());
        QCOMPARE(*g.vaDriver, QStringLiteral("radeonsi"));
    }

    // bd remora-4ei.9 — host cameras into the bare container. The PRIVACY GATE is the point
    // under test: auto passes the live-probed nodes ONLY when the profile opted into the
    // camera_v4l2 feature; no feature = no camera reaches a container nobody asked to have one.
    // An explicit list is exact, and an explicitly EMPTY list is "off", not "auto".
    void resolverCameraDevicesAreFeatureGated() {
        HostCapabilities caps;
        caps.videoNodes = {QStringLiteral("/dev/video0"), QStringLiteral("/dev/video1")};
        RemoraConfig c;
        QVERIFY(resolve(c, Backend::Bare, caps).cameraDevices.isEmpty());  // no feature
        c.image.features = QStringList{QStringLiteral("camera_v4l2")};
        QCOMPARE(resolve(c, Backend::Bare, caps).cameraDevices, caps.videoNodes);  // auto
        c.input.cameraDevices = QStringList{QStringLiteral("/dev/video1")};  // exact
        QCOMPARE(resolve(c, Backend::Bare, caps).cameraDevices,
                 QStringList{QStringLiteral("/dev/video1")});
        c.input.cameraDevices = QStringList{};  // explicitly OFF beats the feature's auto
        QVERIFY(resolve(c, Backend::Bare, caps).cameraDevices.isEmpty());
    }

    void bareCameraDevicesReachDockerArgv() {
        ResolvedConfig c = bareCfg();
        c.cameraDevices = {QStringLiteral("/dev/video0"), QStringLiteral("/dev/video2")};
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains(QStringLiteral("/dev/video0:/dev/video0")));
        QVERIFY(argv.contains(QStringLiteral("/dev/video2:/dev/video2")));
        // absent = absent: no camera token of any kind without a resolved device
        c.cameraDevices.clear();
        for (const QString &a : buildDockerRunArgv(c))
            QVERIFY(!a.contains(QLatin1String("/dev/video")));
    }

    // bd remora-6279 — the trimmed cpufreq view. ON by default, because a desktop's topology is
    // not a neutral truth to pass through: 24 policies overran TikTok's 16-slot per-cluster table
    // by 640 bytes (bd remora-e5x.28), and fixed tables sized for phones are ordinary in native
    // Android code. Off is expressible for measuring the host's own cpufreq from the guest.
    void resolverCpufreqTopologyDefaultsOn() {
        RemoraConfig c;
        QVERIFY(resolve(c, Backend::Bare).cpufreqTopology);   // unset ⇒ on
        QVERIFY(resolve(c, Backend::Remote).cpufreqTopology);  // …on both backends
        c.input.cpufreqTopology = false;
        QVERIFY(!resolve(c, Backend::Bare).cpufreqTopology);
        c.input.cpufreqTopology = true;
        QVERIFY(resolve(c, Backend::Bare).cpufreqTopology);
    }

    void bareCpufreqTopologyReachesDockerArgv() {
        ResolvedConfig c = bareCfg();
        c.cpufreqDir = QStringLiteral("/home/u/.cache/remora/cpufreq-remora-x");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains(QStringLiteral(
            "/home/u/.cache/remora/cpufreq-remora-x:/sys/devices/system/cpu/cpufreq:ro")));
        // ABSENT IS ABSENT, and this is the case that matters most: the flag alone must never
        // emit a bind, because the directory is generated by the deploy chain and a bind to a
        // path nothing generated is answered by docker with an EMPTY directory — a container
        // seeing zero cpu clusters, which is worse than the 24 this feature exists to trim.
        c.cpufreqDir.clear();
        for (const QString &a : buildDockerRunArgv(c))
            QVERIFY2(!a.contains(QLatin1String("cpu/cpufreq")), qPrintable(a));
    }

    // The generator is shell that runs on the DOCKER host, so it is golden-tested like every
    // other command this file builds. Pins the contract rather than the text: cluster by max
    // frequency, name by lowest CPU, write into a sibling and move into place.
    void cpufreqTopologyCommandIsSafeToRerun() {
        const QString cmd = buildCpufreqTopologyCmd(QStringLiteral("/c/cpufreq-x"));
        QVERIFY(cmd.contains(QStringLiteral("/sys/devices/system/cpu/cpufreq")));  // the source
        QVERIFY(cmd.contains(QStringLiteral("out=/c/cpufreq-x")));                 // the target
        // clustered by max frequency, deduplicated and ordered fastest-first
        QVERIFY(cmd.contains(QStringLiteral("cpuinfo_max_freq")));
        QVERIFY(cmd.contains(QStringLiteral("sort -run")));
        // THE DIRECTORY'S INODE MUST SURVIVE. A bind mount resolves to the inode, not the path,
        // so replacing the directory (sibling + mv) leaves a RUNNING container bound to the one
        // moved aside — and removing that empties its view to zero policies mid-session. Found on
        // the first live deploy, where host-prereqs regenerates while the old container is up.
        QVERIFY2(!cmd.contains(QStringLiteral("mv \"$tmp\" \"$out\"")),
                 "regeneration replaces the directory — a live mount would be left behind");
        QVERIFY2(!cmd.contains(QStringLiteral("$out.new")), qPrintable(cmd));
        // …and stale policies are removed only AFTER the wanted ones exist, so a reader caught
        // mid-regeneration sees a superset, never an empty directory.
        QVERIFY(cmd.indexOf(QStringLiteral("keep=\"$keep policy$lo\""))
                < cmd.indexOf(QStringLiteral("rm -rf \"$d\"")));
        // membership comes from related_cpus, which unlike affected_cpus includes offline CPUs
        QVERIFY(cmd.contains(QStringLiteral("related_cpus")));
        // the never-fewer-policies-than-the-host fallback
        QVERIFY(cmd.contains(QStringLiteral("cp -a \"$src\"/policy*")));
        // CPU COUNT IS NOT TOUCHED: nothing here may write to the cpu[0-9]* enumeration, which
        // stays honest so nproc and every scheduler decision keep seeing the real machine.
        QVERIFY2(!cmd.contains(QStringLiteral("/sys/devices/system/cpu/cpu0")), qPrintable(cmd));
    }

    void bareVaDriverReachesBootArgs() {
        ResolvedConfig c = bareCfg();
        c.gpuMode = GpuMode::Host;
        c.gpuNode = QString("/dev/dri/renderD129");
        c.vaDriver = QString("radeonsi");
        QVERIFY(buildDockerRunArgv(c)
                    .contains("androidboot.va_driver=radeonsi"));
        // absent = absent, the anti-stale-bake shape: the .rc default (iHD) must apply untouched
        c.vaDriver.reset();
        for (const QString &a : buildDockerRunArgv(c))
            QVERIFY(!a.startsWith("androidboot.va_driver="));
    }

    // bd remora-4ei.56 — the NVIDIA Venus stack. Every piece below is load-bearing: the guest
    // reaches NVIDIA ONLY through the socket, so a missing mount is a silently SwiftShader'd
    // container rather than a loud failure.
    void bareVenusEmitsSocketLibsAndBuildProp() {
        ResolvedConfig c = bareCfg();
        c.gpuMode = GpuMode::Host;
        c.gpuNode = QString("/dev/dri/renderD129");   // xe — gbm + the VAAPI encoder open this
        c.gralloc = QString("minigbm_gbm_mesa");
        c.venus = true;
        c.venusNode = QString("/dev/dri/renderD128");  // nvidia
        c.venusSocketDir = QString("/home/u/.cache/remora/venus");
        c.venusLibDir = QString("/home/u/.local/share/remora/venus/guest-libs");
        c.buildPropOverlay = QString("/home/u/.cache/remora/venus-build.prop-img");
        const QStringList argv = buildDockerRunArgv(c);
        // BOTH nodes: NVIDIA renders, but the Intel node is what gbm and the encoder open.
        QVERIFY(argv.contains("/dev/dri/renderD129:/dev/dri/renderD129"));
        QVERIFY(argv.contains("/dev/dri/renderD128:/dev/dri/renderD128"));
        // The socket DIRECTORY, not the socket file — the server recreates that inode on restart.
        QVERIFY(argv.contains("/home/u/.cache/remora/venus:/dev/venus"));
        // The container-side path is pinned by mesa.vtest.socket.name in the generated build.prop,
        // so /dev/venus here and that prop must move together or Vulkan finds nothing.
        for (const auto &lib : venusGuestLibs())
            QVERIFY(argv.contains(QStringLiteral("/home/u/.local/share/remora/venus/guest-libs/%1:%2:ro")
                                      .arg(lib.first, lib.second)));
        QVERIFY(argv.contains(
            "/home/u/.cache/remora/venus-build.prop-img:/vendor/build.prop:ro"));
        QVERIFY(argv.contains("androidboot.remora_gralloc=minigbm_gbm_mesa"));
    }

    // venus=false must leave the vector byte-identical to a box where the feature does not exist.
    void bareVenusOffEmitsNothing() {
        ResolvedConfig c = bareCfg();
        c.gpuMode = GpuMode::Host;
        c.gpuNode = QString("/dev/dri/renderD129");
        c.gralloc = QString("minigbm_intel");
        // Populated but disabled: a stale profile must not leak mounts just because paths resolved.
        c.venus = false;
        c.venusNode = QString("/dev/dri/renderD128");
        c.venusSocketDir = QString("/home/u/.cache/remora/venus");
        c.venusLibDir = QString("/home/u/.local/share/remora/venus/guest-libs");
        c.buildPropOverlay = QString("/home/u/.cache/remora/venus-build.prop-img");
        const QStringList argv = buildDockerRunArgv(c);
        for (const QString &a : argv) {
            QVERIFY(!a.contains("/dev/venus"));
            QVERIFY(!a.contains("guest-libs"));
            QVERIFY(!a.contains("venus-build.prop"));
            QVERIFY(!a.contains("renderD128"));
        }
    }

    // bd remora-e5x.33 — gpu_mode=guest carries the build.prop overlay bind WITHOUT any of the
    // venus mounts: the image bakes ro.hardware.vulkan=intel, ro.* is first-wins over
    // gpu_setup_guest's setprop, and a host with no Intel GPU then loops SurfaceFlinger in
    // chooseEglConfig. The overlay is the only extra mount software rendering needs.
    void guestModeEmitsBuildPropOverlayAlone() {
        ResolvedConfig c = bareCfg();
        c.gpuMode = GpuMode::Guest;
        c.venus = false;
        c.buildPropOverlay = QString("/home/u/.cache/remora/guest-build.prop-img");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains(
            "/home/u/.cache/remora/guest-build.prop-img:/vendor/build.prop:ro"));
        for (const QString &a : argv) {
            QVERIFY(!a.contains("/dev/venus"));
            QVERIFY(!a.contains("guest-libs"));
        }
        // …and host mode with no venus must not emit a bind even if the path is populated —
        // the same no-leak rule bareVenusOffEmitsNothing pins for the venus mounts.
        ResolvedConfig h = bareCfg();
        h.gpuMode = GpuMode::Host;
        h.venus = false;
        h.buildPropOverlay = QString("/home/u/.cache/remora/guest-build.prop-img");
        for (const QString &a : buildDockerRunArgv(h))
            QVERIFY(!a.contains("build.prop"));
    }

    // bd remora-4ei.59 — the netfix pair must ride in as read-only bind mounts. A post-boot
    // `docker cp` cannot work: init remounts the container rootfs read-only, and cp has to create
    // a .pivot_root dir at the rootfs root, so it fails for every destination path.
    // bd remora-4u4.3 — the destination is /remora, and must stay OUTSIDE /data: init's
    // use_overlayfs overlay covers /data and overlay layers do not traverse mounts, so a
    // destination beneath /data is shadowed and adb can never be wired.
    void bareNetfixPairBindMountedReadOnly() {
        ResolvedConfig c = bareCfg();
        c.netfixFwdPath = QString("/opt/remora/vendor/native/fwd");
        c.netfixScriptPath = QString("/opt/remora/vendor/container-scripts/remora-netfix.sh");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/opt/remora/vendor/native/fwd:/remora/fwd:ro"));
        QVERIFY(argv.contains(
            "/opt/remora/vendor/container-scripts/remora-netfix.sh:/remora/netfix.sh:ro"));
        // the invariant that replaced "must be a sub-bind of /data": no netfix destination may
        // live under /data, with or without overlayfs.
        for (const QString &a : argv) QVERIFY(!a.contains(":/data/local/tmp/"));
    }

    // bd remora-yn4 — the forwarder's init service. Its destination is the ONE netfix-related
    // bind that may NOT be /remora: Android init only parses .rc files from the partitions' own
    // init directories, so anywhere else and it is simply never read. Mounting it is what makes a
    // container restarted OUTSIDE Remora come back with a working forwarder — previously such a
    // container came back with Android healthy, no fwd, and adb dead until the next deploy, which
    // nothing in Remora could notice because Remora was not running.
    void bareForwarderInitServiceBindMountedIntoVendorInit() {
        ResolvedConfig c = bareCfg();
        c.netfixInitRcPath = QString("/opt/remora/vendor/container-scripts/remora-fwd.rc");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/opt/remora/vendor/container-scripts/remora-fwd.rc"
                              ":/vendor/etc/init/remora-fwd.rc:ro"));
        // Read-only like every other helper bind, and NOT under /data — the overlayfs overlay
        // would shadow it there exactly as it would the netfix pair.
        for (const QString &a : argv)
            if (a.contains("remora-fwd.rc")) {
                QVERIFY(a.endsWith(":ro"));
                QVERIFY(!a.contains(":/data/"));
            }
        // Unset stays unset: a profile with no init .rc resolved must emit no such bind, or docker
        // would create an empty DIRECTORY at the source path and init would parse nothing.
        ResolvedConfig none = bareCfg();
        for (const QString &a : buildDockerRunArgv(none)) QVERIFY(!a.contains("remora-fwd.rc"));
    }

    // bd remora-emoe phase 2 — the composer publishes composed frames to the host encoder, and BOTH
    // halves must ride the same argv or the feature is silently half-present: the container needs
    // the socket's DIRECTORY bound (a file bind cannot survive the encoder recreating the inode,
    // the same trap the venus bind's comment records) and the HAL needs to be told where it landed.
    // The two are separate lines in separate blocks, which is exactly how one gets dropped.
    void hostEncodeBindsTheFrameSocketDirAndTellsTheComposer() {
        ResolvedConfig c = bareCfg();
        c.hostEncode = true;
        c.containerName = QString("remora-a17");
        c.hostEncodeFrameSocket = QString("/tmp/remora-frames-remora-a17/frame.sock");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/tmp/remora-frames-remora-a17:/dev/remora-frames"));
        QVERIFY(argv.contains("androidboot.remora_frame_socket=/dev/remora-frames/frame.sock"));
        // The container-side path the HAL is told must be INSIDE the directory the argv bound, or
        // the composer connects to nothing and the failure is a silently black compose mode.
        QVERIFY(argv.contains("androidboot.remora_frame_socket=/dev/remora-frames/frame.sock")
                && argv.contains("/tmp/remora-frames-remora-a17:/dev/remora-frames"));
        // …and neither appears when host encode is off, so an ordinary profile's argv is untouched.
        ResolvedConfig off = bareCfg();
        for (const QString &a : buildDockerRunArgv(off)) {
            QVERIFY(!a.contains("remora-frames"));
            QVERIFY(!a.contains("remora_frame_socket"));
        }
    }

    // bd remora-28ix.4 — the androidboot.remora_* namespace adapter rides the forwarder's road:
    // bound into /vendor/etc/init by the SAME argv that emits the remora_* boot args, so no
    // reachable deploy pairs the new names with an image that cannot read them.
    void bootPropsShimBindMountedIntoVendorInit() {
        ResolvedConfig c = bareCfg();
        c.propsInitRcPath = QString("/opt/remora/vendor/container-scripts/remora.props.rc");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/opt/remora/vendor/container-scripts/remora.props.rc"
                              ":/vendor/etc/init/remora.props.rc:ro"));
        for (const QString &a : argv)
            if (a.contains("remora.props.rc")) {
                QVERIFY(a.endsWith(":ro"));
                QVERIFY(!a.contains(":/data/"));
            }
        ResolvedConfig none = bareCfg();
        for (const QString &a : buildDockerRunArgv(none)) QVERIFY(!a.contains("remora.props.rc"));
    }

    // REPLACED (bd remora-28ix.4 step 4). This used to require a stanza in
    // remora.props.rc for EVERY emitted androidboot.remora_* key, because the file was an ADAPTER:
    // it mapped the new namespace onto the inherited names that in-image consumers still read, and
    // a key emitted with no stanza would have been silently dropped on the way in. That file is no
    // longer an adapter — its consumers left the image — and what survives in it are two stanzas
    // that set a DIFFERENT property from the one they match on, which no key-coverage rule can
    // describe. Requiring coverage now would demand inventing a name nothing reads for every key.
    //
    // The replacement contract is narrower and still catches the failure that mattered: the file
    // must do exactly its two derivations and must NOT have quietly regrown into a mapper.
    void bootPropsRcDerivesOnlyWhatCannotBeDeferred() {
        QFile rc(QStringLiteral(REMORA_SOURCE_VENDOR_DIR)
                 + QStringLiteral("/container-scripts/remora.props.rc"));
        QVERIFY(rc.open(QIODevice::ReadOnly));
        const QString shim = QString::fromUtf8(rc.readAll());

        // lcd_density cannot be deferred to another early-init action: init matches all early-init
        // property actions when the event is dequeued, before any of their commands run, so a
        // setprop here cannot arm one elsewhere. It has to be derived in the same action.
        QVERIFY(shim.contains(QStringLiteral("property:ro.boot.remora_dpi=*")));
        QVERIFY(shim.contains(QStringLiteral("setprop ro.sf.lcd_density ${ro.boot.remora_dpi}")));
        // the ICD is a per-deploy fact, so it arrives as a boot arg and lands here (bd remora-xqt1)
        QVERIFY(shim.contains(QStringLiteral("property:ro.boot.remora_vulkan=*")));
        QVERIFY(shim.contains(QStringLiteral("setprop ro.hardware.vulkan ${ro.boot.remora_vulkan}")));

        // NOT A MAPPER ANY MORE: no stanza may set a pre-migration name. This is the regression
        // guard — reintroducing one revives two properties for every one, and a second name that
        // nothing reads is exactly what step 4 removed.
        for (const QString &line : shim.split(QLatin1Char('\n'))) {
            if (line.trimmed().startsWith(QLatin1Char('#'))) continue;
            for (const QString &name : forbiddenNames())
                QVERIFY2(!line.contains(name, Qt::CaseInsensitive), qPrintable(line));
        }
        // and it stays byte-identical on both delivery roads (bake + bind-mount)
        QFile baked(QStringLiteral(REMORA_SOURCE_VENDOR_DIR)
                    + QStringLiteral("/source-build/vendor-remora/remora.props.rc"));
        QVERIFY(baked.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(baked.readAll()), shim);
    }

    // Remora is its own project and does not describe itself relative to another one: no source,
    // test, fixture or build file may carry forbiddenNames(), in code, data or comments alike.
    void sourceTreeCarriesNoForbiddenNames() {
        const QString root = QDir(QStringLiteral(REMORA_SOURCE_VENDOR_DIR "/..")).absolutePath();
        QStringList files{root + QStringLiteral("/CMakeLists.txt")};
        for (const QString &sub : {QStringLiteral("src"), QStringLiteral("tests")}) {
            QDirIterator it(root + QLatin1Char('/') + sub, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) files << it.next();
        }
        QVERIFY2(files.size() > 20, "the walk did not find the source tree");
        for (const QString &path : files) {
            QFile f(path);
            QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(path));
            const QString text = QString::fromUtf8(f.readAll());
            for (const QString &name : forbiddenNames())
                QVERIFY2(!text.contains(name, Qt::CaseInsensitive),
                         qPrintable(path + QStringLiteral(" carries ") + name));
        }
    }

    // One shim, two delivery roads — the bind mount above, and the copy vendor/remora bakes into
    // the image (bd remora-28ix.4 R2).
    // They must stay BYTE-IDENTICAL: the mount SHADOWS the bake, so a divergence would ship
    // different behaviour depending on which image is underneath, invisibly — and the baked copy
    // is the one an image uses when something other than Remora starts it.
    void bootPropsShimBakeAndMountAreIdentical() {
        QFile mount(QStringLiteral(REMORA_SOURCE_VENDOR_DIR)
                    + QStringLiteral("/container-scripts/remora.props.rc"));
        QVERIFY(mount.open(QIODevice::ReadOnly));
        QFile baked(QStringLiteral(REMORA_SOURCE_VENDOR_DIR)
                    + QStringLiteral("/source-build/vendor-remora/remora.props.rc"));
        QVERIFY2(baked.open(QIODevice::ReadOnly),
                 "vendor/remora has no remora.props.rc — the image would boot with no boot-property "
                 "adapter at all whenever it is started outside Remora");
        QCOMPARE(QString::fromUtf8(mount.readAll()), QString::fromUtf8(baked.readAll()));
    }

    // bd remora-28ix.3 — the mirror agent rides both roads: the dex to /remora beside the netfix
    // pair, its init service to /vendor/etc/init beside the forwarder's. Same read-only mounts,
    // same not-under-/data rule, and the same all-or-nothing gate: an rc whose class path is
    // missing is a service init retries every 5s forever.
    void bareAgentDexAndInitServiceBindMounted() {
        ResolvedConfig c = bareCfg();
        c.agentDexPath = QString("/opt/remora/vendor/agent/agent.dex");
        c.agentInitRcPath = QString("/opt/remora/vendor/container-scripts/remora-agent.rc");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/opt/remora/vendor/agent/agent.dex:/remora/agent.dex:ro"));
        QVERIFY(argv.contains("/opt/remora/vendor/container-scripts/remora-agent.rc"
                              ":/vendor/etc/init/remora-agent.rc:ro"));
        for (const QString &a : argv)
            if (a.contains("agent.dex") || a.contains("remora-agent.rc")) {
                QVERIFY(a.endsWith(":ro"));
                QVERIFY(!a.contains(":/data/"));
            }
        // Unbuilt agent: no binds at all, so docker cannot turn a missing source into an empty
        // directory mounted over the dex.
        ResolvedConfig none = bareCfg();
        for (const QString &a : buildDockerRunArgv(none)) {
            QVERIFY(!a.contains("agent.dex"));
            QVERIFY(!a.contains("remora-agent.rc"));
        }
    }

    // bd remora-28ix.3.4 — a baked-agent image rides with NO agent mounts, even though the host
    // paths resolved (the dex exists for the dev loop and for other, unbaked containers). The
    // baked rc already defines `service remora_agent`; init discards a second definition as a
    // duplicate, and which of the two it read first is a parse-order accident — so the dev dex
    // could silently shadow the jar the image shipped. The drop is keyed on the IMAGE, not on
    // what the host happens to have built.
    void bareAgentMountsDroppedWhenImageBakesAgent() {
        ResolvedConfig c = bareCfg();
        c.agentDexPath = QString("/opt/remora/vendor/agent/agent.dex");
        c.agentInitRcPath = QString("/opt/remora/vendor/container-scripts/remora-agent.rc");
        c.agentBaked = true;
        for (const QString &a : buildDockerRunArgv(c)) {
            QVERIFY(!a.contains("agent.dex"));
            QVERIFY(!a.contains("remora-agent.rc"));
        }
    }

    // bd remora-28ix.3.4 — where agentBaked comes from: the profile requesting the mirror_agent
    // build feature (a source build stages it whether or not any tag advertises it), or the
    // resolved tag being registered as providing it. Since the flip, the A17 tags advertise the
    // capability and the A17 default set carries the feature — so an unset A17 profile is baked
    // by DEFAULT, while A16 (out of plan, golden images without the agent) stays unbaked.
    void agentBakedFollowsMirrorAgentFeature() {
        RemoraConfig c;  // A16 default profile: no agent anywhere
        QVERIFY(!resolve(c, Backend::Bare).agentBaked);
        QVERIFY(!resolve(c, Backend::Remote).agentBaked);
        c.image.features = QStringList{QStringLiteral("mirror_agent")};
        QVERIFY(resolve(c, Backend::Bare).agentBaked);
        QVERIFY(resolve(c, Backend::Remote).agentBaked);
        // An explicit image_tag does not clear it: the feature list is the build request, and the
        // pinned tag is just the name that build was promoted under — here one whose registry
        // entry does not advertise the agent, so the feature list alone is what marks it baked.
        c.image.imageTag = QString("remora23:x86_64-gapps");
        QVERIFY(resolve(c, Backend::Bare).agentBaked);
        // The post-flip shape: an UNSET A17 profile selects a tag that advertises the agent, and
        // the registry arm marks it baked with no feature pin anywhere in the config.
        RemoraConfig a17;
        a17.image.androidVersion = 17;
        QVERIFY(resolve(a17, Backend::Bare).agentBaked);
    }

    // bd remora-gf7 — the phantom keyboard rides in beside the netfix pair, and for the same
    // reasons: read-only, and OUTSIDE /data so the overlayfs overlay cannot shadow it. Without a
    // keyboard device Android reports Configuration `nokeys` and draws the on-screen IME over
    // every text field.
    void barePhantomKeyboardBindMountedReadOnly() {
        ResolvedConfig c = bareCfg();
        c.uinputKbdPath = QString("/opt/remora/vendor/native/uinput-kbd");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/opt/remora/vendor/native/uinput-kbd:/remora/uinput-kbd:ro"));
        for (const QString &a : argv) QVERIFY(!a.contains(":/data/local/tmp/"));
    }

    // Unset means no mount at all — an empty ResolvedConfig must still resolve to a bootable
    // instance, and the helper is engine-supplied (applyVendorPaths), never baked into core.
    void bareNoPhantomKeyboardMountWhenUnset() {
        const QStringList argv = buildDockerRunArgv(bareCfg());
        for (const QString &a : argv) QVERIFY(!a.contains("uinput-kbd"));
    }

    // bd remora-4u4.3 — the whole point: under overlayfs the pair still rides, unchanged. Before
    // the /remora move this combination was unusable (netfix shadowed -> no adb -> no mirror).
    void bareNetfixPairSurvivesOverlayfs() {
        ResolvedConfig c = bareCfg();
        c.useOverlayfs = true;
        c.dataBaseDir = "/home/user/.remora-data-base";
        c.netfixFwdPath = QString("/opt/remora/vendor/native/fwd");
        c.netfixScriptPath = QString("/opt/remora/vendor/container-scripts/remora-netfix.sh");
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("/opt/remora/vendor/native/fwd:/remora/fwd:ro"));
        QVERIFY(argv.contains(
            "/opt/remora/vendor/container-scripts/remora-netfix.sh:/remora/netfix.sh:ro"));
        // and the overlay pair is what /data is made of, so nothing may sit beneath it
        QVERIFY(argv.contains(QStringLiteral("/home/user/.remora-data-base:/data-base")));
        for (const QString &a : argv) QVERIFY(!a.contains(":/data/"));
    }

    // vm resolves no netfix paths (its netfix is folded into the guest bringup), so nothing rides.
    void vmEmitsNoNetfixMounts() {
        const QStringList argv = buildDockerRunArgv(macvlanCfg());
        for (const QString &a : argv) {
            QVERIFY(!a.endsWith("/remora/fwd:ro"));
            QVERIFY(!a.endsWith("/remora/netfix.sh:ro"));
        }
    }

    // anti-stale-bake, the bare half: no node resolved -> neither token, and no --device at all.
    void bareGuestModeEmitsNoGpuDevice() {
        const QStringList argv = buildDockerRunArgv(bareCfg());
        QVERIFY(!argv.contains("--device"));
        for (const QString &a : argv) {
            QVERIFY(!a.startsWith("androidboot.remora_gpu_node="));
            QVERIFY(!a.startsWith("androidboot.remora_gralloc="));
        }
    }

    // gralloc is independent of the node: an explicit override on a non-xe host must still ride.
    void grallocEmittedWithoutXeDefault() {
        ResolvedConfig c = bareCfg();
        c.gpuMode = GpuMode::Host;
        c.gpuNode = QString("/dev/dri/renderD128");
        c.gralloc = std::nullopt;  // unknown driver -> image default (gbm) applies
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("androidboot.remora_gpu_node=/dev/dri/renderD128"));
        for (const QString &a : argv) QVERIFY(!a.startsWith("androidboot.remora_gralloc="));
    }

    // The composer HAL A/B (bd remora-emoe / 28ix.4 R4). Both HALs are installed in every image and
    // hw_get_module resolves the library from ro.hardware.hwcomposer at runtime, so this one token
    // is the whole switch — which is what keeps trying Remora's HAL a container restart instead of
    // an image rebuild. UNSET MUST EMIT NOTHING: an empty token would set the property to the empty
    // string, and hw_get_module would then look for hwcomposer..so and find no composer at all.
    void hwcomposerSelectionRidesOnlyWhenAsked() {
        ResolvedConfig c = bareCfg();
        for (const QString &a : buildDockerRunArgv(c))
            QVERIFY(!a.startsWith("androidboot.remora_hwc="));

        c.hwcomposer = QString("remora");
        QVERIFY(buildDockerRunArgv(c).contains("androidboot.remora_hwc=remora"));

        // Guest mode too: the composer is selected the same way whichever GPU path is in use, so
        // tying this to the host-GPU resolution would make it silently inert on SwiftShader.
        ResolvedConfig g = bareCfg();
        g.gpuMode = GpuMode::Guest;
        g.hwcomposer = QString("remora");
        QVERIFY(buildDockerRunArgv(g).contains("androidboot.remora_hwc=remora"));
    }

    void mirrorDefaultVectorAndEnv() {
        const auto [env, argv] = buildMirrorArgv(bareCfg(), "localhost:5555");
        // The in-house client is flag-configured: no SDL vars, no env at all.
        QVERIFY(env.isEmpty());
        // Client-side GPU decode is on for every profile: auto picks NVDEC/VAAPI itself and
        // falls back to software when the host has no usable decoder, so an off switch only ever
        // bought a slower mirror. It still has to be switchable from remorarc.
        RemoraConfig empty;
        QVERIFY(resolve(empty, Backend::Bare).hwDecode);
        empty.mirror.hwDecode = false;
        QVERIFY(!resolve(empty, Backend::Bare).hwDecode);
        ResolvedConfig sw = bareCfg();
        sw.hwDecode = false;
        QVERIFY(!buildMirrorArgv(sw, "localhost:5555").second.contains("--hwdec=auto"));
        // The golden default vector: `remora mirror` plus only what the profile resolves. Input
        // behavior (sdk-style touch, nav buttons, Alt chords) and stretched rendering are built
        // into the client — no flags exist for them.
        const QStringList expected{
            "remora", "mirror", "-s", "localhost:5555", "--max-size=0", "--video-bit-rate=30M",
            "--hwdec=auto"};
        QCOMPARE(argv, expected);
    }

    void scrcpyMaxFpsCapWiring() {
        // Unset (the ≤60fps proven default) → no flag; the default vector stays stable.
        ResolvedConfig c = bareCfg();
        auto [e0, a0] = buildMirrorArgv(c, "localhost:5555");
        for (const QString &t : a0) QVERIFY(!t.startsWith("--max-fps"));
        // Resolved cap → emitted (the fork forwards it to the c2-va drop, remora-rjh).
        c.maxFps = 60;
        auto [e1, a1] = buildMirrorArgv(c, "localhost:5555");
        QVERIFY(a1.contains("--max-fps=60"));
    }

    // The Viewer's Navigation card is an ACTION-first view of two button/letter-indexed scrcpy
    // knobs, so the resolved default has to give every action a distinct trigger — the old bhbn
    // put Back on both right-click and the 4th button and left Recents unreachable, which the card
    // cannot even display, let alone edit back.
    void resolverBakesNavigationDefaults() {
        // The resolver bakes the navigation knobs (the GUI's Navigation card edits them) and the
        // client now HONOURS them: mouse_bind, key_bind and shortcut_mod all reach it as flags
        // (bd remora-28ix.2.6, which is what made the card's settings real). scrcpy_keyboard has
        // no flag — sdk-style typing is built in and has no alternative to select.
        RemoraConfig c;
        QCOMPARE(resolve(c, Backend::Bare).mouseBind, QStringLiteral("bhsn:++++"));
        QCOMPARE(resolve(c, Backend::Remote).mouseBind, QStringLiteral("bhsn:++++"));
        QVERIFY(resolve(c, Backend::Bare).shortcutMod.isEmpty());
        c.input.mouseBind = QStringLiteral("++++:bhsn");
        c.input.shortcutMod = QStringLiteral("lsuper");
        c.input.keyBind = QStringLiteral("Pause:b");
        QCOMPARE(resolve(c, Backend::Bare).mouseBind, QStringLiteral("++++:bhsn"));
        QCOMPARE(resolve(c, Backend::Bare).shortcutMod, QStringLiteral("lsuper"));
        QCOMPARE(resolve(c, Backend::Bare).keyBind, QStringLiteral("Pause:b"));
        ResolvedConfig r = bareCfg();
        r.shortcutMod = "lctrl+lalt";
        r.keyBind = "F1:b,F2:h,F3:s";
        const auto argv = buildMirrorArgv(r, "localhost:5555").second;
        QVERIFY(argv.contains(QStringLiteral("--shortcut-mod=lctrl+lalt")));
        QVERIFY(argv.contains(QStringLiteral("--key-bind=F1:b,F2:h,F3:s")));
        for (const QString &t : argv)
            QVERIFY2(!t.startsWith("--keyboard"), qPrintable(t));
    }

    void resolverBakesSixtyFpsDefault() {
        // Regression guard for bd remora-bm7.1: an empty config MUST resolve to 60 Hz. While this
        // was `r.fps = d.fps`, a blank field emitted no fps boot arg at all and the image
        // fell back to its own built-in 30 — the entire UI ran at half rate, undetected, while the
        // GUI placeholder, tooltip and card subtitle all promised 60. An explicit value still wins.
        RemoraConfig c;
        QCOMPARE(*resolve(c, Backend::Bare).fps, 60);
        QCOMPARE(*resolve(c, Backend::Remote).fps, 60);
        QVERIFY(buildDockerRunArgv(resolve(c, Backend::Bare))
                    .contains(QStringLiteral("androidboot.remora_fps=60")));
        c.display.fps = 120;
        QCOMPARE(*resolve(c, Backend::Remote).fps, 120);
    }

    // bd remora-4ei.57 — the bare backend used to hardcode gpuNode = nullopt, so gpu_mode=host
    // half-applied: host mode was emitted with no node and no /dev/dri, which leaves the image's
    // gpu_config.sh unable to find a node (its auto-scan case list has no xe entry) and silently
    // on SwiftShader. The node must come from the live probe, never a bake.
    void resolverBareHostGpuUsesLiveXeNode() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QCOMPARE(r.gpuMode, GpuMode::Host);
        QCOMPARE(*r.gpuNode, QStringLiteral("/dev/dri/renderD129"));
        // xe confirmed by the probe -> minigbm_intel, because gbm_gralloc SIGFPEs on xe buffers
        QCOMPARE(*r.gralloc, QStringLiteral("minigbm_intel"));
    }

    // bd remora-4ei.56 — Venus AUTO. The whole point of the three-part precondition is that
    // enabling this by default can never switch a box onto a stack whose pieces are absent.
    void resolverVenusAutoNeedsAllThreePrerequisites() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        // NVIDIA node alone is not enough — the host prerequisites still have to be installed.
        caps.hostNvidiaRenderNode = QStringLiteral("/dev/dri/renderD128");
        QVERIFY(!resolve(c, Backend::Bare, caps).venus);
        // server but no libs
        caps.venusServerPath = QStringLiteral("/opt/venus/bin/virgl_test_server");
        QVERIFY(!resolve(c, Backend::Bare, caps).venus);
        // libs but no NVIDIA node — an Intel-only box must never be switched onto this path
        caps.venusLibDir = QStringLiteral("/opt/venus/guest-libs");
        HostCapabilities noNvidia = caps;
        noNvidia.hostNvidiaRenderNode = std::nullopt;
        QVERIFY(!resolve(c, Backend::Bare, noNvidia).venus);
        // all three present -> auto-on
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QVERIFY(r.venus);
        QCOMPARE(*r.venusNode, QStringLiteral("/dev/dri/renderD128"));
        // gpuNode stays the XE node: NVIDIA is reached only over the socket, and the Intel node is
        // what the gbm wrapper and the VAAPI encoder open.
        QCOMPARE(*r.gpuNode, QStringLiteral("/dev/dri/renderD129"));
        // Venus must beat the xe->minigbm_intel rule, or allocations never reach the render server.
        QCOMPARE(*r.gralloc, QStringLiteral("minigbm_gbm_mesa"));
    }

    void resolverVenusExplicitFalsePinsItOff() {
        // Everything available, but the operator said no — auto must not override an explicit key.
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        c.gpu.venus = false;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        caps.hostNvidiaRenderNode = QStringLiteral("/dev/dri/renderD128");
        caps.venusServerPath = QStringLiteral("/opt/venus/bin/virgl_test_server");
        caps.venusLibDir = QStringLiteral("/opt/venus/guest-libs");
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QVERIFY(!r.venus);
        QVERIFY(!r.venusNode.has_value());
        QCOMPARE(*r.gralloc, QStringLiteral("minigbm_intel"));  // back to the xe rule
    }

    // bd remora-xqt1 — the Vulkan ICD is resolved HOST-side, because the image cannot work it out:
    // gpu_config.sh's setup_vulkan reads /sys/kernel/debug/dri, which is not mounted in a Remora
    // container, so it has never set anything on any host. The Venus case is the one that proves
    // the decision has to live here rather than being a repaired in-image lookup.
    void resolverVulkanIcdFollowsTheRenderPathNotTheNode() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");

        caps.hostRenderDriver = QStringLiteral("xe");
        QCOMPARE(*resolve(c, Backend::Bare, caps).vulkan, QStringLiteral("intel"));
        caps.hostRenderDriver = QStringLiteral("amdgpu");
        QCOMPARE(*resolve(c, Backend::Bare, caps).vulkan, QStringLiteral("radeon"));

        // An unmapped driver emits NOTHING rather than a plausible guess — same rule as va_driver,
        // so a missing ICD shows up as missing instead of as the wrong one.
        caps.hostRenderDriver = QStringLiteral("someothergpu");
        QVERIFY(!resolve(c, Backend::Bare, caps).vulkan.has_value());

        // THE CASE THAT SETTLES IT. With Venus the selected node stays the INTEL one — that is
        // where gralloc allocates — while rendering leaves for NVIDIA over vtest. Reading the
        // node's own driver would answer "intel"; the right answer is virtio, and only the host
        // knows that, because only the host decided to route through Venus at all.
        caps.hostRenderDriver = QStringLiteral("xe");
        caps.hostNvidiaRenderNode = QStringLiteral("/dev/dri/renderD128");
        caps.venusServerPath = QStringLiteral("/opt/venus/bin/virgl_test_server");
        caps.venusLibDir = QStringLiteral("/opt/venus/guest-libs");
        const ResolvedConfig venus = resolve(c, Backend::Bare, caps);
        QVERIFY(venus.venus);
        QCOMPARE(*venus.gpuNode, QStringLiteral("/dev/dri/renderD129"));  // still the Intel node
        QCOMPARE(*venus.vulkan, QStringLiteral("virtio"));

        // An explicit vulkan= is the user's statement and beats every rule above, Venus included.
        c.gpu.vulkan = QStringLiteral("pastel");
        QCOMPARE(*resolve(c, Backend::Bare, caps).vulkan, QStringLiteral("pastel"));
    }

    // Guest mode has no node and no gralloc, but it DOES have a Vulkan answer, and it is the one
    // the image cannot supply for itself: gpu_setup_guest's own setprop of pastel loses to
    // /vendor/build.prop, which init loads before any action runs (bd remora-xqt1).
    void resolverVulkanIcdIsPastelInGuestMode() {
        RemoraConfig c;  // no gpu.mode -> bare defaults to Guest
        HostCapabilities caps;
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QCOMPARE(r.gpuMode, GpuMode::Guest);
        QVERIFY(!r.gpuNode.has_value());
        QVERIFY(!r.gralloc.has_value());
        QCOMPARE(*r.vulkan, QStringLiteral("pastel"));
        QVERIFY(buildDockerRunArgv(r).contains(QStringLiteral("androidboot.remora_vulkan=pastel")));
    }

    void resolverVenusNeverOnGuestMode() {
        // Guest mode is SwiftShader by definition. Venus is a HOST-GPU path, so a box with every
        // prerequisite installed must still resolve to plain SwiftShader here.
        RemoraConfig c;  // no gpu.mode -> bare defaults to Guest
        HostCapabilities caps;
        caps.hostNvidiaRenderNode = QStringLiteral("/dev/dri/renderD128");
        caps.venusServerPath = QStringLiteral("/opt/venus/bin/virgl_test_server");
        caps.venusLibDir = QStringLiteral("/opt/venus/guest-libs");
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QCOMPARE(r.gpuMode, GpuMode::Guest);
        QVERIFY(!r.venus);
    }

    void resolverBareHostGpuWithoutProbeHasNoNode() {
        // No caps = no live probe. Host mode still resolves (the user asked for it) but nothing is
        // invented to point it at — the preflight is what reports the gap.
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        const ResolvedConfig r = resolve(c, Backend::Bare);
        QCOMPARE(r.gpuMode, GpuMode::Host);
        QVERIFY(!r.gpuNode.has_value());
        QVERIFY(!r.gralloc.has_value());
    }

    void resolverEmptyConfigStillBootsBareOnGuest() {
        // The load-bearing invariant: an empty RemoraConfig resolves to a bootable instance. No GPU
        // config must mean SwiftShader with nothing GPU-shaped emitted at all.
        RemoraConfig c;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QCOMPARE(r.gpuMode, GpuMode::Guest);
        QVERIFY(!r.gpuNode.has_value());
        QVERIFY(!r.gralloc.has_value());
    }

    void resolverGrallocOnlyClaimedForConfirmedXe() {
        // An explicitly configured node the probe did NOT identify as xe must keep the image's own
        // default (gbm) — guessing minigbm_intel on some other vendor's driver would be wrong.
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        c.gpu.gpuNode = QStringLiteral("/dev/dri/renderD128");
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QCOMPARE(*r.gpuNode, QStringLiteral("/dev/dri/renderD128"));
        QVERIFY(!r.gralloc.has_value());
        // ...but naming the xe node explicitly gets the same treatment as the probed one
        c.gpu.gpuNode = QStringLiteral("/dev/dri/renderD129");
        QCOMPARE(*resolve(c, Backend::Bare, caps).gralloc, QStringLiteral("minigbm_intel"));
        // ...and an explicit override always wins
        c.gpu.gralloc = QStringLiteral("gbm");
        QCOMPARE(*resolve(c, Backend::Bare, caps).gralloc, QStringLiteral("gbm"));
    }

    // bd remora-4ei.72 — the minigbm_intel rule is a DRIVER decision, not a node-identity one. A
    // probed node on some other driver (the amdgpu box, bd remora-4ei.69) must still be used as
    // the default node, but claim NO gralloc: the image default (gbm) is correct there, and
    // matching on the node alone would smuggle the xe override onto every future driver.
    // An UNMAPPED driver claims no gralloc — the image default stands until a driver's needs are
    // measured. amdgpu is no longer the example here: it grew its own mapping (cros) when the
    // default was measured to SIGFPE on the encoder's YUV allocations (bd remora-4ei.69).
    void resolverProbedUnmappedDriverClaimsNoGralloc() {
        RemoraConfig c;
        c.gpu.mode = GpuMode::Host;
        HostCapabilities caps;
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("nouveau");
        const ResolvedConfig r = resolve(c, Backend::Bare, caps);
        QCOMPARE(*r.gpuNode, QStringLiteral("/dev/dri/renderD129"));
        QVERIFY(!r.gralloc.has_value());
        // …and the probe landing on amdgpu claims cros, the same way xe claims minigbm_intel.
        caps.hostRenderDriver = QStringLiteral("amdgpu");
        QCOMPARE(*resolve(c, Backend::Bare, caps).gralloc, QStringLiteral("cros"));
    }

    void resolverAutoCapsAboveSixtyFps() {
        // fps ≤ 60 (or unset) → no cap; fps > 60 → auto-cap 60 (the SF-backpressure guard);
        // an explicit max_fps always wins.
        RemoraConfig c;
        QCOMPARE(resolve(c, Backend::Remote).maxFps.has_value(), false);
        c.display.fps = 60;
        QCOMPARE(resolve(c, Backend::Remote).maxFps.has_value(), false);
        c.display.fps = 240;
        QCOMPARE(*resolve(c, Backend::Remote).maxFps, 60);
        c.display.maxFps = 90;
        QCOMPARE(*resolve(c, Backend::Remote).maxFps, 90);
    }

    // Audio is one switch. --audio is the only audio flag the mirror takes, so with mirrorAudio
    // off no audio token may appear at all, and with it on exactly that one — never a codec or
    // buffer option the client does not have (the retired audio_codec/audio_buffer keys).
    void mirrorEmitsAudioFlagOnlyWhenAudioIsOn() {
        ResolvedConfig c = bareCfg();
        for (const QString &t : buildMirrorArgv(c, "localhost:5555").second)
            QVERIFY2(!t.contains(QLatin1String("audio")), qPrintable(t));
        c.mirrorAudio = true;
        QStringList audio;
        for (const QString &t : buildMirrorArgv(c, "localhost:5555").second)
            if (t.contains(QLatin1String("audio"))) audio << t;
        QCOMPARE(audio, QStringList{QStringLiteral("--audio")});
    }

    void mirrorFullscreenMirroredStretch() {
        ResolvedConfig c = bareCfg();
        c.windowWidth = 1880;
        c.windowHeight = 996;
        // fullscreen = genuine mirrored fullscreen (the client always renders stretched);
        // no new-display
        auto [e, a] = buildMirrorArgv(c, "localhost:5555", /*fullscreenDisplay=*/true);
        QVERIFY(a.contains("--fullscreen"));
        for (const QString &t : a) QVERIFY(!t.startsWith("--new-display"));
        for (const QString &t : a) QVERIFY(!t.startsWith("--window-width"));  // dropped in fullscreen
    }

    void mirrorFullscreenSeparateDisplay() {
        ResolvedConfig c = bareCfg();
        c.fullscreenSeparateDisplay = true;  // desktop mode
        c.fullscreenDisplay = "3840x2160/240";
        c.fullscreenApp = "tv.pluto.android";
        c.windowWidth = 1880;
        c.windowHeight = 996;
        auto [e, a] = buildMirrorArgv(c, "localhost:5555", /*fullscreenDisplay=*/true);
        QVERIFY(a.contains("--new-display=3840x2160/240"));
        QVERIFY(a.contains("--start-app=tv.pluto.android"));
        // Measured on the hwc2 image: a new virtual display composes NOTHING without
        // flex_display — the server looks healthy while the encoder starves. Every new-display
        // session must carry it (bd remora-28ix.2).
        QVERIFY(a.contains("--server-param=flex_display=true"));
        // opens WINDOWED (no --fullscreen), at the mirror's frame so it matches the windowed mirror
        for (const QString &t : a) QVERIFY(t != QLatin1String("--fullscreen"));
        QVERIFY(a.contains("--window-width=1880"));
        QVERIFY(a.contains("--window-height=996"));
        // empty resolution → --new-display=auto (server-chosen size)
        c.fullscreenDisplay = QString();
        auto [e2, a2] = buildMirrorArgv(c, "localhost:5555", true);
        QVERIFY(a2.contains("--new-display=auto"));
        // windowed path unaffected by the desktop-mode flag: no new-display, no flex param
        auto [e3, a3] = buildMirrorArgv(c, "localhost:5555", /*fullscreenDisplay=*/false);
        for (const QString &t : a3) {
            QVERIFY(!t.startsWith("--new-display"));
            QVERIFY(!t.startsWith("--server-param"));
        }
    }

    void scrcpyH264PinsSoftwareEncoderOnHwc2() {
        // the hwc2 image's vendor AVC c2 entry can't start — h264 must pin the sw encoder there
        ResolvedConfig c = macvlanCfg();
        c.imageTag = "remora23:x86_64-gapps-wv-hwc2";
        const auto [e, a] = buildMirrorArgv(c, "192.168.0.77:5555");
        QVERIFY(a.contains("--video-encoder=c2.android.avc.encoder"));
        // h265 on hwc2 pins the VAAPI HW HEVC encoder (scrcpy's auto-select intermittently probes it,
        // fails the capability config, and falls back to software AVC — pinning forces the HW VDENC)
        c.videoCodec = "h265";
        const auto [e2, a2] = buildMirrorArgv(c, "192.168.0.77:5555");
        QVERIFY(a2.contains("--video-codec=h265"));
        QVERIFY(a2.contains("--video-encoder=c2.remora.vaapi.hevc.encoder"));
        // non-hwc2 images keep the default vector untouched
        const auto [e3, a3] = buildMirrorArgv(bareCfg(), "localhost:5555");
        for (const QString &t : a3) QVERIFY(!t.startsWith("--video-encoder"));
    }

    void scrcpyVideoCodecOptIn() {
        // default (h264 / empty) emits no --video-codec token…
        const auto [e0, a0] = buildMirrorArgv(bareCfg(), "localhost:5555");
        for (const QString &t : a0) QVERIFY(!t.startsWith("--video-codec"));
        // …h265 does (needs an on-device HEVC encoder → use_codec2)
        ResolvedConfig c = bareCfg();
        c.videoCodec = "h265";
        const auto [e1, a1] = buildMirrorArgv(c, "localhost:5555");
        QVERIFY(a1.contains("--video-codec=h265"));
    }

    void dockerUseRemoraC2OptIn() {
        QVERIFY(!buildDockerRunArgv(bareCfg()).contains("androidboot.use_remora_c2=1"));
        ResolvedConfig c = bareCfg();
        c.useCodec2 = true;
        QVERIFY(buildDockerRunArgv(c).contains("androidboot.use_remora_c2=1"));
    }

    void scrcpyWindowGeometryOptIn() {
        // default: no window-geometry tokens
        const auto [e0, a0] = buildMirrorArgv(bareCfg(), "localhost:5555");
        for (const QString &t : a0) QVERIFY(!t.startsWith("--window-"));
        ResolvedConfig c = bareCfg();
        c.windowWidth = 1880;
        c.windowHeight = 996;
        const auto [e1, a1] = buildMirrorArgv(c, "localhost:5555");
        QVERIFY(a1.contains("--window-width=1880"));
        QVERIFY(a1.contains("--window-height=996"));
        // The frame belongs to the desktop: Remora never asks for a borderless window, at any
        // geometry. (The client has no aspect lock to release — an explicit size just holds.)
        QVERIFY(!a1.contains("--window-borderless"));
        c.windowFullscreen = true;
        const auto [e2, a2] = buildMirrorArgv(c, "localhost:5555");
        QVERIFY(a2.contains("--fullscreen"));
    }

    // Companion apps (bd remora-fgj.1/.2): a Remora-built APK directory bound read-only at
    // /system/system_ext/app/<Name> so PackageManager scans it off a system partition and the app
    // gets FLAG_SYSTEM — the precondition SettingsLib's TileUtils enforces before showing a
    // Settings tile. system_ext is nested under system/ in this device tree, hence the path.
    void companionAppBindVectors() {
        QCOMPARE(companionAppBind(QStringLiteral("/home/u/rezmods/apps/RemoraTweaks")),
                 QStringLiteral("/home/u/rezmods/apps/RemoraTweaks:"
                                "/system/system_ext/app/RemoraTweaks:ro"));
        QCOMPARE(companionAppBind(QStringLiteral("/apps/RemoraTweaks/")),  // trailing slash
                 QStringLiteral("/apps/RemoraTweaks/:/system/system_ext/app/RemoraTweaks:ro"));
        QCOMPARE(companionAppBind(QStringLiteral("/apps/build-out:RemoraTweaks")),  // explicit name
                 QStringLiteral("/apps/build-out:/system/system_ext/app/RemoraTweaks:ro"));

        // "local:" means the dir is HERE and Remora stages it, so the BIND must name the staged
        // host path — never the local one, which does not exist on the guest.
        const QString loc = QStringLiteral("local:/home/u/aosp/out/RemoraTweaks");
        QVERIFY(companionAppIsLocal(loc));
        QCOMPARE(companionAppName(loc), QStringLiteral("RemoraTweaks"));
        QCOMPARE(companionAppSourceDir(loc), QStringLiteral("/home/u/aosp/out/RemoraTweaks"));
        QCOMPARE(companionAppBind(loc, QStringLiteral("/home/u/rezmods")),
                 QStringLiteral("/home/u/rezmods/apps/RemoraTweaks:"
                                "/system/system_ext/app/RemoraTweaks:ro"));
        // …and with no rezmods dir (bare: the docker host IS this machine) it binds in place.
        QCOMPARE(companionAppBind(loc, QString()),
                 QStringLiteral("/home/u/aosp/out/RemoraTweaks:"
                                "/system/system_ext/app/RemoraTweaks:ro"));
        // an explicit name survives the prefix, and the source is still the local dir
        const QString named = QStringLiteral("local:/home/u/out/app-dir:Tweaks");
        QCOMPARE(companionAppSourceDir(named), QStringLiteral("/home/u/out/app-dir"));
        QCOMPARE(companionAppBind(named, QStringLiteral("/rz")),
                 QStringLiteral("/rz/apps/Tweaks:/system/system_ext/app/Tweaks:ro"));
        // unprefixed entries never report a local source — nothing to stage
        QVERIFY(!companionAppIsLocal(QStringLiteral("/apps/RemoraTweaks")));
        QVERIFY(companionAppSourceDir(QStringLiteral("/apps/RemoraTweaks")).isEmpty());

        ResolvedConfig c = macvlanCfg();
        c.companionApps = {QStringLiteral("/apps/RemoraTweaks"), QStringLiteral("/apps/o:Other")};
        const QStringList a = buildDockerRunArgv(c);
        const int i = a.indexOf(
            QStringLiteral("/apps/RemoraTweaks:/system/system_ext/app/RemoraTweaks:ro"));
        QVERIFY(i > 0 && a[i - 1] == QStringLiteral("-v"));
        QVERIFY(a.contains(QStringLiteral("/apps/o:/system/system_ext/app/Other:ro")));
        QVERIFY(i < a.indexOf(c.imageTag));  // binds precede the image, or docker takes them as args
    }

    // bd remora-pzq. "local:" staging was VmDeployer's step and went with the vm backend, so on a
    // remote docker host the form has no execution path: the bind would name a directory on the
    // wrong machine and docker would create it empty — the app silently not installing. The
    // refusal must name exactly the local entries and both ways out, and stay silent for entries
    // that already name a host path.
    void localCompanionAppsAreRefusedForRemote() {
        // Nothing local: silence — host-path entries are the remote-native form.
        QVERIFY(companionAppsRemoteRefusal({}).isEmpty());
        QVERIFY(companionAppsRemoteRefusal({QStringLiteral("/apps/RemoraTweaks")}).isEmpty());
        // One local entry: named, with both remedies.
        const QString one = companionAppsRemoteRefusal(
            {QStringLiteral("local:/home/u/out/RemoraTweaks")});
        QVERIFY(one.contains(QStringLiteral("RemoraTweaks")));
        QVERIFY(one.contains(QStringLiteral("local:")));
        QVERIFY(one.contains(QStringLiteral("drop the \"local:\" prefix")));
        // Mixed: only the local ones are named — the host-path entry is not the problem.
        const QString mixed = companionAppsRemoteRefusal(
            {QStringLiteral("/apps/HostSide"), QStringLiteral("local:/home/u/out/x:Tweaks")});
        QVERIFY(mixed.contains(QStringLiteral("Tweaks")));
        QVERIFY(!mixed.contains(QStringLiteral("HostSide")));
    }

    // Unlike shared folders, these are NOT suppressed by overlayfs: that guard exists because the
    // init-time overlay covers /data and shadows sub-binds beneath it, and /system is not /data.
    void companionAppsSurviveOverlayfs() {
        ResolvedConfig c = macvlanCfg();
        c.useOverlayfs = true;
        c.dataBaseDir = QStringLiteral("/home/u/base");
        c.sharedFolders = {QStringLiteral("/srv/media")};
        c.companionApps = {QStringLiteral("/apps/RemoraTweaks")};
        const QString argv = buildDockerRunArgv(c).join(QLatin1Char(' '));
        QVERIFY(!argv.contains(QLatin1String("/data/media/0/")));       // share suppressed
        QVERIFY(argv.contains(QLatin1String("/system/system_ext/app/RemoraTweaks:ro")));
    }

    // The empty-config contract: an unconfigured profile must produce the argv it produced before
    // this option existed, and adding apps must ADD ONLY the bind pairs — never reorder or drop
    // anything. Comparing an empty list against itself would prove nothing, so the invariant is
    // stated as: argv(with apps) minus the new "-v <bind>" pairs == argv(without), exactly.
    void companionAppsArePurelyAdditive() {
        for (Backend b : {Backend::Bare, Backend::Remote}) {
            ResolvedConfig c = (b == Backend::Remote) ? macvlanCfg() : bareCfg();
            const QStringList without = buildDockerRunArgv(c);
            QVERIFY(!without.join(QLatin1Char(' ')).contains(QLatin1String("system_ext")));

            c.companionApps = {QStringLiteral("/apps/RemoraTweaks")};
            QStringList with = buildDockerRunArgv(c);
            const QString bind =
                QStringLiteral("/apps/RemoraTweaks:/system/system_ext/app/RemoraTweaks:ro");
            const int at = with.indexOf(bind);
            QVERIFY(at > 0 && with[at - 1] == QStringLiteral("-v"));
            with.removeAt(at);
            with.removeAt(at - 1);       // its "-v"
            QCOMPARE(with, without);     // nothing else moved
        }
        // and an empty resolve carries no companion apps at all
        RemoraConfig fresh;
        QVERIFY(resolve(fresh, Backend::Bare).companionApps.isEmpty());
    }

    // The grant has to resolve a PACKAGE from a DIRECTORY: an entry names the bind target, not an
    // app id. `pm list packages -f` reports codePath=package, and the codePath is exactly what we
    // mounted, so the lookup needs no extra config. Must tolerate failure — a platform-signed app
    // already holds the permission by signature and pm grant refuses it outright.
    void companionAppGrantIsResolvedOnDevice() {
        ResolvedConfig c = bareCfg();
        const QString plain = buildDeviceTuneArgv(c, "localhost:5555").last();
        QVERIFY(!plain.contains(QLatin1String("WRITE_SECURE_SETTINGS")));  // default vector clean

        c.companionApps = {QStringLiteral("/apps/RemoraTweaks"), QStringLiteral("/apps/o:Other")};
        const QString tuned = buildDeviceTuneArgv(c, "localhost:5555").last();
        // looks the package up by the path it was mounted at, for both naming forms
        QVERIFY(tuned.contains(QLatin1String("/system/system_ext/app/RemoraTweaks/")));
        QVERIFY(tuned.contains(QLatin1String("/system/system_ext/app/Other/")));
        QVERIFY(!tuned.contains(QLatin1String("/apps/o")));  // host path never reaches the device
        QCOMPARE(tuned.count(QLatin1String("pm grant")), 2);
        QVERIFY(tuned.contains(QLatin1String("android.permission.WRITE_SECURE_SETTINGS")));
        // tolerant: a signature-granted app makes pm grant fail, which must not fail the step
        QVERIFY(tuned.contains(QLatin1String("2>/dev/null; true")));
        // and it is appended, so the pre-existing tune settings are still there
        QVERIFY(tuned.startsWith(plain));
    }

    void sharedFolderBindVectors() {
        QCOMPARE(sharedFolderBind(QStringLiteral("/srv/media")),
                 QStringLiteral("/srv/media:/data/media/0/media"));
        QCOMPARE(sharedFolderBind(QStringLiteral("/srv/media/")),  // trailing slash: same name
                 QStringLiteral("/srv/media/:/data/media/0/media"));
        QCOMPARE(sharedFolderBind(QStringLiteral("/srv/in:Downloads")),
                 QStringLiteral("/srv/in:/data/media/0/Downloads"));
        ResolvedConfig c = macvlanCfg();
        c.sharedFolders = {QStringLiteral("/srv/media"), QStringLiteral("/srv/in:Downloads")};
        const QStringList a = buildDockerRunArgv(c);
        const int i = a.indexOf(QStringLiteral("/srv/media:/data/media/0/media"));
        QVERIFY(i > 0 && a[i - 1] == QStringLiteral("-v"));
        QVERIFY(a.contains(QStringLiteral("/srv/in:/data/media/0/Downloads")));
        QVERIFY(i < a.indexOf(c.imageTag));  // bind options precede the image
        // no shares → the proven default vectors are untouched
        QVERIFY(!buildDockerRunArgv(macvlanCfg())
                     .join(QLatin1Char(' '))
                     .contains(QLatin1String("/data/media/0/")));
    }

    // The grant that makes "Open device storage" work. Golden because it runs as ROOT on the
    // user's machine against a user-chosen path.
    void storageAclVector() {
        // Exact vector: one setfacl, no shell. The elevator's prompt names what it is about to
        // run as root, and there is no script for a path to break out of.
        QCOMPARE(buildStorageAclArgv(QStringLiteral("/home/user/.remora-android-17/upper/media"),
                                     1000),
                 QStringList({"pkexec", "setfacl", "-R", "-m", "u:1000:rwX,d:u:1000:rwx", "--",
                              "/home/user/.remora-android-17/upper/media"}));
        // Both halves in one -m: the access entry AND the default one new children inherit.
        // Drop either and the grant either misses what exists or goes stale on the next file
        // the device writes — the whole reason this is an ACL and not a chmod.
        const QString spec = buildStorageAclArgv(QStringLiteral("/m"), 1000)[4];
        QVERIFY(spec.contains(QStringLiteral("u:1000:rwX")));   // what is there now
        QVERIFY(spec.contains(QStringLiteral("d:u:1000:rwx"))); // what the device creates later
        // A path that would break out of a shell must ride through as one argv element, and the
        // '--' keeps a leading-dash dir from being read as a flag.
        const QStringList evil = buildStorageAclArgv(QStringLiteral("-rf /; echo \"x"), 1000);
        QCOMPARE(evil.last(), QStringLiteral("-rf /; echo \"x"));
        QCOMPARE(evil.at(evil.size() - 2), QStringLiteral("--"));
        // the elevator is swappable for a terminal run, and nothing else moves
        const QStringList sudo =
            buildStorageAclArgv(QStringLiteral("/m"), 1000, QStringLiteral("sudo"));
        QCOMPARE(sudo.first(), QStringLiteral("sudo"));
        QCOMPARE(sudo.mid(1), buildStorageAclArgv(QStringLiteral("/m"), 1000).mid(1));
    }

    void deviceTuneVector() {
        // independent audio is unconditional; the animation duration scales are NOT written at
        // all any more, so Developer options is the one place they are set (they used to be
        // re-pinned on every connect, which silently undid the user's choice).
        const QStringList expected{
            "adb", "-s", "localhost:5555", "shell",
            "settings put system multi_audio_focus_enabled 1; "
            "settings put global verifier_verify_adb_installs 0; "
            "settings put global package_verifier_enable 0"
            "; dumpsys webviewupdate 2>/dev/null | grep -q 'Current WebView package is null' && "
            "{ w=$(dumpsys webviewupdate 2>/dev/null | grep -m1 -oE 'Valid package [a-zA-Z0-9._]+' "
            "| cut -d' ' -f3); [ -n \"$w\" ] && cmd webviewupdate set-webview-implementation \"$w\""
            "; }; true"};
        QCOMPARE(buildDeviceTuneArgv(bareCfg(), "localhost:5555"), expected);
        ResolvedConfig c = bareCfg();
        QVERIFY(!buildDeviceTuneArgv(c, "localhost:5555").last().contains("animation_scale"));
        QVERIFY(!buildDeviceTuneArgv(c, "localhost:5555").last().contains("animator_duration"));
        // fps ≤ 60 (or unset) → no refresh-rate settings (the 60Hz default policy is correct);
        // fps > 60 → peak+min pinned so SF's render clamp follows the raised mode (remora-mzw).
        QVERIFY(!buildDeviceTuneArgv(c, "localhost:5555").last().contains("peak_refresh_rate"));
        c.fps = 120;
        const QString tuned = buildDeviceTuneArgv(c, "localhost:5555").last();
        QVERIFY(tuned.contains("peak_refresh_rate 120"));
        QVERIFY(tuned.contains("min_refresh_rate 120"));
        // WebView provider selection (bd remora-4ei.48): unconditional in the vector, but a no-op
        // on a device that already has one — the whole thing hangs off the "is null" probe, so a
        // healthy device is never re-selected (which would restart relros under running WebViews).
        QVERIFY(tuned.contains("Current WebView package is null"));
        QVERIFY(tuned.contains("cmd webviewupdate set-webview-implementation"));
        // the package is READ from dumpsys, never hardcoded: naming the wrong provider is a no-op
        // that leaves the device broken while looking fixed
        QVERIFY(!tuned.contains("set-webview-implementation com."));
        QVERIFY(tuned.contains("grep -m1 -oE 'Valid package"));
        // and the step never fails the connect: no provider found ⇒ the && short-circuits and the
        // block's own trailing `true` swallows it. Checked as the block's tail rather than the
        // vector's, because fps and companion-app grants append after it.
        QVERIFY(tuned.contains("; }; true"));
    }

    // Guest timezone (bd remora-c8t): emitted only when resolved (the resolver leaves it empty —
    // the host fill is engine-side, so the default tune vector above stays byte-identical),
    // guarded on the current value because a CHANGE broadcasts to every app, and validated to
    // IANA shape because the zone lands inside a quoted shell word.
    void deviceTuneTimezone() {
        ResolvedConfig c = bareCfg();
        QVERIFY(!buildDeviceTuneArgv(c, "localhost:5555").last().contains("timezone"));
        c.timezone = QStringLiteral("Europe/Berlin");
        const QString tz = buildDeviceTuneArgv(c, "localhost:5555").last();
        QVERIFY(tz.contains(QStringLiteral(
            "[ \"$(getprop persist.sys.timezone)\" = 'Europe/Berlin' ] || "
            "setprop persist.sys.timezone 'Europe/Berlin'")));
        // A zone that fails the pattern is DROPPED, not quoted harder — it reaches a shell.
        c.timezone = QStringLiteral("Bad'; reboot; '");
        QVERIFY(!buildDeviceTuneArgv(c, "localhost:5555").last().contains("reboot"));
        QVERIFY(!buildDeviceTuneArgv(c, "localhost:5555").last().contains("timezone"));
    }

    void sourcePatchApplyCommands() {
        // Fixture repointed (bd remora-31sq): omx_codecs was deleted with the other
        // series that patched projects no longer in any tree. sensors_hal is a live equivalent —
        // a distinct project, so the per-project command grouping is still what is under test.
        const QStringList cmds = buildApplyPatchCommands(
            {"sensors_hal", "minigbm_rendernode"}, "/src/tree", "/opt/remora/vendor/source-patches", 16);
        QVERIFY(cmds.filter("hardware/interfaces").size() >= 3);
        // NO `git init` anywhere (bd remora-31sq): no current series creates a project, and
        // initing an absent one manufactured an empty repo whose every per-file step then failed
        // "does not exist in index" — plus zero-commit litter no manifest owns. Absent is now a
        // one-line SKIP (or a FATAL when a feature requires the series); the gate is exercised
        // for real in test_engine's absentProjectIsSkippedNotInited.
        for (const QString &c : cmds) QVERIFY(!c.contains("git init"));
        QVERIFY(!cmds.filter("is not in this tree").isEmpty());
        QVERIFY(!cmds.filter("0001-cros_gralloc").isEmpty());
        // a disabled patch contributes nothing
        for (const QString &c : cmds) QVERIFY(!c.contains("v4l2_codec2"));
        // defaults are the golden stack (hw_encoder is deliberately NOT among them any more —
        // c2-va is staged from vendor/source-build, see c2vaIsNotInTheApplyPath)
        QVERIFY(defaultSourcePatchSet(16).contains("minigbm_rendernode"));
        // the sensors pair is opt-in (NOT default) and spans two projects when enabled
        QVERIFY(!defaultSourcePatchSet(16).contains("sensors_hal"));
        QVERIFY(!defaultSourcePatchSet(16).contains("sensors_product"));
        const QStringList sens = buildApplyPatchCommands(
            {"sensors_hal", "sensors_product"}, "/src/tree", "/root", 16);
        QVERIFY(!sens.filter("hardware/interfaces").isEmpty());
        QVERIFY(!sens.filter("0001-sensors-example-HAL-remora-host-injectable-accel-gyr.patch")
                     .isEmpty());
        QVERIFY(!sens.filter("$d/sensors.patch").isEmpty());
        // ...and the dir it resolves from is emitted, shared series + -a<N> override
        QVERIFY(!sens.filter("d=/root/device-remora-sensors-a16").isEmpty());
    }

    // Two patches sharing one project must reset that project only ONCE — else the later patch's
    // reset wipes the earlier one.
    void sourcePatchSharedProjectResetsOnce() {
        // Fixture repointed (bd remora-31sq): device_sepolicy is gone. minigbm_amdgpu_no_dri
        // and minigbm_gralloc0_metadata are two live series sharing external/minigbm, which is the
        // only property this test needs — one unconditional pre-apply reset for a shared project.
        const QStringList apply = buildApplyPatchCommands(
            {"minigbm_amdgpu_no_dri", "minigbm_gralloc0_metadata"}, "/src/tree", "/root", 16);
        // Count only the UNCONDITIONAL pre-apply reset. Each patch command also carries a
        // reset --hard on its failure path (rewinding a conflicted 3-way so it is a clean skip),
        // which is a different thing and must not be counted here — that one runs only when a
        // patch fails, and it rewinds to the last committed patch rather than wiping the series.
        int resets = 0;
        for (const QString &c : apply)
            if (c.contains("external/minigbm") && c.contains("reset --hard")
                && !c.contains("tree left clean")) ++resets;
        QCOMPARE(resets, 1);
        // both projects' patches still applied
        QVERIFY(!apply.filter("minigbm").isEmpty());
        // and the pre-sync reset dedups too
        const QStringList reset =
            buildResetCommands({"minigbm_amdgpu_no_dri", "minigbm_gralloc0_metadata"}, "/src/tree");
        QCOMPARE(reset.filter("external/minigbm").size(), 1);
    }

    // A series that stops applying across an Android bump gets a per-version override directory,
    // while the shared series keeps serving every other version untouched. The choice is emitted
    // as shell rather than decided here, because patchRoot lives on the build host — which may be
    // a remote box — so probing the directory in C++ would test the wrong filesystem.
    void sourcePatchVersionAwareDir() {
        for (int ver : {16, 17}) {
            const QStringList cmds =
                buildApplyPatchCommands({"minigbm_rendernode"}, "/src/tree", "/root", ver);
            const QString pick = cmds.filter(QStringLiteral("d=/root/external-minigbm")).value(0);
            QVERIFY(!pick.isEmpty());
            // prefers the version override...
            QVERIFY(pick.contains(QStringLiteral("d=/root/external-minigbm-a%1").arg(ver)));
            // ...and falls back to the shared series when that directory is absent
            QVERIFY(pick.contains("[ -d \"$d\" ] || d=/root/external-minigbm"));
            // patches are read through the resolved variable, never a baked-in path
            QVERIFY(!cmds.filter("$d/0001-cros_gralloc").isEmpty());
            for (const QString &c : cmds) QVERIFY(!c.contains("/root/external-minigbm/0001-"));
            // The resolution must be ONE compound command. Callers join with " && ", so a bare
            // `d=A; [ -d ] || d=B` splits the chain: after any earlier patch fails the assignment
            // is skipped, yet the step following the `;` still runs — applying this group's
            // patches out of the previous group's directory. Shipped exactly that bug once.
            QVERIFY(pick.startsWith("{ ") && pick.endsWith("; }"));
        }
    }

    // `git apply --3way` stages what it applies, and `git am` refuses to run against a dirty
    // index ("Dirty index: cannot apply patches"), so a plain-diff step MUST commit or a mailbox
    // patch behind it in the same project dies. A real build hit this.
    // NO PROJECT MIXES THE TWO STYLES TODAY (checked against the registry, bd
    // remora-31sq; the one that did left the manifests). The obligation is
    // therefore asserted on the plain-diff step ALONE rather than through a mixed pair: it has to
    // commit whether or not a mailbox patch currently follows it, because the day one does is
    // exactly the day nobody re-derives this.
    void sourcePatchPlainDiffCommitsBeforeMbox() {
        const QStringList cmds = buildApplyPatchCommands(
            {"boot_animation"}, "/src/tree", "/root", 17);
        const QString diffStep = cmds.filter("apply --reverse --check").value(0);
        QVERIFY(!diffStep.isEmpty());
        QVERIFY(diffStep.contains("commit -qm"));
        // and it must not commit an empty index (git commit would fail the chain)
        QVERIFY(diffStep.contains("diff --cached --quiet ||"));
        // The mbox branch's own 3-way fallback stages too, so it has the same obligation —
        // missing it let one fallback take every later mailbox patch in the project down.
        const QStringList mbox = buildApplyPatchCommands(
            {"surfaceview_secure", "shell_update_owner", "statusbar_hide_list"}, "/src/tree",
            "/root", 17);
        const QString amStep = mbox.filter("am --keep-cr").value(0);
        QVERIFY(!amStep.isEmpty());
        QVERIFY(amStep.contains("apply --3way"));
        QVERIFY(amStep.contains("commit -qm"));
        QVERIFY(amStep.contains("diff --cached --quiet ||"));
        // ...and a conflicted 3-way must rewind rather than leave <<<<<<< in the tree
        QVERIFY(amStep.contains("reset --hard"));
        QVERIFY(amStep.contains("tree left clean"));
        // one project, so the unconditional pre-apply reset happens exactly once
        int resets = 0;
        for (const QString &c : cmds)
            if (c.contains("device/remora") && c.contains("reset --hard")
                && !c.contains("tree left clean")) ++resets;
        QCOMPARE(resets, 1);
    }

    // A patch that fails to apply used to end in an echo that EXITS 0 — callers join with " && ",
    // so the build completed green hours later with the feature's code absent from the image
    // (bd remora-82c.1; the sensors incident of bd remora-4ei.53 arrived by the other door). A
    // patch some requested feature depends on must instead fail the pass loudly, named; a patch
    // no feature asked for keeps the warn-and-continue that lets a series being ported limp.
    void sourcePatchRequiredByFeatureFailsLoudlyOnConflict() {
        // The registry ties sensors → {sensors_hal, sensors_product}; the map is what the apply
        // commands consume.
        const QMap<QString, QString> owners =
            featureRequiredPatchOwners({QStringLiteral("sensors")});
        QCOMPARE(owners.value(QStringLiteral("sensors_hal")), QStringLiteral("sensors"));
        QCOMPARE(owners.value(QStringLiteral("sensors_product")), QStringLiteral("sensors"));
        // An empty feature set resolves to the default build set, which does not include sensors.
        QVERIFY(!featureRequiredPatchOwners({}).contains(QStringLiteral("sensors_hal")));

        const QStringList cmds = buildApplyPatchCommands(
            {"sensors_hal", "minigbm_rendernode"}, "/src/tree", "/root", 16, {}, owners);
        // The required patch's failure arm hard-fails, naming the feature — and still rewinds
        // first, so the tree hygiene contract is unchanged.
        const QString required = cmds.filter("FATAL: patch").value(0);
        QVERIFY(!required.isEmpty());
        QVERIFY(required.contains(QStringLiteral("sensors_hal")));
        QVERIFY(required.contains(QStringLiteral("feature(s) sensors")));
        QVERIFY(required.contains(QStringLiteral("false; }")));
        QVERIFY(required.contains(QStringLiteral("reset --hard")));
        QVERIFY(!required.contains(QStringLiteral("SKIPPED")));
        // The unrequired patch keeps the soft skip, byte-identical to before. Select its APPLY
        // command — the same filename also appears in the missing-file guard, which has no arm.
        QString soft;
        for (const QString &c : cmds)
            if (c.contains(QStringLiteral("0001-cros_gralloc")) &&
                c.contains(QStringLiteral("--3way")))
                soft = c;
        QVERIFY(!soft.isEmpty());
        QVERIFY(soft.contains(QStringLiteral("WARN patch SKIPPED (conflicts)")));
        QVERIFY(!soft.contains(QStringLiteral("FATAL")));
        // And with no owners map at all, everything keeps the old contract.
        const QStringList plain = buildApplyPatchCommands(
            {"sensors_hal"}, "/src/tree", "/root", 16);
        QVERIFY(plain.filter("FATAL: patch").isEmpty());
    }

    // The globbed backport series skips a patch already in history by matching `git log
    // --format=%f` against the filename slug. %f re-slugifies the subject, so the fallback must
    // commit under the BARE slug — a "backport: <slug>" subject yields "backport-<slug>", never
    // matches, and the whole series re-applies on every run (measured: one patch landed 5×).
    void sourcePatchBackportCommitMatchesSkipCheck() {
        const QStringList cmds = buildApplyPatchCommands(
            {"lineage_platform_backport"}, "/src/tree", "/root", 17);
        const QString loop = cmds.filter("am --keep-cr").value(0);
        QVERIFY(!loop.isEmpty());
        QVERIFY(loop.contains("grep -q \"^$s\""));    // PREFIX: $s is the truncated filename slug
        QVERIFY2(!loop.contains("grep -qx"), "exact match never fires — %f is the full slug");
        QVERIFY(loop.contains("commit -qm \"$s\""));  // must round-trip through %f
        QVERIFY2(!loop.contains("backport: $s"), "prefixed subject breaks the skip check");
    }

    // A series can be subsumed upstream for one Android version — the A17 device tree already
    // carries every hunk of the v4l2_codec2 series — where re-applying is a no-op that only emits
    // alarming "patch does not apply" noise. A SKIP marker in the override dir turns the whole
    // series off for that version while the shared series keeps serving the others.
    void sourcePatchSkipMarkerGuardsEveryApply() {
        const QStringList cmds =
            buildApplyPatchCommands({"v4l2_encoder"}, "/src/tree", "/root", 17);
        // the marker is announced once, from the resolution step
        QVERIFY(!cmds.filter("[ -f \"$d/SKIP\" ] && echo").isEmpty());
        // and every apply command is guarded by it — a half-guarded series would apply in part
        int applies = 0;
        for (const QString &c : cmds) {
            if (!c.contains("am --keep-cr") && !c.contains("apply --reverse --check")) continue;
            ++applies;
            QVERIFY2(c.contains("[ -f \"$d/SKIP\" ] ||"), qPrintable("unguarded apply: " + c));
        }
        QCOMPARE(applies, 5);  // the series' five patches (the AV1 enum is its own entry now)
    }

    // Every patch file the registry NAMES must exist. A missing one does not fail loudly: the
    // apply loop reports "can't open patch ... No such file or directory" and then SKIPS THE WHOLE
    // ENTRY, hard-resetting the tree — so one stale filename silently discards every OTHER patch in
    // that entry. That shipped: a patch was moved to its own directory while the old entry kept
    // naming it, which took device_sepolicy down with it and lost the hal_codec2 sepolicy labeling,
    // with no error until nine minutes into a build.
    // Checks the SOURCE tree, not REMORA_VENDOR_DIR. The registry and the patch files are versioned
    // together in git, so that is the pair this test exists to keep consistent. Checking the
    // installed tree was tried first and is wrong: it fails on every newly added patch until the
    // next `remora-install`, which inverts the development order. Install drift is a separate
    // concern — see bd remora-4ei.33, install(DIRECTORY) never prunes.
    void everyRegisteredPatchFileExists() {
        const QString vendor =
            QStringLiteral(REMORA_SOURCE_VENDOR_DIR) + QStringLiteral("/source-patches");
        QVERIFY2(QDir(vendor).exists(), qPrintable(QStringLiteral("no source-patches at ") + vendor));
        for (const SourcePatch &p : sourcePatches())
            for (const QString &f : p.patches) {
                const QString path = vendor + QLatin1Char('/') + p.dir + QLatin1Char('/') + f;
                QVERIFY2(QFile::exists(path),
                         qPrintable(QStringLiteral("patch '%1' names a missing file: %2")
                                        .arg(p.key, path)));
            }
    }

    // The test above points one way, and until now EVERY guard did: a declared file that is
    // missing hard-fails, but an undeclared file sitting in a registered dir was invisible — the
    // apply loop never reaches it, so it is a silent no-op forever. Third instance of the shape
    // (bd remora-82c.9): the c2-va 15th patch (bd remora-4ei.42), the unpinned patch nothing
    // announced (bd remora-4ei.38), and the System-update overlay that sat on disk from the day
    // it was committed without ever applying. Globbed series (empty patch list = every
    // NNNN-*.patch in the dir) consume everything by construction, so their dirs are exempt;
    // version-variant dirs (<dir>-a<N>) resolve through the same entries, so the same names are
    // the law there too. Dirs no entry references at all are out of scope — they are not part of
    // any apply path.
    void everyPatchFileOnDiskIsRegistered() {
        const QString vendor =
            QStringLiteral(REMORA_SOURCE_VENDOR_DIR) + QStringLiteral("/source-patches");
        QHash<QString, QSet<QString>> declared;  // dir → union of names across entries
        QSet<QString> globbed;
        for (const SourcePatch &p : sourcePatches()) {
            if (p.patches.isEmpty()) {
                globbed.insert(p.dir);
                continue;
            }
            for (const QString &f : p.patches) declared[p.dir].insert(f);
        }
        static const QRegularExpression kVariant(QStringLiteral("-a[0-9]+$"));
        for (auto it = declared.constBegin(); it != declared.constEnd(); ++it) {
            if (globbed.contains(it.key())) continue;  // served both ways: the glob eats the rest
            // The shared dir plus every version-variant dir that exists beside it.
            QStringList dirs{it.key()};
            for (const QFileInfo &d :
                 QDir(vendor).entryInfoList({it.key() + QStringLiteral("-a*")}, QDir::Dirs))
                if (kVariant.match(d.fileName()).hasMatch()) dirs << d.fileName();
            for (const QString &d : dirs)
                for (const QFileInfo &fi : QDir(vendor + QLatin1Char('/') + d)
                                               .entryInfoList({QStringLiteral("*.patch")},
                                                              QDir::Files))
                    QVERIFY2(it.value().contains(fi.fileName()),
                             qPrintable(QStringLiteral(
                                            "%1/%2 is on disk but no registry entry names it — "
                                            "the apply loop will NEVER reach it (bd remora-82c.9)")
                                            .arg(d, fi.fileName())));
        }
    }

    // Every emitted command must be a BALANCED shell compound. The guards here nest several
    // brace groups deep, and a substring assertion happily passes while the shell is unparseable
    // — which shipped once: adding the SKIP guard left one '{' unclosed in both apply branches,
    // and every build died instantly with "syntax error: unexpected end of file from `{'".
    void sourcePatchCommandsAreBalanced() {
        QStringList keys;
        for (const SourcePatch &p : sourcePatches()) keys << p.key;
        for (int ver : {16, 17}) {
            const QStringList cmds = buildApplyPatchCommands(keys, "/src/tree", "/root", ver);
            QVERIFY(!cmds.isEmpty());
            for (const QString &c : cmds) {
                const int opens = c.count(QLatin1Char('{'));
                const int closes = c.count(QLatin1Char('}'));
                QVERIFY2(opens == closes,
                         qPrintable(QStringLiteral("unbalanced braces (%1 open, %2 close): %3")
                                        .arg(opens).arg(closes).arg(c.left(120))));
                QCOMPARE(c.count(QLatin1Char('(')), c.count(QLatin1Char(')')));
            }
        }
    }

    // Every emitted command must survive being joined with " && ": no bare top-level ';' that
    // would let a later step run after an earlier failure. Checked with and without the incremental
    // guards, since the guard wraps the body in one more brace group.
    // A patch file that is enabled but ABSENT must stop the build, not warn. bd remora-fgj.11:
    // /usr/bin/remora had been reinstalled and /usr/share/remora/vendor/source-patches had not, so
    // the registry knew statusbar_tuner_entry and the file was missing. `git am` could not open it,
    // the 3-way fallback failed too, and the chain printed "WARN patch SKIPPED (conflicts)" — a
    // warning, and the wrong diagnosis — then produced a green image without the patch. Every other
    // signal (ticked checkbox, key in remorarc, changed SHAs from the re-applied series) said it
    // had worked. Executed for real, because this is shell: the guard is only worth anything if it
    // actually exits non-zero, and quoting bugs in these strings have shipped before.
    void sourcePatchMissingFileHardFails() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dir = root.path() + QStringLiteral("/frameworks-base");
        QVERIFY(QDir().mkpath(dir));
        const QStringList cmds =
            buildApplyPatchCommands({"surfaceview_secure"}, "/src/tree", root.path(), 17);
        // the resolved directory must be announced — with <dir> and <dir>-a<N> both possible,
        // nothing previously said which one a series came out of
        QVERIFY2(!cmds.filter(QStringLiteral("patches: surfaceview_secure <- ")).isEmpty(),
                 "the resolved patch directory is not logged");
        const QString guard = cmds.filter(QStringLiteral("MISSING PATCH:")).value(0);
        QVERIFY2(!guard.isEmpty(), "no missing-file guard was emitted for an explicit patch list");
        const auto sh = [](const QString &cmd) {
            QProcess p;
            p.start(QStringLiteral("sh"), {QStringLiteral("-c"), cmd});
            if (!p.waitForFinished(30000)) return -1;
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        };
        // $d is set by an earlier command in the chain; supply it the same way the build does.
        const QString withD = QStringLiteral("d=%1; %2").arg(dir, guard);
        QCOMPARE(sh(withD) == 0, false);  // file absent → must fail
        QFile f(dir + QStringLiteral("/0001-surfaceview-ignore-secure.patch"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QCOMPARE(sh(withD), 0);  // file present → must pass
        // …and a series deliberately skipped for this Android version may legitimately be
        // incomplete, so SKIP has to win over the guard.
        QVERIFY(f.remove());
        QFile skip(dir + QStringLiteral("/SKIP"));
        QVERIFY(skip.open(QIODevice::WriteOnly));
        skip.close();
        QCOMPARE(sh(withD), 0);
    }

    // The union-merge attributes file must land in the PROJECT'S git dir no matter what the
    // shell's cwd is. bd remora-eoy.12: the command used `git -C <proj> rev-parse --git-path
    // info/attributes`, which prints a bare relative ".git/info/attributes" with nothing tying it
    // to <proj>, so the mkdir and redirect landed in the shell's cwd — the tree ROOT, because
    // repoBringupCmd does `cd <tree>` and the whole chain shares one shell. That manufactured a
    // husk .git at the root (which then broke build-marker recording, bd remora-eoy.11) and left
    // the real driver un-updated. Executed from a deliberately different cwd, because that is the
    // entire failure: a string-level test would have passed against the broken version.
    void sourcePatchUnionAttributesIgnoreCwd() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString proj = tmp.path() + QStringLiteral("/tree/packages/apps/Settings");
        const QString elsewhere = tmp.path() + QStringLiteral("/elsewhere");
        QVERIFY(QDir().mkpath(proj));
        QVERIFY(QDir().mkpath(elsewhere));
        const auto sh = [](const QString &cmd) {
            QProcess p;
            p.start(QStringLiteral("sh"), {QStringLiteral("-c"), cmd});
            if (!p.waitForFinished(30000)) return -1;
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        };
        QCOMPARE(sh(QStringLiteral("git -C %1 init -q").arg(proj)), 0);
        const QStringList cmds = buildApplyPatchCommands({"lineage_platform_backport"},
                                                         tmp.path() + QStringLiteral("/tree"),
                                                         QStringLiteral("/root"), 17);
        const QString attrs = cmds.filter(QStringLiteral("info/attributes")).value(0);
        QVERIFY2(!attrs.isEmpty(), "no union-attributes command was emitted");
        // run it from a cwd that is NOT the project — the broken form wrote ./.git/info/attributes
        QCOMPARE(sh(QStringLiteral("cd %1 && %2").arg(elsewhere, attrs)), 0);
        QVERIFY2(QFile::exists(proj + QStringLiteral("/.git/info/attributes")),
                 "union attributes did not land in the project's git dir");
        QVERIFY2(!QFile::exists(elsewhere + QStringLiteral("/.git")),
                 "union attributes manufactured a husk .git in the shell's cwd");
    }

    void sourcePatchCommandsAreAndSafe() {
        for (const QMap<QString, QString> &marks :
             {QMap<QString, QString>{}, patchProjectMarks(QStringLiteral("/src/tree"))}) {
            const QStringList cmds =
                buildApplyPatchCommands({"minigbm_rendernode", "surfaceview_secure", "lineage_soong"},
                                        "/src/tree", "/root", 17, marks);
            for (const QString &c : cmds) {
                const QString g = guardOf(c);
                const QString body = g.isEmpty() ? c : c.mid(c.indexOf(QLatin1String("|| ")) + 3);
                if (body.startsWith("{ ")) continue;  // brace-wrapped compounds are single units
                QVERIFY2(!body.contains("; "), qPrintable("splits the && chain: " + c));
            }
        }
    }

    // A registered default-on patch missing from an explicit enabled set must be ANNOUNCED. It is
    // the only drop in the pipeline that used to be silent — EXCLUDE drops and missing files both
    // name themselves, but the enabled-set filter is a `continue` at the top of the apply loop, so
    // an unpinned patch produced no output whatsoever and its absence was indistinguishable from
    // the patch not working. Twice in one day that cost a build (bd remora-4ei.38); the second died
    // on the exact KeyError buildprop_touch_rotation exists to fix, with the patch staged on disk
    // and simply not in remorarc's list.
    void sourcePatchUnenabledDefaultsAreAnnounced() {
        QVERIFY(defaultSourcePatchSet(17).contains("buildprop_touch_rotation"));
        const QStringList missed = unappliedDefaultPatches({"minigbm_rendernode"}, 17);
        QVERIFY(missed.contains("buildprop_touch_rotation"));
        QVERIFY(missed.contains("soong_gomemlimit"));
        QVERIFY2(!missed.contains("minigbm_rendernode"), "an enabled patch must not be reported");
        // opt-in patches are deliberately off — reporting them would drown the real signal
        QVERIFY(!missed.contains("sensors_hal"));
        QVERIFY(!missed.contains("boot_animation"));
        // the full default stack is complete by definition — a fresh profile never warns
        QVERIFY(unappliedDefaultPatches(defaultSourcePatchSet(17), 17).isEmpty());
        QVERIFY(unappliedDefaultPatches(defaultSourcePatchSet(16), 16).isEmpty());
        // empty is "unset ⇒ defaults apply" everywhere else in the codebase, not a pin of nothing
        QVERIFY(unappliedDefaultPatches({}, 17).isEmpty());

        // …and it reaches the build log, unguarded, ahead of every apply step
        const QMap<QString, QString> marks = patchProjectMarks(QStringLiteral("/src/tree"));
        const QStringList cmds =
            buildApplyPatchCommands({"minigbm_rendernode"}, "/src/tree", "/root", 17, marks);
        const QStringList warns = cmds.filter("NOT APPLIED");
        QVERIFY2(!warns.isEmpty(), "an unenabled default-on patch was dropped silently");
        QCOMPARE(warns.size(), missed.size());  // every one of them, not just the first
        QVERIFY2(!warns.filter("buildprop_touch_rotation").isEmpty(),
                 "the patch that cost the second build is not named");
        QCOMPARE(cmds.indexOf(warns.last()), warns.size() - 1);  // ahead of every apply step
        // The incremental pass skipping every project must not also skip the warning — the
        // condition is about the profile, not about any project's on-disk state.
        for (const QString &w : warns)
            QVERIFY2(guardOf(w).isEmpty(), qPrintable("mark-guarded, can be skipped: " + w));
    }

    // THE MIRROR CASE, silent for the same structural reason: buildApplyPatchCommands walks the
    // REGISTRY and filters by `enabled`, so a key the PROFILE names that the registry no longer
    // has is never visited and produces no output at all. Found live rather than imagined — the
    // remorarc [Instance-AMD build] profile still lists six entries deleted when the projects they
    // patched left the manifests (bd remora-31sq), so six of its source_patches are phantoms and
    // nothing said so.
    void staleProfilePatchKeysAreNamed() {
        const QMap<QString, QString> marks = patchProjectMarks(QStringLiteral("/src/tree"));
        const QStringList cmds = buildApplyPatchCommands(
            {"minigbm_rendernode", "device_sepolicy", "omx_codecs"}, "/src/tree", "/root", 17, marks);
        const QStringList stale = cmds.filter("STALE PROFILE ENTRY");
        QCOMPARE(stale.size(), 2);  // every one of them, not just the first
        QVERIFY2(!stale.filter("device_sepolicy").isEmpty(), "the deleted key is not named");
        QVERIFY2(!stale.filter("omx_codecs").isEmpty(), "only one stale key was reported");
        QVERIFY2(stale.filter("minigbm_rendernode").isEmpty(), "a registered key was reported");
        // Announced ahead of every apply step, immediately after the NOT APPLIED block.
        const QStringList notApplied = cmds.filter("NOT APPLIED");
        QCOMPARE(cmds.indexOf(stale.last()), notApplied.size() + stale.size() - 1);
        // Never mark-guarded: the condition is about the profile, not about any project's on-disk
        // state, so an incremental pass that skips every project must still say it.
        for (const QString &w : stale)
            QVERIFY2(guardOf(w).isEmpty(), qPrintable("mark-guarded, can be skipped: " + w));
        // A profile carrying only registry keys is silent — the warning must not fire on the
        // golden stack, or it is noise that gets trained away.
        for (int ver : {16, 17})
            QVERIFY(buildApplyPatchCommands(defaultSourcePatchSet(ver), "/src/tree", "/root", ver,
                                            marks)
                        .filter(QStringLiteral("STALE PROFILE ENTRY"))
                        .isEmpty());
    }

    // defaultOn describes ONE golden stack and that stack is A17's, so the first cut of the warning
    // above told an A16 profile to enable eight patches written for A17. Verified against the real
    // lineage-23.0 tree before scoping them: build/make ALREADY emits RecoveryDefaultTouchRotation
    // (upstream, LuK1337's TARGET_RECOVERY_DEFAULT_TOUCH_ROTATION commit) so buildprop_touch_rotation
    // is inert there by its own description; the three statusbar patches are byte-identical to their
    // frameworks-base-a17 copies and all three fail `git apply --check` against A16's SystemUI; and
    // no_lineage_sepolicy / lineage_legal_url / lineage_version_props / lineage_backport_deps exist
    // only to compensate for lineage-24.0 being pristine AOSP (bd remora-cbd.1) — lineage-23.0 has
    // that natively, and no_lineage_sepolicy would REMOVE working sepolicy. A warning that fires on
    // bd remora-82c.10 — the Source page renders by group, so a patch whose group is unknown (or
    // empty) would be built into no section and never appear: enabled in the pin, applied by every
    // build, invisible on the page. Checked in BOTH directions, because a group nothing uses
    // renders as an empty heading with a master checkbox that toggles nothing.
    void everyPatchHasARealGroup() {
        QSet<QString> known, used;
        for (const auto &g : patchGroups()) {
            QVERIFY2(!known.contains(g.first),
                     qPrintable(QStringLiteral("duplicate group '%1'").arg(g.first)));
            known.insert(g.first);
            QVERIFY(!g.second.isEmpty());  // the heading the user reads
        }
        for (const SourcePatch &p : sourcePatches()) {
            QVERIFY2(known.contains(p.group),
                     qPrintable(QStringLiteral("patch '%1' has unknown group '%2'")
                                    .arg(p.key, p.group)));
            used.insert(p.group);
        }
        for (const QString &g : known)
            QVERIFY2(used.contains(g), qPrintable(QStringLiteral("group '%1' has no patches").arg(g)));
    }

    // The page tiers on patchIsDefaultFor, so its scoping is load-bearing for what a user sees,
    // not only for what a build applies.
    void patchIsDefaultForHonoursVersionScope() {
        const SourcePatch *statusbar = nullptr;  // defaultOn, scoped {17}
        const SourcePatch *cursor = nullptr;     // defaultOn, every version
        const SourcePatch *bootanim = nullptr;   // opt-in everywhere
        for (const SourcePatch &p : sourcePatches()) {
            if (p.key == QLatin1String("statusbar_hide_list")) statusbar = &p;
            if (p.key == QLatin1String("cursor_on_motion")) cursor = &p;
            if (p.key == QLatin1String("boot_animation")) bootanim = &p;
        }
        QVERIFY(statusbar && cursor && bootanim);
        QVERIFY(patchIsDefaultFor(*statusbar, 17));
        QVERIFY(!patchIsDefaultFor(*statusbar, 16));  // registered, but never vetted on A16
        QVERIFY(patchIsDefaultFor(*cursor, 16));
        QVERIFY(patchIsDefaultFor(*cursor, 17));
        QVERIFY(!patchIsDefaultFor(*bootanim, 16));
        QVERIFY(!patchIsDefaultFor(*bootanim, 17));
        // and it agrees with the set the build actually applies, on both releases
        for (int v : {16, 17})
            for (const SourcePatch &p : sourcePatches())
                QCOMPARE(patchIsDefaultFor(p, v), defaultSourcePatchSet(v).contains(p.key));
    }

    // every A16 build is worse than the silence it replaced.
    // bd remora-82c.5 — consolidating entries must not cost existing profiles the capability.
    // source_patches is a PIN: once written it is never re-seeded from the defaults, so an
    // absorbed key left unmapped would simply never be reached by the apply loop — silently, which
    // is the failure mode that already cost two builds in bd remora-4ei.38.
    void canonicalPatchKeysRetiresAbsorbedEntries() {
        // exactly what the live A17 profile has pinned for the status bar
        const QStringList pinned{"statusbar_hide_list", "statusbar_clock_hide",
                                 "statusbar_battery_hide", "statusbar_tuner_entry"};
        const QStringList out = canonicalPatchKeys(pinned);
        QCOMPARE(out, QStringList{"statusbar_hide_list"});  // four collapse to the survivor, once

        // a profile that pinned ONLY an absorbed key still gets the capability
        QCOMPARE(canonicalPatchKeys({"statusbar_battery_hide"}),
                 QStringList{"statusbar_hide_list"});

        // order is preserved and unrelated keys are untouched
        const QStringList mixed{"minigbm_rendernode", "statusbar_clock_hide", "soong_gomemlimit"};
        QCOMPARE(canonicalPatchKeys(mixed),
                 (QStringList{"minigbm_rendernode", "statusbar_hide_list", "soong_gomemlimit"}));

        // an unrecognised key passes through rather than being dropped — it is inert either way,
        // and silently discarding a key the user wrote is worse than carrying it
        QCOMPARE(canonicalPatchKeys({"some_future_key"}), QStringList{"some_future_key"});
        QVERIFY(canonicalPatchKeys({}).isEmpty());

        // every retired key must map to an entry that ACTUALLY EXISTS, or the migration quietly
        // points at nothing
        for (const QString &k : canonicalPatchKeys(pinned)) {
            bool known = false;
            for (const SourcePatch &p : sourcePatches())
                if (p.key == k) known = true;
            QVERIFY2(known, qPrintable("retirement target is not a registered patch: " + k));
        }
    }

    // bd remora-22n — the reverse migration: SPLITTING an entry must not cost existing profiles
    // the piece that moved out. Every live profile pinned v4l2_encoder back when the AV1 enum
    // patch was its fifth file; without the expansion the enum patch would silently stop applying
    // for all of them and every A17 source build would die on the missing enum member again.
    void canonicalPatchKeysExpandsSplitEntries() {
        // the old key now stands for both pieces, in place, encoder first
        QCOMPARE(canonicalPatchKeys({"v4l2_encoder"}),
                 (QStringList{"v4l2_encoder", "v4l2_av1_enum"}));

        // a profile saved AFTER the split (both keys present) round-trips unchanged
        QCOMPARE(canonicalPatchKeys({"v4l2_encoder", "v4l2_av1_enum"}),
                 (QStringList{"v4l2_encoder", "v4l2_av1_enum"}));

        // neighbours keep their positions around the expansion
        QCOMPARE(canonicalPatchKeys({"minigbm_rendernode", "v4l2_encoder", "soong_gomemlimit"}),
                 (QStringList{"minigbm_rendernode", "v4l2_encoder", "v4l2_av1_enum",
                              "soong_gomemlimit"}));

        // every expansion target must be a registered entry, same contract as retirement
        for (const QString &k : canonicalPatchKeys({"v4l2_encoder"})) {
            bool known = false;
            for (const SourcePatch &p : sourcePatches())
                if (p.key == k) known = true;
            QVERIFY2(known, qPrintable("expansion target is not a registered patch: " + k));
        }
    }

    // The absorbed patch FILES must all still be applied — a merge that loses a file is a silent
    // capability drop wearing the same key.
    void mergedStatusbarEntryCarriesEveryPatchFile() {
        const SourcePatch *sb = nullptr;
        for (const SourcePatch &p : sourcePatches())
            if (p.key == QLatin1String("statusbar_hide_list")) sb = &p;
        QVERIFY(sb);
        // RENUMBERED (bd remora-31sq): the series moved out of the shared frameworks-base
        // directory into frameworks-base-statusbar and restarted at 0001, so a version override can
        // skip it on A16 without also skipping the two keys that share the old directory and apply
        // there fine. The property under test is unchanged — all four files, in apply order.
        QCOMPARE(sb->patches.size(), 4);
        QCOMPARE(sb->dir, QStringLiteral("frameworks-base-statusbar"));
        for (const char *n : {"0001-statusbar", "0002-statusbar", "0003-statusbar", "0004-statusbar"})
            QVERIFY2(!sb->patches.filter(QLatin1String(n)).isEmpty(),
                     qPrintable(QStringLiteral("merged entry lost %1").arg(QLatin1String(n))));
        // and in apply order, since the icon_blacklist patch is what the other three build on
        QVERIFY(sb->patches.first().startsWith(QLatin1String("0001")));
        QVERIFY(sb->patches.first().contains(QLatin1String("icon_blacklist")));
    }

    void defaultPatchSetIsScopedToTheAndroidVersion() {
        // statusbar_clock_hide / _battery_hide / _tuner_entry were absorbed into
        // statusbar_hide_list (bd remora-82c.5) — the capability is unchanged, the key count is
        // not. canonicalPatchKeysRetiresAbsorbedEntries covers the migration itself.
        // lineage_legal_url and lineage_version_props left this list with their registry entries
        // (bd remora-31sq) — both patched a device project that no longer exists in
        // any tree. The property under test is unchanged: a version-scoped default must be in A17's
        // set and out of A16's.
        const QStringList a17Only{"no_lineage_sepolicy",      "lineage_backport_deps",
                                  "buildprop_touch_rotation", "statusbar_hide_list"};
        for (const QString &k : a17Only) {
            QVERIFY2(defaultSourcePatchSet(17).contains(k), qPrintable("dropped from A17: " + k));
            QVERIFY2(!defaultSourcePatchSet(16).contains(k), qPrintable("still default on A16: " + k));
        }
        // the hardware/build-infra patches carry no scope and must stay default on BOTH
        for (const char *k : {"v4l2_encoder", "v4l2_av1_enum", "minigbm_rendernode",
                              "soong_gomemlimit"})
            for (int v : {16, 17})
                QVERIFY2(defaultSourcePatchSet(v).contains(QLatin1String(k)),
                         qPrintable(QStringLiteral("%1 lost its default on A%2")
                                        .arg(QLatin1String(k)).arg(v)));
        // THE REGRESSION, stated as the live profiles saw it: an A16 profile carrying the whole A16
        // default set must produce NO warning. Before scoping, this one reported all eight.
        QVERIFY(unappliedDefaultPatches(defaultSourcePatchSet(16), 16).isEmpty());
        // …and the A17-only keys must not be silently unreachable — they still warn on A17.
        const QStringList onA17 = unappliedDefaultPatches({"minigbm_rendernode"}, 17);
        for (const QString &k : a17Only) QVERIFY2(onA17.contains(k), qPrintable("lost on A17: " + k));
        // The apply chain is scoped by the SAME version it applies with, or the log contradicts
        // what the build actually did.
        const QStringList a16 =
            buildApplyPatchCommands({"minigbm_rendernode"}, "/src/tree", "/root", 16);
        for (const QString &k : a17Only)
            QVERIFY2(a16.filter(QStringLiteral("NOT APPLIED")).filter(k).isEmpty(),
                     qPrintable("A16 build log still nags about " + k));
    }

    // The union-merge attributes file must be written with printf '%s', NEVER '%%s' (bd
    // remora-4ei.41). QString::arg does not collapse %% — only QString::asprintf does — so the shell
    // received a format with NO conversion specification, printf printed it once and dropped its
    // arguments, and info/attributes became the single line "%s": the merge driver was silently
    // inert, which costs ~24 of the 98 backport patches. It looked fine only because info/attributes
    // lives under .repo/projects/<p>.git/ and survives both `git reset --hard` and `repo sync`, so a
    // hand-written copy from the day the feature landed stayed on disk and kept working.
    void sourcePatchUnionAttributesFormatIsUsable() {
        const QStringList cmds =
            buildApplyPatchCommands({"lineage_platform_backport"}, "/src/tree", "/root", 17);
        const QString attrs = cmds.filter("info/attributes").value(0);
        QVERIFY(!attrs.isEmpty());
        QVERIFY2(attrs.contains("printf '%s\\n'"), qPrintable("bad printf format: " + attrs));
        QVERIFY2(!attrs.contains("'%%s"), "%% reaches the shell verbatim and prints a literal %s");
        QVERIFY(attrs.contains("res/values/cm_strings.xml merge=union"));
    }

    // c2-va must stay OUT of the apply path (bd remora-4ei.42). It used to have two sources of
    // truth: a patch series under vendor/source-patches/ (since removed), and the live tree under
    // vendor/source-build/ that stage-features.sh `rm -rf`s the checkout and replaces with.
    // The staged copy always won — it runs after the apply — so the series was never what built,
    // and by it had drifted 290 lines plus an entire 1449-line VaapiVideoDecoderHEVC.cpp
    // behind. Nothing could have caught that: hardware/remora/c2-va is in no repo manifest, so no
    // sync resets it, and the apply loop skips any patch whose subject slug is already in the log.
    // Re-registering the series would quietly reintroduce a second, older source.
    void c2vaIsNotInTheApplyPath() {
        for (const SourcePatch &p : sourcePatches()) {
            QVERIFY2(!p.projectPath.contains(QLatin1String("c2-va")),
                     qPrintable(QStringLiteral("'%1' puts c2-va back in the apply path — it is "
                                               "staged from vendor/source-build/c2-va, and a patch "
                                               "series can only build something older")
                                    .arg(p.key)));
            QVERIFY(!p.dir.contains(QLatin1String("c2-va")));
        }
        // enabling it by name (a stale remorarc key) must stay a no-op, not resurrect the project
        for (const QString &c : buildApplyPatchCommands({"hw_encoder"}, "/src/tree", "/root", 17))
            QVERIFY2(!c.contains(QLatin1String("c2-va")), qPrintable("c2-va reached the tree: " + c));
    }

    // bd remora-ykhz R3. mesa_source's ONE safety property is that it refuses a partial GL/DRI
    // payload, and it is a property of a MAKEFILE, so nothing in C++ could ever have asserted it.
    // It matters more than its size suggests: the DRI loader ABI changed between the prebuilt Mesa
    // 24.0.8 and 26.x, so a half-swapped cluster links, installs, and then fails at RUNTIME — the
    // one failure mode this project keeps paying for. The Vulkan half must stay INDEPENDENT of
    // that refusal, or the incremental per-driver path the feature exists to offer disappears.
    void mesaSourceRefusesAPartialGlCluster() {
        if (QStandardPaths::findExecutable(QStringLiteral("make")).isEmpty())
            QSKIP("make not on PATH");
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString d = tmp.path();
        for (const QString &sub : {QStringLiteral("lib64/dri"), QStringLiteral("lib64/egl"),
                                   QStringLiteral("lib64/hw"), QStringLiteral("lib64/gbm")})
            QVERIFY(QDir().mkpath(d + QLatin1Char('/') + sub));
        QVERIFY(QFile::copy(QStringLiteral(REMORA_SOURCE_VENDOR_DIR)
                                + QStringLiteral("/source-build/features/mesa_source/mesa.mk"),
                            d + QStringLiteral("/mesa.mk")));
        // inherit-product-if-exists is the only build-system callable mesa.mk still uses; stubbing
        // exactly that keeps the harness from asserting anything the real build does not (an empty
        // LOCAL_PATH made an early run match the HOST's /lib64/libgbm.so.1).
        //
        // THE PAYLOAD ROOT IS REDIRECTED VIA MESA_SRC_ROOT ON make's COMMAND LINE, not by stubbing
        // my-dir. mesa.mk stopped using $(call my-dir) (bd remora-ykhz) — my-dir does not resolve
        // to the file's own directory when a product mk is inherited, which is the bug that shipped
        // an image with no radeon ICD — so it now hard-codes the tree-relative MESA_SRC_ROOT :=
        // vendor/mesa_source. A command-line assignment overrides a makefile's := , so this is the
        // seam that lets the harness point the same file at a synthetic payload. Stubbing my-dir
        // silently stopped working at that change: LOCAL_PATH became the relative
        // vendor/mesa_source, every wildcard found nothing, and the new empty-payload $(error)
        // fired on EVERY case — so the test failed on its first assertion and none of the refusal
        // logic below ran at all.
        {
            QFile h(d + QStringLiteral("/harness.mk"));
            QVERIFY(h.open(QIODevice::WriteOnly | QIODevice::Text));
            h.write("define inherit-product-if-exists\nendef\n"
                    "PRODUCT_PACKAGES :=\nPRODUCT_PACKAGES_REMOVE :=\n"
                    "include $(HARNESS)/mesa.mk\nall:\n"
                    "\t@echo \"P: $(PRODUCT_PACKAGES)\"\n\t@echo \"R: $(PRODUCT_PACKAGES_REMOVE)\"\n");
        }
        const auto run = [&] {
            QProcess p;
            p.start(QStringLiteral("make"),
                    {QStringLiteral("-s"), QStringLiteral("-f"), d + QStringLiteral("/harness.mk"),
                     QStringLiteral("HARNESS=") + d, QStringLiteral("MESA_SRC_ROOT=") + d,
                     QStringLiteral("all")});
            p.waitForFinished(30000);
            return QPair<int, QString>{p.exitCode(),
                                       QString::fromUtf8(p.readAllStandardOutput())
                                           + QString::fromUtf8(p.readAllStandardError())};
        };
        const auto touch = [&](const QString &rel) {
            QFile f(d + QStringLiteral("/lib64/") + rel);
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        // FIVE, not seven — this list tracks what Mesa 26.x ACTUALLY BUILDS, and two members of the
        // 24.0.8 prebuilt cluster are deliberately not in it (bd remora-bm7.4):
        //   libglapi.so.0           shared-glapi was REMOVED from Mesa upstream; there is no
        //                           counterpart to build, and Mesa 26's libEGL_mesa does not link
        //                           it. Its prebuilt stays.
        //   libgallium_drv_video    needs a VA frontend whose libva-drm AOSP's external/libva does
        //                           not define. Its prebuilt stays, and that is not a "mixed pair":
        //                           drv_video is loaded by libva, not by the DRI loader.
        // Requiring either would make the gate refuse every payload the build can produce.
        // SEVEN since bd remora-ykhz.1: gbm/dri_gbm.so is libgbm's runtime backend (a libgbm
        // without it fails gbm_create_device for every consumer, gralloc.gbm included) and
        // libgbm_mesa_wrapper.so is the baked local-gbm allocator wrapper — both produced by the
        // same build-mesa.sh pass, both cluster members by the move-together rule. Order mirrors
        // MESA_SRC_GL_SET; the subset walk below asserts the mk names the NEXT missing entry.
        const QStringList gl = {QStringLiteral("dri/libgallium_dri.so"),
                                QStringLiteral("egl/libEGL_mesa.so"),
                                QStringLiteral("egl/libGLESv1_CM_mesa.so"),
                                QStringLiteral("egl/libGLESv2_mesa.so"),
                                QStringLiteral("libgbm.so.1"),
                                QStringLiteral("gbm/dri_gbm.so"),
                                QStringLiteral("libgbm_mesa_wrapper.so")};

        // NO PAYLOAD AT ALL IS REFUSED, and this assertion is the inverse of what it originally
        // said. It used to read "a clean no-op, NOT an error: the feature can be on while only the
        // Vulkan half has been built" — but bd remora-ykhz established that if this file is being
        // parsed then WITH_MESA_SOURCE is set and stage-features.sh has already verified lib64/
        // exists, so finding NOTHING is a contradiction that can only mean the paths are wrong.
        // Continuing from there removes the prebuilts and installs nothing in their place, which is
        // exactly how an image shipped with no radeon Vulkan ICD at all. The Vulkan-only case below
        // is what covers "only half has been built"; it needs a payload, not an empty directory.
        auto r = run();
        QVERIFY2(r.first != 0, "an empty payload was accepted — a path bug in mesa.mk would go silent "
                               "again and the build would subtract a driver");
        QVERIFY(r.second.contains(QLatin1String("yielded NO drivers")));
        QVERIFY(!r.second.contains(QLatin1String("remora_mesa_")));

        // Vulkan alone proceeds, per driver, and touches nothing in the GL cluster.
        touch(QStringLiteral("hw/vulkan.intel.so"));
        touch(QStringLiteral("hw/vulkan.radeon.so"));
        r = run();
        QCOMPARE(r.first, 0);
        QVERIFY(r.second.contains(QLatin1String("remora_mesa_vulkan_intel")));
        QVERIFY(r.second.contains(QLatin1String("remora_mesa_vulkan_radeon")));
        QVERIFY(r.second.contains(QLatin1String("vulkan.intel")));
        QVERIFY(!r.second.contains(QLatin1String("libEGL_mesa")));

        // ANY proper subset of the GL cluster is refused, and the message names what is missing.
        for (int n = 1; n < gl.size(); ++n) {
            touch(gl.at(n - 1));
            r = run();
            QVERIFY2(r.first != 0, qPrintable(QStringLiteral("%1 of %2 GL files was accepted")
                                                  .arg(n)
                                                  .arg(gl.size())));
            QVERIFY(r.second.contains(QLatin1String("INCOMPLETE")));
            QVERIFY(r.second.contains(gl.at(n)));  // the next one is named as missing
        }

        // …and the complete set is taken, together with the prebuilts it displaces.
        touch(gl.last());
        r = run();
        QCOMPARE(r.first, 0);
        // remora_mesa_glapi is NOT among these any more, and its absence is the assertion: Mesa
        // 26.x does not build a libglapi, so declaring one would claim an install path with no file
        // behind it and leave the prebuilt — the only libglapi that exists — removed.
        for (const QString &m : {QStringLiteral("remora_mesa_gallium_dri"),
                                 QStringLiteral("remora_mesa_egl"), QStringLiteral("remora_mesa_gbm"),
                                 QStringLiteral("remora_mesa_gbm_backend"),
                                 QStringLiteral("remora_mesa_gbm_wrapper")})
            QVERIFY2(r.second.contains(m), qPrintable(m));
        for (const QString &m : {QStringLiteral("remora_mesa_glapi"),
                                 QStringLiteral("remora_mesa_gallium_drv_video")})
            QVERIFY2(!r.second.contains(m), qPrintable(m));
        for (const QString &m : {QStringLiteral("libgallium_dri"), QStringLiteral("libEGL_mesa"),
                                 QStringLiteral("libGLESv1_CM_mesa")})
            QVERIFY2(r.second.contains(m), qPrintable(m));
    }

    // bd remora-22n's residue, generalised: the repo's tests pin the SOURCE tree against the
    // registry, but builds stage from the INSTALLED tree, and nothing checked that one. A binary
    // whose registry names a directory or a file /usr/share does not carry is already fatal — the
    // apply loop hard-fails (bd remora-fgj.11) — but only after repo bring-up. auditPatchAssets
    // asks the same question up front, so `check` can report it and a build can refuse cheaply.
    void patchAssetAuditFindsAStaleTree() {
        // A multi-file entry, so dropping ONE leaves the directory present — that is the partial
        // -staleness shape, distinct from the whole-directory case exercised below. Chosen by
        // shape rather than by name so a registry edit cannot quietly make this test vacuous.
        const SourcePatch *multi = nullptr;
        for (const SourcePatch &p : sourcePatches())
            if (p.patches.size() >= 2) { multi = &p; break; }
        QVERIFY2(multi, "no multi-file patch entry in the registry to audit");
        const QString base = QStringLiteral("source-patches/") + multi->dir;
        const QString first = multi->patches.first();

        // A tree carrying every file the registry names for this entry reports no gap for it.
        FakeVendor full;
        for (const QString &f : multi->patches) full.files[base + QLatin1Char('/') + f] = "x";
        for (const PatchAssetGap &g : auditPatchAssets(17, full))
            QVERIFY2(g.key != multi->key, qPrintable("false gap on a complete series: "
                                                     + g.missing.join(QStringLiteral(","))));

        // Drop one file — the stale-install shape — and it is named, with its directory.
        FakeVendor partial = full;
        partial.files.remove(base + QLatin1Char('/') + first);
        bool found = false;
        for (const PatchAssetGap &g : auditPatchAssets(17, partial))
            if (g.key == multi->key) {
                found = true;
                QVERIFY2(g.missing.contains(first), qPrintable(g.missing.join(QStringLiteral(","))));
                QCOMPARE(g.dir, base);
            }
        QVERIFY2(found, "a missing patch file was not reported — the build would find out late");

        // A SKIP is a deliberate absence for that release, NOT a gap — and it must not mask the
        // OTHER versions, which still resolve to the shared dir and are still audited.
        FakeVendor skipped = partial;
        skipped.files[base + QStringLiteral("-a17/SKIP")] = "";
        for (const PatchAssetGap &g : auditPatchAssets(17, skipped))
            QVERIFY2(g.key != multi->key, "a SKIP'd series was reported as missing files");
        bool still = false;
        for (const PatchAssetGap &g : auditPatchAssets(16, skipped))
            if (g.key == multi->key) still = true;
        QVERIFY2(still, "the -a17 SKIP masked A16's gap — versions must be audited independently");

        // AN ENTRY WITH NO DIRECTORY AT ALL is the headline stale-install shape — a registry entry
        // added after the tree was installed — so it must be reported, not skipped. patchAppliesTo
        // cannot tell that from "this version ships nothing" and correctly hides the row; the
        // apply loop still reaches it if the profile pinned the key, and hard-fails. An empty tree
        // is therefore maximal staleness: every entry is named.
        FakeVendor bare;
        const QVector<PatchAssetGap> all = auditPatchAssets(17, bare);
        QCOMPARE(all.size(), sourcePatches().size());
        for (const PatchAssetGap &g : all)
            QVERIFY2(g.missing.contains(QStringLiteral("<no patch directory>")),
                     qPrintable(g.key + ": " + g.missing.join(QStringLiteral(","))));

        // THE REGRESSION THIS EXISTS FOR, against the REAL tree: every entry the shipped registry
        // names must be backed on every buildable version. A failure here means the repo itself is
        // inconsistent; the same call against /usr/share is what catches a stale install.
        auto sourceTree = [](const QString &rel) {
            QList<QPair<QString, QByteArray>> out;
            const QString root = QStringLiteral(REMORA_SOURCE_VENDOR_DIR) + QLatin1Char('/');
            QDirIterator it(root + rel, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QFile f(it.next());
                if (f.open(QIODevice::ReadOnly))
                    out << qMakePair(f.fileName().mid(root.size()), f.readAll());
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        for (int v : {16, 17})
            for (const PatchAssetGap &g : auditPatchAssets(v, sourceTree))
                QVERIFY2(false, qPrintable(QStringLiteral("A%1 %2: %3 lacks %4")
                                               .arg(v).arg(g.key, g.dir,
                                                           g.missing.join(QStringLiteral(", ")))));
    }

    // The stale-BINARY direction of the same audit (bd remora-y9y9): a .patch file present in a
    // dir the registry claims but NAMED BY NO ENTRY is silently not applied — the build succeeds
    // and ships without it, which is how /usr/bin/remora would have staged five of the six
    // minigbm patches its newer tree carried. Presence-of-what-the-registry-names cannot see it;
    // this side of the audit exists for exactly that file.
    void patchAssetAuditFlagsFilesTheRegistryDoesNotName() {
        const SourcePatch *multi = nullptr;
        for (const SourcePatch &p : sourcePatches())
            if (p.patches.size() >= 2) { multi = &p; break; }
        QVERIFY2(multi, "no multi-file patch entry in the registry to audit");
        const QString base = QStringLiteral("source-patches/") + multi->dir;

        // A complete series plus one file the registry does not name: the stray is reported, on
        // this entry's key, and the declared files are not disturbed into false "missing" rows.
        FakeVendor tree;
        for (const QString &f : multi->patches) tree.files[base + QLatin1Char('/') + f] = "x";
        tree.files[base + QStringLiteral("/9999-added-after-this-binary.patch")] = "y";
        bool flagged = false;
        for (const PatchAssetGap &g : auditPatchAssets(17, tree))
            if (g.key == multi->key) {
                QVERIFY2(g.missing.isEmpty(), qPrintable("false missing: " + g.missing.join(",")));
                flagged = g.unknown.contains(
                    QStringLiteral("9999-added-after-this-binary.patch"));
            }
        QVERIFY2(flagged, "an unnamed .patch in a claimed dir was not reported — a build would "
                          "silently ship without it");

        // NESTED is unnamed too: the apply loop reads the dir's top level only, so a .patch in a
        // subdirectory is just as unapplied, and its basename must not hide behind a declared
        // name it happens to share.
        FakeVendor nested;
        for (const QString &f : multi->patches) nested.files[base + QLatin1Char('/') + f] = "x";
        nested.files[base + QStringLiteral("/sub/") + multi->patches.first()] = "y";
        bool nestedFlagged = false;
        for (const PatchAssetGap &g : auditPatchAssets(17, nested))
            if (g.key == multi->key && g.unknown.contains(QStringLiteral("sub/")
                                                          + multi->patches.first()))
                nestedFlagged = true;
        QVERIFY2(nestedFlagged, "a nested .patch was not reported");

        // ENTRIES SHARE DIRS (frameworks-base carries several), so "unnamed" means named by NO
        // sharing entry — one entry's file must never read as another entry's stray — and a real
        // stray is reported once per sharing key, because the callers scope refusals by key.
        const SourcePatch *first = nullptr, *second = nullptr;
        for (const SourcePatch &p : sourcePatches()) {
            if (p.patches.isEmpty()) continue;
            for (const SourcePatch &q : sourcePatches()) {
                if (&p != &q && q.dir == p.dir && !q.patches.isEmpty()) {
                    first = &p;
                    second = &q;
                    break;
                }
            }
            if (first) break;
        }
        QVERIFY2(first, "no two explicit entries share a dir — pick a new shape for this test");
        const QString shared = QStringLiteral("source-patches/") + first->dir;
        FakeVendor both;
        for (const QString &f : first->patches) both.files[shared + QLatin1Char('/') + f] = "x";
        for (const QString &f : second->patches) both.files[shared + QLatin1Char('/') + f] = "x";
        for (const PatchAssetGap &g : auditPatchAssets(17, both))
            QVERIFY2(g.unknown.isEmpty() || (g.key != first->key && g.key != second->key),
                     qPrintable("a sharing entry's own file read as a stray: "
                                + g.unknown.join(",")));
        both.files[shared + QStringLiteral("/9999-stray.patch")] = "y";
        QSet<QString> strayKeys;
        for (const PatchAssetGap &g : auditPatchAssets(17, both))
            if (g.unknown.contains(QStringLiteral("9999-stray.patch"))) strayKeys.insert(g.key);
        QVERIFY2(strayKeys.contains(first->key) && strayKeys.contains(second->key),
                 "a stray in a shared dir must be reported for every sharing key");

        // A GLOBBED series consumes every NNNN-*.patch by design — nothing in its dir can be
        // unnamed, or regenerating the backport (which renumbers freely) would trip the audit.
        const SourcePatch *globbed = nullptr;
        for (const SourcePatch &p : sourcePatches())
            if (p.patches.isEmpty()) { globbed = &p; break; }
        QVERIFY2(globbed, "no globbed series in the registry to audit");
        FakeVendor glob;
        const QString gbase = QStringLiteral("source-patches/") + globbed->dir;
        glob.files[gbase + QStringLiteral("/0001-anything.patch")] = "x";
        glob.files[gbase + QStringLiteral("/0002-else.patch")] = "x";
        for (const PatchAssetGap &g : auditPatchAssets(17, glob))
            QVERIFY2(g.key != globbed->key || g.unknown.isEmpty(),
                     "a globbed series reported unnamed files — the glob applies them all");

        // A SKIP'd dir is deliberately dead for that release; whatever else it carries is not a
        // skew, it is the series waiting for the versions that do use it.
        FakeVendor skp;
        for (const QString &f : multi->patches) skp.files[base + QLatin1Char('/') + f] = "x";
        skp.files[base + QStringLiteral("/9999-stray.patch")] = "y";
        skp.files[base + QStringLiteral("-a17/SKIP")] = "";
        for (const PatchAssetGap &g : auditPatchAssets(17, skp))
            QVERIFY2(g.key != multi->key || g.unknown.isEmpty(),
                     "a SKIP'd series reported unnamed files");
    }

    // The CONTENT half (bd remora-y9y9 option b): same names on both sides says nothing about
    // same bytes, and the byte-stale failure is the quiet one — a green build shipping the OLD
    // patch. Pure compare over two injected readers; the engine decides which real trees they
    // are and which direction is loud.
    void sourcePatchTreeCompareFindsByteDrift() {
        FakeVendor mine, other;
        const QString d = QStringLiteral("source-patches/some-entry/");
        mine.files[d + QStringLiteral("0001-a.patch")] = "same";
        other.files[d + QStringLiteral("0001-a.patch")] = "same";
        QVERIFY2(compareSourcePatchTrees(mine, other).isEmpty(),
                 "identical trees reported drift");

        // The y9y9 shape itself: one file's bytes moved, every name still present.
        other.files[d + QStringLiteral("0001-a.patch")] = "edited";
        QVector<VendorDriftRow> rows = compareSourcePatchTrees(mine, other);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().dir, QStringLiteral("some-entry"));
        QCOMPARE(rows.first().differs, QStringList{QStringLiteral("0001-a.patch")});
        QVERIFY(rows.first().onlyInMine.isEmpty() && rows.first().onlyInOther.isEmpty());

        // Presence drift lands on the right side of the row, per dir.
        other.files[d + QStringLiteral("0001-a.patch")] = "same";
        mine.files[QStringLiteral("source-patches/new-entry/0001-n.patch")] = "n";
        other.files[QStringLiteral("source-patches/old-entry/0001-o.patch")] = "o";
        rows = compareSourcePatchTrees(mine, other);
        QCOMPARE(rows.size(), 2);  // QMap order: new-entry, old-entry
        QCOMPARE(rows[0].dir, QStringLiteral("new-entry"));
        QCOMPARE(rows[0].onlyInMine, QStringList{QStringLiteral("0001-n.patch")});
        QCOMPARE(rows[1].dir, QStringLiteral("old-entry"));
        QCOMPARE(rows[1].onlyInOther, QStringList{QStringLiteral("0001-o.patch")});

        // A loose file directly under source-patches must be compared, not silently ignored —
        // ignoring one would be this bead all over again.
        FakeVendor looseA, looseB;
        looseA.files[QStringLiteral("source-patches/README")] = "a";
        looseB.files[QStringLiteral("source-patches/README")] = "b";
        rows = compareSourcePatchTrees(looseA, looseB);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().dir, QStringLiteral("<root>"));
    }

    // The per-project state and mark files are named after the project path, so two projects must
    // never slug to the same name — one would silently answer for the other.
    void patchProjectSlugsAreDistinct() {
        QStringList slugs;
        for (const QString &p : sourcePatchProjects()) slugs << patchProjectSlug(p);
        QCOMPARE(slugs.size(), sourcePatchProjects().size());
        QCOMPARE(QSet<QString>(slugs.cbegin(), slugs.cend()).size(), slugs.size());
        // nested paths flatten. device/lineage/sepolicy is the only live project with two
        // slashes, so it is also the only thing keeping
        // this arm honest — if it ever goes, flattening loses its last witness.
        QVERIFY(slugs.contains("device-lineage-sepolicy"));
        for (const QString &s : slugs) QVERIFY(!s.contains(QLatin1Char('/')));
    }

    // THE case this whole mechanism exists to survive: a patch's CONTENT changes while its SUBJECT —
    // and so its filename, and the slug the apply loop skips on — does not. That is the common shape
    // of iterating on a fix, and if the fingerprint does not move, the project is never force-synced,
    // `git log --format=%f` still matches the old commit, and THE OLD PATCH SILENTLY BUILDS.
    // bd remora-82c.3 — patch applicability is DERIVED from the assets, so the set of offerable
    // version/patch combinations widens automatically as per-version series are added, instead of
    // tracking a hand-maintained allow-list that can only go stale and shrink.
    void patchApplicabilityFollowsTheAssets() {
        const SourcePatch *shared = nullptr;   // a series with only the version-neutral dir
        const SourcePatch *perVer = nullptr;   // one that also has an -a17 override
        for (const SourcePatch &p : sourcePatches()) {
            if (p.dir == QLatin1String("frameworks-base")) perVer = &p;
            if (p.dir == QLatin1String("device-remora")) shared = &p;
        }
        QVERIFY(perVer);
        QVERIFY(shared);

        FakeVendor v;
        // shared series only → applies to every version, including ones nobody has heard of
        v.files["source-patches/frameworks-base/0001-x.patch"] = "shared";
        QVERIFY(patchAppliesTo(*perVer, 16, v));
        QVERIFY(patchAppliesTo(*perVer, 17, v));
        QVERIFY(patchAppliesTo(*perVer, 18, v));

        // an -a17 override does not narrow the others — 16 and 18 still resolve to the shared dir
        v.files["source-patches/frameworks-base-a17/0001-x.patch"] = "a17";
        QVERIFY(patchAppliesTo(*perVer, 16, v));
        QVERIFY(patchAppliesTo(*perVer, 17, v));
        QVERIFY(patchAppliesTo(*perVer, 18, v));

        // SKIP means the series is absorbed upstream for that release: nothing to apply, so it is
        // not offered THERE and stays offered everywhere else.
        FakeVendor sk;
        sk.files["source-patches/frameworks-base/0001-x.patch"] = "shared";
        sk.files["source-patches/frameworks-base-a17/SKIP"] = "";
        QVERIFY(!patchAppliesTo(*perVer, 17, sk));
        QVERIFY(patchAppliesTo(*perVer, 16, sk));

        // no assets at all → not offered on any version
        FakeVendor none;
        QVERIFY(!patchAppliesTo(*perVer, 16, none));
        QVERIFY(!patchAppliesTo(*perVer, 17, none));
        QVERIFY(!patchAppliesTo(*shared, 17, none));
    }

    void applicableKeysAreASubsetOfTheRegistry() {
        FakeVendor v;
        v.files["source-patches/frameworks-base/0001-x.patch"] = "one";
        const QStringList keys = applicableSourcePatchKeys(17, v);
        QVERIFY(!keys.isEmpty());
        QVERIFY(keys.size() < sourcePatches().size());  // only the one with assets
        for (const QString &k : keys) {
            bool known = false;
            for (const SourcePatch &p : sourcePatches())
                if (p.key == k) known = true;
            QVERIFY(known);
        }
    }

    void patchFingerprintsScopeToOneProject() {
        FakeVendor v;
        v.files["source-build/apply-container-patches.sh"] = "container";
        v.files["source-build/container-patches/android-17.0.0/system/core/0001-boot.patch"] = "cc";
        v.files["source-build/pinned-manifest-a17.xml"] = "<manifest/>";
        v.files["source-patches/frameworks-base/0001-surfaceview-ignore-secure.patch"] = "one";
        v.files["source-patches/external-minigbm/0001-cros_gralloc-honor.patch"] = "two";
        const QStringList on{"surfaceview_secure", "minigbm_rendernode"};
        const QMap<QString, QString> base = patchProjectFingerprints(on, 17, v);
        // EVERY registered project is fingerprinted, not just the ones with something enabled:
        // unticking the last patch of a project has to register as a change for that project, and by
        // then it is not in `enabled` at all.
        QCOMPARE(base.size(), sourcePatchProjects().size());
        for (const QString &p : sourcePatchProjects()) QVERIFY(base.contains(p));

        // content edited, subject and filename identical
        FakeVendor edited = v;
        edited.files["source-patches/frameworks-base/0001-surfaceview-ignore-secure.patch"] =
            "one, but fixed";
        const QMap<QString, QString> after = patchProjectFingerprints(on, 17, edited);
        QVERIFY(after["frameworks/base"] != base["frameworks/base"]);
        // ...and NO other project is disturbed, which is the whole point: the other ~1180 projects
        // keep their mtimes and Soong keeps its analysis of them.
        for (const QString &p : sourcePatchProjects())
            if (p != QLatin1String("frameworks/base")) QCOMPARE(after[p], base[p]);
        // A source-patch edit must NOT move the repo fingerprint — that would force the global sync
        // this change exists to avoid.
        QCOMPARE(repoStateFingerprint("u", "r", true, 17, edited),
                 repoStateFingerprint("u", "r", true, 17, v));

        // Unticking a project's only enabled patch is a change FOR THAT PROJECT: its old commits are
        // still applied and have to be synced away.
        const QMap<QString, QString> off = patchProjectFingerprints({"minigbm_rendernode"}, 17, v);
        QVERIFY(off["frameworks/base"] != base["frameworks/base"]);
        QCOMPARE(off["external/minigbm"], base["external/minigbm"]);

        // A per-version override directory is an input even though the shell picks between them.
        FakeVendor override17 = v;
        override17.files["source-patches/frameworks-base-a17/0001-surfaceview-ignore-secure.patch"] =
            "a17 variant";
        QVERIFY(patchProjectFingerprints(on, 17, override17)["frameworks/base"]
                != base["frameworks/base"]);
        // ...but only for the version that would read it.
        QCOMPARE(patchProjectFingerprints(on, 16, override17)["frameworks/base"],
                 patchProjectFingerprints(on, 16, v)["frameworks/base"]);
    }

    // The repo fingerprint owns everything a per-project sync cannot fix. The container-compat
    // patches are in it even though they are patches: they land on ~30 projects that have no source
    // patches and therefore no per-project record, so the only safe answer to a change is a global
    // sync of the whole tree.
    void repoFingerprintCoversWhatOnlyAGlobalSyncCanFix() {
        FakeVendor v;
        v.files["source-build/apply-container-patches.sh"] = "container";
        v.files["source-build/container-patches/android-17.0.0/system/core/0001-boot.patch"] = "cc";
        v.files["source-build/container-patches/android-16.0.0_r2/system/core/0001-boot.patch"] = "old";
        v.files["source-build/pinned-manifest-a17.xml"] = "<manifest/>";
        v.files["source-build/local_manifests/remora-a17.xml"] = "<local/>";
        const QString base = repoStateFingerprint("url", "lineage-24.0", true, 17, v);
        QVERIFY(repoStateFingerprint("other", "lineage-24.0", true, 17, v) != base);
        QVERIFY(repoStateFingerprint("url", "lineage-23.2", true, 17, v) != base);
        QVERIFY(repoStateFingerprint("url", "lineage-24.0", false, 17, v) != base);
        QVERIFY(repoStateFingerprint("url", "lineage-24.0", true, 16, v) != base);
        // the container patch set for THIS version
        FakeVendor cc = v;
        cc.files["source-build/container-patches/android-17.0.0/system/core/0001-boot.patch"] = "cc2";
        QVERIFY(repoStateFingerprint("url", "lineage-24.0", true, 17, cc) != base);
        // ...and not another version's, which cannot reach this tree
        FakeVendor other = v;
        other.files["source-build/container-patches/android-16.0.0_r2/system/core/0001-boot.patch"] = "x";
        QCOMPARE(repoStateFingerprint("url", "lineage-24.0", true, 17, other), base);
        // the pinned manifest when pinned...
        FakeVendor pin = v;
        pin.files["source-build/pinned-manifest-a17.xml"] = "<manifest2/>";
        QVERIFY(repoStateFingerprint("url", "lineage-24.0", true, 17, pin) != base);
        // ...and the local manifests only when they are the ones being used (a pinned build deletes
        // them, so hashing them there would force full passes that cannot change anything).
        FakeVendor loc = v;
        loc.files["source-build/local_manifests/remora-a17.xml"] = "<local2/>";
        QCOMPARE(repoStateFingerprint("url", "lineage-24.0", true, 17, loc), base);
        QVERIFY(repoStateFingerprint("url", "lineage-24.0", false, 17, loc)
                != repoStateFingerprint("url", "lineage-24.0", false, 17, v));
    }

    // A kind that says AOSP while the profile's evidence says LineageOS is refused with the
    // reason named, never reinterpreted: the aosp arm skips the manifest sync and every
    // container-compat patch, so building anyway produces the wrong image silently. One stray
    // combo flip cost a 22-minute aborted build.
    void aospKindConflictNamesTheLineageEvidence() {
        // a lineage branch ref is lineage-exclusive; android-* tags are what AOSP syncs
        QVERIFY(aospKindConflict("lineage-24.0", {}).contains("lineage-24.0"));
        QVERIFY(aospKindConflict("android-17.0.0_r1", {}).isEmpty());
        QVERIFY(aospKindConflict(QString(), {}).isEmpty());
        // lineage-tree patches are named, non-lineage ones are not
        QVERIFY(aospKindConflict(QString(), {"lineage_soong"}).contains("lineage_soong"));
        QVERIFY(aospKindConflict(QString(), {"minigbm_rendernode"}).isEmpty());
        // both reasons compose into one refusal
        const QString both = aospKindConflict("lineage-24.0", {"lineage_soong"});
        QVERIFY(both.contains("lineage-24.0") && both.contains("lineage_soong"));
    }

    // bd remora-bahe — the container-patched projects, enumerated from the patchset the same way
    // apply-container-patches.sh walks it: dir ⇒ project, only dirs that actually hold patches.
    void containerPatchProjectsEnumerateThePatchset() {
        FakeVendor v;
        v.files["source-build/container-patches/android-17.0.0/system/core/0001-boot.patch"] = "a";
        v.files["source-build/container-patches/android-17.0.0/device/gpu-prebuilts/0001-va.patch"] = "b";
        v.files["source-build/container-patches/android-17.0.0/device/gpu-prebuilts/0002-ihd.patch"] = "c";
        v.files["source-build/container-patches/android-17.0.0/README.md"] = "prose, not a patch";
        QCOMPARE(containerPatchProjects(17, v),
                 (QStringList{"device/gpu-prebuilts", "system/core"}));
        // no patchset registered for the version ⇒ empty, matching the apply step's own refusal
        QCOMPARE(containerPatchProjects(6, v), QStringList());
    }

    // bd remora-bahe — a matching input fingerprint alone no longer skips a project: the state
    // records each project's post-apply HEAD and the prologue verifies the tree still sits on it.
    // Measured failure this guards: a prebuilts project at the bare pin (its 3 container
    // patches lost by an interrupted pass) while the record said applied — the incremental pass
    // applied nothing and kati died on a duplicate iHD_drv_video module.
    void patchStateVerifiesTheTreeNotJustItsOwnRecords() {
        const QMap<QString, QString> fp{{QStringLiteral("frameworks/base"), QStringLiteral("f1")}};
        const QStringList cc{QStringLiteral("device/gpu-prebuilts")};
        const QString pro = buildPatchStatePrologue("/src/tree", "repofp", fp, cc);
        // the container projects ride as slug|path entries of their own — they have no input
        // fingerprint, so the HEAD record is their only per-project state
        QVERIFY(pro.contains("RP_C='cp-device_gpu-prebuilts|device/gpu-prebuilts'"));
        // THE SLUG IS A CONTRACT WITH apply-container-patches.sh, not a private choice
        // (bd remora-taab). That script stamps the SAME state directory as this code reads, using
        // "cp-" + the project path with slashes as UNDERSCORES. While this side used
        // patchProjectSlug's hyphens instead, the prologue's unrecognised-record sweep rejected
        // all 24 container stamps on EVERY build and forced a full reset, sync and re-apply — the
        // exact work the state exists to skip. Pin the script's own line so the two cannot drift
        // apart again silently; a change there must be made here too.
        QFile acp(QStringLiteral(REMORA_SOURCE_VENDOR_DIR)
                  + QStringLiteral("/source-build/apply-container-patches.sh"));
        QVERIFY(acp.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(acp.readAll())
                     .contains(QStringLiteral("cp-$(echo \"$1\" | tr '/' '_')")),
                 "apply-container-patches.sh changed how it names its stamps — cpEntries() must "
                 "produce the same names or every build forces a full pass (bd remora-taab)");
        // both loops verify against the tree, not the bookkeeping
        QVERIFY(pro.contains("rev-parse HEAD"));
        QVERIFY(pro.contains("h-\"$s\""));
        QVERIFY(pro.contains("bd remora-bahe"));
        // the unrecognised-record sweep must accept the h- records or every second build would
        // read its own HEAD records as foreign and force a full pass
        QVERIFY(pro.contains("n=${b#h-}"));
        // bd remora-taab / bd remora-bahe — a project in BOTH sets must not keep an enabled-patch
        // skip mark once the CONTAINER check has sent it for a re-sync: the reset wipes the
        // enabled patches too, so skipping their re-apply ships a half-patched project. This
        // fired for real the first time the fast path was ever reached — external/v4l2_codec2 was
        // re-synced while its own patch stayed marked applied, and c2-va then failed to compile
        // against a VideoCodec enum missing the AV1 member that patch adds.
        QVERIFY2(pro.contains("[ \"$xp\" = \"$p\" ]"),
                 "the RP_C re-sync branch must invalidate RP_E state for the same project");
        // aosp-kind: no container machinery, no container entries — empty, not absent
        QVERIFY(buildPatchStatePrologue("/src/tree", "repofp", fp, {}).contains("RP_C=''"));
        // the epilogue writes the HEAD records the prologue reads, for BOTH sets
        const QString epi = buildPatchStateEpilogue("/src/tree", "repofp");
        QVERIFY(epi.contains("for e in $RP_E $RP_C"));
        QVERIFY(epi.contains("rev-parse HEAD"));
        QVERIFY(epi.contains("h-\"$s\""));
    }

    // The guard is per COMMAND but the condition is per PROJECT, and that has to stay true: the apply
    // steps set $d (the resolved patch directory) in one command and dereference it in the next, so a
    // guard that let one through while blocking the other would apply this group's patches out of the
    // PREVIOUS group's directory — the failure this file already documents shipping once.
    void patchGuardsKeepProjectGroupsWhole() {
        const QMap<QString, QString> marks = patchProjectMarks(QStringLiteral("/src/tree"));
        QStringList keys;
        for (const SourcePatch &p : sourcePatches()) keys << p.key;
        for (int ver : {16, 17}) {
            const QStringList cmds =
                buildApplyPatchCommands(keys, "/src/tree", "/root", ver, marks);
            QVERIFY(!cmds.isEmpty());
            QString group;  // the guard of the `d=` assignment currently in effect
            int checked = 0;
            for (const QString &c : cmds) {
                const QString g = guardOf(c);
                QVERIFY2(!g.isEmpty(), qPrintable("unguarded patch step: " + c.left(90)));
                QVERIFY2(marks.values().contains(g), qPrintable("guard is not a project mark: " + g));
                if (c.contains(QLatin1String("d=/root/"))) {
                    group = g;
                    continue;
                }
                if (!c.contains(QLatin1String("$d"))) continue;
                ++checked;
                QVERIFY2(g == group,
                         qPrintable(QStringLiteral("step reads $d under a different guard (%1 vs "
                                                   "%2): %3").arg(g, group, c.left(90))));
            }
            QVERIFY(checked > 20);  // the assertion above is only worth anything if it ran
        }
        // Without marks nothing is guarded — that is the standalone "Apply patches" button, which
        // always applies everything.
        for (const QString &c : buildApplyPatchCommands(keys, "/src/tree", "/root", 17))
            QVERIFY(guardOf(c).isEmpty());
        // The pre-sync reset is guarded the same way: resetting a project the pass is leaving alone
        // would rewrite files for nothing.
        for (const QString &c : buildResetCommands(keys, "/src/tree", marks))
            QVERIFY(marks.values().contains(guardOf(c)));
    }

    // The prologue and the epilogue are the only places here where a wrong answer is SILENT: a
    // surviving mark means a project is skipped and its OLD patches ship. Substring assertions cannot
    // show that, so this runs the generated shell for real against a temporary tree. Still an offline
    // unit test — /bin/sh and a temp directory, no network, no docker, no source tree.
    void patchStateShellScopesWorkPerProject() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString tree = tmp.path();
        const PatchStateFiles f = patchStateFiles(tree);
        // frameworks/base is a manifest project, so repo can reset it. hardware/remora/c2-va is not
        // (Remora creates it), and it must never reach `repo sync`, which would abort the build with
        // "project not found".
        //
        // MEMBERSHIP IS .repo/project.list, which repo rewrites on every sync — NOT the presence of
        // a .repo/projects/<p>.git object store, which outlives its manifest entry. c2-va is given
        // exactly such a stale store below and must STILL stay off the sync list. That combination
        // is not hypothetical: device/remora kept a store from before it became a staged project,
        // the old test passed, and the build died on "error: project device/remora not found"
        // (bd remora-ykhz).
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/.repo/projects/frameworks/base.git")));
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/.repo/projects/hardware/remora/c2-va.git")));
        {
            QFile pl(tree + QStringLiteral("/.repo/project.list"));
            QVERIFY(pl.open(QIODevice::WriteOnly | QIODevice::Text));
            pl.write("frameworks/base\n");
        }
        const auto fps = [](const QString &fb, const QString &c2) {
            return QMap<QString, QString>{{QStringLiteral("frameworks/base"), fb},
                                          {QStringLiteral("hardware/remora/c2-va"), c2}};
        };
        const auto sh = [](const QString &cmd) {
            QProcess p;
            p.start(QStringLiteral("sh"), {QStringLiteral("-c"), cmd});
            if (!p.waitForFinished(30000)) return -1;
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        };
        const auto lines = [](const QString &path) {
            QFile fh(path);
            if (!fh.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(fh.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        };
        const QString fbMark = f.markDir + QStringLiteral("/frameworks-base");
        const QString c2Mark = f.markDir + QStringLiteral("/hardware-remora-c2-va");
        const QString fbState = f.stateDir + QStringLiteral("/frameworks-base");
        // The prologue and epilogue run in ONE shell in production and the epilogue reads $RP_E from
        // the prologue, so a "successful build" is modelled by joining them exactly as the build does.
        const auto pass = [&](const QString &repoFp, const QString &fb, const QString &c2,
                              const QStringList &cc = {}) {
            return sh(buildPatchStatePrologue(tree, repoFp, fps(fb, c2), cc)
                      + QStringLiteral(" && ") + buildPatchStateEpilogue(tree, repoFp));
        };

        // 1. First build: no records → full pass, no marks, nothing to sync piecemeal.
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R1", fps("A", "B"), {})), 0);
        QVERIFY(!QFile::exists(f.skipMark));
        QVERIFY(!QFile::exists(fbMark));
        QVERIFY(lines(f.syncList).isEmpty());

        // 2. The build succeeds: one record per project, marks cleaned up behind it.
        QCOMPARE(pass("R1", "A", "B"), 0);
        QCOMPARE(lines(f.repoState), QStringList{"R1"});
        QCOMPARE(lines(fbState), QStringList{"A"});
        QCOMPARE(lines(f.stateDir + QStringLiteral("/hardware-remora-c2-va")), QStringList{"B"});
        QVERIFY(!QFile::exists(f.skipMark));
        QVERIFY(!QFile::exists(fbMark));

        // 3. Nothing changed → the fast path: everything marked, nothing synced, Soong keeps the lot.
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R1", fps("A", "B"), {})), 0);
        QVERIFY(QFile::exists(f.skipMark));
        QVERIFY(QFile::exists(fbMark));
        QVERIFY(QFile::exists(c2Mark));
        QVERIFY(lines(f.syncList).isEmpty());

        // 4. ONE project's patches changed. Only it loses its mark and gets force-synced. Its stale
        //    record is dropped NOW rather than after the build, so a build that dies half way cannot
        //    leave a record claiming a state the tree is not in.
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R1", fps("A2", "B"), {})), 0);
        QVERIFY(QFile::exists(f.skipMark));
        QVERIFY(!QFile::exists(fbMark));
        QVERIFY(QFile::exists(c2Mark));
        QVERIFY(!QFile::exists(fbState));
        QCOMPARE(lines(f.syncList), QStringList{"frameworks/base"});

        // 5. A project repo does not manage is re-applied but never handed to `repo sync`.
        QCOMPARE(pass("R1", "A2", "B"), 0);
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R1", fps("A2", "B2"), {})), 0);
        QVERIFY(QFile::exists(f.skipMark));
        QVERIFY(QFile::exists(fbMark));
        QVERIFY(!QFile::exists(c2Mark));
        QVERIFY(lines(f.syncList).isEmpty());

        // 6. FAIL-SAFE. A changed repo fingerprint means a global sync, which wipes every project, so
        //    every per-project record has to go BEFORE it runs — a survivor would claim "applied" for
        //    a project that was just reset to upstream. Made safe by construction, not by reasoning.
        QCOMPARE(pass("R1", "A2", "B2"), 0);
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R2", fps("A2", "B2"), {})), 0);
        QVERIFY(!QFile::exists(f.skipMark));
        QVERIFY(QDir(f.stateDir).entryList(QDir::Files).isEmpty());
        QVERIFY(!QFile::exists(fbMark));
        QVERIFY(!QFile::exists(c2Mark));

        // 7. A record for a project this build does not manage — a registry entry that went away, or
        //    a tree written by a different Remora. Nothing can be concluded about it, so: full pass.
        QCOMPARE(pass("R2", "A2", "B2"), 0);
        QFile orphan(f.stateDir + QStringLiteral("/some-old-project"));
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.write("X");
        orphan.close();
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R2", fps("A2", "B2"), {})), 0);
        QVERIFY(!QFile::exists(f.skipMark));
        QVERIFY(QDir(f.stateDir).entryList(QDir::Files).isEmpty());

        // 8. bd remora-bahe — the records all say applied and no INPUT changed, but the TREE
        //    moved: a git project off its post-apply HEAD must be re-synced, and only it. This is
        //    the exact shape that shipped a duplicate iHD_drv_video into kati: the container-only
        //    project (no input fingerprint of its own) reset to the bare pin while the incremental
        //    pass concluded "no project was re-synced, so none needs re-patching".
        const QString ccProj = QStringLiteral("device/gpu-prebuilts");
        const QString ccDir = tree + QLatin1Char('/') + ccProj;
        QVERIFY(QDir().mkpath(ccDir));
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/.repo/projects/") + ccProj
                              + QStringLiteral(".git")));
        {   // ...and into project.list, since that is what decides manifest membership now
            QFile pl(tree + QStringLiteral("/.repo/project.list"));
            QVERIFY(pl.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text));
            pl.write(ccProj.toUtf8() + "\n");
        }
        const auto gitq = [&](const QStringList &args) {
            QProcess p;
            p.setWorkingDirectory(ccDir);
            p.start(QStringLiteral("git"), args);
            if (!p.waitForFinished(15000)) return -1;
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        };
        const auto commitAll = [&](const char *msg) {
            QFile w(ccDir + QStringLiteral("/Android.mk"));
            QVERIFY(w.open(QIODevice::WriteOnly));
            w.write(msg);
            w.close();
            QCOMPARE(gitq({"add", "-A"}), 0);
            QCOMPARE(gitq({"-c", "user.email=t@local", "-c", "user.name=t", "commit", "-qm",
                           QString::fromLatin1(msg)}), 0);
        };
        QCOMPARE(gitq({"init", "-q"}), 0);
        commitAll("post-apply state");
        QCOMPARE(pass("R2", "A2", "B2", {ccProj}), 0);  // records the container project's HEAD
        // unchanged tree → the fast path still holds with the container project watched
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R2", fps("A2", "B2"), {ccProj})), 0);
        QVERIFY(QFile::exists(f.skipMark));
        QVERIFY(lines(f.syncList).isEmpty());
        // the tree drifts behind the bookkeeping's back (here a new commit; the measured case was
        // a reset to the pin — any HEAD move means the recorded state is gone)
        commitAll("drift");
        QCOMPARE(sh(buildPatchStatePrologue(tree, "R2", fps("A2", "B2"), {ccProj})), 0);
        QVERIFY(QFile::exists(f.skipMark));  // still incremental: Soong keeps the other ~1200
        QCOMPARE(lines(f.syncList), QStringList{ccProj});  // ...but the drifted project re-syncs
    }

    // The targeted sync and the container-compat pass must both be inert on the paths where they do
    // not belong: no `repo sync` when the global bring-up already ran, and no container pass over
    // projects that still carry theirs.
    void targetedSyncAndContainerPatchesFollowTheSkipMark() {
        const PatchStateFiles f = patchStateFiles(QStringLiteral("/src/tree"));
        // Every one of these is joined into the build chain with " && ", so each has to be a SINGLE
        // compound command. A bare top-level ';' splits the chain and lets the build run on after a
        // failed step — which the first cut of the global version of this did.
        const QString pro = buildPatchStatePrologue(QStringLiteral("/src/tree"), "FP",
                                                    {{QStringLiteral("frameworks/base"), "a"}},
                                                    {QStringLiteral("system/core")});
        QVERIFY(pro.startsWith(QStringLiteral("{ ")) && pro.endsWith(QStringLiteral("; }")));
        const QString epi = buildPatchStateEpilogue(QStringLiteral("/src/tree"), "FP");
        QVERIFY(epi.startsWith(QStringLiteral("{ ")) && epi.endsWith(QStringLiteral("; }")));
        // ...and the bookkeeping all lives at the tree ROOT, outside every repo project, so a sync
        // of any project cannot wipe the record that says what that project holds.
        for (const QString &p : {f.repoState, f.stateDir, f.skipMark, f.markDir, f.syncList})
            QCOMPARE(p.mid(QStringLiteral("/src/tree/").size()).contains(QLatin1Char('/')), false);
        const QString sync = buildTargetedSyncCommand(QStringLiteral("/src/tree"));
        // only when the global sync was skipped AND something actually needs it
        QVERIFY(sync.startsWith(QStringLiteral("if [ -f %1 ] && [ -s %2 ]; then")
                                    .arg(f.skipMark, f.syncList)));
        QVERIFY(sync.contains("repo sync -c -j8 --fail-fast --force-sync --verbose $(cat "));
        QVERIFY(sync.contains("cd /src/tree &&"));  // the bring-up that cds into the tree did not run
        QVERIFY(sync.endsWith(QStringLiteral("; fi")));  // one compound: && -join safe
        // bd remora-4ei.81: repo's exit code lied ("Checking out local projects failed", exit 0),
        // so the sync is followed by a HEAD-vs-manifest verify — restricted to the synced projects
        // — and it must sit BEFORE the lfs pull, whose `|| true` is the one legitimate swallow.
        const QString verify = repoSyncVerifyCommand(QStringLiteral("$(cat %1)").arg(f.syncList));
        QVERIFY(sync.contains(verify));
        QVERIFY(sync.indexOf(verify) < sync.indexOf(QStringLiteral("git lfs pull")));
        QVERIFY(verify.contains(QStringLiteral("-e")));  // one bad project fails the whole forall
        QVERIFY(verify.contains(QStringLiteral("exit 1")));
        QVERIFY(verify.contains(QStringLiteral("$REPO_LREV")));  // judged against the manifest
        QVERIFY(!verify.contains(QLatin1Char('%')));  // safe inside callers' .arg templates

        const QString cc = buildContainerPatchCommand(QStringLiteral("/src/tree"),
                                                      QStringLiteral("/sb"), 17);
        QVERIFY(cc.contains("/sb/container-patches/android-17.0.0"));
        QVERIFY(!cc.contains("android-16"));  // version sets never cross over
        QVERIFY(cc.contains(QStringLiteral("if [ -s %1 ]; then").arg(f.syncList)));
        QVERIFY(cc.contains(QStringLiteral("$(cat %1)").arg(f.syncList)));  // restricted form
        QVERIFY(cc.endsWith(QStringLiteral("; fi")));
        QCOMPARE(buildContainerPatchCommand("/src/tree", "/sb", 16)
                     .contains("/sb/container-patches/android-16.0.0_r2"), true);
    }

    void optInBootArgs() {
        ResolvedConfig c = bareCfg();
        c.fps = 60;
        c.useOverlayfs = true;
        c.dataBaseDir = "/home/user/.remora-data-base";
        c.roOverrides = QStringList{"secure=0"};
        c.dns = QStringList{"192.168.0.4"};
        const QStringList argv = buildDockerRunArgv(c);
        QVERIFY(argv.contains("androidboot.remora_fps=60"));
        QVERIFY(argv.contains("androidboot.use_remora_overlayfs=1"));
        QVERIFY(argv.contains("androidboot.ro.secure=0"));
        QVERIFY(argv.contains("androidboot.remora_net_ndns=1"));
        QVERIFY(argv.contains("androidboot.remora_net_dns1=192.168.0.4"));
    }

    // ── bd remora-4u4.2: per-profile identity, pinned at creation ──

    void profileSlugging() {
        QCOMPARE(profileSlug(QStringLiteral("Android 17")), QStringLiteral("android-17"));
        QCOMPARE(profileSlug(QStringLiteral("  My Phone!!  ")), QStringLiteral("my-phone"));
        QCOMPARE(profileSlug(QStringLiteral("a//b__c")), QStringLiteral("a-b-c"));
        // never empty, or the caller builds "~/.remora-" and two such profiles collide
        QCOMPARE(profileSlug(QStringLiteral("!!!")), QStringLiteral("profile"));
        QCOMPARE(profileSlug(QString()), QStringLiteral("profile"));
    }

    void instanceIdentityIsPerProfileAndPortsDoNotCollide() {
        // first profile on a fresh install keeps the historical 5555 — single-instance unchanged
        const InstanceIdentity a =
            allocateInstanceIdentity(QStringLiteral("Android 17"), QStringLiteral("/home/user"), {});
        QCOMPARE(a.containerName, QStringLiteral("remora-android-17"));
        // named for the profile, nothing else — the folder on disk has to be recognisable
        QCOMPARE(a.dataDir, QStringLiteral("/home/user/.remora-android-17"));
        QCOMPARE(a.hostAdbPort, 5555);
        // second profile gets its own everything — this is the whole bead
        const InstanceIdentity b = allocateInstanceIdentity(
            QStringLiteral("Android 16"), QStringLiteral("/home/user"), {5555});
        QCOMPARE(b.containerName, QStringLiteral("remora-android-16"));
        QCOMPARE(b.dataDir, QStringLiteral("/home/user/.remora-android-16"));
        QCOMPARE(b.hostAdbPort, 5556);
        QVERIFY(a.containerName != b.containerName);
        QVERIFY(a.dataDir != b.dataDir);
        QVERIFY(a.hostAdbPort != b.hostAdbPort);
        // holes are filled rather than skipped past, so ports stay dense and predictable
        QCOMPARE(allocateInstanceIdentity(QStringLiteral("x"), QStringLiteral("/h"), {5555, 5557})
                     .hostAdbPort,
                 5556);
    }

    // The resolver's per-backend defaults must NOT move. An existing profile that pins nothing
    // already resolves to them, and repointing its dataDir would hand a live instance an empty
    // /data. Creation-time pinning is what makes new profiles distinct; the fallbacks stay put.
    // RENAMED (bd remora-28ix.4 step 4), and the test name is now a promise about SHAPE
    // rather than about strings. These per-backend fallbacks were deliberately left on the
    // pre-rename spelling long after the identity flip, because they decide where an UNPINNED
    // profile's /data lives and moving them relocates it. Two things closed that: every profile
    // created since the flip gets remora-<slug> from allocateInstanceIdentity, so only a profile
    // that never pinned a name reaches these; and the adoption shim that used to carry such a
    // container across is gone, so nothing was preserving them anyway.
    // CONSEQUENCE, stated rather than discovered: an unpinned profile deployed before this looks
    // for a new, empty data dir and comes up as a fresh device. Its old one is untouched on disk;
    // pinning data_dir to it in remorarc restores the instance exactly.
    void resolverBackendDefaultsAreRemoraNamed() {
        RemoraConfig rc;
        QCOMPARE(resolve(rc, Backend::Bare).containerName, QStringLiteral("remora-bm"));
        QVERIFY(resolve(rc, Backend::Bare).dataDir.endsWith(QStringLiteral("/.remora-bm-data")));
        QCOMPARE(resolve(rc, Backend::Bare).hostAdbPort, 5555);
        QCOMPARE(resolve(rc, Backend::Remote).containerName, QStringLiteral("remora-remote"));
        // and every resolved default carries the Remora identity, nothing else
        static const QRegularExpression remoraLeaf(QStringLiteral("/\\.?remora-[^/]+$"));
        for (Backend b : {Backend::Bare, Backend::Remote}) {
            const ResolvedConfig r = resolve(rc, b);
            QVERIFY2(r.containerName.startsWith(QLatin1String("remora-")),
                     qPrintable(r.containerName));
            QVERIFY2(remoraLeaf.match(r.dataDir).hasMatch(), qPrintable(r.dataDir));
            QVERIFY2(remoraLeaf.match(r.dataBaseDir).hasMatch(), qPrintable(r.dataBaseDir));
            for (const QString &id : {r.containerName, r.dataDir, r.dataBaseDir})
                for (const QString &name : forbiddenNames())
                    QVERIFY2(!id.contains(name, Qt::CaseInsensitive), qPrintable(id));
        }
    }

    // bd remora-4u4.9 — a CAPTURING profile gets its own venus socket dir, hence its own render
    // server. Frame capture is fixed at server start, so a host_encode profile needs a server to
    // itself by definition; deriving the path from the container name makes two impossible to
    // collide without probing anything.
    void hostEncodeProfilesGetTheirOwnVenusSocketDir() {
        RemoraConfig rc;
        rc.gpu.mode = GpuMode::Host;
        rc.gpu.venus = true;
        rc.mirror.hostEncode = true;
        rc.backend.containerName = QStringLiteral("remora-alpha");
        const ResolvedConfig a = resolve(rc, Backend::Bare);
        QVERIFY(a.hostEncode);
        QVERIFY(a.venusSocketDir.has_value());
        QVERIFY(a.venusSocketDir->endsWith(QStringLiteral("/venus-remora-alpha")));

        rc.backend.containerName = QStringLiteral("remora-beta");
        const ResolvedConfig b = resolve(rc, Backend::Bare);
        QVERIFY(b.venusSocketDir.has_value());
        QVERIFY(*a.venusSocketDir != *b.venusSocketDir);  // two capturing profiles cannot collide
    }

    // …and a RENDER-ONLY profile keeps the shared default. That is where sharing is real
    // (--multi-clients) and a second server would cost GPU memory for nothing.
    void renderOnlyProfilesShareTheDefaultVenusSocketDir() {
        RemoraConfig rc;
        rc.gpu.mode = GpuMode::Host;
        rc.gpu.venus = true;
        rc.mirror.hostEncode = false;
        rc.backend.containerName = QStringLiteral("remora-alpha");
        const ResolvedConfig a = resolve(rc, Backend::Bare);
        QVERIFY(!a.hostEncode);
        QVERIFY(a.venusSocketDir.has_value());
        QVERIFY(a.venusSocketDir->endsWith(QStringLiteral("/remora/venus")));
        rc.backend.containerName = QStringLiteral("remora-beta");
        QCOMPARE(*resolve(rc, Backend::Bare).venusSocketDir, *a.venusSocketDir);  // shared
    }

    // An explicit venus_socket_dir still wins — the derivation is a DEFAULT, not an override.
    void explicitVenusSocketDirBeatsThePerProfileDefault() {
        RemoraConfig rc;
        rc.gpu.mode = GpuMode::Host;
        rc.gpu.venus = true;
        rc.gpu.venusSocketDir = QStringLiteral("/run/my-venus");
        rc.mirror.hostEncode = true;
        rc.backend.containerName = QStringLiteral("remora-alpha");
        QCOMPARE(*resolve(rc, Backend::Bare).venusSocketDir, QStringLiteral("/run/my-venus"));
    }

    void overlayfsDataMounts() {
        // overlayfs on → the /data bind is REPLACED by the base+diff pair the image's init
        // overlays onto /data (lowerdir=/data-base, upper/work in /data-diff)
        ResolvedConfig c = bareCfg();
        c.useOverlayfs = true;
        c.dataBaseDir = "/home/user/.remora-data-base";
        const QStringList a = buildDockerRunArgv(c);
        QVERIFY(a.contains(QStringLiteral("/home/user/.remora-data-base:/data-base")));
        QVERIFY(a.contains(QStringLiteral("/home/user/.remora-bm-data:/data-diff")));
        QVERIFY(!a.contains(QStringLiteral("/home/user/.remora-bm-data:/data")));
        // shared folders can't survive the overlay mount covering /data — omitted, not emitted dead
        c.sharedFolders = {QStringLiteral("/srv/media")};
        QVERIFY(!buildDockerRunArgv(c)
                     .contains(QStringLiteral("/srv/media:/data/media/0/media")));
        // the ssh-docker-host branch takes the same pair
        ResolvedConfig v = macvlanCfg();
        v.useOverlayfs = true;
        v.dataBaseDir = "/home/user/remora-data-base";
        const QStringList av = buildDockerRunArgv(v);
        QVERIFY(av.contains(QStringLiteral("/home/user/remora-data-base:/data-base")));
        QVERIFY(av.contains(QStringLiteral("/home/user/remora-lineage-data:/data-diff")));
        // off → the proven plain-bind vector, and never a base mount
        QVERIFY(buildDockerRunArgv(bareCfg())
                    .contains(QStringLiteral("/home/user/.remora-bm-data:/data")));
        QVERIFY(!buildDockerRunArgv(bareCfg())
                     .join(QLatin1Char(' '))
                     .contains(QLatin1String(":/data-base")));
        // resolver defaults: a static per-backend base path every profile shares
        RemoraConfig rc;
        QVERIFY(resolve(rc, Backend::Bare).dataBaseDir.endsWith("/.remora-data-base"));
        QCOMPARE(resolve(rc, Backend::Remote).dataBaseDir, QStringLiteral("~/remora-data-base"));
    }

    // bd remora-4ei.36: shared_inputs passes through resolution untouched, on BOTH docker-argv
    // backends. Names, not nodes — event numbers churn across replugs, so the deploy and its
    // watcher resolve them against the docker host's live device list at run time.
    void sharedInputsPassThroughResolve() {
        RemoraConfig c;
        c.input.sharedInputs = QStringList{QStringLiteral("Keychron K8 Pro Keyboard"),
                                           QStringLiteral("SteelSeries Rival 3")};
        QCOMPARE(resolve(c, Backend::Bare).sharedInputs, c.input.sharedInputs);
        QCOMPARE(resolve(c, Backend::Remote).sharedInputs, c.input.sharedInputs);
        QVERIFY(resolve(RemoraConfig{}, Backend::Bare).sharedInputs.isEmpty());
    }

    // bd remora-4ei.34. The identity is what Gboard gates physical-keyboard mode on, so picking
    // the wrong one silently is worse than picking none.
    void kbdIdentitySelection() {
        // Designated initialisers: this struct has gained fields twice and positional init broke
        // silently-compiling tests both times.
        const KbdIdentity keychron{.name = "Keychron K8 Pro Keyboard", .phys = "04:42:1a:5d:4a:cd",
                                   .sysfs = "/devices/virtual/misc/uhid/0005:3434:0280.000D",
                                   .node = "event22", .alphabetic = true,
                                   .bus = 0x5, .vendor = 0x3434, .product = 0x280, .version = 0x129};
        const KbdIdentity logi{.name = "Logitech USB Receiver", .phys = "usb-0000:00:14.0-9.1/input0",
                               .sysfs = "/devices/pci0000:00/0000:00:14.0/usb1/1-9/input3",
                               .node = "event3", .alphabetic = true,
                               .bus = 0x3, .vendor = 0x46d, .product = 0xc548, .version = 0x111};
        const QList<KbdIdentity> ks{keychron, logi};

        // auto = first real keyboard
        QCOMPARE(selectKbdIdentity(ks, std::nullopt)->name, keychron.name);
        QCOMPARE(selectKbdIdentity(ks, QStringLiteral(""))->name, keychron.name);
        // explicit exact name, and a case-insensitive substring for convenience
        QCOMPARE(selectKbdIdentity(ks, QStringLiteral("Logitech USB Receiver"))->name, logi.name);
        QCOMPARE(selectKbdIdentity(ks, QStringLiteral("keychron"))->name, keychron.name);
        // opting out must NOT fall through to auto
        QVERIFY(!selectKbdIdentity(ks, QStringLiteral("none")).has_value());
        QVERIFY(!selectKbdIdentity(ks, QStringLiteral("NONE")).has_value());
        // a named keyboard that is absent (e.g. a BT keyboard switched off) returns nothing rather
        // than quietly impersonating a different one
        QVERIFY(!selectKbdIdentity(ks, QStringLiteral("Das Keyboard")).has_value());
        QVERIFY(!selectKbdIdentity({}, std::nullopt).has_value());
    }

    // Sunshine's passthrough and our own persist-kbd are uinput devices that look like keyboards.
    // Auto-picking one would make the guest impersonate a device that is itself an impersonation.
    void kbdIdentitySkipsSyntheticDevicesOnAuto() {
        const KbdIdentity sunshine{.name = "Keyboard passthrough",
                                   .sysfs = "/devices/virtual/input/input25", .node = "event25",
                                   .alphabetic = true, .bus = 0x3, .vendor = 0xbeef,
                                   .product = 0xdead, .version = 0x111};
        const KbdIdentity real{.name = "Keychron K8 Pro Keyboard", .phys = "04:42:1a:5d:4a:cd",
                               .sysfs = "/devices/virtual/misc/uhid/x", .node = "event22",
                               .alphabetic = true, .bus = 0x5, .vendor = 0x3434,
                               .product = 0x280, .version = 0x129};
        // synthetic listed FIRST — source order must not decide it
        QCOMPARE(selectKbdIdentity({sunshine, real}, std::nullopt)->name, real.name);
        // nothing but synthetic devices: decline rather than self-impersonate
        QVERIFY(!selectKbdIdentity({sunshine}, std::nullopt).has_value());
        // an explicit name can still reach one, if that is genuinely what the operator wants
        QCOMPARE(selectKbdIdentity({sunshine, real}, QStringLiteral("passthrough"))->name,
                 sunshine.name);
    }

    void kbdIdentityEnvIsHexAndComplete() {
        const KbdIdentity k{.name = "Keychron K8 Pro Keyboard", .phys = "04:42:1a:5d:4a:cd",
                            .sysfs = "/devices/virtual/misc/uhid/x", .node = "event22",
                            .alphabetic = true, .bus = 0x5, .vendor = 0x3434,
                            .product = 0x280, .version = 0x129};
        QCOMPARE(kbdIdentityEnv(k),
                 (QStringList{"REMORA_KBD_NAME=Keychron K8 Pro Keyboard",
                              "REMORA_KBD_BUS=0x0005",
                              "REMORA_KBD_VENDOR=0x3434",
                              "REMORA_KBD_PRODUCT=0x0280",
                              "REMORA_KBD_VERSION=0x0129",
                              "REMORA_KBD_PHYS=04:42:1a:5d:4a:cd"}));
    }

    // bd remora-4ei.38. PIP is a SECOND mirror beside the main one, so the two things that make it
    // work at all are a distinct window class and staying off the hardware encoder.
    void pipScrcpyArgvIsADistinctSecondMirror() {
        RemoraConfig cfg;
        cfg.mirror.windowWidth = 1880;   // main-mirror geometry that PIP must NOT inherit
        cfg.mirror.windowHeight = 996;
        const ResolvedConfig rc = resolve(cfg, Backend::Remote);
        const auto [env, a] = buildPipMirrorArgv(rc, QStringLiteral("dev:5555"));

        // The unique window class: without it both windows share the binary's class and no KWin
        // or Krohnkite rule can tell them apart. A flag now — the client sets Wayland app_id and
        // X11 WM_CLASS from it.
        QVERIFY(a.contains(QStringLiteral("--app-id=remora-pip")));
        QVERIFY(env.isEmpty());

        // The rate/size caps are what keep a second session cheap — a 480x270 window needs neither
        // the mirror's 60M nor its full panel resolution.
        QVERIFY(a.contains(QStringLiteral("--video-bit-rate=6M")));
        QVERIFY(a.contains(QStringLiteral("--max-size=960")));
        QVERIFY(!a.contains(QStringLiteral("--video-bit-rate=60M")));

        // PIP geometry wins; the main window's does not leak in.
        QVERIFY(a.contains(QStringLiteral("--window-width=480")));
        QVERIFY(a.contains(QStringLiteral("--window-height=270")));
        QVERIFY(!a.contains(QStringLiteral("--window-width=1880")));
        QVERIFY(a.contains(QStringLiteral("--always-on-top")));

        // Silent by construction: the client has no audio path at all.
        for (const QString &t : a) QVERIFY(!t.contains(QLatin1String("audio")));

        // No instance ⇒ the bare title.
        QVERIFY(a.contains(QStringLiteral("--window-title=Remora PIP")));
    }

    // Every profile's app-menu folder carries its own PIP entry, so two profiles can have a PIP up
    // at once. The TITLE is what separates them (cmdPip toggles by matching it): with a shared
    // title, launching profile B's entry while A's PIP is up would close A's and open nothing.
    // The app_id must NOT be per-profile — one constant class is what lets a single
    // KWin/Krohnkite rule cover every PIP, which is the whole reason the class exists.
    void pipWindowTitleIsPerProfileButTheClassIsNot() {
        const ResolvedConfig rc = resolve(RemoraConfig{}, Backend::Remote);
        const auto [envA, a] = buildPipMirrorArgv(rc, QStringLiteral("dev:5555"),
                                                  QStringLiteral("Android 16"));
        const auto [envB, b] = buildPipMirrorArgv(rc, QStringLiteral("dev:5556"),
                                                  QStringLiteral("Android 17"));

        QVERIFY(a.contains(QStringLiteral("--window-title=Remora PIP — Android 16")));
        QVERIFY(b.contains(QStringLiteral("--window-title=Remora PIP — Android 17")));
        QVERIFY(a.contains(QStringLiteral("--app-id=remora-pip")));
        QVERIFY(b.contains(QStringLiteral("--app-id=remora-pip")));
        Q_UNUSED(envA);
        Q_UNUSED(envB);

        // The builder and the toggle must agree on the title, or the toggle silently never matches
        // and every press opens another window.
        QVERIFY(a.contains(QStringLiteral("--window-title=%1")
                               .arg(pipWindowTitle(QStringLiteral("Android 16")))));
        QCOMPARE(pipWindowTitle(QString()), QStringLiteral("Remora PIP"));
    }

    // bd remora-e5x.4. h265 resolves to c2.remora.vaapi.hevc.encoder, the image's ONLY HEVC
    // encoder (it ships no software one), so on a host with no VA encode entrypoint that component
    // cannot start and the mirror never connects — the symptom being a bare "scrcpy exited
    // immediately". A host whose only GPU is NVIDIA is exactly that: its VA driver is NVDEC,
    // decode-only. Downgrade to h264, which the image can always encode.
    void h265FallsBackToH264WithoutVaEncode() {
        // The predicate itself: NVIDIA is the case that matters, and it is NOT merely "no VA
        // driver" — NVIDIA has one, it just has no encoder.
        QVERIFY(driverHasVaEncode(QStringLiteral("xe")));
        QVERIFY(driverHasVaEncode(QStringLiteral("i915")));
        QVERIFY(driverHasVaEncode(QStringLiteral("amdgpu")));
        QVERIFY(!driverHasVaEncode(QStringLiteral("nvidia")));
        QVERIFY(!driverHasVaEncode(QStringLiteral("nouveau")));  // unknown => conservative

        RemoraConfig cfg;
        cfg.mirror.videoCodec = QStringLiteral("h265");
        cfg.gpu.mode = GpuMode::Host;

        HostCapabilities nvidiaOnly;
        nvidiaOnly.hostRenderNode = QStringLiteral("/dev/dri/renderD128");
        nvidiaOnly.hostRenderDriver = QStringLiteral("nvidia");
        const ResolvedConfig nv = resolve(cfg, Backend::Bare, nvidiaOnly);
        QCOMPARE(nv.videoCodec, QStringLiteral("h264"));
        QVERIFY(nv.videoCodecFallback.has_value());  // and it says why
        // The whole point: no VAAPI encoder is pinned, so scrcpy can actually start.
        const auto [nenv, nargv] = buildMirrorArgv(nv, QStringLiteral("dev:5555"), false);
        QVERIFY(!nargv.contains(QStringLiteral("--video-encoder=c2.remora.vaapi.hevc.encoder")));

        // An Intel host keeps h265 — the downgrade must not fire where encode exists.
        HostCapabilities intel;
        intel.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        intel.hostRenderDriver = QStringLiteral("xe");
        const ResolvedConfig ix = resolve(cfg, Backend::Bare, intel);
        QCOMPARE(ix.videoCodec, QStringLiteral("h265"));
        QVERIFY(!ix.videoCodecFallback.has_value());

        // An explicit va_driver= is the user asserting a capability the probe cannot see; trust it.
        cfg.gpu.vaDriver = QStringLiteral("nvidia");
        QCOMPARE(resolve(cfg, Backend::Bare, nvidiaOnly).videoCodec, QStringLiteral("h265"));
    }

    // An h265 profile must get an h265 PIP, hardware encoder included. This once forced h264 on the
    // belief that the hardware HEVC encoder is single-instance. It is not: measured on the live
    // A16 instance, a second VAAPI HEVC session alongside the full-res mirror initialises and runs
    // (the component is created, "vaInitialize() OK on renderD129",
    // "VaapiVideoEncoder ready: coded=960x512") — two HEVC sessions plus a software one at once.
    void pipScrcpyArgvInheritsTheSelectedCodec() {
        RemoraConfig cfg;
        cfg.mirror.videoCodec = QStringLiteral("h265");
        const ResolvedConfig rc = resolve(cfg, Backend::Remote);
        const auto [menv, m] = buildMirrorArgv(rc, QStringLiteral("dev:5555"), false);
        const auto [penv, p] = buildPipMirrorArgv(rc, QStringLiteral("dev:5555"));

        // Whatever codec/encoder the mirror was given, the PIP carries the same tokens.
        const auto video = [](const QStringList &v) {
            QStringList out;
            for (const QString &x : v)
                if (x.startsWith(QStringLiteral("--video-codec"))
                    || x.startsWith(QStringLiteral("--video-encoder")))
                    out << x;
            return out;
        };
        QVERIFY(p.contains(QStringLiteral("--video-codec=h265")));
        QCOMPARE(video(p), video(m));

        // …but still capped: inheriting the codec must not drag the mirror's bit rate in with it.
        QVERIFY(p.contains(QStringLiteral("--video-bit-rate=6M")));
        QVERIFY(p.contains(QStringLiteral("--max-size=960")));
    }
};

QTEST_MAIN(TestBuilders)
#include "test_builders.moc"
