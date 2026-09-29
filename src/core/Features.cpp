#include "core/Features.h"

#include <QHash>

namespace remora {

const QVector<QPair<QString, QString>> &featureSections() {
    static const QVector<QPair<QString, QString>> v = {
        {QStringLiteral("base"), QStringLiteral("Base system")},
        {QStringLiteral("apps"), QStringLiteral("Apps and Google services")},
        {QStringLiteral("compat"), QStringLiteral("Compatibility")},
        {QStringLiteral("root"), QStringLiteral("Root and hiding")},
        {QStringLiteral("hw"), QStringLiteral("Hardware and media")},
    };
    return v;
}

QString featureSectionFor(const QString &category) {
    // gpu/drm/media/hw are one story to a reader — what the device can render and decode.
    if (category == QLatin1String("gpu") || category == QLatin1String("drm")
        || category == QLatin1String("media") || category == QLatin1String("hw"))
        return QStringLiteral("hw");
    for (const auto &s : featureSections())
        if (s.first == category) return s.first;
    return featureSections().last().first;
}

QString featureSectionForPatchGroup(const QString &group) {
    // The patch registry's own groups (patchGroups()) are apply-oriented; only the graphics one
    // has a feature-side counterpart. The rest are base-system work by any reading.
    return group == QLatin1String("gpu") ? QStringLiteral("hw") : QStringLiteral("base");
}

QString featureStatusToString(FeatureStatus s) {
    switch (s) {
        case FeatureStatus::Ok: return QStringLiteral("ok");
        case FeatureStatus::Caveat: return QStringLiteral("caveat");
        case FeatureStatus::Parked: return QStringLiteral("parked");
        case FeatureStatus::Deferred: return QStringLiteral("deferred");
        case FeatureStatus::Mandatory: return QStringLiteral("mandatory");
    }
    return QStringLiteral("ok");
}

const QVector<Feature> &buildFeatures() {
    static const QVector<Feature> f = {
        {"gapps", "Google Apps (Play Store/Services)", "apps", Stage::Build, true,
         {"arm_translate"}, {"microg"}, FeatureStatus::Ok,
         "No x86_64 A16 GApps exist — arm64 GApps need the native bridge."},
        {"microg", "microG (de-Googled Play services)", "apps", Stage::Build, false, {},
         {"gapps"}, FeatureStatus::Caveat,
         "GmsCore + Companion (priv-app) + Aurora Store storefront — the Play-free alternative "
         "to gapps (mutually exclusive). FCM push, check-in, location and Aurora installs work; "
         "signature spoofing is NOT wired yet, so apps hard-verifying the Play signature via "
         "the GMS client library still refuse (follow-up: spoof via the ReZygisk stack). "
         "AOSP rebuild only."},
        {"arm_translate", "ARM app translation (native bridge)", "compat", Stage::Build, true,
         {}, {}, FeatureStatus::Caveat,
         "libndk/berberis; abilist-order execution risk (upstream issue #669)."},
        {"system_updater", "In-Android system updater (host-applied)", "compat", Stage::Build,
         false, {}, {}, FeatureStatus::Ok,
         "Settings > System > Remora Updater; Apply signals the host over /rezmods.",
         // Without the overlay the stock 'System update' entry (GMS's OTA client) shadows the
         // injected RemoraUpdater tile — the feature ships looking broken (bd remora-82c.9).
         {"settings_hide_stock_updater"}},
        {"play_spoof", "Play spoof (Zygisk/PIF, per-app Pixel)", "compat", Stage::Build,
         false, {"magisk_root"}, {}, FeatureStatus::Ok,
         "Bakes the patched ReZygisk + PlayIntegrityFix stack into the image so Google Play sees a "
         "certified Pixel *inside GMS/Play only* (system build.prop untouched, adb stable). Dormant "
         "until the play_spoof runtime option turns it on — then bringup activates it automatically."},
        {"webview_pairip_fix", "WebView PairIP x86_64 fix", "compat", Stage::Build, true,
         {"arm_translate"}, {}, FeatureStatus::Caveat,
         "RECORDED, NOT IMPLEMENTED HERE. This entry was mined from the predecessor catalog as a "
         "property the pre-built A16 golden images already had; Remora has never carried an "
         "implementation. A from-scratch source build delivers nothing named pairip — no source "
         "patch, no staged feature, no build step (bd remora-4ei.55). Kept rather than deleted "
         "because a fix baked into the pre-built tags is still a real capability for anyone using "
         "them, and dropping the entry would silently erase the record of it. The mechanism was "
         "most likely abilist ordering (the sibling arm_translate entry's 'upstream #669' risk), "
         "and x86_64-first is already the platform default on this target with nothing in "
         "the device tree or assemble-image.sh overriding it — so on a source build there may be "
         "nothing left to do. Do not read ChatGPT launching as proof either way: it runs natively "
         "on the source-built A17 image, but that build ships no pairip lib at all, so it no "
         "longer tests what it was chosen to test."},
        {"camera_v4l2", "Camera (external UVC/V4L2 provider)", "hw", Stage::Build, false, {}, {},
         FeatureStatus::Caveat,
         "Builds AOSP's OWN external camera provider (android.hardware.camera.provider-V1-external-"
         "service) plus /vendor/etc/external_camera_config.xml and the camera.external feature "
         "declaration. Nothing here is a hand-written HAL: the image shipped cameraserver with no "
         "provider at all, which is why 'dumpsys media.camera' reported 0 devices (bd remora-4ei.9). "
         "TWO HALVES, and this feature is only the build one — the provider enumerates /dev/video* "
         "at RUNTIME, so the container must also be handed a node at docker run — the resolver "
         "passes the probed /dev/video* nodes when the profile carries this feature. A node that "
         "appears on the docker host later needs a container RECREATE, not a restart, because "
         "devices are bound at create time.",
         },
        {"usb_audio", "Microphone (USB audio HAL)", "hw", Stage::Build, false, {}, {},
         FeatureStatus::Caveat,
         "Builds AOSP's OWN tinyalsa USB audio HAL (audio.usb.default), its policy fragment and "
         "tinycap. Nothing here is a hand-written HAL (bd remora-4ei.37). "
         "WHAT IT FIXES IS SILENCE, NOT A MISSING FEATURE: 'pm list features' already reports "
         "android.hardware.microphone and an AudioRecord on AudioSource.MIC already SUCCEEDS — but "
         "the Built-In Mic it resolves to is served by AOSP's STUB HAL, whose in_read() memsets the "
         "buffer to zero on a faked clock. Apps therefore record perfectly timed silence and report "
         "no error, which is why this went unnoticed. "
         "THE DECLARATION IS THE LOAD-BEARING HALF and it lives in the usb_audio_policy SOURCE "
         "PATCH, not here: AudioPolicyManager opens exactly the modules the top-level policy "
         "includes, so the HAL alone changes nothing. The two ship together or not at all — the "
         "patch's include names the fragment this feature installs, and the feature's HAL is inert "
         "without the patch's include. "
         "UNLIKE THE CAMERA there is no runtime half to arrange — the container runs privileged and "
         "already carries every /dev/snd node, and Android already sees each card in /proc/asound. "
         "Which microphone Android picks is the HAL's own enumeration; it is not selectable here.",
         // Not prose, because prose is what nothing reads — the same lesson sensors cost a live
         // image for (bd remora-4ei.53). Without the patch this feature installs a HAL that is
         // never opened, which is indistinguishable from working until somebody listens.
         {"usb_audio_policy"}},
        {"widevine_l3", "Widevine L3 DRM", "drm", Stage::Build, false, {}, {}, FeatureStatus::Ok,
         "-wv overlay only; L1 impossible (no TEE in a container)."},
        // The ONE feature whose payload Remora builds itself rather than downloading, because
        // there is nowhere to download it from: Mesa has no soong build (external/mesa3d's
        // Android.bp defines only gfxstream infrastructure — no libEGL_mesa, no vulkan.<driver>,
        // no libgallium_dri, no libgbm, no libglapi), so the drivers the image ships were
        // cross-built out of tree by someone else and have been prebuilts ever since.
        // UN-PARKED (bd remora-ykhz.1): the un-park condition — booted on amd-host-a
        // or amd-host-b, where these fifteen files are the entire GPU stack — was met the hard
        // way. amd-host-a's daily container ADOPTED the Mesa-26 image on the RX 580: radeonsi GL
        // composing, RADV reporting the card by name (amdgpu.ids), hardware VAAPI encode under
        // motion through the source stack, gralloc=cros translated in-image to minigbm_gbm_mesa
        // (the cros/DRI conflict that gated this — see gpu_config.sh and bd remora-ykhz.1).
        // It had sat Parked because the machine that can build this cannot exercise it: all
        // fifteen prebuilts read ZERO processes on the NVIDIA/Venus host.
        // REQUIRED FOR AN A17 SOURCE BUILD SINCE (bd remora-28ix.4), AND DELIBERATELY
        // STILL defaultOn=false. Dropping the prebuilt GPU-driver project from the A17 manifest
        // removed the second source of every GL driver and Vulkan ICD, so on that release this
        // stopped being an upgrade over Mesa 24.0.8 and became the only stack there is: with it off
        // an A17 build now yields an image with no libgallium_dri, no libEGL_mesa, no GLES pair and
        // no ICD but lavapipe and pastel — and it builds perfectly cleanly.
        //
        // TWO OBVIOUS WAYS TO EXPRESS THAT ARE BOTH WRONG, and the test suite caught both:
        //  * FeatureStatus::Mandatory is a GUI affordance (checkbox forced on and greyed) with NO
        //    version scope. When this was written that mattered because A16 still had the prebuilt
        //    fallback; since the A16 carry-over (bd remora-31sq) both releases genuinely require
        //    the feature and the cmdSourceBuild gate fires for both — but Mandatory stays wrong
        //    anyway, because it is cosmetic-only and enforces nothing.
        //  * defaultOn=true with defaultOnFor={17} looks like the mirror_agent precedent and is
        //    not: mesa_source is Stage::Build and cannot be overlaid, so flipping it default-on
        //    makes the A17 DEFAULT SET need a source build. a17ResolvesToItsSourceBuiltImage exists
        //    to forbid exactly that — the A17 default must keep resolving to an already-tested
        //    image tag instead of committing every profile to an hour of compiling.
        //    everyDefaultFeatureAppliesToEveryRemoraRelease fired too, on an unscoped first attempt.
        //
        // The constraint is on BUILDING FROM SOURCE, not on what a profile defaults to, so it is
        // enforced where builds happen and nowhere else: cmdSourceBuild refuses in about a second,
        // and device-remora/remora.mk carries a $(error) gated on TARGET_PRODUCT that a manual
        // do-build.sh run hits about two minutes in. Neither changes what anyone's profile means.
        {"mesa_source", "Mesa from source (replaces the 24.0.8 prebuilts)", "gpu", Stage::Build,
         false, {}, {}, FeatureStatus::Ok,
         "Builds Mesa 26.x for Android x86_64 with its own meson + an NDK cross-file "
         "(vendor/host-prereqs/mesa-android/build-mesa.sh) and installs it over the prebuilt "
         "Mesa 24.0.8, which is what every shipped GL and Vulkan driver currently is. TWO "
         "INDEPENDENT HALVES, and the difference matters: the Vulkan ICDs are standalone drivers "
         "the loader dlopens by name and can be replaced ONE AT A TIME, while the GL/DRI cluster "
         "(libgallium_dri, libEGL_mesa, the GLES pair, libgbm, libglapi, libgallium_drv_video) "
         "must move TOGETHER — the DRI loader ABI changed between 24.0.8 and 26.x, so a mixed "
         "pair links and installs and then fails at runtime. mesa.mk refuses a partial GL payload "
         "rather than shipping one. Costs ~120 MB of image growth from statically linked LLVM 20 "
         "(radeonsi only; RADV and iris need none). gralloc.cros/gralloc.gbm are NOT in scope — "
         "they are minigbm, not Mesa."},
        // No native_vulkan entry any more (bd remora-xqt1). Its entire payload was one line —
        // ro.hardware.vulkan=intel appended to /vendor/build.prop — and that line is now actively
        // harmful: ro.* is write-once and build.prop is loaded before any init action, so a
        // BUILD-TIME pin silently overrode every runtime decision. It made both AMD hosts read
        // intel while running amdgpu, and it stopped guest mode booting at all by beating
        // gpu_setup_guest's pastel. The driver is a per-deploy fact, not a per-image one, so it
        // moved to the host as androidboot.remora_vulkan; see Resolver's vulkanIcdFor. A key left
        // in an old profile's features= is dropped by buildRecipe's orphan filter, not refused.
        {"magisk_root", "Magisk root (su + DenyList)", "root", Stage::Build, true, {}, {},
         FeatureStatus::Ok, "su + native DenyList unmount both work."},
        // No zygisk_hiding entry any more: it sat Parked from the day it was registered — the
        // deep-hiding pass it was parked for never happened, and play_spoof carries the fixed
        // ReZygisk injector, which is all anything actually needed. A key left in an old
        // profile's features= is dropped by buildRecipe's orphan filter, not refused.
        {"shamiko", "Shamiko denylist unmount", "root", Stage::Build, false,
         {"magisk_root", "play_spoof"}, {}, FeatureStatus::Caveat,
         "Unmounts Magisk's modifications from denylisted processes — the one hiding job "
         "remora_devicespoof cannot do, since that only rewrites properties. Vendored at v1.2.5 "
         "(414) and staged into the play_spoof stack, which is why it REQUIRES play_spoof: without "
         "ReZygisk and the Magisk denylist it installs a Zygisk module with no Zygisk. Its own "
         "toggle all the same, because bd remora-4ei.16 named Shamiko a prime suspect for breaking "
         "Play Store verification on A16 — 'PIF but no Shamiko' has to stay expressible. "
         "SHIPPED WITHOUT upstream's service.sh: that would set ro.debuggable=0 and ro.adb.secure=1 "
         "and take adb — Remora's whole control path — with it. Property spoofing is "
         "remora_devicespoof's job alone (bd remora-4ei.50, see shamiko/REMORA.md)."},
        // The native_vulkan requirement is GONE with the feature (bd remora-xqt1), and dropping it
        // loses nothing this ever depended on: c2-va is a VA-API/libva codec on iHD, it does not
        // call Vulkan, and what the edge really encoded was "you are on the VF GPU path". That is
        // a deploy-time property the host now resolves per boot, so a build-time feature was never
        // the right place to assert it — and asserting it there made hw_video_decode unselectable
        // without a flag whose only effect was pinning an ICD name.
        {"hw_video_decode", "HW video decode (VA Codec2)", "media", Stage::Build, false,
         {}, {}, FeatureStatus::Caveat,
         "Authored c2.remora.vaapi.* on iHD/libva (not Mesa gallium); AOSP rebuild only "
         "(VNDK-bound, not overlayable) + a real host GPU. Helps clear content; Widevine L3 "
         "decrypts to normal buffers, so no secure-buffer block. See docs/HW-VIDEO-DECODE.md."},
        {"sensors", "Sensors (accel/gyro, host-injectable)", "hw", Stage::Build, false, {}, {},
         FeatureStatus::Ok,
         "AOSP example sensors HAL as a vendor APEX + remora property injection "
         "(debug.remora.sensor.*) — auto-rotate and motion games; `remora rotate` drives it. "
         "Baked into the -hwc2 golden image (verified live); a from-scratch build needs the "
         "sensors_hal + sensors_product source patches.",
         // The prose above said exactly this before requiresPatches existed, and prose is what
         // nothing reads: the A17 source build shipped with neither patch and dumpsys answered
         // "No Sensors on the device" (bd remora-4ei.53). A device reporting zero sensors is also
         // a textbook emulator signal, so the silence cost more than auto-rotate.
         {"sensors_hal", "sensors_product"}},
        {"lineage_frameworks", "LineageOS frameworks", "base", Stage::Build, true, {}, {},
         FeatureStatus::Ok, "LineageOS 23 parts vs plain AOSP."},
        {"container_compat_patches", "Container-compat patch set", "base", Stage::Build, true, {},
         {}, FeatureStatus::Mandatory,
         "binder auto-alloc, selinux-stub, seccomp-off, ro-prop-override — won't boot without them."},
        // Default-on for A17 ONLY (defaultOnFor — the flip, bd remora-28ix.3.4): the
        // A17 source tags advertise it and the bake is live-verified. Never unscoped: no A16
        // golden tag carries the agent and A16 is out of plan, so a universal default would flip
        // every A16 profile to needsSourceBuild — defaultsSelectGappsTag guards that.
        {"mirror_agent", "In-image mirror agent", "base", Stage::Build,
         true, {}, {}, FeatureStatus::Ok,
         "Bakes the Remora agent into the image as a boot service — the mirror's device half, and "
         "the only one. It captures the display, injects input, carries audio and answers the app "
         "list, so an image without it cannot be mirrored at all. Default-on for Android 17, "
         "where every source-built tag carries it.",
         {}, {17}},
    };
    return f;
}

const Feature *findFeature(const QString &key) {
    for (const Feature &f : buildFeatures())
        if (f.key == key) return &f;
    return nullptr;
}

const ImageTag *findImageTag(const QString &tag) {
    for (const ImageTag &t : imageTags())
        if (t.tag == tag) return &t;
    return nullptr;
}

QSet<QString> defaultBuildSet() {
    QSet<QString> s;
    for (const Feature &f : buildFeatures())
        if (f.defaultOn && f.defaultOnFor.isEmpty()) s.insert(f.key);
    return s;
}

QSet<QString> defaultBuildSet(int androidVersion) {
    QSet<QString> s = defaultBuildSet();
    for (const Feature &f : buildFeatures())
        if (f.defaultOn && f.defaultOnFor.contains(androidVersion)) s.insert(f.key);
    return s;
}

QStringList unappliedDefaultFeatures(const QSet<QString> &enabled) {
    if (enabled.isEmpty()) return {};  // unset ⇒ defaults apply; nothing is pinned away
    QStringList out;  // registry order, so the report reads the way the feature list does
    // Universal defaults only: a release-scoped default (mirror_agent, A17) nagging a profile of
    // another release is exactly the noise the header comment promised to prevent — the caller
    // has no version to hand here, so the scoped ones stay out of the report entirely.
    for (const Feature &f : buildFeatures())
        if (f.defaultOn && f.defaultOnFor.isEmpty() && !enabled.contains(f.key)) out << f.key;
    return out;
}

const QVector<AndroidRelease> &androidReleases() {
    // version, lineageMinor, releaseConfig, lunchProduct, containerPatchSet, boardConfigDir.
    // Source-build assets (releaseConfig + lunchProduct + containerPatchSet) exist only for 16/17 —
    // the versions Remora builds. 8-15 carry a LineageOS minor and nothing else: they are NAMED but
    // not built, and since the upstream image offerings were removed selecting one is
    // refused by the "no release config" check rather than silently resolving someone else's image.
    // They are kept so the refusal can name the version, and so adding assets is all it takes.
    // BOTH BUILT RELEASES NOW LUNCH remora_x86_64 FROM device/remora. A16 was the
    // last thing still building against the upstream fork, and carrying it over is what let the
    // per-version arms elsewhere be deleted rather than duplicated: the tag family, the local
    // manifest glob and the container-patch directory each had a `version == 16 ?` branch that
    // existed only because A16 was named differently. What stays per-version is inherent rather
    // than historical — the release config, the pinned manifest and the container-compat patch set,
    // because AOSP itself moves between releases. Adding A18 is a row here plus those two assets.
    static const QVector<AndroidRelease> r = {
        {8, QStringLiteral("15.1"), {}, {}, {}},
        {9, QStringLiteral("16.0"), {}, {}, {}},
        {10, QStringLiteral("17.1"), {}, {}, {}},
        {11, QStringLiteral("18.1"), {}, {}, {}},
        {12, QStringLiteral("19.1"), {}, {}, {}},
        {13, QStringLiteral("20.0"), {}, {}, {}},
        {14, QStringLiteral("21.0"), {}, {}, {}},
        {15, QStringLiteral("22.2"), {}, {}, {}},
        {16, QStringLiteral("23.0"), QStringLiteral("bp2a"), QStringLiteral("remora_x86_64"),
         QStringLiteral("android-16.0.0_r2"),
         QStringLiteral("device/remora/remora_x86_64")},
        {17, QStringLiteral("24.0"), QStringLiteral("cp2a"), QStringLiteral("remora_x86_64"),
         QStringLiteral("android-17.0.0"),
         QStringLiteral("device/remora/remora_x86_64")},
    };
    return r;
}

const AndroidRelease *androidRelease(int version) {
    for (const AndroidRelease &a : androidReleases())
        if (a.version == version) return &a;
    return nullptr;
}

QVector<int> supportedAndroidVersions() {
    QVector<int> v;
    for (const AndroidRelease &a : androidReleases()) v << a.version;
    return v;
}

std::optional<int> refAndroidVersion(const QString &ref) {
    // lineage-<NN>… — LineageOS majors run android + 7 (15.1→8.1 … 24→17)
    if (ref.startsWith(QLatin1String("lineage-"))) {
        const int major = ref.mid(8).section(QLatin1Char('.'), 0, 0)
                              .section(QLatin1Char('-'), 0, 0).toInt();
        return major >= 15 ? std::optional<int>(major - 7) : std::nullopt;
    }
    // android-<N>… (AOSP tags: android-16.0.0_r1) / android<N>-… (branches: android16-release)
    if (ref.startsWith(QLatin1String("android"))) {
        QString rest = ref.mid(7);
        if (rest.startsWith(QLatin1Char('-'))) rest = rest.mid(1);
        int n = 0;
        while (n < rest.size() && rest.at(n).isDigit()) ++n;
        if (n > 0) return rest.left(n).toInt();
        return std::nullopt;
    }
    // Any other ref form (e.g. <vendor>-16.0.0) resolves to no version, which the callers already
    // handle as "unknown" and refuse on, rather than silently mapping a branch name that points at
    // nothing Remora syncs to a release it would then try to build.
    return std::nullopt;
}

QString lineageBranchForAndroid(int androidVersion) {
    // From the registry: unknown version or a version with no LineageOS minor → empty, which the
    // caller reads as "no branch" (an empty ref is repo's default branch, so callers must not pass
    // it on blind — bd remora-82c.2 flagged that gap).
    const AndroidRelease *r = androidRelease(androidVersion);
    if (!r || r->lineageMinor.isEmpty()) return QString();
    return QStringLiteral("lineage-") + r->lineageMinor;
}

const QVector<ImageTag> &imageTags() {
    static const QVector<ImageTag> t = [] {
        QVector<ImageTag> v;
        // --- custom LineageOS-23 / A16 tags (the rich feature set lives here) ---
        v.append({"remora23:x86_64", 16, "x86_64",
                  {"arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches"},
                  "LineageOS 23 / A16, no GApps."});
        v.append({"remora23:x86_64-gapps", 16, "x86_64",
                  {"gapps", "arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root"},
                  "+ GApps + Magisk. Default A16 image."});
        v.append({"remora23:x86_64-gapps-wv", 16, "x86_64",
                  {"gapps", "arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root", "widevine_l3"},
                  "+ Widevine L3."});
        v.append({"remora23:x86_64-gapps-wv-hwc2", 16, "x86_64",
                  {"gapps", "arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root", "widevine_l3",
                   "hw_video_decode", "sensors"},
                  "+ VAAPI HW video decode (c2.remora.vaapi.*) + host-injectable sensors HAL. "
                  "Deploys with use_remora_c2=1."});
        // No registered tag provides shamiko, so the consequence is worth naming: asking for it
        // resolves to no pre-built image and demands a source build, which is where it is baked
        // anyway (the shamiko feature is Stage::Build and rides the play_spoof/ReZygisk stack).
        // --- A17, built from source (LineageOS ship no A17 prebuilt) ---
        // WITHOUT these the registry has NO A17 entry at all, so selectImage() falls to its
        // "(no pre-built image for A17)" sentinel even with the image sitting in `docker images`.
        // The Image page then reports that nothing resolves immediately after a SUCCESSFUL build,
        // and the deploy's own advice — "build one from source (Image panel → Build from source)"
        // — is precisely what was just done. Registering the built tags closes that loop.
        // These are the ACTIVE tags, deliberately WITHOUT the "-src" suffix. A source build first
        // produces "<family>:x86_64<variant>-src" (MainWindow's builtTag), and onPromoteImage()
        // then retags it by stripping "-src" — that promotion is what makes a build deployable.
        // So the resolver must know the PROMOTED name; registering the -src staging tags instead
        // would resolve to an image that only exists between building and promoting. The A16
        // entries above follow the same rule, which is why none of them carry -src either.
        // Feature sets mirror the A16 ladder: same source tree, same WITH_* toggles.
        // remora24 is the A17 family since the identity migration (bd remora-28ix.4): images
        // built from the Remora-owned device tree (lunch remora_x86_64).
        v.append({"remora24:x86_64", 17, "x86_64",
                  {"arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root", "sensors",
                   "system_updater", "mirror_agent"},
                  "Remora A17 (LineageOS 24 base), built from source. No GApps."});
        v.append({"remora24:x86_64-gapps", 17, "x86_64",
                  {"gapps", "arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root", "sensors",
                   "system_updater", "play_spoof", "mirror_agent"},
                  "+ GApps."});
        v.append({"remora24:x86_64-gapps-wv", 17, "x86_64",
                  {"gapps", "arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root", "widevine_l3",
                   "sensors", "system_updater", "play_spoof", "mirror_agent"},
                  "+ Widevine L3 (DRM video)."});
        v.append({"remora24:x86_64-gapps-wv-hwc2", 17, "x86_64",
                  {"gapps", "arm_translate", "webview_pairip_fix", "lineage_frameworks",
                   "container_compat_patches", "magisk_root", "widevine_l3",
                   "hw_video_decode", "sensors", "system_updater", "play_spoof", "mirror_agent"},
                  "+ VAAPI HW video decode (c2.remora.vaapi.*) + host-injectable sensors HAL. "
                  "Deploys with use_remora_c2=1."});
        // ONLY IMAGES THIS PROJECT BUILDS ARE REGISTERED. No pre-rename A17 spelling is recognised
        // (bd remora-28ix.4 step 4): an image old enough to carry one predates the agent bake, so
        // it cannot be mirrored at all and has to be rebuilt regardless. Registering it would only
        // let it resolve capabilities it can no longer deliver.
        //
        // VERSIONS 8-15 STAY IN THE REGISTRY WITH NO IMAGE TAG, a policy call rather than a
        // cleanup: they have no releaseConfig, no lunchProduct and no container patch set, so
        // nothing in this project builds them. The consequence is stated rather than discovered:
        // selecting 8-15 finds no image tag, falls through to the source path, and is REFUSED by
        // the existing "no release config for Android N" check. That is a
        // clear refusal instead of a silent wrong answer, and it keeps the version list honest about
        // what Remora knows how to name while making plain that it does not build them. Adding
        // source-build assets for one of them is all it would take to make it real.
        return v;
    }();
    return t;
}

}  // namespace remora
