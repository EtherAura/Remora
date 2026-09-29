# Magisk raw payloads (ELF binaries + apks) installed to /system/etc/init/magisk as ETC-class
# prebuilts — they're runtime blobs, not linked modules, so ETC avoids both the .apk and ELF
# PRODUCT_COPY_FILES checks. The .sh files stay as PRODUCT_COPY_FILES (not ELF).
LOCAL_PATH := $(call my-dir)

define remora-magisk-etc
include $(CLEAR_VARS)
LOCAL_MODULE := remora_magisk_$(1)
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES := magisk/$(2)
LOCAL_MODULE_STEM := $(2)
LOCAL_MODULE_PATH := $$(TARGET_OUT)/etc/init/magisk
LOCAL_CHECK_ELF_FILES := false
include $$(BUILD_PREBUILT)
endef

$(eval $(call remora-magisk-etc,bin,magisk))
$(eval $(call remora-magisk-etc,boot,magiskboot))
$(eval $(call remora-magisk-etc,init,magiskinit))
$(eval $(call remora-magisk-etc,policy,magiskpolicy))
$(eval $(call remora-magisk-etc,busybox,busybox))
$(eval $(call remora-magisk-etc,apk,magisk.apk))
$(eval $(call remora-magisk-etc,stub,stub.apk))
