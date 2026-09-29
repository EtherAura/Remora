# remora_x86_64 — Remora's containerized Android product (bd remora-28ix.4 R1).

# 64-bit ONLY (bd remora-ykhz) — see the BoardConfig for the measurement that justified it. This is
# the half that sets ro.zygote=zygote64 and TARGET_SUPPORTS_32_BIT_APPS := false; the board dropping
# TARGET_2ND_ARCH is the other half, and they must move together.
#
# ENUMERATED BEFORE INHERITING, per the omx.mk rule. Against core_64_bit.mk this also carries:
#   - init.zygote64.rc only, instead of both zygote rc files
#   - dalvik.vm.dex2oat64.enabled=true
#   - TARGET_SUPPORTS_OMX_SERVICE := false
# That last one looks alarming next to the media_codecs.xml history, and is not: it gates exactly
# one package, android.hardware.media.omx@1.0-service, and only at PRODUCT_SHIPPING_API_LEVEL <= 33
# (build/make/target/product/base_vendor.mk:92). A17 ships API 37, the service is already absent
# from the running image, and Remora's media path is Codec2/c2-va rather than OMX.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_base.mk)

$(call inherit-product, $(LOCAL_PATH)/remora.mk)
$(call inherit-product, $(LOCAL_PATH)/remora_x86_64/device.mk)

# Full LineageOS product package set. The pinned tree ships LineageOS frameworks/base,
# which requires the lineage packages (LineageSettingsProvider, LineageParts, the lineage
# system services, org.lineageos.*) at runtime — without them the framework boots into
# missing-provider crashes. TARGET_DISABLE_EPPE skips the package-existence enforcement:
# this image drops some enforced packages (e.g. Calendar) on purpose.
TARGET_DISABLE_EPPE := true
$(call inherit-product, vendor/lineage/config/common.mk)

PRODUCT_NAME := remora_x86_64
PRODUCT_DEVICE := remora_x86_64
PRODUCT_BRAND := remora
PRODUCT_MODEL := remora17_x86_64

DEVICE_PACKAGE_OVERLAYS := $(LOCAL_PATH)/overlay
