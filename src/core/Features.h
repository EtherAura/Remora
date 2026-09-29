#pragma once
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace remora {

// Build is the only stage: every registry entry is a build feature (Runtime never had a member).
enum class Stage { Build };
enum class FeatureStatus { Ok, Caveat, Parked, Deferred, Mandatory };

QString featureStatusToString(FeatureStatus s);

// How the Image page SECTIONS its list, in display order: {key, label}. Deliberately coarser than
// Feature::category — eight categories over sixteen features averages two rows per heading, which
// is more heading than list. Kept here rather than in MainWindow for the reason patchGroups() gives
// for the same choice: a parallel table in the UI is a second source of truth over the same set,
// and every one of those this project has had has drifted.
const QVector<QPair<QString, QString>> &featureSections();
// Which section a Feature::category belongs to. An unmapped category lands in the last section
// rather than vanishing, so a new one is visibly wrong instead of silently missing.
QString featureSectionFor(const QString &category);
// …and which section a SourcePatch::group belongs to, so the optional patches file alongside the
// features they relate to instead of forming sections of their own.
QString featureSectionForPatchGroup(const QString &group);

struct Feature {
    QString key, label, category;
    Stage stage;
    bool defaultOn = false;
    QStringList requires_;   // feature keys that must also be enabled
    QStringList conflicts;
    FeatureStatus status = FeatureStatus::Ok;
    QString note;
    // SourcePatch keys a from-scratch build needs before this feature does anything. `requires_`
    // covers feature→feature only, so until this existed the constraint could be stated ONLY in
    // prose in `note` — nothing read it, nothing validated it, and a green build shipped a feature
    // that did nothing. That is how `sensors` reached a live A17 image with its HAL absent:
    // "No Sensors on the device", devInitCheck -19, while the profile happily listed the feature
    // (bd remora-4ei.53). Both patches it needs are defaultOn=false, so unappliedDefaultPatches —
    // which only speaks for the default set — could never have caught it either.
    // Declare a key here ONLY where the dependency is real: every entry becomes a build-time
    // warning, and a warning that fires on a working configuration is what teaches people to
    // ignore the whole class. Validated against the patch registry by a test, so a typo is a test
    // failure rather than a phantom warning about a patch nobody can enable.
    QStringList requiresPatches;
    // The releases a defaultOn applies to; empty = every release. The SourcePatch::defaultOnFor
    // treatment the comment on unappliedDefaultFeatures always said would be needed the moment a
    // default became release-specific — mirror_agent is that moment (bd remora-28ix.3.4): its
    // default is A17-only, because no A16 golden tag advertises the agent and A16 is out of plan
    // (epic note), so an unscoped default would flip every A16 profile to
    // needsSourceBuild overnight. defaultsSelectGappsTag is the tripwire that catches it.
    QVector<int> defaultOnFor;
};

struct ImageTag {
    QString tag;
    int android;             // major version (8 == 8.1)
    QString arch;
    QSet<QString> provides;  // build-feature keys this pre-built image satisfies
    QString note;
};

const QVector<Feature> &buildFeatures();
const Feature *findFeature(const QString &key);      // nullptr if unknown
const QVector<ImageTag> &imageTags();
const ImageTag *findImageTag(const QString &tag);    // exact tag match; nullptr if unregistered
// The version-scoped default set: universal defaults plus the ones whose defaultOnFor names this
// release. The no-arg form is the UNIVERSAL defaults only — for the callers with no version in
// hand, where a release-scoped feature must neither appear (it may not apply) nor nag.
QSet<QString> defaultBuildSet();
QSet<QString> defaultBuildSet(int androidVersion);

// Default-on feature keys that `enabled` does NOT contain — registered but never built in.
// Same pinned-list hole the source patches had (bd remora-4ei.38, which cost two builds): once a
// profile has written a non-empty features= to remorarc that list is an explicit pin, and a feature
// added to buildFeatures() afterwards never joins it. Milder here — a missing feature is usually an
// absent capability rather than a build that dies two hours in looking like the fix did not work —
// but just as silent (bd remora-4ei.44). An EMPTY `enabled` is not a pin, it is the codebase-wide
// "unset ⇒ defaults apply" that every caller honours, so it yields nothing.
//
// Reports UNIVERSAL defaults only. The moment the header comment always warned about arrived with
// mirror_agent (default-on for A17 alone): a release-scoped default reported here
// would nag every profile of the releases it does not belong to, so the scoped ones are excluded
// — their absence from a pin is a per-release judgement, not an oversight to report.
QStringList unappliedDefaultFeatures(const QSet<QString> &enabled);

// ─────────── the per-version registry (bd remora-82c.2) ───────────
// ONE table keyed by Android major. Version support used to be ~7 independent hardcoded literals —
// supportedAndroidVersions, lineageBranchForAndroid's minor map, releaseConfigFor, containerPatchSet,
// the imageTags upstream-prebuilt cutoff — and each was silently wrong when a new version missed it:
// an absent lineage branch became "repo init with no -b" (the manifest default, silently), a missing
// containerPatchSet applied the neighbour version's set before bd remora-p4p, a missing release
// config lunched nothing. They now all read from here, so adding a version is one row.
//
// What stays its own data, on purpose: the bespoke A16/A17 image-variant ladder (imageTags — feature
// combos, not a per-version scalar) and the per-patch SourcePatch::defaultOnFor judgement (moving it
// depends on the validity model, bd remora-82c.3). Those resisted derivation; these five did not.
struct AndroidRelease {
    int version;               // Android major (8 == 8.1)
    QString lineageMinor;      // LineageOS minor that ships it ("24.0"). Majors run android+7 but the
                               // minors are irregular (.1 era, 22.2), so they are spelled out. Every
                               // listed version has one today; empty would mean "no LineageOS branch".
    QString releaseConfig;     // lunch release config ("cp2a"); empty ⇒ Remora cannot source-build it
    QString lunchProduct;      // lunch product ("remora_x86_64"); versioned because the R1 tree
                               // (device/remora, bd remora-28ix.4) exists only where it was
                               // evaluated — both releases lunch it since bd remora-31sq. Empty
                               // exactly when releaseConfig is: neither half can lunch alone.
    QString containerPatchSet; // container-compat dir under container-patches/ ("android-17.0.0");
                               // empty ⇒ none, and the container-patch step refuses (bd remora-cbd)
    QString boardConfigDir;    // device tree dir holding the BoardConfig this version builds with
                               // ("device/remora/remora_x86_64"). STATED, not derived from
                               // lunchProduct: the vendor directory and the product name agree today
                               // by coincidence, and guessing one from the other is exactly how the
                               // CPU-tuning sed spent weeks editing the old device tree while A17 built
                               // from device/remora (bd remora-28ix.4 child). Empty ⇒ no board
                               // config to rewrite, and the arch-variant step must refuse rather
                               // than pick a default.
};
const QVector<AndroidRelease> &androidReleases();
const AndroidRelease *androidRelease(int version);   // nullptr if unknown

QVector<int> supportedAndroidVersions();             // {8,9,...,17}, from androidReleases()

// Which Android version a source ref (branch/tag name) builds, parsed from the three ref schemes
// we list (lineage-<NN> where NN = android + 7, android-<N>* / android<N>-* AOSP tags/branches,
// android16-release). nullopt for unversioned refs and the retired upstream ref forms.
std::optional<int> refAndroidVersion(const QString &ref);
// The LineageOS branch that ships the given Android version (17 → "lineage-24.0"); empty when the
// version predates lineage-15.1 / is unknown.
QString lineageBranchForAndroid(int androidVersion);

}  // namespace remora
