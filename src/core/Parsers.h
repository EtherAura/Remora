#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <optional>

namespace remora {

// Pure str→value parsers — the entire load-bearing surface of the status reporter. Tested against
// real fixtures captured on the dev host. Only the engine's probe shells out; these hold no I/O.

std::optional<bool> parseBootCompleted(const QString &getpropOut);
std::optional<QString> parseGralloc(const QString &getpropOut);
std::optional<QString> parseSurfaceflingerGles(const QString &dumpsysOut);
// One physical keyboard as /proc/bus/input/devices describes it. This is what the guest's uinput
// device impersonates: Gboard refuses physical-keyboard mode for a keyboard advertising generic
// identifiers, so the passthrough has to claim a real one (bd remora-a41).
struct KbdIdentity {
    QString name, phys, sysfs, node;  // node = "eventN"
    bool alphabetic = false;   // KEY bitmap carries the letter keys => EventHub sees ALPHAKEY
    bool hasRel = false, hasAbs = false;  // pointer / analog axes — a mouse or a controller
    unsigned bus = 0, vendor = 0, product = 0, version = 0;
};

// Every ALPHABETIC keyboard in a /proc/bus/input/devices dump, best-first is not implied — the
// caller chooses. Selection is by CAPABILITY, not by name: a device qualifies only if its KEY
// bitmap carries the letter keys, which is what makes EventHub classify it ALPHAKEY. Power
// buttons, consumer/system-control endpoints and PC speakers all set "kbd" in Handlers and would
// pass a name-based or handler-based filter.
QList<KbdIdentity> parseInputDevicesKeyboards(const QString &procInput);

// Every input device in the dump, keyboards or not — mice, controllers, tablets. Used to share a
// host device with the guest, which is not restricted to keyboards (bd remora-4ei.36).
QList<KbdIdentity> parseInputDevices(const QString &procInput);

// Play Protect certification for one instance, as the engine's certification scan reports it: line
// 0 the GSF android_id, line 1 "1"/"0" for certified, line 2 the checkin security token, line 3 the
// identity file's mtime (epoch s, empty when unreadable), line 4 the device's own `date +%s`. Any
// of them stays nullopt when the scan could not answer — a container that is down, or a device that
// has never checked in, is UNKNOWN rather than uncertified, and the difference matters:
// "uncertified" tells the user to go register an id, which is wrong advice for a device that does
// not have one yet (bd remora-4ei.83).
struct PlayCertification {
    // The DECIMAL GSF android_id — the number google.com/android/uncertified registers. NB this is
    // NOT `settings get secure android_id`, which returns a per-app SSAID and registers nothing;
    // pasting that into Google's form silently certifies no device at all.
    std::optional<QString> androidId;
    std::optional<bool> certified;
    // The other half of the checkin identity. GSF authenticates a checkin with the PAIR, so an
    // android_id on its own cannot be restored onto a device — that is the "half-restored identity"
    // this was originally feared to produce (bd remora-4ei.84). Present only when the authoritative
    // file could be read; the Checkin.xml fallback carries the id alone.
    std::optional<QString> securityToken;
    // Both halves, in the exact on-device format — "<android_id>:<securityToken>". Empty unless
    // both were read. This is what gets snapshotted and written back, byte for byte.
    QString identityPair() const {
        if (!androidId || !securityToken) return QString();
        return *androidId + QLatin1Char(':') + *securityToken;
    }
    // True when the verdict-by-absence was WITHHELD: no uncertified notice is posted, but the
    // identity is younger than one verdict cycle, and absence then has two causes — GMS passed the
    // device, or GMS has not looked yet. Observed live: a brand-new never-registered identity read
    // "certified" for a continuously-polled five minutes (bd remora-4ei.90). `certified` stays
    // nullopt in this state; identityAgeSec says how young, for the message.
    bool pendingEvaluation = false;
    std::optional<qint64> identityAgeSec;
};
PlayCertification parsePlayCertification(const QString &scanOut);

// The adb FRONT DOOR, as the in-container probe reports it (bd remora-yn4): whether anything
// LISTENs on fwd's :5556 and on adbd's own :5555, plus init's view of the remora_fwd service.
// Listening is read from /proc/net/tcp*, not from pidof: the port is what every adb consumer
// actually feels, and a fwd that is alive but not yet bound must not read as up. nullopt =
// the probe never ran or said nothing (container down) — UNKNOWN, never "down", the same rule
// certification follows.
struct ForwarderProbe {
    std::optional<bool> fwdListening, adbdListening;
    // init.svc.remora_fwd verbatim ("running", "stopped", …). Empty string = the property is
    // unset: a container created before the remora-fwd.rc bind existed, where the netfix shell
    // supervisor owns fwd instead of init — worth distinguishing when reading a disagreement.
    std::optional<QString> service;
};
ForwarderProbe parseForwarderProbe(const QString &probeOut);

// One verdict cycle: the uncertified notice refreshes on GMS's ~11h checkin schedule (or a
// container restart, which this horizon deliberately does not model — a restart inside the window
// just reads pending a while longer, which errs in the safe direction). An identity OLDER than
// this that has drawn no notice has been through at least one evaluation, so absence means passed.
constexpr qint64 kPlayVerdictCycleSec = 12 * 3600;

// The sentence both certify surfaces (CLI and GUI) print for a pending verdict, composed once so
// they can never disagree about what pending means or what to do about it.
QString pendingEvaluationNote(qint64 ageSec);

// How an instance's data dir is laid out on disk, which is not the same question as whether it
// exists. /data is delivered one of two ways (buildDockerRunArgv): a plain bind of the data dir at
// /data, or the overlayfs pair, where the container's init mounts lowerdir=/data-base upperdir=<dir>/upper
// workdir=<dir>/work. The two leave DIFFERENT shapes behind, and each reads the other's shape as a
// perfectly good empty start (bd remora-4ei.89).
enum class DataDirShape {
    Missing,  // not there yet — a first deploy, fine either way
    Empty,    // there but empty — also fine either way
    Layered,  // holds upper/ — written by an overlayfs deploy
    Flat,     // holds a /data tree (adb, app, apex, data…) — written by a plain-bind deploy
};
DataDirShape parseDataDirShape(const QString &probeOut);

// Empty when the delivery about to be used matches what is already on disk; otherwise the refusal,
// naming both the mismatch and the two ways out.
//
// THIS IS A REFUSAL AND NOT A WARNING BECAUSE THE FAILURE IS INVISIBLE AND TOTAL. A flat /data tree
// is an acceptable overlay DIFF dir — it simply contributes nothing, because the overlay reads
// upper/, which does not exist and is created empty. With an empty lower layer, /data then comes up
// BLANK: docker succeeds, the container boots, Android initialises a fresh /data, app uids are reassigned,
// GSF mints a new checkin identity and the instance silently de-certifies, while the previous /data
// sits untouched beside upper/ as an orphan. That is what happened and it cost two
// debugging sessions to find, because every layer of the stack reported success.
//
// The mirror case refuses too: mounting a LAYERED dir as a plain /data hands Android a /data whose
// entire contents are upper/ and work/, which is the same blank start by the other route.
QString dataShapeRefusal(bool useOverlayfs, DataDirShape shape, const QString &dataDir,
                         const QString &dataBaseDir);

// The two facts the arch-variant guard compares (bd remora-bm7.13): the CPU tuning an image was
// COMPILED with, and the flags of the host that is about to execute it. Both come back from one
// probe on whichever machine runs docker, so the pairing cannot be mismatched.
struct ArchVariantProbe {
    QString variant;    // the image's remora.arch_variant label; empty = the image predates the bake
    QString flagsLine;  // the target's raw /proc/cpuinfo "flags : …" line; empty = unreadable
};
// Parses that probe's output: an `arch_variant=<v>` line from docker's label lookup and a raw
// cpuinfo `flags : …` line, in any order and either one absent.
//
// "<no value>" IS NORMALISED TO EMPTY, and that is the whole reason this is a parser rather than two
// section() calls. `docker image inspect --format '{{index .Config.Labels "…"}}'` prints the Go
// template's nil-map sentinel — literally "<no value>" — for an image carrying NO labels at all,
// which is exactly the pre-bake image the guard must wave through. Left alone it would arrive at
// missingArchVariantFlags() as a variant NAME; that returns empty for an unknown name, so the deploy
// would have passed by luck rather than because anything decided it should.
ArchVariantProbe parseArchVariantProbe(const QString &probeOut);

// The host encoder's published shape, one line of key=value it rewrites atomically at
// <frame socket>.status on every (re)build (vendor/native/remora-frame-encoder.c). Exists because
// the encoder degrades AUTONOMOUSLY — halving its output on GPU allocation failures — and that
// used to live only in its stderr, which nothing watches: a 4x resolution drop read as a broken
// Remora and cost a session to diagnose (bd remora-e5x.18.3).
struct EncoderStatus {
    QString state;  // ok | degraded | rebuilding | failed
    int nativeW = 0, nativeH = 0;  // what the display publishes
    int encodeW = 0, encodeH = 0;  // what the encoder actually serves
    qint64 bitrate = 0;
    int degrade = 0;  // halvings from the (possibly --max-size capped) source
    // -1 = the encoder predates these fields. 0 is a REAL answer: publishers=0 is the one state
    // none of the socket/process/freshness checks can see — an encoder that is up, fresh and
    // bound with nothing composing behind it, where every mirror starves (bd remora-r6au).
    int publishers = -1, consumers = -1;
    // -1 = the encoder predates the field. 1 = several consecutive keyframes came in too small for
    // their resolution, i.e. the encoder is being handed flat frames while everything else reads
    // healthy — the one shape where publishers/consumers/rate all say ok and the picture is black
    // (bd remora-pobz). A suspicion to surface with a remedy, never a reason to fail a step.
    int blackStream = -1;
};
std::optional<EncoderStatus> parseEncoderStatus(const QString &fileText);

// Empty when the encoder is serving full size; otherwise one sentence naming the shape and the
// remedy, shared by `remora check` and the connect step's detail so they cannot drift.
QString encoderDegradationNote(const EncoderStatus &s);

std::optional<QString> parseBakedGpuNode(const QString &inspectArgs);
bool parseIdGroups(const QString &idOut, const QString &group);
bool parseLsmodHas(const QString &lsmodOut, const QString &module);

// True when a `journalctl -u systemd-oomd` extract shows an actual kill DECISION. oomd does not
// log the word "killed" — it logs `Marked <cgroup> for killing due to memory used ... and swap
// used ... being more than N%`. Matching on "Killed" therefore never fires, which is how this
// check was first written. The purely informational `Considered N cgroups for killing, top
// candidates were:` line must NOT count: it is emitted while surveying and does not mean anything
// died (bd remora-4ei.23).
bool parseOomdKilled(const QString &journalOut);

// True when the BUILD LOG shows an action SIGKILLed inside the container — the cgroup OOM killer
// hitting the --memory cap, which is a different failure from systemd-oomd taking the whole docker
// scope. It has to be read out of the log because the container itself survives: ninja fails, so
// the OUTER exit code is 1, not 137, and oomdKillNote never fires for this class (bd remora-4ei.30).
// Worth naming loudly, because the log line it produces —
//   sbox_command.0.bash: line 1: 1777 Killed ... metalava ... exit status 137
// reads exactly like a metalava/toolchain error and is not one. That misreading cost a detour.
bool parseInnerOomKill(const QString &buildLog);

// True when the BUILD LOG shows the source tree's filesystem went read-only mid-build. btrfs
// force-latches read-only after a failed write barrier — by design, and NOT a device failure: the
// incident logged a single NVMe flush timeout with wr 0, rd 0, corrupt 0 (bd
// remora-4ei.20). The build then reports a generic exit 1 and the reason is buried in
// out/verbose.log.gz, so the failure reads as a build error. It is not one, and no amount of
// retrying or rebuilding fixes it — a latched btrfs needs a full unmount and remount, not
// `mount -o remount,rw`.
bool parseReadOnlyTree(const QString &buildLog);

QSet<QString> parseDockerPsNames(const QString &psOut);

// What a RUNNING container occupies, for multi-instance collision detection (bd remora-4u4.1).
// Names alone cannot answer "would starting this profile break that one?" — two instances collide
// on the host adb PORT and on the /data bind SOURCE, and those are the two that corrupt or fail.
struct ContainerFacts {
    QString name;
    QSet<QString> hostPorts;    // host side of -p, e.g. "5555"
    QSet<QString> bindSources; // host side of -v, e.g. "/home/user/.remora-bm-data"
    QSet<QString> devices;      // host side of --device, e.g. "/dev/video0"
};
// Parses `docker inspect --format '{{.Name}}\t<ports|>\t<binds|>'` — one record per line, list
// items separated by '|' because a bind is "src:dst:opts" and a path may contain spaces.
// Malformed lines are dropped rather than guessed at: a container whose facts could not be read
// must not become a container with NO facts that every collision check then passes.
QVector<ContainerFacts> parseDockerInspectFacts(const QString &out);

// One "/dev/dri/renderD<N> <driver>" line per render node, from the probe's all-nodes scan (bd
// remora-4ei.79). Malformed lines are dropped rather than guessed at: a node whose driver could
// not be read must not become a node with an EMPTY driver that some comparison then matches.
struct RenderNode {
    QString node, driver;
    // The render node's display sibling (/dev/dri/cardN — same PCI device), empty when the scan
    // could not pair them. Carried so the multi-GPU mask can cover BOTH nodes of a non-selected
    // GPU: a privileged container sees every /dev/dri entry, and the in-container stack does its
    // own device discovery, so an unmasked second GPU can hijack rendering (bd remora-n4qx).
    QString card;
};
QList<RenderNode> parseRenderNodes(const QString &out);

// ─────────── host LAN (macvlan parameters) ───────────

// The docker host's primary IPv4 LAN attachment, as read from `ip route`/`ip addr`. This is what
// a macvlan network needs and what the user should never have to type: parent is the NIC the
// default route leaves by, subnet is that NIC's own network (the /24 in the usual case — the
// prefix comes from the interface, it is not assumed), gateway is the default gateway.
struct LanInterface {
    QString parent;    // "UMN", "eno1", …
    QString subnet;    // "192.168.0.0/24" — NETWORK address, not the host's own
    QString gateway;   // "192.168.0.1"
    QString hostIp;    // "192.168.0.12" — the host's address on that NIC
};

// "192.168.0.12/24" → "192.168.0.0/24". Masks the address down to its network so the CIDR can be
// handed straight to `docker network create --subnet`; docker rejects a subnet whose host bits
// are set. Empty on anything that is not a dotted-quad + prefix in range.
QString ipv4NetworkCidr(const QString &addrCidr);

// The TOP slice of a subnet, as a CIDR: ipv4TopRange("192.168.0.0/24", 28) → "192.168.0.240/28".
// This is the default docker IPAM pool for an auto-assigned macvlan address. It matters because
// docker's IPAM knows nothing about the LAN's DHCP server: given the whole /24 it hands out .2,
// .3, … which is exactly where a consumer router's lease pool lives, and the collision surfaces
// as two devices fighting over an address rather than as a Remora error. The top /28 is the
// conventional "static/reserved" end of a home subnet. Empty when the slice does not fit.
QString ipv4TopRange(const QString &subnetCidr, int prefixLen);

// True when `ip` is inside `cidr` (both IPv4). Used to reject a pinned macvlan address that does
// not belong to the network it would ride — docker's own error for that is opaque.
bool ipv4InSubnet(const QString &ip, const QString &cidr);

// An address `delta` away from the one given (a bare IP or the base of a CIDR):
// ipv4Offset("192.168.0.240/28", -1) → "192.168.0.239". This is how the host shim's own address is
// derived from the container pool — one below it, so the two never contend. Empty when the result
// would leave IPv4 or the input does not parse.
QString ipv4Offset(const QString &ipOrCidr, int delta);

// `ip -o -4 route show default` + `ip -o -4 addr show` → the primary LAN attachment. The default
// route with the LOWEST metric wins (a box with both wifi and ethernet up has two, and the metric
// is the kernel's own answer to "which one is primary"); the address line for that NIC supplies
// the prefix. Docker's own bridges are never candidates — they are on the route table too, and a
// macvlan parented on docker0 would be a network that reaches nothing.
std::optional<LanInterface> parseHostLan(const QString &routeOut, const QString &addrOut);

// `pm list packages -3` → the third-party package names. Each line is "package:com.foo"; -3 has
// already excluded system apps, so what comes back is exactly the set a user installed.
QStringList parsePmPackages(const QString &out);

// `pm path <pkg>` → every APK belonging to that package. A Play-installed app is usually SPLIT —
// base.apk plus per-density/per-abi/per-language splits — and all of them have to be installed
// together (adb install-multiple) or the app installs and then crashes on a missing resource.
// Returning a list rather than a single path is the whole point of this parser.
QStringList parsePmPaths(const QString &out);

// The Apps page's arm probe: one "pkg=updateOwner" line per arm64 third-party app, owner empty
// when no update-ownership claim is recorded (dumpsys prints the literal "null" on some builds;
// that is "no claim" too). The probe's stdout arrives with stderr merged in, so the line shape IS
// the filter — anything that cannot be such a line is dropped rather than trusted.
QHash<QString, QString> parseArmAppOwners(const QString &out);

// True when this update owner PREVENTS Play from silently overwriting an arm64 install: only the
// shell's own claim does — it is what remora-arm-fetch.sh takes at an initial install. Anything
// else (com.android.vending, or no claim at all) leaves the app exposed: Play can deliver the
// broken x86_64 split at any update, and the arm guard can only repair that after the fact, at
// the next Connect (bd remora-4ei.91).
bool armUpdateOwnerProtects(const QString &owner);

// ─────────── mirror flow: are composed frames actually reaching the stream? ───────────
// Frames can be composed on the device and then never arrive at the host encoder, and NOTHING in
// the chain says so: the rate stays plausible, the encoder reports no drops, every process looks
// healthy. What the user sees is judder, and the only way anyone found it was measuring both ends
// by hand (bd remora-j4z: 55.9 fps composed against 31.9 fps encoded — 57% — while a rate-only
// check read "30 fps" and shrugged). Both numbers are already printed; these turn them into an
// answer.

// A detached child's own log, from its /proc/<pid>/cgroup line. spawnDetached puts each child in a
// transient scope named remora-detached-<pid>-<seq> and writes /tmp/<unit>.log, so the cgroup line
// names the log — any running Remora child can be traced back to its output with nothing
// remembered. nullopt when the process is not in one of our scopes.
std::optional<QString> parseDetachedLogPath(const QString &procCgroup);

// Cumulative counters the two ends already print, newest wins: the guest agent's
// "[server] INFO: Composed N frames" and the host encoder's "[encoder] <ms> ms: N packets
// encoded". nullopt when the log carries no such line yet.
std::optional<qint64> parseComposedFrames(const QString &logText);
std::optional<qint64> parseEncodedPackets(const QString &logText);

struct MirrorFlow {
    double composedFps = 0, encodedFps = 0;
    double ratio = 1.0;   // encoded ÷ composed — 1.0 is a mirror losing nothing
    bool starved = false; // sustained loss between compose and encode
};
// The shortfall that counts as starved, shared rather than duplicated: the verdict below applies
// it, and probeMirrorFlow's fast-path gate has to agree with it or the gate can wave through a
// mirror the verdict would have condemned. That is not hypothetical — an absolute-only gate let an
// encoder run at 18 fps against 30 composed (ratio 0.60) and reported it healthy (bd remora-yxy).
constexpr double kMirrorStarvedRatio = 0.75;
// Judge one sampling window. `starved` is deliberately hard to trigger: it needs a real window, a
// real number of composed frames, and a big shortfall. A mirror that legitimately sits at 30 fps
// because that is the video's rate must NEVER trip this — the whole point is to separate "the
// content is slow" from "the frames are being lost", which is the distinction a rate-only reading
// cannot make.
MirrorFlow judgeMirrorFlow(qint64 composedDelta, qint64 encodedDelta, int windowMs);

// The long-running commands (source build, source pull, image pull) run under a pty wrapper —
// `script -qe -c` locally, `ssh -tt` remotely — because soong/ninja/repo/docker only emit their
// progress on a TTY. The wrapper's exit code is a LIE when the wrapper itself dies: a `script`
// killed at session teardown kills its shell, exits 0, and a build that was still mid-compile
// printed 'source build OK' (bd remora-c7y). So the wrapped command reports its own status through
// the pty — `; rc=$?; echo "<sentinel>$rc"; exit $rc` — and the verdict believes the sentinel, not
// the wrapper. `exit $rc` keeps the wrapper's code meaning what it always did; the sentinel is the
// part a signal cannot fake, because only the chain itself can get far enough to print it.
inline constexpr char kPtyExitSentinel[] = "REMORA-PTY-RC:";
struct PtyExit {
    int rc = 0;
    // The wrapper closed before the command reported a status: rc is synthetic (the wrapper's own
    // nonzero code if it had one, else 143), and "the command failed" is NOT established — it may
    // still be running, detached. Callers should say "interrupted", not "failed".
    bool interrupted = false;
};
// wrapperRc is what the wrapper claimed (with a crashed wrapper already folded to nonzero by the
// caller); outputTail is the last stretch of combined pty output. The LAST sentinel wins, and a
// sentinel outranks the wrapper even when the wrapper crashed — the chain reported first, and the
// wrapper dying on teardown afterwards does not un-happen the build's own verdict.
PtyExit judgePtyExit(int wrapperRc, const QString &outputTail);

// The fast-path gate probeMirrorFlow opens with: true means "healthy enough to stop measuring".
// Pure, and here rather than in the prober, because it is a JUDGEMENT and the prober is plumbing —
// the same reason judgeMirrorFlow lives here. It was unreachable to tests while it sat inline
// behind a Spawner, which is how it shipped waving through a mirror at 60% (bd remora-yxy).
//
// Both conditions, never one: the rate says the encoder is carrying real frames rather than its
// keepalive, and the ratio says it is keeping UP with them. Returning false is not an accusation,
// only a refusal to answer early — the caller then measures the full window and lets
// judgeMirrorFlow decide.
bool mirrorGateIsClearlyHealthy(qint64 composedDelta, qint64 encodedDelta, int gateMs);

// NVIDIA loaded-module vs installed-userspace versions (bd remora-81cp). A driver update without
// a reboot leaves the two disagreeing, and every NEW GPU client then dies with 'NVRM: API
// mismatch' — the Venus render server's per-client forks fail, EGL never initialises and
// SurfaceFlinger crash-loops, which presents as an image regression. The scan is one `cat` of
// /proc/driver/nvidia/version plus an `ls` of the libnvidia-glcore.so.* install locations; this
// parses its combined output. Both sides absent is the normal non-NVIDIA answer.
struct NvidiaVersions {
    std::optional<QString> kernel;  // the NVRM line's version — what the LOADED module is
    QStringList userspace;          // distinct installed libnvidia-glcore.so.<version> suffixes
};
NvidiaVersions parseNvidiaVersionScan(const QString &scan);
// True only when both sides are known AND disagree — an absent module or an unfound userspace
// library is "nothing to compare", never a mismatch.
bool nvidiaAbiMismatch(const NvidiaVersions &v);

}  // namespace remora
