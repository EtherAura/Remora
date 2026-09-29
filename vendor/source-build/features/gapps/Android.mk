# Presigned GApps prebuilt app modules (PRODUCT_COPY_FILES rejects .apk — this is the required
# BUILD_PREBUILT form). Partition placement matches the golden image.
LOCAL_PATH := $(call my-dir)

define remora-gapps-app
include $(CLEAR_VARS)
LOCAL_MODULE := $(1)
LOCAL_MODULE_OWNER := google
LOCAL_SRC_FILES := $(2)
LOCAL_CERTIFICATE := PRESIGNED
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_SUFFIX := $$(COMMON_ANDROID_PACKAGE_SUFFIX)
LOCAL_DEX_PREOPT := false
LOCAL_PRIVILEGED_MODULE := $(3)
LOCAL_PRODUCT_MODULE := $(4)
LOCAL_SYSTEM_EXT_MODULE := $(5)
include $$(BUILD_PREBUILT)
endef

$(eval $(call remora-gapps-app,GmsCore,product/priv-app/GmsCore/GmsCore.apk,true,true,))
$(eval $(call remora-gapps-app,Phonesky,product/priv-app/Phonesky/Phonesky.apk,true,true,))
$(eval $(call remora-gapps-app,GoogleServicesFramework,system_ext/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk,true,,true))
$(eval $(call remora-gapps-app,GoogleCalendarSyncAdapter,product/app/GoogleCalendarSyncAdapter/GoogleCalendarSyncAdapter.apk,,true,))
$(eval $(call remora-gapps-app,GoogleContactsSyncAdapter,product/app/GoogleContactsSyncAdapter/GoogleContactsSyncAdapter.apk,,true,))
