#pragma once
#include <QMap>
#include <QStringList>
#include <optional>
#include <utility>

#include "core/Backend.h"
#include "core/Parsers.h"
#include "core/Resolved.h"

namespace remora {

// Pick which host keyboard the guest's uinput device impersonates. selector: nullopt/empty =
// auto (the host's first real keyboard), "none" = decline and keep the generic identity, anything
// else = exact name match, then a case-insensitive substring. Returns nullopt when the selector
// names a keyboard that is not present — a Bluetooth keyboard that is simply switched off looks
// exactly like a typo here, so the caller reports it rather than silently impersonating something
// the operator did not choose (bd remora-4ei.34).
std::optional<KbdIdentity> selectKbdIdentity(const QList<KbdIdentity> &keyboards,
                                             const std::optional<QString> &selector);

// The identity as environment for a uinput-device helper. Hex for the ids because that is
// how /proc/bus/input/devices, uinput and every keyboard datasheet spell them.
QStringList kbdIdentityEnv(const KbdIdentity &k);

// The two command-builders — pure, golden-tested. Reproduce the real docker-run / mirror
// invocations token-for-token. The engine executes these vectors verbatim.
QStringList buildDockerRunArgv(const ResolvedConfig &cfg);
// The androidboot.* tail of the run argv, on its own so the smart-connect gate can compare the
// RESOLVED set against a running container's .Args in one move (bd remora-wlun).
QStringList buildBootArgs(const ResolvedConfig &cfg);
// …and the namespace fold that strips the androidboot./remora_ spellings for that comparison.
QStringList foldBootArgs(const QStringList &args);

// `docker network create` for the macvlan network the run above attaches to (bd remora-400).
// Empty when the config does not describe a creatable network — no name, or no parent/subnet/
// gateway resolved — so a caller cannot accidentally create one on guessed parameters; the deploy
// reports the missing piece instead. --ip-range is emitted only when one resolved; see
// ipv4TopRange() for why an auto-assignment pool narrower than the subnet is the default.
QStringList buildMacvlanCreateArgv(const ResolvedConfig &cfg);

// The host interface that carries the macvlan host route, and the image used to configure it.
extern const char *const kMacvlanShimName;
extern const char *const kMacvlanShimImage;

// The blocks of container addresses this host has to route through the shim: the auto-IP pool,
// plus a pinned macvlan_ip as a /32 when it falls outside that pool (a pin is validated against
// the SUBNET, not the range, so being outside is normal — and a shim covering only the pool would
// leave the pinned container unreachable, which is the whole point of the exercise). Empty when
// there is nothing to route.
QStringList macvlanShimRoutes(const ResolvedConfig &cfg);

// `docker run` that brings the host shim up, in ONE privileged container sharing the host's network
// namespace (bd remora-400). This needs no sudo and grants nothing new: `bare` already requires
// docker-group membership, which is root-equivalent by construction — the same access that lets
// this create a host interface would let anyone mount the host root. It is spelled out in the
// deploy log rather than done quietly.
//
// Idempotent by construction: the link is only added when absent, the address and every route use
// `replace`, so a re-run converges instead of tearing down a working path. Empty when the shim
// cannot be described (no parent, no shim address, nothing to route).
QStringList buildMacvlanShimArgv(const ResolvedConfig &cfg);

// The prebuilt guest libraries the Venus stack needs laid over the image, as (fileName,
// containerPath) pairs. Only gralloc.minigbm_gbm_mesa.so comes from our source patches and is
// inherited from the image; everything here — including the Venus ICD, whose in-image copy is a
// DIFFERENT and capture-broken build — must be overridden. Shared by the docker-run builder and
// the preflight that checks venusLibDir is complete, so the two cannot disagree about what
// "complete" means.
const QList<QPair<QString, QString>> &venusGuestLibs();

// One shared-folder entry "<hostDir>[:<name>]" → the docker bind spec
// "<hostDir>:/data/media/0/<name>" (name defaults to the host dir's basename). /data/media/0 IS
// the backing store of the device's /sdcard, so the folder lands there. Shared by the docker-run
// builder and the smart-connect staleness probe.
QString sharedFolderBind(const QString &entry);
QString stagedShareBind(const QString &entry);  // overlayfs staging spot (rebound after boot)

// Companion-app entry, in one of two forms:
//   "<hostDir>[:<Name>]"        the directory already lives on the DOCKER HOST (like sharedFolders)
//   "local:<localDir>[:<Name>]" the directory lives HERE; Remora stages it to the docker host
// The prefix is explicit rather than inferred from the backend, so an entry means the same thing
// wherever it is read — the alternative (guessing from whether the path also exists locally) would
// let a coincidental same-named local directory overwrite the host's copy.
// <Name> defaults to the directory's basename and is what PackageManager will call the app's
// codePath; it must match the APK inside.
bool companionAppIsLocal(const QString &entry);
QString companionAppName(const QString &entry);
// The local source directory. Empty unless the entry is "local:".
QString companionAppSourceDir(const QString &entry);
// Where the directory sits on the DOCKER HOST. A staged entry lands under <rezmodsDir>/apps/<Name>;
// with no rezmods dir (bare, where the docker host IS this machine) the local path is used as-is.
QString companionAppHostDir(const QString &entry, const QString &rezmodsDir);
// The docker -v value binding that host directory read-only at /system/system_ext/app/<Name>.
QString companionAppBind(const QString &entry, const QString &rezmodsDir = QString());
// Empty when every entry can work on a REMOTE docker host; otherwise the refusal naming the
// "local:" entries, which have had no staging path there since the vm backend retired
// (bd remora-pzq) — the bind would name a directory on the wrong machine and docker would
// silently create it empty. Checked by RemoteDeployer's preflight; bare never asks.
QString companionAppsRemoteRefusal(const QStringList &companionApps);

// Returns (env, argv) for the in-house mirror client (`remora mirror`, bd remora-28ix.2). The
// env map is empty today (the client took its configuration as flags) but stays in the signature
// for the spawn sites, which layer the Alt+D toggle variables on top. argv[0] is the literal
// "remora"; spawn sites substitute the running binary's absolute path.
// fullscreenDisplay=true selects the fullscreen-display mode: a separate Android display via
// --new-display at cfg.fullscreenDisplay (+ --start-app), and window geometry is dropped.
// Default false = the windowed main-display mirror.
std::pair<QMap<QString, QString>, QStringList> buildMirrorArgv(const ResolvedConfig &cfg,
                                                               const QString &target,
                                                               bool fullscreenDisplay = false);

// Picture-in-picture: a SECOND mirror alongside the main one — small, always-on-top, silent.
// Built by transforming the ordinary mirror vector rather than duplicating codec/serial logic.
//
// The window carries its own class via --app-id ("remora-pip"), the ONLY way to distinguish the
// two windows: both are the same binary, so without it a tiling script sees one class and cannot
// tell the PIP from the main mirror. That class is what a KWin/Krohnkite exclusion rule keys on
// (bd remora-4ei.38).
//
// `instance` scopes the WINDOW TITLE ("Remora PIP — <instance>"), which is what the toggle matches
// on; the app_id stays constant so one class rule still covers every profile's PIP. Empty instance
// = the bare "Remora PIP" title.
std::pair<QMap<QString, QString>, QStringList> buildPipMirrorArgv(const ResolvedConfig &cfg,
                                                                  const QString &target,
                                                                  const QString &instance = {});

// The toggle's match target — kept beside the builder so the two cannot drift apart.
QString pipWindowTitle(const QString &instance);

// Make one profile's /sdcard browsable from the desktop file manager, without binding anything as
// a shared folder and without touching the user's account.
//
// /data/media is 0550 media_rw:media_rw the moment init has run once, and the desktop session is
// neither that user nor in that group — so the folder is there and simply not ours to open. This
// grants the desktop uid a POSIX ACL on the tree instead: an access ACL for what exists, and a
// DEFAULT ACL so everything the device creates later inherits one too.
//
// The default ACL is the whole point. A chmod pass covers only what is on disk when it runs and
// goes stale the next time an app writes a file; inheritance is applied by the kernel at creation
// time, so it cannot fall behind. Verified against Android's own behaviour on XFS: a directory
// and file created afterwards as media_rw, then chmodded 02770/0660 the way Android does, still
// carry user:<uid>:rwx — the mode's group bits set the ACL mask, and Android keeps those at rw/
// rwx, so the mask never clamps the entry away.
//
// ONE setfacl call, and no shell: `u:<uid>:rwX,d:u:<uid>:rwx` carries the access and the default
// entry together, and setfacl applies the default half to directories only, so a single -R pass
// over `mediaDir` covers the parent, the /0 tree and everything created under it later. That
// matters beyond tidiness — the elevator's prompt names the program it is about to run as root,
// and "setfacl" is something a user can recognise where "sh" is something they have to trust.
// It also leaves nothing user-supplied inside a script: the path is an argv element, full stop.
//
// `mediaDir` is <dataDir>/media (or <dataDir>/upper/media under overlayfs). Takes effect
// immediately — no re-login, and nothing about the user's account changes.
QStringList buildStorageAclArgv(const QString &mediaDir, uint uid,
                                const QString &elevator = QStringLiteral("pkexec"));

// Post-connect device tune, applied over adb once the target is reachable: independent audio,
// sideload-friendly package verification, a WebView provider when the device reports none, and
// the refresh-rate pins above 60 Hz. Deliberately leaves the animation duration scales alone so
// Developer options stays the one place they are set.
QStringList buildDeviceTuneArgv(const ResolvedConfig &cfg, const QString &target);

// Custom-image pipeline vectors (pure; the engine runs them locally or wraps them over ssh).
QString imageArchivePath(const QString &outputDir, const QString &tag);   // <out>/<tag-sanitized>.tar
// Host-side encoder for the mirror (bd remora-e5x.6). Golden-tested like every other argv.
QStringList buildHostEncoderArgv(const ResolvedConfig &cfg);
// The host-side DECODE helper's argv (bd remora-e5x.37). Listens on one socket;
// c2-va's RemoteVideoDecoder connects per stream.
QStringList buildHostDecoderArgv(const ResolvedConfig &cfg);
// The same helper's --probe argv. Shares ONE node/driver rule with the session argv
// above: a probe answering for a different device than the session opens would
// green-light a configuration nobody tested.
QStringList buildHostDecoderProbeArgv(const ResolvedConfig &cfg);

QStringList buildImageBuildArgv(const QString &tag, const QString &buildDir);
QStringList buildImageSaveArgv(const QString &tag, const QString &archivePath);

// Shell that regenerates the trimmed cpufreq tree at `outDir` on the DOCKER HOST (bd remora-6279).
//
// CLUSTERS BY MAXIMUM FREQUENCY, which is what a cpufreq policy means on a phone — one per
// hardware cluster — and is why this needs no invented numbers: every emitted policy states a real
// frequency and the real set of CPUs that reach it. On the 12900K the host's 24 per-CPU policies
// collapse to exactly three (5.2 GHz favoured P-cores, 5.1 GHz remaining P-cores, 3.9 GHz
// E-cores), which is the shape of a prime/big/little phone rather than an approximation of one.
//
// Named policy<lowest CPU in the cluster>, the kernel's own convention (a Snapdragon's are
// policy0/policy4/policy7), so a guest that parses the name for a CPU index still reads a CPU that
// is genuinely in that cluster.
//
// CPU COUNT IS LEFT ALONE, deliberately: /sys/devices/system/cpu/cpu[0-9]* and nproc keep saying
// 24. The overrun was driven by POLICY count, fixed tables are sized per cluster, and pretending
// there are fewer CPUs would cost real parallelism to fix a problem that is not about cores.
//
// Emitted as shell rather than done in C++ because the tree must be built on whichever machine
// runs docker, which for backend=remote is not this one — the same reason the probe asks the
// docker host for its /dev/video*.
QString buildCpufreqTopologyCmd(const QString &outDir);

}  // namespace remora
