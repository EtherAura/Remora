# Mesa built from source, as prebuilt modules (bd remora-ykhz / remora-bm7.4).
#
# WHY PREBUILT MODULES FOR SOMETHING WE BUILD OURSELVES: Mesa does not build under soong. Its
# Android.bp (external/mesa3d in the manifest) defines only gfxstream infrastructure — no
# libEGL_mesa, no vulkan.<driver>, no libgallium_dri, no libgbm, no libglapi. Upstream Mesa
# reaches Android through its OWN meson with an NDK cross-file, which is how prebuilt Android Mesa
# binaries are produced in the first place. So the build happens host-side, in
# vendor/host-prereqs/mesa-android/build-mesa.sh, and lands here as a payload; this file is only
# how the payload becomes installed modules.
#
# MODULE NAMES ARE REMORA'S, INSTALLED NAMES ARE MESA'S. Every module here is remora_mesa_<x>
# with LOCAL_INSTALLED_MODULE_STEM set to the file Android actually loads. Reusing the prebuilt
# project's module names would be a duplicate-module error while both projects are in the tree,
# and giving two modules one INSTALL PATH is the hard kati error that already renamed
# remora_binder_alloc and gralloc.remora.
#
# WHAT MAKES THE PATH EXCLUSIVE IS NOT mesa.mk. This file used to claim that PRODUCT_PACKAGES_REMOVE
# in mesa.mk left exactly one module writing each path, and that is FALSE — it is why the feature
# failed kati the first time it was ever enabled. BUILD_PREBUILT emits the install rule at module
# DEFINITION time, so a prebuilt that nothing requests still claims its path. Un-requesting a module
# cannot un-define it. What keeps these paths exclusive is that no other project in the tree
# DEFINES the modules; a prebuilt GPU project added back would have to have its definitions
# suppressed where they live, keyed on what this payload contains. mesa.mk's REMOVE lines are still correct and still wanted — they stop
# the module being pulled in as a dependency — they are simply not the thing preventing collision.
#
# LOCAL_CHECK_ELF_FILES := false, as on every prebuilt here: these are NDK cross-builds and do not
# resolve against the platform's own symbol set.

LOCAL_PATH := $(call my-dir)

# $(1) module suffix  $(2) installed file name  $(3) relative install dir  $(4) symlinks
define remora-mesa-lib
include $$(CLEAR_VARS)
LOCAL_MODULE := remora_mesa_$(1)
LOCAL_INSTALLED_MODULE_STEM := $(2)
LOCAL_SRC_FILES := lib64/$(3)$(2)
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_RELATIVE_PATH := $(patsubst %/,%,$(3))
LOCAL_MULTILIB := 64
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_SYMLINKS := $(4)
LOCAL_CHECK_ELF_FILES := false
LOCAL_PREBUILT_STRIP_COMPS := false
include $$(BUILD_PREBUILT)
endef

# The GL/DRI cluster. The loader symlinks are DERIVED from the payload rather than listed: which
# gallium drivers were compiled in decides which
# <driver>_dri.so names must resolve, and a hand-written list would silently diverge from the
# build that produced the megalib.
mesa_dri_links := $(shell cd $(LOCAL_PATH)/lib64/dri 2>/dev/null && find . -name '*_dri.so' -type l -printf '%P\n' 2>/dev/null)
mesa_drv_links := $(shell cd $(LOCAL_PATH)/lib64/dri 2>/dev/null && find . -name '*_drv_video.so' -type l -printf '%P\n' 2>/dev/null)

# EACH ONE IS DECLARED ONLY IF THE PAYLOAD ACTUALLY HAS IT, exactly as the Vulkan ICDs below are.
# BUILD_PREBUILT generates the install rule at DEFINITION time, not at PRODUCT_PACKAGES time, so
# declaring a module whose payload file is missing STILL claims its install path. mesa.mk only
# PRODUCT_PACKAGES_REMOVEs the prebuilt counterparts when the whole cluster is present, so on a
# Vulkan-only payload the surviving prebuilt and this empty declaration both claimed
# vendor/lib64/dri/libgallium_dri.so and kati failed the build outright:
#
#   base_rules.mk:513: error: overriding commands for target
#   `out/target/product/remora_x86_64/vendor/lib64/dri/libgallium_dri.so'
#
# That is the same two-modules-one-install-path error that renamed remora_binder_alloc and
# gralloc.remora, arrived at from the opposite direction: not a name collision between two real
# modules, but a module that has no file to install still reserving the path. The comment at the
# top of this file already stated the rule; the GL cluster just did not follow it, because it was
# written when the payload was assumed to be all-or-nothing rather than Vulkan-first.
# WHO OWNS THE VA ALIASES depends on which Mesa produced the payload, and both cases are handled
# because the answer changed underneath this file. Mesa 24.0.8 built a STANDALONE
# libgallium_drv_video.so and the *_drv_video.so links pointed at it. Mesa 26 has no such target at
# all — src/gallium/meson.build descends into targets/va only `if not with_dri`, and we build the
# DRI megadriver — so it emits the same aliases pointing at libgallium_dri.so instead.
#
# THE LINKS GO TO WHICHEVER FILE THE PAYLOAD ACTUALLY HAS, AND NEVER TO BOTH. Two modules claiming
# vendor/lib64/dri/radeonsi_drv_video.so is the identical hard kati error this file documents at the
# top and that the empty-GL-cluster declaration already caused once.
mesa_va_standalone := $(wildcard $(LOCAL_PATH)/lib64/dri/libgallium_drv_video.so)
$(if $(wildcard $(LOCAL_PATH)/lib64/dri/libgallium_dri.so),\
    $(eval $(call remora-mesa-lib,gallium_dri,libgallium_dri.so,dri/,\
        $(mesa_dri_links) $(if $(mesa_va_standalone),,$(mesa_drv_links)))))
$(if $(mesa_va_standalone),\
    $(eval $(call remora-mesa-lib,gallium_drv_video,libgallium_drv_video.so,dri/,$(mesa_drv_links))))
$(if $(wildcard $(LOCAL_PATH)/lib64/egl/libEGL_mesa.so),\
    $(eval $(call remora-mesa-lib,egl,libEGL_mesa.so,egl/,)))
$(if $(wildcard $(LOCAL_PATH)/lib64/egl/libGLESv1_CM_mesa.so),\
    $(eval $(call remora-mesa-lib,glesv1,libGLESv1_CM_mesa.so,egl/,)))
$(if $(wildcard $(LOCAL_PATH)/lib64/egl/libGLESv2_mesa.so),\
    $(eval $(call remora-mesa-lib,glesv2,libGLESv2_mesa.so,egl/,)))
# libgbm and libglapi install under their VERSIONED sonames, because that is what their consumers
# DT_NEEDED. A glob for the unversioned name finds nothing and reads as absent — the naming trap
# already recorded twice in bd remora-ykhz.
$(if $(wildcard $(LOCAL_PATH)/lib64/libgbm.so.1),\
    $(eval $(call remora-mesa-lib,gbm,libgbm.so.1,,)))
# libgbm's runtime backend, at the path compiled into libgbm (-Dgbm-backends-path — see mesa.mk
# for why the pair is inseparable), and the local-gbm wrapper gralloc.minigbm_gbm_mesa dlopens
# (bd remora-ykhz.1; the venus deploy bind-mounts its vtest flavor over the installed file).
$(if $(wildcard $(LOCAL_PATH)/lib64/gbm/dri_gbm.so),\
    $(eval $(call remora-mesa-lib,gbm_backend,dri_gbm.so,gbm/,)))
$(if $(wildcard $(LOCAL_PATH)/lib64/libgbm_mesa_wrapper.so),\
    $(eval $(call remora-mesa-lib,gbm_wrapper,libgbm_mesa_wrapper.so,,)))
$(if $(wildcard $(LOCAL_PATH)/lib64/libglapi.so.0),\
    $(eval $(call remora-mesa-lib,glapi,libglapi.so.0,,)))

# The Vulkan ICDs. Each is a standalone driver the Vulkan loader dlopens by name; none of them
# links libgallium_dri or libglapi, so unlike the cluster above they move INDEPENDENTLY and one at
# a time. Only the ones the payload actually contains are declared — see mesa.mk, which decides
# which of these are installed and which prebuilt counterparts step aside.
mesa_vk_present := $(patsubst $(LOCAL_PATH)/lib64/hw/vulkan.%.so,%,$(wildcard $(LOCAL_PATH)/lib64/hw/vulkan.*.so))
$(foreach d,$(mesa_vk_present),\
    $(eval $(call remora-mesa-lib,vulkan_$(d),vulkan.$(d).so,hw/,)))
