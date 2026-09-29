#include "core/ImageBuild.h"

#include <QList>

#include "core/Features.h"
#include "core/Gating.h"

namespace remora {

// native_vulkan left this set with the feature (bd remora-xqt1): a build-time ro.hardware.vulkan
// is write-once and beats every runtime decision, so the ICD is a boot arg now, not an overlay.
//
// gapps joined it (bd remora-sgkg) once the overlay was shown to be EXACTLY what the source build
// produces, rather than assumed to be. features/gapps/Android.mk declares every module with
// LOCAL_DEX_PREOPT := false, so the build emits no oat/ next to the APKs and contributes no
// generated file at all — its whole output is the 14 payload files, laid out already in their
// install paths. Verified by sha256 against a booting source-built image: all 14 IDENTICAL. That
// retires this bead's "whether anything needs dexopt that a layer cannot provide" as a no.
static const QSet<QString> kOverlayable = {"widevine_l3", "gapps"};

static QString fragment(const QString &feature) {
    if (feature == QLatin1String("widevine_l3"))
        return QStringLiteral(
            "# Widevine L3 (AIDL, Android TV A13 blob) + its /vendor/lib64 dependency closure\n"
            "COPY wv/vendor/ /vendor/");
    if (feature == QLatin1String("gapps"))
        // /product and /system_ext are symlinks into /system in these images, so the COPY targets
        // are spelled through /system — a COPY to a symlinked path is resolved by the builder, but
        // spelling it out is what makes the layer readable next to the image's own layout.
        //
        // DELIBERATELY NOT SET: gapps.mk's `ro.control_privapp_permissions=enforce`. A COPY layer
        // cannot append to build.prop, and it does not need to. On A17 the property is inert —
        // measured on bd remora-4ei.7, where it was unset in every build.prop and system_server
        // STILL died in a restart loop with "not in privileged permission allowlist"; A17's
        // permission subsystem enforces unconditionally. Below A17 the property only decides
        // whether a MISSING entry boot-loops or degrades, so leaving it unset is the safer of the
        // two failure modes. Either way the allowlist itself rides in the payload and is
        // byte-identical to the one a baked image boots with, so there is no missing entry to
        // degrade over.
        return QStringLiteral(
            "# GApps (MindTheGapps-style prebuilts): the 5 APKs plus their permission and\n"
            "# sysconfig XMLs, byte-identical to what a source build installs.\n"
            "COPY gapps/product/ /system/product/\n"
            "COPY gapps/system_ext/ /system/system_ext/");
    return QString();
}

QString releaseConfigFor(int androidVersion) {
    // From the per-version registry (bd remora-82c.2). Exact, never a range — an unknown version
    // yields empty rather than the nearest neighbour, because the nearest neighbour is the bug:
    // bp2a on an Android 18 tree is an Android 16 release config, and nothing downstream would have
    // said so. A registry version with no releaseConfig (the prebuilt-only older ones) also yields
    // empty, which is correct — Remora does not source-build them.
    const AndroidRelease *r = androidRelease(androidVersion);
    return r ? r->releaseConfig : QString();
}

QString lunchConfigFor(int androidVersion) {
    // The FULL lunch string ("remora_x86_64-cp2a-userdebug"), from the same registry and with the
    // same exact-or-empty contract. The product half is versioned because the R1 device tree
    // (device/remora, bd remora-28ix.4) exists only where it was evaluated: A17 lunches Remora's
    // own tree, and A16 lunches it too since bd remora-31sq. Composed here so the recipe display and the
    // engine's BUILD_LUNCH cannot diverge.
    const AndroidRelease *r = androidRelease(androidVersion);
    if (!r || r->releaseConfig.isEmpty() || r->lunchProduct.isEmpty()) return QString();
    return QStringLiteral("%1-%2-userdebug").arg(r->lunchProduct, r->releaseConfig);
}

// The ImageSelection for an explicitly named base — what selectImage would return if this tag
// were the only candidate (bd remora-sgkg). Needed because selectImage ranks the whole registry
// by fewest MISSING features, so it always prefers a tag that already bakes what was asked for;
// there is no ranking in which "the clean image I actually downloaded" wins over the *_gapps twin
// registered beside it. An UNKNOWN tag is taken to provide NOTHING rather than being rejected:
// Remora genuinely cannot know what a hand-built or renamed image contains, and reporting every
// requested feature as missing is the honest reading — overlay-able ones are then layered on and
// the rest are named as needing a source build, which is exactly what an operator can act on.
static ImageSelection selectionForBase(const QSet<QString> &want, const QString &base) {
    QSet<QString> provides;
    for (const ImageTag &t : imageTags())
        if (t.tag == base) {
            provides = t.provides;
            break;
        }
    const QSet<QString> miss = want - provides;
    return {base, miss, !miss.isEmpty()};
}

ImageRecipe buildRecipe(const QSet<QString> &featuresIn, int androidVersion, const QString &tag,
                        const QString &sourceTree, const QString &sourceKind,
                        const QString &baseOverride) {
    // Ignore keys for features that no longer exist (e.g. a removed feature left stale in a saved
    // config) — an orphan key otherwise looks like an un-satisfiable request and perpetually forces
    // a source rebuild.
    QSet<QString> features;
    for (const QString &f : featuresIn)
        if (findFeature(f)) features.insert(f);
    // normalize() closes the set under `requires` before the comparison, exactly as selectImage
    // does internally — without it an override would measure the raw request against a tag's
    // already-closed provides and report a dependency as missing.
    const ImageSelection sel = baseOverride.isEmpty()
                                   ? selectImage(features, androidVersion)
                                   : selectionForBase(normalize(features), baseOverride);

    QStringList overlayMissing, unbuildable;
    for (const QString &f : sel.missing) {
        if (kOverlayable.contains(f))
            overlayMissing << f;
        else
            unbuildable << f;
    }
    overlayMissing.sort();
    unbuildable.sort();

    QStringList lines{QStringLiteral("FROM %1").arg(sel.tag), QString()};
    QStringList payloads;
    for (const QString &f : overlayMissing) {
        lines << fragment(f) << QString();
        if (f == QLatin1String("widevine_l3")) payloads << QStringLiteral("wv/vendor/");
        if (f == QLatin1String("gapps"))
            payloads << QStringLiteral("gapps/product/") << QStringLiteral("gapps/system_ext/");
    }
    QString dockerfile = lines.join(QLatin1Char('\n')).trimmed() + "\n";

    const bool needsBuild = !overlayMissing.isEmpty();
    const QString base0 = sel.tag.section(QLatin1Char(':'), 0, 0);
    const QString resultTag = !tag.isEmpty() ? tag : (base0 + ":custom");

    ImageRecipe r;
    r.baseTag = sel.tag;
    r.overlays = overlayMissing;
    r.unbuildable = unbuildable;
    r.dockerfile = dockerfile;
    r.tag = needsBuild ? resultTag : sel.tag;
    r.payloads = payloads;
    r.needsBuild = needsBuild;
    r.needsSourceBuild = !unbuildable.isEmpty();
    if (r.needsSourceBuild) {
        const QString tree = sourceTree.isEmpty() ? QStringLiteral("<source dir>") : sourceTree;
        // Lunch config per Android version. Shared with the engine, which passes it to the build
        // as BUILD_LUNCH — so what is displayed here and what is lunched cannot diverge.
        const QString lunch = lunchConfigFor(androidVersion);
        const QString cmd =
            sourceKind == QLatin1String("aosp")
                ? QStringLiteral("cd %1 && source build/envsetup.sh && "
                                 "lunch %2 && m")
                      .arg(tree, lunch)
                : QStringLiteral("cd %1 && ./do-build.sh  (lunch %2) "
                                 "— see docs/FEATURES.md, \"From a feature selection to an "
                                 "image\"")
                      .arg(tree, lunch);
        r.sourceBuildHint =
            QStringLiteral("%1 require a full source rebuild (not overlay-able): %2")
                .arg(unbuildable.join(QStringLiteral(", ")), cmd);
    }
    return r;
}

}  // namespace remora
