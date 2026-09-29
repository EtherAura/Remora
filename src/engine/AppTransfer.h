#pragma once
#include "Engine.h"

namespace remora {

// App portability — transfer between devices, export to / import from a local directory. APKs
// always; app DATA only on request (bd remora-4ei.73): by default apps arrive as fresh installs,
// logged out, because `adb backup` has been deprecated and increasingly ignored by apps since
// Android 12, and copying /data/data across instances means carrying SELinux contexts and
// per-install uids the target assigns differently. The opt-in data carry solves exactly those
// two: both instances are rooted (magisk_root), so it tars /data/data/<pkg> on the
// source and restores it AFTER the target's own install created the directory — then chowns to
// the uid the target actually assigned and restorecon -RF recomputes the per-app SELinux
// categories (they encode the uid, which is why copying contexts can never work).
//
// Everything here takes an already-asserted adb target: profile-name resolution stays with the
// callers (the CLI validates names via requireInstance, the GUI picks from its profile combos)
// so this layer is testable offline against a fake Spawner (bd remora-4ei.75).

// How to reach uid 0 on one side of a data carry (bd remora-4ei.73). NOT `adb shell su`, which
// is what this used to do and which could never have worked here: adb shell runs as uid
// 2000(shell), and Magisk's su CLIENT has to reach magiskd's socket under /debug_ramdisk/.magisk
// — mode 0700 root:root inside a 0711 directory, so the binary RESOLVES and the connect gets
// EACCES ("Cannot connect to daemon: Permission denied"). It then dies on SIGILL instead of
// exiting, so the caller sees a core dump rather than a status. Remora already holds uid 0 in
// the container by the shorter route Deployers uses — docker exec, locally or over ssh — and
// that also drops a dependency on root being GRANTED to adb, which is a Magisk policy setting a
// user can switch off. An EMPTY container means this machine has no profile for that side (a raw
// host:port), where the adb `su` route is all there is and a real rooted phone may well allow it.
struct RootExec {
    QString container;               // docker container name; empty = no container known
    std::optional<QString> sshHost;  // set = that container lives on this ssh host
};

// The RootExec for a profile: its resolved container, plus the ssh host when the backend is not
// bare. Callers hold the config and the backend already; this keeps the docker/ssh split in one
// place rather than at each call site.
RootExec rootExecFor(const RemoraConfig &cfg, Backend backend);

// One operation's outcome. The per-app ok/FAIL/skip lines stream through the LineSink as they
// happen; the counts are what a summary line is built from. `transferred` names the packages
// that actually landed (real installs only — a dry run leaves it empty), so callers can carry
// per-package profile state for exactly the set that moved and no more.
struct AppOpCounts {
    int done = 0, failed = 0, skipped = 0;
    // The data carry's own tally, separate from done/failed on purpose: a failed data carry
    // leaves a correctly installed (fresh, logged-out) app, which is NOT a failed transfer.
    int dataDone = 0, dataFailed = 0;
    QStringList transferred;
};

// One install job: a display label and the APK set that must be installed TOGETHER.
using ApkJob = QPair<QString, QStringList>;

// `pm list packages -3` — third-party only, so exactly the set a user installed.
QStringList listThirdPartyPackages(Spawner &sp, const QString &target);

// Pull one package's APKs into <destDir>/<pkg>/. One directory PER PACKAGE because splits are all
// called base.apk / split_config.*.apk — a flat dump would have packages overwrite each other —
// and because that layout is what lets an import reassemble a split app rather than guess.
// Returns the local paths, empty on failure.
QStringList pullApks(Spawner &sp, const QString &target, const QString &pkg,
                     const QString &destDir, QString *err = nullptr);

// install-multiple handles the single-APK case too, so there is one code path rather than a
// split/non-split branch that only one of them is ever exercised by.
bool installApks(Spawner &sp, const QString &target, const QStringList &apks,
                 QString *err = nullptr);

// Every .apk directly inside a directory, sorted so base.apk leads. install-multiple does not
// require that order, but a stable order makes failures reproducible and logs comparable.
QStringList apksIn(const QString &dir);

// adb connect + get-state gate for an explicit target (a resolveAdbTarget() result or a raw
// host:port). Returns the target once it answers as `device`, else empty.
QString assertAdbEndpoint(Spawner &sp, const QString &target);

// The import half of the directory layout pullApks() writes: each subdirectory is one app
// (splits together); loose APKs at the top level are each their own single-APK install. Grouping
// matters — installing splits separately is what produces an app that launches and then dies on
// a missing resource.
QList<ApkJob> importJobs(const QString &dir);

// Move `pkgs` from src to dst through a temporary staging dir. Under dryRun nothing is pulled or
// installed (dst may even be empty) — the split counts are reported from `pm path` alone. With
// withData, each package that installs also gets its private data carried (transferAppData)
// through srcRoot/dstRoot; a data failure is logged and tallied in dataFailed but never
// un-counts the install. srcRoot/dstRoot are unused without withData.
AppOpCounts transferApps(Spawner &sp, const QString &src, const QString &dst,
                         const QStringList &pkgs, bool dryRun, bool withData = false,
                         const RootExec &srcRoot = {}, const RootExec &dstRoot = {},
                         const LineSink &log = {});

// Carry one INSTALLED package's private data src → dst, staged through <stageDir>/<pkg>-data.tar
// (bd remora-4ei.73). Rooted on both sides via RootExec: the source tars /data/data/<pkg> (minus
// lib and the cache dirs), the target untars it over the directory its own install just created,
// chowns to the uid/gid the target assigned, then restorecon -RF. The tar itself still moves by
// plain `adb pull`/`push` — it is chmod 644 in /data/local/tmp precisely so the unprivileged half
// needs no root and the same code works whether the two sides are local, remote or one of each.
// Call only after installApks succeeded on dst: the install is what creates the target dir,
// assigns the uid and registers the package. The app is force-stopped on both sides first.
// /data/user/0 is /data/data on the guest; user_de and keystore-backed state do not move — apps
// holding hardware-backed keys re-prompt.
//
// TWO OF THE THREE HAZARDS THIS GUARDS AGAINST DO NOT EXIST ON A REMORA CONTAINER, measured
// and the code keeps them anyway because it stays correct for a real device:
//   - `--exclude=<pkg>/lib` never fires. All 162 /data/data dirs on the daily driver have no lib
//     entry; native libs live at /data/app/~~<hash>/<pkg>-<hash>/lib.
//   - `restorecon -RF` is a no-op. SELinux is DISABLED in the container (getenforce = Disabled,
//     /sys/fs/selinux empty, ls -Z prints the literal "HACKED" for every path) — note that
//     ro.boot.selinux still reads "enforcing", the same property-versus-reality gap the
//     magiskpolicy fix turned up. So "the copied contexts are wrong" cannot bite here.
// THE UID HAZARD IS REAL EVERYWHERE, and the stat-before-untar / chown-after ordering below is
// the only thing standing in front of it: the target's install assigns its own uid, and an untar
// restores the SOURCE's ownership over it.
bool transferAppData(Spawner &sp, const QString &src, const QString &dst, const RootExec &srcRoot,
                     const RootExec &dstRoot, const QString &pkg, const QString &stageDir,
                     QString *err = nullptr);

// Save `pkgs` to <dir>/<package>/, splits intact — with importApks a backup/restore that
// survives the instance itself.
AppOpCounts exportApks(Spawner &sp, const QString &src, const QStringList &pkgs,
                       const QString &dir, const LineSink &log = {});

// Install importJobs() onto dst; under dryRun only reports what each job would install.
AppOpCounts importApks(Spawner &sp, const QString &dst, const QList<ApkJob> &jobs, bool dryRun,
                       const LineSink &log = {});

// Carry the source profile's per-app window modes (appModeFor/setAppMode entries) for `pkgs`
// into dst's config — the one piece of per-profile app state a transfer CAN safely move (bd
// remora-4ei.73). An entry the target already has wins: that is the target user's own tuning,
// and the first-launch prompt stored it deliberately. Returns how many entries were carried;
// the caller saves dst if > 0.
int carryAppModes(const RemoraConfig &src, RemoraConfig &dst, const QStringList &pkgs);

}  // namespace remora
