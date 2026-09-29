# libndk_translation (arm64 native bridge). ELF prebuilts (translator libs + runner) are
# BUILD_PREBUILT modules (Android.mk) via PRODUCT_PACKAGES; only non-ELF config goes in
# PRODUCT_COPY_FILES. arm64 guest system libs are built from source (not shipped). WITH_NDK=true.
NDK_ROOT := vendor/ndk_translation

PRODUCT_PACKAGES += ndk_translation_program_runner_binfmt_misc_arm64
PRODUCT_PACKAGES += $(patsubst $(NDK_ROOT)/lib64/%.so,%,$(wildcard $(NDK_ROOT)/lib64/*.so))

# non-ELF config only (cpuinfo, binfmt_misc registrations, cpuinfo.arm64.txt)
PRODUCT_COPY_FILES += \
    $(NDK_ROOT)/lib64/arm64/cpuinfo:$(TARGET_COPY_OUT_SYSTEM)/lib64/arm64/cpuinfo \
    $(NDK_ROOT)/etc/cpuinfo.arm64.txt:$(TARGET_COPY_OUT_SYSTEM)/etc/cpuinfo.arm64.txt
# Skip any binfmt_misc registration the TREE already installs as a Soong module. AOSP 17 carries
# frameworks/libs/binary_translation/prebuilt, whose prebuilt_etc modules arm64_dyn and arm64_exe
# install to exactly these paths. Two rules for one output is a hard kati error 40 minutes into the
# build ("overriding commands for target .../binfmt_misc/arm64_dyn"), long past the point where it
# is cheap to discover.
#
# Dropping ours is safe rather than merely expedient: both registrations are byte-identical and
# route to the same /system/bin/ndk_translation_program_runner_binfmt_misc_arm64, so the tree's
# copy provides the same behaviour. The runner itself still comes from PRODUCT_PACKAGES above.
#
# Derived from the .bp rather than hardcoded, so a new colliding module upstream is handled without
# another 40-minute failure. Empty when the project is absent (older trees), restoring the old
# copy-everything behaviour.
NDK_BINFMT_BP := frameworks/libs/binary_translation/prebuilt/Android.bp
NDK_BINFMT_SKIP := $(if $(wildcard $(NDK_BINFMT_BP)),\
    $(shell sed -n 's/^[[:space:]]*name: "\([a-z0-9_]*\)",/\1/p' $(NDK_BINFMT_BP) 2>/dev/null))
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(NDK_ROOT)/etc/binfmt_misc/*),\
    $(if $(filter $(notdir $(f)),$(NDK_BINFMT_SKIP)),,\
        $(f):$(TARGET_COPY_OUT_SYSTEM)/etc/binfmt_misc/$(notdir $(f))))

# NO armeabi-v7a/armeabi anywhere. The payload here is arm64-ONLY — lib64/arm64 guest libs and a
# 64-bit libnb.so, with no /system/lib/arm and no 32-bit host translator — so advertising the 32-bit
# ARM ABIs meant a 32-bit-ARM-only APK installed and then died at its first native call. Refusing it
# at install is the honest answer (bd remora-rve).
#
# THE abilist PROPERTIES USED TO BE SPELT OUT HERE AND NO LONGER ARE (bd remora-ykhz). They said
# x86_64,x86,arm64-v8a with abilist32=x86, on the stated grounds that "the image really is multilib
# and those apps run". Since the 32-bit drop it is not multilib, and that made the hardcoding an
# outright build failure rather than a stale comment:
#     error: found duplicate sysprop assignments:
#     ro.vendor.product.cpu.abilist=x86_64,arm64-v8a        <- generated, correct
#     ro.vendor.product.cpu.abilist=x86_64,x86,arm64-v8a    <- this block
# which is a good failure: it is caught at build time rather than shipping an image that advertises
# an ABI it cannot execute.
#
# They are simply GONE rather than corrected, because the build system derives all three itself and
# gets arm64-v8a from the device tree's TARGET_NATIVE_BRIDGE_ARCH — the generated value is already
# x86_64,arm64-v8a. Restating it here only creates a second place to forget.
PRODUCT_VENDOR_PROPERTIES += \
    ro.dalvik.vm.native.bridge=libnb.so \
    ro.enable.native.bridge.exec=1
