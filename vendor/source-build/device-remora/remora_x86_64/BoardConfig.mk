# BoardConfig for remora_x86_64 (bd remora-28ix.4 R1): a GSI-shaped x86_64 board with an
# arm64 native bridge, building exactly the two images the container consumes.

TARGET_CPU_ABI := x86_64
TARGET_ARCH := x86_64
TARGET_ARCH_VARIANT := x86_64

# 64-BIT ONLY (bd remora-ykhz). The second arch is gone, and with it every 32-bit copy of every
# native library in the image — that is half of R3's remaining prebuilt payload, ~45 files reducing
# to ~22, plus the 32-bit zygote and webview_zygote as running processes.
#
# MEASURED BEFORE REMOVING, because "nothing uses it" is exactly the claim omx.mk disproved. On a
# fully booted device the ONLY 32-bit processes are the two zygotes themselves (zygote and
# webview_zygote): zero 32-bit app processes against 21 64-bit, with 20 third-party packages
# installed. The 32-bit zygote does preload a Vulkan driver, which is the one thing that would have
# needed a 32-bit Mesa — but nothing ever forks from it to use one.
#
# And 32-bit was already close to unreachable here: ro.product.cpu.abilist was
# 'x86_64,arm64-v8a,x86' with armeabi-v7a DELIBERATELY absent (see the native-bridge note below), so
# 32-bit ARM apps could not install at all and the only possible 32-bit apps were native x86 ones,
# which are effectively extinct on Android. arm64-v8a is untouched: the bridge is 64-bit.
#
# The product side pairs with this — remora_x86_64.mk inherits core_64_bit_only.mk rather than
# core_64_bit.mk, which is what actually sets ro.zygote=zygote64 and TARGET_SUPPORTS_32_BIT_APPS.
# Both halves are required; setting one alone gives a board with no 32-bit arch still shipping a
# zygote64_32 that cannot start its 32-bit half.

include build/make/target/board/BoardConfigGsiCommon.mk

# 64-bit ARM via the native bridge (ndk_translation). A 32-bit ARM second bridge arch
# is deliberately ABSENT: it was never backed by a translator,
# /system/lib/arm was empty, and a 32-bit-ARM-only APK installed and then died at first
# native call. Without the advertisement it is refused at install as incompatible, which
# is the honest answer (bd remora-rve). 32-bit x86 keeps running — the image is genuinely
# multilib — and arm64-v8a is untouched.
TARGET_NATIVE_BRIDGE_ARCH := arm64
TARGET_NATIVE_BRIDGE_ARCH_VARIANT := armv8-a
TARGET_NATIVE_BRIDGE_CPU_VARIANT := generic
TARGET_NATIVE_BRIDGE_ABI := arm64-v8a

TARGET_USERIMAGES_SPARSE_EXT_DISABLED := true

BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_PARTITION_RESERVED_SIZE := 16777216
BOARD_SYSTEMIMAGE_PARTITION_RESERVED_SIZE := 16777216

DEVICE_MANIFEST_FILE += device/remora/manifest.xml

# Vendor sepolicy for the VAAPI Codec2 HAL (c2-va): labels the DRM render nodes
# gpu_device and grants hal_codec2_server access.
BOARD_VENDOR_SEPOLICY_DIRS += device/remora/sepolicy

# A container JITs at runtime; dexpreopt of system-server jars is not installed, so skip
# the dexpreopt completeness check.
DISABLE_DEXPREOPT_CHECK := true
