#pragma once
#include <QByteArray>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace remora {

// What a profile must provide for a patch to be worth OFFERING. All-empty (the common case) means
// "always offered". Evaluated by irrelevantPatches() in Gating.h.
//
// OFFERING, not applying — the distinction is the whole contract. A patch failing its needs is
// hidden from the page and NOTHING ELSE: it is never unticked, for the reason spelled out above
// refreshPatchApplicability (source_patches is a pin, so clearing on hide destroys the user's
// choice and coming back does not restore it). A hidden entry that is still enabled therefore
// still applies, and the page says so rather than letting it happen invisibly.
//
// Declared here rather than in MainWindow because "what this patch needs" is a property OF THE
// PATCH. The same reasoning put Feature::requiresPatches in the feature registry, and the same
// test discipline applies: every key named here is validated against the registries, so a typo is
// a test failure rather than a row that silently never appears.
struct PatchNeeds {
    QString feature;           // build-feature key that must be enabled
    QString patch;             // another patch key that must be enabled (prerequisite-of)
    bool lineageTree = false;  // needs the Lineage projects, i.e. source_kind != aosp
    bool venus = false;        // needs the Venus/NVIDIA vtest path
};

// A vendored source patch: the custom delta for one source-tree project, exposed as an opt-in
// checkbox in the GUI. Applied on top of a clean (pinned) upstream checkout to reconstruct
// Remora's accelerated image reproducibly. Patch files live under vendor/source-patches/<dir>/.
struct SourcePatch {
    QString key;          // stable config key
    QString label;        // UI label
    QString note;         // tooltip
    QString projectPath;  // tree-relative project the patch targets (created if new)
    QString dir;          // vendor/source-patches/<dir>
    QStringList patches;  // file names within dir, in apply order
    bool amStyle;         // true = git am (commits); false = git apply (working-tree diff)
    bool defaultOn;       // on by default (the golden-image stack)
    // Which section of the Image page this belongs to — see patchGroups() for the labels.
    // IN THE REGISTRY, not in the UI, for the reason the whole file keeps repeating: a parallel
    // table in MainWindow would be a second source of truth over the same set, and every one of
    // those this project has had drifted (bd remora-4ei.42 is the expensive one). A patch with no
    // group is a test failure, so a new entry cannot quietly fall out of the page.
    // Grouping is PRESENTATION ONLY. It must never be read as apply order: the registry's own
    // order is load-bearing (lineage_version_props after lineage_legal_url, drop_lineage_updater
    // after the two vendor/lineage entries it re-contexts), so the page collects keys per group
    // and preserves registry order WITHIN each one rather than reordering anything.
    QString group;
    // Which Android versions defaultOn actually MEANS anything for. Empty = every version.
    // defaultOn alone is a single version-blind flag describing ONE golden stack, and the golden
    // stack is A17's — so eight of the default-on patches exist purely to compensate for
    // lineage-24.0 being pristine AOSP (bd remora-cbd.1), which lineage-23.0 does not need and in
    // one case is actively harmed by. Without this an A16 profile is told to enable all eight.
    // Listed explicitly rather than as a minimum, for the same reason containerPatchSet is per
    // version and never crosses over: a patch that has not been evaluated against a release should
    // not be assumed to belong there. Adding a version here is a statement that someone checked.
    QVector<int> defaultOnFor = {};
    PatchNeeds needs = {};
};

const QVector<SourcePatch> &sourcePatches();

// Rewrite a saved patch-key list through the retirement map: a key that has been absorbed into
// another entry becomes that entry, duplicates collapse, order is preserved, and anything
// unrecognised is passed through untouched.
//
// Needed because a profile's source_patches list is a PIN (see unappliedDefaultPatches): once
// written it is never re-seeded from the defaults, so consolidating two entries into one would
// silently drop the capability from every existing profile on its next build — the patch would
// simply never be reached, which is the failure bd remora-4ei.38 already cost two builds to.
// Applied on load, so nothing downstream has to know a key was ever renamed.
QStringList canonicalPatchKeys(const QStringList &keys);

// The default-on patch keys for one Android version (Remora's golden hardware-accelerate stack),
// honouring each entry's defaultOnFor scope.
QStringList defaultSourcePatchSet(int androidVersion);

// The Image page's patch sections, in display order: (group key, human label). Every
// SourcePatch::group is one of these keys, asserted by a test in both directions — an unknown group
// on a patch and a group here that no patch uses are both failures, so the page can never render
// an empty section or silently drop an entry into no section at all.
const QVector<QPair<QString, QString>> &patchGroups();

// Is this patch part of the golden stack FOR THIS VERSION — i.e. defaultOn, scoped by defaultOnFor?
//
// The page tiers on exactly this, and the three answers are genuinely different situations:
//   true                     → golden stack, folded into a collapsed summary (not a decision)
//   false, but p.defaultOn   → registered and proven, just not on this release (demoted, not hidden)
//   false, and !p.defaultOn  → a real opt-in choice, always shown
// Was a file-static; exposed because the tiering is the whole point of the grouped page and
// recomputing `p.defaultOn && (scope empty || scope contains v)` in the UI would be a second copy
// of the rule that defaultSourcePatchSet and unappliedDefaultPatches already share.
bool patchIsDefaultFor(const SourcePatch &p, int androidVersion);

// Default-on patch keys that `enabled` does NOT contain — i.e. registered but never applied.
// defaultOn only seeds a FRESH config: once a profile has written source_patches to remorarc that
// list is an explicit pin, and a patch added to the registry afterwards is never added to it. The
// patch still builds, installs and stages correctly, so nothing anywhere says it was skipped — the
// apply loop simply never looks at it, and the build fails exactly as if the patch did not work.
// That cost two builds in one day (bd remora-4ei.38), the second one dying on the very KeyError its
// unapplied patch exists to fix. Reported at apply time, the way EXCLUDE drops are.
// Opt-in (defaultOn=false) patches are deliberately off and are not reported. An EMPTY `enabled` is
// not a pin — it is the codebase-wide "unset ⇒ resolver applies the defaults" (see Config.h), which
// every caller honours — so it yields nothing.
// Scoped to `androidVersion` via defaultOnFor: the first cut of this warning was version-blind and
// told an A16 profile to enable eight patches written for A17 — one of which (no_lineage_sepolicy)
// would have REMOVED working Lineage sepolicy, and another (buildprop_touch_rotation) fixes a
// producer/consumer split that lineage-23.0's build/make does not have. A warning that cries wolf
// on every A16 build is worse than the silence it replaced.
QStringList unappliedDefaultPatches(const QStringList &enabled, int androidVersion);

// Every distinct source-tree project the registry touches, in registry order. The incremental
// machinery below iterates THIS, not the enabled set: unticking the last patch of a project has to
// count as a change for that project, and a project with nothing enabled is not in `enabled` at all.
QStringList sourcePatchProjects();

// Filesystem-safe name for a project path ("frameworks/base" → "frameworks-base"), used for the
// per-project state and mark files. Collision-free over the registry (asserted by a test).
QString patchProjectSlug(const QString &projectPath);

// The container-compat patch set directory for an Android version, under
// vendor/source-build/container-patches/. PER VERSION and never crossing over, for the same reason
// the pinned manifest is version-suffixed: applying the A16 set to an A17 tree left conflict
// markers inside PID 1's C++ (bd remora-cbd).
QString containerPatchSet(int androidVersion);

// Shell commands that reset each enabled patch project to pristine (drop conflicts, staged
// content, stray files; committed patches survive). Build runs these BEFORE repo sync — a
// conflicted index blocks the sync itself. Safe when a project doesn't exist yet.
// `projectMarks` (projectPath → mark file, from patchProjectMarks()) guards each command so a
// project the incremental pass is leaving alone is not touched; empty = reset everything.
QStringList buildResetCommands(const QStringList &enabled, const QString &sourceDir,
                               const QMap<QString, QString> &projectMarks = {});

// Shell commands (one per patch) that apply `enabled` patches to the tree at `sourceDir`, reading
// patch files from `patchRoot` (the vendored source-patches dir). Pure — the engine runs these
// locally or over ssh. `git am`/`git apply` run inside each project; a new project is created first.
// `androidVersion` selects the series: a per-version override directory `<dir>-a<N>` wins when it
// exists, else the shared `<dir>` applies. Patches routinely stop applying across an Android bump
// (upstream absorbs part of one, or the surrounding file drifts), so a diverged series can get a
// variant without disturbing the versions the shared one still serves.
// `projectMarks` (projectPath → mark file, from patchProjectMarks()) guards every command of a
// project whose patch state the incremental pass found unchanged. The guard is per COMMAND but the
// condition is per PROJECT, so a project's commands still all run or all skip together — which is
// what keeps the shell variables they pass between them ($d, $a) flowing. Empty = apply everything.
// `requiredBy` (patch key → requesting feature(s), from featureRequiredPatchOwners()) upgrades a
// conflict on one of those patches from the warn-and-continue skip to a HARD FAILURE naming the
// feature: the soft skip exits 0, callers join with " && ", and the build otherwise completes
// green hours later with the feature's code absent from the image (bd remora-82c.1). Patches no
// feature asked for keep the soft skip, which is what lets a series being ported limp usefully.
QStringList buildApplyPatchCommands(const QStringList &enabled, const QString &sourceDir,
                                    const QString &patchRoot, int androidVersion,
                                    const QMap<QString, QString> &projectMarks = {},
                                    const QMap<QString, QString> &requiredBy = {});

// ---- incremental patch state ------------------------------------------------------------------
//
// WHY ANY OF THIS EXISTS: every build re-ran `reset, repo sync --force-sync, re-apply` over ~1200
// projects, and force-sync rewrites every source file's mtime, so Soong could never reuse an
// analysis. Measured: 40:48 of a 42:46 build was `bootstrap blueprint`, reaching a
// compile error 30 seconds in. repo sync itself is 2.5s here — the cost is entirely the re-analysis
// it forces. Soong says as much: "Try enabling incremental analysis for faster builds after
// changing Android.bp files."
//
// THE CONSTRAINT THAT SHAPES THE WHOLE DESIGN: the global sync is load-bearing for CORRECTNESS, not
// just freshness. The apply loop skips a patch whose subject slug is already in `git log --format=%f`
// and `git reset --hard` does not remove committed patches — so if a patch's CONTENT changes while
// its SUBJECT does not (the common case while iterating on a fix), anything short of
// `repo sync --force-sync` leaves the old commit in place, the slug still matches, and the OLD
// VERSION SILENTLY APPLIES. "Just skip the sync when patches changed" is therefore wrong. What is
// safe is to force-sync ONLY the projects whose patches changed: repo sync takes project paths.

// Where the incremental bookkeeping lives. All five sit at the tree ROOT, outside every repo
// project, so a sync of any project cannot wipe them.
struct PatchStateFiles {
    QString repoState;  // the sync-input fingerprint of the last successful build
    QString stateDir;   // one file per project: its patch fingerprint at that build
    QString skipMark;   // present ⇒ the sync inputs are unchanged, no global sync this run
    QString markDir;    // one file per project that needs no work this run
    QString syncList;   // newline-separated project paths to force-sync this run
};
PatchStateFiles patchStateFiles(const QString &tree);

// projectPath → the mark file whose PRESENCE means "this project is already in the wanted state".
// Feed to buildResetCommands/buildApplyPatchCommands as their guard map.
QMap<QString, QString> patchProjectMarks(const QString &tree);

// Reads a vendor-relative path (one file, or a directory walked recursively) and returns
// (vendor-relative path, content) pairs sorted by path; empty when the path does not exist. The
// impure half of fingerprinting, injected so everything below stays pure and testable — and read
// from the LOCAL vendor tree, because on a remote build the staged copy lives on the build host.
using VendorReader = std::function<QList<QPair<QString, QByteArray>>(const QString &relPath)>;

// Does this patch have anything to apply on this Android version?
//
// DERIVED FROM THE ASSETS, not hand-declared per patch, and deliberately so: the goal is to offer
// as many version/patch combinations as are actually buildable, and a hand-maintained allow-list
// would shrink that set to whatever someone last remembered to update. This resolves the SAME pair
// the apply loop resolves — <dir>-a<N> when it exists, else the shared <dir> — so "offered in the
// UI" and "has patches to apply" cannot drift apart. A new Android version needs no edit here: drop
// the assets in and the combination appears.
//
// False when neither directory exists (nothing to apply) and when the resolved one carries a SKIP
// marker (the series was absorbed upstream for this release, so enabling it is a no-op).
//
// Reads the LOCAL vendor tree, while the apply loop resolves on the build host — the same content
// in practice, since source-build stages vendor there. One edge they disagree on: an <dir>-a<N>
// that exists but is EMPTY. The shell picks it (`[ -d ]`) and then hard-fails on the missing patch
// file; this returns the shared dir's answer instead. An empty override is a broken asset either
// way, and the build still refuses — it just refuses later.
bool patchAppliesTo(const SourcePatch &p, int androidVersion, const VendorReader &read);

// Every registered patch key applicable to a version, in registry order.
QStringList applicableSourcePatchKeys(int androidVersion, const VendorReader &read);

// One registry entry whose ASSETS do not back it: the resolved directory is missing files the
// registry names (or, for a globbed series, holds no NNNN-*.patch at all) — or carries .patch
// files the registry does NOT name, which is the same skew seen from the other side.
struct PatchAssetGap {
    QString key;
    QString dir;          // the directory that was resolved, vendor-relative
    QStringList missing;  // file names; "<no NNNN-*.patch>" for a globbed series with none, or
                          // "<no patch directory>" when the entry has no assets at all
    // .patch files present in the resolved dir that NO registry entry using that dir names. The
    // apply loop applies explicit entries by name, so an unnamed file is silently NOT APPLIED —
    // the build succeeds and ships without it. That is the stale-BINARY shape (bd remora-y9y9):
    // a patch added to the tree after this binary's registry was compiled, e.g. /usr/bin/remora
    // staging five of the six minigbm patches its newer tree carried (bd remora-4ei.9's trap).
    // Missing files say "reinstall the tree"; unknown files say "rebuild/reinstall remora".
    // Globbed series never report these — the glob consumes every NNNN-*.patch by design — and
    // an unnamed .patch in a SKIP'd dir is as deliberately dead as the series itself.
    QStringList unknown;
};

// Audit the patch tree AGAINST THE REGISTRY for one Android version.
//
// The repo's own tests already assert this for the SOURCE tree, which is why it has never been
// wrong there. The gap is the INSTALLED tree: builds stage from it (REMORA_INSTALLED_VENDOR), and
// a binary carrying a newer registry than /usr/share can name a directory or a file that is simply
// not there. Today that is caught by the apply loop's hard-fail — correct, but only once the build
// is already running, i.e. after a repo sync that costs far more than this check does
// (bd remora-fgj.11 installed the hard-fail; this is the same guard moved earlier).
//
// Resolves the SAME pair the apply loop and patchAppliesTo resolve — <dir>-a<N> when it exists,
// else the shared <dir> — so all three agree on what would actually be read. A SKIP marker means
// the series is deliberately absent for this release and is NOT a gap. A directory that does not
// exist at all IS one, and is the headline case: an entry added after the tree was installed has
// no directory there, which patchAppliesTo cannot distinguish from a version that ships nothing.
//
// Reports the tree's state, NOT the build's: an entry gapped here only matters to a build that
// enabled it, so callers refusing a build scope by the enabled set. `check` reports all of it,
// which is honest because a healthy tree yields nothing at all.
QVector<PatchAssetGap> auditPatchAssets(int androidVersion, const VendorReader &read);

// One top-level directory under source-patches whose CONTENT differs between two vendor trees.
// Lists are relative paths within the dir, sorted; a row exists only when something differs.
struct VendorDriftRow {
    QString dir;
    QStringList onlyInMine;   // present in `mine`, absent in `other`
    QStringList onlyInOther;  // present in `other`, absent in `mine`
    QStringList differs;      // present in both with different bytes
};

// Byte-compare the source-patches trees of two vendor roots, grouped per patch directory.
//
// This is the content half the registry audit above cannot do (bd remora-y9y9 option b): a patch
// whose BYTES changed in one tree but not the other is invisible to any presence check — every
// name the registry expects is there — and the failure it causes is the quiet kind, a build that
// succeeds and ships the OLD patch. Presence-vs-registry has a reference compiled into the binary;
// content has none, so it takes two real trees, and the caller decides which pair means what: on a
// dev box `mine` is the source tree and `other` the inert installed copy (informational), on an
// installed-vendor binary `mine` is what builds actually STAGE and drift means the repo's edits
// are not what ships (loud). Pure and reader-injected like everything above; comparison is by
// exact bytes because both trees are local and small — a hash would only add a way to be wrong.
QVector<VendorDriftRow> compareSourcePatchTrees(const VendorReader &mine,
                                                const VendorReader &other);

// Why source_kind=aosp cannot build this profile — empty when it can. A kind that says AOSP
// while the ref names a LineageOS branch or the enabled set carries lineage-tree patches builds
// the WRONG IMAGE silently: the aosp arm skips the manifest sync and every container-compat
// patch (bd remora-82c.4), and the lineage-only patches are gated off as irrelevant. Measured
// one stray flip of the kind combo put the lineage-24.0 profile onto the aosp arm
// for a 22-minute build that had to be aborted. Refused with the reason named, never
// reinterpreted — the backend=vm precedent. `enabledPatches` is the EFFECTIVE set (defaults
// already resolved); an empty list asserts "no patches", not "defaults".
QString aospKindConflict(const QString &sourceRef, const QStringList &enabledPatches);

// Fingerprint of everything that decides WHICH UPSTREAM the tree is synced to, and of the
// container-compat patches, which land on ~30 projects that carry no per-project state of their own.
// A change here means a global sync, hence a full re-analysis — correct, and rare.
QString repoStateFingerprint(const QString &repoUrl, const QString &ref, bool pinned,
                             int androidVersion, const VendorReader &read);

// Fingerprint per project of everything that decides its PATCHED state: the enabled patch keys
// targeting it, the declared file list, and the CONTENT of the patch files they read. Covers every
// registered project, including ones with nothing enabled — see sourcePatchProjects().
QMap<QString, QString> patchProjectFingerprints(const QStringList &enabled, int androidVersion,
                                                const VendorReader &read);

// The projects the version's container-compat patchset lands on, from the patch files themselves
// (dir ⇒ project, the same mapping apply-container-patches.sh walks). The prologue verifies these
// against their post-apply HEAD records — they carry no input fingerprint of their own, so before
// this existed a container patch lost from the tree had NO record capable of contradicting the
// skip verdict (bd remora-bahe). Empty for a version with no patchset.
QStringList containerPatchProjects(int androidVersion, const VendorReader &read);

// The step that decides, per project, what this build has to redo. Writes the mark files the guards
// above consult and the project list the targeted sync consumes. FAIL-SAFE BY CONSTRUCTION: when
// the sync inputs changed it deletes every per-project record first, because the global sync wipes
// every project and a surviving record would claim "applied" for a project that was just reset.
// A skip needs BOTH an unchanged input fingerprint AND the project still sitting at its recorded
// post-apply HEAD — the record alone describes what a past build did, not what the tree still
// carries, and trusting it alone is how a bare-pin project shipped a duplicate iHD_drv_video
// (bd remora-bahe). `containerProjects` (see containerPatchProjects) get the HEAD check only;
// pass empty for source_kind=aosp, where no Remora sync or container apply exists to act on it.
QString buildPatchStatePrologue(const QString &tree, const QString &repoFp,
                                const QMap<QString, QString> &projectFp,
                                const QStringList &containerProjects);

// The targeted counterpart to the global bring-up: force-sync only the projects the prologue
// listed. Runs only when the global sync was skipped, and is a no-op when nothing changed.
QString buildTargetedSyncCommand(const QString &tree);

// Prove a sync delivered what it reported: every synced project's HEAD must equal the revision the
// manifest wants (REPO_LREV), or the step fails naming the project. Exists because repo's exit code
// cannot gate the build alone — builds 5 and 6 printed "Checking out local projects
// failed", exited 0, and the chain ran on into a container-compat pass that applied nothing (bd
// remora-4ei.81). Same shape as the 4ei.82 fix one step later: guard the outcome, not the one
// trigger observed. `projects` restricts the check (e.g. "$(cat <syncList>)") for the targeted
// sync; empty checks every manifest project. Runs right after the sync, before anything commits on
// top — later in the build HEAD legitimately differs (patches are committed onto the pin).
QString repoSyncVerifyCommand(const QString &projects = QString());

// Apply the container-compat patches: the whole set after a global sync, or restricted to the
// force-synced projects otherwise — a targeted sync drops that project's container-compat commits
// along with ours, so they must be re-applied, and only for those projects (the rest still have
// theirs, and re-running would rewrite files Soong has already analysed).
QString buildContainerPatchCommand(const QString &tree, const QString &sourceBuildDir,
                                   int androidVersion);

// Record the fingerprints — ONLY after the build succeeded. Patch application is deliberately
// failure-TOLERANT (it skips conflicting patches and continues, because ~26 backport conflicts are
// expected), so it always reaches this point no matter how badly patching went; a run whose patches
// had ALL failed once recorded "success" here and the next build skipped patching against a
// half-patched tree. A completed build is the only signal that proves the tree is coherently patched.
// Also records each patched project's post-apply HEAD (h-<slug>), for source-patch AND
// container-patch projects — the tree-side half the prologue's skip verification reads
// (bd remora-bahe).
QString buildPatchStateEpilogue(const QString &tree, const QString &repoFp);

}  // namespace remora
