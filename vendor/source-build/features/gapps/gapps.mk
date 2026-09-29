# MindTheGapps-style prebuilt integration, vendored by Remora. APKs are BUILD_PREBUILT modules
# (Android.mk) added via PRODUCT_PACKAGES; permissions/sysconfig are plain PRODUCT_COPY_FILES.
GAPPS_ROOT := vendor/gapps

PRODUCT_PACKAGES += \
    GmsCore Phonesky GoogleServicesFramework \
    GoogleCalendarSyncAdapter GoogleContactsSyncAdapter

PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(GAPPS_ROOT)/product/etc/permissions/*.xml),\
    $(f):$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/$(notdir $(f)))
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(GAPPS_ROOT)/product/etc/sysconfig/*.xml),\
    $(f):$(TARGET_COPY_OUT_PRODUCT)/etc/sysconfig/$(notdir $(f)))

PRODUCT_PRODUCT_PROPERTIES += ro.control_privapp_permissions=enforce
