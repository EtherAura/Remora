#include <algorithm>
#include <utility>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>

#include "TestPlatform.h"
#include "core/Builders.h"
#include "core/Config.h"
#include "core/Features.h"
#include "core/Gating.h"
#include "core/ImageBuild.h"
#include "core/ImageLineage.h"
#include "core/ManagedHost.h"
#include "core/Prereqs.h"
#include "core/Resolver.h"
#include "core/SourcePatches.h"
#include "engine/AppTransfer.h"
#include "engine/HostShell.h"
#include "engine/PlayInstall.h"
#include "engine/RedditLogin.h"
#include "engine/SelfImage.h"
#include "engine/PublicImages.h"
#include "engine/UpdateWatch.h"
#include "engine/Engine.h"
#include "engine/MirrorRegistry.h"
#include "store/Store.h"
#include "engine/SourceBuild.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#endif

using namespace remora;

// Why a case below cannot hold on Windows (SKIP_ON_WINDOWS, tests/TestPlatform.h).
static constexpr const char kBareDeploy[] = "a bare deploy, which Windows refuses at preflight";
static constexpr const char kBareHost[] =
    "bare: the fakes answer for this machine's docker, which on Windows is Remora's guest";
static constexpr const char kBareOnlyGpu[] = "host encode is bare only, and Windows has no bare";
static constexpr const char kRunsSh[] = "runs the generated shell through sh";

// The targets a deploy case runs against: both, except on Windows, which refuses bare before
// anything runs (bareIsRefusedFirstOnWindows).
static QList<Backend> deployTargets() {
#ifdef Q_OS_WIN
    return {Backend::Remote};
#else
    return {Backend::Bare, Backend::Remote};
#endif
}

// ── a configurable fake spawner ──
class FakeSpawner : public Spawner {
public:
    bool inDocker = true, ashmem = true, imagePresent = true;
    bool guestImageStale = false;  // ssh-side tag points at different content than the local tag
    bool startOk = false, runOk = true, netfixOk = true, sshOk = true, dockerOk = true;
    bool scpOk = true;  // remote host-prereqs staging (bd remora-4ei.88): a failed push must stop
    QString localPs, guestPs;
    QString dockerDown;  // non-empty: local `docker ps` fails with this on stderr (daemon down)
    // What the husk scan reports: newline-separated pids of mirrors that outlived their server.
    // Empty is the normal answer AND the no-evidence answer (no `ss` on the host) — the reaper
    // must treat both as "kill nothing" (bd remora-8d7).
    QString huskScanOut;
    // What `cat /proc/bus/input/devices` reports on the docker host, for the phantom keyboard's
    // identity pick. Empty by default — which is ALSO the real "no keyboard attached" answer, and
    // the case that used to fall back to a generic identity in silence (bd remora-4ei.9 sibling).
    QString inputDevicesOut;
    // What remora-input-share.py's synchronous expose pass prints (bd remora-4ei.36): one
    // SHARED/GRABBED/MISSING line per configured device. Empty by default — a profile that
    // shares nothing never runs the pass, and a test that does share states its outcomes.
    QString inputShareOut;
    // What the NVIDIA version scan reports (bd remora-81cp): the raw /proc NVRM line plus the
    // libnvidia-glcore ls. Empty by default = a host with no NVIDIA stack, which declines to
    // judge — so every pre-existing venus test keeps passing its health checks untouched.
    QString nvidiaVersionScanOut;
    // Exclusion is resource-based (bd remora-4u4.1): after `docker ps` names it, `docker inspect`
    // is asked what each container OCCUPIES. Default is "nothing", so a test that only sets
    // localPs/guestPs still exercises the name path exactly as before; a test that wants a port
    // or /data collision declares it here.
    QHash<QString, QString> exclPorts;  // container name -> host port, e.g. "5555"
    QHash<QString, QString> exclBinds;  // container name -> bind, e.g. "/data-dir:/data"
    QHash<QString, QString> exclDevices;  // container name -> device, e.g. "/dev/video0"
    // What the host-side /data shape probe reports: missing / empty / layered / flat. Default
    // "missing" — a host with nothing on it yet, which is what the rest of this fake models and
    // what a first deploy sees, so no existing test acquires a shape it never declared.
    QString dataDirShape = QStringLiteral("missing");
    // The arch-variant guard's two facts (bd remora-bm7.13): the remora.arch_variant label the image
    // carries, and the target host's /proc/cpuinfo flags line. BOTH empty by default, deliberately —
    // that pair describes an image built before the stamp existed on a host whose cpuinfo the probe
    // could not read, which is precisely the combination the guard must wave through, so no
    // pre-existing deploy test acquires a CPU or an ISA requirement it never declared.
    QString imageArchVariant;
    QString hostCpuFlags;
    // What the image-lineage probe reports (bd remora-3czv): the tags of the image an unregistered
    // pin's labels name, one per line, and no layers. Empty by default — an image naming no root,
    // which is what every image a test pins without saying otherwise has always been treated as.
    QString imageRootTags;
    // What the play_spoof content-drift gate reports (bd remora-4ei.86). false = /data already
    // matches the image, the answer of a device that needs no re-install and the default every
    // pre-existing test was written against.
    bool spoofModulesDrifted = false;
    // The Play-certification scan (id / verdict / token) and the identity-restore script's answer.
    // Default empty scan = a device that answers nothing, which is UNKNOWN, not uncertified.
    QString checkinScanOut;
    QString checkinRestoreOut = QStringLiteral("OK\n");
    QStringList bootStates{"1"};
    // A container that stopped while booting: what `docker inspect` reports for its state and
    // exit code ("exited 129"). Empty = it is running, and `docker exec` reaches it.
    QString containerExit;
    QStringList adbStates{"device"};
    bool detachedDead = false;   // legacy: every launched detached process reports dead
    int detachedDiesFirst = 0;   // the first N spawnDetached()s report dead, then alive (retry sim)
    int detachedDiesOnCall = 0;  // detachedAlive returns dead on exactly this Nth call (alive-then-
                                 // drop sim: an adopted splash that passes liveness then dies at hand-off)
    bool containerRunning = true;   // connect's pre-adb "is the container even up?" probe
    bool containerPaused = false;   // remora sleep state: Running=true + Paused=true
    bool containerStale = false;    // smart connect: container image != what the tag points to
    // What the Play installer answers (bd remora-6laf.8): its stdout, receipt line included, and
    // its exit status. Asked only when a profile turns google_play on.
    QString playInstallOut;
    int playInstallRc = 0;
    bool containerHasC2 = true;     // smart connect: container was booted with the c2 boot arg
    // The canned container's fps boot arg. Defaults to 60 because the RESOLVER defaults fps to
    // 60 (a17-30hz fix) — a current container carries it, so the whole-set compare
    // (bd remora-wlun) reads the default canned container as current. nullopt or another value
    // simulates fps drift.
    std::optional<int> containerFps = 60;
    // Extra androidboot.* lines the canned container carries beyond what the profile under test
    // resolves — the wlun drift shape (an arg the profile dropped, still baked in the container).
    QString containerExtraBootArgs;
    // What the smart-connect probe says the container was CREATED with. It must match the
    // resolution of the backend under test or every reconnect case redeploys, because the resolver
    // deliberately gives each backend different defaults — vm resolves gpu_mode=host, bare and
    // remote resolve guest (bd remora-d5v). Defaults to the vm value so pre-existing tests are
    // untouched; a test on another backend says so.
    QString probeGpuMode = QStringLiteral("host");
    QString containerBinds;         // smart connect: the container's /data/media/0 sub-binds
    // What the smart-connect probe lists as the container's --device passthroughs
    // (.HostConfig.Devices PathOnHost, one per line). The default is a plain input node so the
    // device check has something benign to tolerate; a camera test states its video nodes
    // (bd remora-usn0).
    QString containerDevices = QStringLiteral("/dev/input/event5");
    // waitFirstBootSettled's in-container probe, one answer per poll: "busy" while a module's
    // service.sh is still doing its late work, "idle" once it is done. Default idle — a test that
    // says nothing describes a device with no first-boot work pending, which is every warm path.
    QStringList firstBootStates{"idle"};
    // play-spoof's root-stack gate (bd remora-82c.13). Empty = healthy (the probe answers OK); a
    // test staging the upstream-Magisk failure sets the gap token the probe would print
    // (NO_SBIN / NO_DAEMON / NO_SU).
    QString rootProbeGap;
    bool bootManagedZygisk = false;
    bool nativeZygiskReady = true;
    QString macvlanIp = QStringLiteral("192.168.0.90");  // "" = discovery reads nothing back
    bool macvlanNetPresent = true;   // `docker network inspect <name>` succeeds
    // What that inspect prints for the network's shape (kMacvlanNetworkShapeFormat). Empty by
    // default, which reads as "docker reported nothing usable": the engine then keeps the
    // resolved shape, so every test written before bd remora-8pts sees what it always saw.
    QString macvlanNetShape;
    bool macvlanCreateOk = true;     // `docker network create …` succeeds
    QStringList netCreates;          // every create command the engine issued, in order
    // What the EXISTING container's network attachment inspects as ("<net>=<ip> " records). Empty
    // by default — the same thing a real missing container reports, and the value the staleness
    // rule treats as "nothing to judge". A test that cares about networking states it.
    QString containerNetworks;
    bool shimPresent = false;      // `ip link show remora-shim0` succeeds
    bool shimIpAnswers = false;    // something on the LAN already owns the derived shim address
    bool shimConfigureOk = true;   // the privileged host-netns container succeeds
    bool shimUnitInstalled = false;  // a boot-time unit is also managing the shim
    QStringList shimRuns;          // every shim-configuring command issued, in order
    bool adbTool = true;                     // preflight's local-tools check
    bool screencapOk = true;                 // captureScreenshot's exec-out redirect
    bool litProbeTimesOut = false;           // waitLit gate: screencap wedges (rc 124 from timeout)
    // The client this fake pretends to be (bd remora-7oih.2). Unix by default, so every test
    // written before the Windows client describes the Linux one it was written against.
    LocalOs os = LocalOs::Unix;
    // runTimed's deadline per call, in order: what the engine BOUNDED, as opposed to what it ran.
    QList<int> timedCallsMs;
    // readTail's answers by path, for a log the test stages (the mirror's own detached log).
    QHash<QString, QString> fileTails;
    // When set, spawnDetached hands this back as the child's own log, so a test can stage what a
    // spawned mirror "said" through fileTails.
    QString detachedLog;
    // Venus render-server health, as the two probes see it. Defaults describe a healthy stack.
    bool ashmemNodeMissing = false;  // the in-container ashmem probe reports NO_ASHMEM
    bool venusProcAlive = true;    // `ps -C virgl_test_server` scoped to this profile's socket path
    bool venusSocketPresent = true;  // `[ -S <dir>/venus.sock ]`
    // Consumers the host encoder is serving, as the `ss` probe reports them. -1 = no `ss` on this
    // host, which must read as UNKNOWN and never as a starved mirror (bd remora-e5x.14).
    int servedConsumers = 1;
    // …but not instantly. The first N probes answer 0, then servedConsumers takes over: an adopted
    // splash is NOT a served consumer the moment it is told to attach — it still has to notice the
    // marker, finish its outro, push the server and open the video socket (bd remora-82x).
    int servedZeroFirst = 0;
    // What the mirror registry reports (Spawner::mirrors, bd remora-7oih.1), one snapshot per
    // query with the last one sticky: doConnect asks for husks, then once up front (the
    // predecessors it must spare), again per attempt, and again when reaping a rejected attempt
    // (which by then includes that attempt's own process). Empty by default — no mirror open.
    QVector<QVector<MirrorEntry>> mirrorSnapshots;
    int mirrorQueries = 0;
    // Every stop the engine asked for, in order (also logged into `calls` as "STOP <pids>", so
    // a test can order a stop against the spawns around it), and the pids that will not go.
    struct Stop {
        QList<qint64> pids;
        int graceMs = 0;
        bool force = false;
    };
    QVector<Stop> stops;
    QList<qint64> unstoppable;
    // Has this /data carried a boot through to a working mirror before? false = fresh, which is the
    // only situation the boot-stability hold defends, so the default keeps the hold in play.
    bool dataBootedBefore = false;
    // Does the kernel offer the LEGACY netfilter tables Android's netd needs? false = the nft-only
    // default of a modern host, which is what makes netd retry for seconds (bd remora-u9m).
    bool legacyNetfilterTables = false;
    bool hostEncoderProcAlive = true;  // `ps -C remora-frame-encoder` matched on the frame socket
    // The serve gate's three probes (bd remora-7ps). The log path resolves by default so the
    // gate runs its data-level fast path; the counts default to "0 at baseline, 1 at the gate" —
    // a healthy encoder that served the new consumer before the gate even asked, which is what
    // the live timings show (~1.2 s serve, gate asks after liveness + hold). Values shift off
    // the list with the last one sticky.
    QString hostEncoderLogPath = QStringLiteral("/tmp/remora-detached-77-1.log");
    QStringList firstPacketCounts{QStringLiteral("0"), QStringLiteral("1")};
    QString hostEncoderLogTail;  // what `tail -n 12 <encoder log>` answers on the evidence read
    QString deadEncoderLog;      // the dead-encoder note's lookup: "<path>\n<last line>" or empty
    // What the encoder's published status file holds (bd remora-e5x.18.3). Empty = no file, the
    // answer of a host whose encoder predates status or is not running — never a note.
    QString encoderStatusOut;
    // How many mCreateDevice assertion lines the guest's logcat holds (bd remora-pobz item 2).
    // The storm is what separates a lost RenderEngine from a legitimately dark screen, so the
    // default is the healthy answer: none.
    int guestCreateDeviceAsserts = 0;
    // What the teardown reap's encoder scan reports: newline-separated pids of encoders on the
    // torn-down container's frame socket (bd remora-e5x.19.1). Distinct from the yes/no probe
    // above — this one extracts pids.
    QString orphanEncoderPids;
    // probeMirrorFlow / probeEncoderRate: the endpoint scan's labelled output, then one answer per
    // `tail` of a counter log, popped in order so a two-sample rate can be staged.
    QString mirrorEndpoints;
    QStringList counterReads;
    // app-transfer (engine/AppTransfer.cpp): canned pm output + pull/install switches
    QString pmPackages;              // `pm list packages -3` payload
    QMap<QString, QString> pmPaths;  // per-package `pm path` payload
    bool pullOk = true, installOk = true;
    // the data carry's rooted halves (bd remora-4ei.73): the source tar pack, adb push, and the
    // target's stat/untar/chown/restorecon script, each failable on its own
    bool dataPackOk = true, pushOk = true, dataRestoreOk = true;
    QString renderNodes;  // all-nodes GPU scan payload, "node driver" lines (bd remora-4ei.79)
    QString glesDump;     // `dumpsys SurfaceFlinger` payload for the renderer probe
    // The adb front-door probe's payload (bd remora-yn4). Default: healthy, init-owned — the
    // state every deployed instance settles into, so unrelated status tests stay quiet.
    QString forwarderProbe =
        QStringLiteral("fwd_l=1\nadbd_l=1\nsvc=running\n");
    int spawnCount = 0;
    int waitedMs = 0;  // what the engine ASKED to wait for; nothing here ever actually sleeps
    QStringList calls;

    ProcResult run(const QStringList &argv, const QMap<QString, QString> &,
                   const LineSink &) override {
        calls << argv.join(QLatin1Char(' '));
        const QString cmd = argv.join(QLatin1Char(' '));
        auto R = [](int rc, QString out = {}, QString err = {}) { return ProcResult{rc, out, err}; };

        if (argv.value(0) == "id") return R(0, groupsStr());
        // The boot wait's "is it still there?" (bd remora-7oih), first: its format is unique.
        if (cmd.contains("State.Status")) return R(0, containerExit.isEmpty() ? "running 0\n"
                                                                              : containerExit + "\n");
        if (!containerExit.isEmpty() && argv.mid(0, 2) == QStringList{"docker", "exec"})
            return R(1, {}, "Error response from daemon: container is not running");
        if (argv.value(0) == "python3" && cmd.contains("install.py"))
            return R(playInstallRc, playInstallOut,
                     playInstallRc ? QStringLiteral("Play installation failed: boom") : QString());
        // Ahead of the generic rules: the husk scan is a plain `sh -c` and would otherwise be
        // swallowed by whichever broader pattern matched first. The endpoint scan names `ss -tnHp`
        // too — it picks the mirror by whether it has a tunnel — so it has to be matched on its
        // own distinctive text FIRST, or it gets answered as a husk scan and finds no endpoints.
        if (cmd.contains("/proc/$e/cgroup")) return R(0, mirrorEndpoints);
        if (cmd.contains("ss -tnHp")) return R(0, huskScanOut);
        if (cmd.contains("command -v adb")) return R(adbTool ? 0 : 1);
        if (argv == QStringList{"where", "/q", "adb"}) return R(adbTool ? 0 : 1);
        if (cmd.contains("lsmod | grep -q ashmem")) return R(ashmem ? 0 : 1);
        // The /data shape probe (bd remora-4ei.89). Matched on its discriminator so one rule
        // answers both backends — bare runs it as a local `sh -c`, remote as an ssh payload.
        if (cmd.contains("-d $D/upper")) return R(0, dataDirShape);
        // The arch-variant probe (bd remora-bm7.13), matched on the label key for the same reason:
        // one rule answers bare (a local `sh -c`) and remote (the same script as an ssh payload),
        // and it must sit AHEAD of the generic `docker image inspect` rules below, which would
        // otherwise answer it with "present" and leave both halves unparsed.
        if (cmd.contains("org.remora.addons.root org.remora.play.base") && cmd.contains("RepoTags"))
            return R(0, QStringLiteral("ROOT\n") + imageRootTags + QStringLiteral("LAYERS\n"));
        if (cmd.contains("remora.arch_variant")) {
            QString out;
            if (!imageArchVariant.isEmpty())
                out += QStringLiteral("arch_variant=%1\n").arg(imageArchVariant);
            if (!hostCpuFlags.isEmpty()) out += hostCpuFlags + QLatin1Char('\n');
            return R(0, out);
        }
        // The encoder's status file (bd remora-e5x.18.3): `cat <frame socket>.status`. An empty
        // payload reads as the real thing — cat of a missing file, rc 1 and no output. Matched on
        // the cat as well as the path, because the reap's `rm` names the same file.
        if (cmd.contains("cat ") && cmd.contains(".sock.status"))
            return encoderStatusOut.isEmpty() ? R(1) : R(0, encoderStatusOut);
        // The guest-health probe behind the black-stream heal (bd remora-pobz item 2).
        if (cmd.contains("mCreateDevice"))
            return R(0, QString::number(guestCreateDeviceAsserts) + "\n");
        // The checkin identity pair (bd remora-4ei.84). Restore first: both scripts name the
        // misc_ce path, and only the restore force-stops GMS.
        if (cmd.contains("am force-stop com.google.android.gms")) return R(0, checkinRestoreOut);
        if (cmd.contains("misc_ce/0/checkin/checkin_id_token")) return R(0, checkinScanOut);
        // Matched on the joined argv so ONE rule answers both backends: bare sends it as a
        // `docker exec` argv, vm as the tail of an ssh payload. Ahead of the ssh dispatch below
        // for that reason. Matched on the PATTERN alone, not on the surrounding `pgrep -f '…'`:
        // sshArgv single-quotes the whole payload, so the quotes around it come back as '\'' —
        // the escaped pattern itself is the one part that reads identically on both paths.
        if (cmd.contains("/sys/module/$m"))
            return R(0, legacyNetfilterTables ? QString() : QStringLiteral("ip6table_mangle "));
        if (cmd.contains(".remora-boot-stable"))
            return cmd.contains("touch") ? R(0) : R(dataBootedBefore ? 0 : 1);
        if (cmd.contains("service\\.sh")) return R(0, pop(firstBootStates, "idle"));
        if (cmd.contains("cat /system/etc/remora-spoof/zygisk-runtime"))
            return R(0, bootManagedZygisk ? QStringLiteral("magisk\n") : QString());
        if (cmd.contains("pgrep -x zygiskd64")) return R(nativeZygiskReady ? 0 : 1);
        // play-spoof's root-stack gate (bd remora-82c.13). Must answer explicitly: the fake's
        // default empty-output R(0) would read as a gap and fail every deploy test.
        if (cmd.contains("pgrep -x magiskd"))
            return rootProbeGap.isEmpty() ? R(0, QStringLiteral("OK\n"))
                                          : R(1, rootProbeGap + QStringLiteral("\n"));
        // The hand-off generation probe. Ahead of the generic `docker exec … getprop` rule below,
        // which would otherwise answer it out of bootStates and quietly consume a boot state that
        // a sequencing test was counting on.
        if (cmd.contains("system_serve[r]")) return R(0, "sys=568 srv=1 boot=1\n");
        if (cmd.contains("docker version") && !dockerDown.isEmpty()) return R(1, QString(), dockerDown);
        if (argv.mid(0, 2) == QStringList{"docker", "ps"})
            return dockerDown.isEmpty() ? R(0, localPs) : R(1, QString(), dockerDown);
        // The exclusion facts probe. Matched on argv[0..1] as well as the format field: an ssh
        // argv carries the whole remote command as its last element, so a bare cmd.contains()
        // here would also swallow the GUEST probe and answer it with LOCAL facts.
        if (argv.mid(0, 2) == QStringList{"docker", "inspect"} &&
            cmd.contains("HostConfig.PortBindings"))
            return R(0, inspectFacts(localPs));
        // provision's stale-image check asks these two SEPARATELY (the combined probe below is the
        // smart-connect one). containerStale drives whether they disagree.
        if (argv.mid(0, 3) == QStringList{"docker", "image", "inspect"} && cmd.contains("{{.Id}}"))
            return imagePresent ? R(0, "sha256:img") : R(1, {}, "No such image");
        if (argv.mid(0, 2) == QStringList{"docker", "inspect"} && cmd.contains("{{.Image}}") &&
            !cmd.contains("State.Running"))
            return R(0, containerStale ? "sha256:new" : "sha256:img");
        if (argv.mid(0, 3) == QStringList{"docker", "image", "inspect"})
            return imagePresent ? R(0, "present") : R(1, {}, "No such image");
        if (argv.value(0) == "mkdir") return R(0);
        if (argv.value(0) == "scp") return scpOk ? R(0) : R(1, {}, "scp: connection closed");
        if (argv.mid(0, 2) == QStringList{"docker", "start"})
            return startOk ? R(0) : R(1, {}, "No such container");
        // macvlan host shim (bd remora-400) — the ONE docker run that shares the host's network
        // namespace. Ahead of the generic docker-run rule, which would otherwise answer it as if
        // it were the Android container and leave the shim untested.
        if (argv.mid(0, 2) == QStringList{"docker", "run"} && cmd.contains("--network host")) {
            shimRuns << cmd;
            if (shimConfigureOk) shimPresent = true;
            return shimConfigureOk ? R(0) : R(1, {}, "RTNETLINK answers: File exists");
        }
        if (argv.mid(0, 2) == QStringList{"docker", "run"}) {
            // A successful run leaves the container RUNNING — the ssh dispatch already modelled
            // this and the local one did not, so a bare deploy provisioned and then failed its own
            // connect with "container is not running", which no real deploy does.
            if (runOk) containerRunning = true;
            return runOk ? R(0, "cid") : R(1, {}, "docker run failed: ashmem?");
        }
        // The front-door probe (bd remora-yn4), AHEAD of the generic getprop rule: its script
        // ends in `getprop init.svc.remora_fwd`, which that rule would swallow, answering the
        // port scan with a boot state.
        if (cmd.contains("fwd_l=")) return R(0, forwarderProbe);
        if (argv.mid(0, 2) == QStringList{"docker", "exec"} && cmd.contains("getprop"))
            return R(0, pop(bootStates, "0"));
        if (argv.mid(0, 2) == QStringList{"docker", "cp"}) return R(0);
        if (argv.mid(0, 2) == QStringList{"docker", "exec"} && cmd.contains("netfix.sh"))
            return netfixOk ? R(0, "up") : R(1, {}, "netfix boom");
        // The play_spoof drift gate (bd remora-4ei.86). Matched on the module map it walks, which no
        // other call carries, and answered BEFORE the generic docker-exec rules — the script mentions
        // module.prop and bin/, either of which a broader rule could claim.
        if (cmd.contains("remora-spoof/*/"))
            return spoofModulesDrifted ? R(1) : R(0);
        if (cmd.contains("State.Running") && cmd.contains("{{.Image}}"))  // smart-connect probe
            // Section order mirrors the real probe (c2= from .Args, then binds=, then devs=, then
            // nets=): the devs parser slices between "devs=" and "nets=", so a fake that reordered
            // the sections would hand it bind strings and the test would lie (bd remora-usn0).
            // Matched on the joined argv, so this ONE rule answers both backends: a remote probe
            // arrives as an ssh argv whose payload carries the same format string.
            return containerRunning
                       ? R(0, QStringLiteral("true sha256:img c2=androidboot.remora_gpu_mode=%5\n"
                                             "androidboot.use_memfd=true\n"
                                             "androidboot.remora_width=3760\n"
                                             "androidboot.remora_height=1992\n"
                                             "androidboot.remora_dpi=320\n"
                                             "androidboot.use_remora_overlayfs=1\n"
                                             "%1%7%8"
                                             "binds=%2\n"
                                           "devs=%6\n"
                                             "nets=%4\n%3")
                                  .arg(containerHasC2
                                           ? QStringLiteral("androidboot.use_remora_c2=1\n")
                                           : QString(),
                                       containerBinds,
                                       containerStale ? "sha256:new" : "sha256:img",
                                       containerNetworks, probeGpuMode, containerDevices)
                                  .arg(containerFps ? QStringLiteral("androidboot.remora_fps=%1\n")
                                                          .arg(*containerFps)
                                                    : QString(),
                                       containerExtraBootArgs.isEmpty()
                                           ? QString()
                                           : containerExtraBootArgs + QStringLiteral("\n")))
                       : R(1, {}, "No such container");
        // provision's updater-bind inspect. Matched on its format field, AFTER the smart-connect
        // probe, whose format also ranges over HostConfig.Binds.
        if (argv.mid(0, 2) == QStringList{"docker", "inspect"} &&
            cmd.contains("{{range .HostConfig.Binds}}"))
            return containerRunning ? R(0, containerBinds) : R(1, {}, "No such container");
        // provision's device-staleness inspect (bd remora-usn0). AFTER the smart-connect probe:
        // that one's format also names HostConfig.Devices, so this rule matching first would
        // swallow it and answer with the bare device list.
        if (argv.mid(0, 2) == QStringList{"docker", "inspect"} &&
            cmd.contains("HostConfig.Devices"))
            return containerRunning ? R(0, containerDevices) : R(1, {}, "No such container");
        // TWO different probes wear this field. connect/sleep ask for "{{.State.Running}}
        // {{.State.Paused}}" and read both; provision asks for "{{.State.Running}}" alone and
        // compares the whole trimmed output against "true". Answering the two-field string to both
        // made provision read every running container as NOT running, so it appended REMORA_BOOTING
        // for a boot that never happened — a real deploy gets "true" and does not.
        if (argv.mid(0, 2) == QStringList{"docker", "inspect"} && cmd.contains("State.Running")) {
            if (!containerRunning) return R(1, {}, "No such container");
            if (!cmd.contains("State.Paused")) return R(0, "true");
            return R(0, containerPaused ? "true true" : "true false");
        }
        if (argv.mid(0, 2) == QStringList{"docker", "unpause"}) {
            containerPaused = false;
            return R(0);
        }
        if (argv.mid(0, 2) == QStringList{"docker", "pause"}) {
            containerPaused = true;
            return R(0);
        }
        // The macvlan network ensure (bd remora-400). Both halves arrive as a shell payload — the
        // helper runs them locally on bare and over ssh elsewhere — so match the command text
        // rather than the argv shape, and record creates so a test can assert the exact argv.
        // NO_LAN_NET excludes the vm's guest-config audit, which embeds this same inspect in a
        // compound command: answering that here would swallow the whole audit and report a guest
        // with nothing missing.
        if (cmd.contains("docker network inspect") && !cmd.contains("NO_LAN_NET"))
            return macvlanNetPresent ? R(0, macvlanNetShape) : R(1, {}, "Error: No such network");
        if (cmd.contains("ip link show remora-shim0")) return R(shimPresent ? 0 : 1);
        if (cmd.startsWith(QLatin1String("sh -c ping"))) return R(shimIpAnswers ? 0 : 1);
        if (cmd.contains("remora-macvlan-shim.service")) return R(shimUnitInstalled ? 0 : 1);
        // provision's network-staleness probe. Ahead of the generic container-inspect rules, which
        // key off .State.Running / .Image and would otherwise answer this with the wrong field.
        if (cmd.contains("NetworkSettings.Networks") && cmd.contains("$k"))
            return containerRunning ? R(0, containerNetworks) : R(1, {}, "No such container");
        // macvlan auto-IP discovery at connect. The ssh() dispatch below answers the vm form of
        // this; bare runs the same probe LOCALLY (`sh -c docker inspect …`), which reaches here.
        if (cmd.contains("NetworkSettings.Networks"))
            return R(0, macvlanIp.isEmpty() ? QString() : macvlanIp + "\n");
        if (cmd.contains("docker network create")) {
            netCreates << cmd.section(QLatin1String("docker network create"), 1).trimmed();
            return macvlanCreateOk ? R(0, "netid") : R(1, {}, "parent interface not found");
        }
        if (argv.value(0) == "sh" && cmd.contains("echo \"$r $drv")) return R(0, renderNodes);
        if (cmd.contains("dumpsys SurfaceFlinger")) return R(0, glesDump);
        if (argv.mid(0, 2) == QStringList{"adb", "connect"}) return R(0);
        if (argv.value(0) == "adb" && cmd.contains("pm list packages -3")) return R(0, pmPackages);
        if (argv.value(0) == "adb" && cmd.contains("pm path "))
            return R(0, pmPaths.value(cmd.section(QLatin1String("pm path "), 1).trimmed()));
        if (argv.value(0) == "adb" && argv.value(3) == "pull") return R(pullOk ? 0 : 1);
        if (argv.value(0) == "adb" && argv.value(3) == "push") return R(pushOk ? 0 : 1);
        // The data carry's rooted halves (bd remora-4ei.73), matched on the staged tar's name so
        // no other script is swallowed. Pack and restore are told apart by tar's -cf/-xf. Not
        // pinned to a transport: these ride `docker exec` for a profile and `adb shell su` for a
        // raw host:port, and the pipeline's outcome is the same either way — WHICH route each
        // call took is asserted by the pipeline test, not decided here.
        if (cmd.contains("remora-appdata-")) {
            if (cmd.contains("tar -C /data/data -cf"))
                return dataPackOk ? R(0) : R(1, {}, "tar: pack failed");
            if (cmd.contains("tar -C /data/data -xf"))
                return dataRestoreOk ? R(0) : R(1, {}, "restorecon: failed");
            return R(0);  // the rm -f cleanups
        }
        if (argv.value(0) == "adb" && argv.contains(QStringLiteral("install-multiple")))
            return installOk ? R(0) : R(1, {}, "INSTALL_FAILED_TEST");
        if (argv.value(0) == "adb" && cmd.contains("get-state")) return R(0, pop(adbStates, "device"));
        if (argv.value(0) == "adb" && cmd.contains("getprop")) return R(0, pop(bootStates, "1"));
        // waitLit gate: `adb … screencap`, bounded by runTimed (bd remora-7oih.2).
        // litProbeTimesOut reproduces a wedged screencap: rc 124 is what the deadline returns when
        // it kills the call, and the connect must proceed anyway rather than block.
        if (argv.value(0) == "adb" && cmd.contains("screencap") && !cmd.contains("exec-out"))
            return litProbeTimesOut ? R(124) : R(0, "500000\n");
        // The phantom keyboard's identity probe. Answered for BOTH backends: remote runs it
        // through sshArgv, local through sh -c, and both land in this dispatcher.
        // No phantom keyboard yet, so the step actually RUNS rather than short-circuiting on
        // "already running" — which is what kept the identity pick untested.
        if (cmd.contains("pidof uinput-kbd")) return R(1);
        if (cmd.contains("/proc/bus/input/devices")) return R(0, inputDevicesOut);
        // The shared-inputs expose pass. Matched ahead of the ssh dispatch below so ONE rule
        // answers both backends — bare runs python3 directly, remote sends the same command as
        // an ssh payload (bd remora-4ei.36). The watch spawn and the pkill fall through to the
        // default R(0), which is exactly what both return when they have nothing to do.
        if (cmd.contains("remora-input-share.py") && cmd.contains("expose"))
            return R(inputShareOut.contains(QLatin1String("SHARED")) ? 0 : 1, inputShareOut);
        if (cmd.contains("/proc/driver/nvidia/version")) return R(0, nvidiaVersionScanOut);
        if (cmd.contains("boot_id") && cmd.contains("/dev/ashmem"))
            return R(0, ashmemNodeMissing ? "NO_ASHMEM\n" : "LINKED\n");
        if (argv.value(0) == "sh" && cmd.contains("ps -C virgl_test_server"))
            return venusProcAlive ? R(0) : R(1);
        // Order matters: this must precede the generic remora-frame-encoder match below, because
        // the served-consumer probe also names the encoder's sockets.
        if (argv.value(0) == "sh" && cmd.contains("grep -c ESTAB")) {
            if (servedZeroFirst > 0) {
                --servedZeroFirst;
                return R(0, "0\n");
            }
            return R(0, QString::number(servedConsumers) + "\n");
        }
        // The serve gate's log resolution embeds the encoder ps AND an awk stage, so it must be
        // matched on its own discriminator — the readlink — before both encoder rules below
        // (bd remora-7ps). Empty path = the resolution failed, the gate's degraded mode.
        if (argv.value(0) == "sh" && cmd.contains("readlink /proc"))
            return hostEncoderLogPath.isEmpty() ? R(1) : R(0, hostEncoderLogPath + "\n");
        // The serve gate's data-level probe: how many consumers has this encoder ever served
        // its first decodable packet (bd remora-7ps). First call is doConnect's once-per-connect
        // baseline; later calls are the gate polling. Values shift off the list, last one
        // sticky, so a test can script "0 at baseline, still 0 at the gate" (never served) or
        // leave the default {0,1} healthy fast-path.
        if (argv.value(0) == "sh" && cmd.contains("first decodable packet")) {
            if (firstPacketCounts.size() > 1) return R(0, firstPacketCounts.takeFirst() + "\n");
            return R(0, firstPacketCounts.value(0, QStringLiteral("1")) + "\n");
        }
        // The serve gate's evidence tail — matched apart from the mirror's said-tail (n 8) and the
        // flow-probe counters (n 400).
        if (argv.value(0) == "sh" && cmd.contains("tail -n 12"))
            return R(0, hostEncoderLogTail);
        // A DEAD encoder's log, found by its opening line: "<path>\n<last line>", or nothing.
        if (argv.value(0) == "sh" && cmd.contains("listening for the render server"))
            return R(0, deadEncoderLog);
        // The teardown reap extracts encoder PIDs (its awk stage) rather than asking yes/no, so
        // it must be matched before the generic encoder-liveness rule below (bd remora-e5x.19.1).
        if (argv.value(0) == "sh" && cmd.contains("ps -C remora-frame-encoder") &&
            cmd.contains("awk"))
            return R(0, orphanEncoderPids);
        if (argv.value(0) == "sh" && cmd.contains("ps -C remora-frame-encoder"))
            return hostEncoderProcAlive ? R(0) : R(1);
        // Once the server has been spawned its socket appears, so the post-spawn wait terminates.
        if (argv.value(0) == "sh" && cmd.contains("venus.sock"))
            return (venusSocketPresent || spawnCount > 0) ? R(0) : R(1);
        if (argv.mid(0, 2) == QStringList{"adb", "disconnect"}) return R(0);
        if (argv.value(0) == "ssh") {
            // sshArgv now sends `/bin/sh -c '<payload>'` so a non-POSIX login shell cannot swallow
            // it (bd remora-4ei.68). Recover the payload the way the remote shell would, so these
            // expectations keep describing the command that actually runs rather than its quoting.
            return ssh(unquoteSshPayload(argv.last()));
        }
        return R(0);
    }
    LocalOs localOs() const override { return os; }
    ProcResult runTimed(const QStringList &argv, int timeoutMs, const LineSink &onLine) override {
        timedCallsMs << timeoutMs;
        return run(argv, {}, onLine);
    }
    ProcResult runToFile(const QStringList &argv, const QString &outPath) override {
        calls << argv.join(QLatin1Char(' ')) + " > " + outPath;
        if (argv.contains(QStringLiteral("screencap"))) {
            if (!screencapOk) return ProcResult{1, {}, "adb: device offline"};
            QFile f(outPath);  // a stub PNG, so the size check passes
            if (f.open(QIODevice::WriteOnly)) f.write("PNG-STUB-BYTES");
        }
        return ProcResult{0, {}, {}};
    }
    // The flow probe's counter logs are read 400 lines at a time; one answer per read, popped in
    // order so a two-sample rate can be staged. Anything else answers from fileTails.
    QString readTail(const QString &path, int lines) override {
        calls << QStringLiteral("TAIL %1 %2").arg(lines).arg(path);
        if (lines == 400) return counterReads.isEmpty() ? QString() : counterReads.takeFirst();
        return fileTails.value(path);
    }
    Detached spawnDetached(const QStringList &argv, const QMap<QString, QString> &env) override {
        calls << "DETACH " + argv.join(QLatin1Char(' '));
        // Some of what a detached child is told lives ONLY in its environment — the render
        // server's frame socket and display pin are read once at startup and appear in no argv.
        detachedEnv = env;
        return Detached{4242, detachedLog};
    }
    QMap<QString, QString> detachedEnv;  // env of the most recent spawnDetached
    bool detachedAlive(const Detached &) override {
        if (detachedDead) return false;
        ++spawnCount;
        if (detachedDiesOnCall > 0 && spawnCount == detachedDiesOnCall)
            return false;  // was alive on earlier checks, drops on this one
        return spawnCount > detachedDiesFirst;  // first detachedDiesFirst liveness checks fail
    }
    // The engine paces itself against a real device; a fake device answers instantly, so a test
    // that also slept would only be sleeping. Record the intent and return — this alone took the
    // suite from ~371s to well under a second (bd remora-eoy.9).
    void waitMs(int ms) override { waitedMs += ms; }
    QVector<MirrorEntry> mirrors() override {
        ++mirrorQueries;
        if (mirrorSnapshots.size() > 1) return mirrorSnapshots.takeFirst();
        return mirrorSnapshots.isEmpty() ? QVector<MirrorEntry>() : mirrorSnapshots.first();
    }
    QList<qint64> terminate(const QList<qint64> &pids, int graceMs, bool force) override {
        stops.push_back({pids, graceMs, force});
        QStringList ps;
        for (qint64 p : pids) ps << QString::number(p);
        calls << QStringLiteral("STOP %1%2").arg(ps.join(QLatin1Char(' ')),
                                                 force ? QStringLiteral(" force") : QString());
        QList<qint64> left;
        for (qint64 p : pids)
            if (unstoppable.contains(p)) left << p;
        return left;
    }

private:
    QString groupsStr() const {
        QStringList g{"user"};
        if (inDocker) g << "docker";
        return g.join(QLatin1Char(' '));
    }
    // Render what real `docker inspect --format` emits for the exclusion probe: one
    // "/name<TAB>ports|<TAB>binds|" record per running container. The leading slash is
    // deliberate — docker really does emit .Name that way, and the parser has to strip it.
    QString inspectFacts(const QString &psOut) const {
        QString out;
        for (const QString &ln : psOut.split(QLatin1Char('\n'))) {
            const QString name = ln.trimmed();
            if (name.isEmpty()) continue;
            const QString ports = exclPorts.value(name);
            const QString binds = exclBinds.value(name);
            const QString devs = exclDevices.value(name);
            out += QStringLiteral("/%1\t%2\t%3\t%4\n")
                       .arg(name, ports.isEmpty() ? QString() : ports + QLatin1Char('|'),
                            binds.isEmpty() ? QString() : binds + QLatin1Char('|'),
                            devs.isEmpty() ? QString() : devs + QLatin1Char('|'));
        }
        return out;
    }
    // Inverse of Spawner's shSingleQuote: strip the wrapping quotes and undo the '\'' escaping.
    static QString unquoteSshPayload(QString s) {
        if (s.size() >= 2 && s.startsWith(QLatin1Char('\'')) && s.endsWith(QLatin1Char('\''))) {
            s = s.mid(1, s.size() - 2);
            s.replace(QLatin1String("'\\''"), QLatin1String("'"));
        }
        return s;
    }
    ProcResult ssh(const QString &remote) {
        auto R = [](int rc, QString out = {}, QString err = {}) { return ProcResult{rc, out, err}; };
        if (remote == "echo ok") return sshOk ? R(0, "ok\n") : R(1);
        if (remote.contains("State.Status"))
            return R(0, containerExit.isEmpty() ? "running 0\n" : containerExit + "\n");
        if (!containerExit.isEmpty() && remote.contains("docker exec"))
            return R(1, {}, "Error response from daemon: container is not running");
        if (remote.contains("docker info")) return dockerOk ? R(0, "ok\n") : R(0, "");
        if (remote.contains("HostConfig.PortBindings")) return R(0, inspectFacts(guestPs));
        if (remote.contains("docker ps")) return R(0, guestPs);
        if (remote.contains("docker image inspect"))
            return imagePresent ? R(0, guestImageStale ? "sha256:stale " : "present")
                                : R(1, {}, "No such image");
        if (remote.contains("docker unpause")) {
            containerPaused = false;
            return R(0);
        }
        if (remote.contains("docker pause")) {
            containerPaused = true;
            return R(0);
        }
        if (remote.contains("State.Running") && !remote.contains("State.Paused"))
            return containerRunning ? R(0, "true") : R(1, {}, "No such container");
        if (remote.contains("State.Running"))  // connect/sleep container-state probe
            return containerRunning
                       ? R(0, containerPaused ? "true true\n" : "true false\n")
                       : R(1, {}, "No such container");
        if (remote.contains("docker inspect"))  // macvlan auto-IP discovery at connect
            return R(0, macvlanIp.isEmpty() ? QString() : macvlanIp + "\n");
        if (remote.contains("getprop")) return R(0, pop(bootStates, "0"));
        if (remote.contains("docker start")) return startOk ? R(0) : R(1);
        if (remote.contains("docker run")) {
            // A successful run leaves the container UP. The vm bringup branch above already models
            // this; without it here, a remote deploy provisioned and then failed its own connect
            // with "container is not running", which is not a thing that happens to a real deploy.
            if (runOk) containerRunning = true;
            return runOk ? R(0) : R(1, {}, "run failed");
        }
        if (remote.contains("netfix")) return netfixOk ? R(0) : R(1, {}, "netfix boom");
        return R(0);
    }
    static QString pop(QStringList &l, const QString &def) {
        if (l.size() > 1) {
            const QString v = l.first();
            l.removeFirst();
            return v;
        }
        return l.isEmpty() ? def : l.first();
    }
};

// Whether a folder holds this file. Not QFile::exists: on Windows Qt takes a .lnk for the shortcut
// it is and asks about its target, and a placeholder shortcut has none, so every one of them read
// as missing there and a check that one was deleted passed whatever happened.
static bool holds(const QString &folder, const QString &name) {
    return QDir(folder).entryList(QDir::Files | QDir::System | QDir::Hidden).contains(name);
}

// A Windows client with a device of two apps (bd remora-7oih). The icon render succeeds for com.a
// and leaves an empty file for com.b, and the pull lands nested, as `adb pull` does into a
// destination that exists. Shortcuts are recorded, and written as placeholder files so a refresh
// can be seen to delete what it no longer lists.
class StartMenuFake : public Spawner {
public:
    QString listApps = QStringLiteral(" * App A  com.a\n - App B  com.b\n");
    QStringList calls;
    QVector<Shortcut> links;
    int waits = 0;
    int emptyListsFirst = 0;  // a device just booted: its first N app lists come back empty
    ProcResult run(const QStringList &argv, const QMap<QString, QString> &,
                   const LineSink &) override {
        calls << argv.join(QLatin1Char(' '));
        if (argv.contains(QStringLiteral("--list-apps")))
            return {0, emptyListsFirst-- > 0 ? QString() : listApps, {}};
        if (argv.value(0) == QStringLiteral("adb") && argv.value(4).contains("IconDump"))
            return {0, QStringLiteral("OK com.a\nOK com.b\n"), {}};
        if (argv.value(0) == QStringLiteral("adb") && argv.value(3) == QStringLiteral("pull")) {
            const QString dir = argv.value(5) + QStringLiteral("/remora-icons");
            QDir().mkpath(dir);
            QFile a(dir + QStringLiteral("/com.a.png"));
            if (a.open(QIODevice::WriteOnly))
                a.write(QByteArray("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\xc0\0\0\0\xc0png", 27));
            QFile b(dir + QStringLiteral("/com.b.png"));
            if (b.open(QIODevice::WriteOnly)) b.close();
        }
        return {0, {}, {}};
    }
    Detached spawnDetached(const QStringList &, const QMap<QString, QString> &) override {
        return {};
    }
    bool detachedAlive(const Detached &) override { return false; }
    void waitMs(int) override { ++waits; }
    LocalOs localOs() const override { return LocalOs::Windows; }
    QString writeShortcut(const Shortcut &s) override {
        links << s;
        QFile f(s.path);
        return f.open(QIODevice::WriteOnly) ? QString() : f.errorString();
    }
};

class RecordingSink : public LogSink {
public:
    QVector<QPair<QString, StepState>> states;
    QStringList details;
    void state(const QString &step, StepState st, const QString &detail, const QString &) override {
        states.append({step, st});
        if (!detail.isEmpty()) details << step + ": " + detail;
    }
    StepState of(const QString &step) const {
        StepState r = StepState::Pending;
        for (const auto &p : states)
            if (p.first == step) r = p.second;
        return r;
    }
};

static QString fx(const QString &name) {
    QFile f(sourcePath(QStringLiteral("tests/fixtures/") + name));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

static bool hasCall(const FakeSpawner &sp, const QString &sub) {
    for (const QString &c : sp.calls)
        if (c.contains(sub)) return true;
    return false;
}

// hasCall substring-matches the whole joined argv, which is fine for a distinctive command but
// NOT for a bare program name: a payload can legitimately contain it (an adb pubkey carries a
// "user@host" comment, and a hostname such as "the dev host" contains "ssh"). Match the PROGRAM.
static bool spawnsProgram(const FakeSpawner &sp, const QString &program) {
    for (const QString &c : sp.calls)
        if (c == program || c.startsWith(program + QLatin1Char(' '))) return true;
    return false;
}

static int countCalls(const FakeSpawner &sp, const QString &sub) {
    int n = 0;
    for (const QString &c : sp.calls)
        if (c.contains(sub)) ++n;
    return n;
}

// One registry entry as a running mirror reports it. Live and a minute old unless a test says
// otherwise — a settled session, the one shape no reap may mistake for a husk or a starter.
static MirrorEntry mirrorAt(qint64 pid, const QString &serial, const QString &title,
                            MirrorKind kind = MirrorKind::Main,
                            MirrorSessionState session = MirrorSessionState::Live,
                            qint64 ageMs = 60000) {
    MirrorEntry e;
    e.pid = pid;
    e.serial = serial;
    e.title = title;
    e.kind = kind;
    e.session = session;
    e.startedMs = QDateTime::currentMSecsSinceEpoch() - ageMs;
    return e;
}

// Did the engine stop exactly this set of pids in one call (order aside)?
static bool stopped(const FakeSpawner &sp, QList<qint64> pids, bool force = true) {
    std::sort(pids.begin(), pids.end());
    for (const FakeSpawner::Stop &st : sp.stops) {
        QList<qint64> got = st.pids;
        std::sort(got.begin(), got.end());
        if (got == pids && st.force == force && (!force || st.graceMs > 0)) return true;
    }
    return false;
}

static bool everStopped(const FakeSpawner &sp, qint64 pid) {
    for (const FakeSpawner::Stop &st : sp.stops)
        if (st.pids.contains(pid)) return true;
    return false;
}

// Read a splash status file's full contents ("" when absent).
static QString statusFileText(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// A remote RunContext needs an ssh host, which has no baked default any more — it used to resolve
// to one developer's machine, which made the binary non-reproducible for anyone else (bd
// remora-4ei.12). Supply a throwaway value here; FakeSpawner dispatches on argv shape, not on the
// host, so the exact string does not matter.
// True when every device/<tree> path a build step names is device/remora — the one tree both
// releases lunch from, so any other name is a step editing a tree the build does not use.
static bool onlyRemoraDeviceTree(const QString &step) {
    static const QRegularExpression dev(QStringLiteral("/device/([^/\\s'\"]+)/"));
    int seen = 0;
    for (auto it = dev.globalMatch(step); it.hasNext(); ++seen)
        if (it.next().captured(1) != QLatin1String("remora")) return false;
    return seen > 0;
}

static RemoraConfig withGuest(RemoraConfig c = {}) {
    c.backend.sshHost = QStringLiteral("tester@guest.invalid");
    return c;
}

// …and macvlan on top, for the cases about the container's OWN LAN address. Remote defaults to
// bridge, so a test that wants IPAM discovery has to ask for the mode rather than inherit it.
static RemoraConfig withMacvlan(RemoraConfig c = {}) {
    c = withGuest(c);
    c.network.networkMode = QStringLiteral("macvlan");
    return c;
}

// Plays just enough of a device for injectRedditSession, and records every argv it is handed — the
// point being what those argv contain, since any other user on the host can read them through ps.
class RedditFakeSpawner : public Spawner {
public:
    QList<QStringList> calls;
    // What a pull delivers: a real AccountManager database (see accountDb), as on a device.
    QByteArray pulledDb;
    // Every file pushed back, by its local path, read at the moment it was pushed.
    QList<QByteArray> pushed;
    ProcResult run(const QStringList &argv, const QMap<QString, QString> &,
                   const LineSink &) override {
        calls << argv;
        const QString cmd = argv.join(QLatin1Char(' '));
        ProcResult r;
        if (cmd.contains(QLatin1String("stat -c %u"))) r.out = QStringLiteral("10123\n");
        if (cmd.contains(QLatin1String("mktemp"))) r.out = QStringLiteral("/tmp/remora-rl.AbC123\n");
        // A pull lands in a local file: `… cat '<container path>' > '<local>'` on bare, scp on remote.
        static const QRegularExpression pull(QStringLiteral("cat '[^']+' > '([^']+)'"));
        QString landed = pull.match(cmd).captured(1);
        // scp's operands are its last two arguments, whatever options and `--` come first.
        const bool scp = argv.value(0) == QLatin1String("scp") && argv.size() >= 3;
        const QString scpFrom = scp ? argv.value(argv.size() - 2) : QString();
        const QString scpTo = scp ? argv.last() : QString();
        if (scp && scpFrom.contains(QLatin1Char(':'))) landed = scpTo;
        if (!landed.isEmpty()) {
            QFile f(landed);
            if (f.open(QIODevice::WriteOnly)) f.write(pulledDb.isEmpty() ? QByteArray("db") : pulledDb);
        }
        // …and a push reads one: `… cat > <path>' < '<local>'` on bare, scp to the host on remote.
        static const QRegularExpression push(QStringLiteral("< '([^']+)'$"));
        QString sent = argv.value(0) == QLatin1String("sh") ? push.match(cmd).captured(1) : QString();
        if (scp && scpTo.contains(QLatin1Char(':'))) sent = scpFrom;
        if (!sent.isEmpty()) {
            QFile f(sent);
            if (f.open(QIODevice::ReadOnly)) pushed << f.readAll();
        }
        return r;
    }
    // An accounts database with Android's shape: an AUTOINCREMENT accounts table (hence
    // sqlite_sequence), the extras table, and one unrelated account already in it.
    static QByteArray accountDb(bool ce) {
        QTemporaryDir d;
        const QString path = d.filePath(QStringLiteral("a.db"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("fixture"));
            db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery q(db);
                q.exec(ce ? QStringLiteral("CREATE TABLE accounts(_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                           "name TEXT NOT NULL, type TEXT NOT NULL, password TEXT, "
                                           "UNIQUE(name,type))")
                          : QStringLiteral("CREATE TABLE accounts(_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                           "name TEXT NOT NULL, type TEXT NOT NULL, previous_name TEXT, "
                                           "last_password_entry_time_millis_epoch INTEGER, UNIQUE(name,type))"));
                if (ce)
                    q.exec(QStringLiteral("CREATE TABLE extras(_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                          "accounts_id INTEGER, key TEXT NOT NULL, value TEXT, "
                                          "UNIQUE(accounts_id,key))"));
                q.exec(QStringLiteral("INSERT INTO accounts(name,type) VALUES('other','com.example')"));
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    Detached spawnDetached(const QStringList &, const QMap<QString, QString> &) override {
        return {};
    }
    bool detachedAlive(const Detached &) override { return false; }
    void waitMs(int) override {}
};

// bd remora-lqnv: runs the REAL generated chain from its Mesa staging step to the end, through
// /bin/sh, with every outside command faked — stage-features.sh, assemble-image.sh and docker log
// what they were asked to do into <dir>/log, and a failure is arranged by a file under <dir>/fake.
// No Docker, no tree, nothing outside the temporary directory.
struct MesaTailRun {
    int rc = -1;
    QString out, log;
};
struct MesaTailSetup {
    bool payload = false;        // lib64/dri/libgallium_dri.so exists
    bool emptyPayloadDir = false;
    bool prereqsBuilt = false;   // libdrm.so + libva's generated header already in out/
    bool doBuildScript = true;   // source-build/do-build.sh exists (absent = the cp fails)
    bool publicRelease = false;
    int stageRc = 0, builderRc = 0, moduleRc = 0;
    bool moduleMakesNothing = false;  // module build exits 0 but installs nothing
};
static void writeExec(const QString &path, const QByteArray &body) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(body);
    f.close();
    QVERIFY(f.setPermissions(f.permissions() | QFileDevice::ExeOwner));
}
static MesaTailRun runMesaTail(const QString &dir, const MesaTailSetup &s) {
    MesaTailRun r;
    const QString tree = dir + QStringLiteral("/tree"), sb = dir + QStringLiteral("/sb"),
                  fake = dir + QStringLiteral("/fake"), bin = dir + QStringLiteral("/bin");
    for (const QString &d : {tree, sb + QStringLiteral("/features"), fake, bin}) QDir().mkpath(d);
    const QString drm = tree + QStringLiteral("/out/target/product/remora_x86_64/vendor/lib64");
    const QString vag =
        tree + QStringLiteral("/out/soong/.intermediates/external/libva/libva_gen_headers/gen/va");
    const QString mesa = sb + QStringLiteral("/features/mesa_source/lib64");
    if (s.emptyPayloadDir) QDir().mkpath(mesa);
    if (s.payload) {
        QDir().mkpath(mesa + QStringLiteral("/dri"));
        QFile(mesa + QStringLiteral("/dri/libgallium_dri.so")).open(QIODevice::WriteOnly);
    }
    if (s.prereqsBuilt) {
        QDir().mkpath(drm);
        QDir().mkpath(vag);
        QFile(drm + QStringLiteral("/libdrm.so")).open(QIODevice::WriteOnly);
        QFile(vag + QStringLiteral("/va_version.h")).open(QIODevice::WriteOnly);
    }
    if (s.doBuildScript) writeExec(sb + QStringLiteral("/do-build.sh"), "#!/bin/sh\n");
    auto rcFile = [&](const char *name, int rc) {
        QFile f(fake + QLatin1Char('/') + QLatin1String(name));
        if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::number(rc));
    };
    rcFile("stage-rc", s.stageRc);
    rcFile("build-rc", s.builderRc);
    rcFile("module-rc", s.moduleRc);
    // The real stage-features.sh refuses Mesa staging without its payload; so does the fake.
    writeExec(sb + QStringLiteral("/stage-features.sh"),
              "#!/bin/sh\necho \"STAGE mesa=${WITH_MESA_SOURCE:-false}\" >> \"$FAKE_DIR/../log\"\n"
              "if [ \"$WITH_MESA_SOURCE\" = true ] && [ ! -d \"$2/mesa_source/lib64\" ]; then "
              "exit 1; fi\nexit \"$(cat \"$FAKE_DIR/stage-rc\")\"\n");
    writeExec(sb + QStringLiteral("/assemble-image.sh"),
              "#!/bin/sh\necho ASSEMBLE >> \"$FAKE_DIR/../log\"\n");
    writeExec(bin + QStringLiteral("/docker"),
              QStringLiteral(
                  "#!/bin/sh\nL=\"$FAKE_DIR/../log\"\ncase \"$1\" in\n"
                  "image) exit 0 ;;\n"
                  "build) echo BUILDER-BUILD >> \"$L\"; exit \"$(cat \"$FAKE_DIR/build-rc\")\" ;;\n"
                  "run) case \"$*\" in\n"
                  "  *--module*) echo \"MODULE-BUILD $*\" >> \"$L\"; "
                  "rc=$(cat \"$FAKE_DIR/module-rc\"); if [ \"$rc\" = 0 ] && [ %3 = no ]; then "
                  "mkdir -p %1 %2 && : > %1/libdrm.so && : > %2/va_version.h; fi; exit \"$rc\" ;;\n"
                  "  *) echo FULL-BUILD >> \"$L\"; exit 0 ;;\n  esac ;;\nesac\nexit 0\n")
                  .arg(drm, vag, s.moduleMakesNothing ? QStringLiteral("yes") : QStringLiteral("no"))
                  .toUtf8());

    SourceBuildPlan plan;
    plan.tree = tree;
    plan.sourceBuildDir = sb;
    plan.builtTag = QStringLiteral("remora24:x86_64-src");
    plan.androidVersion = 17;
    plan.withAgent = plan.withMesaSource = true;
    plan.publicRelease = s.publicRelease;
    auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
    const QStringList steps = sourceBuildSteps(plan, noVendor);
    qsizetype first = -1;
    for (qsizetype i = 0; i < steps.size() && first < 0; ++i)
        if (steps.at(i).contains(QLatin1String("stage-features.sh"))) first = i;
    if (first < 0) return r;

    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"), bin + QLatin1Char(':') + env.value(QStringLiteral("PATH")));
    env.insert(QStringLiteral("FAKE_DIR"), fake);
    env.insert(QStringLiteral("HOME"), dir);
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), sourceBuildCommand(steps.mid(first))});
    if (!p.waitForFinished(30000)) return r;
    r.rc = p.exitCode();
    r.out = QString::fromUtf8(p.readAll());
    QFile lf(dir + QStringLiteral("/log"));
    if (lf.open(QIODevice::ReadOnly)) r.log = QString::fromUtf8(lf.readAll());
    return r;
}

// A Linux client whose reachability probes answer as told, and which records every call. The
// image-store query is answered on both run() and runTimed(), so only `timed` tells them apart.
class StoreProbeSpawner : public Spawner {
public:
    bool localDockerUp = true, sshUp = true, remoteDockerUp = true;
    int storeRc = 0;
    QString storeOut;
    QStringList calls;                     // every run() argv, joined
    QList<QPair<QStringList, int>> timed;  // every runTimed() argv and deadline
    bool timedHadSink = false;

    static bool isStoreQuery(const QStringList &argv) {
        return argv.join(QLatin1Char(' ')).contains(QLatin1String("docker info --format"));
    }
    ProcResult run(const QStringList &argv, const QMap<QString, QString> &,
                   const LineSink &) override {
        calls << argv.join(QLatin1Char(' '));
        const QString last = argv.isEmpty() ? QString() : argv.last();
        ProcResult r;
        if (isStoreQuery(argv)) {
            r.rc = storeRc;
            r.out = storeOut;
        } else if (argv.first() == QLatin1String("ssh") && last == QLatin1String("'echo ok'")) {
            r.out = sshUp ? QStringLiteral("ok\n") : QString();
            r.rc = sshUp ? 0 : 255;
        } else if (argv.first() == QLatin1String("ssh") &&
                   last.contains(QLatin1String("docker info >/dev/null"))) {
            r.out = remoteDockerUp ? QStringLiteral("ok\n") : QString();
        } else if (last.contains(QLatin1String("docker version"))) {
            r.rc = localDockerUp ? 0 : 1;
        }
        return r;
    }
    ProcResult runTimed(const QStringList &argv, int timeoutMs, const LineSink &onLine) override {
        timed.append({argv, timeoutMs});
        if (onLine) timedHadSink = true;
        return run(argv, {}, {});
    }
    Detached spawnDetached(const QStringList &, const QMap<QString, QString> &) override {
        return {};
    }
    bool detachedAlive(const Detached &) override { return false; }
    LocalOs localOs() const override { return LocalOs::Unix; }
    QStringList nonStoreCalls() const {
        QStringList out;
        for (const QString &c : calls)
            if (!c.contains(QLatin1String("docker info --format"))) out << c;
        return out;
    }
};

static const QString kStoreHost = QStringLiteral("tester@store.invalid");
static const QStringList kLocalStoreArgv{"docker", "info", "--format", "{{json .}}"};
static const QStringList kRemoteStoreArgv{
    "ssh", "-o", "ConnectTimeout=5", "tester@store.invalid", "/bin/sh", "-c",
    "'docker info --format '\\''{{json .}}'\\'' 2>/dev/null'"};
static const QString kContainerdReply = QStringLiteral(
    R"({"Driver":"overlayfs","DriverStatus":[["driver-type","io.containerd.snapshotter.v1"]]})");
static const QString kOverlay2Reply = QStringLiteral(
    R"({"Driver":"overlay2","DriverStatus":[["Backing Filesystem","extfs"],["Supports d_type","true"]]})");

static QVector<PrereqResult> storeRows(const QVector<PrereqResult> &rows) {
    QVector<PrereqResult> out;
    for (const PrereqResult &r : rows)
        if (r.key == QLatin1String("docker_image_store")) out << r;
    return out;
}

#ifdef Q_OS_LINUX
// SelfImageWatch fixtures: real files and stat/access in a private temporary directory, with only
// exec replaced by a recorder, so nothing is ever exec'd in the test runner.
struct SelfExecLog {
    QList<QPair<QString, QStringList>> calls;
    int nextErr = 0;  // what the next exec "returns": 0 = it happened
};

static SelfImageOps recordingSelfOps(SelfExecLog &log) {
    SelfImageOps ops = realSelfImageOps();
    ops.exec = [&log](const QString &path, const QStringList &argv) {
        log.calls.append({path, argv});
        return log.nextErr;
    };
    return ops;
}

static bool writeSelfFile(const QString &path, const QByteArray &body, bool executable) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body);
    f.close();
    QFile::Permissions p = QFile::ReadOwner | QFile::WriteOwner;
    if (executable) p |= QFile::ExeOwner;
    return f.setPermissions(p);
}

// Stands in for /proc/self/exe: a link naming the file the process is executing.
static bool pointSelfLink(const QString &link, const QString &target) {
    QFile::remove(link);
    return QFile::link(target, link);
}

// A host for runUpdateWatchPass: the profile, the catalog's answers, the Apply signal, and every
// operation the pass performs, in order.
static const QString kOfferVersion = QStringLiteral("20301231.120000");
static ProcResult catalogAnswer(bool update) {
    return {0,
            QStringLiteral("REMORA_IMAGES_RESULT={\"update\": %1, \"version\": \"%2\", "
                           "\"bytes\": 2000000000}\n")
                .arg(update ? QStringLiteral("true") : QStringLiteral("false"), kOfferVersion),
            {}};
}
static ProcResult catalogDown() { return {1, {}, QStringLiteral("network is unreachable")}; }

struct FakeUpdateHost {
    UpdateWatchProfile profile{true, kPublicImageTag};
    QList<ProcResult> checks;  // answered in order; the last one repeats
    bool applyWaiting = false;
    int fetchRc = 0;
    bool deployOk = true;
    std::function<void()> duringDeploy;
    QDateTime clock = QDateTime::fromString(QStringLiteral("2030-01-01T00:00:00Z"), Qt::ISODate);
    QStringList events, said;
    QJsonObject probedOffer;
    QString probedError;

    QStringList take() { return std::exchange(events, {}); }
    UpdateWatchOps ops() {
        UpdateWatchOps o;
        o.profile = [this] { return profile; };
        o.checkRelease = [this] {
            events << QStringLiteral("check");
            return checks.size() > 1 ? checks.takeFirst() : checks.value(0, catalogDown());
        };
        o.probe = [this](const QJsonObject &offer, const QString &lastError) {
            events << QStringLiteral("probe");
            probedOffer = offer;
            probedError = lastError;
            return applyWaiting;
        };
        o.fetchRelease = [this] {
            events << QStringLiteral("fetch");
            return ProcResult{fetchRc, {}, fetchRc ? QStringLiteral("write failed\ndisk full")
                                                   : QString()};
        };
        o.deploy = [this] {
            events << QStringLiteral("deploy");
            if (duringDeploy) duringDeploy();
            return deployOk;
        };
        o.clearApply = [this] {
            events << QStringLiteral("clear");
            applyWaiting = false;
        };
        o.now = [this] { return clock; };
        o.say = [this](const QString &line) { said << line; };
        return o;
    }
};
#endif

class TestEngine : public QObject {
    Q_OBJECT
private slots:
    // The readiness display asks the TARGET daemon, once, with a deadline, and only once that
    // daemon answered: locally for bare, over ssh for remote.
    void imageStoreProbeAsksTargetDaemon() {
        StoreProbeSpawner bare;
        bare.storeOut = kContainerdReply;
        const HostCapabilities b =
            probeCapabilities(Backend::Bare, QString(), bare, ImageStoreQuery::Ask);
        QCOMPARE(bare.timed, (QList<QPair<QStringList, int>>{{kLocalStoreArgv, 5000}}));
        QVERIFY(kImageStoreQueryTimeoutMs > 0);
        QVERIFY(!bare.timedHadSink);
        QVERIFY(b.dockerImageStore.has_value());
        QCOMPARE(b.dockerImageStore->kind, DockerImageStoreKind::ContainerdSnapshotter);
        const auto br = storeRows(checkReadiness(Backend::Bare, b));
        QCOMPARE(br.size(), 1);
        QCOMPARE(br.at(0).label,
                 QStringLiteral("Docker image store: containerd snapshotter (overlayfs)"));
        QCOMPARE(br.at(0).severity, QStringLiteral("info"));

        StoreProbeSpawner remote;
        remote.storeOut = kOverlay2Reply;
        const HostCapabilities r =
            probeCapabilities(Backend::Remote, kStoreHost, remote, ImageStoreQuery::Ask);
        QCOMPARE(remote.timed, (QList<QPair<QStringList, int>>{{kRemoteStoreArgv, 5000}}));
        QVERIFY(!remote.timedHadSink);
        for (const QString &c : remote.calls)  // never the local daemon for a remote target
            QVERIFY2(!c.startsWith(QLatin1String("docker info")), qPrintable(c));
        const auto rr = storeRows(checkReadiness(Backend::Remote, r));
        QCOMPARE(rr.size(), 1);
        QCOMPARE(rr.at(0).label, QStringLiteral("Docker image store: overlay2 graph driver"));

        // An unreachable target daemon is not asked: no query, no store, no row.
        StoreProbeSpawner down;
        down.localDockerUp = false;
        down.storeOut = kContainerdReply;
        const HostCapabilities d =
            probeCapabilities(Backend::Bare, QString(), down, ImageStoreQuery::Ask);
        QVERIFY(down.timed.isEmpty());
        QVERIFY(!d.dockerImageStore);
        QVERIFY(storeRows(checkReadiness(Backend::Bare, d)).isEmpty());
        for (const bool sshUp : {false, true}) {
            StoreProbeSpawner far;
            far.sshUp = sshUp;
            far.remoteDockerUp = false;  // the local daemon answers; there is no fallback to it
            far.storeOut = kContainerdReply;
            const HostCapabilities f =
                probeCapabilities(Backend::Remote, kStoreHost, far, ImageStoreQuery::Ask);
            QVERIFY(far.timed.isEmpty());
            QVERIFY(!f.dockerImageStore);
            QVERIFY(storeRows(checkReadiness(Backend::Remote, f)).isEmpty());
        }
    }

    // Status, deploy capability discovery and the default probe never ask; Ask adds exactly the
    // one query and leaves every other call as it was, in order.
    void imageStoreQueryIsReadinessOnly() {
        for (const Backend target : {Backend::Bare, Backend::Remote}) {
            const QString host = target == Backend::Remote ? kStoreHost : QString();
            StoreProbeSpawner plain;
            plain.storeOut = kContainerdReply;
            const HostCapabilities p = probeCapabilities(target, host, plain);
            QVERIFY(!p.dockerImageStore);
            QVERIFY(plain.timed.isEmpty());
            QVERIFY(plain.calls == plain.nonStoreCalls());
            QVERIFY(storeRows(checkReadiness(target, p)).isEmpty());

            StoreProbeSpawner status;
            status.storeOut = kContainerdReply;
            const StatusReport rep = probeStatus(target, status, host, QString());
            QVERIFY(!rep.host.dockerImageStore);
            QVERIFY(status.timed.isEmpty());
            QVERIFY(status.calls == status.nonStoreCalls());

            StoreProbeSpawner deploy;
            deploy.storeOut = kContainerdReply;
            RemoraConfig cfg;
            cfg.gpu.mode = GpuMode::Host;
            if (target == Backend::Remote) cfg.backend.sshHost = kStoreHost;
            const auto dc = deployCapabilities(target, cfg, deploy);
            QVERIFY(dc.has_value());
            QVERIFY(!deploy.calls.isEmpty());
            QVERIFY(!dc->dockerImageStore);
            QVERIFY(deploy.timed.isEmpty());
            QVERIFY(deploy.calls == deploy.nonStoreCalls());

            StoreProbeSpawner asked;
            asked.storeOut = kContainerdReply;
            const HostCapabilities a = probeCapabilities(target, host, asked, ImageStoreQuery::Ask);
            QCOMPARE(asked.timed.size(), 1);
            QCOMPARE(asked.nonStoreCalls(), plain.calls);
            QCOMPARE(storeRows(checkReadiness(target, a)).size(), 1);
        }
    }

    // A failed or timed-out query is "not reported" even with success-looking stdout, and the row
    // it adds changes nothing else about readiness.
    void imageStoreFailureAndTimeoutStayUnknown() {
        for (const Backend target : {Backend::Bare, Backend::Remote}) {
            for (const int rc : {1, 124}) {
                const QString host = target == Backend::Remote ? kStoreHost : QString();
                StoreProbeSpawner skip;
                const auto before = checkReadiness(target, probeCapabilities(target, host, skip));
                StoreProbeSpawner sp;
                sp.storeRc = rc;
                sp.storeOut = kContainerdReply;
                const HostCapabilities c =
                    probeCapabilities(target, host, sp, ImageStoreQuery::Ask);
                QCOMPARE(sp.timed.size(), 1);
                QCOMPARE(sp.timed.at(0).first,
                         target == Backend::Bare ? kLocalStoreArgv : kRemoteStoreArgv);
                QVERIFY(sp.timed.at(0).second > 0);
                QVERIFY(c.dockerImageStore.has_value());
                QCOMPARE(c.dockerImageStore->kind, DockerImageStoreKind::Unknown);
                const auto after = checkReadiness(target, c);
                QCOMPARE(after.size(), before.size() + 1);
                const auto rows = storeRows(after);
                QCOMPARE(rows.size(), 1);
                QCOMPARE(rows.at(0).label,
                         QStringLiteral("Docker image store: not reported by docker info"));
                QVERIFY(rows.at(0).ok);
                QVERIFY(rows.at(0).remedy.isEmpty());
                QCOMPARE(isReady(after), isReady(before));
                QCOMPARE(blockers(after).size(), blockers(before).size());
                for (const PrereqResult &r : after)
                    QVERIFY(!r.label.contains(QLatin1String("snapshotter")) &&
                            !r.remedy.contains(QLatin1String("snapshotter")));
            }
        }
    }
    // bd remora-lqnv. A fresh tree with mesa_source on and no payload: the chain prepares exactly
    // what build-mesa.sh reads from the tree, then STOPS — nonzero, with the helper's invocation,
    // and without staging Mesa, building the image, assembling it or recording patch state.
    void missingMesaPayloadPreparesLibdrmThenStops_data() {
        QTest::addColumn<bool>("emptyPayloadDir");
        QTest::newRow("no-payload-directory") << false;
        QTest::newRow("empty-payload-directory") << true;
    }
    void missingMesaPayloadPreparesLibdrmThenStops() {
        SKIP_ON_WINDOWS(kRunsSh);
        QFETCH(bool, emptyPayloadDir);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MesaTailSetup s;
        s.emptyPayloadDir = emptyPayloadDir;
        const MesaTailRun r = runMesaTail(dir.path(), s);
        QVERIFY2(r.rc != 0, qPrintable(r.out));
        // staged WITHOUT Mesa (the only staging the module build's guard accepts), then the builder,
        // then the module build — in that order
        const qsizetype stage = r.log.indexOf(QLatin1String("STAGE mesa=false"));
        const qsizetype builder = r.log.indexOf(QLatin1String("BUILDER-BUILD"));
        const qsizetype module = r.log.indexOf(QLatin1String("MODULE-BUILD"));
        QVERIFY2(stage >= 0 && builder > stage && module > builder, qPrintable(r.log));
        for (const char *m : {"libdrm", "libdrm_amdgpu", "libdrm_intel", "libva_gen_headers"})
            QVERIFY2(r.log.contains(QLatin1Char(' ') + QLatin1String(m)), m);
        QVERIFY2(!r.log.contains(QLatin1String("STAGE mesa=true")), qPrintable(r.log));
        QVERIFY2(!r.log.contains(QLatin1String("FULL-BUILD")), qPrintable(r.log));
        QVERIFY2(!r.log.contains(QLatin1String("ASSEMBLE")), qPrintable(r.log));
        QVERIFY2(!r.out.contains(QLatin1String("patch state recorded")), qPrintable(r.out));
        QVERIFY2(r.out.contains(QStringLiteral("REMORA_MESA_TREE=%1/tree").arg(dir.path())),
                 qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("vendor/host-prereqs/mesa-android/build-mesa.sh")),
                 qPrintable(r.out));
        QVERIFY2(r.out.contains(QStringLiteral("REMORA_MESA_OUT=%1/sb/features/mesa_source/lib64")
                                    .arg(dir.path())),
                 qPrintable(r.out));
    }

    // THE REGRESSION THE FIRST CANDIDATE HAD: a failed staging or a failed cp must stop the
    // preparation dead — no builder image, no module build, no full build, no instructions that
    // read as "ready". Each failure is checked separately.
    void mesaPreparationStopsOnEveryEarlyFailure() {
        SKIP_ON_WINDOWS(kRunsSh);
        struct Case {
            const char *name;
            MesaTailSetup setup;
            QStringList mustNotRun;
        };
        MesaTailSetup stageFails, cpFails, builderFails, moduleFails, moduleEmpty;
        stageFails.stageRc = 1;
        cpFails.doBuildScript = false;
        builderFails.builderRc = 1;
        moduleFails.moduleRc = 2;
        moduleEmpty.moduleMakesNothing = true;
        const QStringList all{QStringLiteral("BUILDER-BUILD"), QStringLiteral("MODULE-BUILD"),
                              QStringLiteral("FULL-BUILD"), QStringLiteral("ASSEMBLE"),
                              QStringLiteral("STAGE mesa=true")};
        const QList<Case> cases{
            {"staging fails", stageFails, all},
            {"cp fails", cpFails, all},
            {"builder fails", builderFails, all.mid(1)},
            {"module build fails", moduleFails, all.mid(2)},
            {"module build installs nothing", moduleEmpty, all.mid(2)},
        };
        for (const Case &c : cases) {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const MesaTailRun r = runMesaTail(dir.path(), c.setup);
            QVERIFY2(r.rc > 0, c.name);
            for (const QString &m : c.mustNotRun)
                QVERIFY2(!r.log.contains(m),
                         qPrintable(QStringLiteral("%1: %2 ran\n%3").arg(QLatin1String(c.name), m,
                                                                         r.log)));
            QVERIFY2(!r.out.contains(QLatin1String("build-mesa.sh")),
                     qPrintable(QLatin1String(c.name) + QLatin1Char('\n') + r.out));
            QVERIFY2(!r.out.contains(QLatin1String("patch state recorded")), c.name);
        }
    }

    // Prerequisites a previous build left in out/ are reused: no staging, no Docker at all, straight
    // to the helper's invocation — and still a stop, never a build.
    void mesaPreparationReusesBuiltPrerequisites() {
        SKIP_ON_WINDOWS(kRunsSh);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MesaTailSetup s;
        s.prereqsBuilt = true;
        const MesaTailRun r = runMesaTail(dir.path(), s);
        QVERIFY(r.rc != 0);
        QVERIFY2(r.log.isEmpty(), qPrintable(r.log));
        QVERIFY2(r.out.contains(QLatin1String("build-mesa.sh")), qPrintable(r.out));
        QVERIFY(!r.out.contains(QLatin1String("patch state recorded")));
    }

    // A public release must not be staged without Mesa — its staging refuses that by policy — so a
    // missing payload with nothing built is refused before anything runs, naming the way round.
    void publicMesaPreparationRefusesToStageWithoutMesa() {
        SKIP_ON_WINDOWS(kRunsSh);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MesaTailSetup s;
        s.publicRelease = true;
        const MesaTailRun r = runMesaTail(dir.path(), s);
        QVERIFY(r.rc > 0);
        QVERIFY2(r.log.isEmpty(), qPrintable(r.log));
        QVERIFY2(r.out.contains(QLatin1String("public release")), qPrintable(r.out));
        QVERIFY(!r.out.contains(QLatin1String("patch state recorded")));
        // with the prerequisites already built, the public route reaches the helper too
        QTemporaryDir dir2;
        s.prereqsBuilt = true;
        const MesaTailRun r2 = runMesaTail(dir2.path(), s);
        QVERIFY(r2.rc > 0);
        QVERIFY2(r2.log.isEmpty(), qPrintable(r2.log));
        QVERIFY2(r2.out.contains(QLatin1String("build-mesa.sh")), qPrintable(r2.out));
    }

    // With the payload present nothing changes: Mesa is staged, the full build and assembly run,
    // patch state is recorded, no module build happens, and every step after staging is the very
    // text a mesa-less plan generates.
    void presentMesaPayloadKeepsTheNormalRoute() {
        SKIP_ON_WINDOWS(kRunsSh);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MesaTailSetup s;
        s.payload = true;
        const MesaTailRun r = runMesaTail(dir.path(), s);
        QVERIFY2(r.rc == 0, qPrintable(r.out + r.log));
        QVERIFY2(!r.log.contains(QLatin1String("MODULE-BUILD")), qPrintable(r.log));
        QVERIFY2(!r.log.contains(QLatin1String("STAGE mesa=false")), qPrintable(r.log));
        const qsizetype stage = r.log.indexOf(QLatin1String("STAGE mesa=true"));
        const qsizetype full = r.log.indexOf(QLatin1String("FULL-BUILD"));
        QVERIFY2(stage >= 0 && full > stage && r.log.indexOf(QLatin1String("ASSEMBLE")) > full,
                 qPrintable(r.log));
        QVERIFY(r.out.contains(QLatin1String("patch state recorded")));
        QVERIFY(!r.out.contains(QLatin1String("build-mesa.sh")));

        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        SourceBuildPlan p;
        p.tree = QStringLiteral("/src/tree");
        p.sourceBuildDir = QStringLiteral("/root/source-build");
        p.builtTag = QStringLiteral("t:1");
        p.androidVersion = 17;
        p.withAgent = true;
        const QStringList without = sourceBuildSteps(p, noVendor);
        p.withMesaSource = true;
        const QStringList with = sourceBuildSteps(p, noVendor);
        QCOMPARE(with.size(), without.size());
        qsizetype stageIdx = -1;
        for (qsizetype i = 0; i < with.size(); ++i) {
            if (with.at(i).contains(QLatin1String("stage-features.sh"))) {
                stageIdx = i;
                break;
            }
            QCOMPARE(with.at(i), without.at(i));
        }
        QVERIFY(stageIdx >= 0);
        QCOMPARE(with.mid(stageIdx + 1), without.mid(stageIdx + 1));
    }

    // A FAILING STEP MUST STOP THE CHAIN. This is behavioural, not textual, because the bug it
    // guards was invisible to every string-comparison test: the chain LOOKED right and ran wrong.
    //
    // What happened (bd remora-h457 follow-up): steps were joined with a plain " && ", and one
    // step contained a bare top-level ';'. That splits `a && b; c && d` into TWO and-lists, so
    // everything after the ';' runs even though an earlier step failed, and the exit status is the
    // LAST list's — i.e. success. A real build refused to apply two container-compat patches, said
    // so, exited 1, and the chain carried on to compile and tag an image missing them.
    //
    // So this actually EXECUTES the composed command through /bin/sh and asserts on the exit code.
    // THE TWO ADOPTION TESTS THAT STOOD HERE WENT WITH THE SHIMS THEY COVERED (bd
    // remora-28ix.4 step 4). They pinned that provision renamed a pre-rename container and that
    // ensure-image `docker tag`-aliased a pre-rename image onto its remora24 name. Both behaviours
    // are deliberately gone, so the tests are deleted rather than inverted: an assertion that a
    // deleted function does nothing is not a regression guard, and the real guard for the rename
    // is registryHoldsOnlyRemoraBuiltTags in test_gating, which fails if any registry tag is not
    // a Remora build.

    // There is no guest-side server reap to test: the agent is one init service for the
    // container's lifetime, so no per-session server process can leak. The host-side reap keeps
    // its own coverage under reapSessionOrphans.

    void aFailingStepStopsTheBuildChain() {
        SKIP_ON_WINDOWS(kRunsSh);
        // Step 2 fails; step 3 carries the bare ';' that used to split the list.
        const QStringList steps{QStringLiteral("true"), QStringLiteral("sh -c 'exit 1'"),
                                QStringLiteral("echo ran-anyway; true")};
        const QString cmd = sourceBuildCommand(steps);
        QProcess p;
        p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
        QVERIFY(p.waitForFinished(10000));
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        QVERIFY2(p.exitCode() != 0,
                 "the composed command reported SUCCESS despite a failing step — a refusal "
                 "anywhere in the chain is no longer able to stop a build");
        QVERIFY2(!out.contains(QLatin1String("ran-anyway")),
                 "a step AFTER the failing one still executed — the and-list was split");
    }

    // THE STATE EPILOGUE MUST BE THE CHAIN'S LAST STEP (bd remora-6i2i). "Recorded LAST" is what
    // turns the short-circuit fix above into the guarantee that a refused pass cannot record the
    // tree as good: the epilogue only runs if every step before it succeeded. That ordering lived
    // in a comment; the build that shipped missing container-compat patches also recorded its bad
    // tree as coherent, so a reorder here would silently reopen that hole.
    void patchStateEpilogueIsTheLastStep() {
        SourceBuildPlan plan;
        plan.tree = QStringLiteral("/src/tree");
        plan.patchRoot = QStringLiteral("/src/patches");
        plan.sourceBuildDir = QStringLiteral("/src/sbuild");
        plan.builtTag = QStringLiteral("remora24:test-src");
        plan.androidVersion = 17;
        const QStringList steps = sourceBuildSteps(plan, readVendorPath);
        QVERIFY(!steps.isEmpty());
        QVERIFY2(steps.last().contains(QLatin1String("patch state recorded")),
                 "the patch-state epilogue is no longer the LAST step — a step after it could "
                 "fail after state was already recorded, and the next incremental build would "
                 "trust a tree the failed step left incoherent");
        for (int i = 0; i < steps.size() - 1; ++i)
            QVERIFY2(!steps.at(i).contains(QLatin1String("patch state recorded")),
                     "patch state is recorded by an EARLIER step too — a later failure would "
                     "leave the record claiming a coherent tree");
    }

    // The other half: a clean chain must still run every step and succeed, or the fix above would
    // "work" by breaking all builds.
    void aCleanChainRunsEveryStep() {
        SKIP_ON_WINDOWS(kRunsSh);
        const QStringList steps{QStringLiteral("echo one"), QStringLiteral("echo two; true"),
                                QStringLiteral("echo three")};
        QProcess p;
        p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), sourceBuildCommand(steps)});
        QVERIFY(p.waitForFinished(10000));
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(out.contains(QLatin1String("one")) && out.contains(QLatin1String("two"))
                && out.contains(QLatin1String("three")));
    }

    void initTestCase() {
        // Redirect QStandardPaths (the splash status file + zip cache live under
        // GenericCacheLocation) into ~/.qttest so tests never touch — or hijack — a real
        // splash's status file.
        QStandardPaths::setTestModeEnabled(true);
        // Belt and braces, because that one line was the ONLY thing keeping these tests off live
        // state: the fixtures used 192.168.0.77, which is this developer's actual macvlan_ip in
        // remorarc, so every splash path derived from it collided by construction with a real
        // instance's. They are on RFC 5737 TEST-NET-1 now — reserved for exactly this, routable
        // nowhere — so a missing setTestModeEnabled can no longer aim a test at a running
        // session's files. Asserted rather than assumed: if this ever stops holding, the suite
        // starts reading a file a live splash is writing, and the failure would look like flakiness
        // rather than like a collision (bd remora-eoy.9).
        QVERIFY2(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                     .contains(QLatin1String("qttest")),
                 "QStandardPaths test mode is not in effect — these tests would read and write the "
                 "real ~/.cache/remora, where a live splash writes the same files");
        useCheckoutVendorDir();
    }

    void bareHappyPath() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        auto dep = makeDeployer(Backend::Bare, sp, 24, 0);
        RecordingSink sink;
        QVERIFY(runChain(dep->steps(ctx), sink));
        for (const char *s : {"preflight", "ensure-image", "provision", "boot-wait", "netfix",
                              "ashmem-node", "play-spoof", "connect"})
            QCOMPARE(sink.of(s), StepState::Ok);
        QVERIFY(hasCall(sp, "mirror -s"));
    }

    // bd remora-6279 — the trimmed cpufreq tree must be built BEFORE the container is created,
    // because the docker argv already names it as a bind source and docker answers a missing
    // source by creating an empty directory. An empty cpufreq view means zero CPU clusters, which
    // is a state no phone presents and a fresh way to break the very apps this protects.
    void bareHostPrereqsBuildsTheCpufreqTreeBeforeProvision() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        QVERIFY2(!ctx.rc.cpufreqDir.isEmpty(), "default-on must give the context a directory");
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("host-prereqs"), StepState::Ok);
        int built = -1, ran = -1;
        for (int i = 0; i < sp.calls.size(); ++i) {
            if (built < 0 && sp.calls.at(i).contains(QLatin1String("cpuinfo_max_freq"))) built = i;
            if (ran < 0 && sp.calls.at(i).startsWith(QLatin1String("docker run"))) ran = i;
        }
        QVERIFY2(built >= 0, "the cpufreq tree was never generated");
        QVERIFY2(ran < 0 || built < ran, "the tree must exist before `docker run` mounts it");
        // and the container is told to mount it
        QVERIFY(hasCall(sp, "/sys/devices/system/cpu/cpufreq:ro"));
    }

    // OFF must be total: no directory, no generation, no bind. The knob exists for measuring the
    // host's own cpufreq from inside the guest, which a trimmed view cannot answer.
    void cpufreqTopologyOffGeneratesNothing() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.input.cpufreqTopology = false;
        auto ctx = makeContext(cfg, Backend::Bare);
        QVERIFY(ctx.rc.cpufreqDir.isEmpty());
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        for (const QString &c : sp.calls)
            QVERIFY2(!c.contains(QLatin1String("cpu/cpufreq")), qPrintable(c));
    }

    // bd remora-4ei.67 — a container whose IMAGE has been superseded must be RECREATED, not
    // started. Rebuilding a tag leaves the existing container pinned to the old image id, and the
    // smart-run gate upstream only inspects a RUNNING container, so a stopped one reached
    // provision unchecked and `docker start` silently ran the previous build. After a multi-hour
    // source build that presents as "the rebuild did nothing" — which is exactly how it was found.
    // bd remora-u9m. Android's netd uses LEGACY iptables; a modern host uses the nft backend and
    // never loads the legacy table modules, so every table netd sets up fails and it RETRIES —
    // 4.24s of a ~13s boot, measured. Not fatal, so it must be a NOTE on a passing step, never a
    // failure; but it must be said, because nothing else in the chain would ever mention it and
    // the symptom is simply "the boot is slow".
    void bareHostPrereqsWarnsWhenLegacyNetfilterTablesAreMissing() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;  // no ip[6]_tables_names -> the registries read empty
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("host-prereqs"), StepState::Ok);  // a note, not a failure
        const QString all = sink.details.join(QLatin1Char('\n'));
        QVERIFY2(all.contains(QStringLiteral("SLOW BOOT")), qPrintable(all));
        // names the modules AND how to make it stick, or the operator has to go looking
        QVERIFY2(all.contains(QStringLiteral("ip6table_mangle")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("modules-load.d")), qPrintable(all));
    }

    // …and stays quiet when the kernel does offer them, so it cannot become background noise.
    void bareHostPrereqsSilentWhenNetfilterTablesArePresent() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.legacyNetfilterTables = true;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QVERIFY(!sink.details.join(QLatin1Char('\n')).contains(QStringLiteral("SLOW BOOT")));
    }

    // bd remora-v8h follow-up. The post-boot module work is keyed on "is this a new framework
    // generation", and keying that on system_server's pid ALONE is wrong: every container gets a
    // fresh PID namespace where pids restart low and the boot is deterministic, so a recreated
    // container hands system_server the same pid remarkably often (caught live: stored 540,
    // current 540, different containers). The consequence is silent — play-spoof reports "already
    // injected", the modules never run their post-boot half, and the device is left unspoofed,
    // reporting the build's own ro.product.model. The marker therefore lives in /dev, a tmpfs that dies
    // with the container.
    void spoofGenerationMarkerLivesOnTmpfsNotPersistentData() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.advanced.playSpoof = true;
        auto ctx = makeContext(cfg, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        QVERIFY2(calls.contains(QStringLiteral("/dev/.remora-spoof-gen")), "marker must be on tmpfs");
        QVERIFY2(!calls.contains(QStringLiteral("/data/adb/.remora-spoof-gen")),
                 "a marker under /data survives the container it describes");
    }

    // Magisk disables EVERY module at once when its bootloop protection trips, and a run of failed
    // deploys is enough to trip it (observed: four modules with one identical disable-file mtime,
    // to the nanosecond). playSpoof's loop skips disabled modules, so the post-boot half silently
    // stops running and the device is left unspoofed with every step green. install.sh clears them
    // but only runs when module CONTENT drifted, which it had not — so nothing healed it.
    void staleDisableMarkersAreClearedEveryDeploy() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.advanced.playSpoof = true;
        auto ctx = makeContext(cfg, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        // unconditional, not gated on the drift check that skipped it
        QVERIFY2(calls.contains(QStringLiteral("rm -f /data/adb/modules/$m/disable")),
                 qPrintable(QStringLiteral("no disable-marker sweep in the chain")));
        for (const char *m : {"rezygisk", "playintegrityfix", "remora_devicespoof"})
            QVERIFY2(calls.contains(QLatin1String(m)), m);
    }

    void bareProvisionRecreatesWhenImageSuperseded() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.startOk = true;         // an existing container that `docker start` would happily run
        sp.containerStale = true;  // …but its image id != the tag's
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("provision"), StepState::Ok);
        QVERIFY(hasCall(sp, "rm"));          // the stale container is removed…
        QVERIFY(hasCall(sp, "docker run"));  // …and rebuilt from the current tag
    }

    // The converse: a current image must still take the cheap start path, or every deploy would
    // pay a full container recreate.
    void bareProvisionStartsExistingWhenImageCurrent() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.startOk = true;
        sp.containerStale = false;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QVERIFY(hasCall(sp, "docker start"));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // bd remora-4ei.76 — the boot-id ashmem node must be recreated on every container recreate,
    // and must never fail the deploy. A17's libcutils derives the path as /dev/ashmem<boot_id>, so
    // a container offering only the plain node makes every WebView renderer die and composite
    // black; the link lives in the container's own /dev, so it does not survive a recreate.
    void bareDeployLinksBootIdAshmemNode() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("ashmem-node"), StepState::Ok);
        QVERIFY(hasCall(sp, "boot_id"));   // derived, never hardcoded — it changes every boot
        QVERIFY(hasCall(sp, "/dev/ashmem"));
    }

    // bd remora-oky7 — the host module is asked of the RELEASE. An Android 17 profile deploys on a
    // host with no ashmem_linux at all (it runs on memfd); an Android 16 one is refused in
    // preflight, before anything is created, because its system_server crash-loops without it.
    void barePreflightAsksAshmemOnlyOfAndroid16() {
        SKIP_ON_WINDOWS(kBareDeploy);
        {
            FakeSpawner sp;
            sp.ashmem = false;
            RemoraConfig c;
            c.image.androidVersion = 17;
            auto ctx = makeContext(c, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("preflight"), StepState::Ok);
            QVERIFY(!hasCall(sp, "lsmod | grep -q ashmem"));  // not even asked
        }
        {
            FakeSpawner sp;
            sp.ashmem = false;
            RemoraConfig c;
            c.image.androidVersion = 16;
            auto ctx = makeContext(c, Backend::Bare);
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            QVERIFY(!hasCall(sp, "docker run"));
        }
    }

    // A container with no ashmem at all must still deploy: an Android 17 image runs on memfd, and
    // an Android 16 one never gets this far without the module.
    void bareDeploySurvivesMissingAshmem() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.ashmemNodeMissing = true;  // the in-container probe reports NO_ASHMEM
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("ashmem-node"), StepState::Ok);  // reported, not fatal
    }

    // A profile that pins no container_name gets one named after ITSELF, not the generic
    // per-backend fallback. Two profiles that both said nothing used to resolve to the same
    // "remora-bm" and so to the same container, one silently adopting the other's instance.
    void unpinnedContainerNameDerivesFromProfileName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/remorarc");
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-Android 17"));
            s.setValue(QStringLiteral("android_version"), 17);  // pins no container_name
            s.endGroup();
            s.beginGroup(QStringLiteral("Instance-Android 16"));
            s.setValue(QStringLiteral("android_version"), 16);
            s.endGroup();
            s.sync();
        }
        const RemoraConfig a17 = loadInstance(path, QStringLiteral("Android 17"));
        const RemoraConfig a16 = loadInstance(path, QStringLiteral("Android 16"));
        QCOMPARE(a17.backend.containerName,
                 std::optional<QString>(QStringLiteral("remora-android-17")));
        // The point of the change: two unpinned profiles no longer collide on one container.
        QVERIFY(a17.backend.containerName != a16.backend.containerName);
        // It must survive into the resolved config, not just the parsed one.
        QCOMPARE(resolve(a17, Backend::Bare).containerName,
                 QStringLiteral("remora-android-17"));

        // An EXPLICIT name is never overridden: the container name is how Remora finds a running
        // instance, and paths derived from it are fixed at container creation.
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-Android 17"));
            s.setValue(QStringLiteral("container_name"), QStringLiteral("remora-legacy"));
            s.endGroup();
            s.sync();
        }
        QCOMPARE(loadInstance(path, QStringLiteral("Android 17")).backend.containerName,
                 std::optional<QString>(QStringLiteral("remora-legacy")));
    }

    // bd remora-4ei.36, bare-mode. Sharing is one expose pass (the deploy must SAY which devices
    // are usable and why the rest are not) plus a detached replug watcher. The pkill of the
    // previous watcher is unconditional — the device list may have changed to EMPTY, and a stale
    // watcher would keep re-exposing devices the operator just unticked — but the spawn is not.
    void sharedInputsExposeAndWatch() {
        SKIP_ON_WINDOWS(kBareDeploy);
        // 1. Two devices, one grabbed: both named in the detail; the watcher goes through
        //    spawnDetached (scoped, so it survives Remora exiting) with the same names.
        {
            FakeSpawner sp;
            sp.inputShareOut = QStringLiteral(
                "SHARED event7 Keychron K8 Pro Keyboard\nGRABBED SteelSeries Rival 3\n");
            RemoraConfig c;
            c.input.sharedInputs = QStringList{QStringLiteral("Keychron K8 Pro Keyboard"),
                                               QStringLiteral("SteelSeries Rival 3")};
            auto ctx = makeContext(c, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("shared-inputs"), StepState::Ok);
            QVERIFY(!sink.details.filter(QStringLiteral("sharing Keychron K8 Pro Keyboard")).isEmpty());
            QVERIFY(!sink.details.filter(QStringLiteral("grabbed by another process")).isEmpty());
            QVERIFY(!sp.calls.filter(QStringLiteral("DETACH python3"))
                         .filter(QStringLiteral("watch"))
                         .filter(QStringLiteral("SteelSeries Rival 3"))
                         .isEmpty());
        }
        // 2. Nothing shared: the stale watcher is still killed, and nothing runs or spawns.
        {
            FakeSpawner sp;
            auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("shared-inputs"), StepState::Ok);
            QVERIFY(!sp.calls.filter(QStringLiteral("pkill -f \"remora-input-share.py watch"))
                         .isEmpty());
            QVERIFY(sp.calls.filter(QStringLiteral("remora-input-share.py expose")).isEmpty());
            QVERIFY(sp.calls.filter(QStringLiteral("DETACH python3")).isEmpty());
        }
        // 3. Remote: the pass runs on the docker host from the staged ~/.remora copy — the
        //    devices being shared live on THAT kernel — with multi-word names double-quoted into
        //    the one ssh payload, and the watcher survives via nohup there, never spawnDetached
        //    here.
        {
            FakeSpawner sp;
            sp.inputShareOut = QStringLiteral("SHARED event7 Keychron K8 Pro Keyboard\n");
            RemoraConfig c = withGuest();
            c.input.sharedInputs = QStringList{QStringLiteral("Keychron K8 Pro Keyboard")};
            auto ctx = makeContext(c, Backend::Remote);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("shared-inputs"), StepState::Ok);
            QVERIFY(!sp.calls.filter(QStringLiteral(".remora/remora-input-share.py"))
                         .filter(QStringLiteral("\"Keychron K8 Pro Keyboard\""))
                         .isEmpty());
            QVERIFY(!sp.calls.filter(QStringLiteral("nohup"))
                         .filter(QStringLiteral("watch"))
                         .isEmpty());
            QVERIFY(sp.calls.filter(QStringLiteral("DETACH python3")).isEmpty());
        }
        // 4. Reconnect runs the step too — the container is up, so no deploy chain ever will,
        //    and a reconnect is exactly when a share was just ticked in the GUI. Found missing
        //    by the first real `up` over a running container.
        {
            FakeSpawner sp;
            sp.inputShareOut = QStringLiteral("SHARED event7 Keychron K8 Pro Keyboard\n");
            RemoraConfig c;
            c.input.sharedInputs = QStringList{QStringLiteral("Keychron K8 Pro Keyboard")};
            RecordingSink sink;
            QVERIFY(reconnectRun(Backend::Bare, c, sink, sp));
            QCOMPARE(sink.of("shared-inputs"), StepState::Ok);
            QVERIFY(!sp.calls.filter(QStringLiteral("DETACH python3"))
                         .filter(QStringLiteral("watch"))
                         .isEmpty());
        }
    }

    // bd remora-4ei.9 sibling. Falling back to a GENERIC keyboard identity is not cosmetic: Gboard
    // gates its physical-keyboard toolbar on a keyboard that looks real, which is the only reason
    // this step impersonates one. Two deploys an hour apart once differed by nothing but the word
    // "generic" — a USB hub carrying the operator's keyboard had been unplugged — so the three
    // outcomes must be distinguishable in the step's own detail.
    void phantomKeyboardNamesAMissingIdentity() {
        SKIP_ON_WINDOWS(kBareDeploy);
        const QString devices = fx(QStringLiteral("proc_input_devices.txt"));
        QVERIFY(!devices.isEmpty());

        // 1. The real keyboard IS attached: named, and no warning.
        {
            FakeSpawner sp;
            sp.inputDevicesOut = devices;
            auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("phantom-kbd"), StepState::Ok);
            QVERIFY(!sink.details.filter(QStringLiteral("Keychron K8 Pro Keyboard")).isEmpty());
            QVERIFY(sink.details.filter(QStringLiteral("Gboard will NOT")).isEmpty());
        }
        // 2. AUTO and nothing attached: the fallback is NAMED, with the consequence.
        {
            FakeSpawner sp;  // inputDevicesOut empty = no keyboard on the host
            auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("phantom-kbd"), StepState::Ok);  // named, never fatal
            QVERIFY(!sink.details.filter(QStringLiteral("no real keyboard is attached")).isEmpty());
            QVERIFY(!sink.details.filter(QStringLiteral("Gboard will NOT")).isEmpty());
        }
        // 3. A NAMED keyboard that is not among the attached ones: says which, and how many are.
        {
            FakeSpawner sp;
            sp.inputDevicesOut = devices;  // has the Keychron, not this
            RemoraConfig c;
            c.input.kbdIdentity = QStringLiteral("Some Other Keyboard");
            auto ctx = makeContext(c, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QVERIFY(!sink.details.filter(QStringLiteral("Some Other Keyboard")).isEmpty());
            QVERIFY(!sink.details.filter(QStringLiteral("is not attached"))
                         .isEmpty());
        }
        // 4. "none" is the picker's deliberate Generic choice and must stay QUIET.
        {
            FakeSpawner sp;
            RemoraConfig c;
            c.input.kbdIdentity = QStringLiteral("none");
            auto ctx = makeContext(c, Backend::Bare);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
            QVERIFY(sink.details.filter(QStringLiteral("Gboard will NOT")).isEmpty());
        }
    }

    // bd remora-4ei.9 — a passed-through /dev/videoN arrives owning the docker HOST's video gid,
    // which means nothing in Android: the external camera provider runs as cameraserver (gid 1006,
    // AID_CAMERA) and cannot open the node, so it enumerates zero cameras. This fix-up existed ONLY
    // in the vm bringup script and died with that backend, leaving bare and remote passing the node
    // in with nothing to make it usable. Both surviving backends must hand it over.
    void deployHandsCameraNodesToCameraserver() {
        for (const Backend b : deployTargets()) {
            // Remote has no baked ssh host, so it must be supplied or preflight refuses.
            RemoraConfig c = (b == Backend::Remote) ? withGuest() : RemoraConfig{};
            c.input.cameraDevices =
                QStringList{QStringLiteral("/dev/video0"), QStringLiteral("/dev/video2")};
            FakeSpawner sp;
            auto ctx = makeContext(c, b);
            RecordingSink sink;
            QVERIFY(runChain(makeDeployer(b, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("camera-nodes"), StepState::Ok);
            // 1006 is the whole point: 0:1006 + 0660 is what cameraserver can open.
            QVERIFY(hasCall(sp, "chown 0:1006"));
            // Only the nodes this profile passed — never a blanket /dev/video*, which would reach
            // outside what the operator handed us and could touch another container's device.
            QVERIFY(hasCall(sp, "/dev/video0"));
            QVERIFY(hasCall(sp, "/dev/video2"));
            QVERIFY(!hasCall(sp, "/dev/video*"));
            // …AND the provider is restarted, which is what actually makes the camera appear. The
            // chown alone is too late: the provider starts at `class hal`, long before this step,
            // scans /dev once, fails to open a node it has no group for, and afterwards reacts
            // only to inotify CREATE/DELETE — which a chown does not fire. Measured live: 0
            // cameras at boot with the node correctly owned, 1 after a restart.
            QVERIFY2(hasCall(sp, "ctl.restart vendor.camera.provider-ext"),
                     "the node was chowned but nothing told the HAL to look again");
        }
    }

    // No cameras passed = no exec at all, and certainly no failure: the overwhelmingly common
    // profile carries no camera feature and must not pay for one.
    void deployWithoutCamerasIssuesNoCameraFixup() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("camera-nodes"), StepState::Ok);  // reported, trivially
        QVERIFY(!hasCall(sp, "chown 0:1006"));
        // …and no HAL is restarted either: a profile with no camera must not pay a service bounce
        // for a provider it never uses.
        QVERIFY(!hasCall(sp, "ctl.restart vendor.camera.provider-ext"));
        // ...and it must NOT claim a camera went missing, because none was asked for.
        QVERIFY(sink.details.filter(QStringLiteral("camera_v4l2 is ON")).isEmpty());
    }

    // A profile that ASKED for cameras and got none must say so. Observed: a USB hub
    // carrying the webcam was unplugged between two deploys, and the only difference in the output
    // was "2 camera node(s) handed to cameraserver" becoming "no cameras passed through — nothing
    // to do" — which reads like the profile never wanted one. Requested-and-absent is exactly the
    // case worth hearing about, since Android will enumerate zero cameras.
    void deployNamesCamerasRequestedButAbsent() {
        SKIP_ON_WINDOWS(kBareDeploy);
        RemoraConfig c;
        c.image.features = QStringList{QStringLiteral("camera_v4l2")};
        // No caps => the live probe found nothing, which is the unplugged-camera case.
        const ResolvedConfig rc = resolve(c, Backend::Bare);
        QVERIFY(rc.cameraDevices.isEmpty());
        QVERIFY(rc.cameraWantedButNoneFound);

        FakeSpawner sp;
        auto ctx = makeContext(c, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("camera-nodes"), StepState::Ok);  // named, still not fatal
        QVERIFY(!sink.details.filter(QStringLiteral("camera_v4l2 is ON")).isEmpty());
        QVERIFY(!hasCall(sp, "chown 0:1006"));

        // An EXPLICITLY empty camera_devices= is deliberate "off" and must stay quiet.
        RemoraConfig off = c;
        off.input.cameraDevices = QStringList{};
        QVERIFY(!resolve(off, Backend::Bare).cameraWantedButNoneFound);
    }

    // bd remora-4ei.78 — play_spoof was implemented only in the vm deployer, so a bare profile
    // with it enabled silently got nothing: the device stayed uncertified and Google sign-in
    // failed with "Something went wrong". Off by default, it must stay a pure no-op.
    void bareDeploySkipsPlaySpoofWhenOff() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);  // playSpoof defaults false
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("play-spoof"), StepState::Ok);
        QVERIFY(!hasCall(sp, "remora-spoof"));  // the image is not even probed
        QVERIFY(!hasCall(sp, "zygisk-ptrace64"));
    }

    // Enabled, it must probe the image and reach the injector — and it must NOT bounce the
    // framework when something is already injected, or every reconnect would restart Android.
    void bareDeployRunsPlaySpoofWhenOn() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.advanced.playSpoof = true;
        auto ctx = makeContext(cfg, Backend::Bare);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("play-spoof"), StepState::Ok);
        QVERIFY(hasCall(sp, "/system/etc/remora-spoof"));  // image gate consulted
    }

    // The steps both deployers share act INSIDE the container, so on remote they have to reach the
    // docker host over ssh. They used to run a plain `docker exec`, which aimed a remote deploy at
    // THIS machine's docker: the remote container's ashmem link, camera and audio nodes and Play
    // spoof were never touched, and play_spoof reported an image without the feature
    // (bd remora-7oih). Matching substrings alone could not see it — the command was there, on
    // the wrong machine — so this pins where each one runs.
    void remoteDeployRunsContainerStepsOnTheDockerHost() {
        FakeSpawner sp;
        RemoraConfig cfg = withGuest();
        cfg.advanced.playSpoof = true;
        cfg.input.cameraDevices = QStringList{QStringLiteral("/dev/video0")};
        auto ctx = makeContext(cfg, Backend::Remote);
        ctx.rc.usbAudio = true;
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        for (const char *step : {"ashmem-node", "camera-nodes", "audio-nodes", "spoof-arm",
                                 "play-spoof"})
            QCOMPARE(sink.of(QString::fromLatin1(step)), StepState::Ok);
        for (const QString &c : sp.calls)
            QVERIFY2(!c.startsWith(QLatin1String("docker exec")), qPrintable(c));
        for (const char *sub : {"/dev/ashmem", "chown 0:1006", "chown 0:1005",
                                "/system/etc/remora-spoof"}) {
            bool overSsh = false;
            for (const QString &c : sp.calls)
                if (c.startsWith(QLatin1String("ssh ")) && c.contains(QLatin1String(sub)))
                    overSsh = true;
            QVERIFY2(overSsh, sub);
        }
    }

    void markedMagiskImageKeepsItsModulesAndInjector() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.bootManagedZygisk = true;
        sp.spoofModulesDrifted = true;
        RemoraConfig cfg;
        cfg.advanced.playSpoof = true;
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(makeContext(cfg, Backend::Bare)),
                         sink));
        QVERIFY(hasCall(sp, "pgrep -x zygiskd64"));
        QVERIFY(!hasCall(sp, "remora-spoof/install.sh"));
        QVERIFY(!hasCall(sp, "zygisk-ptrace64 monitor"));
        QVERIFY(!hasCall(sp, "post-fs-data.d/50-rezygisk-pif.sh"));
    }

    void markedMagiskImageRequiresRootAndNativeInjection() {
        SKIP_ON_WINDOWS(kBareDeploy);
        for (bool rootMissing : {false, true}) {
            FakeSpawner sp;
            sp.bootManagedZygisk = true;
            sp.nativeZygiskReady = false;
            if (rootMissing) sp.rootProbeGap = QStringLiteral("NO_DAEMON");
            RemoraConfig cfg;
            cfg.advanced.playSpoof = true;
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(makeContext(cfg, Backend::Bare)),
                              sink));
            QCOMPARE(sink.of("play-spoof"), StepState::Failed);
            QVERIFY(!hasCall(sp, "zygisk-ptrace64 monitor"));
            QVERIFY(!hasCall(sp, "remora-spoof/install.sh"));
        }
    }

    // bd remora-4ei.86. /data is a persistent volume, so a module installed once stays installed:
    // the gate that re-installs on drift is the ONLY thing that ever delivers a new payload to an
    // existing device. It compared post-fs-data.sh alone, and the ReZygisk 515 -> 537 bump moved the
    // BINARIES and module.prop and nothing else — every support script byte-identical across it — so
    // the gate saw nothing, /data kept 515, and this host ran a container off an image carrying 537
    // with 515 actually installed. Measured live: /data/adb/modules/rezygisk/module.prop read
    // "515-333d423-release" while the image's read 537, with no step reporting anything.
    //
    // Pinned as script TEXT because the failure is a missing comparison, not a wrong verdict: the
    // gate returned the right answer for what it looked at, and no behavioural test can catch a
    // witness that was never consulted.
    void spoofDriftGateWatchesVersionAndBinariesNotJustScripts() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.advanced.playSpoof = true;
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(makeContext(cfg, Backend::Bare)),
                         sink));
        QString drift;
        for (const QString &c : sp.calls)
            if (c.contains(QLatin1String("remora-spoof/*/"))) drift = c;
        QVERIFY(!drift.isEmpty());
        // versionCode: what an upstream bump always moves, and the one field Magisk does not
        // rewrite on the /data side (it rewrites description, which is why the file is not compared
        // whole).
        QVERIFY2(drift.contains(QLatin1String("versionCode=")), qPrintable(drift));
        // bin/: our own injector is patched at assemble time, so re-deriving the patch changes the
        // bytes under an UNCHANGED versionCode. Version alone would miss that.
        QVERIFY2(drift.contains(QLatin1String("bin")), qPrintable(drift));
        // ...and the original witness stays, for drift inside one version.
        QVERIFY(drift.contains(QLatin1String("post-fs-data.sh")));
    }

    // The verdict half: drift must actually re-run install.sh, and a matching /data must not — a
    // gate that re-installed every deploy would rm -rf the module dir on every up.
    void spoofDriftReinstallsOnlyWhenSomethingMoved() {
        SKIP_ON_WINDOWS(kBareDeploy);
        for (bool drifted : {true, false}) {
            FakeSpawner sp;
            sp.spoofModulesDrifted = drifted;
            RemoraConfig cfg;
            cfg.advanced.playSpoof = true;
            RecordingSink sink;
            QVERIFY(runChain(
                makeDeployer(Backend::Bare, sp, 24, 0)->steps(makeContext(cfg, Backend::Bare)),
                sink));
            QCOMPARE(hasCall(sp, "remora-spoof/install.sh"), drifted);
        }
    }

    // Root can fail wholesale while every other signal stays green — the upstream-Magisk swap did
    // exactly that (/sbin never created: no magiskd, no su, no denylist) and cost a session of
    // misdiagnosis, because ReZygisk is injected by Remora itself and still reported success. The
    // deploy must fail LOUDLY instead, before any module work runs (bd remora-82c.13).
    void bareDeployFailsLoudlyWhenRootDidNotComeUp() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.rootProbeGap = QStringLiteral("NO_SBIN");
        RemoraConfig cfg;
        cfg.advanced.playSpoof = true;
        auto ctx = makeContext(cfg, Backend::Bare);
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("play-spoof"), StepState::Failed);
        const QString details = sink.details.join(QLatin1Char('\n'));
        QVERIFY(details.contains(QStringLiteral("root did not come up")));
        // The message must carry the recovery, not just the verdict — the whole point is that
        // nothing else in the chain names the cause.
        QVERIFY(details.contains(QStringLiteral("PROVENANCE")));
        // And the module work must not have run against a rootless device.
        QVERIFY(!hasCall(sp, "post-fs-data.d/50-rezygisk-pif.sh"));
    }

    // A profile that asks for no spoof promises nothing about root, so the gate must not run —
    // a rootless image with play_spoof off deploys exactly as before.
    void bareDeploySkipsRootGateWhenSpoofOff() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.rootProbeGap = QStringLiteral("NO_SBIN");
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);  // playSpoof defaults false
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("play-spoof"), StepState::Ok);
        QVERIFY(!hasCall(sp, "pgrep -x magiskd"));
    }

    void bareIsRefusedFirstOutsideLinux_data() {
        QTest::addColumn<bool>("windows");
        QTest::newRow("Windows") << true;
        QTest::newRow("FreeBSD") << false;
    }

    void bareIsRefusedFirstOutsideLinux() {
        QFETCH(bool, windows);
        FakeSpawner sp;
        sp.os = windows ? LocalOs::Windows : LocalOs::FreeBSD;
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)
                              ->steps(makeContext(RemoraConfig{}, Backend::Bare)),
                          sink));
        QCOMPARE(sink.of("preflight"), StepState::Failed);
        QCOMPARE(sink.of("provision"), StepState::Skipped);
        QVERIFY2(sink.details.join(QLatin1Char(' ')).contains(bareNeedsLinuxMessage()),
                 qPrintable(sink.details.join(QLatin1Char(' '))));
        QVERIFY2(sp.calls.isEmpty(), qPrintable(sp.calls.join(QLatin1Char('\n'))));
    }

    void barePreflightFailSkipsRest() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.inDocker = false;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("preflight"), StepState::Failed);
        QCOMPARE(sink.of("provision"), StepState::Skipped);
    }

    void bareImageMissingFails() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.imagePresent = false;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("ensure-image"), StepState::Failed);
    }

    // remote deliberately has NO baked ssh host (bd remora-4ei.12/.13): it used to resolve to one
    // developer's box, so on any other machine a blank field silently dialled a host that does not
    // exist and the failure read as a network problem. Preflight must name the missing field, and
    // must not spawn ssh at all — an ssh attempt against an empty host is exactly the confusing
    // failure this replaced.
    //
    // That is where this machine's docker is a local daemon. Where it is Remora's own guest
    // (Windows), a blank host is the guest (blankHostIsTheGuestWhereLocalDockerIsTheGuest).
    void unsetSshHostFailsByNameBeforeAnySpawn() {
        const ScopedLocalDockerIsGuest daemon(false);
        for (Backend b : {Backend::Remote}) {
            FakeSpawner sp;
            auto ctx = makeContext(RemoraConfig{}, b);  // no sshHost
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            QVERIFY(!sink.details.filter("no ssh host set").isEmpty());
            for (const QString &c : sp.calls)
                QVERIFY2(!c.contains(QLatin1String("ssh")),
                         qPrintable("dialled out despite no host: " + c));
        }
    }

    // bd remora-d5v. A profile that still says backend=vm must FAIL, not be quietly re-read as
    // whichever backend it now maps to. vm resolved gpu_mode=host, no overlayfs, macvlan and
    // container adb 5555; bare and remote default to the opposite of all four — so deploying it
    // "close enough" would move the container and change how it renders without a word. The
    // refusal happens in preflight, before anything is created, and names the keys to edit.
    void retiredVmProfileRefusesInPreflightBeforeCreatingAnything() {
        for (Backend b : {Backend::Bare, Backend::Remote}) {
            FakeSpawner sp;
            RemoraConfig cfg = withGuest();
            cfg.backend.retiredVmBackend = true;  // what the loader sets for backend=vm
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 24, 0)->steps(makeContext(cfg, b)), sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            const QString said = sink.details.join(QLatin1Char(' '));
            QVERIFY(said.contains(QStringLiteral("backend=vm has been removed")));
            QVERIFY(said.contains(QStringLiteral("backend=remote")));  // the remedy is actionable
            QVERIFY(!hasCall(sp, "docker run"));
            QVERIFY(!hasCall(sp, "docker start"));
        }
    }

    // The same rule for gpu_mode=auto: it parsed into a mode nothing handled and deployed as
    // SwiftShader. Refused in preflight, naming the values that do exist.
    void unrecognisedGpuModeRefusesInPreflightBeforeCreatingAnything() {
        for (const Backend b : deployTargets()) {
            FakeSpawner sp;
            RemoraConfig cfg = withGuest();
            cfg.gpu.unrecognisedMode = QStringLiteral("auto");  // what the loader sets
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 24, 0)->steps(makeContext(cfg, b)), sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            const QString said = sink.details.join(QLatin1Char(' '));
            QVERIFY(said.contains(QStringLiteral("gpu_mode=auto")));
            QVERIFY(said.contains(QStringLiteral("gpu_mode=host")));
            QVERIFY(said.contains(QStringLiteral("gpu_mode=guest")));
            QVERIFY(!hasCall(sp, "docker run"));
            QVERIFY(!hasCall(sp, "docker start"));
        }
    }

    // …and the loader is what sets that flag. A later layer naming a valid mode clears it, as
    // [Instance-x] overrides [Defaults] for every other key.
    void loaderFlagsAnUnrecognisedGpuMode() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/remorarc");
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue(QStringLiteral("Defaults/gpu_mode"), QStringLiteral("auto"));
            s.setValue(QStringLiteral("Instance-Auto/width"), 800);
            s.setValue(QStringLiteral("Instance-Host/gpu_mode"), QStringLiteral("host"));
            s.sync();
        }
        const RemoraConfig autoCfg = loadInstance(path, QStringLiteral("Auto"));
        QCOMPARE(autoCfg.gpu.unrecognisedMode, std::optional<QString>(QStringLiteral("auto")));
        QVERIFY(!autoCfg.gpu.mode.has_value());
        const RemoraConfig hostCfg = loadInstance(path, QStringLiteral("Host"));
        QVERIFY(!hostCfg.gpu.unrecognisedMode.has_value());
        QCOMPARE(hostCfg.gpu.mode, std::optional<GpuMode>(GpuMode::Host));
    }

    // bd remora-4ei.89. A data dir holding an UNLAYERED /data must not be deployed as an overlay
    // diff dir. Nothing errors if it is — the overlay creates an empty upper/, /data comes up blank,
    // Android reinitialises, app uids are reassigned and the GSF checkin identity is re-minted, so
    // the instance silently de-certifies while its real /data sits on disk one level away. That is
    // the A17 incident, and every layer of the stack reported success while it happened.
    // Refuse in preflight, before the container exists, and name both remedies.
    void flatDataDirRefusesOverlayDeployBeforeCreatingAnything() {
        for (const Backend b : deployTargets()) {
            FakeSpawner sp;
            sp.dataDirShape = QStringLiteral("flat\n");
            RemoraConfig cfg = withGuest();
            cfg.advanced.useOverlayfs = true;
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 24, 0)->steps(makeContext(cfg, b)), sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            const QString said = sink.details.join(QLatin1Char(' '));
            QVERIFY(said.contains(QStringLiteral("unlayered /data")));
            QVERIFY(said.contains(QStringLiteral("use_overlayfs=false")));  // remedy is actionable
            QVERIFY(!hasCall(sp, "docker run"));
            QVERIFY(!hasCall(sp, "docker start"));
        }
    }

    // The mirror case, and the one that proves the guard is about SHAPE and not about overlayfs
    // being bad: a dir already written as an overlay must not be bound in flat either, or Android
    // gets a /data whose whole contents are upper/ and work/.
    void layeredDataDirRefusesPlainBindDeploy() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.dataDirShape = QStringLiteral("layered\n");
        RemoraConfig cfg = withGuest();
        cfg.advanced.useOverlayfs = false;
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(makeContext(cfg, Backend::Bare)),
                          sink));
        QCOMPARE(sink.of("preflight"), StepState::Failed);
        QVERIFY(sink.details.join(QLatin1Char(' ')).contains(QStringLiteral("use_overlayfs=true")));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // bd remora-bm7.13. An image compiled for a CPU the target does not have must be refused before
    // anything is created. Not a performance question: the build emits instructions the processor
    // lacks, so processes take SIGILL and the log names libart or a HAL rather than the machine.
    // Both backends, because the hazard is DELIVERY — `docker save | ssh docker load` — and the
    // remote arm is the one that carries an image to a host nobody checked.
    static QString zen1Flags() {  // measured on both AMD hosts, trimmed to the tokens this looks at
        return QStringLiteral(
            "flags\t: fpu vme de pse tsc msr sse sse2 ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt "
            "aes xsave avx f16c rdrand lahf_lm abm sse4a 3dnowprefetch bmi1 avx2 bmi2 rdseed adx");
    }
    void alderlakeImageRefusedOnAZenHostBeforeCreatingAnything() {
        for (const Backend b : deployTargets()) {
            FakeSpawner sp;
            sp.imageArchVariant = QStringLiteral("alderlake");
            sp.hostCpuFlags = zen1Flags();
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 24, 0)->steps(makeContext(withGuest(), b)), sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            const QString said = sink.details.join(QLatin1Char(' '));
            QVERIFY(said.contains(QStringLiteral("alderlake")));
            QVERIFY(said.contains(QStringLiteral("avx_vnni")));  // names WHY, not "unsupported"
            QVERIFY(!hasCall(sp, "docker run"));
            QVERIFY(!hasCall(sp, "docker start"));
        }
    }

    // ...and the same image on the machine it was built for deploys, plus the three ways of not
    // knowing. THIS IS THE HALF THAT MATTERS MOST: bd remora-4ei.19 deleted an earlier preflight
    // after three false refusals and zero catches, so a guard that cannot be shown to stay out of
    // the way is worth less than no guard at all.
    void archVariantGuardPassesTheMatchAndEveryUnknown() {
        SKIP_ON_WINDOWS(kBareDeploy);
        struct Case { const char *what; QString variant; QString flags; };
        const QString alderLake = QStringLiteral(
            "flags\t: fpu sse sse2 fma movbe popcnt abm avx f16c bmi1 avx2 bmi2 rdseed adx "
            "serialize waitpkg movdiri movdir64b avx_vnni");
        for (const Case &c : QList<Case>{
                 {"the host it was built for", QStringLiteral("alderlake"), alderLake},
                 {"a baseline image anywhere", QStringLiteral("x86_64"), zen1Flags()},
                 {"broadwell on Zen — the pairing the variant was CHOSEN for",
                  QStringLiteral("broadwell"), zen1Flags()},
                 {"a variant this table has never been taught",
                  QStringLiteral("some-future-variant"), zen1Flags()},
                 {"an image predating the stamp", QString(), zen1Flags()},
                 {"a cpuinfo the probe could not read", QStringLiteral("alderlake"), QString()}}) {
            FakeSpawner sp;
            sp.imageArchVariant = c.variant;
            sp.hostCpuFlags = c.flags;
            RecordingSink sink;
            QVERIFY2(runChain(makeDeployer(Backend::Bare, sp, 24, 0)
                                  ->steps(makeContext(withGuest(), Backend::Bare)),
                              sink),
                     c.what);
            QCOMPARE(sink.of("preflight"), StepState::Ok);
        }
    }

    // bd remora-pzq. A "local:" companion app aimed at a REMOTE docker host is refused in
    // preflight, before anything is dialled: the staging step retired with the vm backend, so the
    // bind would name a directory on the wrong machine and docker would create it empty — an app
    // that silently does not install.
    void localCompanionAppRefusesRemoteDeploy() {
        RemoraConfig cfg = withGuest();
        cfg.integration.companionApps =
            QStringList{QStringLiteral("local:/home/u/out/RemoraTweaks")};

        FakeSpawner sp;
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Remote, sp, 24, 0)
                              ->steps(makeContext(cfg, Backend::Remote)),
                          sink));
        QCOMPARE(sink.of("preflight"), StepState::Failed);
        const QString said = sink.details.join(QLatin1Char(' '));
        QVERIFY(said.contains(QStringLiteral("RemoraTweaks")));  // names the entry
        QVERIFY(said.contains(QStringLiteral("local:")));        // and the form that caused it
        QVERIFY(!hasCall(sp, "docker run"));
        QVERIFY(!hasCall(sp, "docker start"));
    }

    // Bare keeps the form untouched: there the docker host IS this machine and the path binds in
    // place.
    void localCompanionAppBindsInPlaceOnBare() {
        SKIP_ON_WINDOWS(kBareDeploy);
        RemoraConfig cfg = withGuest();
        cfg.integration.companionApps =
            QStringList{QStringLiteral("local:/home/u/out/RemoraTweaks")};
        FakeSpawner sp;
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)
                             ->steps(makeContext(cfg, Backend::Bare)),
                         sink));
        QCOMPARE(sink.of("preflight"), StepState::Ok);
    }

    // …and the guard must stay out of the way of every legitimate deploy. A matching shape, and a
    // host that has never been deployed to, both proceed. A check that refused real work would be a
    // worse regression than the bug it prevents.
    void matchingAndFirstTimeDataShapesDeployNormally() {
        SKIP_ON_WINDOWS(kBareDeploy);
        for (const auto &c : QList<QPair<QString, bool>>{{"layered", true},
                                                         {"flat", false},
                                                         {"missing", true},
                                                         {"missing", false},
                                                         {"empty", true},
                                                         {"empty", false}}) {
            FakeSpawner sp;
            sp.dataDirShape = c.first + QStringLiteral("\n");
            RemoraConfig cfg = withGuest();
            cfg.advanced.useOverlayfs = c.second;
            RecordingSink sink;
            QVERIFY2(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(makeContext(cfg, Backend::Bare)),
                              sink),
                     qPrintable(QStringLiteral("refused a legitimate deploy: shape=%1 overlay=%2")
                                    .arg(c.first, c.second ? "true" : "false")));
            QCOMPARE(sink.of("preflight"), StepState::Ok);
        }
    }

    // The Reddit session cookie is a credential, and a command line is readable by every user on
    // the host for as long as the process runs. It used to ride in argv twice — base64'd in a
    // `docker exec … echo <b64> | base64 -d` and in plain text in the sqlite3 INSERT. It must reach
    // the device through files and stdin only, on both backends, and the remote side must stage in
    // a private mktemp name rather than a fixed path in a shared /tmp.
    void redditSessionCookieNeverInArgv() {
        const QString cookie = QStringLiteral("eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiJ0Ml9hYmMifQ.c2VjcmV0");
        for (const Backend b : {Backend::Bare, Backend::Remote}) {
            RemoraConfig cfg;
            if (b == Backend::Remote) cfg.backend.sshHost = QStringLiteral("me@dockerhost");
            RedditFakeSpawner sp;
            sp.pulledDb = RedditFakeSpawner::accountDb(true);
            QVERIFY(injectRedditSession(sp, makeContext(cfg, b), QStringLiteral("someone"), cookie, {}));
            for (const QStringList &argv : sp.calls) {
                const QString cmd = argv.join(QLatin1Char(' '));
                QVERIFY2(!cmd.contains(cookie), qPrintable(cmd));
                QVERIFY2(!cmd.contains(QLatin1String("base64")), qPrintable(cmd));
                QVERIFY2(!cmd.contains(QLatin1String("/tmp/remora-rl-")), qPrintable(cmd));
                // Qt's SQLite edits the pulled databases (bd remora-7oih.3): no sqlite3 binary,
                // which a Windows client does not have, on either backend.
                QVERIFY2(argv.value(0) != QLatin1String("sqlite3"), qPrintable(cmd));
            }
            // The credential database that went back carries the account, at the next free id,
            // with the cookie as its extra — and the account that was already there untouched.
            bool found = false;
            for (const QByteArray &bytes : sp.pushed) {
                QTemporaryDir d;
                const QString path = d.filePath(QStringLiteral("ce.db"));
                QFile f(path);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(bytes);
                f.close();
                {
                    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
                    db.setDatabaseName(path);
                    QVERIFY(db.open());
                    QSqlQuery q(db);
                    if (q.exec(QStringLiteral("SELECT a._id, x.value FROM accounts a JOIN extras x "
                                              "ON x.accounts_id=a._id WHERE a.name='someone'")) &&
                        q.next()) {
                        found = true;
                        QCOMPARE(q.value(0).toInt(), 2);
                        QVERIFY(q.value(1).toString().contains(cookie));
                        QSqlQuery o(db);
                        QVERIFY(o.exec(QStringLiteral("SELECT count(*) FROM accounts WHERE name='other'")) && o.next());
                        QCOMPARE(o.value(0).toInt(), 1);
                    }
                    db.close();
                }
                QSqlDatabase::removeDatabase(QStringLiteral("check"));
            }
            QVERIFY2(found, "no pushed accounts_ce.db carries the account");
        }
    }

    // Owner-only, file and directory: a checkin can be made in this device's name with it.
    void checkinSnapshotIsOwnerOnly() {
        SKIP_ON_WINDOWS("POSIX mode bits, which Windows does not have");
        FakeSpawner sp;
        sp.checkinScanOut = QStringLiteral("3648232585666691482\n1\n2914131252766307936\n");
        const QString inst = QStringLiteral("Android 17");
        QFile::remove(checkinSnapshotPath(inst));
        probePlayCertification(sp, Backend::Bare, QStringLiteral("c"), QString(), false, inst);
        QVERIFY(loadCheckinSnapshot(inst).has_value());
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(QFileInfo(checkinSnapshotPath(inst)).permissions() & others,
                 QFileDevice::Permissions());
        QCOMPARE(QFileInfo(QFileInfo(checkinSnapshotPath(inst)).absolutePath()).permissions() &
                     others,
                 QFileDevice::Permissions());
        QFile::remove(checkinSnapshotPath(inst));
    }

    // bd remora-4ei.84. Reading the identity is the ONLY moment Remora ever sees it, so it is the
    // only moment it can keep a copy. A probe that saw a healthy pair and did not save it is the
    // whole bug: after a /data rebuild there is nothing left to restore from.
    void certificationProbeSnapshotsTheIdentityPair() {
        FakeSpawner sp;
        sp.checkinScanOut = QStringLiteral("3648232585666691482\n1\n2914131252766307936\n");
        const QString inst = QStringLiteral("Android 17");
        QFile::remove(checkinSnapshotPath(inst));

        // No instance named: no snapshot. probeStatus must be able to read without saving.
        probePlayCertification(sp, Backend::Bare, QStringLiteral("c"), QString(), false, QString());
        QVERIFY(!loadCheckinSnapshot(inst).has_value());

        const PlayCertification c = probePlayCertification(sp, Backend::Bare, QStringLiteral("c"),
                                                           QString(), false, inst);
        QCOMPARE(*c.androidId, QStringLiteral("3648232585666691482"));
        QCOMPARE(*c.securityToken, QStringLiteral("2914131252766307936"));
        QCOMPARE(c.identityPair(),
                 QStringLiteral("3648232585666691482:2914131252766307936"));
        QCOMPARE(loadCheckinSnapshot(inst).value(), c.identityPair());

        // An UNCERTIFIED identity is snapshotted too — it is still this instance's, the user may be
        // about to register it, and losing it between reading and registering is the sequence that
        // costs a session.
        FakeSpawner sp2;
        sp2.checkinScanOut = QStringLiteral("4554079615384372578\n0\n2857179371199824165\n");
        probePlayCertification(sp2, Backend::Bare, QStringLiteral("c"), QString(), false, inst);
        QCOMPARE(loadCheckinSnapshot(inst).value(),
                 QStringLiteral("4554079615384372578:2857179371199824165"));

        // A device that has never checked in has no pair to keep, and must not truncate the one
        // already saved.
        FakeSpawner sp3;
        sp3.checkinScanOut = QStringLiteral("\n1\n\n");
        probePlayCertification(sp3, Backend::Bare, QStringLiteral("c"), QString(), false, inst);
        QCOMPARE(loadCheckinSnapshot(inst).value(),
                 QStringLiteral("4554079615384372578:2857179371199824165"));
        QFile::remove(checkinSnapshotPath(inst));
    }

    // Two profiles must never share a snapshot file, and a hand-mangled one must not reach a device.
    void checkinSnapshotIsPerInstanceAndValidated() {
        QVERIFY(checkinSnapshotPath(QStringLiteral("Android 17")) !=
                checkinSnapshotPath(QStringLiteral("Android-17")));
        QVERIFY(checkinSnapshotPath(QString()).isEmpty());

        const QString inst = QStringLiteral("Validate me");
        for (const QString &bad : {QStringLiteral("3648232585666691482"),  // id alone — the
                                                                          // half-restore to fear
                                   QStringLiteral(":2914131252766307936"),
                                   QStringLiteral("abc:def"), QStringLiteral("")}) {
            QDir().mkpath(QFileInfo(checkinSnapshotPath(inst)).absolutePath());
            QFile f(checkinSnapshotPath(inst));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(bad.toUtf8());
            f.close();
            QVERIFY2(!loadCheckinSnapshot(inst).has_value(), qPrintable("accepted: " + bad));
        }
        QVERIFY(saveCheckinSnapshot(inst, QStringLiteral("1:2")));
        QCOMPARE(loadCheckinSnapshot(inst).value(), QStringLiteral("1:2"));
        QFile::remove(checkinSnapshotPath(inst));
    }

    // The restore itself. The ORDER is the load-bearing part: a live GMS rewrites the identity files
    // from memory, so a write that lands before the force-stop can be silently undone — which is
    // exactly what made the first attempt at this uninterpretable.
    void restoreWritesThePairOnlyAfterForceStopping() {
        const QString inst = QStringLiteral("Restore me");
        QFile::remove(checkinSnapshotPath(inst));
        FakeSpawner none;
        // Nothing saved yet: not attempted, and it must say so rather than half-acting.
        const CheckinRestore r0 =
            restoreCheckinIdentity(none, Backend::Bare, QStringLiteral("c"), QString(), inst);
        QVERIFY(!r0.attempted);
        QVERIFY(!r0.ok);
        QVERIFY(!hasCall(none, "force-stop"));

        QVERIFY(saveCheckinSnapshot(inst, QStringLiteral("111:222")));
        FakeSpawner sp;
        const CheckinRestore r =
            restoreCheckinIdentity(sp, Backend::Bare, QStringLiteral("c"), QString(), inst);
        QVERIFY(r.attempted);
        QVERIFY(r.ok);
        const QString script = sp.calls.filter(QStringLiteral("force-stop")).join(QLatin1Char(' '));
        QVERIFY(!script.isEmpty());
        QVERIFY(script.contains(QStringLiteral("111:222")));
        QVERIFY(script.indexOf(QStringLiteral("force-stop")) <
                script.indexOf(QStringLiteral("printf")));
        // Every cache is dropped so they re-seed from the authoritative file — gservices.db too,
        // because a stale android_id there would be the one copy still disagreeing.
        QVERIFY(script.contains(QStringLiteral("gservices.db")));
        QVERIFY(script.contains(QStringLiteral("chmod 600")));

        // A device with no GMS is reported, not silently called a success.
        FakeSpawner noGms;
        noGms.checkinRestoreOut = QStringLiteral("NO_GMS\n");
        const CheckinRestore bad =
            restoreCheckinIdentity(noGms, Backend::Bare, QStringLiteral("c"), QString(), inst);
        QVERIFY(bad.attempted);
        QVERIFY(!bad.ok);
        QVERIFY(bad.detail.contains(QStringLiteral("GMS")));
        QFile::remove(checkinSnapshotPath(inst));
    }

    // …and the flag the refusal above keys on has to actually come off the file. This is the link
    // that would leave the whole guard inert while every other test still passed: the loader is the
    // only thing that ever sets it, and backend=vm otherwise reads as an unrecognised name (→ Bare).
    void loaderFlagsARetiredVmProfile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/remorarc");
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-Legacy"));
            s.setValue(QStringLiteral("backend"), QStringLiteral("vm"));
            s.setValue(QStringLiteral("ssh_host"), QStringLiteral("tester@guest.invalid"));
            s.endGroup();
            s.beginGroup(QStringLiteral("Instance-Modern"));
            s.setValue(QStringLiteral("backend"), QStringLiteral("remote"));
            s.setValue(QStringLiteral("ssh_host"), QStringLiteral("tester@guest.invalid"));
            s.endGroup();
            s.sync();
        }
        const RemoraConfig legacy = loadInstance(path, QStringLiteral("Legacy"));
        QVERIFY(legacy.backend.retiredVmBackend);
        // unrecognised name → local, per remora-d5v; on Windows, local is the guest
        QCOMPARE(inferBackend(legacy), effectiveBackend(Backend::Bare));

        const RemoraConfig modern = loadInstance(path, QStringLiteral("Modern"));
        QVERIFY(!modern.backend.retiredVmBackend);
        QCOMPARE(inferBackend(modern), Backend::Remote);

        // and saving a profile must not write the retired spelling back out
        QTemporaryDir out;
        QVERIFY(out.isValid());
        const QString saved = out.path() + QStringLiteral("/remorarc");
        saveInstance(saved, QStringLiteral("Legacy"), legacy);
        QVERIFY(!statusFileText(saved).contains(QLatin1String("backend=vm")));
    }

    // The Navigation card's two keys through the store. shortcut_mod is new and has to survive a
    // save→load; window_borderless is RETIRED and must not — a key still being read would let a
    // profile written before the frame became the desktop's business keep forcing the mirror
    // borderless, with nothing in the UI able to say so or take it back.
    void navigationKeysRoundTripAndBorderlessIsRetired() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/remorarc");
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-Nav"));
            s.setValue(QStringLiteral("mouse_bind"), QStringLiteral("b-sn:++++"));
            s.setValue(QStringLiteral("shortcut_mod"), QStringLiteral("lctrl+lalt"));
            s.setValue(QStringLiteral("key_bind"), QStringLiteral("F1:b,F2:h"));
            s.setValue(QStringLiteral("window_borderless"), true);
            s.endGroup();
            s.sync();
        }
        const RemoraConfig cfg = loadInstance(path, QStringLiteral("Nav"));
        QCOMPARE(cfg.input.mouseBind, std::optional<QString>(QStringLiteral("b-sn:++++")));
        QCOMPARE(cfg.input.shortcutMod, std::optional<QString>(QStringLiteral("lctrl+lalt")));
        QCOMPARE(cfg.input.keyBind, std::optional<QString>(QStringLiteral("F1:b,F2:h")));
        const ResolvedConfig r = resolve(cfg, Backend::Bare);
        QCOMPARE(r.shortcutMod, QStringLiteral("lctrl+lalt"));
        QCOMPARE(r.keyBind, QStringLiteral("F1:b,F2:h"));
        for (const QString &t : buildMirrorArgv(r, QStringLiteral("localhost:5555")).second)
            QVERIFY(!t.startsWith(QLatin1String("--window-borderless")));

        QTemporaryDir out;
        QVERIFY(out.isValid());
        const QString saved = out.path() + QStringLiteral("/remorarc");
        saveInstance(saved, QStringLiteral("Nav"), cfg);
        const QString text = statusFileText(saved);
        QVERIFY(text.contains(QLatin1String("shortcut_mod=lctrl+lalt")));
        // QSettings quotes the value because ',' is its own INI list separator.
        QVERIFY(text.contains(QLatin1String("key_bind=\"F1:b,F2:h\"")));
        QVERIFY(!text.contains(QLatin1String("window_borderless")));

        // A HAND-WRITTEN unquoted key_bind=F1:b,F2:h is split on the comma by QSettings and
        // arrives as a QStringList — whose toString() is EMPTY, so a naive read silently drops
        // every binding the user wrote. The store must join it back instead.
        QTemporaryDir hand;
        QVERIFY(hand.isValid());
        const QString handPath = hand.path() + QStringLiteral("/remorarc");
        {
            QFile f(handPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("[Instance-Nav]\nkey_bind=F1:b,F2:h,Pause:s\n");
        }
        QCOMPARE(loadInstance(handPath, QStringLiteral("Nav")).input.keyBind,
                 std::optional<QString>(QStringLiteral("F1:b,F2:h,Pause:s")));
    }

    void placeholderImageTagFailsBeforeDocker() {
        // A version with NO registered image resolves to selectImage's "(no pre-built image for
        // A<N>)" sentinel — ensure-image must fail with build-from-source guidance BEFORE the tag
        // reaches any docker/ssh command (the parens are a bash syntax error, bd remora-bbf).
        // BOTH deployers: remote has its own ensureImage and would otherwise be the one path that
        // could still shell-inject.
        // Uses an UNSUPPORTED version deliberately. This said 17 and broke the moment A17 images
        // were registered: any real version is only imageless until someone builds for it, so
        // pinning the test to one makes it expire the day that version starts working. 99 is
        // outside supportedAndroidVersions(), so no tag can ever be registered for it.
        for (const Backend b : deployTargets()) {
            FakeSpawner sp;
            RemoraConfig cfg;
            cfg.image.androidVersion = 99;
            auto ctx = makeContext(withGuest(cfg), b);
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 24, 0)->steps(ctx), sink));
            QCOMPARE(sink.of("ensure-image"), StepState::Failed);
            QVERIFY(!hasCall(sp, "docker image inspect"));  // the sentinel never hit a shell
            QVERIFY(sink.details.join(' ').contains("Build from source"));  // remedy is actionable
        }
    }

    void staleRemoteImageFailsWithPushRemedy() {
        // A rebuilt-but-never-pushed tag must not deploy green: the remote's tag exists but its
        // layer DiffIDs differ from the local build's (bd remora-t06).
        FakeSpawner sp;
        sp.guestImageStale = true;
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("ensure-image"), StepState::Failed);
        QCOMPARE(sink.of("provision"), StepState::Skipped);
        QVERIFY(sink.details.join(' ').contains("docker save"));  // the remedy is actionable
    }

    void deployRelaysBootMarkerToSplash() {
        SKIP_ON_WINDOWS(kBareDeploy);
        // provision must write REMORA_BOOTING into the splash status file the moment it starts a
        // fresh Android boot, not after the run returns — the splash would miss the whole boot.
        FakeSpawner sp;
        sp.containerRunning = false;  // nothing up yet, so provision starts a real boot
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        QFile::remove(ctx.statusPath);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        // Reported as evidence, not as a bare FALSE. This is the assertion that flaked under load
        // (bd remora-eoy.9) and it could not be reproduced afterwards — 20 runs of
        // the pre-fix build at load average 18, then 400 of the fixed one, all green. So the cause
        // is still unknown, and the one thing worth doing for a failure that rare is making sure
        // the NEXT one arrives with the file's actual contents attached instead of "returned FALSE".
        // appendStatusLine cannot report anything itself: it drops a failed open() on the floor.
        const QString got = statusFileText(ctx.statusPath);
        QVERIFY2(got.contains(QStringLiteral("REMORA_BOOTING")),
                 qPrintable(QStringLiteral("no REMORA_BOOTING in %1 (exists=%2, size=%3) — full "
                                           "contents between the markers >>>%4<<<")
                                .arg(ctx.statusPath)
                                .arg(QFile::exists(ctx.statusPath) ? "yes" : "NO")
                                .arg(QFileInfo(ctx.statusPath).size())
                                .arg(got)));
    }

    // The suite went from ~371s to well under a second by routing every engine wait through
    // Spawner::waitMs and having the fake record instead of sleep. The obvious way to regress that
    // win is to "fix" a slow test by deleting a wait the real device needs, and nothing would
    // notice — a fake device answers instantly either way. So assert the pacing is still REQUESTED:
    // a vm deploy asks for the 4s boot-stability hold (three 1s polls before the fourth probe
    // satisfies it) plus the 1s mirror liveness delay. If this number drops, a real boot got faster
    // on paper only (bd remora-eoy.9).
    //
    // The hold was 15s at a 3s poll, then 8s, and is now 4s at a 1s poll. The budget is what bounds
    // a hung boot; the interval was pure overshoot, and the coarse step also rounded the hold UP to
    // a multiple of itself. It shrank because what it defended against went away: play-spoof's
    // framework bounce was the likeliest late restart and no longer happens (bd remora-v8h), and
    // first-boot module work is held for by waitFirstBootSettled, which measures the work instead
    // of guessing a duration. Changing this number is a deliberate act — that is what this test is
    // for — but taking it to zero is not, because a fresh /data really does reach boot_completed
    // and then reboot seconds later.
    void deployStillPacesItselfAgainstARealDevice() {
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        auto ctx = makeContext(withGuest(cfg), Backend::Remote);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sp.waitedMs, 3 * 1000 + 1000);
    }

    // The hold is for a FRESH /data: it reaches sys.boot_completed and then reboots itself
    // (factory_reset -> ota), taking any attached mirror with it. That is a property of a NEW data
    // dir, so a /data that has already carried a boot through to a working mirror must not keep
    // paying for it — measured at 4s off every single cold deploy, forever, for a hazard that can
    // only happen once (bd remora-tdz).
    void establishedDataSkipsTheBootStabilityHold() {
        FakeSpawner sp;
        sp.dataBootedBefore = true;
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        auto ctx = makeContext(withGuest(cfg), Backend::Remote);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        // only the 1s mirror liveness delay survives — the hold itself is gone
        QCOMPARE(sp.waitedMs, 1000);
    }

    // …and the marker is only earned by a connect that fully succeeded, so a fresh /data still
    // pays the hold and still gets its guarantee.
    void freshDataStillPaysTheBootStabilityHoldAndThenMarksItself() {
        FakeSpawner sp;  // dataBootedBefore defaults false
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        auto ctx = makeContext(withGuest(cfg), Backend::Remote);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sp.waitedMs, 3 * 1000 + 1000);
        QVERIFY2(hasCall(sp, "touch /data/adb/.remora-boot-stable"),
                 "a successful first boot must mark the data dir, or the hold never goes away");
    }

    void deploySkipsBootMarkerWhenNothingBooted() {
        SKIP_ON_WINDOWS(kBareDeploy);
        // A provision that found the container already running boots nothing and must emit no
        // marker — the splash stays on status text rather than playing an animation over a no-op.
        FakeSpawner sp;
        sp.containerRunning = true;
        sp.startOk = true;  // …and `docker start` on it succeeds, so provision reuses it
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        QFile::remove(ctx.statusPath);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QVERIFY(!statusFileText(ctx.statusPath).contains(QStringLiteral("REMORA_BOOTING")));
    }

    void connectRunSpawnsOneWindowSplash() {
        // connectRun (a full deploy) launches the boot splash IMMEDIATELY — the mirror argv plus
        // the --boot-animation flags — writes step titles into its status file, and doConnect
        // ADOPTS that process instead of spawning a second mirror: one window, one process.
        FakeSpawner sp;
        sp.containerRunning = true;  // for connect's container-up preflight
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt,
                           QStringLiteral("Remora:test"), nullptr));
        // exactly ONE mirror: the splash IS the mirror — no second spawn (the other detached
        // process is the KWin centering helper, not a mirror)
        QCOMPARE(countCalls(sp, "mirror -s"), 1);
        QCOMPARE(countCalls(sp, "DETACH"), 2);  // splash + centering helper
        QVERIFY(hasCall(sp, "--boot-animation-status="));
        const auto ctx =
            makeContext(withGuest(cfg), Backend::Remote, std::nullopt, QStringLiteral("Remora:test"));
        const QString status = statusFileText(ctx.statusPath);
        QVERIFY(status.contains(QStringLiteral("starting...")));
        QVERIFY(status.contains(QStringLiteral("Connect + launch mirror...")));
        // This deploy booted nothing (container already running, no REMORA_BOOTING) — doConnect
        // must tell the status-phase splash to attach, or it would wait for a boot forever.
        QVERIFY(status.contains(QStringLiteral("REMORA_ATTACH")));
    }

    // bd remora-7oih.2 — the splash's centering helper is a detached `sh` loop of qdbus6 KWin
    // scripting. A Windows client has none of the three, so it neither spawns the loop nor writes
    // the script it would have run; the splash centres itself there.
    void windowsSplashSpawnsNoKWinHelper() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        const auto ctx = makeContext(withGuest(cfg), Backend::Remote, std::nullopt,
                                     QStringLiteral("Remora:wintest"));
        QFile::remove(ctx.statusPath + QStringLiteral(".center.js"));
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt,
                           QStringLiteral("Remora:wintest"), nullptr));
        QCOMPARE(countCalls(sp, "DETACH"), 1);  // the splash, and nothing else
        QVERIFY(!hasCall(sp, "DETACH sh"));
        QVERIFY(!QFile::exists(ctx.statusPath + QStringLiteral(".center.js")));
    }

    // bd remora-82x. play-spoof BACKGROUNDS each module's service.sh (setsid … &): they block on
    // sys.boot_completed and only THEN do their real work — PIF's is the whole sensitive-property
    // pass. So the step reports ok while that work is still pending, it lands seconds later, and it
    // takes the framework and adb down with it. A splash that has already become the mirror sees
    // "Device disconnected" and exits: the window drops and doConnect spawns a replacement, which
    // is the drop-then-reappear the user reported on every first connect. Hold the hand-off until
    // the container says that work is done — waiting costs nothing there, because until the
    // hand-off the splash is only running adb probes and a disconnect just keeps it animating.
    void firstBootModulesHoldTheMirrorHandoff() {
        FakeSpawner sp;
        sp.containerRunning = true;
        sp.firstBootStates = QStringList{"busy", "busy", "busy", "idle"};
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Bare, RemoraConfig{}, sink, sp, std::nullopt,
                           QStringLiteral("Remora:test"), nullptr));
        // three busy answers, then the one that releases it
        QCOMPARE(countCalls(sp, "service\\.sh"), 4);
        // …and the connect step SAYS it held. ConsoleSink overrides state() and drops line(), so
        // the step detail is the only thing the CLI and the desktop launcher can see — and "was
        // the hand-off held, or did it land mid-restart?" is precisely the question a cold boot
        // has to answer afterwards. The two outcomes are otherwise indistinguishable (the lesson
        // of bd remora-eoy.10, which cost days for want of one recorded decision).
        QVERIFY2(sink.details.join(QLatin1Char('\n'))
                     .contains(QStringLiteral("first-boot modules held the hand-off 3s")),
                 qPrintable(sink.details.join(QLatin1Char('\n'))));
    }

    // Fail-open, in every direction: this gate may DELAY a mirror and must never block one. A
    // container that cannot answer — no pgrep, a mid-restart adb, an ssh that timed out — leaves
    // the hand-off exactly where it would be without the gate, after a single probe.
    void firstBootGateFailsOpenWhenTheContainerCannotAnswer() {
        FakeSpawner sp;
        sp.containerRunning = true;
        sp.firstBootStates = QStringList{QString()};  // neither "busy" nor "idle": nothing usable
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Bare, RemoraConfig{}, sink, sp, std::nullopt,
                           QStringLiteral("Remora:test"), nullptr));
        QCOMPARE(countCalls(sp, "service\\.sh"), 1);
        QVERIFY(!sink.details.join(QLatin1Char('\n'))
                     .contains(QStringLiteral("held the hand-off")));
    }

    // The same gate on the ssh backends. The probe rides inside a `docker exec … sh -c "…"`
    // payload, so the pattern's backslash has to survive the extra quoting with BOTH its meanings
    // intact: match a literal dot, and stay unable to match the probe's own command line.
    void firstBootGateAsksOverSshOnTheVmBackend() {
        FakeSpawner sp;
        sp.containerRunning = true;
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt,
                           QStringLiteral("Remora:test"), nullptr));
        QVERIFY(hasCall(sp, "service\\.sh"));
    }

    void deployNeverStopsItsOwnSplash() {
        // REGRESSION, bd remora-eoy.10. doConnect's stale-mirror reap matches the SPLASH too —
        // same -s, same --window-title, because the splash IS the mirror argv plus the
        // boot-animation flags. It used to be gated on adoptSplash, so the moment that test came
        // out false (which it does on a cold path, ~70s after startSplash) Remora SIGTERMed the
        // live, mirroring window it had just opened: captured on-device as "Terminated" in the
        // splash's own log 21s into a healthy session, seen by the user as the mirror dropping
        // and reopening. Once a splash exists it is the ONLY thing this lookup can still match —
        // startSplash reaped true stales immediately before spawning it — so the stop must not
        // run at all, regardless of how the adoption test turns out.
        FakeSpawner sp;
        sp.containerRunning = true;
        // Reproduce the cold-path condition rather than the happy one: fail the FIRST liveness
        // check, which is precisely the adoptSplash test, while the splash itself is spawned and
        // later checks pass. Without this the fake adopts the splash, the stop is skipped for the
        // old reason, and the test would pass even against the bug it exists to catch.
        sp.detachedDiesFirst = 1;
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        const QString target =
            makeContext(withGuest(cfg), Backend::Remote, std::nullopt, QStringLiteral("Remora:test"))
                .rc.target;
        // A stale mirror before the deploy; once the splash is up, the registry holds the splash
        // (pid 4242, the fake's every spawn) — so a stop aimed at "this instance's mirror" after
        // the spawn would land on it, and be caught here.
        sp.mirrorSnapshots = {
            {mirrorAt(111, target, QStringLiteral("test"))},
            {mirrorAt(4242, target, QStringLiteral("test"), MirrorKind::Splash,
                      MirrorSessionState::Pending, 5000)}};
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt,
                           QStringLiteral("Remora:test"), nullptr));
        int splashAt = -1;
        for (int i = 0; i < sp.calls.size(); ++i)
            if (sp.calls[i].contains(QStringLiteral("mirror -s"))) { splashAt = i; break; }
        QVERIFY2(splashAt >= 0, "no splash was spawned — test cannot check the invariant");
        for (int i = splashAt + 1; i < sp.calls.size(); ++i)
            QVERIFY2(!sp.calls[i].startsWith(QStringLiteral("STOP")),
                     qPrintable(QStringLiteral("deploy stopped a mirror after spawning its own "
                                               "splash: %1").arg(sp.calls[i])));
        // No guest-side kill at ANY point in a deploy: on a cold path the splash already owns a
        // live session, and the agent is the one init service serving it — stopping or killing it
        // is the same eoy.10 drop from the other end.
        for (const QString &c : sp.calls) {
            const bool namesAgent = c.contains(QStringLiteral("com.remora.agent"))
                                    || c.contains(QStringLiteral("remora_agent"));
            const bool stops = c.contains(QStringLiteral("kill"))
                               || c.contains(QStringLiteral("ctl.stop"))
                               || c.contains(QStringLiteral("ctl.restart"))
                               || c.contains(QStringLiteral("stop remora_agent"));
            QVERIFY2(!(namesAgent && stops),
                     qPrintable(QStringLiteral("deploy stopped the guest agent: %1").arg(c)));
        }
        // The LOCAL pre-spawn reap is legitimate and must survive: at that point no splash
        // exists and it is what reaps a genuinely stale mirror window.
        bool localReapBeforeSplash = false;
        for (int i = 0; i < splashAt; ++i)
            if (sp.calls[i] == QStringLiteral("STOP 111 force")) { localReapBeforeSplash = true; break; }
        QVERIFY2(localReapBeforeSplash,
                 "the stale local mirror was never reaped before the splash was spawned");
        QVERIFY(!everStopped(sp, 4242));
        QVERIFY(!spawnsProgram(sp, QStringLiteral("pkill")));
    }




    void connectRunAbortsSplashOnFailure() {
        // A failed chain must close the splash (REMORA_ABORT + kill), not leave it up forever.
        FakeSpawner sp;
        sp.imagePresent = false;  // ensure-image fails -> chain aborts
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        RecordingSink sink;
        QVERIFY(!connectRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt,
                            QStringLiteral("Remora:test"), nullptr));
        const auto ctx =
            makeContext(withGuest(cfg), Backend::Remote, std::nullopt, QStringLiteral("Remora:test"));
        const QString status = statusFileText(ctx.statusPath);
        QVERIFY(status.contains(QStringLiteral("failed:")));
        QVERIFY(status.contains(QStringLiteral("REMORA_ABORT")));
        // The animation phase no longer reads the file, so the splash itself is stopped too —
        // through the stop seam, which escalates if it ignores the request.
        QVERIFY2(stopped(sp, {4242}), qPrintable(sp.calls.join(QLatin1Char('\n'))));
        QVERIFY(!spawnsProgram(sp, QStringLiteral("kill")));
    }

    void sleepFreezesRunningContainer() {
        // Sleep = docker pause, with the local mirror closed FIRST so its window is not left on a
        // dead frame. The guest-side agent pauses and resumes with the container.
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        const QString target =
            makeContext(withGuest(cfg), Backend::Remote, std::nullopt, QStringLiteral("Remora:test"))
                .rc.target;
        // This instance's mirror, its PIP, and another profile's mirror on the same device: sleep
        // closes the first only, exactly as the title pattern did.
        sp.mirrorSnapshots = {{mirrorAt(111, target, QStringLiteral("test")),
                               mirrorAt(112, target, pipWindowTitle(QStringLiteral("test")),
                                        MirrorKind::Pip),
                               mirrorAt(113, target, QStringLiteral("other"))}};
        RecordingSink sink;
        QVERIFY(sleepRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt, QStringLiteral("Remora:test")));
        QVERIFY(hasCall(sp, "docker pause"));
        QVERIFY2(stopped(sp, {111}), qPrintable(sp.calls.join(QLatin1Char('\n'))));
        QVERIFY(!everStopped(sp, 112) && !everStopped(sp, 113));
        QVERIFY(sp.calls.indexOf(QStringLiteral("STOP 111 force"))
                < sp.calls.indexOf(QRegularExpression(QStringLiteral(".*docker pause.*"))));
        QVERIFY(!spawnsProgram(sp, QStringLiteral("pkill")));
        QVERIFY(sp.containerPaused);
    }

    void sleepIsIdempotentWhenAlreadyPaused() {
        FakeSpawner sp;
        sp.containerPaused = true;
        RecordingSink sink;
        QVERIFY(sleepRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(!hasCall(sp, "docker pause"));  // already asleep — no second pause
    }

    void connectWakesSleepingContainer() {
        // Any connect against a paused container unpauses it transparently (~1s wake) instead of
        // burning every adb retry against frozen processes.
        FakeSpawner sp;
        sp.containerPaused = true;
        RecordingSink sink;
        QVERIFY(reconnectRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(hasCall(sp, "docker unpause"));
        QVERIFY(!sp.containerPaused);
    }

    void macvlanAutoIpDiscoveryConnects() {
        // no pinned macvlan IP: the resolved target is deferred; connect discovers the
        // docker-assigned address and both adb and the mirror use it
        FakeSpawner sp;
        auto ctx = makeContext(withMacvlan(), Backend::Remote);
        QVERIFY(ctx.rc.target.isEmpty());
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QVERIFY(hasCall(sp, "adb connect 192.168.0.90:5556"));
        QVERIFY(hasCall(sp, "-s 192.168.0.90:5556"));  // mirror argv re-targeted too
    }

    void resolveAdbTargetPinnedNeedsNoDiscovery() {
        FakeSpawner sp;
        const auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        // localhost:5555, or on Windows the guest's name, which the deploy maps to its port
        QCOMPARE(resolveAdbTarget(sp, ctx), ctx.rc.target);
        QVERIFY(sp.calls.isEmpty());  // a pinned target costs zero round trips
    }

    void resolveAdbTargetDiscoversMacvlanIp() {
        FakeSpawner sp;
        const auto ctx = makeContext(withMacvlan(), Backend::Remote);
        QVERIFY(ctx.rc.target.isEmpty());
        QCOMPARE(resolveAdbTarget(sp, ctx), QStringLiteral("192.168.0.90:5556"));
        QVERIFY(hasCall(sp, "docker inspect"));
    }

    void resolveAdbTargetFailsWhenIpUnreadable() {
        FakeSpawner sp;
        sp.macvlanIp.clear();
        const auto ctx = makeContext(withMacvlan(), Backend::Remote);
        QString err;
        QVERIFY(resolveAdbTarget(sp, ctx, &err).isEmpty());
        QVERIFY(err.contains(QStringLiteral("macvlan IP")));
    }

    // ─────────── macvlan on every backend (bd remora-400) ───────────

    // The bug this fixes: network_mode=macvlan on bare resolved to "bridge", so the profile's own
    // LAN address never reached the argv and the container came up port-mapped instead.
    void bareMacvlanResolvesInsteadOfBeingOverwritten() {
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        const ResolvedConfig rc = resolve(cfg, Backend::Bare);
        QCOMPARE(rc.networkMode, QStringLiteral("macvlan"));
        QCOMPARE(rc.dockerNetwork.value_or(QString()), QStringLiteral("lan"));
        // The port is the CONTAINER's (the bare fwd shim on 5556), never hostAdbPort — nothing is
        // published in this mode, so a host port would describe a bind that does not exist.
        QCOMPARE(rc.target, QStringLiteral("192.0.2.77:5556"));
    }

    // …and the defaults are untouched: bare stays bridged unless a profile asks otherwise.
    void bareDefaultsToBridgeStill() {
        SKIP_ON_WINDOWS(kBareHost);
        const ResolvedConfig rc = resolve(RemoraConfig{}, Backend::Bare);
        QCOMPARE(rc.networkMode, QStringLiteral("bridge"));
        QCOMPARE(rc.target, QStringLiteral("localhost:5555"));
        QVERIFY(!rc.dockerNetwork.has_value());
        QVERIFY(!rc.macvlanSubnet.has_value());
    }

    // Full-hardware H.265 is always the default and top priority (user directive),
    // wherever hardware HEVC can run: a host-mode profile resolves to h265, and — because a
    // non-h264 codec is what triggers the bump — to the -hwc2 image that carries the hardware
    // encoder. Guest mode, the empty profile's, has no hardware HEVC path (measured, bd
    // remora-hsdz), so it resolves h264 and records why — still on the -hwc2 image, whose h264
    // vector pins the software encoder. An explicit h264 pin still wins.
    void h265IsTheDefaultCodec() {
        RemoraConfig host;
        host.gpu.mode = GpuMode::Host;
        QCOMPARE(resolve(host, Backend::Bare).videoCodec, QStringLiteral("h265"));
        const ResolvedConfig rc = resolve(RemoraConfig{}, Backend::Bare);
        QCOMPARE(rc.videoCodec, QStringLiteral("h264"));
        QVERIFY(rc.videoCodecFallback.has_value());
        QVERIFY(rc.imageTag.contains(QLatin1String("hwc2")));
        RemoraConfig pinned;
        pinned.mirror.videoCodec = QStringLiteral("h264");
        QCOMPARE(resolve(pinned, Backend::Bare).videoCodec, QStringLiteral("h264"));
    }

    // The codec2 pipeline is on for EVERY profile — an empty config, an h264 profile, an image with
    // no hw_video_decode feature. It used to be conditional on those, which made the HEVC/AV1
    // encoders reachable only from a profile that had already turned the pipeline on by hand.
    // remorarc's use_codec2=false is the one remaining way off, and it still works.
    void codec2PipelineIsOnForEveryProfile() {
        QVERIFY(resolve(RemoraConfig{}, Backend::Bare).useCodec2);
        QVERIFY(resolve(RemoraConfig{}, Backend::Remote).useCodec2);
        RemoraConfig h264;  // the default codec, and an image with no hw_video_decode baked in
        h264.mirror.videoCodec = QStringLiteral("h264");
        h264.image.imageTag = QStringLiteral("remora23:x86_64");
        QVERIFY(resolve(h264, Backend::Bare).useCodec2);
        RemoraConfig off;  // hand-set remorarc opt-out
        off.advanced.useCodec2 = false;
        QVERIFY(!resolve(off, Backend::Bare).useCodec2);
    }

    // The macvlan shape comes from the LIVE host probe, never from a baked constant — and the
    // auto-assignment range is derived from whichever subnet won, so an override carries its own.
    void macvlanShapeComesFromTheProbedHostLan() {
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("UMN"), QStringLiteral("192.168.0.0/24"),
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.12")};
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        ResolvedConfig rc = resolve(cfg, Backend::Bare, caps);
        QCOMPARE(rc.macvlanParent.value_or(QString()), QStringLiteral("UMN"));
        QCOMPARE(rc.macvlanSubnet.value_or(QString()), QStringLiteral("192.168.0.0/24"));
        QCOMPARE(rc.macvlanGateway.value_or(QString()), QStringLiteral("192.168.0.1"));
        QCOMPARE(rc.macvlanRange.value_or(QString()), QStringLiteral("192.168.0.240/28"));
        QVERIFY(rc.target.isEmpty());  // no pin -> discovered at connect

        cfg.network.macvlanSubnet = QStringLiteral("10.9.0.0/24");
        rc = resolve(cfg, Backend::Bare, caps);
        QCOMPARE(rc.macvlanRange.value_or(QString()), QStringLiteral("10.9.0.240/28"));
        QCOMPARE(rc.macvlanParent.value_or(QString()), QStringLiteral("UMN"));  // still probed
    }

    void ensureMacvlanNetworkCreatesTheMissingNetwork() {
        FakeSpawner sp;
        sp.macvlanNetPresent = false;
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("UMN"), QStringLiteral("192.168.0.0/24"),
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.12")};
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        const NetworkEnsure r =
            ensureMacvlanNetwork(sp, resolve(cfg, Backend::Bare, caps), QString());
        QVERIFY(r.ok);
        QCOMPARE(sp.netCreates.size(), 1);
        QCOMPARE(sp.netCreates.first(),
                 QStringLiteral("-d macvlan --subnet 192.168.0.0/24 --gateway 192.168.0.1 "
                                "--ip-range 192.168.0.240/28 -o parent=UMN lan"));
    }

    void ensureMacvlanNetworkLeavesAnExistingOneAlone() {
        FakeSpawner sp;  // macvlanNetPresent defaults true
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        const NetworkEnsure r =
            ensureMacvlanNetwork(sp, resolve(cfg, Backend::Bare), QString());
        QVERIFY(r.ok);
        QVERIFY(sp.netCreates.isEmpty());
    }

    // A missing network with no readable host LAN is a hard stop. Creating one on a guessed subnet
    // would produce a container that boots, gets an address and routes nowhere — the failure the
    // whole probe exists to avoid.
    void ensureMacvlanNetworkRefusesToGuessASubnet() {
        FakeSpawner sp;
        sp.macvlanNetPresent = false;
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        const NetworkEnsure r =
            ensureMacvlanNetwork(sp, resolve(cfg, Backend::Bare), QString());  // no caps
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("macvlan_parent")));
        QVERIFY(sp.netCreates.isEmpty());
    }

    // A pinned address outside the network's subnet: docker's own refusal names neither, so the
    // mismatch is caught here while both numbers are still in hand.
    void ensureMacvlanNetworkRejectsAPinOutsideTheSubnet() {
        FakeSpawner sp;
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("UMN"), QStringLiteral("192.168.0.0/24"),
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.12")};
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("10.9.9.9");
        const NetworkEnsure r =
            ensureMacvlanNetwork(sp, resolve(cfg, Backend::Bare, caps), QString());
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("10.9.9.9")));
        QVERIFY(r.detail.contains(QStringLiteral("192.168.0.0/24")));
    }

    void ensureMacvlanNetworkIsANoOpInBridgeMode() {
        FakeSpawner sp;
        const NetworkEnsure r =
            ensureMacvlanNetwork(sp, resolve(RemoraConfig{}, Backend::Bare), QString());
        QVERIFY(r.ok);
        QVERIFY(sp.calls.isEmpty());  // bridge mode costs zero round trips
    }

    // ─────────── the macvlan host route (bd remora-400) ───────────

    static ResolvedConfig macvlanRc(bool hostRoute = true) {
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("UMN"), QStringLiteral("192.168.0.0/24"),
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.12")};
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.78");
        if (!hostRoute) cfg.network.macvlanHostRoute = false;
        return resolve(cfg, Backend::Bare, caps);
    }

    // The shim address is derived, not configured: one below the auto-IP pool, so the host and the
    // containers can never be handed the same address.
    void macvlanShimIpIsDerivedFromThePool() {
        const ResolvedConfig rc = macvlanRc();
        QVERIFY(rc.macvlanHostRoute);
        QCOMPARE(rc.macvlanShimIp.value_or(QString()), QStringLiteral("192.168.0.239"));
        QCOMPARE(rc.macvlanRange.value_or(QString()), QStringLiteral("192.168.0.240/28"));
    }

    void hostRouteIsCreatedWhenAbsent() {
        FakeSpawner sp;  // shimPresent defaults false
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc());
        QVERIFY(r.ok);
        QCOMPARE(sp.shimRuns.size(), 1);
        QVERIFY(sp.shimRuns.first().contains(QStringLiteral("ip link add remora-shim0 link UMN")));
        // Both blocks routed: the pool AND the pinned address outside it.
        QVERIFY(sp.shimRuns.first().contains(
            QStringLiteral("want='192.168.0.240/28 192.168.0.78'")));
    }

    // Re-asserting an existing shim must not tear down a CORRECT one — the adb connection is
    // riding it — but the script still has to inspect it, because a shim with the wrong parent or
    // (worse) in VEPA mode has to be rebuilt rather than adopted.
    void hostRouteReassertsWithoutRecreating() {
        FakeSpawner sp;
        sp.shimPresent = true;
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc());
        QVERIFY(r.ok);
        QCOMPARE(sp.shimRuns.size(), 1);  // still runs, because routes may have moved…
        // -d, not a plain `link show`: the mode is only visible in the detailed output, and the
        // mode is the whole question.
        QVERIFY(sp.shimRuns.first().contains(QStringLiteral("ip -d link show remora-shim0")));
        QVERIFY(!hasCall(sp, "ping"));  // …but the address-conflict probe is first-creation only
    }

    // The shim carries a real address on a real segment. Taking one that is already answering
    // would break whatever owns it, so refuse and say which knob fixes it.
    void hostRouteRefusesAnAddressAlreadyInUse() {
        FakeSpawner sp;
        sp.shimIpAnswers = true;
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc());
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("192.168.0.239")));
        QVERIFY(r.detail.contains(QStringLiteral("macvlan_shim_ip")));
        QVERIFY(sp.shimRuns.isEmpty());  // nothing touched
    }

    void hostRouteFailureNamesTheManualCommand() {
        FakeSpawner sp;
        sp.shimConfigureOk = false;
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc());
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("remora-macvlan-shim install UMN")));
        QVERIFY(r.detail.contains(QStringLiteral("192.168.0.78")));
    }

    // "notable" is what reaches the CLI: ConsoleSink prints step details and drops per-line logs,
    // so a change to the operator's host networking that is only logged has not been announced.
    // Creating the interface is notable; re-asserting a correct one is not.
    void hostRouteMarksARealChangeAsNotable() {
        FakeSpawner created;
        QVERIFY(ensureMacvlanHostRoute(created, macvlanRc()).notable);
        FakeSpawner steady;
        steady.shimPresent = true;
        QVERIFY(!ensureMacvlanHostRoute(steady, macvlanRc()).notable);
    }

    // A boot-time unit and the automatic path both owning one interface is a race, not redundancy.
    // Reported for as long as both exist, not once.
    void hostRouteReportsARivalOwner() {
        FakeSpawner sp;
        sp.shimPresent = true;      // nothing to create — the unit alone makes this notable
        sp.shimUnitInstalled = true;
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc());
        QVERIFY(r.ok);
        QVERIFY(r.notable);
        QVERIFY(r.detail.contains(QStringLiteral("systemd unit")));
        QVERIFY(r.detail.contains(QStringLiteral("macvlan_host_route=false")));
    }

    // ─────────── the shim follows the container's network, not the default route (bd remora-8pts) ──

    // The login race this is about: the wired NIC the network was made on has no carrier yet, so
    // the only default route is the Wi-Fi one and that is what the resolver read.
    static HostCapabilities wifiOnlyAtLogin(const QString &wifiSubnet = QStringLiteral("192.168.0.0/24")) {
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("wlp6s0"), wifiSubnet,
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.57")};
        return caps;
    }
    static const QString &umnNetwork() {
        static const QString shape = QStringLiteral("UMN|192.168.0.0/24|192.168.0.1|");
        return shape;
    }

    void macvlanNetworkShapeIsParsedFromInspect() {
        MacvlanNetworkShape s = parseMacvlanNetworkShape(QStringLiteral("UMN|192.168.0.0/24|192.168.0.1|\n"));
        QCOMPARE(s.parent, QStringLiteral("UMN"));
        QCOMPARE(s.subnet, QStringLiteral("192.168.0.0/24"));
        QCOMPARE(s.gateway, QStringLiteral("192.168.0.1"));
        // dual stack, IPv6 listed first: the IPv4 pair is the one everything downstream uses
        s = parseMacvlanNetworkShape(
            QStringLiteral("eno1|fd00:1::/64|fd00:1::1|10.4.0.0/16|10.4.0.1|"));
        QCOMPARE(s.parent, QStringLiteral("eno1"));
        QCOMPARE(s.subnet, QStringLiteral("10.4.0.0/16"));
        QCOMPARE(s.gateway, QStringLiteral("10.4.0.1"));
        // a network with no parent option (not macvlan) and docker's "<no value>" both read empty
        QVERIFY(parseMacvlanNetworkShape(QStringLiteral("<no value>|172.18.0.0/16|172.18.0.1|"))
                    .parent.isEmpty());
        QVERIFY(parseMacvlanNetworkShape(QStringLiteral("|172.18.0.0/16|172.18.0.1|")).parent.isEmpty());
        // nothing at all, and a gateway outside the subnet, are not invented into values
        const MacvlanNetworkShape none = parseMacvlanNetworkShape(QString());
        QVERIFY(none.parent.isEmpty() && none.subnet.isEmpty() && none.gateway.isEmpty());
        QVERIFY(parseMacvlanNetworkShape(QStringLiteral("UMN|192.168.0.0/24|10.0.0.1|"))
                    .gateway.isEmpty());
    }

    // The bug as it was first seen: at login the shim was rebuilt onto the Wi-Fi card and ARP to
    // the container stayed INCOMPLETE for every try.
    void hostRouteFollowsTheExistingNetworksParent() {
        FakeSpawner sp;
        sp.macvlanNetShape = umnNetwork();
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.78");
        const ResolvedConfig rc = resolve(cfg, Backend::Bare, wifiOnlyAtLogin());
        QCOMPARE(rc.macvlanParent.value_or(QString()), QStringLiteral("wlp6s0"));  // the race
        const HostRouteResult r = ensureMacvlanHostRoute(sp, rc);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QCOMPARE(sp.shimRuns.size(), 1);
        const QString run = sp.shimRuns.first();
        QVERIFY2(run.contains(QStringLiteral("ip link add remora-shim0 link UMN")), qPrintable(run));
        QVERIFY2(run.contains(QStringLiteral("*remora-shim0@UMN:*")), qPrintable(run));
        QVERIFY2(!run.contains(QStringLiteral("wlp6s0")), qPrintable(run));
        QCOMPARE(r.parent, QStringLiteral("UMN"));
        // …and the operator is told, in the step detail the CLI prints
        QVERIFY(r.notable);
        QVERIFY2(r.detail.contains(QStringLiteral("UMN")) && r.detail.contains(QStringLiteral("wlp6s0")),
                 qPrintable(r.detail));
    }

    // Same network as the resolver read: nothing to report, nothing changes.
    void hostRouteIsQuietWhenTheNetworkAgrees() {
        FakeSpawner sp;
        sp.shimPresent = true;
        sp.macvlanNetShape = umnNetwork();
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc());
        QVERIFY(r.ok);
        QVERIFY2(!r.notable, qPrintable(r.detail));
        QCOMPARE(r.parent, QStringLiteral("UMN"));
    }

    // No default route at all at resolve time (both NICs still coming up): the resolver had no
    // parent, no pool and no shim address, and used to give up on the shim. The network knows all
    // of it.
    void hostRouteIsBuiltFromTheNetworkWhenTheHostHadNoRoute() {
        FakeSpawner sp;
        sp.macvlanNetShape = umnNetwork();
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.78");
        const ResolvedConfig rc = resolve(cfg, Backend::Bare);  // no caps: nothing was probed
        QVERIFY(!rc.macvlanParent.has_value());
        const HostRouteResult r = ensureMacvlanHostRoute(sp, rc);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QCOMPARE(sp.shimRuns.size(), 1);
        const QString run = sp.shimRuns.first();
        QVERIFY2(run.contains(QStringLiteral("ip link add remora-shim0 link UMN")), qPrintable(run));
        QVERIFY2(run.contains(QStringLiteral("ip addr replace 192.168.0.239/32")), qPrintable(run));
        QVERIFY2(run.contains(QStringLiteral("want='192.168.0.240/28 192.168.0.78'")), qPrintable(run));
    }

    // A configured pool and shim address are the operator's: the network supplies the parent, not
    // replacements for what was stated.
    void hostRouteKeepsAConfiguredShimAddress() {
        FakeSpawner sp;
        sp.macvlanNetShape = umnNetwork();
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.78");
        cfg.network.macvlanShimIp = QStringLiteral("192.168.0.200");
        const HostRouteResult r =
            ensureMacvlanHostRoute(sp, resolve(cfg, Backend::Bare, wifiOnlyAtLogin()));
        QVERIFY2(r.ok, qPrintable(r.detail));
        QVERIFY(sp.shimRuns.first().contains(QStringLiteral("ip addr replace 192.168.0.200/32")));
    }

    // The deploy-time check judges a pinned address against the subnet the container will really
    // be on. A Wi-Fi on another subnet at resolve time used to reject a correct pin.
    void ensureMacvlanNetworkJudgesThePinByTheNetworksOwnSubnet() {
        FakeSpawner sp;
        sp.macvlanNetShape = umnNetwork();
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.78");
        const NetworkEnsure r = ensureMacvlanNetwork(
            sp, resolve(cfg, Backend::Bare, wifiOnlyAtLogin(QStringLiteral("10.20.0.0/24"))),
            QString());
        QVERIFY2(r.ok, qPrintable(r.detail));
        QVERIFY2(r.detail.contains(QStringLiteral("is on UMN, not wlp6s0")), qPrintable(r.detail));
        QVERIFY(sp.netCreates.isEmpty());
        // …and a pin that is outside the network's subnet is still refused, by that subnet
        cfg.network.macvlanIp = QStringLiteral("10.20.0.9");
        const NetworkEnsure bad = ensureMacvlanNetwork(
            sp, resolve(cfg, Backend::Bare, wifiOnlyAtLogin(QStringLiteral("10.20.0.0/24"))),
            QString());
        QVERIFY(!bad.ok);
        QVERIFY2(bad.detail.contains(QStringLiteral("192.168.0.0/24")), qPrintable(bad.detail));
    }

    // When the connect still fails, the remedy it prints must name the NIC the container is on.
    // It named the resolved one, and so told the operator to put a shim on the Wi-Fi card.
    void connectFailureHintNamesTheNetworksParent() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.macvlanNetShape = umnNetwork();
        sp.adbStates = QStringList{QStringLiteral("unknown")};  // nothing ever answers
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.78");
        auto ctx = makeContext(cfg, Backend::Bare, wifiOnlyAtLogin());
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 2, 0, 0, 1);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("own NIC (UMN)")), qPrintable(r.detail));
        QVERIFY2(!r.detail.contains(QStringLiteral("(wlp6s0)")), qPrintable(r.detail));
    }

    void hostRouteRespectsTheOptOut() {
        FakeSpawner sp;
        const HostRouteResult r = ensureMacvlanHostRoute(sp, macvlanRc(/*hostRoute=*/false));
        QVERIFY(r.ok);
        QVERIFY(sp.calls.isEmpty());
        // …and bridge deployments never go near it.
        const HostRouteResult b =
            ensureMacvlanHostRoute(sp, resolve(RemoraConfig{}, Backend::Bare));
        QVERIFY(b.ok);
        QVERIFY(sp.calls.isEmpty());
    }

    // adb holds a transport per ENDPOINT, and `adb connect` to one it already has returns "already
    // connected" without re-handshaking — so a dead transport survives every retry and the whole
    // budget burns against it. Observed live on a pinned macvlan address, where the endpoint is
    // stable across container recreations and the stale entry is indistinguishable from a live one.
    void connectForcesARehandshakeOnAStaleTransport() {
        FakeSpawner sp;
        sp.adbStates = QStringList{"offline", "offline", "device"};
        const auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/4, /*retryDelayMs=*/0);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QVERIFY(hasCall(sp, "adb reconnect offline"));
    }



    // REPLACED (bd remora-28ix.4 step 4; re-landed after a revert ate the first copy). This pinned
    // that a container under the PRE-RENAME name counted as "ours" and auto-consented — needed
    // while the adoption shim could still rename it, wrong once nothing can. What is pinned now is
    // the replacement contract: only the EXACT name is ours; anything else holding our data dir
    // prompts for an explicit takeover, and a pre-rename leftover is simply one of those.
    void onlyTheExactContainerNameIsOurs() {
        ResolvedConfig rc;
        rc.containerName = QStringLiteral("remora-android-17");
        rc.dataDir = QStringLiteral("/home/u/.remora-android-17");
        rc.hostAdbPort = 5555;
        ContainerFacts self;
        self.name = rc.containerName;
        self.bindSources = {rc.dataDir};
        ClusterState s2;
        s2.local = {self};
        const auto own = conflictFor(s2, Backend::Bare, rc);
        QVERIFY(own.has_value());
        QVERIFY(own->sameName);
        // a leftover under any other name is just another foreign holder — prompt, never
        // auto-consent
        ContainerFacts twin;
        twin.name = QStringLiteral("legacy-android-17");
        twin.bindSources = {rc.dataDir};
        ClusterState s3;
        s3.local = {twin};
        const auto foreign = conflictFor(s3, Backend::Bare, rc);
        QVERIFY(foreign.has_value());
        QVERIFY2(!foreign->sameName, "a pre-rename leftover must prompt, not auto-consent");
    }

    void containerNetworkStaleDetectsAModeSwitch() {
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        const ResolvedConfig mac = resolve(cfg, Backend::Bare);
        QVERIFY(containerNetworkStale(QStringLiteral("bridge=172.17.0.2 "), mac));
        QVERIFY(!containerNetworkStale(QStringLiteral("lan=192.168.0.240 "), mac));

        const ResolvedConfig br = resolve(RemoraConfig{}, Backend::Bare);
        QVERIFY(containerNetworkStale(QStringLiteral("lan=192.168.0.240 "), br));
        QVERIFY(!containerNetworkStale(QStringLiteral("bridge=172.17.0.2 "), br));

        // A changed pin recreates too — --ip is fixed at create time.
        cfg.network.macvlanIp = QStringLiteral("192.168.0.77");
        const ResolvedConfig pinned = resolve(cfg, Backend::Bare);
        QVERIFY(containerNetworkStale(QStringLiteral("lan=192.168.0.240 "), pinned));
        QVERIFY(!containerNetworkStale(QStringLiteral("lan=192.168.0.77 "), pinned));

        // No container inspects as nothing, and nothing is not stale — it is about to be created.
        QVERIFY(!containerNetworkStale(QString(), mac));
        QVERIFY(!containerNetworkStale(QStringLiteral("  \n"), br));
    }

    void bareProvisionRecreatesAContainerOnTheWrongNetwork() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.startOk = true;  // the container exists and would start happily
        sp.containerNetworks = QStringLiteral("bridge=172.17.0.2 ");
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("UMN"), QStringLiteral("192.168.0.0/24"),
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.12")};
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        LogSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)
                             ->steps(makeContext(cfg, Backend::Bare, caps)),
                         sink));
        QVERIFY(hasCall(sp, "docker rm -f"));
        QVERIFY(hasCall(sp, "--network lan"));
    }

    // …and the same probe must not make every ordinary deploy recreate its container.
    void bareProvisionKeepsABridgedContainerInBridgeMode() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.startOk = true;
        LogSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)
                             ->steps(makeContext(RemoraConfig{}, Backend::Bare)),
                         sink));
        QVERIFY(!hasCall(sp, "docker rm -f"));
        QVERIFY(hasCall(sp, "docker start"));
    }

    // The full bare deploy with macvlan: the network is ensured before the run, the run carries
    // --network/--ip instead of -p, and the connect goes to the container's own address.
    void bareMacvlanDeployWiresTheWholeChain() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        sp.macvlanNetPresent = false;
        sp.startOk = false;  // force the create path so the docker-run argv is actually issued
        HostCapabilities caps;
        caps.hostLan = LanInterface{QStringLiteral("UMN"), QStringLiteral("192.168.0.0/24"),
                                    QStringLiteral("192.168.0.1"), QStringLiteral("192.168.0.12")};
        RemoraConfig cfg;
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.168.0.77");
        const auto ctx = makeContext(cfg, Backend::Bare, caps);
        QCOMPARE(ctx.rc.target, QStringLiteral("192.168.0.77:5556"));
        LogSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sp.netCreates.size(), 1);
        QVERIFY(hasCall(sp, "--network lan --ip 192.168.0.77"));
        QVERIFY(!hasCall(sp, "-p 5555:5556"));
        QVERIFY(hasCall(sp, "adb connect 192.168.0.77:5556"));
    }

    void doctorReportBundlesSections() {
        FakeSpawner sp;
        const QString rep =
            doctorReport(sp, withMacvlan(), Backend::Remote, QStringLiteral("default"));
        for (const char *sec : {"## resolved config", "## status probe", "## readiness",
                                "## versions", "## device", "## recent logcat"})
            QVERIFY2(rep.contains(QLatin1String(sec)), sec);
        QVERIFY(hasCall(sp, "logcat"));  // recent logcat captured
        QVERIFY(rep.contains(QStringLiteral("192.168.0.90:5556")));  // live target resolved
    }

    void doctorReportSurvivesDeadTarget() {
        FakeSpawner sp;
        sp.macvlanIp.clear();  // discovery reads nothing back — device sections must degrade
        const QString rep =
            doctorReport(sp, withMacvlan(), Backend::Remote, QStringLiteral("default"));
        QVERIFY(rep.contains(QLatin1String("unreachable")));
        QVERIFY(!hasCall(sp, "logcat"));
        QVERIFY(rep.contains(QLatin1String("## versions")));  // the rest still bundled
    }

    // The auto-sleep idle signal: every local client on the target — mirror, splash, PIP, app and
    // desktop windows alike — and only that target. On Windows the pgrep this used to be answered
    // 0 whatever was open, and auto-sleep froze a device someone was watching (bd remora-7oih.1).
    void countMirrorClientsCountsTheRegistry() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        const QString t = QStringLiteral("192.0.2.77:5555");
        sp.mirrorSnapshots = {{mirrorAt(1, t, QStringLiteral("A")),
                               mirrorAt(2, t, pipWindowTitle(QStringLiteral("A")), MirrorKind::Pip),
                               mirrorAt(3, t, QStringLiteral("Maps"), MirrorKind::App),
                               // a serial that merely STARTS with the target is another device
                               mirrorAt(4, QStringLiteral("192.0.2.77:55550"), QStringLiteral("B")),
                               mirrorAt(5, QStringLiteral("192.0.2.78:5555"), QStringLiteral("C"))}};
        QCOMPARE(countMirrorClients(sp, t), 3);
        QCOMPARE(countMirrorClients(sp, QString()), 0);  // no target counts nothing, not everything
        QVERIFY(sp.calls.isEmpty());  // no pgrep, no shell — nothing spawned at all
        sp.mirrorSnapshots.clear();
        QCOMPARE(countMirrorClients(sp, t), 0);
    }

    void captureScreenshotWritesFile() {
        FakeSpawner sp;
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("shot.png"));
        QString err;
        QVERIFY(captureScreenshot(sp, QStringLiteral("192.0.2.77:5555"), path, &err));
        // adb itself, its stdout into the file: no `sh -c "… > file"`, which a Windows client has
        // no shell to run (bd remora-7oih.2)
        QVERIFY(hasCall(sp, "adb -s 192.0.2.77:5555 exec-out screencap -p > " + path));
        QVERIFY(!spawnsProgram(sp, "sh"));
        QVERIFY(QFileInfo(path).size() > 0);
    }

    void captureScreenshotFailsHonestly() {
        FakeSpawner sp;
        sp.screencapOk = false;
        QTemporaryDir tmp;
        QString err;
        QVERIFY(!captureScreenshot(sp, QStringLiteral("192.0.2.77:5555"),
                                   tmp.filePath(QStringLiteral("shot.png")), &err));
        QVERIFY(!err.isEmpty());
    }

    // remora record: the --record extra flows through reconnect into the spawned mirror argv —
    // the recorded session REPLACES the mirror (a second client would degrade the encoder).
    void recordExtraReachesMirrorSpawn() {
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.mirror.extra << QStringLiteral("--record=/tmp/r.mp4");
        RecordingSink sink;
        QVERIFY(reconnectRun(Backend::Bare, cfg, sink, sp));
        bool spawnedWithRecord = false;
        for (const QString &c : sp.calls)
            if (c.startsWith(QLatin1String("DETACH ")) && c.contains(QLatin1String("mirror -s"))
                && c.contains(QLatin1String("--record=/tmp/r.mp4")))
                spawnedWithRecord = true;
        QVERIFY(spawnedWithRecord);
    }

    // bd remora-3czv. A profile pinned to a layer built by hand over a registered image answers
    // through that image, on whichever host it lives: the mirror the reconnect launches pins the
    // HEVC encoder the root ships, where it used to be launched without one. The root is asked on
    // the docker host, not this machine — over ssh for a remote profile.
    void pinnedLocalLayerPinsItsRootsEncoder() {
        forgetImageRoots();
        RemoraConfig cfg;
        cfg.image.androidVersion = 17;
        const QString pin = QStringLiteral("remora-local:shamiko-native-68e16ba0-075");
        cfg.image.imageTag = pin;
        cfg.mirror.videoCodec = QStringLiteral("h265");
        cfg.gpu.mode = GpuMode::Host;  // guest rendering downgrades h265 on its own (remora-hsdz)
        const QString hevc = QStringLiteral("--video-encoder=c2.remora.vaapi.hevc.encoder");
        {
            FakeSpawner sp;  // the image names no root: nothing is pinned, as before
            RecordingSink sink;
            QVERIFY(reconnectRun(Backend::Bare, cfg, sink, sp));
            QVERIFY(hasCall(sp, "org.remora.addons.root"));
            QVERIFY(hasCall(sp, "mirror -s"));
            QVERIFY(!hasCall(sp, hevc));
        }
        for (const Backend b : {Backend::Bare, Backend::Remote}) {
            const RemoraConfig c = b == Backend::Remote ? withGuest(cfg) : cfg;
            FakeSpawner sp;
            sp.imageRootTags =
                QStringLiteral("remora24:x86_64-public\nremora24:x86_64-public-20261113.035540\n");
            RecordingSink sink;
            QVERIFY(reconnectRun(b, c, sink, sp));
            QVERIFY(hasCall(sp, hevc));
            // What the profile resolves to, not what it asked for: where this machine's docker is
            // Remora's own guest (Windows), bare is that guest, over ssh, under the guest's name.
            const ResolvedConfig rc = resolve(c, b);
            QCOMPARE(recordedImageRoot(imageHostKey(rc), pin),
                     QStringLiteral("remora24:x86_64-public"));
            bool overSsh = false;
            for (const QString &call : sp.calls)
                if (call.contains("org.remora.addons.root")) overSsh = call.startsWith("ssh");
            QCOMPARE(overSsh, rc.backend == Backend::Remote);
        }
        forgetImageRoots();
    }

    void reconnectRunsOnlyConnect() {
        FakeSpawner sp;
        RecordingSink sink;
        QVERIFY(reconnectRun(Backend::Bare, RemoraConfig{}, sink, sp));
        QVERIFY(hasCall(sp, "mirror -s"));
        QVERIFY(!hasCall(sp, "docker run"));  // no bring-up
    }

    void connectDeclinedOnConflict() {
        FakeSpawner sp;
        sp.guestPs = "remora-remote\n";  // the remote host already runs it
        RecordingSink sink;
        bool asked = false;
        const bool ok = connectRun(Backend::Remote, withGuest(), sink, sp, std::nullopt, {},
                                   [&](const Conflict &) { asked = true; return false; });
        QVERIFY(!ok);
        QVERIFY(asked);
        QVERIFY(!hasCall(sp, "docker run"));  // chain never ran
    }

    void connectAcceptedStopsThenRuns() {
        FakeSpawner sp;
        sp.guestPs = "remora-remote\n";
        RecordingSink sink;
        const bool ok = connectRun(Backend::Remote, withGuest(), sink, sp, std::nullopt, {},
                                   [&](const Conflict &) { return true; });
        QVERIFY(ok);
        QVERIFY(hasCall(sp, "docker rm -f"));
        QVERIFY(hasCall(sp, "docker run"));
    }

    void bareCoexistsWithARunningRemoteContainer() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;  // local docker ps empty → bare not running; ssh host not probed for bare
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Bare, RemoraConfig{}, sink, sp, std::nullopt, {},
                           [](const Conflict &) { return false; }));
        QVERIFY(!spawnsProgram(sp, "ssh"));  // a bare launch never probes the ssh host
    }

    // ── bd remora-4u4.1: exclusion is by RESOURCE, not by a hardcoded name list ──

    // The false positive that blocked multi-instance outright: a DIFFERENT profile is running,
    // sharing nothing, and the new one used to be refused purely because the running container's
    // name was on a hardcoded list.
    void secondProfileWithItsOwnResourcesIsNotAConflict() {
        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm\n");  // the default name, still running
        sp.exclPorts["remora-bm"] = QStringLiteral("5555");
        sp.exclBinds["remora-bm"] = QStringLiteral("/home/user/.remora-bm-data:/data");
        RemoraConfig cfg;  // a second profile with its own name, port and data dir
        cfg.backend.containerName = QStringLiteral("remora-second");
        cfg.backend.hostAdbPort = 5556;
        cfg.backend.dataDir = QStringLiteral("/home/user/.remora-second-data");
        RecordingSink sink;
        bool asked = false;
        QVERIFY(connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                           [&](const Conflict &) { asked = true; return false; }));
        QVERIFY(!asked);  // never even prompted
    }

    // The false negative that made the above dangerous: two differently-named containers sharing
    // the host adb port used to sail straight through and fail on the docker port bind.
    void sameHostAdbPortIsAConflictEvenWithDifferentNames() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("someone-elses-container\n");
        sp.exclPorts["someone-elses-container"] = QStringLiteral("5555");
        RemoraConfig cfg;
        cfg.backend.containerName = QStringLiteral("remora-second");
        cfg.backend.hostAdbPort = 5555;  // collides
        cfg.backend.dataDir = QStringLiteral("/home/user/.remora-second-data");
        RecordingSink sink;
        Conflict seen;
        QVERIFY(!connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                            [&](const Conflict &c) { seen = c; return false; }));
        QCOMPARE(seen.holder, QStringLiteral("someone-elses-container"));
        QVERIFY(seen.reason.contains(QStringLiteral("5555")));
        QVERIFY(!seen.sameName);
    }

    // The collision that corrupts rather than merely fails: two Android instances writing one /data.
    void sameDataDirIsAConflictEvenWithDifferentNames() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("other\n");
        sp.exclBinds["other"] = QStringLiteral("/home/user/.remora-bm-data:/data");
        RemoraConfig cfg;
        cfg.backend.containerName = QStringLiteral("remora-second");
        cfg.backend.hostAdbPort = 5599;
        cfg.backend.dataDir = QStringLiteral("/home/user/.remora-bm-data"); // collides
        RecordingSink sink;
        Conflict seen;
        QVERIFY(!connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                            [&](const Conflict &c) { seen = c; return false; }));
        QCOMPARE(seen.holder, QStringLiteral("other"));
        QVERIFY(seen.reason.contains(QStringLiteral(".remora-bm-data")));
    }

    // dataBaseDir is the SHARED overlayfs lowerdir — every profile on the host is meant to bind it,
    // so it must never read as a collision or multi-instance is blocked by its own mechanism.
    void sharedDataBaseDirIsNotAConflict() {
        FakeSpawner sp;
        sp.localPs = QStringLiteral("first\n");
        sp.exclBinds["first"] = QStringLiteral("/home/user/.remora-data-base:/data-base");
        RemoraConfig cfg;
        cfg.backend.containerName = QStringLiteral("second");
        cfg.backend.hostAdbPort = 5599;
        // pinned, so the two genuinely share it on any machine rather than only under this $HOME
        cfg.backend.dataBaseDir = QStringLiteral("/home/user/.remora-data-base");
        cfg.backend.dataDir = QStringLiteral("/home/user/.remora-second-diff");
        cfg.advanced.useOverlayfs = true;
        RecordingSink sink;
        bool asked = false;
        QVERIFY(connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                           [&](const Conflict &) { asked = true; return false; }));
        QVERIFY(!asked);
    }

    // Our own container must report sameName so smartRun's recreateOwn auto-consents instead of
    // prompting the user to take over from themselves — it also collides on port and dataDir.
    void ownContainerReportsSameNameNotAResourceCollision() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm\n");
        sp.exclPorts["remora-bm"] = QStringLiteral("5555");
        sp.exclBinds["remora-bm"] = QStringLiteral("/home/user/.remora-bm-data:/data");
        RecordingSink sink;
        Conflict seen;
        QVERIFY(!connectRun(Backend::Bare, RemoraConfig{}, sink, sp, std::nullopt, {},
                            [&](const Conflict &c) { seen = c; return false; }));
        QCOMPARE(seen.holder, QStringLiteral("remora-bm"));
        QVERIFY(seen.sameName);
    }

    // Replacing our OWN running container waits for provision. The takeover used to remove it
    // before the chain ran, so a step that failed before provision left the device down with
    // nothing to replace it — found when the in-Android updater's redeploy failed its host
    // prerequisites and took a working device with it.
    void aReplacementThatFailsEarlyLeavesTheRunningDevice() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm\n");  // ours, running…
        sp.containerStale = true;                      // …on a superseded image: a replacement
        sp.dataDirShape = QStringLiteral("flat");      // which preflight then refuses
        RecordingSink sink;
        QVERIFY(!smartRun(Backend::Bare, RemoraConfig{}, sink, sp));
        QCOMPARE(sink.of("preflight"), StepState::Failed);
        QVERIFY(!hasCall(sp, "docker rm"));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    void aReplacementRemovesTheOldContainerOnlyAtProvision() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm\n");
        sp.containerStale = true;
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, RemoraConfig{}, sink, sp));
        int shape = -1, rm = -1, run = -1;
        for (int i = 0; i < sp.calls.size(); ++i) {
            if (shape < 0 && sp.calls[i].contains("-d $D/upper")) shape = i;  // preflight
            if (rm < 0 && sp.calls[i].startsWith("docker rm -f remora-bm")) rm = i;
            if (run < 0 && sp.calls[i].startsWith("docker run -d")) run = i;
        }
        QVERIFY(shape >= 0);
        QVERIFY(rm > shape);
        QVERIFY(run > rm);
        QCOMPARE(sink.of("provision"), StepState::Ok);
    }

    // bd remora-4u4.6 — an explicitly passed host device is exclusive. V4L2 capture is
    // single-opener, so handing /dev/video0 to a second instance does not share the camera, it
    // just makes the second one fail to open it.
    void sameCameraDeviceIsAConflict() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("first\n");
        sp.exclDevices["first"] = QStringLiteral("/dev/video0");
        RemoraConfig cfg;
        cfg.backend.containerName = QStringLiteral("second");
        cfg.backend.hostAdbPort = 5599;
        cfg.backend.dataDir = QStringLiteral("/home/user/.remora-second-data");
        cfg.input.cameraDevices = QStringList{QStringLiteral("/dev/video0")};
        RecordingSink sink;
        Conflict seen;
        QVERIFY(!connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                            [&](const Conflict &c) { seen = c; return false; }));
        QCOMPARE(seen.holder, QStringLiteral("first"));
        QVERIFY(seen.reason.contains(QStringLiteral("/dev/video0")));
    }

    // …but a GPU render node is NOT exclusive. Every instance is meant to share one, and the live
    // setup does exactly that — refusing here would block multi-instance on any host-GPU profile.
    void sharedGpuRenderNodeIsNotAConflict() {
        FakeSpawner sp;
        sp.localPs = QStringLiteral("first\n");
        sp.exclDevices["first"] = QStringLiteral("/dev/dri/renderD129");
        RemoraConfig cfg;
        cfg.backend.containerName = QStringLiteral("second");
        cfg.backend.hostAdbPort = 5599;
        cfg.backend.dataDir = QStringLiteral("/home/user/.remora-second-data");
        cfg.gpu.mode = GpuMode::Host;
        cfg.gpu.gpuNode = QStringLiteral("/dev/dri/renderD129");  // the SAME node
        RecordingSink sink;
        bool asked = false;
        QVERIFY(connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                           [&](const Conflict &) { asked = true; return false; }));
        QVERIFY(!asked);
    }

    // Taking over stops the ACTUAL holder, not a hardcoded name.
    void acceptedConflictStopsTheHolderByName() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.localPs = QStringLiteral("someone-elses-container\n");
        sp.exclPorts["someone-elses-container"] = QStringLiteral("5555");
        RemoraConfig cfg;
        cfg.backend.containerName = QStringLiteral("remora-second");
        cfg.backend.hostAdbPort = 5555;
        RecordingSink sink;
        QVERIFY(connectRun(Backend::Bare, cfg, sink, sp, std::nullopt, {},
                           [](const Conflict &) { return true; }));
        QVERIFY(hasCall(sp, "docker rm -f someone-elses-container"));
        QVERIFY(!hasCall(sp, "docker rm -f remora-bm"));  // never the backend's default name
    }

    void hwc2PassesGrallocAndC2BootArgs() {
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.mirror.videoCodec = QStringLiteral("h265");  // resolves to the -hwc2 image + codec2
        cfg.display.fps = 240;
        cfg.gpu.mode = GpuMode::Host;  // gralloc is only resolved in host mode…
        HostCapabilities caps;         // …and minigbm_intel only when the picked node is xe
        caps.hostRenderNode = QStringLiteral("/dev/dri/renderD129");
        caps.hostRenderDriver = QStringLiteral("xe");
        auto ctx = makeContext(withGuest(cfg), Backend::Remote, caps);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QVERIFY(hasCall(sp, "androidboot.use_remora_c2=1"));
        QVERIFY(hasCall(sp, "androidboot.remora_gralloc=minigbm_intel"));
        QVERIFY(hasCall(sp, "androidboot.remora_fps=240"));
    }

    // The mirror can die on its first launch (a transient guest codec/buffer-setup race); connect
    // must relaunch it rather than fail outright.
    void connectRelaunchesMirrorThenSucceeds() {
        FakeSpawner sp;
        sp.detachedDiesFirst = 2;  // first two launches die immediately, third stays up
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/3);
        QVERIFY(r.ok);
        QCOMPARE(countCalls(sp, "mirror -s"), 3);  // relaunched until it stayed up
    }

    // bd remora-4ei.56 — CONNECT must bring the Venus render server up, not just the deploy chain.
    // The server is host state the container depends on but does not own. Connecting to an
    // already-running container skips host-prereqs entirely, so when the server had died in the
    // meantime the container boot-looped SurfaceFlinger against a dead socket and never left the
    // boot animation. Observed for real: the server was killed under a running container, the GUI
    // runner reconnected, and the guest logged "venus vtest: failed to connect: Connection refused"
    // on a loop with 300 crash-buffer hits.
    void connectStartsVenusServerWhenItDied() {
        FakeSpawner sp;
        sp.venusProcAlive = false;  // killed since the container was created
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/3);
        QVERIFY(r.ok);
        QVERIFY(hasCall(sp, "virgl_test_server"));  // the server was (re)started, not assumed
    }

    // bd remora-7oih.3 — venus, host_encode and host_decode are BARE ONLY. Each helper runs on
    // THIS machine, and a remote deploy has no step that starts one on the docker host, so the old
    // behaviour started the render server and the encoder/decoder here beside a container that was
    // somewhere else. The resolver keeps them off for remote; this pins the connect step's own
    // gate, which is what a reconnect or a hand-built context reaches — nothing may be started or
    // probed locally whatever the flags say.
    void remoteConnectNeverStartsOrProbesHostHelpers() {
        FakeSpawner sp;
        sp.venusProcAlive = false;
        sp.venusSocketPresent = false;
        auto ctx = makeContext(withGuest(), Backend::Remote);
        ctx.rc.venus = true;
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        ctx.rc.hostEncodeFrameSocket = hostEncodeFrameSocketFor(ctx.rc.containerName);
        ctx.rc.hostEncodeVideoSocket = hostEncodeVideoSocketFor(ctx.rc.containerName);
        ctx.rc.hostDecode = true;
        ctx.rc.hostDecodeSocket = hostDecodeSocketFor(ctx.rc.containerName);
        RecordingSink sink;
        doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0, /*livenessDelayMs=*/0,
                  /*mirrorRetries=*/3);
        QVERIFY(!hasCall(sp, "virgl_test_server"));
        QVERIFY(!hasCall(sp, "remora-frame-encoder"));
        QVERIFY(!hasCall(sp, "remora-frame-decoder"));
        QVERIFY(!hasCall(sp, "/tmp/remora-frames-"));
        QVERIFY(!hasCall(sp, "/tmp/remora-decode-"));
        // …and the mirror it launches reads the device's own stream, not a local socket.
        QVERIFY(!hasCall(sp, "--external-video-socket"));
    }

    // The same request on a remote PROFILE is refused at preflight, before anything is dialled or
    // created, naming the setting and both ways out.
    void remoteBareOnlyHostGpuSettingsRefuseInPreflight() {
        for (const char *key : {"venus", "host_encode", "host_decode"}) {
            RemoraConfig cfg = withGuest();
            cfg.gpu.mode = GpuMode::Host;
            if (QLatin1String(key) == QLatin1String("venus")) cfg.gpu.venus = true;
            if (QLatin1String(key) == QLatin1String("host_encode")) cfg.mirror.hostEncode = true;
            if (QLatin1String(key) == QLatin1String("host_decode")) cfg.mirror.hostDecode = true;
            FakeSpawner sp;
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(Backend::Remote, sp, 24, 0)
                                  ->steps(makeContext(cfg, Backend::Remote)),
                              sink));
            QCOMPARE(sink.of("preflight"), StepState::Failed);
            const QString said = sink.details.join(QLatin1Char(' '));
            QVERIFY2(said.contains(QStringLiteral("%1=true").arg(QLatin1String(key))), key);
            QVERIFY(said.contains(QStringLiteral("backend=bare")));
            QVERIFY(!hasCall(sp, "echo ok"));  // config shape: refused before ssh is dialled
            QVERIFY(!hasCall(sp, "docker run"));
            QVERIFY(!hasCall(sp, "virgl_test_server"));
        }
        // Guest mode never resolved any of them on any backend, so it is no reason to refuse.
        RemoraConfig guest = withGuest();
        guest.mirror.hostEncode = true;
        FakeSpawner sp;
        RecordingSink sink;
        runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(makeContext(guest, Backend::Remote)),
                 sink);
        QCOMPARE(sink.of("preflight"), StepState::Ok);
    }

    // bd remora-82c.3 — the same applicability rule, but against the REAL vendor tree through the
    // engine's own readVendorPath, not a fake. The unit tests pin the logic; this pins the WIRING,
    // which is where a filter like this actually goes wrong (reading the wrong root, or a path
    // shape the fake happens to model differently).
    //
    // The v4l2_codec2 pair is the live example, and it has been through both failure modes
    // (bd remora-22n): a blanket -a17 SKIP swallowed the late-landing AV1 enum patch (every A17
    // build died on the missing enum member), and the first fix —
    // listing 0005 alone in the override — made every build emit four expected "error: can't
    // open patch" lines for the files the registry still named, exactly the noise that hid the
    // original failure. The resolution is structural: v4l2_encoder is the series upstream
    // subsumes on 17 (its -a17 SKIP is back, and correct), and v4l2_av1_enum carries the enum
    // patch on EVERY version in its own dir, out of the SKIP's reach.
    void patchApplicabilityAgainstTheRealVendorTree() {
        const SourcePatch *v4l2 = nullptr, *av1 = nullptr;
        for (const SourcePatch &p : sourcePatches()) {
            if (p.key == QLatin1String("v4l2_encoder")) v4l2 = &p;
            if (p.key == QLatin1String("v4l2_av1_enum")) av1 = &p;
        }
        QVERIFY(v4l2);
        QVERIFY(av1);
        // The series: offered where it applies, withheld where upstream already carries it.
        QVERIFY(patchAppliesTo(*v4l2, 16, readVendorPath));
        QVERIFY2(!patchAppliesTo(*v4l2, 17, readVendorPath),
                 "the -a17 SKIP is gone — A17 would re-apply a series upstream subsumes, with "
                 "'patch does not apply' noise for all four files");
        // The enum patch: every version. Losing it on 17 is the build breaker.
        QVERIFY(patchAppliesTo(*av1, 16, readVendorPath));
        QVERIFY2(patchAppliesTo(*av1, 17, readVendorPath),
                 "A17 lost the VideoCodec::AV1 patch — c2-va will not compile");
        const QStringList a16 = applicableSourcePatchKeys(16, readVendorPath);
        const QStringList a17 = applicableSourcePatchKeys(17, readVendorPath);
        QVERIFY(a16.contains(QStringLiteral("v4l2_encoder")));
        QVERIFY(!a17.contains(QStringLiteral("v4l2_encoder")));
        QVERIFY(a16.contains(QStringLiteral("v4l2_av1_enum")));
        QVERIFY(a17.contains(QStringLiteral("v4l2_av1_enum")));
        QVERIFY(!a16.isEmpty() && !a17.isEmpty());

        // The av1 dir must carry the enum patch under the exact name the registry lists — an
        // emptied dir fails patchAppliesTo above, but a renamed file would not, and the apply
        // loop looks the registry's filename up verbatim.
        const auto av1files =
            readVendorPath(QStringLiteral("source-patches/external-v4l2_codec2-av1"));
        bool hasAv1 = false;
        for (const auto &e : av1files)
            if (e.first.contains(QLatin1String("VideoCodec-AV1"))) hasAv1 = true;
        QVERIFY2(hasAv1, "external-v4l2_codec2-av1 lost the VideoCodec::AV1 patch");

        // A version that has NO override still falls back to the shared series unchanged.
        QVERIFY(patchAppliesTo(*v4l2, 15, readVendorPath));
        QVERIFY(patchAppliesTo(*av1, 15, readVendorPath));
    }

    // ── bd remora-4u4.7: wiping a data dir the calling user does not own ──

    void wipeDataDirRefusesWhileTheContainerRuns() {
        FakeSpawner sp;
        sp.containerRunning = true;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        ctx.rc.dataDir = QString("/home/user/.remora-bm-data");
        const StepResult r = wipeDataDir(sp, ctx.rc, {});
        QVERIFY(!r.ok);
        QVERIFY(!hasCall(sp, "docker run"));  // nothing was removed
    }

    // The whole point: Android wrote the tree as root, so the removal must run WITH that privilege
    // rather than as the calling user, and it must delete the CONTENTS while leaving the directory
    // (which the user does own) in place — otherwise the next deploy recreates it root-owned.
    void wipeDataDirRemovesContentsInAPrivilegedContainer() {
        FakeSpawner sp;
        sp.containerRunning = false;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        ctx.rc.dataDir = QString("/home/user/.remora-bm-data");
        const StepResult r = wipeDataDir(sp, ctx.rc, {});
        QVERIFY(r.ok);
        QVERIFY(hasCall(sp, "--privileged"));
        QVERIFY(hasCall(sp, "/home/user/.remora-bm-data:/wipe/target"));
        QVERIFY(hasCall(sp, "rm -rf /wipe/target/*"));
        // busybox, not the Android image: the latter's /system/bin/sh cannot be exec'd as an
        // entrypoint because its ELF interpreter lives in an apex only init mounts.
        QVERIFY(hasCall(sp, "busybox"));
    }

    void wipeDataDirRefusesWithoutADataDir() {
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        ctx.rc.dataDir = QString();
        QVERIFY(!wipeDataDir(sp, ctx.rc, {}).ok);
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // ── bd remora-gcu: docker plants a directory where the venus build.prop belongs ──
    //
    // A bind-mount SOURCE that does not exist when a container is created is auto-created by
    // docker as a root-owned DIRECTORY — and venusBuildProp is mounted at /vendor/build.prop, so
    // any docker run that beats the host-prereqs write leaves one. The filename embeds the image
    // tag, so the file is missing exactly once per NEW tag: right after a source build, where the
    // resulting "cannot write" read as a permission problem and survived every retry. An EMPTY
    // directory at a generated-file path can only be that artefact, so it is healed in place; a
    // non-empty one is NOT the artefact and must be named, never deleted.
    void venusBuildPropHealsDockerCreatedDir() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dst = tmp.filePath("venus-build.prop-new-tag");

        // nothing there: nothing to do
        QCOMPARE(clearDockerCreatedDir(dst), QString());

        // the artefact: an empty directory is removed so the write can proceed. (User-owned here
        // where docker's is root-owned, but rmdir needs the PARENT's write permission either way,
        // and the parent is the user's cache dir in both.)
        QVERIFY(QDir().mkdir(dst));
        QCOMPARE(clearDockerCreatedDir(dst), QString());
        QVERIFY(!QFileInfo::exists(dst));

        // a regular file — the healthy case — passes through untouched
        QFile f(dst);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("ro.hardware.vulkan=virtio\n");
        f.close();
        QCOMPARE(clearDockerCreatedDir(dst), QString());
        QVERIFY(QFileInfo(dst).isFile());

        // a NON-empty directory is not docker's doing: refuse, name the state, leave it alone
        QVERIFY(f.remove());
        QVERIFY(QDir().mkdir(dst));
        QFile inner(dst + QStringLiteral("/keep"));
        QVERIFY(inner.open(QIODevice::WriteOnly));
        inner.close();
        const QString err = clearDockerCreatedDir(dst);
        QVERIFY2(err.contains(QStringLiteral("DIRECTORY")), "the state must be named");
        QVERIFY2(err.contains(dst), "the path must be named");
        QVERIFY(QFileInfo(dst).isDir());  // left in place for inspection
    }

    // bd remora-e5x.33 — the venus overlay transform: the image's own copies of the owned GPU
    // keys must be STRIPPED before ours are appended, because build.prop is first-wins for ro.*
    // props — leaving ro.hardware.vulkan=intel in place is exactly the bug that kept the Intel
    // ICD alive whatever mode asked for.
    void venusBuildPropLinesStripsBeforeAppending() {
        const QStringList out = venusBuildPropLines(
            {QStringLiteral("ro.product.name=remora_x86_64"),
             QStringLiteral("ro.hardware.vulkan=intel"),
             QStringLiteral("debug.hwui.renderer=skiagl"),
             QStringLiteral("ro.sf.lcd_density=320")});
        QVERIFY(out.contains(QStringLiteral("ro.product.name=remora_x86_64")));  // image props survive
        QVERIFY(out.contains(QStringLiteral("ro.sf.lcd_density=320")));
        QVERIFY(!out.contains(QStringLiteral("ro.hardware.vulkan=intel")));
        QVERIFY(!out.contains(QStringLiteral("debug.hwui.renderer=skiagl")));
        QVERIFY(out.contains(QStringLiteral("ro.hardware.vulkan=virtio")));
        QVERIFY(out.contains(QStringLiteral("ro.hardware.egl=angle")));
    }

    // The guest overlay's ONE script (bare runs it via local sh -c, remote over ssh) must strip
    // every owned key and append ONLY the SwiftShader ICD: pastel is what gpu_setup_guest's own
    // setprop tries and loses to the baked intel value, and ro.hardware.egl must stay unset so
    // gpu_setup_guest keeps picking angle by itself (bd remora-e5x.33).
    void guestBuildPropScriptStripsAndAppendsPastelOnly() {
        const QString s = guestBuildPropScript(QStringLiteral("~/.cache/remora/guest-build.prop-t"),
                                               QStringLiteral("remora24:tag"));
        for (const QString &k : overlayOwnedKeys())
            QVERIFY2(s.contains(k), qPrintable(k + QStringLiteral(" missing from the strip")));
        QVERIFY(s.contains(QStringLiteral("docker create remora24:tag")));
        QVERIFY(s.contains(QStringLiteral(":/vendor/build.prop")));
        QVERIFY(s.contains(QStringLiteral("ro.hardware.vulkan=pastel")));
        QVERIFY(!s.contains(QStringLiteral("ro.hardware.egl=")));   // never appended
        QVERIFY(!s.contains(QStringLiteral("vulkan=virtio")));      // venus content stays venus's
    }

    // Default gpu_mode is GUEST on bare, so the overlay is part of the DEFAULT deploy: it must be
    // generated in host-prereqs — before `docker run` names it as a bind source — and its failure
    // must stop the deploy (without the overlay the baked Intel ICD wins and SurfaceFlinger
    // crash-loops in chooseEglConfig before boot completes).
    void bareGuestModeGeneratesBuildPropOverlayBeforeRun() {
        SKIP_ON_WINDOWS(kBareDeploy);
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare);
        QVERIFY2(ctx.rc.buildPropOverlay.has_value(), "guest default must resolve an overlay path");
        QVERIFY(ctx.rc.buildPropOverlay->contains(QStringLiteral("guest-build.prop-")));
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, sp, 24, 0)->steps(ctx), sink));
        int wrote = -1, ran = -1;
        for (int i = 0; i < sp.calls.size(); ++i) {
            if (wrote < 0 && sp.calls.at(i).contains(QLatin1String("vulkan=pastel"))) wrote = i;
            if (ran < 0 && sp.calls.at(i).startsWith(QLatin1String("docker run"))) ran = i;
        }
        QVERIFY2(wrote >= 0, "the guest overlay was never generated");
        QVERIFY2(ran < 0 || wrote < ran, "the overlay must exist before `docker run` mounts it");
        QVERIFY(hasCall(sp, ":/vendor/build.prop:ro"));  // and the container mounts it
        // …while HOST mode must not touch it: no path, no generation, no bind.
        FakeSpawner hsp;
        RemoraConfig host;
        host.gpu.mode = GpuMode::Host;
        auto hctx = makeContext(host, Backend::Bare);
        QVERIFY(!hctx.rc.buildPropOverlay.has_value());
        RecordingSink hsink;
        QVERIFY(runChain(makeDeployer(Backend::Bare, hsp, 24, 0)->steps(hctx), hsink));
        for (const QString &c : hsp.calls) QVERIFY2(!c.contains(QLatin1String("build.prop")), qPrintable(c));
    }

    // The remote twin: the same ONE script rides the ssh carriage, and a remote context resolves
    // the overlay path under the REMOTE home (~/.cache/remora) — the bind source must exist on
    // the machine running docker, never on this one.
    void remoteGuestModeGeneratesOverlayOnTheRemote() {
        FakeSpawner sp;
        auto ctx = makeContext(withGuest(), Backend::Remote);
        QVERIFY(ctx.rc.buildPropOverlay.has_value());
        QVERIFY(ctx.rc.buildPropOverlay->startsWith(QStringLiteral("~/.cache/remora/")));
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        QVERIFY(calls.contains(QStringLiteral("vulkan=pastel")));
        QVERIFY(calls.contains(QStringLiteral(":/vendor/build.prop:ro")));
    }

    // bd remora-emoe — THE RENDER SERVER MUST NEVER BE HANDED A FRAME SOCKET. Its publisher is
    // gated on REMORA_FRAME_SOCKET existing in its environment, so withholding the variable is the
    // whole mechanism that keeps the interception off; nothing in any argv would reveal its return,
    // which is exactly why it is pinned here rather than left to review.
    void venusServerNeverGetsTheFrameSocket() {
        FakeSpawner sp;
        sp.venusProcAlive = false;      // no incumbent, so the step SPAWNS one —
        sp.venusSocketPresent = false;  // otherwise it adopts and never builds an environment
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.hostEncode = true;
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        ctx.rc.hostEncodeFrameSocket = QString("/tmp/remora-frames-x/frame.sock");
        ensureVenusServer(sp, ctx.rc, {});
        QVERIFY2(!sp.detachedEnv.contains(QStringLiteral("REMORA_FRAME_SOCKET")),
                 "the render server was given a frame socket — the interception is back on");
        // …while the rest of the venus environment is untouched. This retires the FRAME source, not
        // the rendering path, and confusing the two would kill the GPU.
        QCOMPARE(sp.detachedEnv.value(QStringLiteral("REMORA_VIDEO_CAPTURE")), QStringLiteral("1"));
    }

    // bd remora-emoe, retiring bd remora-4u4.5 — a running server that publishes NOTHING is now
    // correct, and must be adopted rather than refused.
    //
    // 4u4.5 was real: capture lived inside the render server and was fixed at start, so two
    // host_encode profiles sharing a venusSocketDir left the second one's encoder waiting on a
    // socket nothing ever wrote to — a black mirror with no error anywhere — and the guard existed
    // to name that instead. Capture no longer lives there at all: each container's own hwcomposer
    // publishes to its own frame socket, so a shared server feeding several profiles is fine and
    // "not publishing" is the normal state of every server. The guard could only produce false
    // refusals, and managed one against a healthy profile on the first deploy after the switch.
    void venusAdoptsAServerThatPublishesNothing() {
        FakeSpawner sp;
        sp.venusProcAlive = true;        // a server IS up on this socket path…
        sp.venusSocketPresent = true;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeFrameSocket = QString("/tmp/remora-frames-second.sock");
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        const StepResult r = ensureVenusServer(sp, ctx.rc, {});
        QVERIFY2(r.ok, qPrintable(QStringLiteral("refused a healthy server: %1").arg(r.detail)));
    }

    // bd remora-e5x.18.9 — the heap tripwire rides in the ENVIRONMENT, so nothing about the argv
    // would reveal its loss, and the encoder runs rarely enough that nobody would notice for
    // months. Pinned for that reason alone: this asserts the env is DELIVERED, which is a
    // different claim from the tunable being useful. It may well not be — see the call site: on
    // glibc 2.43 three synthetic corruptions abort identically with and without it, because the
    // default hardening already catches them.
    void hostEncoderCarriesTheHeapTripwireInItsEnvironment() {
        FakeSpawner sp;
        sp.hostEncoderProcAlive = false;  // no incumbent to adopt, so the step must SPAWN one
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeFrameSocket = QString("/tmp/remora-frames-tripwire.sock");
        ctx.rc.hostEncodeVideoSocket = QString("/tmp/remora-video-tripwire.sock");
        bool starved = false;
        ensureHostEncoder(sp, ctx.rc, {}, &starved);
        // The call site rewrites argv[0] to the resolved ABSOLUTE path, so match the flag that
        // only this spawn carries rather than the bare binary name.
        QVERIFY2(hasCall(sp, "DETACH ") && hasCall(sp, "remora-frame-encoder --frame-socket"),
                 "the encoder must actually be spawned for this test to mean anything");
        QCOMPARE(sp.detachedEnv.value(QStringLiteral("GLIBC_TUNABLES")),
                 QStringLiteral("glibc.malloc.check=3"));
    }

    // bd remora-81cp — a driver update without a reboot: the parent server stays healthy on the
    // old module, each per-client FORK dies with 'NVRM: API mismatch', and the deploy used to
    // produce a container whose SurfaceFlinger crash-loops — investigated as an image regression
    // while the host was the cause. The process/socket health checks cannot see it, so the step
    // must compare versions up front, refuse, and name the reboot.
    void venusRefusesOnNvidiaAbiMismatch() {
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        {
            FakeSpawner sp;
            sp.venusProcAlive = true;  // everything else looks perfectly healthy
            sp.venusSocketPresent = true;
            sp.nvidiaVersionScanOut = QStringLiteral(
                "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.03  Release "
                "Build  (portage@localhost)  Mon Aug 10 04:31:19 PM EDT 2026\n"
                "/usr/lib64/libnvidia-glcore.so.610.57.04\n");
            const StepResult r = ensureVenusServer(sp, ctx.rc, {});
            QVERIFY(!r.ok);
            QVERIFY(r.detail.contains(QStringLiteral("610.43.03")));  // names BOTH versions…
            QVERIFY(r.detail.contains(QStringLiteral("610.57.04")));
            QVERIFY(r.detail.contains(QStringLiteral("Reboot")));     // …and the remedy
        }
        // Matched versions fall straight through to the normal health checks.
        {
            FakeSpawner sp;
            sp.venusProcAlive = true;
            sp.venusSocketPresent = true;
            sp.nvidiaVersionScanOut = QStringLiteral(
                "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.57.04  Release "
                "Build\n/usr/lib64/libnvidia-glcore.so.610.57.04\n");
            QVERIFY(ensureVenusServer(sp, ctx.rc, {}).ok);
        }
    }

    // bd remora-7qs — the server binds its socket with no mode of its own, so it lands at
    // 0777 & ~umask: the umask of whoever started Remora. At 022 that is srwxr-xr-x, and connect(2)
    // needs write, so inside the container SurfaceFlinger (uid 1000) connects while every app UID
    // gets EPERM — the guest half-works and each accelerated app dies on "Assertion failed:
    // !gpuCount", with no host-side step reporting anything wrong. Chmod it.
    void venusServerSocketIsReachableFromTheContainer() {
        FakeSpawner sp;
        sp.venusProcAlive = false;  // nothing up yet → it gets started
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeFrameSocket = QString("/tmp/remora-frames-mine.sock");
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        ctx.rc.width = 3760;
        ctx.rc.height = 1992;
        QVERIFY(ensureVenusServer(sp, ctx.rc, {}).ok);
        QVERIFY(hasCall(sp, "chmod 0777 /run/venus/venus.sock"));
        // REMORA_FRAME_SOCKET is deliberately NOT asserted here — the render server never receives
        // one now (bd remora-emoe); venusServerNeverGetsTheFrameSocket owns that invariant.
        QCOMPARE(sp.detachedEnv.value(QStringLiteral("REMORA_VIDEO_CAPTURE")),
                 QStringLiteral("1"));
        // …and WHICH display it publishes. The server labels by extent, which cannot survive the
        // second Android display an app window creates (bd remora-ljm); the size is baked at
        // `docker run`, so the host states it rather than letting the server guess.
        QCOMPARE(sp.detachedEnv.value(QStringLiteral("REMORA_DISPLAY_SIZE")),
                 QStringLiteral("3760x1992"));

        // A render-only profile shares the server and captures nothing, so it must be told
        // neither — frame capture is fixed at server start and belongs to one profile.
        FakeSpawner sp2;
        sp2.venusProcAlive = false;
        ctx.rc.hostEncode = false;
        QVERIFY(ensureVenusServer(sp2, ctx.rc, {}).ok);
        QVERIFY(!sp2.detachedEnv.contains(QStringLiteral("REMORA_FRAME_SOCKET")));
        QVERIFY(!sp2.detachedEnv.contains(QStringLiteral("REMORA_VIDEO_CAPTURE")));
        QVERIFY(!sp2.detachedEnv.contains(QStringLiteral("REMORA_DISPLAY_SIZE")));
        QVERIFY(hasCall(sp2, "chmod 0777 /run/venus/venus.sock"));  // reachability is unconditional
    }

    // …and the single-instance case must stay healthy: same server, same profile, no restart.
    void venusReusesARunningServer() {
        FakeSpawner sp;
        sp.venusProcAlive = true;
        sp.venusSocketPresent = true;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeFrameSocket = QString("/tmp/remora-frames-mine.sock");
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        const StepResult r = ensureVenusServer(sp, ctx.rc, {});
        QVERIFY(r.ok);
        // reused, not restarted. Matched on the DETACH marker the fake actually records — a
        // check for the literal "spawnDetached" would never match and would pass vacuously.
        QVERIFY(!hasCall(sp, "DETACH /opt/venus/bin/virgl_test_server"));
    }

    // A render-only profile shares the server by design (--multi-clients) and must not be
    // refused. Kept alongside venusAdoptsAServerThatPublishesNothing because it pins the same
    // adoption for hostEncode=false — the two used to take different paths through the capture
    // guard, and now deliberately take the same one.
    void venusRenderOnlyProfileSharesTheServer() {
        FakeSpawner sp;
        sp.venusProcAlive = true;
        sp.venusSocketPresent = true;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.hostEncode = false;  // render only
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        QVERIFY(ensureVenusServer(sp, ctx.rc, {}).ok);
    }

    // bd remora-e5x.14 — A STARVED MIRROR IS ALIVE, so liveness alone cannot judge it. When the
    // video comes from the host encoder, a mirror that connected but was never SERVED sits blocked
    // waiting for a stream header: it passes every liveness check, logs nothing, and the connect
    // reported success with its pid while no window ever appeared. Observed exactly that.
    // bd remora-82x, and the bd remora-e5x.19 flickering it turned out to cause. A REJECTED attempt
    // must actually go away, and `kill det.pid` missed twice over: det.pid is spawnDetached's pid —
    // systemd-run's, not the mirror's — and a starved client ignored SIGTERM anyway (observed
    // live: alive in futex_wait 84 SECONDS after a TERM, gone instantly on a KILL). The survivor
    // became a SECOND client on the one host encoder — the heavy black flickering — while the
    // connect step reported only the replacement's pid, so nothing admitted a second window existed.
    // Reproduced from the outside: one `remora reconnect` left two mirrors 15s apart.
    void rejectedMirrorAttemptIsReapedAndEscalatedToKill() {
        FakeSpawner sp;
        sp.servedConsumers = 0;  // every attempt is rejected as starved
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        // 111 predates this connect and the attempt's reap must spare it; 222 is what this attempt
        // spawned. Asked by the husk reap, the predecessor snapshot and the pre-launch reap first
        // (with no splash that one stops 111 on its own account, as the pkill did), then per
        // attempt and per reap. The session is untitled, which matches only the legacy "Remora:"
        // prefix — so the mirrors are titled that way.
        const QString legacy = QStringLiteral("Remora:legacy");
        const MirrorEntry prior = mirrorAt(111, ctx.rc.target, legacy);
        const MirrorEntry spawned = mirrorAt(222, ctx.rc.target, legacy, MirrorKind::Main,
                                             MirrorSessionState::Pending, 500);
        sp.mirrorSnapshots = {{prior}, {prior}, {prior}, {prior, spawned}};
        RecordingSink sink;
        doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0, /*livenessDelayMs=*/0,
                  /*mirrorRetries=*/2, /*bootWaitMs=*/240000, /*waitLit=*/false,
                  /*bootStableMs=*/0, /*mirrorHoldMs=*/0);
        // The attempt's real pid AND the spawn pid go, and whatever ignores the graceful stop is
        // killed outright rather than left to flicker; the predecessor is spared.
        QVERIFY2(stopped(sp, {222, 4242}), qPrintable(sp.calls.join(QLatin1Char('\n'))));
        for (const FakeSpawner::Stop &st : sp.stops)
            QVERIFY(!(st.pids.contains(4242) && st.pids.contains(111)));
        QVERIFY(!hasCall(sp, "kill "));
    }

    // bd remora-c3h. play-spoof and a first boot both restart adbd immediately before connect, so
    // the opening seconds legitimately read: endpoint answers, device says "offline", device comes
    // good. That must NOT eat the reachability budget — measured once at 30s of it, ending in a
    // failure that recommended installing a macvlan shim to a host whose shim was working and
    // whose ping to the container was 0.03 ms.
    void offlineDeviceWaitsOnItsOwnBudgetNotTheRetryCount() {
        FakeSpawner sp;
        // Answers "offline" far more times than the retry budget allows, then comes online.
        sp.adbStates = QStringList{"offline", "offline", "offline", "offline", "offline", "offline",
                                   "offline", "offline", "offline", "offline", "device"};
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/4, /*retryDelayMs=*/1000,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/1,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0,
                                       /*mirrorHoldMs=*/0);
        // More offline answers were consumed than there were tries, so the waiting cannot have
        // come out of the retry budget.
        QVERIFY2(r.ok, qPrintable(r.detail + " / " + r.stderrTail));
        // …and the loop's own remedies still ran first. Waiting patiently INSTEAD of reapplying
        // netfix and trying adbd directly is how a dead fwd shim turned into a 48.9s connect that
        // two seconds of remedy would have fixed.
        QVERIFY2(hasCall(sp, "netfix"), "the first-failure netfix reapply must not be skipped");
    }

    // …and when it never does come online, the failure must say THAT, not blame the network. An
    // endpoint that replied "offline" is reachable by construction.
    void answeringButOfflineDeviceIsNotReportedAsARoutingFailure() {
        FakeSpawner sp;
        sp.adbStates = QStringList{"offline"};  // answers, never recovers
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        auto ctx = makeContext(withGuest(cfg), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/2, /*retryDelayMs=*/1000,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/1,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0,
                                       /*mirrorHoldMs=*/0);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("answered but the device never came online")),
                 qPrintable(r.detail));
        // none of the layer-2 remedies, which would send the operator at the wrong thing entirely
        QVERIFY2(!r.detail.contains(QStringLiteral("layer 2")), qPrintable(r.detail));
        QVERIFY2(!r.detail.contains(QStringLiteral("macvlan-shim")), qPrintable(r.detail));
    }

    // The routing diagnosis reads THIS host's tables with `ip`, which a Windows client lacks — its
    // empty answers read as "no ARP entry", so every failed connect there blamed layer 2 and handed
    // over Linux route commands (bd remora-7oih.2). Windows gets the plain failure instead.
    void windowsConnectFailureBlamesNoRoute() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        sp.adbStates = QStringList{QStringLiteral("unknown")};  // nothing ever answers
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        auto ctx = makeContext(withGuest(cfg), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/2, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/1);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("not ready after 2 tries")), qPrintable(r.detail));
        QVERIFY2(!r.detail.contains(QStringLiteral("layer 2")), qPrintable(r.detail));
        QVERIFY(!hasCall(sp, "ip route"));
        QVERIFY(!hasCall(sp, "ip neigh"));
        // the same failure on Linux still runs the diagnosis
        FakeSpawner lin;
        lin.adbStates = QStringList{QStringLiteral("unknown")};
        RecordingSink lsink;
        QVERIFY(!doConnect(lin, ctx, lsink, 2, 0, 0, 1).ok);
        QVERIFY(hasCall(lin, "ip neigh show"));
    }

    void connectFailsWhenTheEncoderServesNobody() {
        FakeSpawner sp;
        sp.servedConsumers = 0;  // the mirror attached, but nothing is being served
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/2);
        QVERIFY(!r.ok);  // the old bug: this reported success
    }

    void connectSucceedsWhenTheEncoderIsServingTheMirror() {
        FakeSpawner sp;
        sp.servedConsumers = 1;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        QVERIFY(doConnect(sp, ctx, sink, 5, 0, 0, 2).ok);
    }

    // FAIL-SAFE. The probe needs `ss`; a host without it reports -1, which must read as UNKNOWN and
    // let the connect through. Refusing a working mirror because a diagnostic tool is missing would
    // be a worse bug than the false ok this check exists to close.
    void connectIgnoresTheEncoderCheckWhenItCannotBeProbed() {
        FakeSpawner sp;
        sp.servedConsumers = -1;  // no `ss` on this host
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        QVERIFY(doConnect(sp, ctx, sink, 5, 0, 0, 2).ok);
    }

    // The check must not fire for the ordinary device-encoded mirror: there the video never goes
    // near the host encoder's socket, so "serving nobody" is the normal, correct state.
    void connectIgnoresTheEncoderCheckWhenTheDeviceDrivesTheMirror() {
        FakeSpawner sp;
        sp.servedConsumers = 0;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = false;  // mirror still takes video from the device
        RecordingSink sink;
        QVERIFY(doConnect(sp, ctx, sink, 5, 0, 0, 2).ok);
    }

    // ── bd remora-7ps: the serve gate — connect must not succeed over a dead video path ──
    //
    // The incident: reconnect's predecessor teardown SIGSEGV'd the encoder; the fresh client had
    // an ESTAB on the video socket (connection-level health), so connect printed ✓ — and seconds
    // later the mirror's no-first-frame guard took the unserved client down, leaving NO mirror and
    // NO encoder behind a green step. The gate demands DATA-level proof: a new 'first decodable
    // packet' line in the encoder's own log, or a named failure carrying that log's tail.

    void connectFailsWhenTheEncoderDiesBeforeServingTheFreshMirror() {
        FakeSpawner sp;
        sp.servedConsumers = 1;             // connection-level evidence looks perfectly healthy
        sp.hostEncoderProcAlive = false;    // …but the encoder process is gone
        sp.firstPacketCounts = {QStringLiteral("0"), QStringLiteral("0")};  // and nobody was served
        sp.hostEncoderLogTail = QStringLiteral("[encoder] 512 ms: SIGSEGV was here");
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 2);
        QVERIFY(!r.ok);  // the old bug: this combination reported a successful connect
        QVERIFY2(r.detail.contains(QStringLiteral("host encoder DIED")), qPrintable(r.detail));
        // the encoder's log rides along as evidence — it is the only place the death explains itself
        QVERIFY2(r.stderrTail.contains(QStringLiteral("SIGSEGV was here")),
                 qPrintable(r.stderrTail));
    }

    void separateDisplayDoesNotDependOnTheMainHostEncoder() {
        for (bool app : {false, true}) {
            FakeSpawner sp;
            sp.hostEncoderProcAlive = false;
            sp.servedConsumers = 0;
            sp.firstPacketCounts = {QStringLiteral("0")};
            RemoraConfig cfg = withGuest();
            cfg.mirror.fullscreenSeparateDisplay = true;
            if (app) cfg.mirror.fullscreenApp = QStringLiteral("software.sonar.sonar_mobile");
            auto ctx = makeContext(cfg, Backend::Bare, std::nullopt, {}, true);
            ctx.rc.hostEncode = true;
            ctx.rc.hostEncodeDrivesMirror = true;
            RecordingSink sink;
            const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
            QVERIFY2(r.ok, qPrintable(r.detail));
            QVERIFY(!r.detail.contains(QStringLiteral("serve unconfirmed")));
            QVERIFY(!hasCall(sp, "remora-frame-encoder"));
            QVERIFY(!hasCall(sp, "first decodable packet"));
            QVERIFY(!hasCall(sp, "stop && start"));
        }
    }

    void fullscreenMirrorStillRequiresItsHostEncoder() {
        FakeSpawner sp;
        sp.hostEncoderProcAlive = false;
        sp.servedConsumers = 1;
        sp.firstPacketCounts = {QStringLiteral("0")};
        auto ctx = makeContext(withGuest(), Backend::Bare, std::nullopt, {}, true);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("host encoder DIED")), qPrintable(r.detail));
    }

    // A dead encoder is named with ITS last words, not a guess. The incident: an encoder that
    // landed on the wrong GPU died with "opening the encoder: Bad address" while half the card
    // was free, and the note sent the reader to nvidia-smi.
    void deadEncoderNoteQuotesTheEncodersOwnLog() {
        FakeSpawner sp;
        sp.detachedDead = true;  // every mirror attempt exits at once
        sp.hostEncoderProcAlive = false;
        sp.deadEncoderLog = QStringLiteral(
            "/tmp/remora-detached-9-3.log\n"
            "remora-frame-encoder: opening the encoder: Bad address\n");
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 2);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("NOT running")), qPrintable(r.detail));
        QVERIFY2(r.detail.contains(QStringLiteral("/tmp/remora-detached-9-3.log")),
                 qPrintable(r.detail));
        QVERIFY2(r.detail.contains(QStringLiteral("opening the encoder: Bad address")),
                 qPrintable(r.detail));
        QVERIFY2(!r.detail.contains(QStringLiteral("nvidia-smi")), qPrintable(r.detail));

        // No log found keeps the old advice rather than saying nothing.
        FakeSpawner bare;
        bare.detachedDead = true;
        bare.hostEncoderProcAlive = false;
        const StepResult g = doConnect(bare, ctx, sink, 5, 0, 0, 2);
        QVERIFY2(g.detail.contains(QStringLiteral("nvidia-smi")), qPrintable(g.detail));
    }

    void connectFailsWhenTheFreshMirrorDiesUnserved() {
        FakeSpawner sp;
        sp.servedConsumers = 1;
        sp.detachedDiesOnCall = 2;  // alive at the liveness check, dead when the gate re-asks
        sp.firstPacketCounts = {QStringLiteral("0"), QStringLiteral("0")};  // never served
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("mirror exited before the host encoder")),
                 qPrintable(r.detail));
    }

    // FAIL-OPEN (the bd remora-82x lesson): both processes alive but no first-packet line within
    // grace is a PASS with a note, never a veto — a slow serve must not cost a healthy mirror.
    void connectPassesWithANoteWhenServeIsUnconfirmed() {
        FakeSpawner sp;
        sp.servedConsumers = 1;
        sp.firstPacketCounts = {QStringLiteral("0"), QStringLiteral("0")};  // line never appears
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 2);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QVERIFY2(r.detail.contains(QStringLiteral("serve unconfirmed")), qPrintable(r.detail));
    }

    // SERVED is not SURVIVED (bd remora-10bt): a mirror can clear liveness AND the serve gate and
    // still be evicted by the encoder 1.5-2 s later (a consumer stalled past the 500 ms send
    // deadline under desktop-stream GPU contention — observed live, twice in one evening). The
    // survival hold must catch the death and hand the retry loop its chance…
    void connectRetriesWhenAServedMirrorDiesInTheSurvivalHold() {
        FakeSpawner sp;
        sp.servedConsumers = 1;
        // detachedAlive call #1 is the liveness check; the serve gate exits on its data fast
        // path without asking; call #2 is the survival hold's first re-ask — die exactly there.
        sp.detachedDiesOnCall = 2;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 2);
        QVERIFY2(r.ok, qPrintable(r.detail));  // attempt 2 survives the hold — the retry worked
    }

    // …and when every attempt dies there, the step must FAIL with the eviction story, never
    // print ✓ for a corpse (the "says its connected" report this hold exists to end).
    void connectFailsWhenEveryServedMirrorDiesInTheHold() {
        FakeSpawner sp;
        sp.servedConsumers = 1;
        sp.detachedDiesOnCall = 2;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QStringLiteral("seconds after being served")),
                 qPrintable(r.detail));
    }

    // The publisher gate (bd remora-r6au): an encoder reporting publishers=0 is up, fresh and
    // bound with nothing composing behind it — the gate wakes the device (a composed frame is
    // both the retry tick and the proof) and, per the bd remora-82x lesson, FAILS OPEN when the
    // publisher still has not returned: a warning, never a veto. An encoder that predates the
    // counts (or a healthy one) must not be woken at all.
    void connectWakesTheDeviceWhenTheEncoderHasNoPublisher() {
        struct RecordingFake : FakeSpawner {
            QStringList cmds;
            ProcResult run(const QStringList &argv, const QMap<QString, QString> &env,
                           const LineSink &log) override {
                cmds << argv.join(QLatin1Char(' '));
                return FakeSpawner::run(argv, env, log);
            }
        };
        RecordingFake sp;
        sp.servedConsumers = 1;
        sp.encoderStatusOut = QStringLiteral(
            "state=starting native=0x0 encode=0x0 bitrate=0 degrade=0 pid=7 "
            "publishers=0 consumers=0\n");
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY2(r.ok, qPrintable(r.detail));  // fail-open: warned, not vetoed
        QVERIFY(sp.cmds.filter(QStringLiteral("input keyevent 224")).size() == 1);

        // Healthy counts: no wake. And an encoder with no counts at all (pre-r6au binary or no
        // status file): also no wake — unknown is not zero.
        RecordingFake healthy;
        healthy.servedConsumers = 1;
        healthy.encoderStatusOut = QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=7 "
            "publishers=1 consumers=1\n");
        RecordingSink sink2;
        QVERIFY(doConnect(healthy, ctx, sink2, 5, 0, 0, 1).ok);
        QVERIFY(healthy.cmds.filter(QStringLiteral("keyevent 224")).isEmpty());
        RecordingFake preR6au;  // default empty status = no file
        preR6au.servedConsumers = 1;
        RecordingSink sink3;
        QVERIFY(doConnect(preR6au, ctx, sink3, 5, 0, 0, 1).ok);
        QVERIFY(preR6au.cmds.filter(QStringLiteral("keyevent 224")).isEmpty());
    }

    // bd remora-pobz item 2: a flat stream WITH the guest's RenderEngine dead is the one state
    // every other gate certifies as healthy, and it is worth the 15 s framework restart before a
    // mirror is handed over black. Both signals required — see the false-positive test below.
    void connectHealsABlackStreamWhenTheGuestRenderEngineDied() {
        struct RecordingFake : FakeSpawner {
            QStringList cmds;
            ProcResult run(const QStringList &argv, const QMap<QString, QString> &env,
                           const LineSink &log) override {
                cmds << argv.join(QLatin1Char(' '));
                return FakeSpawner::run(argv, env, log);
            }
        };
        RecordingFake sp;
        sp.servedConsumers = 1;
        sp.guestCreateDeviceAsserts = 7;  // the storm
        sp.encoderStatusOut = QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=7 "
            "publishers=1 consumers=1 black_stream=1\n");
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QVERIFY(!sp.cmds.filter(QStringLiteral("stop && start")).isEmpty());
    }

    // …and the false positive that must NOT cost a session: a genuinely dark screen (a game's
    // black loading scene) trips black_stream honestly while the guest is perfectly healthy.
    // Warn, never restart — the remedy is destructive to whatever the user was running.
    void connectDoesNotRestartTheFrameworkForAMerelyDarkScreen() {
        struct RecordingFake : FakeSpawner {
            QStringList cmds;
            ProcResult run(const QStringList &argv, const QMap<QString, QString> &env,
                           const LineSink &log) override {
                cmds << argv.join(QLatin1Char(' '));
                return FakeSpawner::run(argv, env, log);
            }
        };
        RecordingFake sp;
        sp.servedConsumers = 1;
        sp.guestCreateDeviceAsserts = 0;  // the guest is fine; the content is just dark
        sp.encoderStatusOut = QStringLiteral(
            "state=ok native=3760x1992 encode=3760x1992 bitrate=30000000 degrade=0 pid=7 "
            "publishers=1 consumers=1 black_stream=1\n");
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QVERIFY(sp.cmds.filter(QStringLiteral("stop && start")).isEmpty());
    }

    // The healthy fast path: the serve line predates the gate (live timing: ~1.2 s serve, the
    // gate asks after liveness + hold), so a served mirror carries no note and pays no wait.
    // And when the encoder's log cannot be resolved at all, the gate degrades to its death
    // checks alone rather than idling out the grace window on a log it cannot read.
    void connectServeGateIsSilentWhenServedAndDegradesWithoutALog() {
        FakeSpawner sp;  // defaults: log resolves, baseline 0, gate reads 1 — served
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;
        const StepResult served = doConnect(sp, ctx, sink, 5, 0, 0, 2);
        QVERIFY2(served.ok, qPrintable(served.detail));
        QVERIFY2(!served.detail.contains(QStringLiteral("serve unconfirmed")),
                 qPrintable(served.detail));

        FakeSpawner sp2;
        sp2.hostEncoderLogPath.clear();  // resolution fails — no data-level evidence exists
        auto ctx2 = makeContext(withGuest(), Backend::Bare);
        ctx2.rc.hostEncode = true;
        ctx2.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink2;
        const StepResult degraded = doConnect(sp2, ctx2, sink2, 5, 0, 0, 2);
        QVERIFY2(degraded.ok, qPrintable(degraded.detail));
        QVERIFY2(!degraded.detail.contains(QStringLiteral("serve unconfirmed")),
                 qPrintable(degraded.detail));
    }

    // A STALE SOCKET must not read as health. A killed server leaves its socket inode behind, so a
    // process check alone says "gone" while a socket check alone says "fine" — only together do
    // they describe the state that actually broke: file present, nobody listening, ECONNREFUSED.
    void connectRestartsVenusWhenSocketIsStale() {
        FakeSpawner sp;
        sp.venusProcAlive = false;      // dead
        sp.venusSocketPresent = true;   // ...but its socket file is still there
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = true;
        ctx.rc.venusServerPath = QString("/opt/venus/bin/virgl_test_server");
        ctx.rc.venusSocketDir = QString("/run/venus");
        RecordingSink sink;
        QVERIFY(doConnect(sp, ctx, sink, 5, 0, 0, 3).ok);
        QVERIFY(hasCall(sp, "virgl_test_server"));
        QVERIFY(hasCall(sp, "rm"));  // the stale inode is cleared before the restart
    }

    // venus=false must leave connect byte-identical to before the feature existed.
    void connectDoesNotTouchVenusWhenOff() {
        FakeSpawner sp;
        sp.venusProcAlive = false;
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.venus = false;
        RecordingSink sink;
        QVERIFY(doConnect(sp, ctx, sink, 5, 0, 0, 3).ok);
        QVERIFY(!hasCall(sp, "virgl_test_server"));
    }

    // bd remora-4ei.65 — a wedged screencap must DELAY the connect, not block it. The lit-screen
    // gate budgets 60s by counting loop iterations, which silently assumes every probe returns;
    // screencap can hang forever on the Venus/NVIDIA stack, and `remora up` then sat at the connect
    // step indefinitely with the container up and rendering. Each probe is now bounded, and a
    // timeout ends the gate rather than restarting it.
    void connectProceedsWhenLitScreenProbeWedges() {
        FakeSpawner sp;
        sp.litProbeTimesOut = true;  // every screencap probe returns 124 (timeout killed it)
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r =
            doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0, /*livenessDelayMs=*/0,
                      /*mirrorRetries=*/3, /*bootWaitMs=*/240000, /*waitLit=*/true);
        QVERIFY(r.ok);                                   // the mirror opens regardless
        QCOMPARE(countCalls(sp, "mirror -s"), 1);
        // and it gives up after ONE wedged probe instead of re-polling for the whole budget
        QCOMPARE(countCalls(sp, "screencap -p 2>/dev/null | wc -c"), 1);
        // The bound is the spawner's deadline, not a local coreutils `timeout`: Windows' timeout.exe
        // is a different program, so there every probe failed at once and the gate spent its whole
        // 60 s on each deploy (bd remora-7oih.2).
        QVERIFY(!spawnsProgram(sp, "timeout"));
        QCOMPARE(sp.timedCallsMs, QList<int>{8000});
    }

    // The real halves of the three seams that replaced local shell plumbing (bd remora-7oih.2).
    // Offline: /bin/sh and printf stand in for adb, and the tail reads a file written here.
    void realSpawnerSeamsBehaveLikeTheShellTheyReplace() {
        SKIP_ON_WINDOWS("the stand-ins are POSIX tools");
        QTemporaryDir tmp;
        RealSpawner sp;
        // runToFile carries bytes run() would mangle: a NUL, a stray 0xFF, no trailing newline
        const QString png = tmp.filePath(QStringLiteral("shot.png"));
        QCOMPARE(sp.runToFile({"printf", "\\211PNG\\000\\377"}, png).rc, 0);
        QFile f(png);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("\x89PNG\0\xff", 6));
        // runTimed reports 124, as `timeout` did, and does not wait the child out
        QElapsedTimer t;
        t.start();
        QCOMPARE(sp.runTimed({"sh", "-c", "sleep 30"}, 300).rc, 124);
        QVERIFY(t.elapsed() < 10000);
        QCOMPARE(sp.runTimed({"sh", "-c", "echo ok; exit 3"}, 5000).rc, 3);
        // readTail is `tail -n`: the last N lines, a trailing newline ending the last one, the
        // whole file when it is shorter, nothing for a file that is not there — and the same
        // answer from a file far bigger than its first read window.
        const QString log = tmp.filePath(QStringLiteral("m.log"));
        QFile w(log);
        QVERIFY(w.open(QIODevice::WriteOnly));
        for (int i = 0; i < 5000; ++i) w.write(QByteArray("line ") + QByteArray::number(i) + "\n");
        w.close();
        QCOMPARE(sp.readTail(log, 3), QStringLiteral("line 4997\nline 4998\nline 4999\n"));
        QCOMPARE(sp.readTail(log, 400).count(QLatin1Char('\n')), 400);
        QVERIFY(sp.readTail(log, 400).startsWith(QStringLiteral("line 4600\n")));
        QVERIFY(w.open(QIODevice::WriteOnly | QIODevice::Truncate));
        w.write("a\nb");
        w.close();
        QCOMPARE(sp.readTail(log, 1), QStringLiteral("b"));
        QCOMPARE(sp.readTail(log, 8), QStringLiteral("a\nb"));
        QCOMPARE(sp.readTail(tmp.filePath(QStringLiteral("absent.log")), 8), QString());
    }

    // The mirror's dying words are read from ITS log in-process. They used to come from a local
    // `sh -c tail`, which on Windows answered nothing, so a mirror that had already declared itself
    // dead passed as connected (bd remora-7oih.2).
    void fatalLineInTheMirrorsOwnLogRejectsTheAttempt() {
        FakeSpawner sp;
        sp.detachedLog = QStringLiteral("/nonexistent/remora-detached-1-0.log");
        sp.fileTails.insert(sp.detachedLog,
                            QStringLiteral("connecting\nmirror: fatal: no first frame in 30 s\n"));
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/2);
        QVERIFY(!r.ok);  // a live process whose log says it gave up is not a connected mirror
        QCOMPARE(countCalls(sp, "TAIL 8 " + sp.detachedLog), 2);  // once per attempt
        QVERIFY(!hasCall(sp, "tail -n"));
        QVERIFY2(r.detail.contains(QStringLiteral("mirror: fatal: no first frame")),
                 qPrintable(r.detail));
    }

    // The boot-anim splash, adopted as the mirror, can pass the liveness check while still playing
    // its animation, then die at the hand-off to live mirroring — the recurring "first connect
    // closes, reconnect holds". A deploy (mirrorHoldMs>0) must notice the drop and relaunch a plain
    // mirror automatically instead of reporting a dead session. Call sequence with a splash set:
    // 1=adoptSplash probe, 2=attempt-1 liveness (both alive), 3=hold-window poll (drops here) ->
    // relaunch, 4=attempt-2 liveness on the fresh spawn (alive) -> success.
    void connectRelaunchesWhenAdoptedSplashDropsAtHandoff() {
        FakeSpawner sp;
        sp.detachedDiesOnCall = 3;  // adopted splash alive at adopt+liveness, drops during the hold
        auto ctx = makeContext(withGuest(), Backend::Remote);
        ctx.splash = std::make_shared<Detached>(Detached{999});  // pre-spawned boot-anim splash
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/3,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0, /*mirrorHoldMs=*/1000);
        QVERIFY(r.ok);
        // exactly one fresh mirror spawned (the adopted splash was ctx.splash, not a DETACH) — the
        // stable reconnect, now automatic
        QCOMPARE(countCalls(sp, "mirror -s"), 1);
    }

    // bd remora-82x, the half that survives a working attach gate. With host_encode driving the
    // mirror, the starved-consumer check ran one livenessDelayMs after REMORA_ATTACH was written —
    // before the adopted splash could possibly have noticed the marker, finished its outro, pushed
    // the server and opened the video socket. It therefore ALWAYS read "serving nobody", killed the
    // window the hand-off had just been arranged for, and let the retry spawn a replacement: the
    // drop-then-reappear, arrived at from the other direction. Captured live on a cold run whose
    // splash log stops dead after "Boot animation: pulled" with the marker already written and no
    // "[adopted splash]" in the connect detail. An arriving mirror must be WAITED for, not judged
    // on arrival — and only the ADOPTED first attempt gets that grace, because a plain spawn that
    // is serving nobody really is starved.
    void adoptedSplashIsWaitedForRatherThanCalledStarved() {
        FakeSpawner sp;
        sp.servedZeroFirst = 3;  // it becomes a consumer on the 4th probe, ~3s after the marker
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        ctx.splash = std::make_shared<Detached>(Detached{999});
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/3,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0, /*mirrorHoldMs=*/12000);
        QVERIFY(r.ok);
        QCOMPARE(countCalls(sp, "mirror -s"), 0);  // the splash kept the window: no replacement
        QVERIFY(r.detail.contains(QStringLiteral("[adopted splash]")));
    }

    // The converse, so the grace above cannot quietly become "never call anything starved": a
    // PLAIN spawn serving nobody is starved on the spot, and an adopted splash that never arrives
    // within the hold is too.
    void plainMirrorServingNobodyIsStillStarvedImmediately() {
        FakeSpawner sp;
        sp.servedConsumers = 0;  // never arrives
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        RecordingSink sink;  // no ctx.splash: nothing to adopt, so no grace
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/2,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0, /*mirrorHoldMs=*/12000);
        QVERIFY(!r.ok);
        // Judged at once: none of the 12s adopted-splash grace was spent on it. The waits that DO
        // happen are the reap's own TERM-then-KILL pause, one per rejected attempt — bounded by
        // that, and nowhere near a single hold.
        QVERIFY2(sp.waitedMs < 12000, qPrintable(QStringLiteral("waitedMs=%1").arg(sp.waitedMs)));
    }

    // bd remora-e5x.18.3. The encoder degrades AUTONOMOUSLY under VRAM pressure — 3760x1992 was
    // measured serving 944x504 with the story only in the encoder's stderr, which nothing
    // watches. The connect detail is the surface the CLI, GUI and desktop launcher all show, so
    // the published status must land there — and ONLY when something is actually reduced: a note
    // on every healthy connect is noise that trains the user to skip the one that matters.
    void degradedEncoderIsNamedInTheConnectDetailAndHealthyOneIsSilent() {
        auto connect = [&](const QString &status, StepResult *out) {
            FakeSpawner sp;
            sp.encoderStatusOut = status;
            auto ctx = makeContext(withGuest(), Backend::Bare);
            ctx.rc.hostEncode = true;
            ctx.rc.hostEncodeDrivesMirror = true;
            RecordingSink sink;
            *out = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                             /*livenessDelayMs=*/0, /*mirrorRetries=*/3,
                             /*bootWaitMs=*/240000, /*waitLit=*/false,
                             /*bootStableMs=*/0, /*mirrorHoldMs=*/1000);
        };
        StepResult degraded;
        connect(QStringLiteral("state=degraded native=3760x1992 encode=944x504 "
                               "bitrate=7500000 degrade=2 pid=7\n"),
                &degraded);
        QVERIFY(degraded.ok);
        QVERIFY2(degraded.detail.contains(QStringLiteral("DEGRADED to 944x504")),
                 qPrintable(degraded.detail));
        QVERIFY(degraded.detail.contains(QStringLiteral("3760x1992")));  // how far it fell

        StepResult healthy;
        connect(QStringLiteral("state=ok native=3760x1992 encode=3760x1992 "
                               "bitrate=30000000 degrade=0 pid=7\n"),
                &healthy);
        QVERIFY(healthy.ok);
        QVERIFY(!healthy.detail.contains(QStringLiteral("host encoder")));

        // No status file at all — an encoder from before status existed, or none running. The
        // absence of evidence must never read as a note.
        StepResult none;
        connect(QString(), &none);
        QVERIFY(none.ok);
        QVERIFY(!none.detail.contains(QStringLiteral("host encoder")));
    }

    // The hold ends on EVIDENCE, not on the clock: once the host encoder is serving this mirror,
    // the question the hold exists to answer ("did the window survive the hand-off?") has been
    // answered by a live session pulling frames. Spending the rest of the timer re-asking it is
    // pure latency on the cold-deploy path (bd remora-tdz).
    void adoptedSplashHoldEndsAsSoonAsTheEncoderServesIt() {
        FakeSpawner sp;
        sp.servedConsumers = 1;  // serving from the first probe
        auto ctx = makeContext(withGuest(), Backend::Bare);
        ctx.rc.hostEncode = true;
        ctx.rc.hostEncodeDrivesMirror = true;
        ctx.splash = std::make_shared<Detached>(Detached{999});
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/3,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0,
                                       /*mirrorHoldMs=*/12000);
        QVERIFY(r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("[adopted splash]")));
        QCOMPARE(countCalls(sp, "mirror -s"), 0);  // the splash was kept
        // one 1s liveness tick, then the encoder vouches for it — not the full 12s
        QVERIFY2(sp.waitedMs < 12000, qPrintable(QStringLiteral("waitedMs=%1").arg(sp.waitedMs)));
    }

    // A mirror that keeps holding past the hand-off must NOT be needlessly relaunched.
    void connectKeepsAdoptedSplashWhenItHolds() {
        FakeSpawner sp;  // splash stays alive across every check
        auto ctx = makeContext(withGuest(), Backend::Remote);
        ctx.splash = std::make_shared<Detached>(Detached{999});
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                                       /*livenessDelayMs=*/0, /*mirrorRetries=*/3,
                                       /*bootWaitMs=*/240000, /*waitLit=*/false,
                                       /*bootStableMs=*/0, /*mirrorHoldMs=*/1000);
        QVERIFY(r.ok);
        QCOMPARE(countCalls(sp, "mirror -s"), 0);  // adopted splash held — no fresh spawn
    }

    void connectFailsWhenMirrorKeepsDying() {
        FakeSpawner sp;
        sp.detachedDead = true;  // every launch dies immediately
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, /*mirrorRetries=*/3);
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("after 3 attempts")));
        QCOMPARE(countCalls(sp, "mirror -s"), 3);  // exhausted the full budget
    }

    // The one-button Connect: reconnect when the container runs, full deploy when it doesn't.
    void smartConnectReconnectsWhenRunning() {
        FakeSpawner sp;  // containerRunning defaults true
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(!hasCall(sp, "docker run"));  // no provision — straight to connect
        QVERIFY(hasCall(sp, "mirror -s"));
    }

    // A running container from LAST build's image must not be silently reattached — the tag
    // moved, so the fast path would hide the rebuild. Redeploy instead.
    void smartConnectRedeploysWhenImageStale() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerStale = true;
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));  // took the deploy chain (which recreates)
    }

    // …but an ADDITIONAL window must not pay for that recreate. Same stale container as above,
    // opened as an app popup instead of the mirror: recreating would docker-rm the device out from
    // under every window already on it, to open one popup nobody asked to redeploy for.
    static RemoraConfig appWindowProfile() {
        RemoraConfig cfg = withGuest();
        cfg.mirror.fullscreenSeparateDisplay = true;  // its own Android display
        cfg.mirror.fullscreenApp = QStringLiteral("com.anthropic.claude");
        return cfg;
    }

    void smartConnectNeverRedeploysForAnAppWindow() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerStale = true;                   // the very state that redeploys for the mirror
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, appWindowProfile(), sink, sp, std::nullopt, {}, {},
                         /*fullscreenDisplay=*/true));
        QVERIFY(!hasCall(sp, "docker run"));   // no recreate…
        QVERIFY(!hasCall(sp, "docker rm -f"));
        QVERIFY(hasCall(sp, "mirror -s"));  // …just the extra window
    }

    // A container that is not running still deploys — an extra window does need the device up.
    void smartConnectStillDeploysForAnAppWindowWhenDown() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");
        sp.containerRunning = false;
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, appWindowProfile(), sink, sp, std::nullopt, {}, {},
                         /*fullscreenDisplay=*/true));
        QVERIFY(hasCall(sp, "docker run"));
    }

    // Networking is baked at docker run like the image and the geometry, and this no-op is the
    // most invisible of the lot: switching a profile to macvlan reconnected onto the still-bridged
    // container, the deploy reported success, and the only symptom was an adb target that never
    // answered — while plan and check both described the macvlan deployment nobody had created.
    // Caught live on the first real bare macvlan deploy (bd remora-400).
    // The pair below differ ONLY in the container's network attachment, so what they prove is that
    // the network alone decides — everything else about the profile matches the running container.
    static RemoraConfig macvlanBareProfile() {
        RemoraConfig cfg;
        cfg.gpu.mode = GpuMode::Host;  // matches the fake's canned probe, so gpuOk is not the cause
        cfg.network.networkMode = QStringLiteral("macvlan");
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        return cfg;
    }

    void smartConnectRedeploysWhenNetworkModeChanged() {
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("bridge=172.17.0.2 ");  // created before the switch
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, macvlanBareProfile(), sink, sp));
        QVERIFY(hasCall(sp, "docker rm -f"));  // took the deploy chain, which recreates
        QVERIFY(hasCall(sp, "--network lan --ip 192.0.2.77"));
    }

    // …and the identical profile against a container ALREADY on that network takes the fast path,
    // so the check cannot be satisfied by recreating on every deploy.
    void smartConnectReconnectsWhenNetworkMatches() {
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, macvlanBareProfile(), sink, sp));
        QVERIFY(!hasCall(sp, "docker rm -f"));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // bd remora-6laf.8 — Google Play materializes BEFORE anything compares images: the probe has
    // to ask about the overlay the profile needs, and the deploy has to run that overlay. A failure
    // stops the run, because going on would recreate the device on the base, without Play.
    static RemoraConfig playBareProfile() {
        RemoraConfig cfg = macvlanBareProfile();
        cfg.image.androidVersion = 17;
        cfg.image.imageTag = QStringLiteral("remora24:x86_64-public");
        cfg.image.googlePlay = true;
        cfg.image.playTerms = playTermsVersion();
        return cfg;
    }

    void smartConnectDeploysTheGooglePlayOverlay() {
        SKIP_ON_WINDOWS(kBareHost);
        const QString overlay = QStringLiteral("remora-local:play-08c62ac56c4d-07db5d1f5dc7");
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");
        sp.containerStale = true;  // the running container is the bare base, not the overlay
        sp.playInstallOut = QStringLiteral("REMORA_PLAY_RESULT={\"tag\":\"%1\"}\n").arg(overlay);
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, playBareProfile(), sink, sp));
        QCOMPARE(sink.of("play-overlay"), StepState::Ok);
        int install = -1, probe = -1;
        for (int i = 0; i < sp.calls.size(); ++i) {
            if (install < 0 && sp.calls[i].contains("install.py")) install = i;
            if (probe < 0 && sp.calls[i].contains("State.Running") &&
                sp.calls[i].contains("{{.Image}}"))
                probe = i;
        }
        QVERIFY(install >= 0);
        QVERIFY(probe > install);
        QVERIFY(sp.calls[probe].contains(overlay));
        QVERIFY(sp.calls[install].contains("--base remora24:x86_64-public"));
        bool ranOverlay = false;
        for (const QString &c : sp.calls)
            if (c.startsWith("docker run") && c.contains(" " + overlay + " ")) ranOverlay = true;
        QVERIFY(ranOverlay);
    }

    // The in-Android updater's channel is a docker-run bind: a container created without it keeps
    // an updater that can neither see an update nor ask for one, so Connect recreates it — in the
    // fast path's check AND in provision, which a running container otherwise passes untouched.
    static RemoraConfig updaterProfile() {
        RemoraConfig cfg = macvlanBareProfile();
        cfg.image.androidVersion = 17;
        cfg.image.imageTag = QStringLiteral("remora24:x86_64-public");  // carries system_updater
        return cfg;
    }

    void smartConnectRecreatesAContainerWithoutTheUpdaterChannel() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");
        sp.containerBinds = QStringLiteral("/srv/data:/data-diff");  // made before the channel
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, updaterProfile(), sink, sp));
        QVERIFY(hasCall(sp, "docker rm -f"));
        const QString dir = QDir::homePath() + "/.local/share/remora/updater/remora-bm";
        QVERIFY(hasCall(sp, "-v " + dir + ":/rezmods/updater"));
        QVERIFY(hasCall(sp, "mkdir -p"));  // host-prereqs made the bind source first
    }

    void smartConnectReconnectsWhenTheUpdaterChannelIsThere() {
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");
        sp.containerBinds = QDir::homePath() +
                            "/.local/share/remora/updater/remora-bm:/rezmods/updater";
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, updaterProfile(), sink, sp));
        QVERIFY(!hasCall(sp, "docker rm -f"));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    void smartConnectStopsWhenGooglePlayCannotBeAdded() {
        FakeSpawner sp;
        sp.playInstallRc = 1;
        RecordingSink sink;
        QVERIFY(!smartRun(Backend::Bare, playBareProfile(), sink, sp));
        QCOMPARE(sink.of("play-overlay"), StepState::Failed);
        QVERIFY(!hasCall(sp, "State.Running"));
        QVERIFY(!hasCall(sp, "docker rm"));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // Devices are docker-run flags like the network above, and the camera is the one that drifts:
    // a container created while the camera was absent carries only its GPU nodes, and the fast
    // path used to reconnect onto it reporting healthy — with the resolver's /dev/video* never
    // reaching Android, and the manual `docker rm` smart connect exists to remove back in the
    // loop (bd remora-usn0). The trio below differ ONLY in the device lists, so the devices alone
    // decide.
    static RemoraConfig cameraBareProfile() {
        RemoraConfig cfg = macvlanBareProfile();
        cfg.input.cameraDevices = QStringList{QStringLiteral("/dev/video0")};
        return cfg;
    }

    void smartConnectRedeploysWhenCameraNodeMissing() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;  // containerDevices default carries no video node
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");  // network is NOT the cause
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, cameraBareProfile(), sink, sp));
        QVERIFY(hasCall(sp, "docker rm -f"));  // took the deploy chain, which recreates
        QVERIFY(hasCall(sp, "--device /dev/video0:/dev/video0"));
    }

    // …the identical profile against a container that already carries the node takes the fast
    // path, so the check cannot be satisfied by recreating on every deploy.
    void smartConnectReconnectsWhenCameraNodeMatches() {
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");
        sp.containerDevices = QStringLiteral("/dev/input/event5\n/dev/video0");
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Bare, cameraBareProfile(), sink, sp));
        QVERIFY(!hasCall(sp, "docker rm -f"));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // …and the reverse drift recreates too: a camera the profile no longer resolves — unplugged,
    // or explicitly switched off — must not stay passed through to a container that reports
    // healthy. Same silent config drift, pointing the other way.
    void smartConnectRedeploysWhenCameraUnplugged() {
        SKIP_ON_WINDOWS(kBareHost);
        FakeSpawner sp;
        sp.containerNetworks = QStringLiteral("lan=192.0.2.77 ");
        sp.containerDevices = QStringLiteral("/dev/input/event5\n/dev/video0");
        RecordingSink sink;
        RemoraConfig cfg = macvlanBareProfile();
        cfg.input.cameraDevices = QStringList{};  // explicitly OFF
        QVERIFY(smartRun(Backend::Bare, cfg, sink, sp));
        QVERIFY(hasCall(sp, "docker rm -f"));
    }

    // A running container booted WITHOUT the c2 boot arg has no working HEVC/opus encoder — the
    // mirror session dies NAME_NOT_FOUND every time. When the profile wants c2, don't reconnect to
    // it; redeploy.
    void smartConnectRedeploysWhenC2Missing() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerHasC2 = false;
        RemoraConfig cfg;
        cfg.mirror.videoCodec = QStringLiteral("h265");  // -> useCodec2 resolves true
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));  // took the deploy chain (recreates with c2)
    }

    // GPU mode is baked at docker run: a profile switched from guest to host (or back) must
    // recreate, because the running container keeps the mode it was created with — ANGLE and no
    // DRI device — and a warm reconnect would report success against it. Observed live: an A16
    // profile switched to gpu_mode=host kept running ANGLE, so h265 stayed impossible (the only
    // HEVC encoder on the -hwc2 image is the VAAPI one, which needs the VF).
    void smartConnectRedeploysWhenGpuModeChanged() {
        FakeSpawner sp;                                  // canned container reports gpu_mode=host
        RemoraConfig cfg;
        cfg.gpu.mode = GpuMode::Guest;                   // profile now wants guest -> mismatch
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));        // took the deploy chain (recreates)
    }

    // ...and an UNCHANGED gpu mode must NOT recreate, or every reconnect would rebuild the
    // container. vm resolves to host, which is what the canned container advertises.
    void smartConnectKeepsContainerWhenGpuModeMatches() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(!hasCall(sp, "docker run"));       // warm reconnect, no recreate
    }

    // Shared folders are baked at docker run: a share added to the profile must recreate the
    // container (the running one doesn't mount it)…
    void smartConnectRedeploysWhenShareAdded() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RemoraConfig cfg;
        cfg.integration.sharedFolders = {QStringLiteral("/srv/media")};
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));  // deploy chain (recreates with the bind)
    }

    // …a share the container already mounts reconnects fast…
    void smartConnectReconnectsWhenSharesMatch() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerBinds = QStringLiteral("/srv/media:/remora/share/media");
        RemoraConfig cfg;
        cfg.integration.sharedFolders = {QStringLiteral("/srv/media")};
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(!hasCall(sp, "docker run"));
    }

    // …and a share REMOVED from the profile recreates too — an un-shared folder staying mounted
    // would be a silent privacy leak.
    void smartConnectRedeploysWhenShareRemoved() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerBinds = QStringLiteral("/srv/media:/remora/share/media");
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));
    }

    // Every connect re-asserts the share dirs' media perms (setgid 1023) — without them the
    // MediaProvider FUSE creates device-side files as 600/<app-uid>, unreadable from the host.
    void connectFixesSharedFolderPerms() {
        FakeSpawner sp;
        RemoraConfig cfg;
        cfg.integration.sharedFolders = {QStringLiteral("/srv/media")};
        sp.containerBinds = QStringLiteral("/srv/media:/data/media/0/media");  // match → reconnect
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "chgrp 1023 /data/media/0/media"));
    }

    // Companion apps are docker-run binds too (bd remora-fgj.3), and both directions matter more
    // quietly than shares do: the app is a SYSTEM app, so one removed from the profile keeps its
    // Settings tile AND its WRITE_SECURE_SETTINGS reach until a recreate, while one added never
    // appears at all — and the deploy reports success either way.
    void smartConnectReconnectsWhenCompanionAppsMatch() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RemoraConfig cfg;
        cfg.integration.companionApps = {QStringLiteral("/apps/RemoraTweaks")};
        sp.containerBinds = companionAppBind(QStringLiteral("/apps/RemoraTweaks"));
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(!hasCall(sp, "docker run"));  // fast path kept
    }

    void smartConnectRedeploysWhenCompanionAppAdded() {
        FakeSpawner sp;  // container has no app binds at all
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RemoraConfig cfg;
        cfg.integration.companionApps = {QStringLiteral("/apps/RemoraTweaks")};
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));  // recreate, or the app silently never appears
    }

    void smartConnectRedeploysWhenCompanionAppRemoved() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerBinds = companionAppBind(QStringLiteral("/apps/RemoraTweaks"));
        RecordingSink sink;  // profile no longer lists it
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));
    }

    // A DIFFERENT app mounted than the one configured is still a mismatch — equal counts must not
    // be mistaken for equal contents.
    void smartConnectRedeploysWhenCompanionAppSwapped() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RemoraConfig cfg;
        cfg.integration.companionApps = {QStringLiteral("/apps/RemoraTweaks")};
        sp.containerBinds = companionAppBind(QStringLiteral("/apps/SomethingElse"));
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));
    }

    // androidboot.remora_fps is baked at docker run (first-write-wins ro. prop): a changed
    // fps must redeploy, and a matching one must keep the fast path (bd remora-bqt).
    void smartConnectRedeploysWhenFpsChanged() {
        FakeSpawner sp;  // canned probe reports a container booted with remora_fps=60
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        RemoraConfig cfg;
        cfg.display.fps = 120;
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));  // took the deploy chain (recreates at 120)
    }

    void smartConnectReconnectsWhenFpsMatches() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerFps = 60;  // the canned container was booted at the same rate
        RemoraConfig cfg;
        cfg.display.fps = 60;
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(cfg), sink, sp));
        QVERIFY(!hasCall(sp, "docker run"));  // fast path kept
    }

    // bd remora-wlun — the drift the per-arg gate missed, in BOTH directions. Reverse: the
    // container carries a boot arg the profile no longer resolves (gralloc un-set; androidboot.*
    // lands in first-write-wins ro. props, so the old HAL stays until a recreate) — the gate
    // used to reconnect right onto it. Forward: the profile gained an arg the container lacks.
    void smartConnectRedeploysOnBootArgDrift() {
        {
            FakeSpawner sp;
            sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
            sp.containerExtraBootArgs = QStringLiteral("androidboot.remora_gralloc=gbm");
            RecordingSink sink;
            // caps present (an `up` always probes) => the full set applies, gralloc included
            QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp, HostCapabilities{}));
            QVERIFY(hasCall(sp, "docker run"));  // recreated, not reconnected
        }
        {
            FakeSpawner sp;
            sp.probeGpuMode = QStringLiteral("guest");
            RemoraConfig c;
            c.display.fps = 90;  // profile now wants a rate the container was not booted with
            RecordingSink sink;
            QVERIFY(smartRun(Backend::Remote, withGuest(c), sink, sp));
            QVERIFY(hasCall(sp, "docker run"));
        }
        // The capsless tolerance: an app/desktop window opens WITHOUT a capability probe, so the
        // node-derived keys (gpu_node/gralloc/va_driver) cannot resolve — they leave both sides
        // rather than recreate a healthy container out from under every window on it. The same
        // gralloc leftover that recreates above is tolerated here; anything else still drifts.
        {
            FakeSpawner sp;
            sp.probeGpuMode = QStringLiteral("guest");
            sp.containerExtraBootArgs = QStringLiteral("androidboot.remora_gralloc=gbm");
            RecordingSink sink;
            // no caps passed — the popup shape
            QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp, std::nullopt));
            QVERIFY(!hasCall(sp, "docker run"));
        }
    }

    // …and the retired-key tolerance: androidboot.hardware= was retired with the identity
    // rename, so a pre-retirement container carrying it (the daily driver, live-diffed) must
    // still reconnect — recreating would reboot a session to change nothing the builder controls.
    void smartConnectToleratesRetiredBootArgs() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");
        sp.containerExtraBootArgs = QStringLiteral("androidboot.hardware=remora");
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp, HostCapabilities{}));
        QVERIFY(!hasCall(sp, "docker run"));  // current, despite the fossil arg
    }

    // bd remora-xqt1 — the NEW-key tolerance, and it must be ONE-WAY. remora_vulkan started being
    // emitted while it is still inert (native_vulkan pins ro.hardware.vulkan in build.prop, which
    // init loads first and ro.* is write-once), so a container that predates the key must NOT be
    // recreated to gain an arg that changes nothing. A container carrying a DIFFERENT value is a
    // real disagreement and must still redeploy — otherwise the tolerance would outlive the pin
    // and silently keep a wrong ICD once the value starts mattering.
    void smartConnectToleratesAbsentVulkanButNotAWrongOne() {
        {  // predates the key entirely: current, no recreate
            FakeSpawner sp;
            sp.probeGpuMode = QStringLiteral("guest");
            RecordingSink sink;
            QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp, HostCapabilities{}));
            QVERIFY(!hasCall(sp, "docker run"));
        }
        {  // carries a different ICD than this config resolves (guest resolves pastel): drift
            FakeSpawner sp;
            sp.probeGpuMode = QStringLiteral("guest");
            sp.containerExtraBootArgs = QStringLiteral("androidboot.remora_vulkan=intel");
            RecordingSink sink;
            QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp, HostCapabilities{}));
            QVERIFY(hasCall(sp, "docker run"));
        }
    }

    void smartConnectDeploysWhenNotRunning() {
        FakeSpawner sp;
        sp.probeGpuMode = QStringLiteral("guest");  // remote resolves guest
        sp.containerRunning = false;
        RecordingSink sink;
        QVERIFY(smartRun(Backend::Remote, withGuest(), sink, sp));
        QVERIFY(hasCall(sp, "docker run"));  // full deploy chain ran first
        QVERIFY(hasCall(sp, "mirror -s"));
    }

    // Connect against a stopped/removed container must fail fast with "use Start", not burn the
    // adb retries on "No route to host".
    void connectFailsFastWhenContainerNotRunning() {
        FakeSpawner sp;
        sp.containerRunning = false;
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1);
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("not running")));
        QVERIFY(!hasCall(sp, "adb connect"));  // never touched adb
    }

    // adb answering ≠ Android booted: connect must gate on sys.boot_completed and never hand a
    // half-booted system to the mirror (the device half needs services that are not up yet).
    void connectWaitsForBootBeforeMirror() {
        FakeSpawner sp;
        sp.bootStates = QStringList{"", "1"};  // first probe: still booting; second: done
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1, /*bootWaitMs=*/10000);
        QVERIFY(r.ok);
        QCOMPARE(countCalls(sp, "sys.boot_completed"), 2);
        QVERIFY(hasCall(sp, "mirror -s"));
    }

    void connectFailsWhenBootNeverCompletes() {
        FakeSpawner sp;
        sp.bootStates = QStringList{"0"};  // stuck mid-boot forever
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        const StepResult r = doConnect(sp, ctx, sink, 5, 0, 0, 1, /*bootWaitMs=*/0);
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QStringLiteral("did not finish booting")));
        QVERIFY(!hasCall(sp, "mirror -s"));  // mirror never launched into a half-booted system
    }

    void reconnectReappliesNetfixOnAdbFailure() {
        FakeSpawner sp;
        // Empty, not "offline": a dead fwd shim means adb cannot reach the port at all, so
        // get-state fails with nothing on stdout. "offline" is a DIFFERENT fault — adb holding a
        // stale transport — and it now has its own, cheaper recovery that would resolve this
        // attempt before netfix was ever reached (bd remora-400).
        sp.adbStates = QStringList{"", "device"};  // first try fails, second succeeds
        RecordingSink sink;
        QVERIFY(reconnectRun(Backend::Remote, withGuest(), sink, sp));
        // The dead-fwd heal: netfix re-ran inside the connect step, over ssh for the remote
        // backend. /remora, not /data/local/tmp — remote now binds the staged pair at /remora
        // like bare rather than docker-cp'ing it post-boot (bd remora-4ei.88).
        QVERIFY(hasCall(sp, "/remora/netfix.sh"));
        QVERIFY2(!hasCall(sp, "/data/local/tmp/netfix.sh"),
                 "remote must not fall back to the post-boot docker-cp path");
        QVERIFY(hasCall(sp, "mirror -s"));
    }

    void reconnectSkipsNetfixWhenAdbHealthy() {
        FakeSpawner sp;  // adb reports "device" immediately
        RecordingSink sink;
        QVERIFY(reconnectRun(Backend::Bare, RemoraConfig{}, sink, sp));
        QVERIFY(!hasCall(sp, "netfix.sh"));
    }

    // Remote wires adb the SAME way bare does — /remora bind mounts of the staged ~/.remora pair —
    // not the post-boot `docker cp` into /data/local/tmp that init's read-only remount defeats
    // (bd remora-4ei.59, bd remora-4ei.88). The run argv must name ~/.remora sources (the remote
    // shell expands the ~), the netfix step must exec /remora/netfix.sh, and no docker cp of the
    // pair may survive anywhere in the deploy.
    void remoteDeployWiresNetfixByBindNotDockerCp() {
        FakeSpawner sp;
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        QVERIFY(runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("netfix"), StepState::Ok);
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        // the run argv binds the staged pair at /remora, sourced from the remote's ~/.remora
        QVERIFY(calls.contains(QStringLiteral("~/.remora/fwd:/remora/fwd:ro")));
        QVERIFY(calls.contains(QStringLiteral("~/.remora/remora-netfix.sh:/remora/netfix.sh:ro")));
        // The forwarder's init service is staged and bound the same way (bd remora-yn4). Its
        // DESTINATION differs and must: init reads .rc files only from the partitions' own init
        // dirs, so /remora would never be parsed. It is also pushed unconditionally — it is a
        // committed source file, not a built artefact that might be absent.
        QVERIFY2(calls.contains(QStringLiteral("remora-fwd.rc")),
                 "the init service must be staged to the remote");
        QVERIFY(calls.contains(
            QStringLiteral("~/.remora/remora-fwd.rc:/vendor/etc/init/remora-fwd.rc:ro")));
        // netfix execs the mounted script, never a copied-in one
        QVERIFY(calls.contains(QStringLiteral("/remora/netfix.sh")));
        // and the old docker-cp mechanism is gone entirely — for the pair AND for uinput-kbd.
        // Pinned as "no cp INTO the deploy container": copying INTO a booted container is what
        // init's read-only remount defeats. The guest-overlay prereq legitimately cps OUT of a
        // created (never started) throwaway image container — always as `docker cp $id:` — and
        // this must not misfire on it (bd remora-e5x.33).
        for (const QString &c : sp.calls)
            if (c.contains(QLatin1String("docker cp")))
                QVERIFY2(c.contains(QLatin1String("docker cp $id:/vendor/build.prop")),
                         "no post-boot docker cp may survive on the remote path (bd remora-4ei.88)");
        QVERIFY(!calls.contains(QStringLiteral(":/data/local/tmp")));
        QVERIFY(!calls.contains(QStringLiteral("/data/local/tmp/netfix.sh")));
    }

    // host-prereqs stages the pair as the BIND SOURCES for the run argv, so a failed scp must stop
    // the deploy there — otherwise docker creates an empty directory at the missing source and the
    // container comes up with an empty /remora (bd remora-4ei.88).
    void remoteHostPrereqsFailsLoudlyWhenAScpFails() {
        FakeSpawner sp;
        sp.scpOk = false;
        auto ctx = makeContext(withGuest(), Backend::Remote);
        RecordingSink sink;
        QVERIFY(!runChain(makeDeployer(Backend::Remote, sp, 24, 0)->steps(ctx), sink));
        QCOMPARE(sink.of("host-prereqs"), StepState::Failed);
        QCOMPARE(sink.of("provision"), StepState::Skipped);  // no container off an empty /remora
    }

    void sourceRebuildPlanInRecipe() {
        // hw_video_decode on A13 has no base tag and is not overlay-able -> source rebuild
        const ImageRecipe r = buildRecipe(QSet<QString>{"hw_video_decode"}, 13);
        QVERIFY(r.needsSourceBuild);
        QVERIFY(r.sourceBuildHint.contains("do-build.sh"));
        QVERIFY(r.sourceBuildHint.contains("hw_video_decode"));
        // a plain overlay recipe carries no source-build plan
        const ImageRecipe o = buildRecipe(QSet<QString>{"widevine_l3"}, 13);
        QVERIFY(!o.needsSourceBuild);
        QVERIFY(o.sourceBuildHint.isEmpty());
    }

    void imageBuildLocalCommands() {
        SKIP_ON_WINDOWS("builds with this machine's docker, which Windows does not have");
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        RemoraConfig cfg;
        // A13 upstream bases don't bake widevine → the recipe needs an overlay build
        cfg.image.features = QStringList{"widevine_l3"};
        cfg.image.buildDir = tmp.path();
        cfg.image.outputDir = tmp.path() + "/out";
        const ResolvedConfig rc = resolve(cfg, Backend::Bare);
        const QSet<QString> feats(cfg.image.features.begin(), cfg.image.features.end());
        const ImageRecipe recipe = buildRecipe(feats, 13);
        QVERIFY(recipe.needsBuild);
        // stage the widevine payload the recipe requires
        QVERIFY(QDir().mkpath(tmp.path() + "/wv/vendor"));

        FakeSpawner sp;
        RecordingSink sink;
        const bool ok = runChain(imageBuildSteps(sp, rc, recipe), sink);
        if (!ok) qWarning() << sink.details;
        QVERIFY(ok);
        QVERIFY(hasCall(sp, "docker build -t " + recipe.tag + " " + tmp.path()));
        QVERIFY(hasCall(sp, "docker save -o"));
        // the Dockerfile was staged into the build context
        QVERIFY(QFileInfo::exists(tmp.path() + "/Dockerfile"));
    }

    void imageBuildRemoteWrapsSsh() {
        RemoraConfig cfg;
        cfg.image.features = QStringList{"widevine_l3"};
        cfg.image.buildHost = QStringLiteral("user@buildbox");
        const ResolvedConfig rc = resolve(cfg, Backend::Bare);
        const ImageRecipe recipe =
            buildRecipe(QSet<QString>{"widevine_l3"}, 13);

        FakeSpawner sp;
        RecordingSink sink;
        QVERIFY(runChain(imageBuildSteps(sp, rc, recipe), sink));
        QVERIFY(hasCall(sp, "user@buildbox"));
        QVERIFY(hasCall(sp, "docker build"));
        // no local staging happened — everything rode over ssh
        QVERIFY(!QFileInfo::exists(rc.buildDir + "/Dockerfile") || true);
    }

    // Desktop app-menu integration: auto-refresh defaults on (an XDG menu on Linux, the Start menu
    // on Windows), checkbox opt-out resolves through, and the profile→slug mapping matches
    // slugify() in remora-app-menu.sh.
    void desktopMenuIntegration() {
        RemoraConfig cfg;
        QCOMPARE(resolve(cfg, Backend::Remote).desktopMenu, kDesktopMenuDefault);
        cfg.integration.desktopMenu = false;
        QVERIFY(!resolve(cfg, Backend::Remote).desktopMenu);
        QCOMPARE(appMenuSlug(QStringLiteral("Android 16")), QStringLiteral("android-16"));
        QCOMPARE(appMenuSlug(QStringLiteral("--Daily Driver (vm)--")),
                 QStringLiteral("daily-driver-vm"));
    }

    // bd remora-7oih — on Windows the menu folder is a Start-menu folder of shortcuts, written
    // through Spawner::writeShortcut, with the agent's icon renders converted to .ico. It is the
    // Linux folder entry for entry, and it touches only the files its manifest says it wrote.
    void windowsStartMenuIsTheAppMenuFolder() {
        const QString menuRoot = startMenuRoot();
        const QString state = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                              QStringLiteral("/remora/menu");
        // Test mode only: the paths this deletes must never be a real Start menu.
        QVERIFY2(menuRoot.contains(QStringLiteral("qttest")) &&
                     state.contains(QStringLiteral("qttest")),
                 qPrintable(menuRoot + QLatin1Char(' ') + state));
        const QString folder = menuRoot + QStringLiteral("/My Phone");
        QDir(folder).removeRecursively();
        QDir(state).removeRecursively();
        // Somebody else's shortcut, in a folder that happens to have the profile's name.
        QVERIFY(QDir().mkpath(folder));
        QFile other(folder + QStringLiteral("/Other.lnk"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();

        StartMenuFake sp;
        RecordingSink sink;
        QVERIFY(appMenuSync(sp, withGuest(), Backend::Remote, QStringLiteral("My Phone"), sink));
        QCOMPARE(sink.of(QStringLiteral("app-menu")), StepState::Ok);
        QCOMPARE(sp.links.size(), 5);
        // The device's three entries, as the Linux folder has them…
        QCOMPARE(QFileInfo(sp.links[0].path).fileName(), QStringLiteral("Android (mirror).lnk"));
        QCOMPARE(sp.links[0].arguments, QStringLiteral("up \"My Phone\""));
        QCOMPARE(sp.links[1].arguments, QStringLiteral("desktop \"My Phone\""));
        QCOMPARE(sp.links[2].arguments, QStringLiteral("pip \"My Phone\""));
        QVERIFY(sp.links[0].appId.isEmpty());
        // …and one per app, paired with the app's window by its AppUserModelID.
        const Shortcut &a = sp.links[3];
        QCOMPARE(QDir::cleanPath(a.path), QDir::cleanPath(folder + QStringLiteral("/App A.lnk")));
        QCOMPARE(a.arguments, QStringLiteral("app \"My Phone\" com.a --title \"App A\""));
        QCOMPARE(a.appId, QStringLiteral("remora-my-phone-com.a"));
        QCOMPARE(a.description, QStringLiteral("App A — My Phone (com.a)"));
        QCOMPARE(a.iconPath, state + QStringLiteral("/my-phone/com.a.ico"));
        {
            // Closed again before the removal below: Windows does not delete an open file.
            QFile ico(a.iconPath);
            QVERIFY(ico.open(QIODevice::ReadOnly));
            QVERIFY(ico.readAll().startsWith(QByteArray("\0\0\1\0\1\0", 6)));
        }
        QCOMPARE(appMenuIconForAppId(a.appId), a.iconPath);  // what the app's window shows
        // The render failed for com.b (an empty file): no icon, so the entry has Remora's.
        QVERIFY(sp.links[4].iconPath.isEmpty());
        QVERIFY(appMenuIconForAppId(QStringLiteral("remora-my-phone-com.b")).isEmpty());
        // The Apps page lists what the menu holds.
        const QVector<AppMenuEntry> listed = appMenuEntries(QStringLiteral("my-phone"));
        QCOMPARE(listed.size(), 2);
        QCOMPARE(listed[0].pkg, QStringLiteral("com.a"));
        QCOMPARE(listed[1].name, QStringLiteral("App B"));

        // com.b uninstalled: its entry and icon go, everything else stays.
        sp.listApps = QStringLiteral(" * App A  com.a\n");
        QVERIFY(appMenuSync(sp, withGuest(), Backend::Remote, QStringLiteral("My Phone"), sink));
        QVERIFY(!holds(folder, QStringLiteral("App B.lnk")));
        QVERIFY(holds(folder, QStringLiteral("App A.lnk")));
        QVERIFY(holds(folder, QStringLiteral("Android (PIP).lnk")));
        QVERIFY(holds(folder, QStringLiteral("Other.lnk")));

        // Removal takes Remora's shortcuts and icons and leaves the folder to its other owner.
        RecordingSink rm;
        QVERIFY(appMenuRemove(sp, QStringLiteral("My Phone"), rm));
        QCOMPARE(rm.of(QStringLiteral("app-menu")), StepState::Ok);
        QVERIFY(!holds(folder, QStringLiteral("App A.lnk")));
        QVERIFY(!holds(folder, QStringLiteral("Android (mirror).lnk")));
        QVERIFY(holds(folder, QStringLiteral("Other.lnk")));
        QVERIFY(!QDir(state + QStringLiteral("/my-phone")).exists());
        QVERIFY(appMenuEntries(QStringLiteral("my-phone")).isEmpty());
        QDir(folder).removeRecursively();

        // Linux still runs the script.
        FakeSpawner lin;
        RecordingSink lsink;
        appMenuRemove(lin, QStringLiteral("p"), lsink);
        QVERIFY(spawnsProgram(lin, "bash"));
    }

    // The real writer, through the shell's own IShellLink: what it wrote is read back the same
    // way, the AppUserModelID included, since that is what pairs a window with its entry.
    void windowsShortcutsAreWhatTheShellReadsBack() {
#ifndef Q_OS_WIN
        RealSpawner sp;
        QVERIFY(!sp.writeShortcut(Shortcut{}).isEmpty());  // nowhere else to write one
        QSKIP("writes a .lnk through the Windows shell");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Shortcut s;
        s.path = dir.filePath(QStringLiteral("App A.lnk"));
        s.target = QCoreApplication::applicationFilePath();
        s.arguments = windowsArgs({QStringLiteral("app"), QStringLiteral("My Phone"),
                                   QStringLiteral("com.a"), QStringLiteral("--title"),
                                   QStringLiteral("App \"A\"")});
        s.workingDir = QCoreApplication::applicationDirPath();
        s.description = QStringLiteral("App A — My Phone (com.a)");
        s.appId = QStringLiteral("remora-my-phone-com.a");
        RealSpawner sp;
        QCOMPARE(sp.writeShortcut(s), QString());
        QVERIFY(QFileInfo(s.path).size() > 0);

        QVERIFY(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
        IShellLinkW *link = nullptr;
        QVERIFY(SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_IShellLinkW, reinterpret_cast<void **>(&link))));
        IPersistFile *file = nullptr;
        QVERIFY(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file))));
        QVERIFY(SUCCEEDED(file->Load(reinterpret_cast<LPCOLESTR>(
                                         QDir::toNativeSeparators(s.path).utf16()),
                                     STGM_READ)));
        wchar_t buf[1024] = {};
        QVERIFY(SUCCEEDED(link->GetArguments(buf, 1024)));
        QCOMPARE(QString::fromWCharArray(buf), s.arguments);
        QVERIFY(SUCCEEDED(link->GetPath(buf, 1024, nullptr, 0)));
        QCOMPARE(QFileInfo(QString::fromWCharArray(buf)), QFileInfo(s.target));
        IPropertyStore *store = nullptr;
        QVERIFY(SUCCEEDED(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void **>(&store))));
        static const PROPERTYKEY kAppUserModelId = {
            {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
        PROPVARIANT v;
        PropVariantInit(&v);
        QVERIFY(SUCCEEDED(store->GetValue(kAppUserModelId, &v)));
        QCOMPARE(v.vt, VARTYPE(VT_LPWSTR));
        QCOMPARE(QString::fromWCharArray(v.pwszVal), s.appId);
        ::PropVariantClear(&v);
        store->Release();
        file->Release();
        link->Release();
        ::CoUninitialize();
#endif
    }

    // remora-app-menu.sh's guards hold on Windows too: a device that reports a fraction of the
    // menu is still booting, and the menu is kept rather than emptied (bd remora-5sd); without a
    // device address the app entries are left alone and only the device's own are rewritten.
    void windowsStartMenuKeepsWhatItCannotCheck() {
        const QString menuRoot = startMenuRoot();
        QVERIFY(menuRoot.contains(QStringLiteral("qttest")));
        const QString folder = menuRoot + QStringLiteral("/Settle");
        StartMenuFake sp;
        RecordingSink sink;
        QString six;
        for (int i = 0; i < 6; ++i) six += QStringLiteral(" - App %1  com.app%1\n").arg(i);
        sp.listApps = six;
        QVERIFY(appMenuSync(sp, withGuest(), Backend::Remote, QStringLiteral("Settle"), sink));
        QCOMPARE(appMenuEntries(QStringLiteral("settle")).size(), 6);

        sp.listApps = QStringLiteral(" - App 0  com.app0\n");
        RecordingSink settling;
        QVERIFY(appMenuSync(sp, withGuest(), Backend::Remote, QStringLiteral("Settle"), settling));
        QCOMPARE(settling.of(QStringLiteral("app-menu")), StepState::Skipped);
        QCOMPARE(sp.waits, 8);
        QVERIFY(holds(folder, QStringLiteral("App 5.lnk")));
        QCOMPARE(appMenuEntries(QStringLiteral("settle")).size(), 6);

        // macvlan with no address read: no target, so no app list to judge by.
        sp.links.clear();
        RecordingSink plain;
        QVERIFY(appMenuSync(sp, withMacvlan(), Backend::Remote, QStringLiteral("Settle"), plain));
        QCOMPARE(plain.of(QStringLiteral("app-menu")), StepState::Ok);
        QCOMPARE(sp.links.size(), 3);  // the device's entries only
        QCOMPARE(appMenuEntries(QStringLiteral("settle")).size(), 6);
        QVERIFY(holds(folder, QStringLiteral("App 5.lnk")));

        // What the uninstaller runs: every folder, whichever profile it was for.
        RecordingSink all;
        QVERIFY(appMenuRemoveAll(sp, all));
        QVERIFY(!holds(folder, QStringLiteral("App 5.lnk")));
        QVERIFY(!QDir(folder).exists());
        QVERIFY(appMenuEntries(QStringLiteral("settle")).isEmpty());
    }

    void whpxBootWaitUsesTimedProbesAndStopsOnContainerExit() {
        class TimedSpawner : public FakeSpawner {
        public:
            QVector<int> deadlines;
            ProcResult runTimed(const QStringList &argv, int timeoutMs,
                                const LineSink &onLine) override {
                deadlines.append(timeoutMs);
                return run(argv, {}, onLine);
            }
        } sp;
        auto deployer = makeDeployer(Backend::Remote, sp, 1, 0, 60 * 1000);
        const auto steps = deployer->steps(makeContext(withGuest(), Backend::Remote));
        const auto boot = std::find_if(steps.begin(), steps.end(), [](const Step &step) {
            return step.name == QLatin1String("boot-wait");
        });
        QVERIFY(boot != steps.end());
        RecordingSink sink;
        sp.bootStates = QStringList{"", "0", "1"};
        const auto ready = boot->run(sink);
        QVERIFY2(ready.ok, qPrintable(ready.detail));
        QCOMPARE(sp.deadlines.size(), 3);
        QVERIFY(ready.detail.contains(QStringLiteral("60 s WHPX budget")));
        for (const int deadline : sp.deadlines) QVERIFY(deadline > 0 && deadline <= 30000);

        sp.deadlines.clear();
        sp.containerExit = QStringLiteral("exited 129");
        const auto stopped = boot->run(sink);
        QVERIFY(!stopped.ok);
        QVERIFY(stopped.detail.contains(QStringLiteral("Android asked to reboot")));
        QCOMPARE(sp.deadlines.size(), 2);
    }

    // A container that stops while Android boots ends the boot wait at once, saying why. It was
    // polled for the whole budget: half an hour under an emulated guest on the Windows VM, when
    // Android asked to reboot nine minutes in (bd remora-7oih).
    void bootWaitStopsWhenTheContainerStops() {
        for (Backend b : deployTargets()) {
            FakeSpawner sp;
            sp.containerExit = QStringLiteral("exited 129");
            const RunContext ctx =
                makeContext(b == Backend::Remote ? withGuest() : RemoraConfig{}, b);
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 1800, 0)->steps(ctx), sink));
            QCOMPARE(sink.of(QStringLiteral("boot-wait")), StepState::Failed);
            QCOMPARE(countCalls(sp, QStringLiteral("sys.boot_completed")), 1);
            const QString d = sink.details.join(QLatin1Char('\n'));
            QVERIFY2(d.contains(QStringLiteral("Android asked to reboot")), qPrintable(d));
        }
        // A boot that is merely slow waits out its budget, and never asks docker anything more.
        for (Backend b : deployTargets()) {
            FakeSpawner sp;
            sp.bootStates = {};  // getprop answers "0" (not booted) every time
            const RunContext ctx =
                makeContext(b == Backend::Remote ? withGuest() : RemoraConfig{}, b);
            RecordingSink sink;
            QVERIFY(!runChain(makeDeployer(b, sp, 5, 0)->steps(ctx), sink));
            QCOMPARE(countCalls(sp, QStringLiteral("sys.boot_completed")), 5);
            QCOMPARE(countCalls(sp, QStringLiteral("State.Status")), 0);
            QVERIFY(sink.details.join(QLatin1Char('\n')).contains(QStringLiteral("after 5 polls")));
        }
    }

    // A device that lists no apps right after its boot is waited for before a first menu fails
    // (an emulated guest on the Windows VM did that for a few seconds after boot_completed), and
    // one that never lists any still fails, after the same wait a refresh gives a settling device.
    void windowsStartMenuWaitsForAFirstAppList() {
        const QString menuRoot = startMenuRoot();
        QVERIFY(menuRoot.contains(QStringLiteral("qttest")));
        StartMenuFake sp;
        sp.emptyListsFirst = 2;
        RecordingSink sink;
        QVERIFY(appMenuSync(sp, withGuest(), Backend::Remote, QStringLiteral("Fresh"), sink));
        QCOMPARE(sink.of(QStringLiteral("app-menu")), StepState::Ok);
        QCOMPARE(sp.waits, 2);
        QCOMPARE(appMenuEntries(QStringLiteral("fresh")).size(), 2);
        RecordingSink rm;
        QVERIFY(appMenuRemove(sp, QStringLiteral("Fresh"), rm));

        StartMenuFake none;
        none.listApps.clear();
        RecordingSink failed;
        QVERIFY(!appMenuSync(none, withGuest(), Backend::Remote, QStringLiteral("Empty"), failed));
        QCOMPARE(failed.of(QStringLiteral("app-menu")), StepState::Failed);
        QCOMPARE(none.waits, 8);
        QVERIFY(none.links.isEmpty());
    }

    // bd remora-a766 — the props rc carries the container's sysrq mask, so every deploy has to
    // mount it, on either backend: no gate, unlike the agent's files, which wait for a built dex.
    void everyDeployMountsTheSysrqGuard() {
        for (Backend b : {Backend::Bare, Backend::Remote}) {
            const RunContext ctx =
                makeContext(b == Backend::Remote ? withGuest() : RemoraConfig{}, b);
            QVERIFY(ctx.rc.propsInitRcPath.has_value());
            QVERIFY(buildDockerRunArgv(ctx.rc).contains(
                *ctx.rc.propsInitRcPath + QStringLiteral(":/vendor/etc/init/remora.props.rc:ro")));
        }
    }

    // Per-app window modes ("<pkg>=desktop|freeform"): unset asks, set round-trips, re-set
    // replaces; the freeform popup geometry has a phone-shaped resolver default.
    void appWindowModes() {
        RemoraConfig cfg;
        QCOMPARE(appModeFor(cfg, "com.a"), QString());  // unset → first launch prompts
        setAppMode(cfg, "com.a", "freeform");
        setAppMode(cfg, "com.b", "desktop");
        QCOMPARE(appModeFor(cfg, "com.a"), QStringLiteral("freeform"));
        QCOMPARE(appModeFor(cfg, "com.b"), QStringLiteral("desktop"));
        setAppMode(cfg, "com.a", "desktop");  // replace, not append
        QCOMPARE(appModeFor(cfg, "com.a"), QStringLiteral("desktop"));
        QCOMPARE(cfg.integration.appModes.size(), 2);
        setAppMode(cfg, "com.b", "freeform-stretch");  // freeform + stretch-on-resize variant
        QCOMPARE(appModeFor(cfg, "com.b"), QStringLiteral("freeform-stretch"));
        setAppMode(cfg, "com.a", QString());  // clear → back to "ask"
        QCOMPARE(appModeFor(cfg, "com.a"), QString());
        QCOMPARE(resolve(cfg, Backend::Remote).freeformDisplay, QStringLiteral("1080x2340/420"));
    }

    // The Alt+D mirror↔desktop toggle belongs to the MIRROR. Handing it to an additional window
    // wired that window to itself — REMORA_MIRROR_CMD relaunched the popup, REMORA_IS_DESKTOP was
    // unset so the popup believed it WAS the mirror, and Alt+D spawned a duplicate then closed the
    // original. An app popup and the mirror must launch independently of each other.
    void appWindowCarriesNoMirrorToggle() {
        RemoraConfig cfg;
        cfg.mirror.fullscreenSeparateDisplay = true;
        cfg.mirror.fullscreenApp = QStringLiteral("com.anthropic.claude");
        const QString target = QStringLiteral("localhost:5555");

        auto popup = makeContext(cfg, Backend::Bare, std::nullopt, QStringLiteral("Claude"),
                                 /*fullscreenDisplay=*/true);
        QVERIFY(popup.extraWindow());
        const QMap<QString, QString> pe = mirrorSpawnEnv(popup, target, popup.mirrorArgv);
        QVERIFY(!pe.contains(QStringLiteral("REMORA_MIRROR_CMD")));
        QVERIFY(!pe.contains(QStringLiteral("REMORA_DESKTOP_CMD")));

        // The mirror on the SAME profile still gets both sides of the toggle.
        auto mirror = makeContext(cfg, Backend::Bare, std::nullopt, QStringLiteral("Dev"));
        QVERIFY(!mirror.extraWindow());
        const QMap<QString, QString> me = mirrorSpawnEnv(mirror, target, mirror.mirrorArgv);
        QVERIFY(me.contains(QStringLiteral("REMORA_MIRROR_CMD")));
        QVERIFY(me.contains(QStringLiteral("REMORA_DESKTOP_CMD")));

        // A genuine mirrored fullscreen is the mirror, not an extra window — fullscreenDisplay
        // alone must not be read as "additional window" (the GUI's Fullscreen action sets it).
        RemoraConfig plain;
        auto fs = makeContext(plain, Backend::Bare, std::nullopt, QStringLiteral("Dev"),
                              /*fullscreenDisplay=*/true);
        QVERIFY(!fs.extraWindow());
        QVERIFY(mirrorSpawnEnv(fs, target, fs.mirrorArgv)
                    .contains(QStringLiteral("REMORA_MIRROR_CMD")));
    }

    // bd remora-nr9 — a desktop window IS one side of the pair, so it keeps the toggle: it is the
    // device on its own display, not one app. `remora desktop` starts on that side without ever
    // having been the mirror, so the caller's argv is the DESKTOP one and cannot be handed back as
    // "the mirror" — that is what made Alt+D there spawn another desktop and close the original.
    void desktopWindowTogglesBackToTheMirror() {
        RemoraConfig cfg;
        cfg.mirror.fullscreenSeparateDisplay = true;
        const QString target = QStringLiteral("localhost:5555");
        auto d = makeContext(cfg, Backend::Bare, std::nullopt,
                             kDesktopTitlePrefix + QStringLiteral("Dev"),
                             /*fullscreenDisplay=*/true);
        QVERIFY(d.extraWindow());
        QVERIFY(d.desktopWindow());
        const QMap<QString, QString> e = mirrorSpawnEnv(d, target, d.mirrorArgv);
        // it is the desktop side, so Alt+D reads MIRROR_CMD…
        QCOMPARE(e.value(QStringLiteral("REMORA_IS_DESKTOP")), QStringLiteral("1"));
        const std::optional<LaunchSpec> mirror =
            decodeLaunchSpec(e.value(QStringLiteral("REMORA_MIRROR_CMD")));
        QVERIFY(mirror.has_value());
        const QString mirrorCmd = mirror->argv.join(QLatin1Char(' '));
        // …and what it names is a MIRROR: no new display, and titled for the profile, not
        // "Remora-desktop:". Handing back this window's own argv is the bug being fixed.
        QVERIFY(!mirrorCmd.contains(QStringLiteral("--new-display")));
        QVERIFY(!mirrorCmd.contains(kDesktopTitlePrefix));
        QCOMPARE(mirror->argv.last(), QStringLiteral("Dev"));
        // the mirror side drops the desktop marker, so the window it opens knows it is the mirror
        QCOMPARE(mirror->unset, QStringList{QStringLiteral("REMORA_IS_DESKTOP")});
        QVERIFY(mirror->set.isEmpty());

        // An APP window on the same profile still gets nothing — one app has no "other side".
        cfg.mirror.fullscreenApp = QStringLiteral("com.anthropic.claude");
        auto p = makeContext(cfg, Backend::Bare, std::nullopt, QStringLiteral("Claude"),
                             /*fullscreenDisplay=*/true);
        QVERIFY(!p.desktopWindow());
        const QMap<QString, QString> pe = mirrorSpawnEnv(p, target, p.mirrorArgv);
        QVERIFY(!pe.contains(QStringLiteral("REMORA_MIRROR_CMD")));
        QVERIFY(!pe.contains(QStringLiteral("REMORA_IS_DESKTOP")));
    }

    // bd remora-7oih.2 — the two sides of Alt+D are data, not POSIX shell lines: a Windows mirror
    // has no `sh -c` to hand one to. Each names the running binary and an argv that survives
    // titles a shell would have split, and the env edit that says which side it is.
    void altDToggleIsAnArgvNotAShellLine() {
        RemoraConfig cfg;
        const QString target = QStringLiteral("localhost:5555");
        auto m = makeContext(cfg, Backend::Bare, std::nullopt, QStringLiteral("Pixel 'A' $HOME"));
        const QMap<QString, QString> e = mirrorSpawnEnv(m, target, m.mirrorArgv);
        const std::optional<LaunchSpec> desk =
            decodeLaunchSpec(e.value(QStringLiteral("REMORA_DESKTOP_CMD")));
        const std::optional<LaunchSpec> back =
            decodeLaunchSpec(e.value(QStringLiteral("REMORA_MIRROR_CMD")));
        QVERIFY(desk.has_value());
        QVERIFY(back.has_value());
        QCOMPARE(desk->argv.first(), QCoreApplication::applicationFilePath());
        QCOMPARE(desk->set.value(QStringLiteral("REMORA_IS_DESKTOP")), QStringLiteral("1"));
        QVERIFY(desk->unset.isEmpty());
        // the title is ONE argument, quote and dollar intact — nothing re-parses it any more
        QCOMPARE(desk->argv.last(), kDesktopTitlePrefix + QStringLiteral("Pixel 'A' $HOME"));
        QVERIFY(desk->argv.join(QLatin1Char(' ')).contains(QStringLiteral("--new-display")));
        // the mirror side is exactly what the caller spawned, with the desktop marker removed
        QCOMPARE(back->argv, m.mirrorArgv);
        QCOMPARE(back->unset, QStringList{QStringLiteral("REMORA_IS_DESKTOP")});
        for (const QString &k : {QStringLiteral("REMORA_DESKTOP_CMD"),
                                 QStringLiteral("REMORA_MIRROR_CMD")})
            QVERIFY2(!e.value(k).contains(QStringLiteral("env -u")), qPrintable(e.value(k)));
    }

    // arm64 pins (bd remora-e5x.23): an app with no working x86_64 build is installed as arm64
    // and must STAY arm64 — Play re-delivers the broken split at its next update. Pinning is
    // what lets the guard put it back; the pins round-trip through remorarc so a repair survives
    // a restart, and the guard defaults ON so an ARM-only app just works.
    void armPinsRoundTrip() {
        RemoraConfig cfg;
        QVERIFY(!isArmPinned(cfg, "com.zhiliaoapp.musically"));
        setArmPinned(cfg, "com.zhiliaoapp.musically", true);
        setArmPinned(cfg, "software.sonar.sonar_mobile", true);
        QVERIFY(isArmPinned(cfg, "com.zhiliaoapp.musically"));
        QCOMPARE(cfg.integration.armApps.size(), 2);
        setArmPinned(cfg, "com.zhiliaoapp.musically", true);  // idempotent, never duplicates
        QCOMPARE(cfg.integration.armApps.size(), 2);
        setArmPinned(cfg, "com.zhiliaoapp.musically", false);
        QVERIFY(!isArmPinned(cfg, "com.zhiliaoapp.musically"));
        QVERIFY(isArmPinned(cfg, "software.sonar.sonar_mobile"));

        // The guard is opt-OUT: an unset config repairs, and only the explicit false is stored.
        QVERIFY(cfg.integration.armGuard.value_or(true));
        cfg.integration.armGuard = false;
        QVERIFY(!cfg.integration.armGuard.value_or(true));  // pins kept as a record, repairs off
    }

    // bd remora-4ei.29: startDetached detaches the process but not the cgroup, so a mirror launched
    // from a desktop-launcher GUI stayed inside that app unit and died with it. The wrap is what
    // lifts it out.
    void scopedArgvWrapsInATransientScope() {
        const QStringList argv{"remora", "mirror", "-s", "192.0.2.77:5555", "--stay-awake"};
        const QStringList got = Spawner::scopedArgv(argv, true, QStringLiteral("remora-detached-7-0"));
        QCOMPARE(got.mid(0, 6),
                 (QStringList{"systemd-run", "--user", "--scope", "--quiet", "--collect",
                              "--unit=remora-detached-7-0"}));
        // the original command must survive intact and in order, right after the wrapper
        QCOMPARE(got.mid(6), argv);
        // --quiet is not cosmetic: Chain.cpp tails the mirror's own /tmp/remora-detached-*.log for
        // its dying words, and systemd-run's unit announcement would land in that very file.
        QVERIFY(got.contains(QStringLiteral("--quiet")));
    }

    // No systemd user manager (or no unit name) must leave the command completely untouched, so a
    // non-systemd host keeps the old behaviour rather than failing to launch at all.
    void scopedArgvIsIdentityWithoutSystemd() {
        const QStringList argv{"remora", "mirror", "-s", "dev"};
        QCOMPARE(Spawner::scopedArgv(argv, false, QStringLiteral("u")), argv);
        QCOMPARE(Spawner::scopedArgv(argv, true, QString()), argv);
        QCOMPARE(Spawner::scopedArgv({}, true, QStringLiteral("u")), QStringList());
    }

    // bd remora-4ei.50. A build toggle that reaches nothing is the defect bd remora-4ei.55 spent an
    // investigation on, so pin that the shamiko feature actually changes the assemble command.
    // sourceBuildSteps is otherwise untested; this covers the env-assembly line only.
    void shamikoFlagReachesTheAssembleStep() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto asmStep = [&](bool playSpoof, bool shamiko) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.withPlaySpoof = playSpoof;
            p.withShamiko = shamiko;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(QLatin1String("assemble-image.sh"))) return s;
            return QString();
        };
        // Both on: the two env vars ride together, since assemble stages Shamiko inside the
        // play_spoof block and the copy is a no-op without it.
        const QString both = asmStep(true, true);
        QVERIFY(!both.isEmpty());
        QVERIFY(both.contains(QLatin1String("WITH_PLAY_SPOOF=true")));
        QVERIFY(both.contains(QLatin1String("WITH_SHAMIKO=true")));
        // play_spoof alone must NOT drag Shamiko in. This is the case bd remora-4ei.16 needs to
        // stay expressible — it named Shamiko a suspect for breaking Play Store verification, so
        // "PIF but no Shamiko" is a configuration someone will want.
        const QString pifOnly = asmStep(true, false);
        QVERIFY(pifOnly.contains(QLatin1String("WITH_PLAY_SPOOF=true")));
        QVERIFY(!pifOnly.contains(QLatin1String("WITH_SHAMIKO")));
        // Neither: the default assemble command is untouched, per the project's empty-is-a-no-op rule.
        const QString neither = asmStep(false, false);
        QVERIFY(!neither.contains(QLatin1String("WITH_PLAY_SPOOF")));
        QVERIFY(!neither.contains(QLatin1String("WITH_SHAMIKO")));
    }

    // The build container nests under containers.slice and gets a fixed 4 GiB of swap above its
    // cap. A swap allowance of 2x the cap let a build thrash its host instead of failing. EXECUTED:
    // the real docker-run step runs under sh with `docker` stubbed to print its argv, so the
    // shell arithmetic that sizes --memory-swap is what is checked, not the template text. An
    // explicit cap makes every figure independent of the host's own memory.
    void sourceBuildContainerNestsUnderContainersSliceWithBoundedSwap() {
        SKIP_ON_WINDOWS(kRunsSh);
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto runArgv = [&](int memGiB) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.buildMemGiB = memGiB;
            QString step;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(QLatin1String("docker run --rm"))) step = s;
            if (step.isEmpty()) return QStringList{QStringLiteral("<no docker run step>")};
            QProcess sh;
            sh.start(QStringLiteral("sh"),
                     {QStringLiteral("-c"),
                      QStringLiteral("docker() { printf 'ARG:%s\\n' \"$@\"; }; ") + step});
            if (!sh.waitForFinished(15000) || sh.exitCode() != 0)
                return QStringList{QStringLiteral("<step failed>")};
            QStringList args;
            for (const QString &l : QString::fromUtf8(sh.readAllStandardOutput()).split('\n'))
                if (l.startsWith(QLatin1String("ARG:"))) args << l.mid(4);
            return args;
        };
        const QString nested = QStringLiteral("--cgroup-parent=containers-remorabuild.slice");
        for (int cap : {8, 24, 30}) {
            const QStringList a = runArgv(cap);
            QVERIFY2(a.size() > 2 && a.at(0) == QLatin1String("run"), qPrintable(a.join(' ')));
            QVERIFY2(a.contains(nested), qPrintable(a.join(' ')));
            QVERIFY(!a.contains(QStringLiteral("--cgroup-parent=remorabuild.slice")));
            QCOMPARE(a.filter(QStringLiteral("--cgroup-parent=")).size(), 1);
            // Exactly the cap plus 4 GiB: 8→12, 24→28, 30→34 — never 2x the cap.
            QVERIFY2(a.contains(QStringLiteral("--memory=%1g").arg(cap)), qPrintable(a.join(' ')));
            QVERIFY2(a.contains(QStringLiteral("--memory-swap=%1g").arg(cap + 4)),
                     qPrintable(a.join(' ')));
            QCOMPARE(a.filter(QStringLiteral("--memory-swap=")).size(), 1);
            // The Go heap bounds that keep soong_build and siso under the cap are untouched.
            const int heap = cap * 65 / 100;
            QVERIFY(a.contains(QStringLiteral("SOONG_GOMEMLIMIT=%1GiB").arg(heap)));
            QVERIFY(a.contains(QStringLiteral("GOMEMLIMIT=%1GiB").arg(heap)));
            QVERIFY(a.contains(QStringLiteral("--cpu-shares=128")));
        }
        // No cap: no memory flags at all, but still the nested slice.
        const QStringList uncapped = runArgv(0);
        QVERIFY2(uncapped.contains(nested), qPrintable(uncapped.join(' ')));
        QVERIFY(uncapped.filter(QStringLiteral("--memory")).isEmpty());
    }

    // assemble-image.sh overlaid the ARM translation payload whenever its guest directory was
    // vendored, so an image built with arm_translate OFF still carried the bridge. EXECUTED against
    // the gate assemble sources, with the WITH_NDK the real assemble step sets, on a fixture of
    // sentinel files: a contains() check cannot tell a gate that always copies from one that works.
    void armTranslationOverlayFollowsTheSelection() {
        SKIP_ON_WINDOWS(kRunsSh);
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        const QString sbuild = sourcePath(QStringLiteral("vendor/source-build"));
        QVERIFY(QFile::exists(sbuild + QStringLiteral("/ndk-overlay.sh")));
        const QString invoke = QStringLiteral("sh ") + sbuild + QStringLiteral("/assemble-image.sh");
        // The env prefix of the real assemble step, i.e. exactly what assemble is started with.
        auto asmEnvFor = [&](bool withNdk) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = sbuild;
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.withNdk = withNdk;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(invoke)) return s.left(s.indexOf(invoke));
            return QStringLiteral("<no assemble step>");
        };
        const QString on = asmEnvFor(true), off = asmEnvFor(false);
        QVERIFY2(on.contains(QLatin1String("WITH_NDK=true ")), qPrintable(on));
        // Stated, not omitted: an absent WITH_NDK is the unset case, which still copies.
        QVERIFY2(off.contains(QLatin1String("WITH_NDK=false ")), qPrintable(off));

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = tmp.path() + QStringLiteral("/ndk_translation");
        const auto put = [](const QString &path) {
            if (!QDir().mkpath(QFileInfo(path).path())) return false;
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write("sentinel\n") == 9;
        };
        QVERIFY(put(root + QStringLiteral("/guest/system/etc/guest-sentinel")));
        QVERIFY(put(root + QStringLiteral("/lib64/host-sentinel.so")));
        // Runs `<env>sh -c '<what assemble does with the gate>'` in a fresh context dir, with any
        // inherited WITH_NDK removed so only the prefix can select.
        int n = 0;
        const auto stage = [&](const QString &envPrefix, const QString &ndkRoot) {
            const QString ctx = tmp.path() + QStringLiteral("/ctx%1").arg(++n);
            if (!QDir().mkpath(ctx)) return QString();
            const QString inner = QStringLiteral(
                "sh -c '. \"$1/ndk-overlay.sh\"; if ndk_overlay_on \"$2\"; then "
                "stage_ndk_overlay \"$2\" \"$3\"; fi' gate \"$1\" \"$2\" \"$3\"");
            QProcess sh;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.remove(QStringLiteral("WITH_NDK"));
            sh.setProcessEnvironment(env);
            sh.start(QStringLiteral("sh"),
                     {QStringLiteral("-c"), envPrefix + inner, QStringLiteral("outer"), sbuild,
                      ndkRoot, ctx});
            if (!sh.waitForFinished(15000) || sh.exitCode() != 0) return QString();
            return ctx + QStringLiteral("/ndkguest/system");
        };
        const auto staged = [](const QString &sys) {
            return QFile::exists(sys + QStringLiteral("/etc/guest-sentinel")) &&
                   QFile::exists(sys + QStringLiteral("/lib64/host-sentinel.so"));
        };

        // EXPLICITLY OFF + payload present → nothing staged. The defect: the old gate copied here.
        const QString offSys = stage(off, root);
        QVERIFY(!offSys.isEmpty());
        QVERIFY2(!QDir(offSys).exists(), "arm_translate off still staged the translation overlay");
        // EXPLICITLY ON → both halves staged, guest payload and host bridge libs.
        const QString onSys = stage(on, root);
        QVERIFY(!onSys.isEmpty());
        QVERIFY2(staged(onSys), "arm_translate on did not stage the translation overlay");
        // UNSET (a hand invocation naming no selection) → the original presence rule: staged.
        const QString unsetSys = stage(QString(), root);
        QVERIFY(!unsetSys.isEmpty());
        QVERIFY2(staged(unsetSys), "an unset WITH_NDK no longer stages a vendored overlay");
        // Payload absent → nothing to stage, and no failure, whatever the selection.
        const QString absent = tmp.path() + QStringLiteral("/no-such-payload");
        for (const QString &prefix : {on, QString()}) {
            const QString sys = stage(prefix, absent);
            QVERIFY(!sys.isEmpty());
            QVERIFY(!QDir(sys).exists());
        }
    }

    // bd remora-6laf.3/.4. The Play spoof layer's context, staged by the real script against a
    // fixture payload. The vendored zygisk_pif is laid first and never overwritten, so Remora's
    // installer is always this checkout's whatever a payload carries, and the payload fills in only
    // what the vendored tree lacks: in a public clone, every binary.
    void spoofLayerLaysThePayloadUnderTheVendoredTree() {
        SKIP_ON_WINDOWS(kRunsSh);
        const QString sbuild =
            sourcePath(QStringLiteral("vendor/source-build"));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto put = [](const QString &path, const QByteArray &body) {
            if (!QDir().mkpath(QFileInfo(path).path())) return false;
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(body) == body.size();
        };
        const QString pay = tmp.path() + QStringLiteral("/zygisk_pif");
        for (const char *f : {"/rez-mod/lib64/libzygisk.so", "/rez-mod/bin/zygiskd64",
                              "/rez-mod/bin/zygisk-ptrace64", "/pifork/zygisk/x86_64.so",
                              "/shamiko/zygisk/x86_64.so", "/shamiko/module.prop"})
            QVERIFY(put(pay + QLatin1String(f), "payload\n"));
        QVERIFY(put(pay + QStringLiteral("/install.sh"), "a payload's own installer\n"));
        const QString nb = tmp.path() + QStringLiteral("/nb_sigfix");
        QVERIFY(put(nb + QStringLiteral("/module.prop"), "id=nb_sigfix\n"));
        QVERIFY(put(nb + QStringLiteral("/zygisk/x86_64.so"), "nb\n"));
        const auto run = [&](const QString &ctx, const QStringList &args) {
            QProcess sh;
            sh.start(QStringLiteral("sh"),
                     QStringList{sbuild + QStringLiteral("/stage-spoof-layer.sh"), ctx} + args);
            return sh.waitForFinished(30000) ? sh.exitCode() : -1;
        };
        const auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };

        const QString ctx = tmp.path() + QStringLiteral("/ctx");
        QCOMPARE(run(ctx, {"--payload", pay, "--spoof", "--shamiko", "--nb-sigfix", nb}), 0);
        const QString spoof = ctx + QStringLiteral("/spoof");
        // the tree the deploy installs from, as assemble-image.sh lays it
        for (const char *f : {"/rez-mod/lib64/libzygisk.so", "/rez-mod/bin/zygiskd64",
                              "/playintegrityfix/zygisk/x86_64.so", "/pif.json", "/install.sh",
                              "/devicespoof/post-fs-data.sh", "/nb_sigfix/zygisk/x86_64.so"})
            QVERIFY2(QFile::exists(spoof + QLatin1String(f)), f);
        QVERIFY(QFile::exists(ctx + QStringLiteral("/shamiko/zygisk/x86_64.so")));
        QVERIFY(!QDir(spoof + QStringLiteral("/shamiko")).exists());  // its own COPY
        // Remora's installer wins over the payload's
        QCOMPARE(read(spoof + QStringLiteral("/install.sh")),
                 read(sbuild + QStringLiteral("/features/zygisk_pif/install.sh")));
        // no merge directory left behind, and a re-stage replaces rather than nests
        QCOMPARE(QDir(ctx).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot),
                 (QStringList{"shamiko", "spoof"}));
        QCOMPARE(run(ctx, {"--payload", pay, "--spoof"}), 0);
        QVERIFY(!QDir(spoof + QStringLiteral("/rez-mod/rez-mod")).exists());
        QVERIFY(!QDir(ctx + QStringLiteral("/shamiko")).exists());  // not asked for this time
        QVERIFY(!QDir(spoof + QStringLiteral("/nb_sigfix")).exists());

        // Shamiko asked for and absent from every payload: refused by name, nothing staged. (The
        // vendored tree carries it only on a machine that has it, so this uses a payload-free
        // checkout's view: a payload directory that lacks it, and a check of the message.)
        QFile::remove(pay + QStringLiteral("/shamiko/zygisk/x86_64.so"));
        QFile::remove(pay + QStringLiteral("/shamiko/module.prop"));
        if (!QFile::exists(sbuild + QStringLiteral("/features/zygisk_pif/shamiko/zygisk/x86_64.so"))) {
            const QString bad = tmp.path() + QStringLiteral("/ctx-bad");
            QVERIFY(run(bad, {"--payload", pay, "--shamiko"}) != 0);
            QVERIFY(!QDir(bad + QStringLiteral("/shamiko")).exists());
        }
        QVERIFY(run(tmp.path() + "/ctx-x", {"--payload", tmp.path() + "/nope", "--spoof"}) != 0);
    }

    // bd remora-6laf.5. The arm_translate layer's context, staged by the SAME shell function the
    // stage script runs, against a fixture payload. python3 is a stub on PATH that records its
    // arguments: the real patcher refuses anything but the harvested binaries by hash, so the
    // fixture could never pass it, and what is under test here is the layout and the order, with
    // the patcher's own refusal driven by a stub that fails.
    void armLayerStagesPatchedPayloadAndProps() {
        SKIP_ON_WINDOWS(kRunsSh);
        const QString sbuild =
            sourcePath(QStringLiteral("vendor/source-build"));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = tmp.path() + QStringLiteral("/ndk_translation");
        const auto put = [](const QString &path) {
            if (!QDir().mkpath(QFileInfo(path).path())) return false;
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write("sentinel\n") == 9;
        };
        for (const char *f : {"/guest/system/bin/arm64/linker64", "/guest/system/lib64/arm64/libc.so",
                              "/guest/system/etc/ld.config.arm64.txt", "/lib64/libnb.so",
                              "/lib64/libndk_translation.so",
                              "/bin/ndk_translation_program_runner_binfmt_misc_arm64",
                              "/etc/cpuinfo.arm64.txt", "/etc/binfmt_misc/arm64_exe"})
            QVERIFY(put(root + QLatin1String(f)));
        const auto stub = [&](const QString &dir, int rc) {
            QVERIFY(QDir().mkpath(dir));
            QFile s(dir + QStringLiteral("/python3"));
            QVERIFY(s.open(QIODevice::WriteOnly));
            s.write(QStringLiteral("#!/bin/sh\necho \"$@\" >> \"%1/patched\"\nexit %2\n")
                        .arg(dir)
                        .arg(rc)
                        .toUtf8());
            s.close();
            QVERIFY(s.setPermissions(s.permissions() | QFileDevice::ExeOwner));
        };
        const auto run = [&](const QString &stubDir, const QString &payload, const QString &ctx) {
            QProcess sh;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("PATH"), stubDir + QLatin1Char(':') + env.value("PATH"));
            sh.setProcessEnvironment(env);
            sh.start(QStringLiteral("sh"),
                     {QStringLiteral("-c"),
                      QStringLiteral(". \"$1/ndk-overlay.sh\"; stage_arm_layer \"$2\" \"$1\" \"$3\""),
                      QStringLiteral("t"), sbuild, payload, ctx});
            return sh.waitForFinished(15000) ? sh.exitCode() : -1;
        };
        const auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };

        const QString okStub = tmp.path() + QStringLiteral("/ok"), ctx = tmp.path() + "/ctx";
        stub(okStub, 0);
        QCOMPARE(run(okStub, root, ctx), 0);
        const QString sys = ctx + QStringLiteral("/ndkguest/system");
        // The guest runtime and the host bridge libs, as a source build overlays them…
        QVERIFY(QFile::exists(sys + "/bin/arm64/linker64"));
        QVERIFY(QFile::exists(sys + "/lib64/arm64/libc.so"));
        QVERIFY(QFile::exists(sys + "/etc/ld.config.arm64.txt"));
        QVERIFY(QFile::exists(sys + "/lib64/libnb.so"));
        QVERIFY(QFile::exists(sys + "/lib64/libndk_translation.so"));
        // …what ndk.mk installs beside them, and not the binfmt_misc registrations, which a
        // privileged container would register in the host kernel.
        QVERIFY(QFile::exists(sys + "/bin/ndk_translation_program_runner_binfmt_misc_arm64"));
        QVERIFY(QFile::exists(sys + "/etc/cpuinfo.arm64.txt"));
        QVERIFY(!QDir(sys + "/etc/binfmt_misc").exists());
        // The patcher ran on the STAGED copies, never the payload.
        QCOMPARE(read(okStub + "/patched").trimmed(),
                 QStringLiteral("%1/patch-arm-translation.py %2/bin/arm64/linker64 "
                                "%2/lib64/libnb.so")
                     .arg(sbuild, sys));
        QVERIFY(read(sys + "/etc/init/remora-berberis.rc")
                    .contains(QLatin1String("setprop ro.berberis.flags accurate-sigsegv")));
        // The properties the public build pinned to "no bridge", undone by a file init reads
        // after /vendor/build.prop.
        const QString props = read(ctx + QStringLiteral("/ndkguest/odm_dlkm/etc/build.prop"));
        for (const char *line : {"ro.product.cpu.abilist=x86_64,arm64-v8a\n",
                                 "ro.product.cpu.abilist64=x86_64,arm64-v8a\n",
                                 "ro.product.cpu.abilist32=\n",
                                 "ro.dalvik.vm.native.bridge=libnb.so\n",
                                 "ro.dalvik.vm.isa.arm64=x86_64\n"})
            QVERIFY2(props.contains(QLatin1String(line)), line);
        QVERIFY(!props.contains(QLatin1String("native.bridge.exec")));

        // A re-stage replaces the context instead of nesting a second copy inside it.
        QCOMPARE(run(okStub, root, ctx), 0);
        QVERIFY(!QDir(sys + QStringLiteral("/system")).exists());

        // The patcher refusing (a re-harvested binary) leaves NO context, so the recipe's stage
        // check, which looks for the prop file, refuses rather than building an unpatched bridge.
        const QString badStub = tmp.path() + QStringLiteral("/bad");
        stub(badStub, 1);
        QVERIFY(run(badStub, root, ctx) != 0);
        QVERIFY(!QDir(ctx + QStringLiteral("/ndkguest")).exists());

        // No payload: refused by name, nothing staged.
        const QString empty = tmp.path() + QStringLiteral("/ctx-empty");
        QVERIFY(run(okStub, tmp.path() + QStringLiteral("/no-payload"), empty) != 0);
        QVERIFY(!QDir(empty + QStringLiteral("/ndkguest")).exists());
    }

    // bd remora-mdq2. An A17 build ran with the A16 pin sitting in .repo/manifests/remora-pinned.xml
    // and `repo sync --force-sync` dragged 23 projects back to lineage-23.0 revisions; it then died
    // in kati on a missing file, with nothing anywhere pointing at the manifest. The guard added for
    // it LOOKS LIKE IT CANNOT FAIL — the `cp` that writes that file is two commands earlier — which
    // is exactly why this test EXECUTES it and drives it BOTH WAYS. A guard that always passes and a
    // guard that always fails are indistinguishable to a contains() check, and only one of them is
    // worth having.
    void pinnedManifestClobberIsRefusedBeforeSync() {
        SKIP_ON_WINDOWS(kRunsSh);
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        SourceBuildPlan p;
        p.tree = QStringLiteral("/src/tree");
        p.sourceBuildDir = tmp.path();
        p.patchRoot = QStringLiteral("/root/source-patches");
        p.builtTag = QStringLiteral("t:1");
        p.lineage = true;
        p.pinned = true;
        // Non-empty repoUrl is load-bearing: sourceBuildSteps gates the whole repo bring-up on
        // `lineage && !repoUrl.isEmpty()`, so without it this test would assert against a command
        // that never contained a manifest step at all and pass or fail for the wrong reason.
        p.repoUrl = QStringLiteral("https://github.com/LineageOS/android.git");
        p.androidVersion = 17;
        const QString all = sourceBuildSteps(p, noVendor).join(QLatin1Char('\n'));

        // (1) It exists, names the VERSION-SUFFIXED pin, and runs BEFORE the sync. Order is the
        // whole point: after the sync, 1229 projects have already moved and the check is a post-
        // mortem. repoSyncVerifyCommand() already occupies that slot and cannot catch this — a
        // correctly-executed sync of the WRONG manifest leaves every project matching the pin it
        // was handed.
        // Anchored on the GROUP OPENER, not bare "cmp -s": the tree-version guard that now runs
        // ahead of the bring-up (bd remora-mdq2 item 3) also uses cmp -s, earlier in the chain,
        // so the bare needle extracts the wrong step.
        const int guardAt = all.indexOf(QStringLiteral("{ cmp -s "));
        const int syncAt = all.indexOf(QStringLiteral("repo sync -c -j8"));
        QVERIFY2(guardAt > 0, "no pinned-manifest guard in the bring-up command");
        QVERIFY2(syncAt > guardAt, "the guard must run BEFORE repo sync, not after");
        QVERIFY(all.contains(QStringLiteral("pinned-manifest-a17.xml")));

        // (2) Execute it. Extract the whole `{ cmp … exit 1; }; }` group and run it under sh with
        // the layout it expects, so the shell quoting is tested too and not just the wording.
        const QString endMark = QStringLiteral("exit 1; }; }");
        const int end = all.indexOf(endMark, guardAt);
        QVERIFY(end > guardAt);
        const int start = all.lastIndexOf(QLatin1Char('{'), guardAt);
        QVERIFY(start >= 0);
        const QString guard = all.mid(start, end - start + endMark.size());

        const QString cwd = tmp.path() + QStringLiteral("/tree");
        QVERIFY(QDir().mkpath(cwd + QStringLiteral("/.repo/manifests")));
        const QString inTree = cwd + QStringLiteral("/.repo/manifests/remora-pinned.xml");
        const QString pin = tmp.path() + QStringLiteral("/pinned-manifest-a17.xml");
        const auto put = [](const QString &path, const QByteArray &body) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return false;
            return f.write(body) == body.size();
        };
        const auto run = [&](QByteArray *out) {
            QProcess sh;
            sh.setWorkingDirectory(cwd);
            sh.start(QStringLiteral("sh"), {QStringLiteral("-c"), guard});
            if (!sh.waitForFinished(15000)) return -1;
            if (out) *out = sh.readAllStandardOutput();
            return sh.exitCode();
        };

        // MATCHING → passes. Without this half the guard could be refusing unconditionally and
        // every build would be blocked; the failing half alone would not notice.
        QVERIFY(put(pin, QByteArrayLiteral("<manifest>A17</manifest>\n")));
        QVERIFY(put(inTree, QByteArrayLiteral("<manifest>A17</manifest>\n")));
        QCOMPARE(run(nullptr), 0);

        // CLOBBERED → refuses non-zero AND says so. This is the A16-pin-in-an-A17-tree case, and
        // the message has to name the file, because the symptom it prevents (a kati missing-file
        // error minutes later) names something else entirely.
        QVERIFY(put(inTree, QByteArrayLiteral("<manifest>A16 pin, wrong tree</manifest>\n")));
        QByteArray said;
        QCOMPARE(run(&said), 1);
        QVERIFY2(said.contains("PINNED MANIFEST CLOBBERED"), said.constData());
        QVERIFY2(said.contains("remora-pinned.xml"), said.constData());

        // A MISSING in-tree pin must refuse too, not pass by accident: `cmp -s` on an absent file
        // is non-zero, which is the behaviour relied on here.
        QVERIFY(QFile::remove(inTree));
        QCOMPARE(run(nullptr), 1);
    }

    // bd remora-31sq finding 2: a series whose project is not in this tree used to be `git init`ed
    // into an empty repo, so every per-file step failed "does not exist in index" — 30 lines of
    // red for a one-line situation — and the tree gained zero-commit litter no manifest owns
    // (observed after the first A16 run). The gate that
    // replaced the init is emitted as shell for a possibly-remote build host, so this test
    // EXECUTES the full joined chain all three ways; a contains() check cannot tell a gate that
    // always skips from one that never does.
    void absentProjectIsSkippedNotInited() {
        SKIP_ON_WINDOWS(kRunsSh);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString tree = tmp.path() + QStringLiteral("/tree");
        QVERIFY(QDir().mkpath(tree));

        const auto runAll = [&](const QMap<QString, QString> &requiredBy, QByteArray *out,
                                QByteArray *err) {
            const QString all =
                buildApplyPatchCommands({QStringLiteral("minigbm_rendernode")}, tree,
                                        tmp.path() + QStringLiteral("/source-patches"), 17, {},
                                        requiredBy)
                    .join(QStringLiteral(" && "));
            QProcess sh;
            sh.start(QStringLiteral("sh"), {QStringLiteral("-c"), all});
            if (!sh.waitForFinished(15000)) return -1;
            if (out) *out = sh.readAllStandardOutput();
            if (err) *err = sh.readAllStandardError();
            return sh.exitCode();
        };

        // ABSENT + optional → the chain exits 0, says SKIP once naming the project, and — the
        // litter half — creates NOTHING under the tree.
        QByteArray out, err;
        QCOMPARE(runAll({}, &out, nullptr), 0);
        QVERIFY2(out.contains("SKIP minigbm_rendernode"), out.constData());
        QVERIFY2(out.contains("external/minigbm is not in this tree"), out.constData());
        QVERIFY(!QDir(tree + QStringLiteral("/external/minigbm")).exists());
        QVERIFY(!out.contains("does not exist in index"));

        // ABSENT + required by a feature → FATAL, non-zero, naming the feature: the
        // remora-82c.1 contract. "Not even in the tree" is the strongest form of cannot-land,
        // and a soft skip here ships a green image with the feature missing.
        QVERIFY(runAll({{QStringLiteral("minigbm_rendernode"), QStringLiteral("hw_video_decode")}},
                       nullptr, &err)
                != 0);
        QVERIFY2(err.contains("FATAL"), err.constData());
        QVERIFY2(err.contains("hw_video_decode"), err.constData());
        QVERIFY(!QDir(tree + QStringLiteral("/external/minigbm")).exists());

        // PRESENT → the gate passes and the chain proceeds into the real steps: with this
        // fixture's empty patch root it must reach the MISSING PATCH refusal, which is that
        // state's pre-existing failure and proves the skip did NOT fire. Without this arm a
        // gate that skips unconditionally would sail the other two.
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/external/minigbm")));
        QProcess init;
        init.start(QStringLiteral("git"),
                   {QStringLiteral("-C"), tree + QStringLiteral("/external/minigbm"),
                    QStringLiteral("init"), QStringLiteral("-q")});
        QVERIFY(init.waitForFinished(15000));
        QCOMPARE(init.exitCode(), 0);
        out.clear();
        err.clear();
        QVERIFY(runAll({}, &out, &err) != 0);
        QVERIFY2(err.contains("MISSING PATCH"), err.constData());
        QVERIFY2(!out.contains("is not in this tree"), out.constData());
    }
    // bd remora-mdq2 item 3. The incident put the A16 pin into the A17 tree and the
    // failure surfaced minutes later as a kati missing-file error naming nothing manifest-shaped.
    // The pin tripwire catches a clobbered pin; this guard ties the TREE to a version so a
    // wrong-version INVOCATION is refused before anything is touched — including on incremental
    // builds, which skip the bring-up and its tripwire entirely. Executed, not merely matched,
    // and in every arm: the guard's adoption logic must stamp only on PROOF, and the difference
    // between "stamps on proof" and "stamps on first touch" is invisible to contains().
    void treeVersionGuardRefusesWrongVersionBuilds() {
        SKIP_ON_WINDOWS(kRunsSh);
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString tree = tmp.path() + QStringLiteral("/tree");
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/.repo/manifests")));
        SourceBuildPlan p;
        p.tree = tree;
        p.sourceBuildDir = tmp.path();
        p.patchRoot = QStringLiteral("/root/source-patches");
        p.builtTag = QStringLiteral("t:1");
        p.lineage = true;
        p.pinned = true;
        p.repoUrl = QStringLiteral("https://github.com/LineageOS/android.git");
        p.androidVersion = 17;
        const QStringList steps = sourceBuildSteps(p, noVendor);
        const QString all = steps.join(QLatin1Char('\n'));

        // It exists, runs BEFORE the bring-up, and the bring-up stamps the marker only after the
        // pin tripwire passes — the order is the design.
        const QString guard = steps.filter(QStringLiteral("vf=")).value(0);
        QVERIFY2(!guard.isEmpty(), "no tree-version guard step emitted");
        QVERIFY(all.indexOf(QStringLiteral("vf=")) < all.indexOf(QStringLiteral("repo init")));
        const int stampAt = all.indexOf(QStringLiteral("echo 17 > .remora-android-version"));
        QVERIFY2(stampAt > all.indexOf(QStringLiteral("PINNED MANIFEST CLOBBERED")),
                 "the stamp must come after the pin tripwire, not before");

        const QString marker = tree + QStringLiteral("/.remora-android-version");
        const QString pin = tree + QStringLiteral("/.repo/manifests/remora-pinned.xml");
        const auto put = [](const QString &path, const QByteArray &body) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return false;
            return f.write(body) == body.size();
        };
        const auto run = [&](QByteArray *err) {
            QProcess sh;
            sh.start(QStringLiteral("sh"), {QStringLiteral("-c"), guard});
            if (!sh.waitForFinished(15000)) return -1;
            if (err) *err = sh.readAllStandardError();
            return sh.exitCode();
        };
        // A pin is identified by its <default> revision — the lineage branch — not by its bytes.
        // `note` varies the parts that carry no release identity (comments, group labels), which
        // is exactly what the project-group rename changed in the real snapshots.
        const auto pinFor = [](const char *branch, const char *note) {
            return QByteArray("<manifest>\n  <!-- ") + note +
                   " -->\n  <default remote=\"github\" revision=\"refs/heads/" + branch +
                   "\" sync-j=\"4\" sync-c=\"true\"/>\n</manifest>\n";
        };
        // The version-suffixed snapshots. Deliberately NOT byte-equal to anything installed in the
        // tree below: nothing in this guard may depend on them matching (see the next case).
        QVERIFY(put(tmp.path() + QStringLiteral("/pinned-manifest-a17.xml"),
                    pinFor("lineage-24.0", "snapshot")));
        QVERIFY(put(tmp.path() + QStringLiteral("/pinned-manifest-a16.xml"),
                    pinFor("lineage-23.0", "snapshot")));

        // UNMARKED, NO PIN → passes and deliberately stamps NOTHING: an empty tree has no
        // identity to adopt, and the bring-up settles it.
        QCOMPARE(run(nullptr), 0);
        QVERIFY(!QFile::exists(marker));

        // UNMARKED, PIN IS THIS VERSION'S BRANCH → adopts, EVEN THOUGH ITS BYTES DIFFER FROM THE
        // SNAPSHOT. This is the regression that re-opened the hole: the byte-cmp cut of
        // this guard left /mnt/Build/remora-a17 matching no snapshot after a comment-and-group
        // rename, so the tree stayed unmarked and an A16 build against it sailed through.
        const QByteArray installed = pinFor("lineage-24.0", "renamed groups, same release");
        QVERIFY(put(pin, installed));
        {
            QFile snap(tmp.path() + QStringLiteral("/pinned-manifest-a17.xml"));
            QVERIFY(snap.open(QIODevice::ReadOnly));
            QVERIFY2(snap.readAll() != installed,
                     "fixture is pointless unless the installed pin and the snapshot differ");
        }
        QCOMPARE(run(nullptr), 0);
        QVERIFY(QFile::exists(marker));
        {
            QFile f(marker);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll().trimmed(), QByteArrayLiteral("17"));
        }

        // MARKED WITH THE WRONG VERSION → refused, naming both versions and the remedy.
        QVERIFY(put(marker, QByteArrayLiteral("16\n")));
        QByteArray err;
        QVERIFY(run(&err) != 0);
        QVERIFY2(err.contains("marked Android 16"), err.constData());
        QVERIFY2(err.contains("per-version"), err.constData());

        // MARKED RIGHT → passes. Without this half the guard could refuse unconditionally.
        QVERIFY(put(marker, QByteArrayLiteral("17\n")));
        QCOMPARE(run(nullptr), 0);

        // UNMARKED, PIN IS A DIFFERENT VERSION'S BRANCH → refused, and the refusal names both the
        // release and the snapshot that carries it; stamping here would adopt the bug as identity,
        // which is the one thing the guard must never do.
        QVERIFY(QFile::remove(marker));
        QVERIFY(put(pin, pinFor("lineage-23.0", "an A16 tree, whatever the path says")));
        err.clear();
        QVERIFY(run(&err) != 0);
        QVERIFY2(err.contains("Android 16 pin"), err.constData());
        QVERIFY2(err.contains("pinned-manifest-a16.xml"), err.constData());
        QVERIFY(!QFile::exists(marker));

        // UNMARKED, PIN CARRIES NO BRANCH THIS BUILD KNOWS → passes unmarked. Fail-open is the
        // deliberate choice for "cannot tell", and the bring-up settles the identity; asserted so
        // that it stays a decision rather than becoming an accident.
        QVERIFY(put(pin, pinFor("lineage-99.0", "a release this binary has never heard of")));
        QCOMPARE(run(nullptr), 0);
        QVERIFY(!QFile::exists(marker));
        QVERIFY(put(pin, QByteArrayLiteral("<manifest><!-- no default element --></manifest>\n")));
        QCOMPARE(run(nullptr), 0);
        QVERIFY(!QFile::exists(marker));
    }


    // bd remora-82c.4. source_kind=aosp bypasses all three of the lineage path's version guards
    // (pinned manifest, container patch set, local manifests), so the version-safety work did not
    // cover it. The AOSP branch must (a) refuse an Android version with no release config — the one
    // thing Remora needs to lunch the tree — and (b) SAY that it applied neither the manifest sync
    // nor the container-compat patches, so a non-booting bring-your-own tree is a stated
    // expectation, not a silent green build.
    void aospSourceKindIsVersionGuardedAndDocumented() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto steps = [&](bool lineage, int ver) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.patchRoot = QStringLiteral("/root/source-patches");
            p.builtTag = QStringLiteral("t:1");
            p.lineage = lineage;
            p.androidVersion = ver;
            return sourceBuildSteps(p, noVendor);
        };
        auto joined = [](const QStringList &s) { return s.join(QLatin1Char('\n')); };

        // AOSP on a SUPPORTED version: no refusal, but the skip is announced.
        const QString aosp17 = joined(steps(false, 17));
        QVERIFY2(!aosp17.contains(QStringLiteral("no release config for Android")),
                 "A17 has a release config — must not refuse");
        QVERIFY(aosp17.contains(QStringLiteral("NOTE (source_kind=aosp)")));
        QVERIFY(aosp17.contains(QStringLiteral("container-compat patches")));
        // and it genuinely skips the lineage-only bringup + container-patch steps
        QVERIFY2(!aosp17.contains(QStringLiteral("apply-container-patches.sh")),
                 "AOSP must not run the container-compat apply");

        // AOSP on an UNSUPPORTED version: a hard, named refusal — and it must be an early step, so
        // the build cannot burn hours first. The refusal exits non-zero; callers join with " && ".
        const QString aosp18 = joined(steps(false, 18));
        QVERIFY(aosp18.contains(QStringLiteral(
            "source_kind=aosp: no release config for Android 18")));
        QVERIFY(aosp18.contains(QStringLiteral("exit 1")));

        // Lineage keeps its own guards and never grows the AOSP note.
        const QString lin17 = joined(steps(true, 17));
        QVERIFY(lin17.contains(QStringLiteral("apply-container-patches.sh")));
        QVERIFY(!lin17.contains(QStringLiteral("NOTE (source_kind=aosp)")));
    }

    // bd remora-4ei.81. Builds 5 and 6: the global repo sync printed "Checking out
    // local projects failed", exited 0, and the chain ran on — into a container-compat pass that
    // applied 0 of 24 projects. repo's exit code cannot gate the build, so both sync paths get
    // --fail-fast AND a HEAD-vs-manifest verify that fails naming the project. Pin the wiring:
    // the verify rides BOTH syncs, after the sync and before the lfs pull (whose `|| true` is the
    // one legitimate swallow in these chains).
    void repoSyncFailureFailsTheBuild() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        SourceBuildPlan p;
        p.tree = QStringLiteral("/src/tree");
        p.sourceBuildDir = QStringLiteral("/root/source-build");
        p.patchRoot = QStringLiteral("/root/source-patches");
        p.builtTag = QStringLiteral("t:1");
        p.lineage = true;
        p.androidVersion = 17;
        p.repoUrl = QStringLiteral("https://example/manifest");
        QString bringup, targeted;
        for (const QString &s : sourceBuildSteps(p, noVendor)) {
            if (s.contains(QLatin1String("repo init -u"))) bringup = s;
            if (s.contains(QLatin1String("--force-sync --verbose $(cat "))) targeted = s;
        }
        QVERIFY(!bringup.isEmpty());   // the self-healing bring-up rides repoUrl
        QVERIFY(!targeted.isEmpty());  // and the targeted sync always follows it
        QVERIFY(bringup.contains(
            QLatin1String("repo sync -c -j8 --fail-fast --force-sync --verbose")));
        const QString verifyAll = repoSyncVerifyCommand();
        QVERIFY(bringup.contains(verifyAll));
        QVERIFY(bringup.indexOf(verifyAll) > bringup.indexOf(QLatin1String("repo sync")));
        QVERIFY(bringup.indexOf(verifyAll) < bringup.indexOf(QLatin1String("git lfs pull")));
        QVERIFY(targeted.contains(QLatin1String("--fail-fast")));
    }

    // bd remora-4ei.9. WITH_CAMERA existed in stage-features.sh for days while nothing on the
    // Remora side ever set it, so the external camera provider was never staged and the image kept
    // reporting 0 cameras. That is the same dead-toggle shape as remora-4ei.55; pin the wiring.
    void cameraFlagReachesStageFeatures() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto stageStep = [&](bool camera) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.withCamera = camera;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(QLatin1String("stage-features.sh"))) return s;
            return QString();
        };
        const QString on = stageStep(true);
        QVERIFY(!on.isEmpty());
        QVERIFY(on.contains(QLatin1String("WITH_CAMERA=true")));
        // Off must leave the staging command untouched — stage-features.sh runs unconditionally
        // (it also stages c2-va and the VF gralloc line), so the flag must be the only difference.
        QVERIFY(!stageStep(false).contains(QLatin1String("WITH_CAMERA")));
    }

    // bd remora-4ei.37, and the same dead-toggle shape the test above exists for — a WITH_* that
    // stage-features.sh honours while nothing on the Remora side ever sets it. Worth pinning
    // separately rather than trusting the camera test to cover the pattern: this feature's failure
    // is SILENT in the strongest sense, since the image keeps advertising a microphone and
    // AudioRecord keeps succeeding, so a dead toggle here looks exactly like a working one until
    // somebody listens to the samples.
    // bd remora-cbd.2. The backport's retirement watch was a STANDING INSTRUCTION on a bead —
    // "after every repo sync, run check-lineage-backport.sh" — which is a recurring obligation with
    // nobody to remind them, and its own record drifted twice as a result. The build now runs it.
    // Pinned in both directions because a watch that fires on every profile is noise and a watch
    // that never fires is worse than none.
    void backportRetirementWatchRidesTheBuild() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto chainFor = [&](const QStringList &patches) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.patchRoot = QStringLiteral("/root/source-patches");
            p.builtTag = QStringLiteral("t:1");
            p.lineage = true;
            p.androidVersion = 17;
            p.enabledPatches = patches;
            return sourceBuildSteps(p, noVendor).join(QLatin1Char('\n'));
        };
        const QString on = chainFor({QStringLiteral("lineage_platform_backport")});
        QVERIFY2(on.contains(QLatin1String("check-lineage-backport.sh")),
                 "the backport is enabled but the build never checks whether it can be retired");
        // The verdict must be ACTED on, not merely printed into a multi-thousand-line log.
        QVERIFY2(on.contains(QLatin1String("=> RETIRE")),
                 "nothing watches for the retirement verdict — the check would scroll past unread");
        QVERIFY2(on.contains(QLatin1String("RETIREMENT CONDITION MET")),
                 "the retirement case prints no banner of its own");
        // Advisory, never fatal: the verdict describes UPSTREAM, not this build, and a failed
        // fetch must not kill a multi-hour build.
        const QString step = sourceBuildSteps([&] {
                                 SourceBuildPlan p;
                                 p.tree = QStringLiteral("/src/tree");
                                 p.sourceBuildDir = QStringLiteral("/root/source-build");
                                 p.patchRoot = QStringLiteral("/root/source-patches");
                                 p.builtTag = QStringLiteral("t:1");
                                 p.lineage = true;
                                 p.androidVersion = 17;
                                 p.enabledPatches = {QStringLiteral("lineage_platform_backport")};
                                 return p;
                             }(),
                                              noVendor)
                                 .filter(QStringLiteral("check-lineage-backport.sh"))
                                 .value(0);
        QVERIFY(!step.isEmpty());
        QVERIFY2(step.contains(QLatin1String("|| true")) && step.trimmed().endsWith("true"),
                 "the retirement watch can fail the build — a network hiccup would cost hours");
        // …and a profile without the backport neither pays for it nor is advised about it.
        QVERIFY2(!chainFor({QStringLiteral("minigbm_rendernode")})
                      .contains(QLatin1String("check-lineage-backport.sh")),
                 "the watch runs for a profile that does not carry the backport");
    }

    void usbAudioFlagReachesStageFeatures() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto stageStep = [&](bool usbAudio) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.withUsbAudio = usbAudio;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(QLatin1String("stage-features.sh"))) return s;
            return QString();
        };
        const QString on = stageStep(true);
        QVERIFY(!on.isEmpty());
        QVERIFY(on.contains(QLatin1String("WITH_USB_AUDIO=true")));
        QVERIFY(!stageStep(false).contains(QLatin1String("WITH_USB_AUDIO")));
    }

    // The feature's payload must actually be on disk under the name stage-features.sh stages, or
    // the flag above is wired to nothing. stage() refuses a missing payload at build time, but
    // that refusal happens inside a container mid-build; catching it here costs nothing.
    void usbAudioFeatureShipsItsPayload() {
        const auto mkFiles =
            readVendorPath(QStringLiteral("source-build/features/usb_audio/usb_audio.mk"));
        // Same two causes as vendoredPifProfileIsReachableAndDatable spells out: a wrong path, or
        // an installed-vendor build (-DREMORA_INSTALLED_VENDOR=ON) that has not had
        // `remora-install` run since the file appeared. Both read as "missing" here.
        QVERIFY2(mkFiles.size() == 1,
                 "usb_audio.mk not found under REMORA_VENDOR_DIR — wrong path, or an "
                 "installed-vendor build that needs remora-install. WITH_USB_AUDIO would stage "
                 "nothing and the image would keep recording silence.");
        const QByteArray mk = mkFiles.first().second;
        // The HAL and the policy include are the two halves; either alone is inert.
        QVERIFY(mk.contains("audio.usb.default"));
        QVERIFY(mk.contains("usb_audio_policy_configuration.xml"));
        // EXACTLY ONE CLAIMANT ON /vendor/etc/audio_policy_configuration.xml, and this assertion
        // replaces one that passed while being worthless. It used to require
        //     PRODUCT_PACKAGES_REMOVE += audio_policy_configuration.xml
        // in this file. That variable does not exist in the build system — `grep -r
        // PRODUCT_PACKAGES_REMOVE build/make/` is empty — so the test pinned the PRESENCE OF A LINE
        // NOTHING READS, reported green, and the first build to enable the feature died on the very
        // collision the assertion claimed to prevent:
        //     Makefile:139: error: overriding commands for target
        //     .../vendor/etc/audio_policy_configuration.xml
        // A test may only assert a mechanism that exists; naming the desired outcome is not enough.
        // Matched as an ASSIGNMENT, not as a substring: the comment above it in the .mk explains
        // the trap at length and naming it there must stay allowed — the file documenting why the
        // variable is useless is the main defence against someone re-adding it.
        for (const QByteArray &line : mk.split('\n'))
            QVERIFY2(!line.trimmed().startsWith("PRODUCT_PACKAGES_REMOVE"),
                     "PRODUCT_PACKAGES_REMOVE is not a thing in this build system — a line using "
                     "it reads as protection and provides none");
        // AND NO PRODUCT_COPY_FILES ONTO THE TOP-LEVEL POLICY, which is the other half of the same
        // lesson: soong emits an install rule for every prebuilt_etc in a registered namespace
        // regardless of PRODUCT_PACKAGES (proven on a2dp_audio_policy_configuration.xml — a rule,
        // no PRODUCT_PACKAGES entry, required by nothing), so a copy onto that destination is
        // always a second rule for one target and kati refuses at parse time. Two full builds died
        // on it before the declaration moved into a source patch.
        // Checked on NON-COMMENT lines only: the long note in the .mk quotes the kati error and
        // the destination path deliberately, and that documentation is the main thing stopping the
        // approach being retried. Matching it would forbid explaining it.
        for (const QByteArray &line : mk.split('\n')) {
            const QByteArray t = line.trimmed();
            if (t.startsWith('#')) continue;
            QVERIFY2(!t.contains("TARGET_COPY_OUT_VENDOR"),
                     "the feature copies into vendor/etc again — a copy onto the top-level audio "
                     "policy collides with the soong prebuilt_etc of the same path and cannot "
                     "build (bd remora-4ei.37)");
        }
        // THE DECLARATION NOW LIVES IN A SOURCE PATCH, and the feature must say so — a HAL with no
        // module declaration is installed, never opened, and records perfectly timed silence with
        // no error. That is indistinguishable from working, which is exactly why the dependency
        // has to be machine-readable rather than prose (the lesson sensors cost a live image for,
        // bd remora-4ei.53).
        const Feature *usb = findFeature(QStringLiteral("usb_audio"));
        QVERIFY2(usb, "the usb_audio feature is gone — if it was finished, update this test");
        QVERIFY2(usb->requiresPatches.contains(QStringLiteral("usb_audio_policy")),
                 "usb_audio does not require usb_audio_policy — the HAL would ship inert");
        // The patch half must exist, target frameworks/av, and be tied back to this feature so the
        // Source page never offers it alone: its <xi:include> names a fragment only the feature
        // installs, and an xi:include of a missing file fails the whole policy parse.
        const SourcePatch *pol = nullptr;
        for (const SourcePatch &p : sourcePatches())
            if (p.key == QLatin1String("usb_audio_policy")) pol = &p;
        QVERIFY2(pol, "the usb_audio_policy patch is not registered");
        QCOMPARE(pol->projectPath, QStringLiteral("frameworks/av"));
        QCOMPARE(pol->needs.feature, QStringLiteral("usb_audio"));
        QVERIFY2(!pol->defaultOn, "usb_audio_policy is default-on — it would break audio on every "
                                  "profile that does not also enable the usb_audio feature");
        // The patch FILE has to be on disk and has to actually add the include. The registry's
        // missing-file check (patchFilesExist) proves the first; only reading it proves the second,
        // and a series that applies cleanly while adding the wrong line is the failure mode this
        // whole bead is about — everything installs, nothing is declared, and the image records
        // perfectly timed silence.
        // Read as a SET rather than by index or count, so renumbering or splitting the series
        // cannot break this — what matters is that both halves are present somewhere in it.
        QByteArray diff;
        for (const QString &f : pol->patches)
            for (const auto &pf : readVendorPath(QStringLiteral("source-patches/") + pol->dir +
                                                 QStringLiteral("/") + f))
                diff += pf.second;
        QVERIFY2(!diff.isEmpty(), "the usb_audio_policy patch files are missing");
        // HALF ONE: the module is declared, or AudioPolicyManager never opens the HAL at all.
        QVERIFY2(diff.contains("audio_policy_configuration_generic.xml"),
                 "nothing targets the generic policy — that file is the prebuilt_etc's own src, "
                 "and editing it is the only route that keeps one install rule");
        QVERIFY2(diff.contains("+        <xi:include href=\"usb_audio_policy_configuration.xml\"/>"),
                 "the usb include is not ADDED — the HAL would be installed and never opened");
        // HALF TWO: the device is ATTACHED, or it stays merely declared and AudioSource.MIC keeps
        // resolving to the silent stub. Remora's container has no USB host stack, so the
        // hot-plug event that would normally connect it can never arrive (bd remora-4ei.37).
        QVERIFY2(diff.contains("+        <item>USB Device In</item>"),
                 "USB Device In is not declared in attachedDevices — with no USB host stack in the "
                 "container nothing will ever mark it connected");
        // …and the address, without which the HAL leaves card/device at -1 and
        // profile_read_device_info() fails with 'cannot read profile'. Measured exactly so.
        QVERIFY2(diff.contains("address=\"card=0;device=0\""),
                 "the attached devicePort carries no card/device address — the HAL cannot open it");
    }

    // bd remora-28ix.3.4 — the same dead-toggle shape a third time, and this one SHIPPED: the
    // first cut tested the mirror_agent FEATURE key against enabledPatches (a set that can never
    // contain it), so WITH_AGENT never fired and a full A17 validation build came out agent-less
    // with every gate green. Pin both directions: the plan flag reaches stage-features.sh, and
    // the patches list is NOT where the answer comes from.
    void agentFlagReachesStageFeatures() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto stageStep = [&](bool agent, const QStringList &patches) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.withAgent = agent;
            p.enabledPatches = patches;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(QLatin1String("stage-features.sh"))) return s;
            return QString();
        };
        QVERIFY(stageStep(true, {}).contains(QLatin1String("WITH_AGENT=true")));
        QVERIFY(!stageStep(false, {}).contains(QLatin1String("WITH_AGENT")));
        // the original bug, pinned: a stray "mirror_agent" among the PATCHES must change nothing
        QVERIFY(!stageStep(false, {QStringLiteral("mirror_agent")})
                     .contains(QLatin1String("WITH_AGENT")));
    }

    // bd remora-phvx — the public_release profile is a refusal contract held by THREE steps
    // (staging, the in-container build, assembly), and the dead-toggle history above is why each
    // is pinned separately: a flag that reaches two of them builds an image that looks public and
    // is not. A contradictory plan must never reach any of them.
    void publicReleaseReachesEveryGateAndRefusesPayloads() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        SourceBuildPlan p;
        p.tree = QStringLiteral("/src/tree");
        p.sourceBuildDir = QStringLiteral("/root/source-build");
        p.builtTag = QStringLiteral("remora24:x86_64-public-src");
        p.androidVersion = 17;
        p.withAgent = p.withMesaSource = true;
        p.publicRelease = true;
        const QStringList steps = sourceBuildSteps(p, noVendor);
        auto step = [](const QStringList &all, const char *needle) {
            for (const QString &s : all)
                if (s.contains(QLatin1String(needle))) return s;
            return QString();
        };
        QVERIFY(step(steps, "stage-features.sh").contains(QLatin1String("WITH_PUBLIC_RELEASE=true")));
        // do-build.sh runs installclean on it — the stale-partition half of the contract
        QVERIFY(step(steps, "--cgroup-parent=containers-remorabuild.slice")
                    .contains(QLatin1String("-e WITH_PUBLIC_RELEASE=true")));
        const QString assemble = step(steps, "assemble-image.sh");
        QVERIFY(assemble.contains(QLatin1String("WITH_PUBLIC_RELEASE=true")));
        QVERIFY(assemble.contains(QLatin1String("WITH_NDK=false")));
        // a private plan carries none of it
        p.publicRelease = false;
        for (const QString &s : sourceBuildSteps(p, noVendor))
            QVERIFY2(!s.contains(QLatin1String("WITH_PUBLIC_RELEASE")), qPrintable(s));
        // contradictory: the whole chain is one refusing step, naming what it refused
        p.publicRelease = true;
        p.withGapps = p.withNdk = true;
        const QStringList bad = sourceBuildSteps(p, noVendor);
        QCOMPARE(bad.size(), 1);
        QVERIFY(bad.first().contains(QLatin1String("gapps, arm_translate")));
        QVERIFY(bad.first().contains(QLatin1String("false")));
        QCOMPARE(publicReleaseConflicts(p), QStringList({"gapps", "arm_translate"}));
        // one public name, whatever the media features; the CPU variant still marks a tuned build
        QCOMPARE(sourceBuiltTag(17, true, false, false, false, true, {}, true),
                 QStringLiteral("remora24:x86_64-public-src"));
        QCOMPARE(sourceBuiltTag(17, true, false, false, false, true, QStringLiteral("broadwell"),
                                true),
                 QStringLiteral("remora24:x86_64-public-broadwell-src"));
    }

    // bd remora-ykhz R3 — the dead-toggle shape a fourth time, pinned before it can happen rather
    // than after. mesa_source is the feature whose silent failure would be worst of the four: it
    // replaces every GL and Vulkan driver in the image, so a flag that never reaches
    // stage-features.sh means a build that reports "Mesa from source" and ships the 24.0.8
    // prebuilts, which is indistinguishable from success until someone hashes the binaries.
    void mesaSourceFlagReachesStageFeatures() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto stageStep = [&](bool mesa) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.withMesaSource = mesa;
            for (const QString &s : sourceBuildSteps(p, noVendor))
                if (s.contains(QLatin1String("stage-features.sh"))) return s;
            return QString();
        };
        QVERIFY(stageStep(true).contains(QLatin1String("WITH_MESA_SOURCE=true")));
        QVERIFY(!stageStep(false).contains(QLatin1String("WITH_MESA_SOURCE")));
    }

    // bd remora-bm7.11. The arch variant is another build toggle that could silently reach
    // nothing (the fgj.11 / 4ei.55 dead-toggle shape): pin that setting it produces the
    // BoardConfig rewrite with its hard-fail anchor guard, that BOTH arch lines ride the one
    // sed, and that UNSET leaves the chain without any arch step at all. Plus the tag contract —
    // the variant lands right before -src, or a tuned image would overwrite the portable one
    // under the same name instead of coexisting with it.
    void archVariantReachesBoardConfigAndTag() {
        auto noVendor = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        auto steps = [&](const QString &v) {
            SourceBuildPlan p;
            p.tree = QStringLiteral("/src/tree");
            p.sourceBuildDir = QStringLiteral("/root/source-build");
            p.builtTag = QStringLiteral("t:1");
            p.androidVersion = 17;
            p.archVariant = v;
            return sourceBuildSteps(p, noVendor);
        };
        QString sedStep, guardStep;
        for (const QString &s : steps(QStringLiteral("broadwell"))) {
            if (s.contains(QLatin1String("sed -i")) && s.contains(QLatin1String("ARCH_VARIANT")))
                sedStep = s;
            if (s.contains(QLatin1String("no TARGET_ARCH_VARIANT anchor"))) guardStep = s;
        }
        QVERIFY(!guardStep.isEmpty());  // a missing anchor must fail the build, not skip silently
        QVERIFY(sedStep.contains(
            QLatin1String("s/^TARGET_ARCH_VARIANT := .*/TARGET_ARCH_VARIANT := broadwell/")));
        QVERIFY(sedStep.contains(QLatin1String(
            "s/^TARGET_2ND_ARCH_VARIANT := .*/TARGET_2ND_ARCH_VARIANT := broadwell/")));
        // THE DEVICE TREE IS PER VERSION, and this assertion used to pin the wrong one: it named
        // the upstream device tree for androidVersion 17, which was true until bd remora-28ix.4 R1
        // gave A17 its own tree and false from that commit onward. The test kept passing, so the CPU-tuning
        // sed went on rewriting a BoardConfig A17 does not build while images shipped tagged
        // -alderlake and compiled at baseline. Assert BOTH versions, from the release registry, so
        // a future tree move breaks a test instead of silently untuning the product.
        QVERIFY(sedStep.contains(
            QLatin1String("/src/tree/device/remora/remora_x86_64/BoardConfig.mk")));
        QVERIFY2(onlyRemoraDeviceTree(sedStep), qPrintable(sedStep));
        {
            SourceBuildPlan p16;
            p16.tree = QStringLiteral("/src/tree");
            p16.sourceBuildDir = QStringLiteral("/root/source-build");
            p16.builtTag = QStringLiteral("t:1");
            p16.androidVersion = 16;
            p16.archVariant = QStringLiteral("broadwell");
            QString sed16;
            for (const QString &s16 : sourceBuildSteps(p16, noVendor))
                if (s16.contains(QLatin1String("sed -i")) && s16.contains(QLatin1String("ARCH_VARIANT")))
                    sed16 = s16;
            // A16 NOW WRITES THE SAME BOARD CONFIG AS A17: it lunches remora_x86_64
            // from device/remora too, so this arm no longer proves "each version writes to a
            // DIFFERENT tree" — it proves the weaker but still load-bearing thing, that the path
            // comes from the registry row rather than a hardcoded product name. That is the whole
            // defect bd remora-28ix.4.2 recorded: the sed spent weeks editing another device tree
            // while A17 built from device/remora, because the path was assumed instead of looked up.
            QVERIFY(sed16.contains(
                QLatin1String("/src/tree/device/remora/remora_x86_64/BoardConfig.mk")));
            QVERIFY2(onlyRemoraDeviceTree(sed16), qPrintable(sed16));
        }
        // A missing board config must fail the build, not sed into the void.
        QVERIFY(guardStep.contains(QLatin1String("does not exist")));
        // Unset = byte-identical chain to before the option existed.
        for (const QString &s : steps(QString()))
            QVERIFY(!s.contains(QLatin1String("ARCH_VARIANT")));
        // ASSEMBLE IS TOLD THE SAME BOARDCONFIG THE SED EDITS (bd remora-bm7.13), from the same
        // registry field, so the file the build read and the file the image's stamp is read from
        // are the same file by construction. This is the divergence that made every -alderlake
        // image in the pre-eae38f6 window a baseline build; the stamp must not be able to repeat it.
        // UNCONDITIONAL, unlike the sed: a BASELINE build has to be stamped x86_64 too, or "no
        // label" would mean both "predates the stamp" and "built portable" and the guard could not
        // tell a checkable image from an uncheckable one.
        for (const QString &v : {QStringLiteral("broadwell"), QString()}) {
            QString asmStep;
            for (const QString &s : steps(v))
                if (s.contains(QLatin1String("assemble-image.sh"))) asmStep = s;
            QVERIFY(!asmStep.isEmpty());
            QVERIFY2(asmStep.contains(
                         QLatin1String("BOARD_CONFIG_DIR=device/remora/remora_x86_64")),
                     qPrintable(asmStep));
        }
        // The tag: variant right before -src; unset unchanged (incl. A16's bare family).
        QCOMPARE(sourceBuiltTag(17, true, true, false, true, true, QStringLiteral("broadwell")),
                 QStringLiteral("remora24:x86_64-gapps-wv-hwc2-broadwell-src"));
        QCOMPARE(sourceBuiltTag(17, true, true, false, true, true),
                 QStringLiteral("remora24:x86_64-gapps-wv-hwc2-src"));
        QCOMPARE(sourceBuiltTag(16, true, false, false, false, false),
                 QStringLiteral("remora23:x86_64-src"));
        // The curated GUI list must only offer tag-safe raw soong names.
        for (const QString &v : knownArchVariants()) {
            QVERIFY(!v.isEmpty());
            QVERIFY(!v.contains(QLatin1Char(' ')));
            QVERIFY(!v.contains(QLatin1Char(':')));
        }

        // THE TWO SIDES MUST AGREE (bd remora-bm7.12). sourceBuiltTag() names what the build
        // PRODUCES; resolve() names what a deploy RUNS. They are computed independently, and when
        // they drifted the tuned image was built and then never deployed — `up` silently kept the
        // portable one, which costs a whole build to notice. Assert the deploy tag is exactly the
        // built tag's promoted form (the built tag minus the "-src" Promote strips).
        RemoraConfig c;
        c.image.androidVersion = 17;
        c.image.features = QStringList{QStringLiteral("gapps"), QStringLiteral("widevine_l3"),
                                       QStringLiteral("hw_video_decode")};
        c.image.archVariant = QStringLiteral("broadwell");
        const QString built = sourceBuiltTag(17, true, true, false, true, true,
                                             QStringLiteral("broadwell"));
        QVERIFY(built.endsWith(QLatin1String("-src")));
        QCOMPARE(resolve(c, Backend::Bare).imageTag, built.chopped(4));
        // Unset variant: byte-identical to before the option existed, on both sides.
        c.image.archVariant.reset();
        QCOMPARE(resolve(c, Backend::Bare).imageTag,
                 sourceBuiltTag(17, true, true, false, true, true).chopped(4));
        // An explicit tag is a literal the user typed — the variant must NOT be appended to it.
        c.image.archVariant = QStringLiteral("broadwell");
        c.image.imageTag = QStringLiteral("some/pinned:tag");
        QCOMPARE(resolve(c, Backend::Bare).imageTag, QStringLiteral("some/pinned:tag"));
    }

    // ─────────── the pty wrapper's exit sentinel (bd remora-c7y) ───────────
    // A `script -qe` killed at session teardown kills its shell and exits 0, so a build killed
    // mid-compile printed 'source build OK'. The wrap makes the CHAIN report its own status as
    // the last line through the pty; judgePtyExit believes that line and calls a run without one
    // interrupted. This pins the wrap; the judgement itself is pinned in test_parsers.
    void ptyWrapperCarriesTheExitSentinel() {
        QCOMPARE(ptyRunArgv(QStringLiteral("make it"), QString()),
                 (QStringList{QStringLiteral("script"), QStringLiteral("-q"),
                              QStringLiteral("-e"), QStringLiteral("-c"),
                              QStringLiteral(
                                  "make it; rc=$?; echo \"REMORA-PTY-RC:$rc\"; exit $rc"),
                              QStringLiteral("/dev/null")}));
        // `exit $rc` is load-bearing: script -e / ssh keep returning exactly what the chain
        // returned, so every caller that judged honestly before is behaviourally untouched.
        QCOMPARE(ptyRunArgv(QStringLiteral("docker pull t"), QStringLiteral("u@h")),
                 (QStringList{QStringLiteral("ssh"), QStringLiteral("-tt"), QStringLiteral("u@h"),
                              QStringLiteral("sh -c 'docker pull t; rc=$?; "
                                             "echo \"REMORA-PTY-RC:$rc\"; exit $rc'")}));
    }


    // main.cpp's warnStalePifProfile (bd remora-4ei.47) reads this exact relative path through
    // readVendorPath, which returns EMPTY for a miss. A typo there does not fail loudly — it
    // switches the warning off forever, the same shape as the silently-ignored patch file in bd
    // remora-fgj.11 and the grep -c guard in remora-25u. Pin the path.
    // Deliberately NOT asserting the profile is stale: that would fail the day someone refreshes
    // it, i.e. punish the fix. Only that it is reachable and carries a date we can read.
    void vendoredPifProfileIsReachableAndDatable() {
        const auto files =
            readVendorPath(QStringLiteral("source-build/features/zygisk_pif/pif.json"));
        // 0 means the warning can never fire. Two causes, and the message must name both: the
        // path is wrong, or REMORA_VENDOR_DIR points at the INSTALLED tree (cmake
        // -DREMORA_INSTALLED_VENDOR=ON) and `remora-install` has not been run since the file
        // appeared. The second is not a code defect but fails identically — see memory
        // source-patches-version-aware for the same trap biting patch overrides.
        QVERIFY2(files.size() == 1,
                 "pif.json not found under REMORA_VENDOR_DIR — wrong path, or an installed-vendor "
                 "build that needs remora-install");
        // A far-FUTURE "today" with maxMonths 0 makes every DATABLE profile report, whatever its
        // patch level, so this stays green across refreshes. It must be future: age is signed, so
        // a past date yields negative months and reads as fresh — which is what this assertion
        // caught when it was first written the other way round.
        const auto age =
            stalePifProfile(QString::fromUtf8(files.first().second), QDate(2100, 1, 1), 0);
        QVERIFY(age.has_value());
        QVERIFY2(age->monthsOld != -1,
                 qPrintable(QStringLiteral("vendored pif.json has no parseable SECURITY_PATCH: '")
                            + age->securityPatch + QLatin1Char('\'')));
    }

    // ── app transfer (bd remora-4ei.75) — engine-level so the GUI worker runs tested code ──

    void transferAppsDryRunPullsNothing() {
        FakeSpawner sp;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n"
                           "package:/data/app/x/com.a-1/split_config.x86_64.apk\n");
        QStringList lines;
        const LineSink sink = [&lines](const QString &, const QString &t) { lines << t; };
        // com.gone has no pm path answer — the not-installed shape, counted as a skip
        const AppOpCounts c = transferApps(sp, QStringLiteral("S"), QString(),
                                           {QStringLiteral("com.a"), QStringLiteral("com.gone")},
                                           true, false, {}, {}, sink);
        QCOMPARE(c.done, 1);
        QCOMPARE(c.skipped, 1);
        QCOMPARE(c.failed, 0);
        QVERIFY(lines.at(0).contains(QLatin1String("would transfer com.a")));
        QVERIFY(lines.at(0).contains(QLatin1String("2 apks, split")));
        // A dry run must stay READ-ONLY: nothing pulled, nothing installed.
        for (const QString &call : sp.calls) {
            QVERIFY(!call.contains(QLatin1String(" pull ")));
            QVERIFY(!call.contains(QLatin1String("install-multiple")));
        }
    }

    void transferAppsInstallsEverySplitTogether() {
        FakeSpawner sp;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n"
                           "package:/data/app/x/com.a-1/split_config.en.apk\n"
                           "package:/data/app/x/com.a-1/split_config.x86_64.apk\n");
        const AppOpCounts c =
            transferApps(sp, QStringLiteral("S"), QStringLiteral("D"), {QStringLiteral("com.a")},
                         false, {});
        QCOMPARE(c.done, 1);
        QCOMPARE(c.failed, 0);
        // ONE install-multiple, on the TARGET, carrying all three splits — splitting this up is
        // exactly the bug the whole feature exists to avoid.
        QStringList installs;
        for (const QString &call : sp.calls)
            if (call.contains(QLatin1String("install-multiple"))) installs << call;
        QCOMPARE(installs.size(), 1);
        QVERIFY(installs.first().startsWith(QLatin1String("adb -s D install-multiple -r -g")));
        QVERIFY(installs.first().contains(QLatin1String("base.apk")));
        QVERIFY(installs.first().contains(QLatin1String("split_config.en.apk")));
        QVERIFY(installs.first().contains(QLatin1String("split_config.x86_64.apk")));
    }

    void transferAppsFailedInstallCountsAndContinues() {
        FakeSpawner sp;
        sp.installOk = false;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        sp.pmPaths[QStringLiteral("com.b")] =
            QStringLiteral("package:/data/app/x/com.b-1/base.apk\n");
        QStringList lines;
        const LineSink sink = [&lines](const QString &, const QString &t) { lines << t; };
        const AppOpCounts c = transferApps(sp, QStringLiteral("S"), QStringLiteral("D"),
                                           {QStringLiteral("com.a"), QStringLiteral("com.b")},
                                           false, false, {}, {}, sink);
        // One failure must not abort the run — the second app still gets its attempt.
        QCOMPARE(c.failed, 2);
        QCOMPARE(c.done, 0);
        QVERIFY(lines.at(0).startsWith(QLatin1String("  FAIL com.a")));
        QVERIFY(lines.at(1).startsWith(QLatin1String("  FAIL com.b")));
    }

    void importJobsGroupsSubdirSplitsAndLooseApks() {
        QTemporaryDir d;
        QDir().mkpath(d.path() + QLatin1String("/com.a"));
        auto touch = [](const QString &p) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        };
        touch(d.path() + QLatin1String("/com.a/base.apk"));
        touch(d.path() + QLatin1String("/com.a/split_config.en.apk"));
        touch(d.path() + QLatin1String("/loose.apk"));
        touch(d.path() + QLatin1String("/README.txt"));  // non-APK noise: never a job
        const QList<ApkJob> jobs = importJobs(d.path());
        QCOMPARE(jobs.size(), 2);
        // the subdirectory is one job with BOTH splits, base.apk leading (sorted)
        QCOMPARE(jobs.at(0).first, QStringLiteral("com.a"));
        QCOMPARE(jobs.at(0).second.size(), 2);
        QVERIFY(jobs.at(0).second.first().endsWith(QLatin1String("base.apk")));
        // the loose APK is its own single-APK job
        QCOMPARE(jobs.at(1).second, QStringList{d.path() + QLatin1String("/loose.apk")});
        // and a dry-run import reports both without a single adb call
        FakeSpawner sp;
        const AppOpCounts c = importApks(sp, QString(), jobs, true, {});
        QCOMPARE(c.done, 2);
        QVERIFY(sp.calls.isEmpty());
    }

    // bd remora-4ei.73 — window modes ride along with a transfer, but the target's own stored
    // answer must never be overwritten: it came from the first-launch prompt, i.e. the target
    // user's deliberate choice, and a migration must not undo it.
    void carryAppModesPrefersTargetsOwnAnswer() {
        RemoraConfig src, dst;
        src.integration.appModes = {QStringLiteral("com.a=desktop"),
                                    QStringLiteral("com.b=freeform"),
                                    QStringLiteral("com.c=freeform-stretch")};
        dst.integration.appModes = {QStringLiteral("com.b=desktop")};
        // com.a: carried; com.b: dst wins; com.c: not transferred; com.d: src has no answer
        const int n = carryAppModes(src, dst,
                                    {QStringLiteral("com.a"), QStringLiteral("com.b"),
                                     QStringLiteral("com.d")});
        QCOMPARE(n, 1);
        QCOMPARE(appModeFor(dst, QStringLiteral("com.a")), QStringLiteral("desktop"));
        QCOMPARE(appModeFor(dst, QStringLiteral("com.b")), QStringLiteral("desktop"));
        QVERIFY(appModeFor(dst, QStringLiteral("com.c")).isEmpty());
        QVERIFY(appModeFor(dst, QStringLiteral("com.d")).isEmpty());
    }

    // The carry consumes counts.transferred, so its contract matters: real installs only — a
    // failed install or a dry run must contribute nothing, or modes would be carried for apps
    // that never arrived.
    void transferredListsOnlyRealInstalls() {
        FakeSpawner sp;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        QCOMPARE(transferApps(sp, QStringLiteral("S"), QStringLiteral("D"),
                              {QStringLiteral("com.a")}, false, {})
                     .transferred,
                 QStringList{QStringLiteral("com.a")});
        QVERIFY(transferApps(sp, QStringLiteral("S"), QString(), {QStringLiteral("com.a")}, true,
                             {})
                    .transferred.isEmpty());  // dry run
        FakeSpawner bad;
        bad.installOk = false;
        bad.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        QVERIFY(transferApps(bad, QStringLiteral("S"), QStringLiteral("D"),
                             {QStringLiteral("com.a")}, false, {})
                    .transferred.isEmpty());  // failed install
    }

    // bd remora-4ei.73, the data half. The rooted pipeline's ORDER is the contract: pack on the
    // source, pull, push, then the target's single stat→untar→chown→restorecon script — the stat
    // must read the uid the TARGET's install assigned before the untar clobbers the directory,
    // and restorecon must run after the chown because the per-app MLS categories derive from the
    // owning uid. The lib symlink and cache dirs are excluded at pack time.
    //
    // The ROUTE is the other half of the contract, and it is what a live run caught: the two
    // rooted scripts must go through `docker exec <container>`, NOT `adb shell su -c`. adb shell
    // is uid 2000 and Magisk's su client gets EACCES reaching magiskd's 0700 socket, so the adb
    // route can never carry data off a container — the pack failed on the first real transfer
    // ever attempted. The tar itself still rides adb pull/push: it is chmod 644 on purpose.
    void transferAppDataRunsRootedPipelineInOrder() {
        FakeSpawner sp;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        const AppOpCounts c =
            transferApps(sp, QStringLiteral("S"), QStringLiteral("D"), {QStringLiteral("com.a")},
                         false, true, RootExec{QStringLiteral("csrc"), {}},
                         RootExec{QStringLiteral("cdst"), {}}, {});
        QCOMPARE(c.done, 1);
        QCOMPARE(c.dataDone, 1);
        QCOMPARE(c.dataFailed, 0);
        int install = -1, pack = -1, pull = -1, push = -1, restore = -1;
        for (int i = 0; i < sp.calls.size(); ++i) {
            const QString &call = sp.calls.at(i);
            if (call.contains(QLatin1String("install-multiple"))) install = i;
            if (call.contains(QLatin1String("tar -C /data/data -cf"))) pack = i;
            if (call.contains(QLatin1String("remora-appdata-com.a.tar")) &&
                call.contains(QLatin1String(" pull ")))
                pull = i;
            if (call.contains(QLatin1String(" push "))) push = i;
            if (call.contains(QLatin1String("tar -C /data/data -xf"))) restore = i;
        }
        QVERIFY(install >= 0 && pack > install && pull > pack && push > pull && restore > push);
        const QString packCall = sp.calls.at(pack);
        QVERIFY(packCall.startsWith(QLatin1String("docker exec csrc sh -c ")));  // SOURCE, uid 0
        QVERIFY(!packCall.contains(QLatin1String("su -c")));
        QVERIFY(packCall.contains(QLatin1String("--exclude=com.a/lib")));
        QVERIFY(packCall.contains(QLatin1String("--exclude=com.a/cache")));
        QVERIFY(packCall.contains(QLatin1String("--exclude=com.a/code_cache")));
        // the tar still moves unprivileged, over adb, so one side may be remote
        QVERIFY(sp.calls.at(pull).startsWith(QLatin1String("adb -s S ")));
        QVERIFY(sp.calls.at(push).startsWith(QLatin1String("adb -s D ")));
        const QString restoreCall = sp.calls.at(restore);
        QVERIFY(restoreCall.startsWith(QLatin1String("docker exec cdst sh -c ")));  // TARGET
        QVERIFY(!restoreCall.contains(QLatin1String("su -c")));
        // ordering inside the one script: stat before untar, chown before restorecon
        const int atStat = restoreCall.indexOf(QLatin1String("stat -c"));
        const int atUntar = restoreCall.indexOf(QLatin1String("tar -C /data/data -xf"));
        const int atChown = restoreCall.indexOf(QLatin1String("chown -R"));
        const int atRecon = restoreCall.indexOf(QLatin1String("restorecon -RF"));
        QVERIFY(atStat >= 0 && atUntar > atStat && atChown > atUntar && atRecon > atChown);
    }

    // A data failure must NOT un-count the install: the app landed and is merely logged out —
    // the exact state a default transfer produces — so done/transferred keep it and only
    // dataFailed says what went wrong.
    void transferAppDataFailureKeepsTheInstall() {
        FakeSpawner sp;
        sp.dataRestoreOk = false;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        QStringList lines;
        const LineSink sink = [&lines](const QString &, const QString &t) { lines << t; };
        const AppOpCounts c =
            transferApps(sp, QStringLiteral("S"), QStringLiteral("D"), {QStringLiteral("com.a")},
                         false, true, RootExec{QStringLiteral("csrc"), {}},
                         RootExec{QStringLiteral("cdst"), {}}, sink);
        QCOMPARE(c.done, 1);
        QCOMPARE(c.failed, 0);
        QCOMPARE(c.dataDone, 0);
        QCOMPARE(c.dataFailed, 1);
        QCOMPARE(c.transferred, QStringList{QStringLiteral("com.a")});
        QVERIFY(lines.at(1).startsWith(QLatin1String("  data FAIL com.a")));
    }

    // Without withData not a single data command may run, and a withData DRY run stays as
    // read-only as a plain one — it only announces the carry.
    void transferAppDataIsOptInAndDryRunSafe() {
        FakeSpawner sp;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        transferApps(sp, QStringLiteral("S"), QStringLiteral("D"), {QStringLiteral("com.a")},
                     false, false, RootExec{QStringLiteral("csrc"), {}},
                     RootExec{QStringLiteral("cdst"), {}}, {});
        for (const QString &call : sp.calls)
            QVERIFY(!call.contains(QLatin1String("remora-appdata-")));
        FakeSpawner dry;
        dry.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        QStringList lines;
        const LineSink sink = [&lines](const QString &, const QString &t) { lines << t; };
        const AppOpCounts c =
            transferApps(dry, QStringLiteral("S"), QString(), {QStringLiteral("com.a")}, true,
                         true, RootExec{QStringLiteral("csrc"), {}},
                         RootExec{QStringLiteral("cdst"), {}}, sink);
        QCOMPARE(c.dataDone, 1);
        QVERIFY(lines.at(0).contains(QLatin1String("+ app data")));
        for (const QString &call : dry.calls) {
            QVERIFY(!call.contains(QLatin1String("remora-appdata-")));
            QVERIFY(!call.contains(QLatin1String(" pull ")));
        }
    }

    // A side this machine has no profile for — the raw host:port escape hatch — resolves no
    // container, so it KEEPS the adb `su` route. That route cannot work on a Remora container
    // (uid 2000 vs magiskd's 0700 socket), which is what moved the profile case to docker exec,
    // but on a genuinely rooted phone Magisk does grant adb shell root. Dropping it would turn
    // "works there" into "refuses", and the container fix was not about taking a device away.
    void transferAppDataKeepsTheAdbSuRouteWithoutAContainer() {
        FakeSpawner sp;
        sp.pmPaths[QStringLiteral("com.a")] =
            QStringLiteral("package:/data/app/x/com.a-1/base.apk\n");
        const AppOpCounts c = transferApps(sp, QStringLiteral("S"), QStringLiteral("D"),
                                           {QStringLiteral("com.a")}, false, true, {}, {}, {});
        QCOMPARE(c.dataDone, 1);
        int pack = -1, restore = -1;
        for (int i = 0; i < sp.calls.size(); ++i) {
            if (sp.calls.at(i).contains(QLatin1String("tar -C /data/data -cf"))) pack = i;
            if (sp.calls.at(i).contains(QLatin1String("tar -C /data/data -xf"))) restore = i;
        }
        QVERIFY(pack >= 0 && restore > pack);
        for (const int i : {pack, restore}) {
            QVERIFY(sp.calls.at(i).startsWith(QLatin1String("adb -s ")));
            QVERIFY(sp.calls.at(i).contains(QLatin1String("su -c")));
            QVERIFY(!sp.calls.at(i).contains(QLatin1String("docker exec")));
            // su is off every app-visible PATH (bd remora-4ei.101), so the call must say where
            // it lives or this route regresses to "su: not found".
            QVERIFY(sp.calls.at(i).contains(QLatin1String("PATH=/debug_ramdisk:/sbin:")));
        }
    }

    // bd remora-4ei.79 — the probe names ANY driver now, not just xe. The sort rules are the
    // load-bearing part: first non-nvidia node becomes the render node (nvidia is Venus-only,
    // gbm gralloc cannot drive it), vgem is skipped (virtual GEM renders nothing), and the
    // driver that was always anonymous before — amdgpu — comes back named, which is what the
    // VA-driver mapping (bd remora-4ei.80) keys off.
    void probeSortsRenderNodesByRole() {
        FakeSpawner sp;
        sp.renderNodes = QStringLiteral("/dev/dri/renderD128 nvidia\n/dev/dri/renderD129 xe\n");
        const HostCapabilities c = probeCapabilities(Backend::Bare, QString(), sp);
        QCOMPARE(*c.hostRenderNode, QStringLiteral("/dev/dri/renderD129"));
        QCOMPARE(*c.hostRenderDriver, QStringLiteral("xe"));
        QCOMPARE(*c.hostNvidiaRenderNode, QStringLiteral("/dev/dri/renderD128"));
        FakeSpawner amd;
        amd.renderNodes = QStringLiteral("/dev/dri/renderD128 amdgpu\n");
        const HostCapabilities a = probeCapabilities(Backend::Bare, QString(), amd);
        QCOMPARE(*a.hostRenderNode, QStringLiteral("/dev/dri/renderD128"));
        QCOMPARE(*a.hostRenderDriver, QStringLiteral("amdgpu"));
        QVERIFY(!a.hostNvidiaRenderNode.has_value());
        FakeSpawner vg;
        vg.renderNodes = QStringLiteral("/dev/dri/renderD128 vgem\n/dev/dri/renderD129 i915\n");
        QCOMPARE(*probeCapabilities(Backend::Bare, QString(), vg).hostRenderDriver,
                 QStringLiteral("i915"));
    }

    // bd remora-7oih.2 — a Windows client has no sh, lsmod, id or local docker. The probe asked
    // them all anyway, and `command -v adb` is a SHELL BUILTIN, so `check` and the GUI readiness
    // reported adb MISSING on every Windows install. There the bare-only rows are not asked at all,
    // and adb is looked up with where.exe — while the remote rows still go over ssh as before.
    void windowsProbeAsksForNoLinuxTool() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        sp.renderNodes = QStringLiteral("/dev/dri/renderD128 xe\n");
        const HostCapabilities c =
            probeCapabilities(Backend::Remote, QStringLiteral("tester@guest.invalid"), sp);
        QCOMPARE(c.localOs, LocalOs::Windows);
        QVERIFY(c.adbPresent);
        QVERIFY(hasCall(sp, "where /q adb"));
        for (const char *tool : {"sh", "lsmod", "id", "docker"})
            QVERIFY2(!spawnsProgram(sp, QLatin1String(tool)), tool);
        QVERIFY(c.sshReachable.value_or(false));  // the docker host is still asked, over ssh
        QVERIFY(c.remoteDockerOk.value_or(false));

        FakeSpawner noAdb;
        noAdb.os = LocalOs::Windows;
        noAdb.adbTool = false;
        QVERIFY(!probeCapabilities(Backend::Remote, QStringLiteral("tester@guest.invalid"), noAdb)
                     .adbPresent);

        // A bare profile asked from Windows scans nothing locally either: there is no /dev/dri,
        // and no LAN to read for a docker host that does not exist. The GUI's GPU picker is empty.
        FakeSpawner bare;
        bare.os = LocalOs::Windows;
        bare.renderNodes = QStringLiteral("/dev/dri/renderD128 xe\n");
        const HostCapabilities b = probeCapabilities(Backend::Bare, QString(), bare);
        QVERIFY(!b.hostRenderNode.has_value());
        QVERIFY(probeRenderNodes(bare).isEmpty());
        QVERIFY(!spawnsProgram(bare, "sh"));

        // and on Linux nothing changed: the same probe still asks the shell and the local tools
        FakeSpawner onLinux;
        onLinux.renderNodes = QStringLiteral("/dev/dri/renderD128 xe\n");
        QVERIFY(probeCapabilities(Backend::Bare, QString(), onLinux).hostRenderNode.has_value());
        QVERIFY(hasCall(onLinux, "command -v adb"));
        QVERIFY(spawnsProgram(onLinux, "lsmod"));
        QCOMPARE(probeRenderNodes(onLinux).size(), 1);
    }

    // …and status/doctor from Windows ask no local docker: the bare row reads not-running, which is
    // the truth, rather than a docker error about a daemon the client was never meant to have.
    void windowsStatusAndDoctorAskNoLocalDocker() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        sp.localPs = QStringLiteral("remora-bm");  // would answer, if it were asked
        const StatusReport r = probeStatus(Backend::Bare, sp);
        QVERIFY(!r.bareRunning);
        QVERIFY(!r.bareDockerError.has_value());
        QVERIFY(!spawnsProgram(sp, "docker"));

        FakeSpawner doc;
        doc.os = LocalOs::Windows;
        const QString report =
            doctorReport(doc, withMacvlan(), Backend::Remote, QStringLiteral("default"));
        QVERIFY(!report.contains(QStringLiteral("docker (local)")));
        QVERIFY(report.contains(QStringLiteral("docker (tester@guest.invalid)")));
        QVERIFY(!spawnsProgram(doc, "docker"));
        QVERIFY(!spawnsProgram(doc, "sh"));
    }

    // The SwiftShader-fallback detector's datum: status carries the renderer SurfaceFlinger
    // actually composited with, for the bare rows like the vm rows always did.
    void statusCarriesBareGles() {
        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm");
        sp.glesDump =
            QStringLiteral("GLES: Google (SwiftShader), SwiftShader driver, OpenGL ES 3.0\n");
        const StatusReport r = probeStatus(Backend::Bare, sp);
        QVERIFY(r.bareRunning);
        QVERIFY(r.bareGles.value_or(QString()).contains(QLatin1String("SwiftShader")));
    }

    // The dead-front-door datum (bd remora-yn4): status distinguishes "fwd down, adbd healthy" —
    // the state where every adb consumer sees a dead device while Android is fine — from a device
    // that is actually down. A container that is not running answers nothing, and unknown must
    // stay unknown rather than read as down.
    void statusCarriesForwarderProbe() {
        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm");
        const StatusReport healthy = probeStatus(Backend::Bare, sp);
        QVERIFY(healthy.forwarder.fwdListening.value_or(false));
        QVERIFY(healthy.forwarder.adbdListening.value_or(false));
        QCOMPARE(healthy.forwarder.service.value_or(QString()), QStringLiteral("running"));

        FakeSpawner dead;
        dead.localPs = QStringLiteral("remora-bm");
        dead.forwarderProbe = QStringLiteral("fwd_l=0\nadbd_l=1\nsvc=stopped\n");
        const StatusReport door = probeStatus(Backend::Bare, dead);
        QVERIFY(!door.forwarder.fwdListening.value_or(true));
        QVERIFY(door.forwarder.adbdListening.value_or(false));

        FakeSpawner down;  // no container: the probe never runs, everything stays unknown
        const StatusReport unknown = probeStatus(Backend::Bare, down);
        QVERIFY(!unknown.forwarder.fwdListening.has_value());
        QVERIFY(!unknown.forwarder.adbdListening.has_value());
    }

    // The same datum as the JSON consumers read it: the object is there, with real booleans, only
    // when a running container was probed, and absent (not null, not empty, not false) otherwise.
    void statusJsonShowsFrontDoorOnlyWhenProbed() {
        const auto statusJson = [](FakeSpawner &sp) {
            QJsonParseError err;
            const QJsonDocument doc =
                QJsonDocument::fromJson(statusToJson(probeStatus(Backend::Bare, sp)).toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return std::optional<QJsonObject>();
            return std::optional<QJsonObject>(doc.object());
        };

        FakeSpawner sp;
        sp.localPs = QStringLiteral("remora-bm");
        const auto healthy = statusJson(sp);
        QVERIFY(healthy.has_value());
        QVERIFY(healthy->value("adb_front_door").isObject());
        const QJsonObject open = healthy->value("adb_front_door").toObject();
        QVERIFY(open.value("forwarder_listening").isBool());
        QVERIFY(open.value("forwarder_listening").toBool());
        QVERIFY(open.value("adbd_listening").isBool());
        QVERIFY(open.value("adbd_listening").toBool());
        QCOMPARE(open.value("service").toString(), QStringLiteral("running"));

        FakeSpawner dead;
        dead.localPs = QStringLiteral("remora-bm");
        dead.forwarderProbe = QStringLiteral("fwd_l=0\nadbd_l=1\nsvc=stopped\n");
        const auto door = statusJson(dead);
        QVERIFY(door.has_value());
        QVERIFY(door->value("adb_front_door").isObject());
        const QJsonObject shut = door->value("adb_front_door").toObject();
        // isBool first: a missing or null field reads false through toBool() and must not pass.
        QVERIFY(shut.contains("forwarder_listening"));
        QVERIFY(shut.value("forwarder_listening").isBool());
        QCOMPARE(shut.value("forwarder_listening").toBool(true), false);
        QVERIFY(shut.value("adbd_listening").isBool());
        QVERIFY(shut.value("adbd_listening").toBool());
        QCOMPARE(shut.value("service").toString(), QStringLiteral("stopped"));

        FakeSpawner down;  // no container: nothing was probed, so nothing may be claimed
        const auto unknown = statusJson(down);
        QVERIFY(unknown.has_value());
        QVERIFY(!unknown->contains("adb_front_door"));
        QVERIFY(unknown->value("adb_front_door").isUndefined());
    }

    // Docker being unreachable is not the container being stopped. The report carries
    // the daemon's own line, and the JSON omits `running` rather than emit a false one.
    void statusSaysDockerUnreachable() {
        FakeSpawner down;
        down.localPs = QStringLiteral("remora-bm");  // the container IS up — docker just cannot say
        down.dockerDown = QStringLiteral(
            "Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker "
            "daemon running?\n");
        const StatusReport r = probeStatus(Backend::Bare, down);
        QVERIFY(!r.bareRunning);
        QVERIFY(r.bareDockerError.has_value());
        QVERIFY(r.bareDockerError->startsWith(QLatin1String("Cannot connect to the Docker daemon")));
        const QJsonObject bare =
            QJsonDocument::fromJson(statusToJson(r).toUtf8()).object().value("bare").toObject();
        QVERIFY(!bare.contains("running"));
        QVERIFY(bare.contains("docker_error"));
        FakeSpawner up;
        up.localPs = QStringLiteral("remora-bm");
        const QJsonObject ok =
            QJsonDocument::fromJson(statusToJson(probeStatus(Backend::Bare, up)).toUtf8())
                .object().value("bare").toObject();
        QVERIFY(ok.value("running").toBool());
        QVERIFY(!ok.contains("docker_error"));
    }

    void assertAdbEndpointGatesOnDeviceState() {
        FakeSpawner sp;
        QCOMPARE(assertAdbEndpoint(sp, QStringLiteral("10.0.0.5:5555")),
                 QStringLiteral("10.0.0.5:5555"));
        FakeSpawner off;
        off.adbStates = QStringList{QStringLiteral("offline")};
        QVERIFY(assertAdbEndpoint(off, QStringLiteral("10.0.0.5:5555")).isEmpty());
        QVERIFY(assertAdbEndpoint(sp, QString()).isEmpty());  // no target resolved upstream
    }

    // ─────────── the flow probe's gate (bd remora-d3t) ───────────
    // probeMirrorFlow opens its window with a gate: once the encoder is plainly carrying frames
    // it answers early instead of spending the full ten seconds. The gate lives INSIDE the window
    // rather than ahead of it, because a gate with its own window costs its seconds on top
    // whenever it fails to fire — measured, that made an idle mirror slower (17.2s) than having no
    // gate at all (13.3s).
    static ResolvedConfig hostEncodeRc() {
        RemoraConfig cfg;
        cfg.mirror.hostEncode = true;  // drives the mirror by itself since the v1 purge
        cfg.gpu.mode = GpuMode::Host;
        return resolve(cfg, Backend::Bare);
    }
    static QString endpointScan() {
        return QStringLiteral(
            "enc 100 0::/user.slice/app.slice/remora-detached-1-0.scope \n"
            "mir 200 0::/user.slice/app.slice/remora-detached-2-0.scope \n");
    }
    // readCounters reads the encoder log then the mirror log, so each sample pops two entries.
    static QString encLine(qint64 n) {
        return QStringLiteral("[encoder] 1 ms: %1 packets encoded\n").arg(n);
    }
    static QString mirLine(qint64 n) {
        return QStringLiteral("[server] INFO: Composed %1 frames\n").arg(n);
    }

    void flowProbeAnswersEarlyOnceTheEncoderIsPlainlyCarryingFrames() {
        SKIP_ON_WINDOWS(kBareOnlyGpu);
        const ResolvedConfig rc = hostEncodeRc();
        QVERIFY(rc.hostEncode && rc.hostEncodeDrivesMirror);
        FakeSpawner sp;
        sp.mirrorEndpoints = endpointScan();
        // t0, then the gate read 4s later: 120 packets = 30 fps, far above the threshold. Only two
        // samples are staged, so a third read would return empty and fail the probe — which is
        // exactly how this asserts that the full window was never waited for.
        sp.counterReads = QStringList{encLine(1000), mirLine(5000),
                                      encLine(1120), mirLine(5120)};
        const auto flow = probeMirrorFlow(sp, rc, 10000);
        QVERIFY(flow.has_value());
        QVERIFY(!flow->starved);
        QCOMPARE(qRound(flow->encodedFps), 30L);
        QVERIFY2(sp.counterReads.isEmpty(), "it read more than the gate needed");
    }

    // The shape starvation actually produces: the keepalive is so slow that no printed line lands
    // inside the gate, so the encoder counter does not move and the gate cannot fire. The probe
    // then spends its full window and judges on the ratio — 300 composed against 30 encoded.
    void flowProbeFallsThroughToTheFullWindowWhenTheGateCannotFire() {
        SKIP_ON_WINDOWS(kBareOnlyGpu);
        const ResolvedConfig rc = hostEncodeRc();
        FakeSpawner sp;
        sp.mirrorEndpoints = endpointScan();
        sp.counterReads = QStringList{encLine(1000), mirLine(5000),    // t0
                                      encLine(1000), mirLine(5120),    // gate: encoder frozen
                                      encLine(1030), mirLine(5300),    // full window
                                      // the confirming second pass a starved verdict requires
                                      encLine(2000), mirLine(9000),
                                      encLine(2000), mirLine(9120),
                                      encLine(2030), mirLine(9300)};
        const auto flow = probeMirrorFlow(sp, rc, 10000);
        QVERIFY(flow.has_value());
        QVERIFY2(flow->starved, "a frozen encoder against a composing device is the whole fault");
        QCOMPARE(qRound(flow->composedFps), 30L);
        QCOMPARE(qRound(flow->encodedFps), 3L);  // the ~3.3 fps keepalive, as measured live
    }

    // The fault the gate waved through live (bd remora-yxy): 18 fps encoded against 30 composed.
    // That is more than twice the 8 fps floor, so an absolute-only gate answers healthy on the
    // short window — where judgeMirrorFlow can never say starved — and the ratio test that would
    // have condemned it never runs. The gate must also ask the RELATIVE question and fall through.
    void flowProbeGateRefusesAnEncoderAboveTheFloorButBelowTheRatio() {
        SKIP_ON_WINDOWS(kBareOnlyGpu);
        const ResolvedConfig rc = hostEncodeRc();
        FakeSpawner sp;
        sp.mirrorEndpoints = endpointScan();
        // Each pass: t0, the gate 4s in (72/120 = 18 vs 30 fps, ratio 0.60), then the full window
        // (180/300 over 10s — enough composed frames for a real verdict). Starved needs a
        // confirming second pass, and its gate must decline for the same reason as the first.
        sp.counterReads = QStringList{encLine(1000), mirLine(5000),
                                      encLine(1072), mirLine(5120),
                                      encLine(1180), mirLine(5300),
                                      encLine(2000), mirLine(9000),
                                      encLine(2072), mirLine(9120),
                                      encLine(2180), mirLine(9300)};
        const auto flow = probeMirrorFlow(sp, rc, 10000);
        QVERIFY(flow.has_value());
        QVERIFY2(flow->starved, "18 fps against 30 composed is the flicker the user reported");
        QCOMPARE(qRound(flow->encodedFps), 18L);
        QCOMPARE(qRound(flow->composedFps), 30L);
        QVERIFY2(sp.counterReads.isEmpty(), "both passes must spend their full window");
    }

    // The relative check must not cost the fast path its other honest case: a still screen
    // composes nothing while the encoder legitimately keeps emitting. Zero composed frames is not
    // a shortfall to measure — the gate treats it as healthy and answers on the short window.
    void flowProbeGateStillFiresEarlyOnAnIdleMirror() {
        SKIP_ON_WINDOWS(kBareOnlyGpu);
        const ResolvedConfig rc = hostEncodeRc();
        FakeSpawner sp;
        sp.mirrorEndpoints = endpointScan();
        // Two samples only: t0 and the gate — encoder at 30 fps, composed counter frozen. A third
        // read would come back empty and fail the probe, which is how this asserts the early exit.
        sp.counterReads = QStringList{encLine(1000), mirLine(5000),
                                      encLine(1120), mirLine(5000)};
        const auto flow = probeMirrorFlow(sp, rc, 10000);
        QVERIFY(flow.has_value());
        QVERIFY(!flow->starved);
        QVERIFY2(sp.counterReads.isEmpty(), "an idle mirror must still get the gate's fast path");
    }

    void flowProbeRefusesToAnswerWithoutBothEnds() {
        const ResolvedConfig rc = hostEncodeRc();
        // An encoder nothing is watching has nothing to be starved of, so half an answer is none.
        FakeSpawner half;
        half.mirrorEndpoints = QStringLiteral("enc 100 0::/remora-detached-1-0.scope \nmir  \n");
        QVERIFY(!probeMirrorFlow(half, rc, 10000).has_value());

        // A device-encoded profile has no host encoder in the path at all.
        RemoraConfig plain;
        FakeSpawner none;
        QVERIFY(!probeMirrorFlow(none, resolve(plain, Backend::Bare), 10000).has_value());
        QVERIFY(none.calls.isEmpty());  // and it must not even look
    }

    // ─────────── husk reaper (bd remora-8d7) ───────────
    // A mirror whose guest server was killed out from under it stays alive forever, holding an
    // ESTAB slot on the host encoder's video socket with nothing composing behind it. Reproduced
    // live: the healthy client held two tunnel connections, the husk zero.

    void huskReaperKillsExactlyWhatTheScanNames() {
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare, std::nullopt,
                               QStringLiteral("Android 17"));
        const QString t = QStringLiteral("10.0.0.5:5555");
        const QString title = QStringLiteral("Android 17");
        sp.mirrorSnapshots = {
            {mirrorAt(4242, t, title), mirrorAt(4243, t, title), mirrorAt(4244, t, title)}};
        sp.huskScanOut = QStringLiteral("4242\n4243\n");
        QCOMPARE(reapMirrorHusks(sp, ctx, t, {}), 2);
        // `ss` judges the instance's own mirrors, which the registry named — not a pgrep pattern.
        QVERIFY2(hasCall(sp, "for p in 4242 4243 4244;"), qPrintable(sp.calls.join('\n')));
        QVERIFY(!hasCall(sp, "pgrep"));
        // TERM alone is not enough for a husk — it is a stuck client by definition — so the
        // escalation must follow (the same stop the rejected-attempt reap uses).
        QVERIFY(stopped(sp, {4242, 4243}));
        QVERIFY(!everStopped(sp, 4244));
    }

    // The two ways the scan says "nothing to do" are indistinguishable on purpose: a host with no
    // `ss` exits 0 with no output, exactly like a host whose mirrors are all healthy. Absent
    // evidence must never become a kill — reaping a live mirror because a diagnostic tool is
    // missing would be far worse than the leak this closes.
    void huskReaperKillsNothingWithoutEvidence() {
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare, std::nullopt,
                               QStringLiteral("Android 17"));
        sp.mirrorSnapshots = {
            {mirrorAt(4242, QStringLiteral("10.0.0.5:5555"), QStringLiteral("Android 17"))}};
        sp.huskScanOut = QString();
        QCOMPARE(reapMirrorHusks(sp, ctx, QStringLiteral("10.0.0.5:5555"), {}), 0);
        QVERIFY(sp.stops.isEmpty());
    }

    void huskReaperNeverTurnsStrayOutputIntoAKill() {
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare, std::nullopt,
                               QStringLiteral("Android 17"));
        sp.mirrorSnapshots = {
            {mirrorAt(4242, QStringLiteral("10.0.0.5:5555"), QStringLiteral("Android 17"))}};
        // Anything that is not one of the candidates is dropped: this output reaches a stop call,
        // and a stray word — or a pid the registry never named — has no business there.
        sp.huskScanOut = QStringLiteral("ss: command not found\n-1\n99 88\n777\n\n");
        QCOMPARE(reapMirrorHusks(sp, ctx, QStringLiteral("10.0.0.5:5555"), {}), 0);
        QVERIFY(sp.stops.isEmpty());
    }

    // An app or desktop window (--new-display) is a session in its own right, not a mirror, and
    // must never be reaped by a connect that is only refreshing the mirror.
    void huskReaperLeavesFullscreenDisplayLaunchesAlone() {
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare, std::nullopt,
                               QStringLiteral("Android 17"), /*fullscreenDisplay=*/true);
        sp.huskScanOut = QStringLiteral("4242\n");
        QCOMPARE(reapMirrorHusks(sp, ctx, QStringLiteral("10.0.0.5:5555"), {}), 0);
        QVERIFY(sp.calls.isEmpty());  // it must not even ask
        QCOMPARE(sp.mirrorQueries, 0);
    }

    // A mirror with no session yet is no husk: it has outlived no server. Above all the splash,
    // which animates a cold boot for minutes owning no tunnel at all — exactly the shape the `ss`
    // test reads as dead once it is past the age floor.
    void huskReaperNeverJudgesAMirrorThatHasNotConnectedYet() {
        FakeSpawner sp;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare, std::nullopt,
                               QStringLiteral("Android 17"));
        sp.mirrorSnapshots = {{mirrorAt(4242, QStringLiteral("10.0.0.5:5555"),
                                        QStringLiteral("Android 17"), MirrorKind::Splash,
                                        MirrorSessionState::Pending, 120000)}};
        sp.huskScanOut = QStringLiteral("4242\n");
        QCOMPARE(reapMirrorHusks(sp, ctx, QStringLiteral("10.0.0.5:5555"), {}), 0);
        QVERIFY(!spawnsProgram(sp, QStringLiteral("sh")));  // nothing to ask `ss` about
    }

    // Windows has no `ss` (bd remora-7oih.1), so the mirror's own report decides: a registered
    // session that came up and lost its tunnel, past the age floor. Nothing else — not a young
    // loss, not a live or a starting session, not a mirror nobody registered, not another
    // instance's window or a PIP — and nothing reaches a shell.
    void huskReaperWithoutPosixUserlandReadsTheSelfReport() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        auto ctx = makeContext(RemoraConfig{}, Backend::Bare, std::nullopt,
                               QStringLiteral("Android 17"));
        const QString t = QStringLiteral("10.0.0.5:5555");
        const QString title = QStringLiteral("Android 17");
        MirrorEntry legacy = mirrorAt(505, t, title, MirrorKind::Main, MirrorSessionState::Unknown);
        legacy.registered = false;
        sp.mirrorSnapshots = {
            {mirrorAt(501, t, title, MirrorKind::Main, MirrorSessionState::Lost, 60000),
             mirrorAt(502, t, title, MirrorKind::Main, MirrorSessionState::Lost, 5000),
             mirrorAt(503, t, title, MirrorKind::Main, MirrorSessionState::Live, 60000),
             mirrorAt(504, t, title, MirrorKind::Splash, MirrorSessionState::Pending, 60000),
             legacy,
             mirrorAt(506, t, QStringLiteral("Other"), MirrorKind::Main, MirrorSessionState::Lost),
             mirrorAt(507, t, pipWindowTitle(title), MirrorKind::Pip, MirrorSessionState::Lost)}};
        sp.huskScanOut = QStringLiteral("502\n503\n504\n505\n");  // must not even be asked
        QCOMPARE(reapMirrorHusks(sp, ctx, t, {}), 1);
        QVERIFY2(stopped(sp, {501}), qPrintable(sp.calls.join('\n')));
        QCOMPARE(sp.stops.size(), 1);
        QVERIFY(!spawnsProgram(sp, QStringLiteral("sh")));
    }

    // ─────────── teardown reap (bd remora-e5x.19.1) ───────────
    // The mirror is detached into its own scope so Remora's exit cannot kill it — which meant
    // nothing killed it when its CONTAINER died either. Observed live after a full destroy: the
    // mirror client and the frame encoder both still running against a device that no longer
    // existed, flickering over the next session's encoder at ~0.5 GiB each.

    void teardownReapsClientsEncoderAndSockets() {
        FakeSpawner sp;
        const QString t = QStringLiteral("10.0.0.5:5555");
        // Every kind on the target goes — the device is being destroyed — and nothing on a
        // serial that merely starts with it.
        sp.mirrorSnapshots = {{mirrorAt(311, t, QStringLiteral("A17")),
                               mirrorAt(312, t, pipWindowTitle(QStringLiteral("A17")),
                                        MirrorKind::Pip),
                               mirrorAt(313, QStringLiteral("10.0.0.5:55550"), QStringLiteral("B"))}};
        sp.orphanEncoderPids = QStringLiteral("507\n");
        QCOMPARE(reapSessionOrphans(sp, QStringLiteral("remora-a17"), t, {}), 3);
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        // An orphan is a stuck client by definition, so the escalation must follow.
        QVERIFY2(stopped(sp, {311, 312, 507}), qPrintable(calls));
        QVERIFY(!everStopped(sp, 313));
        QVERIFY(!calls.contains(QStringLiteral("pgrep")));
        // The socket inodes go with them: a leftover inode is what made a dead stack look
        // present to the next generation's adoption checks.
        QVERIFY(calls.contains(QStringLiteral(
            "rm -f /tmp/remora-frames-remora-a17/frame.sock /tmp/remora-video-remora-a17.sock")));
    }

    // No serial (an unpinned macvlan profile, or a takeover of a profile this process cannot
    // resolve) means no client lookup worth trusting — but the encoder and its sockets derive
    // from the container NAME alone, so they are cleaned regardless.
    void teardownWithoutATargetStillCleansTheEncoder() {
        FakeSpawner sp;
        sp.mirrorSnapshots = {{mirrorAt(311, QStringLiteral("10.0.0.5:5555"), QStringLiteral("A"))}};
        sp.orphanEncoderPids = QStringLiteral("507\n");
        QCOMPARE(reapSessionOrphans(sp, QStringLiteral("remora-a17"), QString(), {}), 1);
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        QCOMPARE(sp.mirrorQueries, 0);
        QVERIFY(stopped(sp, {507}));
        QVERIFY(!everStopped(sp, 311));
        // The frame socket moved into a per-container DIRECTORY so the composer can reach it
        // through a bind mount (bd remora-emoe phase 2). Teardown removes the SOCKET and
        // leaves the directory: it is the bind source, and deleting it under a running
        // container would strand the mount.
        QVERIFY(calls.contains(QStringLiteral("rm -f /tmp/remora-frames-remora-a17/frame.sock")));
    }

    // bd remora-7oih.3 — host encode is bare-only, so a REMOTE teardown has no encoder on this
    // machine to find. Its half of the reap must not run there: it would scan and delete in this
    // machine's /tmp for a container that never had anything here. The clients are still reaped,
    // because the mirror runs here on every backend.
    void remoteTeardownLeavesTheEncoderHalfAlone() {
        FakeSpawner sp;
        const QString t = QStringLiteral("10.0.0.5:5555");
        sp.mirrorSnapshots = {{mirrorAt(311, t, QStringLiteral("A17"))}};
        sp.orphanEncoderPids = QStringLiteral("507\n");
        QCOMPARE(reapSessionOrphans(sp, QStringLiteral("remora-a17"), t, {}, Backend::Remote), 1);
        const QString calls = sp.calls.join(QLatin1Char('\n'));
        QVERIFY2(stopped(sp, {311}), qPrintable(calls));
        QVERIFY(!everStopped(sp, 507));
        QVERIFY(!calls.contains(QStringLiteral("remora-frame-encoder")));
        QVERIFY(!calls.contains(QStringLiteral("/tmp/remora-frames-")));
    }

    // ─────────── image-side shell helpers (engine/HostShell, bd remora-7oih.3) ───────────

    // A process that never STARTED used to read as success: Qt leaves exitCode() at 0 with
    // NormalExit, and the GUI-thread wait returns at once for a process that is not running. So a
    // missing ssh turned every library scan, prune and push into "it worked, and said nothing".
    // Run on the test's main thread, which is the GUI-thread path the bug lived on.
    void shellThatNeverStartsIsAFailureNotSilence() {
        QTemporaryDir empty;
        QVERIFY(empty.isValid());
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", empty.path().toLocal8Bit());  // no ssh, no sh: nothing can start
        const ShellResult r = runShellRc(QStringLiteral("user@host.invalid"), QStringLiteral("true"));
        const QString out = runShell(QStringLiteral("user@host.invalid"), QStringLiteral("echo hi"));
        const ShellResult pipe = runHostPipe({QString(), {QStringLiteral("docker"), QStringLiteral("save"), QStringLiteral("x")}},
                                             {QString(), {QStringLiteral("docker"), QStringLiteral("load")}}, 5000);
        qputenv("PATH", path);
        QCOMPARE(r.rc, 127);
        QVERIFY2(r.out.contains(QLatin1String("could not start ssh")), qPrintable(r.out));
        QVERIFY(out.isEmpty());
        QCOMPARE(pipe.rc, 127);
    }

    // …while a command that runs keeps its own exit code and output. A blank host is this
    // machine: sh on Linux, and on Windows, which has none, a refusal saying where to run it.
    void aBlankHostRunsHereOrSaysWhyNot() {
#ifdef Q_OS_WIN
        const ShellResult r = runShellRc(QString(), QStringLiteral("echo out; exit 3"));
        QCOMPARE(r.rc, 127);
        QCOMPARE(r.out, noLocalShellMessage());
        QVERIFY(runShell(QString(), QStringLiteral("echo hi")).isEmpty());
#else
        QCOMPARE(runShellRc(QString(), QStringLiteral("echo out; exit 3")).rc, 3);
        QCOMPARE(runShell(QString(), QStringLiteral("echo hi")), QStringLiteral("hi\n"));
#endif
    }

    // The push/fetch pipe, without a local sh: two processes joined by Qt. The data arrives
    // intact, and the exit code follows pipefail — a producer that died is not a successful load,
    // and a consumer that refused the stream is named rather than the producer's EPIPE.
    void hostPipeCarriesTheBytesAndReportsEitherEndFailing() {
        SKIP_ON_WINDOWS("the ends are local sh processes, and Windows runs no local host command");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.path() + QStringLiteral("/got");
        const HostCommand sink{QString(), {QStringLiteral("sh"), QStringLiteral("-c"),
                                           QStringLiteral("cat > '%1'").arg(file)}};
        const ShellResult ok = runHostPipe(
            {QString(), {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("printf abc; echo note >&2")}},
            sink, 10000);
        QCOMPARE(ok.rc, 0);
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("abc"));
        QVERIFY(ok.out.contains(QLatin1String("note")));  // the producer's stderr is kept
        QCOMPARE(runHostPipe({QString(), {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("printf x; exit 5")}},
                             {QString(), {QStringLiteral("cat")}}, 10000).rc,
                 5);
        QCOMPARE(runHostPipe({QString(), {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("printf x")}},
                             {QString(), {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("exit 7")}},
                             10000).rc,
                 7);
    }

    // A remote stage runs under /bin/sh -c on the far side (the login shell may be fish), with each
    // word quoted, batch-mode so a password prompt can never hang a transfer.
    void hostCommandArgvNamesTheShellAndQuotesEachWord() {
        const QStringList a = hostCommandArgv(
            {QStringLiteral("user@h"), {QStringLiteral("docker"), QStringLiteral("save"), QStringLiteral("a b")}});
        QCOMPARE(a.mid(0, 3), (QStringList{QStringLiteral("ssh"), QStringLiteral("-o"),
                                           QStringLiteral("BatchMode=yes")}));
        QVERIFY(a.contains(QStringLiteral("user@h")));
        QCOMPARE(a.at(a.indexOf(QStringLiteral("user@h")) + 1), QStringLiteral("/bin/sh"));
        QVERIFY(a.last().contains(QLatin1String("a b")));
        QCOMPARE(hostCommandArgv({QString(), {QStringLiteral("docker"), QStringLiteral("load")}}),
                 (QStringList{QStringLiteral("docker"), QStringLiteral("load")}));
    }

    // A one-route HTTP server for the fetches below: /missing is a 404, anything else `body`.
    static QString serveBody(QTcpServer &server, const QByteArray &body) {
        if (!server.listen(QHostAddress::LocalHost)) return QString();
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &body] {
            QTcpSocket *s = server.nextPendingConnection();
            QObject::connect(s, &QTcpSocket::readyRead, s, [s, &body] {
                const QByteArray req = s->readAll();
                if (req.contains("GET /missing"))
                    s->write("HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found");
                else
                    s->write("HTTP/1.0 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                             "\r\n\r\n" + body);
                s->disconnectFromHost();
            });
        });
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }

    // The http(s) half of a fetch, with Qt instead of a local curl: the body streams into the
    // consumer, and an error status hands it NOTHING — `docker load` would report a 404 page as a
    // corrupt archive and the real reason would be lost.
    void httpFetchStreamsTheBodyAndNeverAnErrorPage() {
        SKIP_ON_WINDOWS("the consumer is a local sh, and Windows runs no local host command");
        QTcpServer server;
        const QByteArray body(300000, 'z');  // several reads' worth
        const QString base = serveBody(server, body);
        QVERIFY(!base.isEmpty());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.path() + QStringLiteral("/got");
        const HostCommand sink{QString(), {QStringLiteral("sh"), QStringLiteral("-c"),
                                           QStringLiteral("cat > '%1'").arg(file)}};
        const ShellResult ok = runHttpToHost(base + QStringLiteral("/image.tar"), sink, 20000);
        QCOMPARE(ok.rc, 0);
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), body);
        }
        const ShellResult missing = runHttpToHost(base + QStringLiteral("/missing"), sink, 20000);
        QVERIFY(missing.rc != 0);
        QVERIFY2(missing.out.contains(QLatin1String("404")), qPrintable(missing.out));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("not found"));
    }

    // The small-document GET the mirror index uses: the body, or nothing for an error.
    void httpGetSmallReturnsTheBodyOrNothing() {
        QTcpServer server;
        const QByteArray body(300000, 'z');
        const QString base = serveBody(server, body);
        QVERIFY(!base.isEmpty());
        QCOMPARE(httpGetSmall(base + QStringLiteral("/index.txt"), 10000), body);
        QString err;
        QVERIFY(httpGetSmall(base + QStringLiteral("/missing"), 10000, &err).isEmpty());
        QVERIFY(err.contains(QLatin1String("404")));
    }

    // Anything that is not a bare pid is dropped before it can reach the stop call — the same
    // arbitrary-argument guard the husk scan carries, for the encoder half's `ps` output.
    void teardownNeverTurnsStrayOutputIntoAKill() {
        FakeSpawner sp;
        sp.orphanEncoderPids = QStringLiteral("awk: not found\n-9 1\n");
        QCOMPARE(reapSessionOrphans(sp, QStringLiteral("remora-a17"),
                                    QStringLiteral("10.0.0.5:5555"), {}),
                 0);
        QVERIFY(sp.stops.isEmpty());
    }

    // Remora's own guest (bd remora-7oih.5) is named by a reserved ssh_host and reached by its
    // forwarded port, key and pinned host key: ssh and scp both expand the name, and nothing else.
    void managedGuestNameExpandsInSshAndScpOnly() {
        ManagedHost h;
        h.alias = guestAlias();
        h.user = QStringLiteral("remora");
        h.sshPort = 47022;
        h.identityFile = QStringLiteral("C:\\state\\client");
        h.knownHostsFile = QStringLiteral("C:\\state\\known_hosts");
        registerManagedHost(h);
        const QStringList ssh = Spawner::sshArgv(guestAlias(), QStringLiteral("true"));
        QVERIFY(ssh.join(' ').contains(QStringLiteral(
            "-p 47022 -i C:\\state\\client -o IdentitiesOnly=yes -o BatchMode=yes "
            "-o StrictHostKeyChecking=yes -o UserKnownHostsFile=C:\\state\\known_hosts "
            "remora@127.0.0.1 /bin/sh -c 'true'")));
        QVERIFY(!ssh.contains(guestAlias()));
        // A Windows source path has a colon too; it is not a host.
        const QStringList scp = Spawner::scpArgv(
            {QStringLiteral("C:\\Remora\\vendor\\fwd"), guestAlias() + QStringLiteral(":.remora/fwd")});
        QCOMPARE(scp.first(), QStringLiteral("scp"));
        QCOMPARE(scp.mid(scp.size() - 3),
                 (QStringList{QStringLiteral("--"), QStringLiteral("C:\\Remora\\vendor\\fwd"),
                              QStringLiteral("remora@127.0.0.1:.remora/fwd")}));
        QVERIFY(scp.contains(QStringLiteral("-P")));
        // Any other host is passed through untouched, with no options of the guest's.
        const QStringList other = Spawner::scpArgv(
            {QStringLiteral("/v/fwd"), QStringLiteral("me@nas:.remora/fwd")});
        QCOMPARE(other, (QStringList{QStringLiteral("scp"), QStringLiteral("--"),
                                     QStringLiteral("/v/fwd"), QStringLiteral("me@nas:.remora/fwd")}));
        QVERIFY(!Spawner::sshArgv(QStringLiteral("me@nas"), QStringLiteral("true")).contains(
            QStringLiteral("-p")));
        forgetManagedHost(guestAlias());
    }
    // ─────────── mirror registry (bd remora-7oih.1) ───────────
    // Every find / stop / count of mirrors went through pgrep patterns over the Linux command
    // line. On Windows each one returned nothing: a reconnect left the old mirror beside the new,
    // a rejected attempt was never killed, Stop left windows open, the PIP toggle only ever
    // opened, and auto-sleep froze a device in use.

    // The Windows-shaped reconnect: the previous mirror is found and stopped, the new one is
    // launched, and none of it goes near a shell or procps.
    void reconnectWithoutPosixUserlandReplacesTheMirrorWithoutAShell() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        auto ctx = makeContext(withGuest(), Backend::Remote, std::nullopt,
                               QStringLiteral("Android 17"));
        sp.mirrorSnapshots = {{mirrorAt(900, ctx.rc.target, QStringLiteral("Android 17"))}};
        RecordingSink sink;
        QVERIFY(doConnect(sp, ctx, sink, /*retries=*/5, /*retryDelayMs=*/0,
                          /*livenessDelayMs=*/0).ok);
        QVERIFY2(stopped(sp, {900}), qPrintable(sp.calls.join('\n')));
        QVERIFY(hasCall(sp, "DETACH"));
        for (const QString &prog : {QStringLiteral("sh"), QStringLiteral("pgrep"),
                                    QStringLiteral("pkill"), QStringLiteral("kill")})
            QVERIFY2(!spawnsProgram(sp, prog),
                     qPrintable(QStringLiteral("spawned %1:\n%2").arg(prog, sp.calls.join('\n'))));
        QVERIFY(!hasCall(sp, "ss -tnHp"));
    }

    // The same machine through sleep's reap and through Stop's teardown.
    void sleepAndStopWithoutPosixUserlandUseTheStopSeam() {
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        const QString target =
            makeContext(withGuest(cfg), Backend::Remote, std::nullopt, QStringLiteral("A")).rc.target;
        {
            FakeSpawner sp;
            sp.os = LocalOs::Windows;
            sp.mirrorSnapshots = {{mirrorAt(901, target, QStringLiteral("A"))}};
            RecordingSink sink;
            QVERIFY(sleepRun(Backend::Remote, withGuest(cfg), sink, sp, std::nullopt,
                             QStringLiteral("A")));
            QVERIFY(stopped(sp, {901}));
            QVERIFY(!spawnsProgram(sp, QStringLiteral("pkill")));
        }
        {
            FakeSpawner sp;
            sp.os = LocalOs::Windows;
            sp.mirrorSnapshots = {{mirrorAt(902, target, QStringLiteral("A")),
                                   mirrorAt(903, target, QStringLiteral("Maps"), MirrorKind::App)}};
            QCOMPARE(reapSessionOrphans(sp, QStringLiteral("remora-a"), target, {}), 2);
            QVERIFY(stopped(sp, {902, 903}));
            QVERIFY(!hasCall(sp, "pgrep") && !spawnsProgram(sp, QStringLiteral("kill")));
        }
    }

    void pipToggleClosesOnlyItsOwnProfilesPip() {
        FakeSpawner sp;
        sp.os = LocalOs::Windows;
        const QString t = QStringLiteral("10.0.0.5:5555");
        // Profile "A 2"'s PIP is up; "A"'s is not. The old pattern was an unanchored regex, so
        // "Remora PIP — A" matched "Remora PIP — A 2" and toggling A closed A 2's window.
        sp.mirrorSnapshots = {
            {mirrorAt(41, t, pipWindowTitle(QStringLiteral("A 2")), MirrorKind::Pip),
             mirrorAt(42, t, QStringLiteral("A"))}};
        QCOMPARE(closePip(sp, pipWindowTitle(QStringLiteral("A"))), PipClose::NotRunning);
        QVERIFY(sp.stops.isEmpty());
        QCOMPARE(closePip(sp, pipWindowTitle(QStringLiteral("A 2"))), PipClose::Closed);
        // Asked to close and waited for, never escalated: a PIP is a healthy window.
        QCOMPARE(sp.stops.size(), 1);
        QCOMPARE(sp.stops[0].pids, QList<qint64>{41});
        QVERIFY(!sp.stops[0].force);
        QVERIFY(sp.stops[0].graceMs > 0);
        sp.unstoppable = {41};
        QCOMPARE(closePip(sp, pipWindowTitle(QStringLiteral("A 2"))), PipClose::StillRunning);
        QVERIFY(sp.calls.filter(QRegularExpression(QStringLiteral("^(sh|pgrep|pkill|kill)\\b")))
                    .isEmpty());
    }

    // ── the pure selection rules ──

    void mirrorKindIsReadFromTheLaunchFlags() {
        QCOMPARE(mirrorKindFor(false, false, QString(), QStringLiteral("A")), MirrorKind::Main);
        QCOMPARE(mirrorKindFor(false, true, QString(), QStringLiteral("A")), MirrorKind::Splash);
        QCOMPARE(mirrorKindFor(false, false, QString(kPipAppId), QStringLiteral("x")),
                 MirrorKind::Pip);
        QCOMPARE(mirrorKindFor(true, false, QStringLiteral("remora-a-com.maps"),
                               QStringLiteral("Maps")),
                 MirrorKind::App);
        QCOMPARE(mirrorKindFor(true, false, QString(), kDesktopTitlePrefix + QStringLiteral("A")),
                 MirrorKind::Desktop);
    }

    // The argv every launcher actually builds, read back the way the Linux upgrade fallback reads
    // /proc — so a mirror an older Remora started is classified exactly like a registered one.
    void mirrorEntryFromArgvReadsEveryLauncherShape() {
        RemoraConfig cfg;
        cfg.network.macvlanIp = QStringLiteral("192.0.2.77");
        RunContext ctx = makeContext(cfg, Backend::Bare, std::nullopt, QStringLiteral("Android 17"));
        // argv[0] is the running binary, which here is this suite rather than remora.
        ctx.mirrorArgv[0] = ctx.splashArgv[0] = QStringLiteral("/usr/bin/remora");
        const auto main = mirrorEntryFromArgv(ctx.mirrorArgv);
        QVERIFY(main);
        QCOMPARE(main->serial, ctx.rc.target);
        QCOMPARE(main->title, QStringLiteral("Android 17"));
        QCOMPARE(main->kind, MirrorKind::Main);
        QVERIFY(!main->registered);
        QCOMPARE(mirrorEntryFromArgv(ctx.splashArgv).value_or(MirrorEntry{}).kind,
                 MirrorKind::Splash);

        QStringList pip =
            buildPipMirrorArgv(ctx.rc, ctx.rc.target, QStringLiteral("Android 17")).second;
        pip[0] = QStringLiteral("/usr/bin/remora");
        const auto p = mirrorEntryFromArgv(pip);
        QVERIFY(p);
        QCOMPARE(p->kind, MirrorKind::Pip);
        QCOMPARE(p->title, pipWindowTitle(QStringLiteral("Android 17")));  // the `=` spelling

        RemoraConfig dcfg = cfg;
        dcfg.mirror.fullscreenSeparateDisplay = true;
        RunContext desk = makeContext(dcfg, Backend::Bare, std::nullopt,
                                      kDesktopTitlePrefix + QStringLiteral("Android 17"), true);
        desk.mirrorArgv[0] = QStringLiteral("remora");
        QCOMPARE(mirrorEntryFromArgv(desk.mirrorArgv).value_or(MirrorEntry{}).kind,
                 MirrorKind::Desktop);
        dcfg.mirror.fullscreenApp = QStringLiteral("com.maps");
        RunContext app = makeContext(dcfg, Backend::Bare, std::nullopt, QStringLiteral("Maps"), true);
        app.mirrorArgv[0] = QStringLiteral("remora");
        QCOMPARE(mirrorEntryFromArgv(app.mirrorArgv).value_or(MirrorEntry{}).kind, MirrorKind::App);

        // Not clients: the app-menu's one-shot listing, a wrapper shell that merely mentions a
        // mirror, another program, a mirror without a serial.
        QVERIFY(!mirrorEntryFromArgv({"remora", "mirror", "-s", "x:1", "--list-apps"}));
        QVERIFY(!mirrorEntryFromArgv({"/bin/sh", "-c", "remora mirror -s x:1"}));
        QVERIFY(!mirrorEntryFromArgv({"/usr/bin/vlc", "mirror", "-s", "x:1"}));
        QVERIFY(!mirrorEntryFromArgv({"remora", "mirror", "--window-title", "A"}));
        QVERIFY(!mirrorEntryFromArgv({"remora", "gui"}));
        // Windows' own spelling of argv[0], and QCommandLineParser's other value form.
        const auto win = mirrorEntryFromArgv(
            {"C:\\Program Files\\Remora\\remora.exe", "mirror", "--serial=x:1"});
        QVERIFY(win);
        QCOMPARE(win->serial, QStringLiteral("x:1"));
        QCOMPARE(win->title, QStringLiteral("Remora mirror — x:1"));  // MirrorApp's own default
    }

    void instanceTitleMatchesTheProfileExactly() {
        QVERIFY(isInstanceMirrorTitle(QStringLiteral("A 17"), QStringLiteral("A 17")));
        QVERIFY(isInstanceMirrorTitle(QStringLiteral("Remora:A 17"), QStringLiteral("A 17")));
        QVERIFY(isInstanceMirrorTitle(QStringLiteral("A 17"), QStringLiteral("Remora:A 17")));
        QVERIFY(!isInstanceMirrorTitle(QStringLiteral("A 17 beta"), QStringLiteral("A 17")));
        QVERIFY(!isInstanceMirrorTitle(kDesktopTitlePrefix + QStringLiteral("A 17"),
                                       QStringLiteral("A 17")));
        // C++ and friends are names, not patterns.
        QVERIFY(isInstanceMirrorTitle(QStringLiteral("C++ (x)"), QStringLiteral("C++ (x)")));
        // An untitled session takes only the legacy prefix — "any title" would take app and
        // desktop windows with it.
        QVERIFY(isInstanceMirrorTitle(QStringLiteral("Remora:old"), QString()));
        QVERIFY(!isInstanceMirrorTitle(QStringLiteral("old"), QString()));
    }

    void instanceMirrorsAreTheMainWindowAndItsSplashOnly() {
        const QString t = QStringLiteral("10.0.0.5:5555");
        const QVector<MirrorEntry> all{
            mirrorAt(1, t, QStringLiteral("A")),
            mirrorAt(2, t, QStringLiteral("A"), MirrorKind::Splash, MirrorSessionState::Pending),
            mirrorAt(3, t, QStringLiteral("A"), MirrorKind::App),  // an app titled like the profile
            mirrorAt(4, t, pipWindowTitle(QStringLiteral("A")), MirrorKind::Pip),
            mirrorAt(5, t, kDesktopTitlePrefix + QStringLiteral("A"), MirrorKind::Desktop),
            mirrorAt(6, QStringLiteral("10.0.0.5:55550"), QStringLiteral("A")),
            mirrorAt(7, t, QStringLiteral("B"))};
        QCOMPARE(mirrorPids(instanceMirrors(all, t, QStringLiteral("A"))), (QList<qint64>{1, 2}));
        QVERIFY(instanceMirrors(all, QString(), QStringLiteral("A")).isEmpty());
        MirrorQuery q;
        q.target = t;
        QCOMPARE(selectMirrors(all, q).size(), 6);
        q = {};
        q.exactTitle = pipWindowTitle(QStringLiteral("A"));
        QCOMPARE(mirrorPids(selectMirrors(all, q)), QList<qint64>{4});
    }

    void mirrorEntryRoundTripsAndRefusesWhatIsNotOne() {
        MirrorEntry e = mirrorAt(77, QStringLiteral("10.0.0.5:5555"), QStringLiteral("Remora PIP — A"),
                                 MirrorKind::Pip, MirrorSessionState::Lost);
        const auto back = decodeMirrorEntry(encodeMirrorEntry(e));
        QVERIFY(back);
        QCOMPARE(back->pid, e.pid);
        QCOMPARE(back->serial, e.serial);
        QCOMPARE(back->title, e.title);
        QCOMPARE(back->kind, e.kind);
        QCOMPARE(back->startedMs, e.startedMs);
        QCOMPARE(back->session, e.session);
        QVERIFY(back->registered);
        QVERIFY(!decodeMirrorEntry("not json"));
        QVERIFY(!decodeMirrorEntry(R"({"pid":0,"serial":"x:1"})"));
        QVERIFY(!decodeMirrorEntry(R"({"pid":5})"));
        // A kind from a newer client is still a mirror on its target.
        QCOMPARE(decodeMirrorEntry(R"({"pid":5,"serial":"x:1","kind":"hologram"})")->kind,
                 MirrorKind::Main);
    }

    // ── the registry on disk ──

    void registryShowsALiveRegistrationAndForgetsItOnExit() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MirrorEntry self = mirrorAt(QCoreApplication::applicationPid(), QStringLiteral("x:1"),
                                    QStringLiteral("A"), MirrorKind::Main,
                                    MirrorSessionState::Pending);
        {
            MirrorRegistration reg(self, dir.path());
            QVERIFY2(reg.ok(), qPrintable(reg.error()));
            QVector<MirrorEntry> live = readMirrorRegistry(dir.path());
            QCOMPARE(live.size(), 1);
            QCOMPARE(live[0].pid, self.pid);
            QCOMPARE(live[0].serial, self.serial);
            QCOMPARE(live[0].session, MirrorSessionState::Pending);
            reg.setSession(MirrorSessionState::Live);
            QCOMPARE(readMirrorRegistry(dir.path()).value(0).session, MirrorSessionState::Live);
            reg.setSession(MirrorSessionState::Lost);
            QCOMPARE(readMirrorRegistry(dir.path()).value(0).session, MirrorSessionState::Lost);
        }
        QVERIFY(readMirrorRegistry(dir.path()).isEmpty());
        QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    // What a SIGKILLed or crashed mirror leaves: its entry and its lock file, with nobody holding
    // the lock. The pid in it is THIS process's — alive, and a Remora — which is precisely the
    // reused-pid case a pid-based staleness test (QLockFile's) gets wrong; the lock decides.
    void registryReapsTheEntryACrashedMirrorLeftBehind() {
        QTemporaryDir dir;
        const qint64 pid = QCoreApplication::applicationPid();
        const QString id = QStringLiteral("%1-00000000deadbeef").arg(pid);
        QFile json(dir.filePath(id + QStringLiteral(".json")));
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write(encodeMirrorEntry(mirrorAt(pid, QStringLiteral("x:1"), QStringLiteral("A"))));
        json.close();
        QFile lock(dir.filePath(id + QStringLiteral(".lock")));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();
        // …and one whose lock file is gone altogether.
        const QString id2 = QStringLiteral("%1-00000000feedface").arg(pid);
        QFile json2(dir.filePath(id2 + QStringLiteral(".json")));
        QVERIFY(json2.open(QIODevice::WriteOnly));
        json2.write(encodeMirrorEntry(mirrorAt(pid, QStringLiteral("x:1"), QStringLiteral("B"))));
        json2.close();
        // …and a lock left with no entry at all.
        QFile orphan(dir.filePath(QStringLiteral("99-0000000000000001.lock")));
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.close();

        MirrorRegistration live(mirrorAt(pid, QStringLiteral("x:1"), QStringLiteral("C")), dir.path());
        QVERIFY(live.ok());
        const QVector<MirrorEntry> found = readMirrorRegistry(dir.path());
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].title, QStringLiteral("C"));
        // The dead ones' files are gone; the live one's are untouched.
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2);
    }

    // A held lock vouches for its own entry only: an entry whose pid disagrees with the id its
    // owner wrote is damaged, and a damaged entry's pid is not one to hand to a stop call.
    void registrySkipsAnEntryWhosePidDisagreesWithItsId() {
        QTemporaryDir dir;
        MirrorRegistration reg(mirrorAt(QCoreApplication::applicationPid(), QStringLiteral("x:1"),
                                        QStringLiteral("A")),
                               dir.path());
        QVERIFY(reg.ok());
        const QStringList jsons = QDir(dir.path()).entryList({QStringLiteral("*.json")});
        QCOMPARE(jsons.size(), 1);
        QFile f(dir.filePath(jsons[0]));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(encodeMirrorEntry(mirrorAt(1, QStringLiteral("x:1"), QStringLiteral("A"))));
        f.close();
        QVERIFY(readMirrorRegistry(dir.path()).isEmpty());
        QVERIFY(QFile::exists(dir.filePath(jsons[0])));  // not removed: its owner is alive
    }

    void registryOfANonexistentDirectoryIsEmpty() {
        QTemporaryDir dir;
        QVERIFY(readMirrorRegistry(dir.filePath(QStringLiteral("never-made"))).isEmpty());
    }

#ifdef Q_OS_LINUX
    // The upgrade fallback reads /proc: a mirror an older Remora started is found by its exact
    // argv — no pgrep, no joined-string ambiguity over a title with spaces in it, no wrapper shell
    // mistaken for the mirror it mentions. Fed a proc tree of its own so the suite never sees the
    // machine's real mirrors.
    void procFallbackFindsMirrorsThatNeverRegistered() {
        QTemporaryDir proc;
        const auto task = [&](const QString &pid, const QStringList &argv) {
            QDir().mkpath(proc.filePath(pid));
            QFile f(proc.filePath(pid + QStringLiteral("/cmdline")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            for (const QString &a : argv) f.write(a.toUtf8() + '\0');
        };
        task(QStringLiteral("4100"), {"/opt/old/remora", "mirror", "-s", "10.9.8.7:5555",
                                      "--window-title=Remora PIP — legacy one",
                                      "--app-id=remora-pip"});
        task(QStringLiteral("4101"), {"./build/remora", "mirror", "-s", "10.9.8.7:5555",
                                      "--window-title", "legacy one"});
        task(QStringLiteral("4102"), {"/bin/sh", "-c", "remora mirror -s 10.9.8.7:5555"});
        task(QStringLiteral("4103"), {"remora", "mirror", "-s", "10.9.8.7:5555", "--list-apps"});
        task(QString::number(QCoreApplication::applicationPid()),
             {"/usr/bin/remora", "mirror", "-s", "10.9.8.7:5555"});  // never itself
        task(QStringLiteral("self"), {"/usr/bin/remora", "mirror", "-s", "10.9.8.7:5555"});
        QDir().mkpath(proc.filePath(QStringLiteral("4104")));  // a kernel thread: no cmdline
        QVector<MirrorEntry> found = scanMirrorProcesses(proc.path());
        std::sort(found.begin(), found.end(),
                  [](const MirrorEntry &a, const MirrorEntry &b) { return a.pid < b.pid; });
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].pid, qint64(4100));
        QCOMPARE(found[0].kind, MirrorKind::Pip);
        QCOMPARE(found[0].title, QStringLiteral("Remora PIP — legacy one"));
        QCOMPARE(found[1].pid, qint64(4101));
        QCOMPARE(found[1].kind, MirrorKind::Main);
        QCOMPARE(found[1].title, QStringLiteral("legacy one"));
        QVERIFY(!found[0].registered && !found[1].registered);
        // …and it is an instance mirror like any registered one, so every reap still finds it.
        QCOMPARE(mirrorPids(instanceMirrors(found, QStringLiteral("10.9.8.7:5555"),
                                            QStringLiteral("legacy one"))),
                 QList<qint64>{4101});
    }
#endif

    // bd remora-nx24: the update watcher restarts itself from the path it was launched as when an
    // upgrade has replaced the file there.
    void selfImageLaunchPathIsArgv0AsLaunched() {
        QCOMPARE(launchPathFromArgv0(QStringLiteral("/opt/r/bin/remora"), QStringLiteral("/x")),
                 QStringLiteral("/opt/r/bin/remora"));
        QCOMPARE(launchPathFromArgv0(QStringLiteral("bin/remora"), QStringLiteral("/opt/r")),
                 QStringLiteral("/opt/r/bin/remora"));
        QCOMPARE(launchPathFromArgv0(QStringLiteral("./remora"), QStringLiteral("/opt/r/")),
                 QStringLiteral("/opt/r/./remora"));
        // A bare name was found on PATH; which file that was cannot be known now.
        QVERIFY(launchPathFromArgv0(QStringLiteral("remora"), QStringLiteral("/opt/r")).isEmpty());
        QVERIFY(launchPathFromArgv0(QString(), QStringLiteral("/opt/r")).isEmpty());
        QVERIFY(launchPathFromArgv0(QStringLiteral("bin/remora"), QString()).isEmpty());
#ifdef Q_OS_LINUX
        // A final symlink is kept as the name the unit runs, never resolved to today's target.
        QTemporaryDir d;
        QVERIFY(writeSelfFile(d.filePath("remora-1"), "one", true));
        QVERIFY(QFile::link(d.filePath("remora-1"), d.filePath("remora")));
        QCOMPARE(launchPathFromArgv0(d.filePath("remora"), QStringLiteral("/")),
                 d.filePath("remora"));
#endif
    }

#ifdef Q_OS_LINUX
    // The blocking case: the running binary was renamed to a backup BEFORE the watcher started,
    // and the replacement is already at the original path. The executing-image link names the
    // backup; the first check must pick the original launch path, with the original arguments.
    void selfImageRenamedToBackupBeforeStartRestartsFromLaunchPath() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), bak = d.filePath("remora.bak"),
                      link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(inst, "old image", true));
        QVERIFY(QFile::rename(inst, bak));
        QVERIFY(pointSelfLink(link, bak));  // what /proc/self/exe now names
        QVERIFY(writeSelfFile(inst, "new image, longer", true));
        const QString launch = launchPathFromArgv0(inst, QStringLiteral("/"));
        const QStringList argv{inst, QStringLiteral("update-watch"), QStringLiteral("My phone")};
        SelfExecLog log;
        SelfImageWatch w = SelfImageWatch::seeded(launch, argv, recordingSelfOps(log), link);
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Replaced);
        QCOMPARE(log.calls.size(), 1);
        QCOMPARE(log.calls[0].first, inst);
        QCOMPARE(log.calls[0].second, argv);

        // The rejected selection — the installation path read back from the link — names the
        // backup, whose identity IS the running image: it never sees the replacement.
        QCOMPARE(QFileInfo(link).symLinkTarget(), bak);
        SelfExecLog rejected;
        SelfImageWatch r = SelfImageWatch::seeded(QFileInfo(link).symLinkTarget(), argv,
                                                  recordingSelfOps(rejected), link);
        QCOMPARE(r.check(), SelfImageWatch::Outcome::Unchanged);
        QCOMPARE(r.check(), SelfImageWatch::Outcome::Unchanged);
        QVERIFY(rejected.calls.isEmpty());
    }

    // The ordinary upgrade: rename and install happen while the watcher is already running.
    void selfImageRenamedWhileRunningRestartsAtNextCheck() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), bak = d.filePath("remora.bak"),
                      link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(inst, "old image", true));
        QVERIFY(pointSelfLink(link, inst));
        const QStringList argv{inst, QStringLiteral("update-watch")};
        SelfExecLog log;
        SelfImageWatch w = SelfImageWatch::seeded(inst, argv, recordingSelfOps(log), link);
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Unchanged);
        QVERIFY(QFile::rename(inst, bak));
        QVERIFY(pointSelfLink(link, bak));  // the kernel's link follows the renamed inode
        // Mid-install: nothing at the launch path yet. Wait, do not exec the backup.
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Missing);
        QVERIFY(writeSelfFile(inst, "new image", true));
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Replaced);
        QCOMPARE(log.calls.size(), 1);
        QCOMPARE(log.calls[0].first, inst);
        QCOMPARE(log.calls[0].second, argv);
    }

    // The running image unlinked outright (an install by unlink + create). A real magic link to
    // an open descriptor of the deleted file stands in for /proc/self/exe.
    void selfImageUnlinkedRunningImageIsStillTheBaseline() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora");
        QVERIFY(writeSelfFile(inst, "old image", true));
        QFile held(inst);
        QVERIFY(held.open(QIODevice::ReadOnly));
        QVERIFY(QFile::remove(inst));
        QVERIFY(writeSelfFile(inst, "new image", true));
        const QString link = QStringLiteral("/proc/self/fd/%1").arg(held.handle());
        SelfExecLog log;
        SelfImageWatch w = SelfImageWatch::seeded(inst, {inst, QStringLiteral("update-watch")},
                                                  recordingSelfOps(log), link);
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Replaced);
        QCOMPARE(log.calls.size(), 1);
        QCOMPARE(log.calls[0].first, inst);
    }

    // The successor: its own link names the file at the launch path, so it never restarts again.
    void selfImageSuccessorSeesItselfUnchanged() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(inst, "new image", true));
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog log;
        SelfImageWatch w = SelfImageWatch::seeded(inst, {inst, QStringLiteral("update-watch")},
                                                  recordingSelfOps(log), link);
        for (int i = 0; i < 3; ++i) QCOMPARE(w.check(), SelfImageWatch::Outcome::Unchanged);
        QVERIFY(log.calls.isEmpty());
    }

    // A replacement that cannot run yet is waited for; once its mode is repaired the SAME file
    // is started at the next check — no delay, no extra polling.
    void selfImageNotExecutableWaitsThenRunsAfterRepair() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), bak = d.filePath("remora.bak"),
                      link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(bak, "old image", true));
        QVERIFY(pointSelfLink(link, bak));
        QVERIFY(writeSelfFile(inst, "new image", false));
        SelfExecLog log;
        SelfImageWatch w = SelfImageWatch::seeded(inst, {inst, QStringLiteral("update-watch")},
                                                  recordingSelfOps(log), link);
        QCOMPARE(w.check(), SelfImageWatch::Outcome::NotExecutable);
        QCOMPARE(w.check(), SelfImageWatch::Outcome::NotExecutable);
        QVERIFY(log.calls.isEmpty());
        QVERIFY(QFile(inst).setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                           QFile::ExeOwner));
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Replaced);
        QCOMPARE(log.calls.size(), 1);
        QCOMPARE(log.calls[0].first, inst);
    }

    // exec refused (EACCES, e.g. a noexec mount or a mode the kernel rejects): the running image
    // stays the baseline — the replacement's identity is never adopted — so after a permission-
    // only repair the same, unchanged file is attempted again at the next check and succeeds.
    void selfImageFailedExecKeepsRunningBaselineAndRetries() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), bak = d.filePath("remora.bak"),
                      link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(bak, "old image", true));
        QVERIFY(pointSelfLink(link, bak));
        QVERIFY(writeSelfFile(inst, "new image", true));
        const QStringList argv{inst, QStringLiteral("update-watch"), QStringLiteral("p")};
        SelfExecLog log;
        const SelfImageOps ops = recordingSelfOps(log);
        const std::optional<SelfFileId> before = ops.stat(inst);
        SelfImageWatch w = SelfImageWatch::seeded(inst, argv, ops, link);
        log.nextErr = EACCES;
        QCOMPARE(w.check(), SelfImageWatch::Outcome::ExecFailed);
        QCOMPARE(w.lastErrno(), EACCES);
        log.nextErr = 0;  // the repair
        QCOMPARE(w.check(), SelfImageWatch::Outcome::Replaced);
        QCOMPARE(log.calls.size(), 2);
        QCOMPARE(log.calls[1].first, inst);
        QCOMPARE(log.calls[1].second, argv);
        QVERIFY(before && ops.stat(inst) && *before == *ops.stat(inst));  // the same file
    }

    // Nothing reliable to restart from: never guess a path, never exec.
    void selfImageWithoutLaunchPathOrRunningImageIsDisabled() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora");
        QVERIFY(writeSelfFile(inst, "new image", true));
        SelfExecLog log;
        SelfImageWatch bare = SelfImageWatch::seeded(
            launchPathFromArgv0(QStringLiteral("remora"), d.path()),
            {QStringLiteral("remora"), QStringLiteral("update-watch")}, recordingSelfOps(log),
            d.filePath("self-exe"));
        QCOMPARE(bare.check(), SelfImageWatch::Outcome::Disabled);
        // The running image cannot be identified: no baseline is invented from the launch path.
        SelfImageWatch blind = SelfImageWatch::seeded(
            inst, {inst, QStringLiteral("update-watch")}, recordingSelfOps(log),
            d.filePath("no-such-link"));
        QCOMPARE(blind.check(), SelfImageWatch::Outcome::Disabled);
        QVERIFY(log.calls.isEmpty());
    }

    // bd remora-nx24, the review's case: the old watcher holds a signed-catalog offer, the device
    // taps Apply, and an upgrade replaces the watcher before that Apply is read. The successor
    // starts with no offer and its first catalog check fails: the Apply must stay waiting —
    // neither dropped nor served by redeploying the image the device already runs — and once the
    // catalog answers, the requested release is downloaded and the device moved onto it.
    void updateWatchSuccessorHoldsApplyUntilCatalogAnswers() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), bak = d.filePath("remora.bak"),
                      link = d.filePath("self-exe");
        const QStringList argv{inst, QStringLiteral("update-watch"), QStringLiteral("p")};
        QVERIFY(writeSelfFile(inst, "old image", true));
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog oldLog;
        SelfImageWatch old = SelfImageWatch::seeded(inst, argv, recordingSelfOps(oldLog), link);
        FakeUpdateHost host;
        host.checks = {catalogAnswer(true)};
        UpdateWatchState st;
        QCOMPARE(runUpdateWatchPass(old, st, host.ops()), UpdateWatchPass::Idle);
        QCOMPARE(host.take(), (QStringList{"check", "probe"}));
        QCOMPARE(host.probedOffer.value("version").toString(), kOfferVersion);

        host.applyWaiting = true;  // the device taps Apply
        QVERIFY(QFile::rename(inst, bak));  // and an upgrade lands before the next pass
        QVERIFY(pointSelfLink(link, bak));
        QVERIFY(writeSelfFile(inst, "new image", true));
        QCOMPARE(runUpdateWatchPass(old, st, host.ops()), UpdateWatchPass::Restarted);
        QVERIFY(host.take().isEmpty());  // nothing read, nothing deployed, signal untouched
        QVERIFY(host.applyWaiting);
        QCOMPARE(oldLog.calls.size(), 1);
        QCOMPARE(oldLog.calls[0].first, inst);
        QCOMPARE(oldLog.calls[0].second, argv);

        // The successor: a fresh process, so a fresh state; its own image is the one installed.
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog succLog;
        SelfImageWatch succ = SelfImageWatch::seeded(inst, argv, recordingSelfOps(succLog), link);
        UpdateWatchState fresh;
        host.checks = {catalogDown()};
        for (int pass = 0; pass < 3; ++pass) {
            QCOMPARE(runUpdateWatchPass(succ, fresh, host.ops()), UpdateWatchPass::Held);
            QCOMPARE(host.take(), (QStringList{"check", "probe"}));  // asked again every pass
            QVERIFY(host.applyWaiting);
            QVERIFY(fresh.lastError.isEmpty());
            host.clock = host.clock.addSecs(15);
        }
        QCOMPARE(host.said.filter(QStringLiteral("the request is kept")).size(), 1);

        host.checks = {catalogAnswer(true)};
        QCOMPARE(runUpdateWatchPass(succ, fresh, host.ops()), UpdateWatchPass::Applied);
        QCOMPARE(host.take(), (QStringList{"check", "probe", "fetch", "deploy", "clear"}));
        QVERIFY(!host.applyWaiting);
        QVERIFY(fresh.offer.isEmpty());
        QVERIFY(!fresh.nextCheck.isValid());  // the catalog is asked again on the next pass
        QVERIFY(fresh.lastError.isEmpty());
        QVERIFY(succLog.calls.isEmpty());  // the successor never restarts itself
    }

    // The control: a watcher that has an answer keeps it through a failed check (the last answer
    // stands), so an Apply is handled as it always was: the offered release fetched, then the
    // deploy. A catalog that said "nothing newer" leaves a plain redeploy onto the local tag.
    void updateWatchKeepsItsAnswerAcrossAFailedCheck() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(inst, "image", true));
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog log;
        SelfImageWatch self = SelfImageWatch::seeded(inst, {inst, QStringLiteral("update-watch")},
                                                     recordingSelfOps(log), link);
        FakeUpdateHost host;
        host.checks = {catalogAnswer(true)};
        UpdateWatchState st;
        QCOMPARE(runUpdateWatchPass(self, st, host.ops()), UpdateWatchPass::Idle);
        host.take();
        host.clock = host.clock.addSecs(kReleaseCheckSecs);
        host.checks = {catalogDown()};
        host.applyWaiting = true;
        QCOMPARE(runUpdateWatchPass(self, st, host.ops()), UpdateWatchPass::Applied);
        QCOMPARE(host.take(), (QStringList{"check", "probe", "fetch", "deploy", "clear"}));

        FakeUpdateHost latest;
        latest.checks = {catalogAnswer(false)};
        latest.applyWaiting = true;
        UpdateWatchState none;
        QCOMPARE(runUpdateWatchPass(self, none, latest.ops()), UpdateWatchPass::Applied);
        QCOMPARE(latest.take(), (QStringList{"check", "probe", "deploy", "clear"}));
        QVERIFY(log.calls.isEmpty());
    }

    // A profile on its own tag never asks the catalog and never waits for it; a failed download
    // of an offered release says so on the device's screen and does not deploy.
    void updateWatchPrivateTagAndFailedDownload() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), link = d.filePath("self-exe");
        QVERIFY(writeSelfFile(inst, "image", true));
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog log;
        SelfImageWatch self = SelfImageWatch::seeded(inst, {inst, QStringLiteral("update-watch")},
                                                     recordingSelfOps(log), link);
        FakeUpdateHost own;
        own.profile.imageTag = QStringLiteral("remora24:x86_64-mine");
        own.checks = {catalogDown()};
        own.applyWaiting = true;
        UpdateWatchState st;
        QCOMPARE(runUpdateWatchPass(self, st, own.ops()), UpdateWatchPass::Applied);
        QCOMPARE(own.take(), (QStringList{"probe", "deploy", "clear"}));

        FakeUpdateHost cut;
        cut.checks = {catalogAnswer(true)};
        cut.fetchRc = 1;
        cut.applyWaiting = true;
        UpdateWatchState st2;
        QCOMPARE(runUpdateWatchPass(self, st2, cut.ops()), UpdateWatchPass::Failed);
        QCOMPARE(cut.take(), (QStringList{"check", "probe", "fetch", "clear"}));
        QVERIFY(st2.lastError.contains(kOfferVersion));
        QVERIFY(st2.lastError.contains(QStringLiteral("disk full")));
        QCOMPARE(runUpdateWatchPass(self, st2, cut.ops()), UpdateWatchPass::Idle);
        QVERIFY(cut.probedError.contains(kOfferVersion));  // what the device's screen is given
    }

    // The self-restart runs only at the top of a pass, through the production pass: an upgrade
    // that lands during an Apply is taken after that Apply has finished and removed its signal; a
    // replacement found with an Apply waiting execs before the signal is read and leaves it for
    // the successor; a failed exec runs the ordinary pass, Apply included, and is said once.
    void updateWatchSelfRestartOnlyBetweenPasses() {
        QTemporaryDir d;
        const QString inst = d.filePath("remora"), bak = d.filePath("remora.bak"),
                      link = d.filePath("self-exe");
        const QStringList argv{inst, QStringLiteral("update-watch")};
        QVERIFY(writeSelfFile(inst, "old image", true));
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog log;
        SelfImageWatch w = SelfImageWatch::seeded(inst, argv, recordingSelfOps(log), link);
        FakeUpdateHost host;
        host.profile.imageTag = QStringLiteral("remora24:x86_64-mine");
        host.applyWaiting = true;
        host.duringDeploy = [&]() {
            QFile::rename(inst, bak);
            pointSelfLink(link, bak);
            writeSelfFile(inst, "new image", true);
        };
        UpdateWatchState st;
        QCOMPARE(runUpdateWatchPass(w, st, host.ops()), UpdateWatchPass::Applied);
        QCOMPARE(host.take(), (QStringList{"probe", "deploy", "clear"}));
        QVERIFY(log.calls.isEmpty());
        QCOMPARE(runUpdateWatchPass(w, st, host.ops()), UpdateWatchPass::Restarted);
        QVERIFY(host.take().isEmpty());
        QCOMPARE(log.calls.size(), 1);

        // A new Apply waiting when the replacement is found: exec first, signal untouched.
        host.duringDeploy = nullptr;
        QVERIFY(pointSelfLink(link, inst));
        SelfExecLog log2;
        w = SelfImageWatch::seeded(inst, argv, recordingSelfOps(log2), link);
        QVERIFY(QFile::remove(inst));
        QVERIFY(writeSelfFile(inst, "newer image, again", true));
        host.applyWaiting = true;
        QCOMPARE(runUpdateWatchPass(w, st, host.ops()), UpdateWatchPass::Restarted);
        QVERIFY(host.take().isEmpty());
        QVERIFY(host.applyWaiting);

        // exec refused: this process keeps serving, the Apply is handled, the failure said once.
        log2.nextErr = EACCES;
        QCOMPARE(runUpdateWatchPass(w, st, host.ops()), UpdateWatchPass::Applied);
        QCOMPARE(host.take(), (QStringList{"probe", "deploy", "clear"}));
        QCOMPARE(runUpdateWatchPass(w, st, host.ops()), UpdateWatchPass::Idle);
        QCOMPARE(host.said.filter(QStringLiteral("could not start the replaced")).size(), 1);
        QCOMPARE(log2.calls.size(), 3);  // tried again at every pass
        log2.nextErr = 0;  // the repair
        QCOMPARE(runUpdateWatchPass(w, st, host.ops()), UpdateWatchPass::Restarted);
    }
#endif
};

QTEST_MAIN(TestEngine)
#include "test_engine.moc"
