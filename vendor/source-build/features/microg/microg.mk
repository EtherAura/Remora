# microG de-Googled Play services, vendored by Remora — the gapps alternative (mutually
# exclusive; the microg feature conflicts with gapps in the Features engine). GmsCore +
# Companion (FakeStore successor, package com.android.vending) as priv-apps, Aurora Store as
# the storefront. Deliberately NO ro.control_privapp_permissions=enforce here: the whitelist
# below is best-effort, and enforce turns an unlisted priv-perm into a boot loop.
#
# Signature spoofing is NOT wired yet: apps hard-verifying the Play signature via the GMS
# client library will still refuse; FCM push, check-in, location and Aurora installs work
# without it. Follow-up: spoof via the existing ReZygisk stack (zygisk_pif infrastructure).
MICROG_ROOT := vendor/microg

PRODUCT_PACKAGES += \
    MicroGmsCore MicroGCompanion AuroraStore

PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(MICROG_ROOT)/product/etc/permissions/*.xml),\
    $(f):$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/$(notdir $(f)))
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(MICROG_ROOT)/product/etc/sysconfig/*.xml),\
    $(f):$(TARGET_COPY_OUT_PRODUCT)/etc/sysconfig/$(notdir $(f)))
