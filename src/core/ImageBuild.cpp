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
//
// magisk_root joined it (bd remora-6laf.2) on the inventory in docs/ADDONS.md: the build only
// PLACES its eleven files (seven BUILD_PREBUILT modules, four PRODUCT_COPY_FILES), with no
// sepolicy, no property and no compiled module. The "boot-flow change" it was once kept out for
// is magisk.rc itself, and init loads an rc from /system/etc/init/ whichever way it got there.
//
// arm_translate joined it (bd remora-6laf.5) once it was clear that the A17 tree builds none of
// the ARM side: the guest runtime, the translator and its proxies are all payload files, the
// staging assemble-image.sh does is already a COPY, and the only non-file part (the bridge
// properties) is a prop file init reads after the base's (stage_arm_layer, ndk-overlay.sh).
static const QSet<QString> kOverlayable = {"widevine_l3", "gapps", "magisk_root",
                                           "arm_translate"};

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
    if (feature == QLatin1String("magisk_root"))
        // The context's magisk/ is vendor/source-build/features/magisk after `fetch-payloads.sh
        // magisk`, so these are that tree's paths. Named file by file because the payload
        // directory also holds PROVENANCE.md, which a source build does not install either. No
        // file needs an executable bit in the image: magisk.rc runs both scripts through
        // /system/bin/sh, and magisk-setup.sh copies the binaries into its tmpfs and chmods them
        // there.
        return QStringLiteral(
            "# Magisk v30.7 (su + DenyList), started from init: the upstream binaries, apks and\n"
            "# scripts plus Remora's setup script, and the rc that runs them.\n"
            "COPY magisk/magisk/magisk magisk/magisk/magiskboot magisk/magisk/magiskinit \\\n"
            "     magisk/magisk/magiskpolicy magisk/magisk/busybox \\\n"
            "     magisk/magisk/magisk.apk magisk/magisk/stub.apk \\\n"
            "     magisk/magisk/util_functions.sh magisk/magisk/boot_patch.sh \\\n"
            "     magisk/magisk-setup.sh /system/etc/init/magisk/\n"
            "COPY magisk/magisk.rc /system/etc/init/magisk.rc");
    if (feature == QLatin1String("arm_translate"))
        // ndkguest/ is what vendor/source-build/stage-arm-layer.sh stages: the payload patched by
        // the same function a source build uses (finish_ndk_overlay), so the two cannot drift.
        return QStringLiteral(
            "# ARM translation (arm64 native bridge): the translator, its guest runtime, the\n"
            "# berberis flag, and the properties that turn the bridge on, from stage-arm-layer.sh.\n"
            "COPY ndkguest/system/ /system/\n"
            "COPY ndkguest/odm_dlkm/ /odm_dlkm/");
    return QString();
}

// Every file the magisk_root fragment copies, for the stage step to check before docker build
// does. Listed per file, not as the directory: a public clone carries the scripts but not the
// binaries (vendor/PAYLOADS.md), so the directory exists there and would pass.
static QStringList magiskPayloads() {
    QStringList p;
    for (const char *f : {"magisk", "magiskboot", "magiskinit", "magiskpolicy", "busybox",
                          "magisk.apk", "stub.apk", "util_functions.sh", "boot_patch.sh"})
        p << QStringLiteral("magisk/magisk/") + QLatin1String(f);
    p << QStringLiteral("magisk/magisk-setup.sh") << QStringLiteral("magisk/magisk.rc");
    return p;
}

const QStringList &addonFeatures() {
    static const QStringList a{QStringLiteral("arm_translate"), QStringLiteral("magisk_root"),
                               QStringLiteral("widevine_l3")};
    return a;
}

QString addonLayerDockerfile(const QStringList &addons, const QString &baseTag, int androidVersion,
                             const QString &baseId, const QString &rootId, const QString &recipe) {
    QSet<QString> want;
    for (const QString &a : addons)
        if (addonFeatures().contains(a)) want.insert(a);
    QStringList sorted(want.begin(), want.end());
    sorted.sort();
    // baseOverride with a remora-local: base (the Play layer) finds no registry entry, so every
    // add-on asked for is layered; the engine has already dropped the ones the registered base
    // carries, asking the registry under the base's own name.
    QString df = buildRecipe(want, androidVersion, QString(), QString(), QString(), baseTag).dockerfile;
    df += QStringLiteral("LABEL org.remora.addons.local_only=\"true\"\n"
                         "LABEL org.remora.addons.list=\"%1\"\n"
                         "LABEL org.remora.addons.base=\"%2\"\n"
                         "LABEL org.remora.addons.root=\"%3\"\n"
                         "LABEL org.remora.addons.recipe=\"%4\"\n")
              .arg(sorted.join(QLatin1Char(' ')), baseId, rootId, recipe);
    return df;
}

QString addonLayerTag(const QString &baseId, const QString &recipe) {
    QString b = baseId;
    if (b.startsWith(QLatin1String("sha256:"))) b = b.mid(7);
    return QStringLiteral("remora-local:addons-%1-%2").arg(b.left(12), recipe.left(12));
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
        if (f == QLatin1String("magisk_root")) payloads << magiskPayloads();
        // The prop file is written last, only after the patcher has accepted both binaries, so
        // its presence is what says the context was staged by stage-arm-layer.sh and not copied
        // raw: an unpatched bridge boots and then segfaults every ARM app (bd remora-rve).
        if (f == QLatin1String("arm_translate"))
            payloads << QStringLiteral("ndkguest/system/lib64/libnb.so")
                     << QStringLiteral("ndkguest/system/bin/arm64/linker64")
                     << QStringLiteral("ndkguest/system/etc/init/remora-berberis.rc")
                     << QStringLiteral("ndkguest/odm_dlkm/etc/build.prop");
    }
    const bool needsBuild = !overlayMissing.isEmpty();
    // A layer never inherits the public claim. Every overlay-able feature is one public_release
    // conflicts with, so any overlay is a private image by construction, and the base's
    // LABEL remora.public_release="true" would otherwise ride through into it — the label the
    // catalog verifier and package-release.py accept as "this is the publishable image".
    if (needsBuild) lines << QStringLiteral("LABEL remora.public_release=\"false\"");
    QString dockerfile = lines.join(QLatin1Char('\n')).trimmed() + "\n";

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
