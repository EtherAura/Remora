#include "engine/SourceBuild.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QSet>

#include "core/Builders.h"
#include "core/Features.h"
#include "core/ImageBuild.h"
#include "core/Parsers.h"

#ifndef REMORA_VENDOR_DIR
#define REMORA_VENDOR_DIR "vendor"
#endif

namespace remora {


// The repo bring-up chain shared by Pull source and Build from source: init at the ref,
// select the manifest (pinned snapshot vs default + the remora local manifest), sync.
// Build runs it too, so a tree that was never pulled — or pulled unpinned — heals itself
// instead of patching the wrong base.
// The impure half of the patch fingerprints (core/SourcePatches.h explains what they are for and
// why they are split): read one vendor-relative file, or walk a vendor-relative directory, and hand
// back (vendor-relative path, content) pairs sorted by path.
//
// The LOCAL vendor tree, never the staged copy: on a remote build the staged path lives on the
// build host and is not readable here, but it is a copy of exactly this. Sorted because directory
// order is not stable while a fingerprint must be, and keyed on the RELATIVE path so a dev-tree
// build and an installed build of identical content agree instead of each forcing a full pass.
QList<QPair<QString, QByteArray>> readVendorTreeAt(const QString &vendorRoot,
                                                   const QString &relPath) {
    const QString root = vendorRoot + QLatin1Char('/') + relPath;
    QList<QPair<QString, QByteArray>> out;
    const auto add = [&out](const QString &key, const QString &path) {
        QFile fh(path);
        if (fh.open(QIODevice::ReadOnly)) out << qMakePair(key, fh.readAll());
    };
    const QFileInfo fi(root);
    if (fi.isFile()) {
        add(relPath, root);
        return out;
    }
    QStringList files;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) files << it.next();
    files.sort();
    for (const QString &f : files) add(relPath + f.mid(root.size()), f);
    return out;
}

QList<QPair<QString, QByteArray>> readVendorPath(const QString &relPath) {
    return readVendorTreeAt(QStringLiteral(REMORA_VENDOR_DIR), relPath);
}


QString defaultSourceRepo(bool lineage) {
    return lineage ? QStringLiteral("https://github.com/LineageOS/android.git")
                   : QStringLiteral("https://android.googlesource.com/platform/manifest");
}

QString repoBringupCmd(const QString &tree, const QString &repoUrl, const QString &ref,
                       bool pinned, const QString &sbuild, int androidVersion) {
    // Manifests are per-Android-version and MUST NOT cross over: the A16 pin is rooted at
    // lineage-23.0, so replaying it on a lineage-24.0 tree makes `repo sync --force-sync` drag
    // every project back to Android 16 — silently destroying an A17 checkout and its build
    // output. Both files are therefore version-suffixed, and a missing pin is a hard error
    // rather than a fallback, because falling back is precisely the downgrade (bd remora-p4p).
    const QString pinFile = QStringLiteral("pinned-manifest-a%1.xml").arg(androidVersion);
    // ONE GLOB, NO PER-VERSION ARM, the same rule the tag family follows: every release's local
    // manifests are named remora-a<N>*.xml, so the versioned pattern covers them all and nothing
    // needs a special case purely because of a filename.
    const QString localGlob = QStringLiteral("remora-a%1*.xml").arg(androidVersion);
    const QString manifestStep =
        pinned
            // A pinned manifest from `repo manifest -r` is a COMPLETE snapshot — it already
            // contains the projects the local manifests add — so every local manifest has to go
            // or repo sees the same project defined twice.
            // THE cmp AFTER `repo init` IS A TRIPWIRE, NOT A TAUTOLOGY (bd remora-mdq2). It looks
            // like it can only ever pass — the `cp` two commands earlier put that exact file there
            // and it would have failed: an A17 build ran with the A16 pin installed
            // as .repo/manifests/remora-pinned.xml (byte-identical to pinned-manifest-a16.xml, and
            // `repo manifest` confirmed repo was really using it), and the sync dragged 23 projects
            // back to lineage-23.0 revisions. It did NOT reproduce on the next run, so the cause is
            // still open; this check does not depend on knowing it.
            //
            // PLACED AFTER `repo init` DELIBERATELY. Before it, the check really would be a
            // tautology against the cp. `repo init` fetches and checks out the manifests repo, so
            // it is the one step between the cp and the sync that could replace the file — and it
            // is the last moment the damage is still cheap.
            //
            // WHY IT REFUSES RATHER THAN WARNS. The failure is not loud: a wrong pin downgrades a
            // multi-hour checkout in place and then surfaces, minutes later, as an unrelated
            // missing-file error from kati ("build/make/core/soong_system_modules.mk: No such file
            // or directory") with nothing pointing at the manifest. Nobody reads a warning that
            // scrolls past 1229 sync lines. This is the same reasoning as the missing-pin check
            // above, which is a hard error for the same reason — falling back IS the downgrade.
            //
            // Complements repoSyncVerifyCommand() rather than duplicating it: that one checks the
            // OUTCOME of a sync against the manifest, this one checks the manifest itself before a
            // single project moves. The verify cannot tell a correctly-executed sync of the wrong
            // manifest from a correct one — every project would match the pin it was handed.
            //
            // Only on the pinned arm: the unpinned arm installs local manifests over default.xml
            // and has no version-suffixed snapshot to compare against.
            ? QStringLiteral("{ [ -f %1/%2 ] || { echo \"no pinned manifest for Android %3 "
                             "(%1/%2) — regenerate it with 'repo manifest -r -o' from a freshly "
                             "synced tree BEFORE any patch pass (taken after it, the pin names "
                             "local patch commits no remote has), or untick 'Pin to Remora's "
                             "tested manifest'\"; exit 1; }; } "
                             "&& cp %1/%2 .repo/manifests/remora-pinned.xml && "
                             "rm -f .repo/local_manifests/*.xml && "
                             "repo init -m remora-pinned.xml && "
                             "{ cmp -s %1/%2 .repo/manifests/remora-pinned.xml || { "
                             "echo \"PINNED MANIFEST CLOBBERED — refusing to sync (bd "
                             "remora-mdq2).\"; "
                             "echo \"  .repo/manifests/remora-pinned.xml does not match %1/%2 "
                             "after repo init.\"; "
                             "echo \"  Syncing now would check this tree out against the wrong "
                             "Android version: the A16 pin is\"; "
                             "echo \"  rooted at lineage-23.0, so --force-sync silently drags "
                             "every project back and the build then\"; "
                             "echo \"  fails somewhere unrelated (bd remora-p4p).\"; "
                             "echo \"  Restore: cp %1/%2 .repo/manifests/remora-pinned.xml && "
                             "repo init -m remora-pinned.xml\"; "
                             // The tripwire passing is the moment the tree is PROVEN committed to
                             // this version's pin — the only honest place to stamp its identity
                             // (bd remora-mdq2 item 3; the guard that reads the stamp is at the
                             // head of sourceBuildSteps, and it deliberately never stamps from
                             // the plan alone).
                             "exit 1; }; } && echo %3 > .remora-android-version && ")
                  .arg(sbuild, pinFile)
                  .arg(androidVersion)
            : QStringLiteral("repo init -m default.xml && "
                             "mkdir -p .repo/local_manifests && "
                             "rm -f .repo/local_manifests/*.xml && "
                             "cp %1/local_manifests/%2 .repo/local_manifests/ && "
                             // Same identity stamp as the pinned arm: the version-suffixed local
                             // manifests are installed, so the tree is committed to this version.
                             "echo %3 > .remora-android-version && ")
                  .arg(sbuild, localGlob)
                  .arg(androidVersion);
    // repo sync doesn't run the git-lfs smudge filter, so LFS-backed prebuilts (e.g. the
    // chromium webview APKs) land as 134-byte pointer files and the build dies at aapt2. Pull
    // LFS content across all projects after sync (no-op where there's nothing to fetch).
    // --fail-fast plus the HEAD-vs-manifest verify carry bd remora-4ei.81: builds 5 and 6 of
    // had this sync print "Checking out local projects failed", exit 0, and hand a
    // half-synced tree to a container-compat pass that then applied nothing — the exit code
    // alone cannot gate the build, so the outcome is checked against the manifest itself.
    return QStringLiteral("mkdir -p %1 && cd %1 && repo init -u %2%3 && %4"
                          "repo sync -c -j8 --fail-fast --force-sync --verbose && %5 && "
                          "repo forall -c 'git lfs pull 2>/dev/null || true'")
        .arg(tree, repoUrl, ref.isEmpty() ? QString() : QStringLiteral(" -b ") + ref,
             manifestStep, repoSyncVerifyCommand());
}


// The pull runs on a pseudo-TTY (for repo's % progress), which makes 'repo init' interactive:
// it crashes without a committer identity and blocks on a color-display prompt when color.ui
// is unset. git am needs the identity too. Supply neutral defaults on the source host only
// where nothing is set — a user's real config always wins. Prepended to pull/apply/build.
// ONE brace group, ending in `true`. Callers join steps with " && ", and the bare top-level ';'
// this used to have SPLIT that chain: `preflight && identity1; identity2 && sync && …` is two
// AND-lists, so everything after the ';' ran even when an earlier step had failed. The `true` keeps
// the best-effort semantics — a host where these writes fail (read-only HOME) should not lose its
// build over a colour setting.
QString gitIdentityGuard() {
    return QStringLiteral(
        "{ git config --global user.name >/dev/null 2>&1 || "
        "git config --global user.name Remora; "
        "git config --global user.email >/dev/null 2>&1 || "
        "git config --global user.email remora@localhost; "
        "git config --global color.ui >/dev/null 2>&1 || "
        "git config --global color.ui auto; true; }");
}

QString shellQuote(const QString &s) {
    QString q = s;
    q.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QStringList ptyRunArgv(const QString &cmd, const QString &sshHost) {
    // `;` not `&&`: the sentinel must print for a FAILED chain too — that is a real verdict, and
    // judgePtyExit reads it as one. `exit $rc` keeps script -e / ssh returning what they did
    // before the sentinel existed, so nothing downstream of the exit code changes on the paths
    // that were already honest.
    const QString wrapped = cmd
                            + QStringLiteral("; rc=$?; echo \"%1$rc\"; exit $rc")
                                  .arg(QLatin1String(kPtyExitSentinel));
    if (sshHost.isEmpty())
        return {QStringLiteral("script"), QStringLiteral("-q"), QStringLiteral("-e"),
                QStringLiteral("-c"), wrapped, QStringLiteral("/dev/null")};
    // remote: hand the command to a POSIX sh explicitly — the login shell may be fish
    return {QStringLiteral("ssh"), QStringLiteral("-tt"), sshHost,
            QStringLiteral("sh -c ") + shellQuote(wrapped)};
}

void posixShellEnv(QProcess &p) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SHELL"), QStringLiteral("/bin/sh"));
    p.setProcessEnvironment(env);
}

QString stageSourceAssets(Spawner &sp, const QString &host) {
    const QString localVendor = QStringLiteral(REMORA_VENDOR_DIR);
    if (host.isEmpty()) return localVendor + QStringLiteral("/source-patches");
    const QString remoteRoot = QStringLiteral("~/.cache/remora/source-assets");
    sp.run(Spawner::sshArgv(host, QStringLiteral("mkdir -p %1").arg(remoteRoot)), {}, {});
    sp.run({QStringLiteral("scp"), QStringLiteral("-r"), QStringLiteral("-q"),
            localVendor + QStringLiteral("/source-build"),
            localVendor + QStringLiteral("/source-patches"),
            host + QStringLiteral(":") + remoteRoot + QStringLiteral("/")}, {}, {});
    return remoteRoot + QStringLiteral("/source-patches");
}

QStringList knownArchVariants() {
    // Raw soong names, portable-first. broadwell is the x86-64-v3 sweet spot (AVX2/FMA/BMI2/ADX;
    // Intel 2014+ and every AMD Zen). alderlake and newer add VAES/GFNI, which pre-Zen4 AMD lacks
    // — a wrong pick SIGILLs at runtime, so the GUI tooltip carries the warning, not just this
    // comment. The small-core names (silvermont/goldmont/tremont) exist for Atom-class boxes.
    return {QStringLiteral("broadwell"),  QStringLiteral("haswell"),
            QStringLiteral("skylake"),    QStringLiteral("alderlake"),
            QStringLiteral("silvermont"), QStringLiteral("goldmont"),
            QStringLiteral("tremont")};
}

QString sourceBuiltTag(int androidVersion, bool lineage, bool withGapps, bool withMicrog,
                       bool withWidevine, bool withHwc2, const QString &archVariant) {
    QString variant;
    if (withGapps) variant += QStringLiteral("-gapps");
    if (withMicrog) variant += QStringLiteral("-microg");
    if (withWidevine) variant += QStringLiteral("-wv");
    if (withHwc2) variant += QStringLiteral("-hwc2");
    // CPU tuning last, right before -src: "…-hwc2-broadwell-src" promotes to "…-hwc2-broadwell",
    // a parallel family to the portable "…-hwc2" rather than a replacement for it.
    if (!archVariant.isEmpty()) variant += QLatin1Char('-') + archVariant;
    // ONE FORMULA, NO PER-VERSION ARM. Every Lineage release is remora<android + 7>:* (Lineage
    // majors run android + 7, so 16 -> 23 and 17 -> 24), which is how the pre-built contracts in
    // Features.cpp spell it, and Promote — which just drops "-src" — only lines up if the built
    // tag is exactly one of those plus "-src". A per-version special case here is how a build and
    // its promoted name drift apart, so there is none to keep in step.
    const QString family = lineage ? QStringLiteral("remora%1").arg(androidVersion + 7)
                                   : QStringLiteral("aosp-remora%1").arg(androidVersion);
    return QStringLiteral("%1:x86_64%2-src").arg(family, variant);
}


QString sourceBuildCommand(const QStringList &steps) {
    // Why brace groups and not `set -e`: POSIX suppresses errexit for a command on the LEFT of
    // `&&`, which is every step but the last, so `set -e` does not abort this chain at all --
    // verified, not assumed. Containment is what actually works.
    QStringList wrapped;
    for (const QString &s : steps) wrapped << QStringLiteral("{ %1; }").arg(s);
    return wrapped.join(QStringLiteral(" && "));
}

QStringList sourceBuildSteps(const SourceBuildPlan &plan, const VendorReader &readVendorPath) {
    // Bound to the names the chain was written against, so the step text below is byte-identical to
    // what MainWindow emitted before it moved here. A rename would be a silent behaviour change in
    // 380 lines of shell.
    const QString &tree = plan.tree, &patchRoot = plan.patchRoot, &sbuild = plan.sourceBuildDir;
    const QStringList &enabled = plan.enabledPatches;
    const bool lineage = plan.lineage, pinned = plan.pinned;
    const QString &repoUrl = plan.repoUrl, &ref = plan.ref, &builtTag = plan.builtTag;
    const int androidVer = plan.androidVersion, buildNice = plan.buildNice,
              buildJobs = plan.buildJobs, buildMemGiB = plan.buildMemGiB,
              soongMemGiB = plan.soongMemGiB;
    const bool withGapps = plan.withGapps, withMicrog = plan.withMicrog, withNdk = plan.withNdk,
               withMagisk = plan.withMagisk, withUpdater = plan.withUpdater,
               withPlaySpoof = plan.withPlaySpoof,
               withShamiko = plan.withShamiko, withCamera = plan.withCamera,
               withUsbAudio = plan.withUsbAudio,
               withMesaSource = plan.withMesaSource;

    const QMap<QString, QString> marks = patchProjectMarks(tree);
    const QString repoFp =
        repoStateFingerprint(repoUrl, ref, pinned, androidVer, readVendorPath);
    const QMap<QString, QString> projectFp =
        patchProjectFingerprints(enabled, androidVer, readVendorPath);
    const PatchStateFiles psf = patchStateFiles(tree);

    QStringList steps = buildApplyPatchCommands(enabled, tree, patchRoot, androidVer, marks,
                                                plan.requiredPatchOwners);
    // Everything that has to land BEFORE the source patches, in final order: the tree-version
    // guard first (refuse before anything at all is touched), then the reset pass.
    QStringList head;
    // THE TREE DECLARES ITS ANDROID VERSION, AND A DISAGREEING BUILD IS REFUSED FIRST
    // (bd remora-mdq2 item 3). The incident put the A16 pin into the A17 tree and the
    // failure surfaced as a kati missing-file error naming nothing manifest-shaped; the cmp
    // tripwire in repoBringupCmd now catches a clobbered PIN, but nothing tied the TREE to a
    // version — any A16-configured operation pointed at the A17 tree's path was free to try.
    // The marker is stamped only on PROOF, never on first touch: repoBringupCmd writes it after
    // the cmp tripwire passes (the tree is committed to that version's pin), and the adoption arm
    // below writes it only from evidence carried by the tree itself. Stamping an unmarked tree
    // from plan.androidVersion alone would let the first wrong-version build brand the tree wrong
    // — adopting the bug as identity. When the installed pin belongs to a DIFFERENT version the
    // refusal names it; an unpinned or empty tree stays unmarked and the bring-up settles it.
    // Unconditional (not behind the skipMark): incremental builds are exactly where a
    // wrong-version invocation would otherwise sail through, because they skip the bring-up and
    // its tripwire entirely. Both lineage arms, not just pinned: the marker-vs-plan half needs no
    // pin file, and an unpinned bring-up stamps the marker too — its local manifests are just as
    // version-suffixed. On an unpinned tree the adoption arms below no-op (no installed pin to
    // read), and the bring-up settles the identity.
    //
    // THE EVIDENCE IS THE PIN'S <default> REVISION, NOT THE PIN'S BYTES. The first cut compared
    // the installed pin byte-for-byte against pinned-manifest-a<N>.xml, and that silently
    // un-protected the very tree this guard was written for. Measured: /mnt/Build/
    // remora-a17's installed pin matched NO snapshot, because a manifest cleanup rewrote comments
    // and renamed the `local::*` project groups — 17 bytes of pure cosmetics, zero revision
    // changes. An unmarked tree matching no snapshot falls through every arm, so an A16-configured
    // build pointed at the A17 tree PASSED. That is the incident verbatim, re-opened by
    // a rename. Byte-equality is the wrong test because it also breaks on a re-pin within the same
    // version, which is a routine thing to do.
    //
    // repo's `<default revision="refs/heads/lineage-NN.0">` is the release identity instead: it is
    // what makes the manifest an Android-16 or Android-17 pin at all, it survives comment and
    // group edits, and it survives re-pinning to newer upstream revisions. An unrecognised or
    // absent branch falls to the `*)` arm and passes unmarked — the same fail-open as before, and
    // stated rather than accidental.
    if (lineage && !repoUrl.isEmpty()) {
        const QString thisBranch = lineageBranchForAndroid(androidVer);
        QString arms;
        if (!thisBranch.isEmpty()) {
            arms += QStringLiteral("%1) echo %2 > \"$vf\" ;; ").arg(thisBranch).arg(androidVer);
        }
        // Only versions Remora can actually source-build get a refusal arm, because only those
        // have a pinned-manifest-a<N>.xml for the message to name — and only those can be the
        // legitimate identity of a Remora tree in the first place. A branch from the named-but-
        // not-built rows (8-15) is unrecognised here and takes the `*)` fail-open, which is the
        // honest answer: Remora did not create that tree and has nothing to say about it.
        QSet<QString> seen{thisBranch};
        for (int v : supportedAndroidVersions()) {
            const AndroidRelease *r = androidRelease(v);
            if (!r || r->releaseConfig.isEmpty()) continue;
            const QString b = lineageBranchForAndroid(v);
            if (b.isEmpty() || seen.contains(b)) continue;
            seen.insert(b);
            arms += QStringLiteral(
                        "%1) echo \"FATAL: tree %2 carries an Android %3 pin (its <default> "
                        "revision is refs/heads/%1, i.e. pinned-manifest-a%3.xml) but this build "
                        "is for Android %4 — Remora trees are per-version (bd remora-mdq2). Point "
                        "source_tree at the Android-%4 tree\" >&2; exit 1 ;; ")
                        .arg(b, tree)
                        .arg(v)
                        .arg(androidVer);
        }
        arms += QStringLiteral("*) : ;; ");
        head << QStringLiteral(
                    "vf=%1/.remora-android-version; pin=%1/.repo/manifests/remora-pinned.xml; "
                    "if [ -f \"$vf\" ]; then v=$(cat \"$vf\"); [ \"$v\" = \"%2\" ] || { "
                    "echo \"FATAL: tree %1 is marked Android $v but this build is for Android %2 "
                    "— Remora trees are per-version (bd remora-mdq2). Point source_tree at the "
                    "Android-%2 tree, or delete $vf only if this tree is really being "
                    "re-initialised\" >&2; exit 1; }; "
                    "elif [ -f \"$pin\" ]; then tb=$(sed -n 's|.*<default[^>]*"
                    "revision=\"refs/heads/\\([^\"]*\\)\".*|\\1|p' \"$pin\" | head -1); "
                    "case \"$tb\" in %3esac; fi; true")
                    .arg(tree, QString::number(androidVer), arms);
    }
    // Conflicted patch projects must be reset ahead of the sync — an unmerged index blocks
    // repo's checkout — so the reset pass comes next.
    head << buildResetCommands(enabled, tree, marks);
    // self-healing: sync the tree to the right manifest before touching it — a tree that was
    // never pulled (or pulled unpinned) is brought to the patch base, not patched wrong. Skipped
    // when the sync inputs are unchanged, in favour of the targeted sync behind it.
    if (lineage && !repoUrl.isEmpty()) {
        head << QStringLiteral("[ -f %1 ] || { %2 ; }")
                    .arg(psf.skipMark, repoBringupCmd(tree, repoUrl, ref, pinned, sbuild,
                                                      androidVer));
        head << buildTargetedSyncCommand(tree);
    }
    // The container-compat patches (upstream's plus ours, version-pinned) go on before the rest —
    // AOSP core (init/frameworks) so the built image boots as a container. Without them a stock
    // init exits in ~90ms as PID1. The apply is always onto a pristine project: either the global
    // sync just ran, or the command restricts itself to the projects the targeted sync reset.
    if (lineage) {
        head << buildContainerPatchCommand(tree, sbuild, androidVer);
    } else {
        // AOSP-kind is a BRING-YOUR-OWN-TREE path (bd remora-82c.4). It deliberately skips both
        // Remora bringup steps above — the pinned/local manifest sync AND the container-compat
        // overlay — because it points at a tree the operator has prepared and synced themselves.
        // That is a real choice, but it drops the version safety the lineage path gets for free,
        // and this states both consequences at the gate rather than leaving them to be discovered
        // in a dead build.
        //
        // VERSION. The lineage path refuses an unknown Android version three ways (the pinned
        // manifest, the container patch set, both hard-fail). AOSP-kind reaches none of them, so
        // it would otherwise build whatever ANDROID_VER it was handed and die deep in the tree
        // with an opaque lunch error. Guard it on the one thing Remora must know to lunch the tree
        // at all — a release config — so an unsupported version fails HERE, named. Prepended so it
        // lands ahead of everything expensive, the same reason the preflight is.
        if (releaseConfigFor(androidVer).isEmpty())
            head.prepend(
                QStringLiteral(
                    "{ echo \"source_kind=aosp: no release config for Android %1 — Remora cannot "
                    "lunch this version (known: 16, 17). Build a supported version, or add it to "
                    "releaseConfigFor()\"; exit 1; }")
                    .arg(androidVer));
        // CONTAINER-COMPAT. Those patches are what make the image boot as a container instead of
        // exiting as PID1. AOSP-kind does not apply them, so the operator's tree MUST already
        // carry them — said in the build log rather than shipping a non-booting image green.
        head << QStringLiteral(
            "echo \"NOTE (source_kind=aosp): Remora applied no manifest sync and no "
            "container-compat patches — this tree must already be a bootable container tree "
            "(bd remora-82c.4)\"");
    }
    // device/remora, Remora's own device tree, goes in LAST in head — i.e. immediately before the
    // patch pass that follows (bd remora-28ix.4). It cannot ride with the other staged assets in
    // stage-features.sh: that runs AFTER patching (deliberately, so its mk lines survive the
    // reset), and a device tree landing there would overwrite everything the patches just did.
    // The script is idempotent and clears this project's patch marks whenever it re-stages; see
    // its header for the three ways this goes silently wrong.
    head << QStringLiteral("sh %1/stage-device-tree.sh %2 %1/device-remora").arg(sbuild, tree);
    steps = head + steps;
    // THE BACKPORT'S RETIREMENT WATCH, RUN BY THE BUILD INSTEAD OF BY A PERSON (bd remora-cbd.2).
    // The backport exists only because LineageOS cut lineage-24.0 as a pure AOSP-tracking branch
    // and have not forward-ported their platform work; the single most valuable thing to know
    // about it is WHEN IT CAN BE DELETED. That was a standing instruction on a tracker bead —
    // "after every repo sync, run check-lineage-backport.sh" — and a standing instruction is a
    // recurring obligation nobody is reminded of, which is exactly how its own record drifted:
    // the bead still described 98 patches long after triage had cut the series to 70, and the
    // note was stale the same way before that.
    //
    // So the build carries it. ~12 s, one shallow fetch, and the script is already read-only and
    // failure-tolerant by design. ADVISORY, NEVER FATAL: the verdict is about UPSTREAM's state,
    // not this build's correctness, and a network hiccup must not fail a multi-hour build — hence
    // the `|| true` and the tolerance inside the script itself.
    //
    // Gated on the series actually being enabled, so a profile that does not carry the backport
    // neither pays the 12 s nor reads advice about something it does not ship.
    //
    // WHAT THE BUILD CANNOT ANSWER, and the reason this is a watch rather than a gate: the script
    // reports retirement, upstream drift and base drift — it does NOT test whether the series
    // still applies. Conflicts show up only in the patch pass above ("WARN backport patch
    // SKIPPED"), which is why both halves have to be read. Measured: the script gave a
    // completely clean bill while two patches were in fact still conflicting on every build.
    if (lineage && enabled.contains(QStringLiteral("lineage_platform_backport"))) {
        steps << QStringLiteral(
                     "echo '=== LineageOS backport: retirement watch (bd remora-cbd.2) ==='; "
                     "out=$(sh %1/check-lineage-backport.sh %2 2>&1) || true; echo \"$out\"; "
                     "case \"$out\" in *'=> RETIRE'*) echo 'BACKPORT RETIREMENT CONDITION MET: "
                     "LineageOS have forward-ported to the branch this builds. DELETE the backport "
                     "rather than maintain it — it is a temporary measure and this is the signal "
                     "it was waiting for (bd remora-cbd.2).' ;; esac; true")
                     .arg(sbuild, tree);
    }
    // AOSP-kind passes no container projects: it runs no Remora sync and no container apply, so
    // there is nothing to act on a HEAD-drift verdict — flagging drift it cannot fix would only
    // re-warn forever about a tree the operator owns (bd remora-82c.4).
    steps.prepend(buildPatchStatePrologue(
        tree, repoFp, projectFp,
        lineage ? containerPatchProjects(androidVer, readVendorPath) : QStringList()));
    steps.prepend(gitIdentityGuard());  // git am needs a committer identity

    // BUILD PREFLIGHT. Prepended LAST so it lands FIRST: every other step here prepends too,
    // and on the initial cut this sat behind the repo sync, firing only after a full sync had
    // run. A preflight that runs after the expensive part is not a preflight.
    //
    // WHAT ACTUALLY KILLS BUILDS HERE: systemd-oomd. Three A17 builds died with exit 137
    // and were misdiagnosed for most of a day as memory exhaustion, a failing NVMe,
    // and a THP regression in turn. The journal had it all along:
    //   systemd-oomd: Marked /system.slice/docker-8521....scope for killing due to memory used
    //   (60776927232) / total (66940563456) and swap used (61069553664) / total (67830013952)
    //   being more than 90.00%
    // oomd is a USERSPACE killer driven by PSI. It logs to the JOURNAL, NOT to dmesg, so
    // `dmesg | grep oom-kill` reads zero while it is killing your builds — that false negative
    // is what sent the whole investigation sideways. Its criterion is used/total >= 90% for RAM
    // AND swap together, so on a box with a large swap it trips on SWAP FILL over a long build,
    // independent of free RAM and independent of -j. Capping 'jobs' cannot fix it; it only
    // changes how fast swap fills. The fix is oomd configuration (SwapUsedLimit, or
    // ManagedOOMPreference on the slice), which is why that is what this warns about.
    //
    // THE MEMORY CEILINGS THAT USED TO REFUSE HERE ARE GONE — see the block below for why.
    // The oomd check above is the calibrated part and stays: its criterion is read from the
    // running config, and it was verified live (warns at SwapUsedLimit=90, silent at 98).
    // What remains true from that work: soong_build is a SINGLE process, so 'jobs' cannot
    // bound the analysis phase at all — which is why the answer is a cap on the process
    // (SOONG_GOMEMLIMIT) and on the highmem job class, not a smaller -j.
    //
    // BEWARE THE STALL NUMBERS. siso logs "memory stall 4m58s/s" from PSI-memory, which counts
    // time blocked in reclaim AND writeback. A disk that stops acknowledging flushes sends it
    // vertical while free RAM is untouched — measured here at 18 GiB free and RISING three
    // seconds before an NVMe flush timeout that forced the tree read-only. And oomd kills on
    // that same PSI signal, so a high stall reading means "something is about to be killed",
    // not "memory is short". Check the JOURNAL for oomd, MemAvailable, and the kernel log —
    // in that order. See bd remora-4ei.
    steps.prepend(QStringLiteral(
        // --- the real hazard first: is a userspace PSI killer armed against this build? ---
        "if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet systemd-oomd "
        "2>/dev/null; then "
        "LIM=$(systemd-analyze cat-config systemd/oomd.conf 2>/dev/null | "
        "grep -oE '^SwapUsedLimit=[0-9]+' | tail -1 | cut -d= -f2); [ -n \"$LIM\" ] || LIM=90; "
        "SWU=$(awk '/^SwapTotal:/{t=$2}/^SwapFree:/{f=$2}END{if(t>0) print int((t-f)*100/t); "
        "else print 0}' /proc/meminfo 2>/dev/null); "
        "if [ \"${LIM:-90}\" -le 90 ]; then "
        "echo \"build preflight WARNING: systemd-oomd is active with SwapUsedLimit=${LIM}% and "
        "swap is ${SWU}% used. oomd kills on used/total >= the limit for RAM AND swap TOGETHER, "
        "it logs to the journal and NOT to dmesg, and it has killed builds on this host. A long "
        "build fills swap regardless of 'jobs', so lowering jobs will NOT prevent it.\"; "
        "echo \"  fix: raise SwapUsedLimit in /etc/systemd/oomd.conf, or set "
        "ManagedOOMPreference=avoid on system.slice. If a build dies with exit 137, check "
        "'journalctl -u systemd-oomd' BEFORE believing dmesg.\"; fi; fi; "
        // --- then the memory numbers, AS DIAGNOSTICS ONLY ---
        // The jobs*2 GiB ceiling and the 24 GiB analysis floor that used to REFUSE here are
        // retired (bd remora-4ei.19). Final record: three false refusals, zero prevented
        // failures. Retired rather than retuned, because both numbers are now known wrong and
        // wrong in opposite directions:
        //   - The floor was 24 GiB. soong_build was OOM-killed at 35.8 and 37.8 GiB on two
        //     separate runs, so the floor passed both builds that actually died. And the 19.4
        //     GiB it was derived from is a Go LIVE-HEAP PLATEAU, not a peak: with no GOGC set
        //     the heap grows to ~2x the live set, so the real working peak is ~38.8 GiB.
        //   - jobs*2 assumed every job peaks at once and that jobs are alike. Measured on the
        //     compile phase once it finally ran: javac/R8/d8 hold 3.4-4.0 GiB EACH while
        //     clang steps hold 0.08-0.11 GiB. The distribution is bimodal by ~40x, so a single
        //     per-job constant cannot model it and its mean means nothing.
        // The premise is obsolete too. This existed because nothing bounded the build; the
        // build now runs under a cgroup cap (bd remora-4ei.27) with Soong's highmem pool sized
        // from that cap (bd remora-4ei.30), which bounds the heavy job class specifically —
        // exactly what a flat -j figure could not do. A refusal here would now only block
        // builds the cap would have contained.
        // The numbers are still PRINTED: seeing RAM and swap before a multi-hour run is worth
        // something. Asserting a verdict from them is not.
        // --- a running libvirt guest may not fit alongside an A17 build (bd remora-0n4) ---
        // A guest with a VFIO device has its RAM pinned and unswappable, so it is worth more than
        // its size. MEASURED on a 62 GiB host, sampling RSS every 10s through real
        // builds:
        //   soong_build A17 analysis peaks ~37 GiB and PLATEAUS there (bd remora-4ei.27)
        //   VM up   -> ~38 GiB MemAvailable: the build was OOM-killed at 29.8 GiB against a 30 cap
        //   VM down -> ~47 GiB MemAvailable: the same build completed
        // WARNS, DOES NOT REFUSE. The container cap and its own slice already stop a build taking
        // the host or the guest down — this bead's original failure (the VM dying) is contained by
        // construction now; what is left is that the BUILD will die, which is cheap and obvious.
        // A refusal here would repeat bd remora-4ei.19, whose record was three false refusals and
        // zero prevented failures.
        "if command -v virsh >/dev/null 2>&1; then "
        "VMS=$(virsh -c qemu:///system list --name 2>/dev/null | tr -d ' ' | grep -v '^$' | "
        "tr '\\n' ' '); "
        "if [ -n \"$VMS\" ]; then "
        "echo \"build preflight WARNING: libvirt guest(s) running: ${VMS}— a guest with a "
        "passthrough (VFIO) device has all of its RAM pinned and unswappable, so it cannot be "
        "reclaimed for the build.\"; "
        "echo \"  A17 Soong analysis needs ~37 GiB resident and plateaus there. If the host "
        "cannot spare that beside the guest, shut the guest down for a source build.\"; fi; fi; "
        // --- the tree must be WRITABLE, proven by writing (bd remora-4ei.20) ---
        // btrfs force-latches read-only after a failed write barrier and stays that way until a
        // full unmount+remount — `mount -o remount,rw` will not clear it. A build started against a
        // latched tree dies deep inside soong with a generic exit 1 and the reason buried in
        // out/verbose.log.gz. This REFUSES rather than warns, unlike the memory checks: a failed
        // write is a determined fact about right now, not a prediction, so it cannot false-positive
        // the way remora-4ei.19's thresholds did. Tests the TREE, not $TMPDIR — a read-only /src
        // with a writable /tmp is exactly the observed failure.
        "T=%2/.remora-write-test; if ! (: > \"$T\") 2>/dev/null; then "
        "echo \"build preflight: %2 IS NOT WRITABLE. If this is btrfs it has probably latched "
        "read-only after a failed write barrier (a single NVMe flush timeout is enough, and is NOT "
        "a device failure — check 'btrfs device stats' for wr/rd/corrupt before suspecting the "
        "drive). Remounting rw will not clear it: unmount fully and mount again, closing whatever "
        "holds the mount first. See bd remora-4ei.20.\"; exit 1; fi; rm -f \"$T\"; "
        "J=%1; [ \"$J\" -gt 0 ] || J=$(nproc); "
        "AVAIL=$(awk '/^MemAvailable:/{print int($2/1048576)}' /proc/meminfo 2>/dev/null); "
        "SWAP=$(awk '/^SwapFree:/{print int($2/1048576)}' /proc/meminfo 2>/dev/null); "
        "[ -n \"$AVAIL\" ] || AVAIL=0; [ -n \"$SWAP\" ] || SWAP=0; "
        "echo \"build preflight: ${AVAIL} GiB RAM available, ${SWAP} GiB swap free, $J jobs. "
        "Memory is bounded by the container cap and by Soong's highmem pool, not by this "
        "check — it no longer refuses (three false refusals, zero catches: bd remora-4ei.19).\"")
                      .arg(buildJobs)
                      .arg(tree));
    // do-build.sh runs INSIDE the containerized build env (tree mounted at /src) — the
    // vendored Dockerfile recreates the builder image on any machine when it's missing
    // stage the à-la-carte prebuilt features (gapps/ndk/magisk) into the tree + wire the
    // product makefile — AFTER the patch reset, so the inheritance lines survive
    {
        QString fenv;
        if (withGapps) fenv += QStringLiteral("WITH_GAPPS=true ");
        if (withMicrog) fenv += QStringLiteral("WITH_MICROG=true ");
        if (withNdk) fenv += QStringLiteral("WITH_NDK=true ");
        if (withMagisk) fenv += QStringLiteral("WITH_MAGISK=true ");
        if (withUpdater) fenv += QStringLiteral("WITH_UPDATER=true ");
        if (withCamera) fenv += QStringLiteral("WITH_CAMERA=true ");
        if (withUsbAudio) fenv += QStringLiteral("WITH_USB_AUDIO=true ");
        if (withMesaSource) fenv += QStringLiteral("WITH_MESA_SOURCE=true ");
        // Not a prebuilt feature — driven by a SOURCE PATCH. The patch adds the soong modules for
        // the gbm_mesa/vtest gralloc; this makes stage-features.sh add the PRODUCT_PACKAGES line
        // without which nothing references them and soong never builds them (bd remora-4ei.56).
        if (enabled.contains(QStringLiteral("minigbm_gbm_mesa_vtest")))
            fenv += QStringLiteral("WITH_GBM_MESA_GRALLOC=true ");
        // The in-image mirror agent (bd remora-28ix.3.4): the staging overlays agent/src into
        // the tree, so the image bakes exactly the code the bind-mount dev loop runs. From the
        // PLAN flag, not from `enabled` — that is enabledPatches, mirror_agent is a feature, and
        // testing the wrong set is how the first validation build shipped agent-less with every
        // gate green (nothing asserted WITH_AGENT reached the environment; now a test does).
        if (plan.withAgent) fenv += QStringLiteral("WITH_AGENT=true ");
        // UNCONDITIONAL, even with no prebuilt feature ticked (bd remora-4ei.42). This script
        // does two things that have nothing to do with the à-la-carte features: it stages
        // hardware/remora/c2-va, which is in no repo manifest and therefore exists ONLY
        // because this copies it in, and it appends the xe VF gralloc line without which
        // SurfaceFlinger dies on the VF. Gating the whole script on `fenv` non-empty meant a
        // profile that ticked none of GAPPS/MICROG/NDK/MAGISK/UPDATER silently built a c2-va
        // reconstructed from the patch series instead — and that series is 1449 lines of HEVC
        // decoder and ~290 further lines behind the vendored tree. Every feature stanza inside
        // is `[ "$WITH_X" = true ] && stage …`, so an empty env is a clean no-op for them.
        steps << QStringLiteral("%1sh %2/stage-features.sh %3 %2/features")
                     .arg(fenv, sbuild, tree);
    }
    // CPU arch variant (bd remora-bm7.11): write the requested -march level into the device
    // BoardConfig — AFTER the patch reset, like the staging edits, so it survives to the build.
    // The anchor grep HARD-FAILS when the line is missing: a silently unapplied variant would
    // ship a baseline image under a CPU-tuned tag, the remora-fgj.11 failure shape. Both arches
    // get the same name; every curated choice exists for x86_64 and 32-bit x86 in this soong,
    // and an unknown name fails fast in soong's config parse rather than 40 minutes in.
    if (!plan.archVariant.isEmpty()) {
        // THE PATH COMES FROM THE VERSION REGISTRY, not a hardcoded one. It was hardcoded to the
        // old upstream tree, and from the day the A17 device tree became device/remora (bd
        // remora-28ix.4 R1) the sed rewrote a BoardConfig that A17 does not build: measured
        // afterwards, the old tree said alderlake, device/remora said x86_64, and the image shipped
        // tagged -alderlake having been compiled at baseline. Every A17 performance number taken in
        // that window measured an untuned build. The guard below could not catch it either — the
        // old tree WAS still synced then and still carried the anchor, so the grep found it and the
        // sed "succeeded". (Neither release syncs that tree any more; the registry is the only
        // opinion left, which is the point.)
        const AndroidRelease *rel = androidRelease(plan.androidVersion);
        const QString bcDir = rel ? rel->boardConfigDir : QString();
        if (bcDir.isEmpty()) {
            steps << QStringLiteral(
                         "{ echo \"arch variant: Android %1 has no board config dir in the release "
                         "registry — refusing to guess which device tree to tune\" >&2; false; }")
                         .arg(plan.androidVersion);
            return steps;
        }
        const QString bc = tree + QLatin1Char('/') + bcDir + QStringLiteral("/BoardConfig.mk");
        steps << QStringLiteral(
                     "{ [ -f %1 ] || { echo \"arch variant: %1 does not exist — the release "
                     "registry's board config dir is stale\" >&2; false; }; "
                     "grep -q '^TARGET_ARCH_VARIANT := ' %1 || { echo \"arch variant: no "
                     "TARGET_ARCH_VARIANT anchor in %1 — wrong tree or device config moved\" >&2; "
                     "false; }; }")
                     .arg(bc);
        steps << QStringLiteral(
                     "sed -i -e 's/^TARGET_ARCH_VARIANT := .*/TARGET_ARCH_VARIANT := %2/' "
                     "-e 's/^TARGET_2ND_ARCH_VARIANT := .*/TARGET_2ND_ARCH_VARIANT := %2/' %1 && "
                     "echo \"arch variant: %2 (TARGET_ARCH_VARIANT + TARGET_2ND_ARCH_VARIANT)\"")
                     .arg(bc, plan.archVariant);
    }
    steps << QStringLiteral("cp %1/do-build.sh %2/do-build.sh").arg(sbuild, tree);
    steps << QStringLiteral(
                  "[ \"$(docker image inspect -f '{{index .Config.Labels \"remora.builder\"}}' "
                  "remora-builder:latest 2>/dev/null)\" = 2 ] || "
                  "docker build -t remora-builder:latest -f %1/builder.Dockerfile %1")
                  .arg(sbuild);
    QString featureEnv;
    if (withGapps) featureEnv += QStringLiteral(" -e WITH_GAPPS=true");
    if (withNdk) featureEnv += QStringLiteral(" -e WITH_NDK=true");
    if (withMagisk) featureEnv += QStringLiteral(" -e WITH_MAGISK=true");
    // CONTAINER BOUNDS — bd remora-4ei.27. On this docker run had no --memory and
    // no --cgroup-parent, and it hung the entire host for three hours.
    //
    // WHY --cgroup-parent IS THE LOAD-BEARING HALF. A plain `docker run` scope lands in
    // system.slice. This host caps that slice at MemoryHigh=20G, and MemoryHigh throttles by
    // FORCED RECLAIM — it never kills. So an oversized build does not die, it stalls, and it
    // stalls every other unit in the slice with it: bluetooth.service (the keyboard went
    // away), systemd-journald (SIGABRT on its 3-minute watchdog, then a 14-restart loop) and
    // systemd-resolved. The desktop survived only because user.slice carries MemoryLow=16G,
    // which left a responsive screen and no input device to use it with. Measured: the build
    // wrote nothing to disk for 3h09m while its status line kept ticking, because the status
    // line goes to a pipe and targets go to disk. Own slice => that blast radius is gone.
    // The name is deliberately undashed: systemd reads '-' in a slice name as hierarchy, so
    // "remora-build.slice" would imply a parent remora.slice. The slice needs no unit file —
    // systemd creates it on demand with no limits, which is what we want, because the binding
    // limit should be the container's own.
    //
    // WHY --memory-swap EQUALS --memory: that disables container swap entirely, so the build
    // hits a hard wall and exits 137 instead of quietly filling 60 GB of swap over an hour —
    // which is the condition systemd-oomd trips on (see the preflight note above).
    //
    // THE AUTO FIGURE IS NOT CALIBRATED. MemAvailable minus a 6 GiB reserve is first
    // principles, not a measurement, and it is deliberately taken at LAUNCH so a machine
    // with a different amount of RAM — or one running a VM — sizes itself. It is exposed as
    // 'memory cap' in the Source build card precisely so it can be overridden when it is wrong;
    // if a build is ever killed at a limit that should have fit, raise it and record the
    // number on the bead rather than removing the bound.
    //
    // SOONG_INCREMENTAL_ANALYSIS=false — the A17 memory blow-up. MEASURED:
    // soong_build was OOM-killed at 35.7 GiB anon-RSS and was STILL CLIMBING, against 19.4 GiB
    // plateaued for 58 minutes on the earlier tree. Cause is not the tree growing, it is a
    // release flag: build/release/flag_values/cp2a/RELEASE_SOONG_INCREMENTAL_ANALYSIS.textproto
    // sets bool_value:true, and cp2a IS the A17 config (16→bp2a, 17→cp2a). bp2a has no such
    // file, which is why A16 never showed this. Incremental analysis keeps the whole analysis
    // graph resident, so it trades RAM for re-analysis speed.
    // The env var WINS over the release flag: ui/build/config.go sets incrementalBuildActions
    // false AND incrementalBuildActionsSetInEnv true, and dumpvars.go only consults
    // RELEASE_SOONG_INCREMENTAL_ANALYSIS when that "SetInEnv" flag is unset.
    // Passed unconditionally: on A16 the flag is absent so this is a no-op, and one code path
    // beats a version branch that has to be kept in sync with the lunch mapping.
    // WHEN TO REVISIT: the speed it buys only materialises when analysis is actually REUSED
    // between runs. It now can be — a patch edit re-syncs only its own project (bd remora-4ei.39)
    // — but a FAILED build still records nothing, so a retry after a compile error re-analyses
    // from scratch anyway (bd remora-4ei.26). Fix that bead first, confirm the host has headroom
    // for ~36 GiB+, then consider turning this back on.
    // SOONG_GOMEMLIMIT — WITHOUT THIS THE CAP ABOVE KILLS BUILDS IT SHOULD HAVE ALLOWED.
    // soong_build is a Go program and Go's GC does NOT read cgroup limits (soong sets neither
    // GOMEMLIMIT nor GOGC — verified by grep over build/soong). At the default GOGC=100 the
    // heap may reach 2x the live set before a collection runs. MEASURED on this host:
    //     live heap plateau (bd remora-4ei.19)   19.4 GiB
    //     => GC would not fire until           ~38.8 GiB
    //     run at --memory=36g  killed at 35.8 GiB
    //     run at --memory=38g  killed at 37.8 GiB
    // Each kill landed just under whatever cap it was given — the giveaway that the process
    // was tracking the ceiling, not expressing a real demand. Raising the cap only moves the
    // kill; it never converges.
    //
    // --cpu-shares=128 KEEPS THE MIRROR SMOOTH WHILE BUILDING. MEASURED mid-build with
    // everything at the default weight:
    //     machine.slice (the VM)  cpu.pressure some avg10 = 73.4%   <- vCPUs stalled waiting
    //     user.slice                                        5.8%
    //     remorabuild.slice                                10.0%
    //     siso alone                                        955% CPU (9.5 cores)
    // The mirror is a 60 Mbps H.265 stream out of a container INSIDE the guest, so a guest
    // vCPU that misses its slot drops frames. Network was NOT the cause — wlp6s0 showed zero
    // tx/rx errors and zero drops.
    // BUILD_NICE does NOT help here: nice orders tasks WITHIN a cgroup, while cgroup v2
    // arbitrates BETWEEN slices by weight, and remorabuild.slice sat at the same weight (100)
    // as machine.slice. 128 shares maps to cpu.weight 21 on this runc — VERIFIED empirically
    // rather than from the conversion formula, which is runc-version dependent (measured:
    // 1024->100, 512->59, 256->35, 200->29, 128->21).
    // This costs BUILD THROUGHPUT ALMOST NOTHING because cgroup weights are work-conserving:
    // capacity the VM does not want flows straight back to the build. It binds only when both
    // genuinely compete, which is exactly when the mirror needs to win.
    // NOT a hugepages problem, though that is the intuitive suspect (bd remora-1aw.3): the
    // host half of that work is applied but INEFFECTIVE (ShmemHugePages 0.9% of guest RAM,
    // because a VFIO hostdev pins all guest RAM at start and pinned pages can never be
    // collapsed by khugepaged), the guest half was never applied at all, and hugepages address
    // memory LATENCY (EPT walks) rather than vCPU scheduling delay. Different mechanism.
    //
    // NINJA_HIGHMEM_NUM_JOBS IS NOT SET HERE, AND THAT IS CORRECT — BUT NOT FOR THE REASON
    // THIS COMMENT USED TO GIVE. It claimed the pool was "created, sized, and never
    // referenced", on a count of `pool = highmem_pool` that came back ZERO. That count was
    // taken against out/soong/build.<product>.ninja, which is only the INDEX; soong
    // shards the real output into build.<product>.0.ninja … .9.ninja beside it.
    // RECOUNTED, per shard: 0,8,0,2,4,0,0,0,2,4 and 0 in the index = 20 references,
    // plus 50 on the kati side. They are RULE-level, which applies to every edge using the
    // rule, and they are exactly the rules that matter:
    //     rule m.test-api-stubs-docs-non-updatable_android_common.metalava
    //         pool = highmem_pool
    // — the very target that was OOM-killed in bd remora-4ei.30. The pool is declared in
    // out/combined-<product>.ninja ABOVE both `subninja` lines, so one depth governs kati
    // and soong alike, and soong_ui's HighmemParallel() returns NINJA_HIGHMEM_NUM_JOBS before
    // any RAM-derived branch. The knob therefore works and does bound metalava.
    // It is not set HERE only because do-build.sh derives it INSIDE the container from that
    // container's own /sys/fs/cgroup/memory.max — the one place that can see the real cap. Setting
    // it out here would hard-code a number computed against host RAM, which is the original bug.
    // So: do not re-add it to this docker run, and do not remove it from do-build.sh either.
    // BUILD_JOBS (ninja -j) also bounds concurrency, but bluntly — it throttles every step
    // class, not just the memory-heavy ones, which is precisely what the pool exists to avoid.
    //
    // AND NOTE WHAT THE FAILURE DATA COULD NOT SHOW: siso_metrics.json records only steps that
    // COMPLETE. The OOM-killed metalava is absent from it, and the 766 steps that did complete
    // peaked at 0.02 GiB each / 0.9 GiB concurrent. So concurrency of the heavy Java steps is
    // not measurable from siso's own metrics — reconstructing it needs sampling of the cgroup
    // while the build runs, not a post-mortem of the metrics file.
    //
    // PLAIN GOMEMLIMIT BOUNDS SISO — a DIFFERENT process from soong_build, and the two are not
    // redundant. siso is the build EXECUTOR and is also Go with no heap bound of its own, but
    // unlike soong_build it is NOT launched through `env -i`, so the container's environment
    // does reach it. MEASURED mid-compile with only SOONG_GOMEMLIMIT set:
    //     siso                     19.3 GiB
    //     javac/R8 steps           3.4 GiB EACH, three concurrent
    //     cgroup anon              36.1 GiB of a 38 GiB cap
    // siso alone held two thirds of the unreclaimable memory, leaving ~9 GiB for the work that
    // actually compiles. Bounding it hands that back to the jobs.
    // Sized at 65%, RAISED from 50% after sampling showed the bound could not be met. MEASURED
    // across two runs with SISOMEM=17: siso peaked 18.7 then 19.6 GiB — ABOVE its own limit
    // both times. GOMEMLIMIT is SOFT: when the live heap genuinely exceeds it, Go cannot
    // collect below it and merely burns CPU trying, so a bound under the live set costs GC
    // work and returns nothing. siso sits at ~19.6 GiB either way.
    // NOTE THE RATIO IS CONCEPTUALLY WRONG AND KEPT ONLY FOR SCALING SANITY: siso's live set
    // tracks the size of the build GRAPH (218k targets here), not the container cap. 65% of a
    // 34 GiB cap is 22 GiB, comfortably over the observed 19.6; on a much smaller cap no ratio
    // will help and the answer is a bigger cap, not a smaller bound.
    // The two limits do NOT have to fit simultaneously: analysis and compile are sequential
    // phases, and soong_build has exited before siso does the heavy work.
    //
    // WHY THE OTHER VARIABLE IS SOONG_-PREFIXED. Passing GOMEMLIMIT for soong_build
    // does NOTHING, and it was tried first: the bootstrap ninja launches soong_build through
    // `env -i` (build/blueprint/bootstrap/bootstrap.go renders `env -i $env "$$BUILDER"`), so
    // it starts with an EMPTY environment and inherits nothing from this container. Proven by
    // a build that reached 37.8 GiB with GOMEMLIMIT=30GiB set. The only channel in is
    // soong_ui's invocationEnv, which is what the vendored 'soong_gomemlimit' source patch
    // opens — it forwards SOONG_GOMEMLIMIT from soong_ui's environment into soong_build's.
    // SO: THIS FLAG DOES NOTHING WITHOUT THAT PATCH ENABLED. If soong_build starts getting
    // OOM-killed again, check the patch is still applied before touching these numbers.
    //
    // (The NINJA_HIGHMEM_NUM_JOBS reasoning that used to sit here has been removed along with
    // the flag — see the note above: soong reads the variable and sizes the pool, but no build
    // statement joins the pool, so it never bounded anything.)
    //
    // Sized at 65% of the container cap. It must sit ABOVE the live set or Go thrashes (GC
    // burning CPU against a flat heap) and BELOW 2x the live set or it never binds — with a
    // 19.4 GiB live set that window is roughly 22-35 GiB, and 65% of a 37 GiB cap lands at 24.
    // If a build ever thrashes, the live set has outgrown this; raise the cap so 65% of it
    // clears the live set, and RECORD the live figure — nobody has re-measured it since
    // 4ei.19, and every ceiling here is derived from that one number.
    //
    // SOONG_INCREMENTAL_ANALYSIS WAS FORCED OFF HERE AND HAS BEEN REMOVED. It was added on the
    // theory that A17's cp2a release flag (RELEASE_SOONG_INCREMENTAL_ANALYSIS=true, absent for
    // A16's bp2a) caused the memory blow-up. The env var demonstrably took effect —
    // --incremental-build-actions vanished from the soong_build command line — and the very
    // next build was OOM-killed at its cap regardless. The theory was WRONG; the real cause was
    // Go's GC not seeing the cgroup, fixed by SOONG_GOMEMLIMIT above.
    // Turning it back on is deliberate: incremental analysis exists to make RE-analysis cheap,
    // which is exactly the cost being paid on every failed-build retry (~40 min, bd
    // remora-4ei.26). The reason it was a risk — unbounded analysis memory — is now bounded
    // directly. Leaving it off would have kept a mitigation for a cause that does not exist
    // while paying its price on every iteration.
    // THE HARD MEMORY CAP IS NOW OPT-IN (memory cap spin box), NOT AUTOMATIC. It was automatic
    // and it killed FOUR builds that were otherwise progressing — each one OOM-killed just
    // under whatever ceiling it had been given, while the same workload ran fine unbounded
    // earlier in the day. Sampled at the last two kills: page cache had already been reclaimed
    // to 0.0 and 0.1 GiB, so this was the cap binding on genuine demand, not an accounting
    // artifact — the container simply needs more than the auto figure allows once siso's ~20
    // GiB (measured twice, irreducible) is subtracted.
    //
    // WHAT ACTUALLY PREVENTS THE HOST HANG IS THE SLICE, NOT THE CAP, and the two
    // are separable. That incident was a MemoryHigh THROTTLE DEADLOCK inside system.slice:
    // bluetoothd, systemd-journald and systemd-resolved were throttled alongside the build
    // because docker scopes default into that slice. --cgroup-parent=remorabuild.slice is what
    // fixes it — a top-level slice with no limits cannot throttle system services however
    // large it grows. Dropping --memory therefore does NOT reintroduce the hang.
    // It does reintroduce lesser risks, honestly: the build can now grow into swap, and
    // systemd-oomd may kill it on swap fill (it has before — see the preflight note). Both are
    // CLEAN failures rather than a wedged host, and the desktop keeps its MemoryLow=16G while
    // the VM's RAM is VFIO-pinned and cannot be swapped out from under the mirror.
    // Set the spin box to a positive value to restore a hard ceiling.
    steps << QStringLiteral(
                  // BASE IS DERIVED FROM MemTotal, NOT MemAvailable — MemAvailable is an
                  // instantaneous reading and this is a multi-hour budget. Measured
                  // within ten minutes: 40.8 GiB with the machine idle, 15 GiB with
                  // a build running. The same command would hand soong_build a 22 GiB Go heap
                  // or a 5 GiB one depending on nothing but when it was started, and the
                  // caps the old automatic path chose across one day spanned 34..38 GiB. A
                  // budget that moves with the weather cannot be reasoned about or reproduced,
                  // which is the whole point of the pinned build.
                  // 55% of MemTotal reproduces the 34 GiB BASE that has been working on this
                  // 62 GiB host, and scales to other machines instead of collapsing on them
                  // the way a fixed subtraction would. The remainder covers the VM's
                  // VFIO-pinned RAM (unswappable), the desktop's MemoryLow, and siso's own
                  // ~20 GiB. The spin box overrides it outright when the guess is wrong.
                  "MEMG=%6; TOTAL=$(awk '/^MemTotal:/{print int($2/1048576)}' "
                  "/proc/meminfo 2>/dev/null); [ -n \"$TOTAL\" ] || TOTAL=16; "
                  "BASE=$MEMG; [ \"$BASE\" -gt 0 ] || BASE=$((TOTAL*55/100)); "
                  "[ \"$BASE\" -ge 8 ] || BASE=8; "
                  "GOMEM=%7; [ \"$GOMEM\" -gt 0 ] || GOMEM=$((BASE*65/100)); "
                  "[ \"$GOMEM\" -ge 4 ] || GOMEM=4; "
                  "SISOMEM=$((BASE*65/100)); [ \"$SISOMEM\" -ge 4 ] || SISOMEM=4; "
                  "if [ \"$MEMG\" -gt 0 ]; then "
                  // --memory-swap ABOVE --memory, so the cap bounds RESIDENT memory and the
                  // overflow pages out instead of being killed. Equal values disable swap
                  // entirely, which turns any cap below the workload's true peak into a
                  // guaranteed OOM: soong_build's live heap plateaus at 19.4 GiB and Go wants
                  // roughly 2x that before it collects, so a 26 GiB swap-off cap died at 2:52 in
                  // analysis (bd remora-82c.13). Swapping is slow, but on a host that is already
                  // 15 GiB into swap by design, "slower" is the entire point of throttling and
                  // "killed" is not a throttle at all.
                  // 2x the cap, not unlimited: bounded so a runaway cannot drag the whole host
                  // into thrash, and still far more headroom than the peak needs.
                  "MEMFLAGS=\"--memory=${MEMG}g --memory-swap=$((MEMG*2))g\"; "
                  "CAPMSG=\"${MEMG} GiB resident + $((MEMG)) GiB swap\"; else MEMFLAGS=; "
                  "CAPMSG=\"UNCAPPED (slice-isolated only)\"; fi; "
                  "echo \"build container ${CAPMSG} in "
                  "remorabuild.slice, soong_build Go heap ${GOMEM} GiB, siso Go heap "
                  "${SISOMEM} GiB\"; "
                  "docker run --rm --cgroup-parent=remorabuild.slice --cpu-shares=128 "
                  "${MEMFLAGS} "
                  "-e SOONG_GOMEMLIMIT=${GOMEM}GiB -e GOMEMLIMIT=${SISOMEM}GiB "
                  // BUILD_LUNCH is passed EXPLICITLY rather than left to do-build.sh's own
                  // ANDROID_VER case: the two mappings disagreed above 17, and the shell's `*)`
                  // arm silently yields an Android-16 release config for anything newer. Sending
                  // the resolved value makes releaseConfigFor() the only opinion in the system.
                  "-v %1:/src -e BUILD_NICE=%2 -e ANDROID_VER=%3 -e BUILD_LUNCH=%8%4%5 "
                  "remora-builder:latest bash /src/do-build.sh")
                  .arg(tree)
                  .arg(buildNice)
                  .arg(androidVer)
                  .arg(buildJobs > 0 ? QStringLiteral(" -e BUILD_JOBS=%1").arg(buildJobs)
                                     : QString(),
                       featureEnv)
                  .arg(buildMemGiB)
                  .arg(soongMemGiB)
                  // %8. Empty for a version releaseConfigFor() does not know, which leaves
                  // BUILD_LUNCH empty and lets do-build.sh's own case answer — the same result as
                  // before this was passed at all. The lineage path cannot reach that state
                  // anyway: buildContainerPatchCommand hard-fails first for an unknown version.
                  // THE FULL LUNCH STRING, not the bare release config. do-build.sh uses
                  // BUILD_LUNCH verbatim as `lunch "$cfg"`, so passing just "cp2a" ran
                  // `lunch cp2a`: TARGET_PRODUCT became cp2a, dumpvars failed with "Don't have a
                  // product spec for: 'cp2a'", and lunch STILL returned 0 having exported nothing.
                  // The build then died a second later in release_config.mk saying TARGET_RELEASE
                  // was unset — naming neither lunch nor the product, which is why this survived.
                  // Every A17 source build has failed this way since the value was first passed
                  // (bd remora-82c.12).
                  .arg(lunchConfigFor(androidVer));
    // package the compiled partitions into a bootable docker image on the host (the builder
    // container has no docker) — this is what makes the build show up in Built images. The tag
    // (builtTag, computed up front) reflects the baked-in features, with a -src marker so a
    // source build never clobbers a prebuilt/golden tag of the same features.
    // Run assemble straight from the source-build dir (not a copy in the tree) so its overlay
    // features (widevine/ndk_translation/device_compat/hw_features/zygisk_pif, resolved via
    // dirname $0) always come from the complete repo — the tree's features/ copy is stale. The
    // tree is passed as $1 (it only supplies out/target/product for the partitions).
    QString asmEnv;
    if (withPlaySpoof) asmEnv += QStringLiteral("WITH_PLAY_SPOOF=true ");
        // Only meaningful alongside play_spoof — assemble stages it inside that block.
        if (withShamiko) asmEnv += QStringLiteral("WITH_SHAMIKO=true ");
    // Name the product dir EXPLICITLY: with both device trees building through the transition
    // (bd remora-28ix.4), assemble's glob would find two product dirs and refuse — and the one
    // this build just wrote is exactly the lunch's product.
    if (!lunchConfigFor(androidVer).isEmpty())
        asmEnv += QStringLiteral("BUILD_LUNCH=%1 ").arg(lunchConfigFor(androidVer));
    // Name the BoardConfig assemble should stamp the arch variant from (bd remora-bm7.13). Same
    // registry field the sed above edits, passed rather than re-derived, so the file the build read
    // and the file the stamp reads are the same file BY CONSTRUCTION — the divergence that made
    // every -alderlake image in the pre-eae38f6 window a baseline build cannot happen twice.
    if (const AndroidRelease *rel = androidRelease(androidVer);
        rel && !rel->boardConfigDir.isEmpty())
        asmEnv += QStringLiteral("BOARD_CONFIG_DIR=%1 ").arg(rel->boardConfigDir);
    steps << QStringLiteral("%3sh %1/assemble-image.sh %2 %4")
                 .arg(sbuild, tree, asmEnv, builtTag);

    // Recorded LAST: only a build that got all the way through proves the tree is coherently
    // patched. Patch application is deliberately failure-TOLERANT, so an earlier position always
    // gets reached — a run whose patches had ALL failed once recorded "success" here and the next
    // build skipped patching against a half-patched tree. The cost of waiting is that a COMPILE
    // failure does not skip on retry, which was the most valuable case; buying that back needs
    // the patch steps to report whether they succeeded, which they currently cannot.
    steps << buildPatchStateEpilogue(tree, repoFp);
    return steps;
}

}  // namespace remora
