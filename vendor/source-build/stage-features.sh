#!/bin/sh
# Stage the à-la-carte prebuilt features (GApps / ARM translation / Magisk) into a source tree
# before `m`, and wire remora_x86_64.mk to inherit them. Runs AFTER the patch reset/apply, so
# the inheritance lines it appends survive (the reset pass wipes ad-hoc edits). Idempotent.
#
# Args: <tree> <features-dir>   Env: WITH_GAPPS / WITH_NDK / WITH_MAGISK (=true to include)
set -e
TREE="$1"
FEATDIR="$2"
[ -d "$TREE" ] && [ -d "$FEATDIR" ] || { echo "usage: stage-features.sh <tree> <features-dir>"; exit 2; }

# Both releases lunch device/remora (bd remora-31sq), so there is one product mk to wire.
PRODUCT_MK="$TREE/device/remora/remora_x86_64.mk"
[ -f "$PRODUCT_MK" ] || { echo "stage-features: $PRODUCT_MK not found (wrong tree?)"; exit 1; }

# A feature was asked for and its payload is not here. The public repo ships the .mk files and
# READMEs but NOT the third-party binaries beside them (vendor/PAYLOADS.md, ADR-0001), so this is
# the normal state of a fresh clone — and it must FAIL, loudly, naming the fix. Staging without
# it would produce a green build whose image silently lacks the feature it was configured for,
# which is the failure shape bd remora-4ei.53 and remora-4ei.55 already cost us twice.
refuse_missing() {  # <feature-name> <vendor-subdir>
    echo "stage-features: '$1' is enabled but its payload is missing:" >&2
    echo "    $FEATDIR/$2" >&2
    echo "  Third-party payloads are not distributed with Remora — see vendor/PAYLOADS.md" >&2
    echo "  for what each one is and how to obtain it, then:  vendor/fetch-payloads.sh $2" >&2
    echo "  (or turn the feature off and rebuild)." >&2
    exit 1
}

# The directory alone proves nothing: its .mk and README ship publicly, so a fresh clone HAS the
# directory and only lacks the binaries. Asked by content, the same test fetch-payloads.sh reports
# with — a bare -d here once passed every fresh clone straight through to an image without GApps.
has_payload() {  # <vendor-subdir>
    find "$FEATDIR/$1" \( -name '*.apk' -o -name '*.so' -o -name '*.jar' -o -name '*.dex' \) \
        -type f -print -quit 2>/dev/null | grep -q .
}

stage() {  # <feature-name> <vendor-subdir> <mk-relpath>
    name="$1"; sub="$2"; mk="$3"
    echo "=== staging $name ==="
    [ -d "$FEATDIR/$sub" ] || refuse_missing "$name" "$sub"
    case "$sub" in
        gapps|microg|ndk_translation|magisk) has_payload "$sub" || refuse_missing "$name" "$sub" ;;
    esac
    rm -rf "$TREE/vendor/$sub"
    cp -a "$FEATDIR/$sub" "$TREE/vendor/$sub"
    # append the guarded inherit once per product mk (survives because we run after the
    # patch reset)
    if ! grep -q "$mk" "$PRODUCT_MK"; then
        printf '\n# --- Remora feature: %s ---\n$(call inherit-product-if-exists, %s)\n' \
            "$name" "$mk" >> "$PRODUCT_MK"
    fi
}

# THE OTHER HALF OF stage(): TAKE IT BACK OUT WHEN THE FEATURE IS OFF (bd remora-bm7.14).
#
# stage() is idempotent for the ON case — it rm -rf's before copying — but nothing removed a payload
# when the feature was OFF, and the TREE IS SHARED BETWEEN PROFILES. So a payload staged by one
# profile's build stayed in the tree and changed what the NEXT profile built, even though that
# profile never enabled the feature.
#
# Measured, and it cost an afternoon: [Instance-AMD build] enables mesa_source and [Instance-Android
# 17] does not. An AMD build left vendor/mesa_source in the tree; the next Android 17 build then hit
# the prebuilt GPU project's "mesa_source supplies vulkan [radeon] — prebuilt stepping aside" logic,
# which keys off the payload EXISTING rather than off WITH_MESA_SOURCE. The prebuilt radeon ICD was
# dropped, mesa_source installed nothing in its place because the feature was off, and the image
# shipped with NO radeon Vulkan driver at all. verify-image.sh caught it; nothing else would have.
#
# mesa.mk's own $(error) guard cannot catch this: it fires when the payload is EMPTY, and here the
# payload is present and perfectly valid — it just belongs to a different profile.
#
# Applied to every gated feature rather than only mesa_source, because the mechanism is general: any
# staged payload that a product .mk or a prebuilt inherit keys off can do the same thing. Removing
# is safe and cheap — stage() re-creates it from $FEATDIR on the next build that asks for it.
unstage() {  # <feature-name> <vendor-subdir>
    [ -d "$TREE/vendor/$2" ] || return 0
    echo "=== unstaging $1 (feature is off; leaving it would change this build) ==="
    rm -rf "$TREE/vendor/$2"
}

[ "$WITH_GAPPS" = true ]  || unstage "GApps"            gapps
[ "$WITH_MICROG" = true ] || unstage "microG"           microg
[ "$WITH_NDK" = true ]    || unstage "ARM translation"  ndk_translation
[ "$WITH_MAGISK" = true ] || unstage "Magisk"           magisk
[ "$WITH_UPDATER" = true ] || unstage "Remora updater"  remora_updater
[ "$WITH_CAMERA" = true ] || unstage "External camera"  camera_external
[ "$WITH_USB_AUDIO" = true ] || unstage "USB audio"       usb_audio
[ "$WITH_MESA_SOURCE" = true ] || unstage "Mesa from source" mesa_source

[ "$WITH_GAPPS" = true ]  && stage "GApps"            gapps           vendor/gapps/gapps.mk
[ "$WITH_MICROG" = true ] && stage "microG"           microg          vendor/microg/microg.mk
# WITH_NDK builds in the host native-bridge libs + binfmt registration. On A17 the tree already
# registers arm64_dyn/arm64_exe itself; ndk.mk detects that and skips its own copies (a duplicate
# is a hard kati error), and assemble-image.sh overlays the same libs as a backstop (bd remora-rve).
[ "$WITH_NDK" = true ]    && stage "ARM translation"  ndk_translation vendor/ndk_translation/ndk.mk
[ "$WITH_MAGISK" = true ] && stage "Magisk"           magisk          vendor/magisk/magisk.mk
[ "$WITH_UPDATER" = true ] && stage "Remora updater"   remora_updater  vendor/remora_updater/updater.mk
# The agent's Java sources are NOT duplicated into the feature dir — agent/src at the repo root
# is the single copy (agent/build.sh compiles the same files for the bind-mount dev loop), so a
# feature-dir copy would silently drift. Overlay the live sources after the generic stage.
if [ "$WITH_AGENT" = true ]; then
    stage "Remora agent"     remora_agent    vendor/remora_agent/agent.mk
    rm -rf "$TREE/vendor/remora_agent/src"
    cp -a "$FEATDIR/../../../agent/src" "$TREE/vendor/remora_agent/src"
fi
# WITH_CAMERA builds AOSP's external (UVC/V4L2) camera provider into the image. The image ships
# cameraserver but no provider, so Android reports zero cameras; this supplies one. The container
# still has to be given a /dev/video* node at bringup for it to find anything (bd remora-4ei.9).
[ "$WITH_CAMERA" = true ] && stage "External camera"  camera_external vendor/camera_external/camera.mk
# WITH_USB_AUDIO builds AOSP's own tinyalsa USB audio HAL (audio.usb.default) plus a top-level
# audio policy that actually includes the usb module. Without that include the HAL installs and is
# never opened, and capture keeps coming from the primary STUB, which returns silence with fake
# timing — an app records zeros and reports no error (bd remora-4ei.37). /dev/snd is already in
# the container (privileged), so unlike the camera there is no bringup half.
[ "$WITH_USB_AUDIO" = true ] && stage "USB audio"       usb_audio       vendor/usb_audio/usb_audio.mk
# Mesa built from source (bd remora-ykhz R3 / bd remora-bm7.4). UNLIKE every feature above, its
# payload is not a third-party download — it is produced on this machine by
# vendor/host-prereqs/mesa-android/build-mesa.sh, because Mesa does not build under soong and
# reaches Android only through its own meson + NDK cross-file. stage()'s missing-payload refusal is
# therefore the right behaviour but the wrong instruction, so this one says how to make it instead.
if [ "$WITH_MESA_SOURCE" = true ] && [ ! -d "$FEATDIR/mesa_source/lib64" ]; then
    echo "stage-features: 'Mesa from source' is enabled but its payload is missing:" >&2
    echo "    $FEATDIR/mesa_source/lib64" >&2
    echo "  This payload is BUILT, not downloaded — Mesa has no soong build and cross-compiles" >&2
    echo "  through its own meson. Produce it with:" >&2
    echo "    REMORA_MESA_NDK=<ndk> vendor/host-prereqs/mesa-android/build-mesa.sh" >&2
    echo "  (or turn the feature off and rebuild on the prebuilt Mesa 24.0.8)." >&2
    exit 1
fi
[ "$WITH_MESA_SOURCE" = true ] && stage "Mesa from source" mesa_source vendor/mesa_source/mesa.mk

# c2-va (the VAAPI Codec2 HW video component: HEVC encode for the mirror, AVC + HEVC hardware
# decode) is a full SOURCE component rather than a prebuilt feature, and it is in no repo
# manifest — it is Remora's own source, so a synced tree simply does not have it.
# Stage it from the vendored copy. Its build deps are vendored as patches alongside
# (external/v4l2_codec2, external/libchrome), and each device tree's va.mk inherit is what
# imports the c2-va + v4l2_codec2 soong namespaces.
#
# IT LIVES AT hardware/remora/c2-va SINCE R2c (bd remora-28ix.4). The path is the SAME for A16
# and A17 on purpose — a per-release destination would mean two soong namespace names, two
# visibility entries in libchrome and two spellings in every comment, to save editing two patch
# files once.
C2VA="$(dirname "$FEATDIR")/c2-va"
if [ -d "$C2VA" ]; then
    echo "=== staging c2-va (VAAPI Codec2 HW video) ==="
    rm -rf "$TREE/hardware/remora/c2-va"
    mkdir -p "$TREE/hardware/remora"
    cp -a "$C2VA" "$TREE/hardware/remora/c2-va"
fi

# device/remora is NOT staged here. It is Remora-owned source like c2-va, but unlike c2-va it
# has patch-registry entries, so it must be in place BEFORE the patch pass — which runs before
# this script. See stage-device-tree.sh, which the build calls ahead of patching.

# xe gralloc: the resolver selects gralloc.minigbm_intel through the gralloc boot arg whenever the
# render node is xe (the gbm gralloc SIGFPEs / stripes video buffers there — Resolver.cpp). Upstream
# minigbm defines the module but nothing installs it — without this line the image falls back to
# gbm and surfaceflinger dies with "no suitable EGLConfig found".
if ! grep -q "gralloc.minigbm_intel" "$PRODUCT_MK"; then
    printf '\n# --- Remora: xe gralloc ---\nPRODUCT_PACKAGES += gralloc.minigbm_intel\n' >> "$PRODUCT_MK"
fi

# gbm_mesa/vtest gralloc (NVIDIA via Venus): same "defined but nothing installs it" trap as the
# intel line above — the minigbm_gbm_mesa_vtest source patch adds the soong modules, and without a
# PRODUCT_PACKAGES entry nothing references them so soong never builds them and the image ships
# the same five gralloc variants as before. Gated because the module is useless without a Venus
# vtest server listening; SourceBuild sets this when that patch is enabled (bd remora-4ei.56).
if [ "$WITH_GBM_MESA_GRALLOC" = true ] && ! grep -q "gralloc.minigbm_gbm_mesa" "$PRODUCT_MK"; then
    printf '\n# --- Remora: gbm_mesa/vtest gralloc (NVIDIA) ---\nPRODUCT_PACKAGES += gralloc.minigbm_gbm_mesa\n' >> "$PRODUCT_MK"
fi

echo "=== features staged ==="
