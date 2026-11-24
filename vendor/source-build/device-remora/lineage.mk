# LineageOS app-suite + framework overlay for remora (A17), written at the R1
# reimplementation (bd remora-28ix.4) with the lineage_legal_url and lineage_version_props
# patch content native here — those two registry entries are SKIPped on A17.
#
# The Lineage apps come from lineage-24.0 with the rest of the tree, since LineageOS forward-ported
# to it in late 2026 (bd remora-q6t5). This makes the remora_x86_64 (aosp_base) product present as
# LineageOS: the LineageOS launcher (its fork of packages/apps/Launcher3), LineageParts settings,
# the Lineage app suite, and ro.lineage.* branding.

# LineageOS appearance: a static framework-res overlay giving the circular adaptive-icon mask (the
# LineageOS "look" — AOSP A17 defaults to a rounded square) and the LineageOS default wallpaper.
# Kept minimal (icons + wallpaper) rather than inheriting vendor/lineage/overlay/common wholesale,
# whose peripheral package overlays (SimpleDeviceConfig, LatinIME, …) risk A17 aapt2 conflicts.
PRODUCT_PACKAGE_OVERLAYS += $(LOCAL_PATH)/lineage-overlay

# NOTE: LineageParts and Profiles are deliberately NOT listed here. LineageParts ships all the
# same: vendor/lineage/config/common.mk, which remora_x86_64.mk inherits, adds it. lineage-24.0's
# frameworks/base carries the LineageOS platform itself: it loads the 0x3f
# org.lineageos.platform-res into every process, and SystemServer starts the external system
# server named by config_externalSystemServer, which runs the LineageHardware/LiveDisplay/Profile
# services. The standalone apps below need no SDK services and work as-is.
PRODUCT_PACKAGES += \
    Jelly \
    Etar \
    Aperture \
    Glimpse \
    Twelve \
    Recorder \
    Seedvault \
    Backgrounds \
    DeskClock \
    ExactCalculator \
    Dialer \
    Messaging

# Strip AOSP defaults that LineageOS replaces or that are dead weight in a GApps-less container:
#   Camera2  -> replaced by Aperture (the AOSP camera ships as com.google.android.apps.googlecamera
#               .fishfood and duplicates the STILL_IMAGE_CAMERA handler)
#   Gallery2 -> replaced by Glimpse (removed from the list above)
#   QuickSearchBox -> the AOSP search app behind the hotseat "Google" bar; nothing to search without
#               a Google provider, so it and the bar (container patch packages/apps/Launcher3/0001)
#               are dropped.
PRODUCT_PACKAGES_REMOVE += \
    Camera2 \
    Gallery2 \
    QuickSearchBox

# LineageOS platform SDK packages (org.lineageos.platform{,-res} + feature permission XMLs). The
# kept apps were compiled against the SDK (static-linked internal classes), so the SDK jar/res are
# shipped for their symbol/0x3f references. lineage-24.0's frameworks/base resolves those 0x3f
# references and starts LineageSystemServer (see the LineageParts note above).
$(call inherit-product, vendor/lineage/config/lineage_sdk_common.mk)

PRODUCT_PACKAGES += \
    LineageSettingsProvider

# LineageOS Styles (Settings > Wallpaper & style): the AOSP ThemePicker as LineageOS ships it —
# ThemesStub declares the theme categories it enumerates, LineageBlackTheme is the pure-black dark
# overlay. Accent colour is handled dynamically by Material You. Gives a runtime icon-shape/font/
# wallpaper chooser on top of the baked-in circular icon mask.
PRODUCT_PACKAGES += \
    ThemePicker \
    ThemesStub \
    LineageBlackTheme

PRODUCT_COPY_FILES += \
    vendor/lineage/config/permissions/org.lineageos.android.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/org.lineageos.android.xml

# LineageOS branding. Inheriting vendor/lineage/config/version.mk (the canonical version props) pulls
# in device/lineage/sepolicy via the LineageOS device-build path, which has an A17-undeclared attribute
# (sdcard_posix_contextmount_type) — a sepolicy cascade not worth it while SELinux is neutered. So set
# the ro.lineage.* version props directly with literals instead (empty $(LINEAGE_VERSION)&co would be
# dropped by the build.prop generator, which is why the earlier var-based attempt left only .device).
# These drive Settings > About phone ("LineageOS version") and LineageParts, so the image reads as
# LineageOS 24 rather than bare Android 17. Bump the date stamp when cutting a new image.
# LINEAGE_BUILD is normally derived by vendor/lineage/build/envsetup.sh from a `lineage_`-prefixed
# lunch target. Ours is remora_x86_64-cp2a-userdebug, so it comes out EMPTY and version.mk builds
# "24.0-<date>-UNOFFICIAL-" with nothing after the final dash. Set it explicitly. This works only
# because of inherit ORDER: remora_x86_64.mk pulls in remora.mk (which inherits this file)
# BEFORE it inherits vendor/lineage/config/common.mk — and common.mk:303 is what includes
# version.mk. Move either inherit and this silently reverts to an empty suffix.
LINEAGE_BUILD := remora_x86_64

# vendor/lineage/config/common.mk emits ro.adb.secure=1 for any non-eng build unless this is set,
# while Remora needs adb reachable without authentication or the container is unusable. Two
# different values for one sysprop is a HARD ERROR in build.prop generation, not a warning, so
# leaving this unset does not merely make adb awkward — it fails the build.
WITH_ADB_INSECURE := true

# The ro.lineage.version / display.version / modversion literals that used to live here are GONE.
# vendor/lineage/config/version.mk computes all three, and two DIFFERENT values for one sysprop is
# a hard error — which is exactly how this surfaced, at 78% of a two-hour build. The literals also
# carried a hand-bumped date stamp ("Bump the date stamp when cutting a new image") that had gone
# stale at 20260722; the computed value tracks the real build date with no manual step.
# The three kept below are safe: version.mk also emits releasetype and build.version, but with
# IDENTICAL values, and duplicates that agree are only a warning. ro.lineage.device,
# plat.sdk and ro.lineagelegal.url are ours alone.
PRODUCT_PRODUCT_PROPERTIES += \
    ro.lineage.device=remora_x86_64 \
    ro.lineage.build.version.plat.sdk=37 \
    ro.lineagelegal.url=https://lineageos.org/legal
