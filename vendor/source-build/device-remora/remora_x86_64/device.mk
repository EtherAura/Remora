# remora_x86_64 device shape (bd remora-28ix.4 R1): the x86_64 seccomp policy for the
# media codecs, and the arm64-on-x86_64 native bridge wiring (ndk_translation, staged by
# the arm_translation build feature; libnb.so is its loader shim).

# ONE SOURCE, TWO DESTINATIONS, deliberately. mediacodec and mediaswcodec each need the same
# five extra syscalls for the same reason — Mesa/gallium's dri path calls them and the stock
# policies do not allow them — and the two lists were byte-identical (md5 cdb5ccea) when
# both were carried (bd remora-28ix.4); keeping two copies of one list is how they drift apart.
PRODUCT_COPY_FILES += \
    device/remora/mediacodec.policy.x86:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediacodec.policy \
    device/remora/mediacodec.policy.x86:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediaswcodec.policy \

# The bridge properties follow the arm_translate selection (REMORA_NATIVE_BRIDGE_ENABLED, written
# by stage-features.sh) like the BoardConfig advertisement does. Without a translator the honest
# value is AOSP's own "0" — no bridge — and ro.remora.public_release marks a public_release build
# for the audit and for anyone reading getprop (bd remora-phvx).
-include device/remora/remora-features.mk
ifneq ($(REMORA_NATIVE_BRIDGE_ENABLED),false)
PRODUCT_PROPERTY_OVERRIDES += \
    ro.enable.native.bridge.exec=1 \
    ro.dalvik.vm.isa.arm64=x86_64 \
    ro.dalvik.vm.isa.arm=x86 \
    ro.dalvik.vm.native.bridge=libnb.so \

else
PRODUCT_PROPERTY_OVERRIDES += ro.dalvik.vm.native.bridge=0
endif
ifeq ($(REMORA_PUBLIC_RELEASE),true)
PRODUCT_SYSTEM_PROPERTIES += ro.remora.public_release=true
endif

# NO VULKAN ICD IS REQUESTED HERE (bd remora-28ix.4). vulkan.intel and vulkan.intel_hasvk come from
# mesa_source: mesa.mk:205-206 requests remora_mesa_vulkan_<driver> for every ICD in the payload.
#
# NB this is why the product is coupled to mesa_source rather than independent of it: there is no
# second source of a Vulkan ICD or a GL driver in the image at all.
