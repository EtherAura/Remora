# remora.mk — the shared core of Remora's container-Android product (bd remora-28ix.4 R1).
# Everything here is a statement about running Android as a privileged docker container on
# a Linux host; anything device-shape-specific (arch, ABI, native bridge) lives in
# remora_x86_64/ instead.

PRODUCT_MANUFACTURER := remora

# Android 17: base_system.mk puts the new com.android.webapp mainline module on the
# bootclasspath, but the bare cp1a release config leaves RELEASE_WEBAPP_MODULE unset (only
# the GMS cp2a config sets it), so neither the source module nor a prebuilt is selected and
# soong analysis fails: prebuilt_framework-webapp depends on the disabled
# framework-webapp.impl. The release/ map turns the flag on so the module builds from
# source (present at packages/modules/WebApp; needs no GMS prebuilt).
#
# GATED ON THE RELEASE CONFIG, AND IT HAS TO BE (bd remora-31sq). RELEASE_WEBAPP_MODULE is an A17
# flag: it does not exist in A16's release configuration at all. Offering a value for a flag the
# release-config tool has never heard of is not ignored, it PANICS —
#     panic: Setting value for undefined flag RELEASE_WEBAPP_MODULE in
#            device/remora/release/flag_values/cp1a/RELEASE_WEBAPP_MODULE.textproto
# — and it panics inside dumpvars, so lunch itself fails with an error that names neither this
# file nor the flag: "Don't have a product spec for: 'remora_x86_64'". That is what an A16 build
# hit the first time this tree was lunched for it, and the misleading message is why this comment
# quotes the real one.
#
# KEYED ON A RELEASE FLAG, AND IT HAS TO BE ONE. The obvious discriminator, TARGET_RELEASE, is
# FORBIDDEN in a product makefile and the build says so — "TARGET_RELEASE may not be accessed
# directly. Use individual flags." — but only at KATI time. lunch and dumpvars read it happily, so
# a direct `lunch` test passes and the build then dies two stages later. RELEASE_PLATFORM_SDK_VERSION
# is a declared release flag in both trees (36 on A16, 37 on A17), which is the sanctioned form.
ifeq ($(RELEASE_PLATFORM_SDK_VERSION),37)
PRODUCT_RELEASE_CONFIG_MAPS += $(LOCAL_PATH)/release/release_config_map.textproto
endif

# Android 17: apexd gained a "mount before data" path (data APEX as a dm-linear device on
# the userdata block device) that activates when apexd.config.use_fiemap is set AND init
# runs with a single mount namespace (ro.init.mnt_ns.count==1, which the container does).
# In a container /data is a bind mount, not a block device, so apexd-bootstrap's
# WaitForDataBlockDevice() fails and it exits 1 (reboot_on_failure → boot loop). Disable
# fiemap/pinned-apex so apexd falls back to the A16-style loop-device mounting that works
# in a container.
PRODUCT_PROPERTY_OVERRIDES += apexd.config.use_fiemap=false

# Android 17: SurfaceFlinger's startup shader-cache priming draws a hole-punch layer into
# a temporary RenderEngine buffer, which this stack's gralloc does not allocate as
# GPU-writeable -> SkiaRenderEngine aborts "output buffer not gpu writeable", crash-looping
# SF and blocking boot_completed. Priming is only a first-frame latency optimization;
# disable it (real composition to the actual GPU-writeable framebuffer is unaffected).
PRODUCT_PROPERTY_OVERRIDES += service.sf.prime_shader_cache=false

# Android 17: system_server's new ApplicationSharedMemory (SystemServer.run) calls
# ashmem_create_region very early. libcutils use_memfd() only selects memfd when the
# SELinux memfd_class policy-capability file exists AND app_target_sdk>=37; this image
# runs with SELinux disabled (no /sys/fs/selinux) and a finalized SDK 36, so use_memfd()
# returns false and falls back to /dev/ashmem, which the container does not provide ->
# "Failed to create ashmem: No such file or directory" kills system_server. memfd_create
# is a plain syscall needing no device node or SELinux, so force it on (the intended
# sys.use_memfd override).
PRODUCT_PROPERTY_OVERRIDES += sys.use_memfd=true

# Android 17: Keystore2's LockSettingsService.initKeystoreSuperKeys()
# (PHASE_THIRD_PARTY_APPS_CAN_START) requires an AIDL KeyMint HAL; the legacy
# keymaster@4.1 HIDL service no longer satisfies it (keymint
# IRemotelyProvisionedComponent absent from the VINTF manifest), so initUserSuperKeys()
# fails and system_server crashes right before boot_completed. Ship the AOSP software
# (nonsecure) KeyMint HAL — it implements keymint/sharedsecret/secureclock with a
# simulated TA and needs no TEE, which is correct for a container. Its vintf fragments
# register automatically.
PRODUCT_PACKAGES += android.hardware.security.keymint-service.nonsecure

# Android 17: the prebuilt Profiling / Telephony module SDK systemserverclasspath
# fragments list service-anomaly-detector / service-telecom and require them declared.
# Enabling the RELEASE_* flags satisfied that but ALSO added framework-anomaly-detector /
# framework-telecom to the boot classpath, which the prebuilt apexes' runtime
# bootclasspath.pb omit -> boot.art component-count mismatch, zygote dies. Instead,
# declare just the system-server jars directly (the prebuilt apexes DO ship these)
# without enabling the flags, so the boot image stays consistent with runtime.
#
# GATED FOR THE SAME REASON AS THE RELEASE MAP ABOVE (bd remora-31sq): both jars are A17 modules.
# Neither service-anomaly-detector nor service-telecom is defined anywhere in an A16 tree, and an
# APEX_SYSTEM_SERVER_JARS entry naming an undefined module is NOT tolerated the way a missing
# PRODUCT_PACKAGES entry is — it fails soong analysis outright:
#     error: build/soong/Android.bp:135:1: "dexpreopt_systemserver_check" depends on undefined
#            module "service-anomaly-detector".
# That distinction is the point: this file requests three HIDL packages that do not exist in
# EITHER tree (wifi@1.0-service, drm@1.4-service-lazy.clearkey, composer@2.1-impl) and A17 builds
# anyway, because an absent package is silently dropped. So "A17 builds it" is not evidence that a
# name resolves — only the jars fail loudly, and only they need the gate.
ifeq ($(RELEASE_PLATFORM_SDK_VERSION),37)
PRODUCT_APEX_SYSTEM_SERVER_JARS += \
    com.android.profiling:service-anomaly-detector \
    com.android.telephonycore:service-telecom
endif

# Flattened, uncompressed APEXes: the container has no /metadata, no apexd early-boot
# block-device dance, and no OTA — flattening sidesteps all three.
OVERRIDE_TARGET_FLATTEN_APEX := true
OVERRIDE_PRODUCT_COMPRESSED_APEX := false

$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# no kernel involved — the container runs on the host's
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

PRODUCT_USE_DYNAMIC_PARTITION_SIZE := true

# The image set a container needs and nothing more: system + vendor. No ramdisk, no
# userdata (that is a bind mount), no vbmeta/super (no bootloader ever sees this).
PRODUCT_BUILD_CACHE_IMAGE := false
PRODUCT_BUILD_ODM_IMAGE := false
PRODUCT_BUILD_PRODUCT_IMAGE := false
PRODUCT_BUILD_PRODUCT_SERVICES_IMAGE := false
PRODUCT_BUILD_RAMDISK_IMAGE := false
PRODUCT_BUILD_SUPER_PARTITION := false
PRODUCT_BUILD_SYSTEM_OTHER_IMAGE := false
PRODUCT_BUILD_USERDATA_IMAGE := false
PRODUCT_BUILD_VBMETA_IMAGE := false
PRODUCT_BUILD_VENDOR_IMAGE := true

ifeq ($(BUILD_VENDOR_ONLY), true)
PRODUCT_BUILD_SYSTEM_IMAGE := false
else
PRODUCT_BUILD_SYSTEM_IMAGE := true
endif

PRODUCT_SHIPPING_API_LEVEL := 34

AUDIOSERVER_MULTILIB := first

TARGET_VENDOR_PROP += device/remora/remora.prop

# ANGLE (GL-on-Vulkan) and the pastel Vulkan software renderer: the guest-mode GPU story
# when no host node is passed through.
PRODUCT_PACKAGES += \
    libEGL_angle \
    libGLESv1_CM_angle \
    libGLESv2_angle \
    vulkan.pastel \

# Phone App required
PRODUCT_PACKAGES += \
    rild

# WiFi required by SystemUI
PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \

# The HAL set a headless container answers VINTF with: audio (r_submix + impl), clearkey
# DRM, software gatekeeper, gralloc/mapper/composer at the versions manifest.xml declares,
# and example/mock health, power and thermal — there is no battery, no PMIC and no
# thermistor to be honest about.
PRODUCT_PACKAGES += \
    audio.r_submix.default \
    android.hardware.audio.service \
    android.hardware.audio@7.0-impl \
    android.hardware.audio.effect@7.0-impl \
    android.hardware.drm@1.4-service-lazy.clearkey \
    android.hardware.gatekeeper@1.0-service.software \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.mapper@2.0-impl-2.1 \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.composer@2.1-impl \
    android.hardware.health-service.example \
    android.hardware.power-service.example \
    android.hardware.thermal@2.0-service.mock \

# Media stack (bd remora-28ix.4). Every piece here is Remora's or AOSP's: the codec2
# activation wiring lives in vendor/remora, and the VAAPI codec2 decode/encode HAL (c2-va)
# is Remora's own source.
#
# NO OMX PLUGIN IS BUILT; the product is codec2 throughout. The one that used to ship gated
# itself on a boot property Remora has never emitted, so it was dead weight in every
# deployment. Nothing may be added that installs /vendor/etc/media_codecs.xml alongside the
# unconditional block below — two PRODUCT_COPY_FILES lines racing for one path is a build
# error, not a preference.
#
# THE MEDIA CONFIG WAS NEVER OPTIONAL, AND IS THE DEVICE TREE'S OWN (below). The plugin's omx.mk
# carried far more than the plugin: /vendor/etc/media_codecs.xml (the top-level list every other list hangs off),
# the three AOSP software codec lists, and media_profiles. Dropping the inherit without
# replacing them shipped an image advertising NO vendor codecs — the VAAPI encoder vanished
# and the mirror died at session setup with a server-side configuration error, live on the
# first build (bd remora-28ix.4). Device config belongs to the device tree; it must never
# ride along with a HAL that can be turned off.

# Media configuration — unconditional, and the device tree's own.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    $(LOCAL_PATH)/media_profiles.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles_V1_0.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video.xml \

# The codec2 activation wiring is vendor/remora's (R2c, bd remora-28ix.4): its init rc and
# script read ro.boot.use_remora_c2 directly, and the mediaswcodec seccomp policy — byte-identical
# to the mediacodec one — installs from remora_x86_64/device.mk.

# VAAPI Codec2 hardware decode/encode HAL (libva + the in-tree iHD/Mesa drivers).
$(call inherit-product, hardware/remora/c2-va/va.mk)

DEVICE_MANIFEST_FILE += device/remora/android.hardware.bluetooth@1.1.xml

PRODUCT_PACKAGES += android.hardware.bluetooth@1.1-service.sim

PRODUCT_SOONG_NAMESPACES += frameworks/av/services/audiopolicy/config
# The VAAPI Codec2 HAL (hardware/remora/c2-va) reuses the C2 framework libs from the
# external/v4l2_codec2 namespace, which it imports — both must be registered here.
PRODUCT_SOONG_NAMESPACES += external/v4l2_codec2 hardware/remora/c2-va

# audio policy
#
# DO NOT TRY TO REPLACE audio_policy_configuration.xml WITH PRODUCT_COPY_FILES. It cannot work in
# this tree, and the reason is not obvious enough to rediscover cheaply (bd remora-4ei.37,
# measured). PRODUCT_SOONG_NAMESPACES above registers
# frameworks/av/services/audiopolicy/config, and soong emits an install rule into
# out/soong/installs-<product>.mk for EVERY prebuilt_etc in a registered namespace — independently
# of PRODUCT_PACKAGES. A PRODUCT_COPY_FILES onto the same destination is then a second rule for one
# target, which kati rejects at parse time:
#     build/make/core/Makefile:139: error: overriding commands for target
#     `.../vendor/etc/audio_policy_configuration.xml'
# PROVEN, not inferred: a2dp_audio_policy_configuration.xml is a prebuilt_etc in that namespace, is
# absent from PRODUCT_PACKAGES, is required by nothing in the tree — and still has an install rule.
# Dropping the module from PRODUCT_PACKAGES therefore does NOT retract its rule; it was tried, the
# dumped product config confirmed the entry was gone, and the collision was identical.
# (device/generic/goldfish gets away with the same PRODUCT_COPY_FILES only because it never
# registers that namespace.)
#
# THE ROUTE THAT DOES WORK is to change what the existing module INSTALLS rather than to add a
# second installer: patch frameworks/av's audio_policy_configuration_generic.xml, which is the
# module's src via the :audio_policy_configuration_generic filegroup. One rule, our content.
PRODUCT_PACKAGES += \
    audio_policy_configuration.xml \
    r_submix_audio_policy_configuration.xml \
    audio_policy_volumes.xml \
    default_volume_tables.xml \
    primary_audio_policy_configuration.xml \
    surround_sound_configuration_5_0.xml \

# NO HARDWARE INIT RC IS INSTALLED BY THE BLOCK BELOW, deliberately (bd remora-28ix.4 R4). "init
# warns about a missing hardware rc, so an empty file is quieter than no file" is wrong: init
# imports /vendor/etc/init/hw/init.$(ro.hardware).rc, a directory this image does not have at all,
# so a placeholder at /vendor/etc/init/ is never the one being looked for — only the generic
# directory scan would pick it up. Verified on the live image.
PRODUCT_COPY_FILES += \
    frameworks/av/media/libeffects/data/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute-0.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level-1.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version-1_1.xml \
    frameworks/native/data/etc/handheld_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml \
    $(LOCAL_PATH)/remora-removed-permissions.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/remora-removed-permissions.xml \

# required by Settings
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \

$(call inherit-product, frameworks/native/build/phone-xhdpi-6144-dalvik-heap.mk)

$(call inherit-product-if-exists, product.mk)

# Remora's own vendor project (R2, bd remora-28ix.4): the container's init behaviour, GPU
# bring-up, boot-property namespace and native modules.
$(call inherit-product, vendor/remora/vendor.mk)

# NO PREBUILT GPU PROJECT IS INHERITED (bd remora-28ix.4). R3 (bd remora-ykhz) replaced every
# prebuilt library a shipped image reached, and the closing census is worth keeping because it
# corrected a WRONG one: the prebuilt set supplied THREE files to the last image built with it —
# vulkan.virtio.so, libc++_shared.so and a suffixed copy of amdgpu.ids. An earlier reading matched
# image filenames against the prebuilts and added vulkan.pastel and vulkan.lvp to that list; neither
# was ever in it (no libvulkan_pastel.so or libvulkan_lvp.so at any arch, in any commit). Both are
# built in-tree, which their DT_NEEDED on the platform libc++.so says and the NDK's
# libc++_shared.so — carried by the genuine prebuilts — denies.
#
# Each of the three went for its own reason, and only the first is interesting:
#   vulkan.virtio       SHADOWED WHERE IT IS USED, UNSELECTED EVERYWHERE ELSE. ro.hardware.vulkan
#                       is virtio only on the Venus path, and there the deploy bind-mounts the
#                       prebuilt Venus guest set's 29.7 MB ICD over it (Builders.cpp
#                       venusGuestLibs()); AMD resolves radeon and guest mode resolves pastel.
#                       Dropping the image's own 1.2 MB copy is safe for a reason that predates
#                       this change: THREE of the five destinations in that mount table — the
#                       whole ANGLE trio — already do not exist in the image, so binding onto a
#                       path the image lacks is how this deploy has always worked.
#   libc++_shared       the image copy of vulkan.virtio was its LAST consumer (recursive readelf
#                       over vendor/lib64). The Venus ICD that actually loads does not link it.
#   amdgpu.ids (suffixed)  read by nothing. The etc define's stem defaults to the module name, so it
#                       shipped under a suffixed name while radeonsi opens `amdgpu.ids`
#                       exactly — see the libdrm block below, which is where that file comes from.
#
# THE PACKAGE LIST ITSELF LIVED IN THE PREBUILT PROJECT, which is what made this more than a
# manifest edit: its prebuilts.mk was inherited here and requested libEGL_mesa, the GLES pair, six
# vulkan ICDs, both grallocs, uinputd/vncserver, the vaapi bins and libgallium_drv_video. Every
# one of those is re-requested elsewhere — mesa.mk:142-144 for the GL cluster, mesa.mk:205-206 for
# the ICDs it builds — or deliberately retired. EXACTLY ONE had no other home: lavapipe. It is
# external/mesa3d's own in-tree module and the software Vulkan driver that is not pastel, so
# losing it to a manifest edit would have been silent.
PRODUCT_PACKAGES += vulkan.lvp

# mesa_source IS NOW MANDATORY FOR THIS PRODUCT, and this refusal is the only thing that makes that
# true rather than merely intended. With no prebuilt GPU project there is no SECOND source of any
# GL driver or Vulkan ICD in the image, so building with the feature off gives you an image with no
# libgallium_dri, no libEGL_mesa, no GLES pair and no ICD except lavapipe and pastel — one that
# builds perfectly cleanly and cannot composite a frame.
#
# THIS EXACT SHAPE HAS ALREADY SHIPPED ONCE, from the other direction: stage-features.sh:59-69
# records an afternoon lost to an AMD build leaving vendor/mesa_source in the tree, the next
# Android 17 build seeing the payload and stepping the prebuilts aside, and the feature being OFF so
# nothing installed in their place. That was fixed by UNSTAGING the payload when the feature is off
# — which is precisely what makes this wildcard a trustworthy test of the feature rather than of a
# leftover directory.
#
# Gated on TARGET_PRODUCT, not PLATFORM_VERSION: lunch sets it before product config, and AOSP loads
# every AndroidProducts.mk entry when enumerating products, so an unguarded $(error) here could fire
# while configuring a build that is not this product at all. remora_x86_64 is the product
# do-build.sh lunches on every release, and TARGET_PRODUCT — unlike PLATFORM_VERSION — is always
# set by the time this file is read.
#
# The companion check is in mesa.mk and answers a DIFFERENT question — it refuses a payload that is
# present but empty, i.e. a path bug. This one refuses a payload that is absent, i.e. the feature is
# off. Neither catches the other's case.
ifeq ($(TARGET_PRODUCT),remora_x86_64)
ifeq (,$(wildcard $(TOPDIR)vendor/mesa_source/lib64/dri/libgallium_dri.so))
$(error remora_x86_64 requires the mesa_source build feature. The prebuilt GPU project is gone from \
both manifests (bd remora-28ix.4), so there is no prebuilt GL or Vulkan stack to fall back on and this build \
would produce an image that cannot render. Enable "Mesa from source" in the build features — or, \
for a manual do-build.sh run, stage the payload first: vendor/source-build/stage-features.sh with \
WITH_MESA_SOURCE=true, which is what puts vendor/mesa_source/lib64 in the tree. If the feature IS \
enabled and you are seeing this, the host-side Mesa build has not been run: see \
vendor/host-prereqs/mesa-android/build-mesa.sh)
endif
endif

# EIGHT PRODUCT_PACKAGES_REMOVE ENTRIES STOOD HERE AND LEFT WITH THE PROJECT THAT DEFINED THEM.
# Keeping the reasoning, because it is the record of why each was safe to drop and it is no longer
# reconstructible from a tree that has none of them: uinputd and vncserver self-gated on
# a boot property Remora has never emitted (input arrives through the in-image
# agent, display through the mirror); libevdev was not gated on anything and was simply uinputd's
# dependency, measured unloaded in every process on all three hosts before removal; the prebuilt
# libdrm.so.2 and its four versioned backends lost to AOSP's own source builds (see below), which
# also retired a genuine duplicate — the image carried two libdrm; and vulkan.broadcom (v3dv,
# VideoCore) and vulkan.freedreno (turnip, Adreno) drive GPUs that exist only inside ARM SoCs, so
# unlike every other ICD there is no machine that can run this x86_64 image and have the hardware.
# That last distinction is the policy, not an exception to it: drivers for hardware this project
# does not own are KEPT so other people's machines work, which is why nouveau and intel_hasvk moved
# to Mesa source builds instead. Reversible if an arm64 image ever exists.
#
# A PRODUCT_PACKAGES_REMOVE naming a module no project defines is inert, and leaving eight of them
# would read as active policy. The matching assemble-image.sh exclusions stay: they clear these
# files out of an out/ tree that still holds them from an earlier build.

# amdgpu device-name table (bd remora-ykhz.1: radeonsi complains "/vendor/etc/hwdata/amdgpu.ids:
# No such file" and names the GPU generically without it). This is external/libdrm's OWN soong
# module (data/Android.bp, prebuilt_etc into hwdata) — same provenance as the source-built
# libdrm_amdgpu beside it. Two wrong attempts are worth their history here: a PRODUCT_COPY_FILES
# to the same destination collided with this module's soong install rule (defined is enough;
# "module wins with a warning" was a wrong prediction, measured), and requesting the
# prebuilt project's suffixed amdgpu.ids module instead shipped the file UNDER THE MODULE NAME — the etc
# define's stem defaults to the module name, and radeonsi opens amdgpu.ids exactly.
PRODUCT_PACKAGES += amdgpu.ids

# libdrm AND ITS FOUR BACKENDS ARE BUILT FROM SOURCE (bd remora-ykhz, R3). AOSP already builds all
# five — external/libdrm for libdrm, and its own amdgpu/, intel/, nouveau/ and radeon/ subdirectory
# Android.bp files for the backends, every one of them already `vendor: true`. It simply never
# INSTALLS the four, which is the whole reason a prebuilt project used to supply compiled
# copies. So this is an install-list change, not a port: the module names above are the PREBUILT
# ones (that project names its modules after the versioned files), and the ones below are AOSP's.
#
# CHECKED BEFORE SWITCHING, because these are not exercised on this machine at all — libdrm_amdgpu,
# _intel, _nouveau and _radeon read as unloaded on the NVIDIA/Venus host and are loaded in 12-14
# processes on both AMD ones (bd remora-ykhz). Comparing exported symbols, the source builds cover
# the prebuilt ABI exactly: amdgpu 79 prebuilt symbols vs 84 source (a superset — AOSP's libdrm is
# 2.4.124, newer), intel 83 vs 83, nouveau 43 vs 43, radeon 44 vs 44, with ZERO prebuilt symbols
# missing from any source build. libdrm itself: 212 vs 212, zero missing.
#
# This also retires a genuine duplicate. The image carried TWO libdrm — AOSP's libdrm.so, loaded in
# 57 processes, and the prebuilt libdrm.so.2 which the 32-bit zygote pulls in via vulkan.virtio.so.
# One implementation now serves both names.
PRODUCT_PACKAGES += \
    libdrm_amdgpu \
    libdrm_intel \
    libdrm_nouveau \
    libdrm_radeon \
    libdrm_so_2_symlink64 \
    libdrm_amdgpu_so_1_symlink64 \
    libdrm_intel_so_1_symlink64 \
    libdrm_nouveau_so_2_symlink64 \
    libdrm_radeon_so_1_symlink64 \

# LineageOS app suite + branding (backported apps — see lineage.mk). if-exists on purpose:
# the file is part of this tree, but a build that strips it (bare-AOSP experiments) must
# degrade to stock apps, not die.
$(call inherit-product-if-exists, $(LOCAL_PATH)/lineage.mk)
