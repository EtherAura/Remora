# Presigned microG prebuilt app modules (PRODUCT_COPY_FILES rejects .apk — this is the required
# BUILD_PREBUILT form), mirroring the gapps feature. GmsCore + Companion are priv-app on product
# (they need the privileged permission set); Aurora Store is a plain product app.
LOCAL_PATH := $(call my-dir)

define remora-microg-app
include $(CLEAR_VARS)
LOCAL_MODULE := $(1)
LOCAL_MODULE_OWNER := microg
LOCAL_SRC_FILES := $(2)
LOCAL_CERTIFICATE := PRESIGNED
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_SUFFIX := $$(COMMON_ANDROID_PACKAGE_SUFFIX)
LOCAL_DEX_PREOPT := false
LOCAL_PRIVILEGED_MODULE := $(3)
LOCAL_PRODUCT_MODULE := true
include $$(BUILD_PREBUILT)
endef

$(eval $(call remora-microg-app,MicroGmsCore,product/priv-app/MicroGmsCore/MicroGmsCore.apk,true))
$(eval $(call remora-microg-app,MicroGCompanion,product/priv-app/MicroGCompanion/MicroGCompanion.apk,true))
$(eval $(call remora-microg-app,AuroraStore,product/app/AuroraStore/AuroraStore.apk,))
