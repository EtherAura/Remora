#pragma once
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

#include "core/Backend.h"
#include "core/Caps.h"
#include "core/Config.h"
#include "core/Parsers.h"
#include "core/SourcePatches.h"  // VendorDriftRow — auditVendorTreeDrift's row type
#include "core/Resolved.h"

namespace remora {

// ─────────── the subprocess boundary (injectable for tests) ───────────
struct ProcResult {
    int rc = 0;
    QString out;
    QString stderrTail;
};
using LineSink = std::function<void(const QString & /*stream*/, const QString & /*text*/)>;
struct Detached {
    qint64 pid = 0;
};

class Spawner {
public:
    virtual ~Spawner() = default;
    virtual ProcResult run(const QStringList &argv, const QMap<QString, QString> &env = {},
                           const LineSink &onLine = {}) = 0;
    virtual Detached spawnDetached(const QStringList &argv,
                                   const QMap<QString, QString> &env = {}) = 0;
    virtual bool detachedAlive(const Detached &d) = 0;
    // Waiting is impurity like any other, so it goes behind the Spawner too — the engine paces
    // itself against a real device (boot polls, encoder settle, liveness holds), and a fake device
    // answers instantly, so a test that also waits is only sleeping. This is not a micro-
    // optimisation: those waits WERE the engine suite's entire runtime (~371s of 371s), and a
    // six-minute gate gets run less often than it should be, exactly when it matters most —
    // during a build (bd remora-eoy.9). Virtual with a real default so RealSpawner and any future
    // implementation keep the blocking behaviour without opting in.
    virtual void waitMs(int ms);

    static QStringList sshArgv(const QString &host, const QString &remote, int connectTimeout = 5);
    // Wrap a detached command in a transient systemd scope so it does not inherit the launching
    // app unit's cgroup. startDetached() detaches the PROCESS but not the cgroup, so a GUI started
    // from the desktop launcher left the mirror inside its app-*.service and the unit's KillMode
    // reaped the mirror on teardown — breaking the documented "the mirror survives Remora exiting"
    // guarantee for every non-terminal launch (bd remora-4ei.29). Returns argv unchanged when
    // there is no systemd user manager to ask, so non-systemd hosts keep the old behaviour.
    static QStringList scopedArgv(const QStringList &argv, bool haveSystemdUser,
                                  const QString &unit);
};

class RealSpawner : public Spawner {
public:
    ProcResult run(const QStringList &argv, const QMap<QString, QString> &env,
                   const LineSink &onLine) override;
    Detached spawnDetached(const QStringList &argv, const QMap<QString, QString> &env) override;
    bool detachedAlive(const Detached &d) override;
};

// ─────────── steps + chain runner ───────────
enum class StepState { Pending, Running, Ok, Failed, Skipped };
QString stepStateToString(StepState s);

struct StepResult {
    bool ok = false;
    QString detail;
    QString stderrTail;
    static StepResult good(const QString &d = {}) { return {true, d, {}}; }
    static StepResult fail(const QString &d, const QString &err = {}) { return {false, d, err}; }
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void line(const QString & /*step*/, const QString & /*stream*/, const QString & /*text*/) {}
    virtual void state(const QString & /*step*/, StepState /*st*/, const QString & /*detail*/ = {},
                       const QString & /*stderrTail*/ = {}) {}
};

struct Step {
    QString name, title;
    std::function<StepResult(LogSink &)> run;
};
bool runChain(const QVector<Step> &steps, LogSink &sink);

// ─────────── the resolved plan a backend executes ───────────
// Window-title prefix that marks a desktop-mode window (the device on its own display, as opposed
// to the mirror or to one app's window). It is both the user-visible taskbar name and the thing
// RunContext::desktopWindow() tests, so `remora desktop` and the window Alt+D spawns must spell it
// the same way — hence one constant rather than the two string literals this used to be.
inline constexpr QLatin1String kDesktopTitlePrefix{"Remora-desktop:"};

struct RunContext {
    Backend backend;
    ResolvedConfig rc;
    QStringList dockerRunArgv;
    QMap<QString, QString> mirrorEnv;
    QStringList mirrorArgv;
    // The mirror argv + boot-splash flags (shares mirrorEnv): ONE mirror whose window opens
    // immediately, shows deploy status (statusPath), plays the device's boot animation while it
    // boots, then mirrors — all in the same window.
    QStringList splashArgv;
    // Append-only status file the splash tails. Chain-step titles land here (StatusSink), the
    // provision step appends REMORA_BOOTING when a fresh Android boot starts, and a failed run
    // appends REMORA_ABORT so the splash closes instead of waiting forever.
    QString statusPath;
    // The pre-chain-spawned splash mirror (startSplash). doConnect adopts it instead of spawning
    // a second mirror; null when no splash runs (reconnect, fullscreen, auto-IP targets).
    std::shared_ptr<Detached> splash;
    QString guest;
    // The profile on disk still says backend=vm. Carried here so preflight can refuse it — see
    // retiredVmBackend() in Deployers.cpp for why it is not quietly treated as remote.
    bool retiredVmBackend = false;
    // The profile's gpu_mode= is neither host nor guest; preflight refuses it by name.
    std::optional<QString> unrecognisedGpuMode;
    QString windowTitle;
    // A desktop/app launch (--new-display) is an ADDITIONAL window: it must not disturb a running
    // mirror, so doConnect skips the stale-mirror / stale-server kills when this is set.
    bool fullscreenDisplay = false;
    // True only for a window on its OWN Android display (an app popup or desktop mode) — the one
    // kind of session that is not the device's mirror and must not act on the mirror's behalf.
    // NOT the same as fullscreenDisplay: the GUI's Fullscreen action sets that on a profile with
    // no separate display, and a genuine mirrored fullscreen IS the mirror.
    bool extraWindow() const { return fullscreenDisplay && rc.fullscreenSeparateDisplay; }
    // …and of those, the ones showing the DEVICE's desktop-mode display rather than one app: the
    // other side of the Alt+D pair. Keyed on the window title because that string is already the
    // marker — cmdDesktop names the window with it and mirrorSpawnEnv gives the window Alt+D
    // spawns the very same name, so the test cannot drift from what the user sees in the taskbar.
    bool desktopWindow() const {
        return extraWindow() && windowTitle.startsWith(kDesktopTitlePrefix);
    }
};
RunContext makeContext(const RemoraConfig &cfg, Backend backend,
                       const std::optional<HostCapabilities> &caps = std::nullopt,
                       const QString &windowTitle = {}, bool fullscreenDisplay = false);

// shared connect tail — adb (retry), boot gate, then the external mirror (relaunched if it dies
// immediately). bootWaitMs bounds the wait for sys.boot_completed once adb is up: adbd starts
// long before system_server, and a first boot (GApps dex2oat) needs minutes. The adb window must
// cover a cold start: adbd on a freshly started container needs well past 5s to answer over
// macvlan (a 5×1s window failed seconds before the device came ready). A dead target still fails
// fast via the container-running probe, not this loop.
// waitLit holds the mirror launch until the device's screen actually composites (screencap-size
// probe) — sys.boot_completed can precede the launcher's first frame by ~10-15s on a slow GPU,
// and a mirror attached in that window opens onto black. Pass waitLit on any path where a boot just
// happened. When ctx.splash is alive, the launch ADOPTS it (the splash IS the mirror, already
// showing the boot in the window the mirror will use) instead of spawning a second mirror.
// bootStableMs: once sys.boot_completed reads 1, require it to HOLD this long before proceeding —
// a fresh /data first boot hits boot_completed=1, then does its setup and REBOOTS (factory_reset ->
// ota) seconds later, which tears down a mirror attached to that first =1. Deploy paths (a boot may
// have just happened) pass a non-zero value; warm reconnects leave it 0 so they stay instant.
// mirrorHoldMs: after the mirror passes its liveness check, require it to keep RUNNING this long
// before reporting success — but only when this attempt ADOPTED the boot-anim splash. The adopted
// splash passes the 1s liveness check while still playing its cached animation and can still drop
// afterwards, so if it dies inside this window, relaunch a plain mirror.
// NB: the guess this comment used to carry — "the guest server / VAAPI encoder isn't ready
// the instant boot_completed lands" — was WRONG, and cost a day chasing encoder readiness. The
// "first connect closes, second connect holds" symptom was Remora's own killStaleServer reaping
// the splash's live guest server; measured encoder readiness was +0s on every cold boot. See bd
// remora-eoy.10. This parameter remains a backstop for an unexplained drop, not for that one.
// Deploy paths pass a non-zero value; reconnects (no splash) leave it 0 so they stay instant.
StepResult doConnect(Spawner &sp, const RunContext &ctx, LogSink &sink, int retries = 15,
                     int retryDelayMs = 2000, int livenessDelayMs = 1000, int mirrorRetries = 5,
                     int bootWaitMs = 240000, bool waitLit = false,
                     int bootStableMs = 0, int mirrorHoldMs = 0);

// Ensure the phantom keyboard is up (bd remora-gf7). Idempotent — it checks for a running helper
// first — because it runs on every path that can leave a container without one: a deploy creates
// it, but a RECONNECT attaches to a container whose helper may have died, and watch-boot restarts
// the container, which kills the helper outright. Without this on those paths the physical
// keyboard silently disappears mid-session and the on-screen IME comes back.
StepResult phantomKeyboard(Spawner &sp, const RunContext &ctx, LogSink &sink);
// Expose the profile's shared_inputs devices inside the container and leave a replug watcher
// behind (bd remora-4ei.36). Advisory like the phantom keyboard: it never fails the deploy.
StepResult shareInputDevices(Spawner &sp, const RunContext &ctx, LogSink &sink);

// Kill this profile's mirrors that have outlived their guest server (bd remora-8d7). Such a
// client stays alive forever holding an ESTAB slot on the host encoder's video socket with
// nothing composing behind it, and the encoder keeps fanning packets at it — which intermittently
// stalls delivery to the window someone is actually watching. Identified by the TUNNEL: a live
// mirror owns TCP connections for the adb forward, a husk owns none. Returns how many were
// reaped. Conservative by construction — it skips processes younger than the age floor (a
// starting mirror has no tunnel yet either) and reaps nothing at all when `ss` is unavailable,
// because absent evidence must never read as "it is dead".
int reapMirrorHusks(Spawner &sp, const RunContext &ctx, const QString &target,
                    const LineSink &log);

// Reap the host-side session of a container being TORN DOWN: every mirror client on its adb
// target plus the frame encoder on its socket, then the socket inodes themselves. The mirror is
// detached into its own scope precisely so Remora's exit cannot kill it — which means nothing
// killed it when its container died either, and the survivors flickered over the next session's
// encoder while holding ~0.5 GiB each (bd remora-e5x.19.1). An empty `target` skips the client
// reap (a macvlan profile with no pinned IP, or a takeover of an unresolvable profile) but still
// cleans the encoder and sockets, which derive from the container name alone. Returns how many
// processes were reaped.
int reapSessionOrphans(Spawner &sp, const QString &containerName, const QString &target,
                       const LineSink &log);

// Run a shell snippet ON THE DOCKER HOST, wherever that is: locally when `host` is empty (bare),
// over ssh otherwise (remote). Without this, "the docker host" is spelled two ways at every
// call site and a helper written for one backend silently builds `ssh "" …` on the other.
QStringList dockerHostArgv(const QString &host, const QString &cmd);

// Ensure the macvlan network the resolved config names exists on the docker host, creating it from
// the probed host LAN when it does not (bd remora-400). Returns a one-line description of what it
// found or made; on failure the message names the missing piece — a network cannot be conjured
// from a subnet nobody knows. No-op (empty return, ok) outside macvlan mode.
struct NetworkEnsure {
    bool ok = true;
    QString detail;  // "lan present" | "created lan (192.168.0.0/24 via …)" | failure reason
};
NetworkEnsure ensureMacvlanNetwork(Spawner &sp, const ResolvedConfig &rc, const QString &host,
                                   const LineSink &log = {});


// Put the macvlan host route in place so THIS machine can reach the container it is hosting
// (bd remora-400). Bare only — everywhere else the docker host is a different machine and the
// traffic crosses the real LAN, where nothing is isolated. Idempotent and cheap enough to run on
// every connect; the shim survives until reboot and is simply re-asserted.
//
// Returns false only when the shim was needed, attempted, and could not be established; `detail`
// then names the missing piece. A profile that opted out (macvlan_host_route=false) returns true
// with an empty detail, as does every non-macvlan deployment.
struct HostRouteResult {
    bool ok = true;
    QString detail;
    // The shim was created or rebuilt, or something else on the host is also managing it. Callers
    // put these in the STEP DETAIL rather than the log, because ConsoleSink drops per-line output
    // entirely — a change to host networking that only appears in the GUI log has not been
    // announced to the person running the CLI.
    bool notable = false;
};
HostRouteResult ensureMacvlanHostRoute(Spawner &sp, const ResolvedConfig &rc,
                                       const LineSink &log = {});

// The docker-inspect format that reports a container's network attachment, and the judgement over
// its output: does the EXISTING container still match the networking the config now asks for?
//
// This is the recreate trigger for a networking change, and it exists because `docker start` on an
// existing container silently ignores one — the container keeps the network it was CREATED with.
// Without this, switching a profile to macvlan and running Up reports success while the container
// stays bridged, which is the same shape of silent no-op that hid the bug in the first place
// (bd remora-400). Empty inspect output = no such container = not stale (it is about to be made).
extern const char *const kNetworkInspectFormat;
bool containerNetworkStale(const QString &inspectOut, const ResolvedConfig &rc);

// The same judgement for the container's --device passthroughs (.HostConfig.Devices), for the
// same reason: devices are fixed at CREATE time, and the camera is the one that DRIFTS — nodes
// appear and vanish with a replug while the resolver probes them live, so a container created
// while the camera was absent reported healthy with the resolved /dev/video* never reaching it,
// and the manual `docker rm` smart connect exists to remove was back (bd remora-usn0).
// `devices` is the inspected PathOnHost list, already tokenized; the CALLER decides existence —
// an empty list from a missing container must not read as stale, but an empty list from a
// device-less container genuinely is when the profile resolves a camera.
bool containerDevicesStale(const QStringList &devices, const ResolvedConfig &rc);

// The live adb target for a context. A pinned macvlan IP and every bridge target resolve
// statically (rc.target); only macvlan-with-auto-IP costs a round trip — one docker-inspect on the
// docker host for the address IPAM assigned at provision. Empty return = discovery failed (err
// says why). Shared by doConnect, appMenuSync and the shell/logcat/prop CLI passthroughs.
QString resolveAdbTarget(Spawner &sp, const RunContext &ctx, QString *err = nullptr,
                         const LineSink &log = {});

// Local mirror clients currently attached to a target — mirror, app windows and desktop
// sessions alike (every client argv carries "-s <target>"). The auto-sleep idle signal:
// 0 = nobody is looking at the device.
int countMirrorClients(Spawner &sp, const QString &target);

// Device screenshot to outPath. exec-out is binary-safe and the LOCAL shell redirects the PNG
// into the file, so no pixel data crosses ProcResult (its QString decode would mangle it). adb
// talks TCP to the resolved target, so this runs locally on every backend. The caller asserts
// the adb link first (adb connect). False + err on a failed or empty capture.
bool captureScreenshot(Spawner &sp, const QString &target, const QString &outPath,
                       QString *err = nullptr);

// The env a mirror is spawned with: mirrorEnv + the Alt+D mirror/desktop launch commands
// (built with the resolved target). Shared by doConnect's launch and the pre-chain splash spawn —
// the splash becomes the mirror, so it needs the same switching machinery.
QMap<QString, QString> mirrorSpawnEnv(const RunContext &ctx, const QString &target,
                                      const QStringList &mirrorArgv);

// Append one line to the splash status file (no-op when path is empty). REMORA_BOOTING switches
// the splash to the boot animation; REMORA_ABORT closes it; anything else is displayed.
void appendStatusLine(const QString &statusPath, const QString &text);

// ─────────── host-side helpers ───────────
// Bring the NVIDIA Venus render server up if it is not already serving, and no-op when venus is
// off. Shared by the deploy chain's host-prereqs step and by doConnect, because the server is host
// state the container depends on but does not own: any path that brings a Venus container to life
// has to check it, and a connect to an already-running container skips the chain entirely
// (bd remora-4ei.56). Treats a live process with no socket as unhealthy — a killed server leaves
// its socket inode behind, and that stale file makes a dead stack look present.
StepResult ensureVenusServer(Spawner &sp, const ResolvedConfig &rc, const LineSink &log);

// Start the host-side DECODE helper if host_decode is on, or adopt a healthy one (bd
// remora-e5x.37). Gated on the helper's own --probe rather than on our idea of the host's
// capabilities. A no-op returning good() when host_decode is off.
StepResult ensureHostDecoder(Spawner &sp, const ResolvedConfig &rc, const LineSink &log);

// Start the host frame encoder for compose mode. Declared here, beside ensureVenusServer, so the
// offline suites can assert on what it spawns — notably that the child carries the
// bd remora-e5x.18.9 heap tripwire in its environment, which lives ONLY in the env and would
// otherwise be untestable and easy to drop in a refactor.
StepResult ensureHostEncoder(Spawner &sp, const ResolvedConfig &rc, const LineSink &log,
                             bool *reboundStarved);

// Heal the artefact docker leaves at a bind-mount SOURCE that did not exist when a container was
// created: a root-owned empty DIRECTORY at the path. buildDockerRunArgv mounts buildPropOverlay at
// /vendor/build.prop, so any `docker run` that precedes the write plants one — and once there it
// makes QFile::open fail forever, with a message that reads like a permission problem. It is
// missing exactly once per NEW image tag (the filename embeds it), i.e. right after a source
// build, when an opaque failure costs the most (bd remora-gcu). Removing it is safe precisely
// because this is a generated-file path: a directory there can only be docker's artefact, never
// data. rmdir, not rm -rf — it refuses anything non-empty, and the parent is user-owned so no
// privilege is needed. Returns empty when the path is clear (nothing there, a regular file, or
// the artefact removed); an error message when a NON-empty directory sits there, which is not
// the artefact and must be inspected rather than deleted.
QString clearDockerCreatedDir(const QString &path);

// The /vendor/build.prop overlay, two generators for two modes with one shared strip list
// (overlayOwnedKeys). Stripping matters because build.prop is first-wins for ro.* props — a baked
// ro.hardware.vulkan=intel otherwise beats every later setprop (venus: bd remora-4ei.56; guest:
// the SurfaceFlinger chooseEglConfig crash loop of bd remora-e5x.33), and it beats the boot-arg
// shim too. Current images bake nothing (bd remora-xqt1 retired the pin and moved the ICD to
// androidboot.remora_vulkan); this remains the transition path for images built before that.
//  - venusBuildPropLines: pure transform for the venus writer (image lines → overlay lines).
//  - guestBuildPropScript: the ONE generation script both carriages run for gpu_mode=guest —
//    bare under a local `sh -c`, remote as an ssh payload — docker cp the image's own copy out,
//    strip the owned keys, append the SwiftShader ICD.
const QStringList &overlayOwnedKeys();
QStringList venusBuildPropLines(QStringList lines);
QString guestBuildPropScript(const QString &path, const QString &imageTag);

// Build the trimmed cpufreq tree on the docker host (empty sshHost = local), so the bind the
// docker argv already names resolves to a populated directory. Runs BEFORE the container is
// created, for the same reason venusPrereqs does: docker answers a missing bind source by creating
// an empty directory, and an empty cpufreq view is a state no phone presents. No-op when
// cpufreqDir is empty. Non-fatal on failure — see the message it returns (bd remora-6279).
StepResult ensureCpufreqTopology(Spawner &sp, const ResolvedConfig &rc, const QString &sshHost,
                                 const LineSink &log);

// Delete a profile's /data tree, INCLUDING the parts the calling user cannot touch.
//
// Android runs as root inside the container and fills the tree with root- and system-owned files,
// so an unprivileged `rm -rf` on the host fails partway through with EACCES on upper/{apex,misc,
// user,property,…} — the user owns the profile but not its contents (bd remora-4u4.7). The
// removal is therefore done BY a throwaway privileged container with the tree bind-mounted, which
// is the same privilege Android had when it wrote them and needs no host sudo.
//
// Destructive and never implicit: this is every app, account and setting on that instance. The
// caller is responsible for confirming, and for refusing while the container is still running.
// Under use_overlayfs only the per-instance diff is removed; the shared dataBaseDir is left alone.
StepResult wipeDataDir(Spawner &sp, const ResolvedConfig &rc, const LineSink &log);

// ─────────── vendored asset locations ───────────
QString vendorScript(const QString &kind, const QString &name);  // container-scripts/guest-scripts/…
// One `sh -c` payload emitting the raw inputs of parseNvidiaVersionScan (bd remora-81cp):
// /proc/driver/nvidia/version plus the libnvidia-glcore.so.* install locations. Shared by the
// readiness probe (both backends — ssh'd for remote) and ensureVenusServer's refusal.
extern const QString kNvidiaVersionScan;

// One row per source-buildable Android version (a registry entry with a release config), reporting
// whether its on-disk build assets — container patch set, pinned manifest, local manifest — are all
// present. The core registry declares WHICH versions build; this is the filesystem check the core
// cannot do, surfaced by `remora check` so a version with missing assets is caught before a build
// rather than during one (bd remora-82c.2).
struct VersionAssetRow {
    int version;
    bool ok;
    QString detail;  // the assets found, or the specific ones missing
};
QVector<VersionAssetRow> auditVersionAssets();

// The patch-tree half of the same question, over the LOCAL vendor tree the build stages from:
// every buildable version audited against the registry, flattened to one line per (version, entry)
// gap. Empty on a healthy tree. See auditPatchAssets() for why this is worth asking BEFORE a build
// rather than leaving it to the apply loop's hard-fail.
struct PatchAssetRow {
    int version;
    QString key;
    QString dir;
    QString detail;         // the missing file names — the tree is older than the binary
    QString unknownDetail;  // .patch files no registry entry names — the binary is older than
                            // the tree, and would silently build without them (bd remora-y9y9)
};
QVector<PatchAssetRow> auditPatchAssetsOnDisk();

// The CONTENT half of the same audit (bd remora-y9y9 option b): byte-compare this binary's
// source-patches tree against its counterpart — the installed copy for a dev binary, the source
// tree for an installed-vendor binary. Presence-vs-registry cannot see a patch whose bytes
// changed but whose name did not, and that failure ships the OLD patch under a green build.
//
// otherRoot is EMPTY when no counterpart exists to compare against (a packaged machine with no
// repo checkout, a dev box never installed to) — that is silence, not health, and callers must
// not print an ok-line for it. binaryStagesFromDrifted says which way the skew bites: true means
// builds stage from the drifted tree itself (loud — the repo's edits are not what ships), false
// means the drifted copy is inert to this binary (informational; bd remora-y9y9's correction is
// that exactly this box's staleness never failed a build).
struct VendorDriftReport {
    QString otherRoot;
    bool binaryStagesFromDrifted = false;
    QVector<VendorDriftRow> rows;
};
VendorDriftReport auditVendorTreeDrift();
QString vendorNativeBinary(const QString &name);                 // the built `fwd` helper
QString agentDex();  // the built mirror-agent dex (agent/build.sh); may not exist

// Fill the vendor-asset host paths the docker argv bind-mounts (the netfix pair and the init .rc
// files, the agent dex, the helpers) plus the host-derived fields core cannot read. Lives
// here rather than in the resolver because the vendor dir is a build-time location and core must
// stay free of it; makeContext() and `remora plan` both call it so the two agree token-for-token.
void applyVendorPaths(ResolvedConfig &rc, Backend backend);

// Fingerprint of everything a feature/source image build consumes: the enabled source patches'
// file contents, the vendor/source-build asset tree, and the recipe knobs (features, Android
// version, source ref). Equal fingerprints ⇒ a rebuild would consume identical inputs, so the
// GUI can hide the Build button and say so.
QString imageBuildFingerprint(const QStringList &enabledPatches, const QStringList &features,
                              int androidVersion, const QString &sourceRef);

// ─────────── backends (deployers) ───────────
class Deployer {
public:
    virtual ~Deployer() = default;
    virtual QVector<Step> steps(const RunContext &ctx) = 0;
};
// The boot poll is FINE-GRAINED rather than long: the budget is what bounds a hung boot, but the
// interval is pure overshoot on a healthy one — a 3 s interval spent up to 3 s of every cold deploy
// waiting after Android was already up. Same 72 s budget, ~1.5 s saved on average.
std::unique_ptr<Deployer> makeDeployer(Backend backend, Spawner &sp, int bootAttempts = 72,
                                       int bootIntervalMs = 1000);

// ─────────── custom-image pipeline ───────────
// Steps that realize the feature recipe on rc.buildHost (unset = build locally): stage the build
// context (Dockerfile + operator-staged payloads) → docker build → docker save into rc.outputDir.
// Run via runChain.
struct ImageRecipe;
QVector<Step> imageBuildSteps(Spawner &sp, const ResolvedConfig &rc, const ImageRecipe &recipe);

// ─────────── mutual exclusion ───────────
// Exclusion is RESOURCE-based, not name-based (bd remora-4u4.1). It used to compare running
// containers against a hardcoded list of historical names, which was wrong twice over: a second
// profile with its own name was refused even though nothing actually collided, and two profiles
// that DID collide on the host adb port or the /data bind were both waved through.
struct ClusterState {
    QVector<ContainerFacts> local;
    std::optional<QVector<ContainerFacts>> guest;  // nullopt = the docker host was unreachable
};
class ExclusionService {
public:
    ExclusionService(Spawner &sp, QString guest) : sp_(sp), guest_(std::move(guest)) {}
    ClusterState probe(bool includeGuest);
    // Stop one named container: `backend` selects the transport (local docker vs ssh). `target`
    // is the container's adb serial when the caller can resolve it — it lets the teardown reap
    // take the mirror clients too, not just the encoder (bd remora-e5x.19.1).
    void stop(Backend backend, const QString &containerName, const QString &target = QString());

private:
    Spawner &sp_;
    QString guest_;
};
// A running container that would collide with the one we are about to start, and why.
struct Conflict {
    QString holder;         // the running container's name
    QString reason;         // the resource, phrased for a message: "the host adb port 5555"
    bool sameName = false;  // the holder IS our container → a recreate, not a foreign holder
};
// nullopt when the target is free. sameName is reported in preference to any resource collision:
// our own running container necessarily collides with itself on every resource, and that case is
// a recreate the caller auto-consents to, not a conflict to prompt about.
std::optional<Conflict> conflictFor(const ClusterState &s, Backend backend,
                                    const ResolvedConfig &rc);

// ─────────── orchestrator ───────────
using ConflictFn = std::function<bool(const Conflict &)>;

bool connectRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
                const std::optional<HostCapabilities> &caps = std::nullopt,
                const QString &windowTitle = {}, const ConflictFn &onConflict = {},
                bool fullscreenDisplay = false);

bool reconnectRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
                  const std::optional<HostCapabilities> &caps = std::nullopt,
                  const QString &windowTitle = {}, bool fullscreenDisplay = false);

// Restart the device and attach the mirror during boot so the boot animation is visible (it plays only
// before sys.boot_completed, which the normal connect waits past). Requires a deployed container.
bool watchBootRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
                  const std::optional<HostCapabilities> &caps = std::nullopt,
                  const QString &windowTitle = {});

// The one-button entry: reconnect when the container is already running, full deploy otherwise.
// fullscreenDisplay=true launches the separate --new-display fullscreen session instead of the
// windowed mirror.
bool smartRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
              const std::optional<HostCapabilities> &caps = std::nullopt,
              const QString &windowTitle = {}, const ConflictFn &onConflict = {},
              bool fullscreenDisplay = false);

// Freeze the device in place (docker pause): zero CPU, full session state preserved — the
// instant-resume counterpart to a ~50s cold boot. Closes the local mirror and the device-side
// session first (a frozen session leaks its encoder context; a frozen mirror shows a dead
// frame). Waking is implicit: any Connect against a paused container unpauses it (~1s) inside
// doConnect and attaches.
bool sleepRun(Backend backend, const RemoraConfig &cfg, LogSink &sink, Spawner &sp,
              const std::optional<HostCapabilities> &caps = std::nullopt,
              const QString &windowTitle = {});

// ─────────── desktop app-menu integration ───────────
// One launcher folder per profile ("<profile>" in the desktop menu), one .desktop per installed
// Android app with its real icon; entries run `remora app "<profile>" <pkg>`. Synced after each
// successful connect when the profile's desktopMenu flag is on; also managed manually (GUI
// Integration card / `remora apps`).
struct AppMenuEntry {
    QString pkg, name;  // one generated app entry (the folder's mirror/desktop links excluded)
};
QString appMenuSlug(const QString &instance);
// pgrep/pkill pattern matching THIS instance's mirror for an adb target. Mirror windows
// are titled with just the profile name (legacy sessions carried a "Remora:" prefix — still
// matched); app and desktop windows carry other titles and must not match.
QString mirrorTitlePattern(const QString &target, const QString &windowTitle);
QVector<AppMenuEntry> appMenuEntries(const QString &slug);  // one folder's app entries
// Per-app window mode ("desktop" | "freeform" | "" = ask on first launch), kept in
// cfg.integration.appModes as "<pkg>=<mode>".
QString appModeFor(const RemoraConfig &cfg, const QString &pkg);
void setAppMode(RemoraConfig &cfg, const QString &pkg, const QString &mode);
// Packages pinned to arm64 (cfg.integration.armApps): apps with no working x86_64 build, which
// Play will happily "update" back to the broken split unless something puts them back.
bool isArmPinned(const RemoraConfig &cfg, const QString &pkg);
void setArmPinned(RemoraConfig &cfg, const QString &pkg, bool pinned);
// Which pinned packages have LOST their arm64 ABI on the live device (Play re-installed the
// x86_64 split). Empty when everything is intact, the device is unreachable, or nothing is
// pinned — the caller cannot distinguish those and does not need to: all three mean "no repair
// to run now".
QStringList armPinsNeedingRepair(Spawner &sp, const RemoraConfig &cfg, Backend backend);
bool appMenuSync(Spawner &sp, const RemoraConfig &cfg, Backend backend, const QString &instance,
                 LogSink &sink);
bool appMenuRemove(Spawner &sp, const QString &instance, LogSink &sink);

class ConsoleSink : public LogSink {
public:
    void state(const QString &step, StepState st, const QString &detail,
               const QString &stderrTail) override;
};

// ─────────── readiness + status probes ───────────
// sshHost comes from the resolved config and may be empty when unconfigured, in which case the
// probes that need it are SKIPPED rather than run against a guess. No personal host or domain
// name is baked in (bd remora-4ei.12).
HostCapabilities probeCapabilities(Backend target, const QString &sshHost, Spawner &sp);

// Just the LOCAL host's render nodes (node + driver each) — the GUI's multi-GPU picker uses this
// without paying for a full capability probe (bd remora-e5x.1).
QList<RenderNode> probeRenderNodes(Spawner &sp);

// The capability probe a DEPLOY OR CONNECT needs, or nullopt when this profile does not need one.
// EVERY path that hands caps to connectRun/reconnectRun/smartRun must go through this — passing
// std::nullopt is not a missed optimisation, it changes what the resolver produces:
//   · gpu_mode=host can only work with the LIVE render node, because the resolver refuses to bake
//     one; without the probe the mode half-applies and the guest falls back to SwiftShader
//     (bd remora-4ei.57).
//   · Venus paths are resolved FROM the probe. Without it an explicit venus=true fails its own
//     preflight claiming the render server and guest libs are missing when they are installed, and
//     an auto profile silently declines to restart a render server that has died, leaving
//     SurfaceFlinger boot-looping against a dead socket (bd remora-4ei.56, .85).
// This existed as a private helper in main.cpp, so the CLI was correct and the GUI — which passed
// std::nullopt on every mode — was not. One definition, so that cannot recur.
std::optional<HostCapabilities> deployCapabilities(Backend backend, const RemoraConfig &cfg,
                                                   Spawner &sp);

struct StatusReport {
    Backend backend;
    // bare
    bool bareRunning = false;
    // Set when `docker ps` itself failed (daemon down, socket unreachable, not in the docker
    // group): the container's state is then UNKNOWN, and bareRunning=false must not be read as
    // "stopped" — statusToJson omits `running` and emits this instead.
    std::optional<QString> bareDockerError;
    std::optional<bool> bareBoot;
    std::optional<QString> bareGles;  // SurfaceFlinger's renderer — the software-fallback datum
    // remote (the ssh docker host)
    bool remoteSshOk = false;
    bool remoteRunning = false;
    std::optional<QString> remoteDockerError;  // same as bareDockerError, for the ssh host's docker
    std::optional<bool> remoteBoot;
    std::optional<QString> remoteGpuNode, remoteGralloc, remoteGles;
    // Play Protect certification of whichever container this backend runs (bd remora-4ei.83). An
    // uncertified device blocks GOOGLE SIGN-IN, not just Play downloads, so it presents as "the
    // Play Store is broken" — worth a status row rather than a logcat dig.
    PlayCertification playCert;
    // The adb front door of whichever container this backend runs (bd remora-yn4): a dead
    // forwarder makes the device look dead to every adb consumer while adbd sits healthy on
    // :5555 — very different repairs, and nothing else in the report can tell them apart.
    ForwarderProbe forwarder;
    HostCapabilities host;
};
StatusReport probeStatus(Backend backend, Spawner &sp, const QString &sshHost = QString(),
                         const QString &containerName = QString());
QString statusToJson(const StatusReport &r);

// Is the running mirror actually carrying the frames the device composes? Samples the two
// cumulative counters that already exist — the guest agent's composed count and the host
// encoder's encoded count — twice, windowMs apart, and judges the gap.
//
// This exists because the failure it catches is invisible to every other check (bd remora-j4z).
// Frames composed on the device can stop reaching the host encoder while the rate stays entirely
// plausible, the encoder reports no drops, and every process in the chain looks healthy; the user
// sees judder and there is nothing to point at. Measured by hand once: 55.9 fps composed against
// 31.9 encoded. A rate-only reading of that says "30 fps" and finds nothing wrong.
//
// Both processes are found by their argv (the video socket identifies this profile's pair) and
// each one's log comes from its own cgroup, so nothing has to be remembered across runs — a mirror
// started by a long-gone `remora up` is as readable as one this process launched.
//
// nullopt means "cannot tell": no mirror, no encoder, a log without counters yet, or a profile
// whose video does not come from the host encoder. Never a verdict — refusing to answer beats
// inventing a fault, since the caller's whole use for this is to WARN.
// The default window is set by the SLOWEST rate worth judging, not by patience. judgeMirrorFlow
// needs several of the guest's 120-frame print batches before a ratio means anything, and at the
// 30 fps a video app composes at, six seconds only yields 180 — under the floor, so the check
// declined to answer in exactly the case it was built for (caught live: compose 29.9 fps against
// an encoder emitting 3.2, its keepalive rate, and the six-second window returned nothing).
std::optional<MirrorFlow> probeMirrorFlow(Spawner &sp, const ResolvedConfig &rc,
                                          int windowMs = 10000);


// Play Protect certification of a running container, in one round trip on whichever host runs
// docker. Everything stays nullopt when the container is down or unreachable — see
// PlayCertification for why unknown must not collapse into "uncertified".
// forceCheckin asks GMS to check in again FIRST, then waits before reading the verdict: that is
// what makes a freshly-registered android_id take effect without a reboot. The wait goes through
// Spawner::waitMs so the offline suites do not actually sleep.
// `snapshotInstance`, when set, saves the identity pair this read just observed. The probe is the
// only place that ever sees a healthy identity, so it is the only place that can keep one: a probe
// that read a good pair and did not save it is the whole bug this guards against. Writes only on
// change, and only when BOTH halves were read.
PlayCertification probePlayCertification(Spawner &sp, Backend backend, const QString &containerName,
                                         const QString &sshHost = QString(),
                                         bool forceCheckin = false,
                                         const QString &snapshotInstance = QString());

// ── The checkin identity snapshot (bd remora-4ei.84) ──
// A /data rebuild mints a NEW GSF identity, and the old one's Play Protect registration does not
// carry over — the instance silently de-certifies. The identity is a PAIR, "<android_id>:<token>",
// and it lives in exactly one authoritative file on the device; keeping a copy makes the manual
// re-registration step unnecessary. See restoreCheckinIdentity() in Probe.cpp for why writing it
// back is safe, and why the original "half-restored identity" worry did not apply.
QString checkinSnapshotPath(const QString &instance);
bool saveCheckinSnapshot(const QString &instance, const QString &pair);
std::optional<QString> loadCheckinSnapshot(const QString &instance);

// Where the snapshot stands relative to the device, in one sentence. Shared by the CLI and the GUI
// so the FACT cannot drift; each adds its own call to action, since how you run a restore differs.
QString checkinSnapshotNote(const QString &instance, const PlayCertification &live);

struct CheckinRestore {
    bool attempted = false;  // false when there was nothing to restore, or nowhere to restore it to
    bool ok = false;
    QString detail;  // always set — the reason when it did not happen, the outcome when it did
    QString pair;    // the snapshot that was (or would have been) written
};
// Write the snapshot back and make GSF adopt it. Force-stops GMS FIRST: a live GMS rewrites the
// identity files from memory, so a restore against a running one can be silently undone.
CheckinRestore restoreCheckinIdentity(Spawner &sp, Backend backend, const QString &containerName,
                                      const QString &sshHost, const QString &instance);

// How the instance's data dir is laid out, read on whichever host will run docker — the LOCAL
// filesystem for bare, over ssh for remote. Runs BEFORE the container exists, which is the whole
// point: the shape has to be checked while a mismatch is still refusable (bd remora-4ei.89).
// Anything unreadable comes back Missing, so an unreachable host cannot manufacture a refusal.
DataDirShape probeDataDirShape(Spawner &sp, Backend backend, const QString &dataDir,
                               const QString &sshHost = QString());

// The image's baked CPU tuning and the target's CPU flags, read in ONE call on whichever machine
// runs docker (bd remora-bm7.13). Both halves have to come from the same side: for remote the image
// lives on the ssh target and so does the processor that must execute it, and reading the local
// /proc/cpuinfo instead would compare a build against the wrong machine entirely — which is the
// exact `docker save | ssh docker load` mistake the guard exists to catch.
//
// Answers an empty struct without spawning anything when there is no image to ask about: no tag, a
// remote with no ssh host, or selectImage's "(no pre-built image …)" sentinel, whose parentheses are
// a shell syntax error and must never reach one.
ArchVariantProbe probeArchVariant(Spawner &sp, Backend backend, const QString &imageTag,
                                  const QString &sshHost = QString());

// Google's uncertified-device registration form — the only way to certify a GSF android_id, and it
// needs a Google login, so Remora can present it but never complete it. One definition so the CLI
// text and the GUI button cannot drift apart.
inline QString playCertRegistrationUrl() {
    return QStringLiteral("https://www.google.com/android/uncertified/");
}

// remora doctor: one shareable text bundle for triage — resolved config, the status probe,
// readiness, component versions, device props and recent logcat. Every section tolerates failure:
// a dead target still yields a bundle.
QString doctorReport(Spawner &sp, const RemoraConfig &cfg, Backend backend,
                     const QString &instance);

}  // namespace remora
