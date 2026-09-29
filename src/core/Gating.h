#pragma once
#include <QDate>
#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

#include "core/SourcePatches.h"  // VendorReader, for featureVersionGaps' asset-derived check

namespace remora {

struct Gate {
    QString key;
    bool ok = true;
    QString reason;
    QString severity;  // info | warn | block
};

struct ImageSelection {
    QString tag;               // best-covering pre-built tag (or a placeholder if none for the version)
    QSet<QString> missing;     // requested features no tag has → a custom build would be needed
    bool needsBuild = false;
};

// Close a requested build-feature set under `requires` (auto-add hard deps).
QSet<QString> normalize(const QSet<QString> &requested);

// Per-feature availability: unmet requires / conflicts / status (parked→block, deferred→warn, …).
QVector<Gate> featureGates(const QSet<QString> &requested);

// One enabled feature and the source patches it declares but this build will not apply.
struct FeaturePatchGap {
    QString feature;
    QStringList missingPatches;
};

// Enabled features whose Feature::requiresPatches are not all in the enabled patch set — i.e. a
// build that will complete green and ship the feature doing nothing.
//
// THIS IS THE THIRD INSTANCE OF ONE SHAPE, which is why it is a registry rule and not another
// note: webview had a valid provider installed and never selected (bd remora-4ei.48), c2-va had
// two sources of truth and the staged copy silently won (bd remora-4ei.42), and sensors had the
// feature enabled with neither of its patches (bd remora-4ei.53). Each was a green build
// delivering less than the config claimed, and each was found by accident, live, long after.
//
// EMPTY MEANS "UNSET ⇒ DEFAULTS APPLY" ON BOTH INPUTS, exactly as everywhere else (see Config.h),
// so both are resolved before comparing. That distinction is the whole point on the patch side:
// unappliedDefaultPatches returns nothing for an empty list because a profile that pinned nothing
// has pinned nothing away — but here an empty list resolves to defaultSourcePatchSet(), and a
// feature's required patches are typically defaultOn=FALSE (both of sensors' are), so they are
// genuinely absent and must be reported. Reading empty as "nothing to say" would blind this to
// every fresh profile, which is the case most likely to hit it.
// Reported in feature-registry order; a feature with nothing missing is omitted entirely.
QVector<FeaturePatchGap> featurePatchGaps(const QSet<QString> &enabledFeatures,
                                          const QStringList &enabledPatches, int androidVersion);

// One requested feature that CANNOT be built on the target Android version, because a source patch
// it requires does not apply there — no assets for the version, or a SKIP series absorbed upstream.
// This is a harder failure than featurePatchGaps: that one says "enable the patch"; this says the
// patch is not enable-able at all here, so the feature is off the menu on this version (bd
// remora-82c.3 — the FEATURE half of the validity model).
struct FeatureVersionGap {
    QString feature;
    QStringList inapplicablePatches;  // required patches with nothing to apply on this version
};

// Requested features whose requiresPatches include one that does not apply to `androidVersion`,
// DERIVED from the on-disk assets (via applicableSourcePatchKeys), never declared per feature —
// so a new version needs no feature edit, the answer follows the assets (the design recorded on
// bd remora-82c.3). Empty features resolve to defaultBuildSet(), same as featurePatchGaps. The
// reader is injected so the pure core stays testable offline.
QVector<FeatureVersionGap> featureVersionGaps(const QSet<QString> &enabledFeatures,
                                              int androidVersion, const VendorReader &read);

// Patch key → the requested feature(s) that require it, ", "-joined. The other half of
// featurePatchGaps: gaps warn when a required patch is NOT enabled, this names the ones that ARE
// enabled and must therefore actually LAND. buildApplyPatchCommands turns a conflict on one of
// these into a hard failure instead of a green build that ships the feature missing (bd
// remora-82c.1). Empty features resolve to defaultBuildSet(), same as featurePatchGaps.
QMap<QString, QString> featureRequiredPatchOwners(const QSet<QString> &enabledFeatures);

// Everything irrelevantPatches() is allowed to judge a patch against. A struct rather than four
// parameters because callers keep growing: the GUI, `plan`, and the pre-build modal all need the
// same answer, and a positional bool list is how two of them end up asking a subtly different
// question.
struct PatchProfile {
    QSet<QString> features;      // enabled build features (already resolved — see below)
    QStringList enabledPatches;  // enabled patch keys (already resolved)
    bool lineageTree = true;     // source_kind != aosp, i.e. the Lineage projects are synced
    bool venus = false;          // the Venus/NVIDIA vtest path is configured
};

// Patch keys that cannot do anything under this profile → a short reason, for the UI to show.
//
// HIDES, NEVER DISABLES. The returned keys are dropped from the PAGE; the enabled set is not
// touched, because source_patches is a pin and clearing it on a transient view change destroys a
// choice that switching back cannot restore. A key here that is still enabled therefore still
// applies, and the caller is expected to say so — silently applying a patch the page has stopped
// showing is the same silent-delivery bug this whole area keeps producing (bd remora-4ei.42/.53).
//
// EMPTY MEANS "UNSET ⇒ DEFAULTS APPLY" on both list inputs, as everywhere else (see Config.h), so
// resolve them before calling — an unresolved empty enabledPatches would make every needs.patch
// dependency read as unmet and hide entries a fresh profile does apply.
//
// Deliberately NOT version-scoped: applicability by Android version is a separate, asset-derived
// question that patchAppliesTo() already answers, and folding the two together would make it
// impossible to tell "no assets for this release" from "your profile does not want this".
QMap<QString, QString> irrelevantPatches(const PatchProfile &profile);

// The vendored PlayIntegrityFix profile, judged by age. Pure: the caller reads the file (it lives
// in REMORA_VENDOR_DIR, possibly on a remote build host), this decides whether to complain.
//
// WHY THIS EXISTS AS A CHECK RATHER THAN A REMINDER TO REFRESH: the pinned profile reached 15
// months old and Google's blocklist before anyone noticed, and every image built in that window
// re-baked it silently (bd remora-4ei.47). "Refresh it and keep it fresh" is exactly the kind of
// standing obligation that had already failed once by the time it was written down.
//
// A BANNED PROFILE IS WORSE THAN NO PROFILE, which is why staleness is worth a warning even now
// that Play Integrity is settled as unreachable here (bd remora-4ei.51): a blocklisted fingerprint
// is a positive tamper signal, where an unspoofed device is merely an unknown one. Nothing about
// this check claims a fresh profile would pass.
struct PifProfileAge {
    QString securityPatch;  // the SECURITY_PATCH as found; empty when the JSON carries none
    int monthsOld = -1;     // whole months to `today`; -1 when the profile cannot be dated at all
};
// nullopt when the profile is younger than maxMonths — and when `pifJson` is empty, because
// "nothing vendored" is a different problem from "what is vendored is old" and belongs to whoever
// enabled the feature. An undatable profile IS reported: one you cannot date is one you cannot
// call fresh.
std::optional<PifProfileAge> stalePifProfile(const QString &pifJson, const QDate &today,
                                             int maxMonths = 6);

// Pick the pre-built tag for `androidVersion` that best covers the requested build-features.
ImageSelection selectImage(const QSet<QString> &requested, int androidVersion = 16);

// True for selectImage's "(no pre-built image for A17)"-style sentinel — honest in plan output
// but never a real tag, so anything that would hand the tag to docker must check this first.
// (A legal docker reference can't start with '(': repository names are [a-z0-9._/-].)
bool isPlaceholderImageTag(const QString &tag);

}  // namespace remora
