#pragma once
#include <QSet>
#include <QString>
#include <QStringList>

namespace remora {

struct ImageRecipe {
    QString baseTag;
    QStringList overlays;      // overlay-able features layered on the base
    QStringList unbuildable;   // requested features no tag has and no overlay can add
    QString dockerfile;
    QString tag;
    QStringList payloads;      // build-context paths the operator must stage
    bool needsBuild = false;
    // Source-rebuild delivery: unbuildable features need a full AOSP tree build, not a Dockerfile
    // overlay. When set, sourceBuildHint carries the concrete command sequence to run.
    bool needsSourceBuild = false;
    QString sourceBuildHint;
};

// Reproduces the predecessor overlay model (IMAGE-RECIPE.md): a custom image is a Dockerfile-COPY
// overlay on a base tag. widevine_l3 and gapps are overlay-able; the rest is base-baked.
// gapps is what makes the clean-mirror policy deliver (ADR-0001, bd remora-sgkg): a user can take
// a published GApps-free image and apply the proprietary half locally in seconds, instead of
// being told to run an hour-scale source build. arm_translate is the same shape and the same
// argument, but is not done here. magisk_root is NOT this shape at all — it patches init and the
// boot flow rather than dropping files into a partition.
// sourceTree/sourceKind parameterize the source-rebuild hint for unbuildable features:
// kind "lineage" (default) uses the do-build.sh wrapper, "aosp" the stock envsetup/lunch/m flow.
// androidVersion picks the lunch release config (16→bp2a, 17→cp2a) in either flow.
//
// baseOverride names the base to layer onto instead of letting selectImage rank the registry.
// That ranking scores by fewest MISSING features, so a request containing gapps always resolves
// to the *_gapps tag registered beside the clean one and the overlay can never be reached — the
// override is what lets an operator say "this is the image I have, add the rest to IT". An
// unknown tag is treated as providing nothing (see selectionForBase).
ImageRecipe buildRecipe(const QSet<QString> &features, int androidVersion = 16,
                        const QString &tag = QString(), const QString &sourceTree = QString(),
                        const QString &sourceKind = QString(),
                        const QString &baseOverride = QString());

// The AOSP release config (the middle field of remora_x86_64-<rel>-userdebug) for an Android
// version. EXACT per version, empty for one we have not been told about.
//
// THE SINGLE SOURCE OF TRUTH, and it has to be: do-build.sh derived the same mapping itself with
// `case ANDROID_VER in 17) cp2a ;; *) bp2a ;;` while ImageBuild used `>= 17 ? cp2a : bp2a`, so the
// two AGREED only for 16 and 17. On 18 the recipe shown to the user said cp2a and the build would
// have lunched bp2a — an Android-16 release config — with nothing reporting the difference. The
// engine now passes the value here to the build as BUILD_LUNCH, which do-build.sh already honours
// as an override, so the shell case is a fallback for hand-runs rather than a second opinion.
QString releaseConfigFor(int androidVersion);
// The full lunch string ("remora_x86_64-cp2a-userdebug"); empty when the version cannot be
// source-built. The product half is per-version: A17 lunches the Remora-owned device tree
// (bd remora-28ix.4); A16 followed with the carry-over (bd remora-31sq).
QString lunchConfigFor(int androidVersion);

}  // namespace remora
