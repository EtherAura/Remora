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

PRODUCT_PROPERTY_OVERRIDES += \
    ro.enable.native.bridge.exec=1 \
    ro.dalvik.vm.isa.arm64=x86_64 \
    ro.dalvik.vm.isa.arm=x86 \
    ro.dalvik.vm.native.bridge=libnb.so \

# NO VULKAN ICD IS REQUESTED HERE (bd remora-28ix.4). vulkan.intel and vulkan.intel_hasvk come from
# mesa_source: mesa.mk:205-206 requests remora_mesa_vulkan_<driver> for every ICD in the payload.
#
# NB this is why the product is coupled to mesa_source rather than independent of it: there is no
# second source of a Vulkan ICD or a GL driver in the image at all.
