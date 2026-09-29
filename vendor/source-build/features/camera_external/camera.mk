# External (USB/V4L2) camera support for the Remora image.
#
# NOTHING HERE IS A HAL WE WROTE. AOSP already ships a camera provider for UVC/V4L2 devices —
# hardware/interfaces/camera/provider/default (AIDL V1), backed by the ExternalCameraDevice
# implementation under hardware/interfaces/camera/device/3.x/default. The image simply never built
# it, which is why `dumpsys media.camera` reported "Number of camera devices: 0" while cameraserver
# was running. See bd remora-4ei.9.
#
# The provider enumerates /dev/video* at runtime, so the container must be given the node — that is
# a deploy concern, not a build one.
PRODUCT_PACKAGES += android.hardware.camera.provider-V1-external-service

# THE PROVIDER CANNOT REGISTER WITHOUT THIS. The AOSP module declares only init_rc and ships NO
# vintf_fragments, so PRODUCT_PACKAGES installs the binary and its .rc and nothing else. The service
# then starts at class hal and ABORTS — "Error while registering ext camera provider service: -3" —
# because servicemanager refuses an AIDL interface the device manifest does not declare, and
# dumpsys media.camera keeps reporting 0 devices. Indistinguishable from "no HAL installed" unless
# you read the crash buffer (bd remora-4ei.9).
# DEVICE_MANIFEST_FILE, not PRODUCT_COPY_FILES: build/make/core/Makefile:139 hard-errors on VINTF
# metadata copied by hand ("use DEVICE_MANIFEST_FILE / DEVICE_MATRIX_FILE / vintf_compatibility_matrix
# / vintf_fragments instead!"). vintf_fragments would mean patching the AOSP module; this tree
# already sets DEVICE_MANIFEST_FILE from a PRODUCT makefile — see device/remora's remora.mk for
# android.hardware.bluetooth@1.1.xml — so the precedent and the mechanism are both local.
DEVICE_MANIFEST_FILE += vendor/camera_external/android.hardware.camera.provider-external.xml

# The provider reads this at ExternalCameraConfig::kDefaultCfgPath. Without it the HAL falls back to
# built-in defaults that assume a phone; with it we can bound resolutions and, importantly, restrict
# which video nodes are considered — a UVC camera usually exposes two nodes and only one streams.
PRODUCT_COPY_FILES += \
    vendor/camera_external/external_camera_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/external_camera_config.xml

# Declare the feature, or apps that filter on it (most camera apps, and the Play Store) will not
# consider a camera present even once the provider is up.
# One file, not two: android.hardware.camera.external.xml already declares BOTH
# android.hardware.camera.any and android.hardware.camera.external. There is no
# android.hardware.camera.any.xml in the tree, and naming a PRODUCT_COPY_FILES source that does not
# exist fails the build rather than being ignored.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.camera.external.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/android.hardware.camera.external.xml
