# libndk_translation ELF prebuilts. AOSP rejects ELF files in PRODUCT_COPY_FILES, so the
# translator libs + runner are BUILD_PREBUILT modules. check_elf_files off — these are prebuilt
# from Google's ndk_translation build and don't resolve against our tree's symbols.
LOCAL_PATH := $(call my-dir)

define remora-ndk-lib
include $(CLEAR_VARS)
LOCAL_MODULE := $(1)
LOCAL_SRC_FILES := lib64/$(1).so
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MULTILIB := 64
LOCAL_MODULE_TAGS := optional
LOCAL_CHECK_ELF_FILES := false
LOCAL_PREBUILT_STRIP_COMPS := false
include $$(BUILD_PREBUILT)
endef

NDK_LIBS := $(patsubst lib64/%.so,%,$(patsubst $(LOCAL_PATH)/%,%,$(wildcard $(LOCAL_PATH)/lib64/*.so)))
$(foreach l,$(NDK_LIBS),$(eval $(call remora-ndk-lib,$(l))))

# the binfmt_misc runner (x86_64 ELF executable)
include $(CLEAR_VARS)
LOCAL_MODULE := ndk_translation_program_runner_binfmt_misc_arm64
LOCAL_SRC_FILES := bin/ndk_translation_program_runner_binfmt_misc_arm64
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MULTILIB := 64
LOCAL_MODULE_TAGS := optional
LOCAL_CHECK_ELF_FILES := false
include $(BUILD_PREBUILT)
