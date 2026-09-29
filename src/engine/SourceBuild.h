#pragma once
#include <QMap>
#include <QProcess>
#include <QString>
#include <QStringList>

#include "core/SourcePatches.h"  // VendorReader
#include "engine/Engine.h"

namespace remora {

// Everything a source build needs that is not derivable here. Deliberately a plain struct of
// resolved values rather than a RemoraConfig: the GUI reads several of these off widgets that have
// no config key (the nice/jobs/memory spin boxes), and the CLI resolves them from remorarc, so the
// one thing both agree on is the resolved set.
struct SourceBuildPlan {
    QString tree;            // source tree, as seen ON THE BUILD HOST
    QString patchRoot;       // staged vendor/source-patches, on the build host
    QString sourceBuildDir;  // staged vendor/source-build, on the build host
    QStringList enabledPatches;
    bool lineage = true;     // false = stock AOSP flow (no repo bringup, no container patches)
    bool pinned = true;      // sync to the pinned manifest rather than the branch tip
    QString repoUrl, ref;
    QString builtTag;        // docker tag assemble-image.sh produces
    int androidVersion = 16;
    int buildNice = 15;
    int buildJobs = 0;       // 0 = let the builder decide
    int buildMemGiB = 0;     // 0 = derive from MemTotal inside the step
    int soongMemGiB = 0;     // 0 = derive from the cap inside the step
    bool withGapps = false, withMicrog = false, withNdk = false;
    bool withMagisk = false, withUpdater = false, withPlaySpoof = false;
    // The in-image mirror agent (the mirror_agent FEATURE, bd remora-28ix.3.4). A plan flag like
    // every other feature: the first cut tested the feature key against enabledPatches — a set
    // that can never contain it — so WITH_AGENT silently never fired and a full A17 build shipped
    // agent-less while every gate stayed green. Callers map it from the feature set they already
    // hold, the same way withGapps is mapped.
    bool withAgent = false;
    // Shamiko rides the play_spoof stack (it needs ReZygisk + the Magisk denylist) but is its
    // OWN flag, because it is the one piece you may want to drop while keeping PIF: bd
    // remora-4ei.16 named it a prime suspect for breaking Play Store verification on A16.
    bool withShamiko = false;
    // -> WITH_CAMERA for stage-features.sh, which builds AOSP's external (UVC/V4L2) camera
    // provider. The provider enumerates /dev/video* at RUNTIME, so the container also has to be
    // handed a node at docker run — the resolver's camera gate does that (bd remora-4ei.9).
    bool withCamera = false;
    // -> WITH_USB_AUDIO for stage-features.sh (bd remora-4ei.37). Builds AOSP's own tinyalsa USB
    // audio HAL plus a top-level audio policy that includes the usb module — the include is the
    // load-bearing half, because without it the HAL is installed and never opened and capture
    // keeps coming from the primary STUB, which memsets its buffer to zero on a faked clock. No
    // bringup counterpart, unlike withCamera: the container is privileged and already carries
    // every /dev/snd node.
    bool withUsbAudio = false;
    // -> WITH_MESA_SOURCE for stage-features.sh (bd remora-ykhz R3). Unlike every other feature
    // flag here, its payload is BUILT on this machine by vendor/host-prereqs/mesa-android/
    // build-mesa.sh rather than downloaded — Mesa has no soong build — so stage-features.sh gives
    // this one its own missing-payload message naming that script instead of fetch-payloads.sh.
    bool withMesaSource = false;
    // TARGET_ARCH_VARIANT to write into the device BoardConfig (both arches — every curated name
    // exists for x86_64 AND 32-bit x86 in this soong). Empty = leave the tree's generic x86-64
    // baseline untouched, byte-identical chain to before the option existed (bd remora-bm7.11).
    QString archVariant;
    // Patch key → requesting feature(s), from featureRequiredPatchOwners(). A conflict on one of
    // these fails the patch pass loudly instead of shipping the feature missing (bd remora-82c.1).
    QMap<QString, QString> requiredPatchOwners;
};

// The ordered shell steps of a full source build, to be joined with " && ".
//
// EXTRACTED FROM MainWindow SO THE CLI CAN RUN THE SAME BUILD (bd remora-25u/4ei.39/4ei.27/4ei.30
// all close on "one real build", and it used to take a GUI button click to get one). Duplicating
// the chain for a second caller was the alternative, and a duplicate would drift — this chain
// encodes hard-won ordering (reset before sync, container patches before ours, staging AFTER the
// reset so its makefile edits survive, the state epilogue recorded LAST so only a completed build
// marks the tree coherent). One definition, two callers.
//
// Pure apart from `readVendor`, which is injected for the same reason SourcePatches injects it: the
// fingerprints must read the LOCAL vendor tree even when the build host is remote.
QStringList sourceBuildSteps(const SourceBuildPlan &plan, const VendorReader &readVendor);

// Join steps into ONE shell command. Always use this rather than steps.join(" && "): each step is
// brace-grouped so that a bare top-level ';' ANYWHERE inside a step cannot split the AND-list.
// A plain join silently turns `a && b; c && d` into TWO and-lists -- everything after the ';' runs
// even when an earlier step failed, and the command's exit status is the LAST list's, so the chain
// reports success. That shipped an image missing container-compat patches (bd remora-h457 follow-up).
QString sourceBuildCommand(const QStringList &steps);

// Reads a vendor-relative path for the fingerprints. The concrete VendorReader over
// REMORA_VENDOR_DIR; pass it to sourceBuildSteps unless a test wants a fake tree.
QList<QPair<QString, QByteArray>> readVendorPath(const QString &relPath);

// The same walk rooted anywhere — the vendor-drift audit compares this binary's tree against its
// counterpart (installed vs source), and only the root differs between the two readers.
QList<QPair<QString, QByteArray>> readVendorTreeAt(const QString &vendorRoot,
                                                   const QString &relPath);

// The manifest URL a source build uses when the profile names none. Derived from the tree flavour
// rather than stored, because it is not a choice anyone makes independently of `source_kind`: the
// GUI used to offer a two-entry combo per kind whose first entry was always the right one. A
// profile that genuinely needs another manifest still sets source_repo= in remorarc, which wins.
QString defaultSourceRepo(bool lineage);

// `repo init`+`sync` to bring `tree` to the right manifest, guarded so it is safe to re-run.
QString repoBringupCmd(const QString &tree, const QString &repoUrl, const QString &ref, bool pinned,
                       const QString &sbuild, int androidVersion);

// Ensure a committer identity exists — `git am` refuses without one.
QString gitIdentityGuard();

// Single-quote a string for safe embedding in a shell command.
QString shellQuote(const QString &s);

// The pty wrapper argv every long-running streamed command uses: `script -qe -c` locally,
// `ssh -tt` remotely, so soong/ninja/repo/docker emit their progress lines. One definition for
// all four call sites because it now carries a correctness burden, not just a TTY trick: the
// command is wrapped to print the kPtyExitSentinel status line, and the caller must judge the run
// with judgePtyExit rather than the wrapper's exit code — a killed `script` exits 0 having killed
// the shell mid-build, which read as 'source build OK' (bd remora-c7y).
QStringList ptyRunArgv(const QString &cmd, const QString &sshHost);

// Force SHELL=/bin/sh on a QProcess. `script -c` hands the command to $SHELL, and this developer's
// login shell is fish, which rejects POSIX `VAR=value` outright — 'Unsupported use of =' — so the
// whole chain dies on its first assignment. Interactive runs hid it because the ambient SHELL was
// bash; it surfaced the moment the build ran as a systemd unit, which inherits the LOGIN shell.
void posixShellEnv(QProcess &p);

// Put the vendored source assets where the build host can read them, and return the patch root.
// Local is a no-op (the vendor tree is already there); remote scps source-build + source-patches.
// NB MainWindow keeps its OWN copy of this — deliberately, not by oversight. The GUI's runs on the
// main thread from the Pull-source path and must keep the event loop turning (bd remora-51z), which
// a plain Spawner cannot do; this one must not depend on Qt Widgets. Both are eight lines and do
// the same two commands. If either grows, merge them behind an injected wait.
QString stageSourceAssets(Spawner &sp, const QString &host);

// The docker tag a source build produces. Carries the platform version, because a build for one
// Android version landing in another's namespace is indistinguishable from the golden images, and
// "-src" so a source build never clobbers a prebuilt tag of the same features. A CPU arch variant
// is part of the tag for the same reason the version is: a broadwell build is a DIFFERENT image
// with different host requirements, and it must coexist with (and be publishable beside) the
// portable baseline rather than silently replace it.
QString sourceBuiltTag(int androidVersion, bool lineage, bool withGapps, bool withMicrog,
                       bool withWidevine, bool withHwc2, const QString &archVariant = {});

// The curated TARGET_ARCH_VARIANT choices the GUI offers (raw soong names; the combo stays
// editable for anything newer). Every entry exists for both x86_64 and 32-bit x86 in the A17
// tree's soong. Deliberately a subset: the meaningful host classes, not the whole go map.
QStringList knownArchVariants();

}  // namespace remora
