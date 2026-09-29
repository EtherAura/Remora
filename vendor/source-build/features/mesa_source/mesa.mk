# Mesa from source — which prebuilts step aside, and the rules that decide it
# (bd remora-ykhz R3, jointly with bd remora-bm7.4).
#
# MEASURED SET, by hashing every file in the prebuilt GPU project's x86_64 set and
# looking each hash up in the live image (name matching under-reports — AOSP builds several of
# these itself under the same names and the soong output wins). SEVENTEEN prebuilts shipped:
# fifteen Mesa, plus libc++_shared and the prebuilt composer. This file is about the fifteen.
#
# THEY ARE NOT ONE THING. Two clusters, with different rules, and conflating them is what makes R3
# look like an all-or-nothing 15-file swap when it is not:
#
#  1. THE GL/DRI CLUSTER — libgallium_dri, libgallium_drv_video, libEGL_mesa, libGLESv1_CM_mesa,
#     libGLESv2_mesa, libgbm, libglapi. THESE MOVE TOGETHER OR NOT AT ALL. The DRI loader ABI
#     changed between 24.0.8 and 26.x: the prebuilt exports __driDriverGetExtensions_<driver> per
#     driver and Mesa 26 exports none of them, so the loader half and the driver half must agree on
#     which interface they speak. A partial swap links and installs and then fails at runtime,
#     which is the worst available outcome. Hence the refusal below rather than a best-effort.
#
#  2. THE VULKAN ICDs — vulkan.{intel,intel_hasvk,radeon,broadcom,freedreno,nouveau}. Each is a
#     standalone driver the loader dlopens by name; none links libgallium_dri or libglapi, so they
#     are INDEPENDENTLY replaceable, one driver at a time. That makes them the correct first slice,
#     not the GL cluster the README used to lead with.
#
# NOT IN SCOPE, and deliberately: gralloc.cros and gralloc.gbm ship from the same project and are
# NOT Mesa — they come from minigbm / gbm_gralloc, and minigbm is already in our patch set. They
# are a separate slice. libc++_shared is the NDK runtime the whole prebuilt graphics stack links;
# it retires when the LAST of these does, not with any one of them.

# A TREE-RELATIVE PATH, not $(call my-dir). This file is PRODUCT CONFIG, inherited by
# stage-features.sh, and my-dir does not resolve to this directory there — it resolved to something
# else entirely, so every $(wildcard $(LOCAL_PATH)/lib64/...) below found NOTHING. The failure was
# silent and total: MESA_SRC_VK_HAVE came back empty, no remora_mesa_vulkan_* reached
# PRODUCT_PACKAGES, our module was never built, and because the companion patch had already removed
# the PREBUILT definition the image shipped with NO radeon Vulkan ICD AT ALL — strictly worse than
# the 24.0.8 driver it was meant to replace, and it built cleanly while doing it.
#
# Every other feature product-mk here uses exactly this idiom (GAPPS_ROOT := vendor/gapps,
# NDK_ROOT := vendor/ndk_translation, MICROG_ROOT, MAGISK_ROOT). mesa.mk was the only one reaching
# for my-dir, and it is the only one that silently shipped nothing. Android.mk keeps my-dir, which
# is correct THERE — that file really is parsed as a module makefile.
MESA_SRC_ROOT := vendor/mesa_source
LOCAL_PATH := $(MESA_SRC_ROOT)

# THE SET IS WHAT MESA 26.x ACTUALLY BUILDS, which is not what 24.0.8 shipped. Two files that were
# in the prebuilt cluster are deliberately NOT here, and both keep their prebuilt:
#  - libglapi.so.0: shared-glapi was REMOVED from Mesa upstream, so there is no counterpart to
#    build. Its prebuilt is left DEFINED here, but note what actually happens: it was only ever
#    installed as a LOCAL_REQUIRED_MODULES of the GL modules that now step aside, so with those
#    suppressed nothing requests it and it DOES NOT SHIP. Verified on the assembled image — no
#    /vendor/lib64/libglapi.so.0, and a readelf sweep of all 182 vendor libraries finds ZERO
#    DT_NEEDED references to it. That is correct: Mesa 26's libEGL_mesa does not link it, and this
#    product is 64-bit only (core_64_bit_only.mk), so the 32-bit stack that used to need it is not
#    built either. Left defined rather than deleted so the entry survives for a product that does
#    build 32-bit, where the prebuilt would again be the only libglapi available.
#  - dri/libgallium_drv_video.so: kept prebuilt, but NOT for the reason recorded here
# which measured false on both halves (bd remora-ykhz). It said the VA frontend
#    "needs libva-drm, which AOSP's external/libva does not define". Nothing links libva-drm: the
#    shipped drv_video's DT_NEEDED is libdrm/libz/libc++_shared and no libva at all, because a VA
#    driver is dlopened BY libva rather than linked against it. Mesa takes libva HEADERS ONLY
#    (meson.build:750 partial_dependency), and build-mesa.sh now generates the libva.pc that makes
#    that dependency resolve — the frontend configures, and `Frontends: mesa va` comes out.
#    WHAT ACTUALLY REMAINS is a packaging step, not a build one: Mesa 26 has no standalone VA
#    target at all (src/gallium/meson.build descends into targets/va only `if not with_dri`), and
#    serves VA from the MEGADRIVER — targets/dri emits <driver>_drv_video.so as a SYMLINK to
#    libgallium_dri.so. So the replacement is not a new payload file; it is an alias set beside the
#    libgallium_dri.so already installed here, plus extending the prebuilt GPU project's
#    definition-suppression patch (see the PRODUCT_PACKAGES comment below) so the prebuilt stops
#    claiming the path. Until that lands the prebuilt stays and keeps serving VA.
# Requiring either would make the gate refuse every payload this build can produce, forever.
MESA_SRC_GL_SET := \
    dri/libgallium_dri.so \
    egl/libEGL_mesa.so \
    egl/libGLESv1_CM_mesa.so \
    egl/libGLESv2_mesa.so \
    libgbm.so.1 \
    gbm/dri_gbm.so \
    libgbm_mesa_wrapper.so \

# The last two joined the cluster with bd remora-ykhz.1, and they are cluster members by the same
# move-together logic, not by being Mesa proper:
#  - gbm/dri_gbm.so is libgbm's RUNTIME BACKEND. Mesa 26 libgbm loads it from a compiled-in
#    /vendor/lib64/gbm (build-mesa.sh sets -Dgbm-backends-path; the meson default is a path no
#    Android namespace permits), and without it gbm_create_device fails for EVERY consumer —
#    including gralloc.gbm, the image's fallback default, which is how the gbm path shipped
#    silently dead on the first Mesa-26 images. A libgbm without its backend is the mixed pair
#    this gate exists to refuse.
#  - libgbm_mesa_wrapper.so is what gralloc.minigbm_gbm_mesa dlopens, LOCAL-gbm flavor, baked so
#    every non-venus host has a working gbm_mesa gralloc (the venus deploy bind-mounts its vtest
#    wrapper over this file, so the NVIDIA path is unchanged by construction). It is built by the
#    same build-mesa.sh pass against this payload's libgbm, so a payload missing it means the
#    script did not finish — the same "some of it" condition the gate refuses for the rest.
mesa_src_gl_have := $(strip $(foreach f,$(MESA_SRC_GL_SET),$(wildcard $(LOCAL_PATH)/lib64/$(f))))
mesa_src_gl_want := $(words $(MESA_SRC_GL_SET))
mesa_src_gl_got  := $(words $(mesa_src_gl_have))

# REFUSE A PARTIAL GL CLUSTER. Not a warning: a warning here produces an image that boots, renders
# through a loader and a driver that disagree about the DRI interface, and fails somewhere far from
# the cause. The payload is produced by one script in one pass, so "some of it" means that script
# did not finish, and the honest response is to stop.
ifneq ($(mesa_src_gl_got),0)
ifneq ($(mesa_src_gl_got),$(mesa_src_gl_want))
$(error mesa_source: the GL/DRI payload is INCOMPLETE — $(mesa_src_gl_got) of $(mesa_src_gl_want) \
files under $(LOCAL_PATH)/lib64. These must be replaced together, because the DRI loader ABI \
changed between the prebuilt Mesa 24.0.8 and 26.x and a mixed pair fails at runtime, not at build \
time. Missing: $(filter-out $(patsubst $(LOCAL_PATH)/lib64/%,%,$(mesa_src_gl_have)),$(MESA_SRC_GL_SET)). \
Re-run vendor/host-prereqs/mesa-android/build-mesa.sh, or remove the partial payload and leave \
the prebuilts in place)
endif

# The whole cluster is present: install ours, and take the prebuilt counterparts out of
# PRODUCT_PACKAGES.
#
# THIS IS NOT WHAT MAKES THE INSTALL PATH EXCLUSIVE, despite what this comment used to say.
# BUILD_PREBUILT emits the install rule when a module is DEFINED, so a prebuilt nothing requests
# still claims its path, and two claimants is a hard kati error. Suppressing the DEFINITION is a
# patch to the prebuilt GPU project, and that patch covered the Vulkan ICDs ONLY — the GL
# cluster below would still collide the moment a payload supplies it. Extend the filter in
# container-patches .../0006-A17-let-mesa_source-replace-a-Vulkan-prebuilt-at-all.patch before
# turning the GL slice on. These REMOVE lines remain correct and wanted: they stop the prebuilt
# arriving as someone else's dependency.
PRODUCT_PACKAGES += \
    remora_mesa_gallium_dri \
    remora_mesa_egl \
    remora_mesa_glesv1 \
    remora_mesa_glesv2 \
    remora_mesa_gbm \
    remora_mesa_gbm_backend \
    remora_mesa_gbm_wrapper \

# Suppressing the DEFINITION is what avoids the kati collision (the prebuilt GPU project's patch
# did that); these REMOVE lines remain correct and wanted for the separate reason they always were —
# they stop the prebuilt arriving as somebody else's LOCAL_REQUIRED_MODULES.
#
# libglapi is absent from BOTH lists on purpose, and drv_video USED TO BE for a reason that no
# longer holds. They shared the wording "Mesa 26.x builds neither": true of libglapi (shared-glapi
# was removed upstream, so there is no counterpart at any bitness) and FALSE of drv_video. Mesa 26
# builds the VA frontend INTO libgallium_dri.so and names <driver>_drv_video.so aliases beside it,
# which is why drv_video is handled below rather than here — there is no separate PACKAGE to add,
# only a prebuilt to stop installing.
PRODUCT_PACKAGES_REMOVE += \
    libgallium_dri \
    libEGL_mesa \
    libGLESv1_CM_mesa \
    libGLESv2_mesa \

# THE VA PREBUILT STEPS ASIDE ONLY IF THE PAYLOAD ACTUALLY CARRIES THE ALIASES, which is a
# NARROWER condition than the GL cluster above and must stay that way. build-mesa.sh emits
# <driver>_drv_video.so only for VA-capable gallium drivers (radeonsi/r600/nouveau/virgl — iris has
# none, Intel video is iHD's), so a perfectly valid iris-only payload supplies the whole GL cluster
# and NO VA at all. Keying this on the cluster instead would remove the prebuilt and replace it with
# nothing, leaving libva with no driver to open on an Intel-only source build.
#
# As everywhere else here, REMOVE is not what prevents the kati collision — the prebuilt DEFINITION
# is suppressed in the prebuilt GPU project (container-patches 0008), keyed on the same payload
# aliases. This line stops it arriving as somebody else's LOCAL_REQUIRED_MODULES.
mesa_src_va_links := $(wildcard $(LOCAL_PATH)/lib64/dri/*_drv_video.so)
ifneq ($(mesa_src_va_links),)
PRODUCT_PACKAGES_REMOVE += libgallium_drv_video
endif

# gralloc.cros RETIRES WITH THE GL CLUSTER, and only with it (bd remora-ykhz.1): its amdgpu
# backend dri_init()s against the __driDriverGetExtensions interface this Mesa no longer
# exports, and the failure is -ENODEV at init — a gralloc that cannot start on the hardware it
# names. gpu_config.sh translates gralloc=cros to minigbm_gbm_mesa on exactly these images
# (keyed on the wrapper + backend files this cluster installs), so nothing selects it. Scoped
# HERE rather than in device/remora because an image built WITHOUT mesa_source still serves
# real cros and must keep shipping it — the same reason the translation is file-conditional.
# assemble-image.sh carries the matching payload-conditional exclusion for incremental builds.
PRODUCT_PACKAGES_REMOVE += gralloc.cros
# gralloc.gbm RETIRES ON THE SAME KEY, for a different reason (bd remora-ykhz). cros goes because it
# cannot initialise against Mesa 26; gbm goes because it is the LAST PREBUILT HOLDING libc++_shared
# (drv_video retired, vulkan.virtio is out of scope) and minigbm_gbm_mesa serves every
# case it did. gpu_config.sh translates gbm -> minigbm_gbm_mesa on exactly these images, so profiles
# that ask for gbm — and the bare default, which is gbm — keep working unchanged.
#
# SAFE BECAUSE THE DEFAULT IS HOST-ONLY: that default lives in gpu_setup_host, and gpu_setup reaches
# it only with a render node; the nodeless path is gpu_setup_guest, which sets gralloc=remora and
# never consults it. Both libraries need a node regardless — gralloc.gbm links libgbm and opens
# /dev/dri/renderD128 — so retiring it gives up nothing minigbm_gbm_mesa cannot do.
PRODUCT_PACKAGES_REMOVE += gralloc.gbm
# NOTE ON libgbm/libglapi: they are not named in the prebuilt GPU project's prebuilts.mk
# PRODUCT_PACKAGES at all — they arrive as LOCAL_REQUIRED_MODULES of the modules above, so removing
# their parents is what removes them. Listing them here would be a no-op that reads as coverage.
endif

# --- Vulkan ICDs, per driver -------------------------------------------------------------------
# Whatever the payload contains, and nothing more. A driver absent from the payload keeps its
# prebuilt, which is exactly the incremental property the GL cluster cannot have.
MESA_SRC_VK_HAVE := $(patsubst $(LOCAL_PATH)/lib64/hw/vulkan.%.so,%,$(wildcard $(LOCAL_PATH)/lib64/hw/vulkan.*.so))

# REFUSE TO BUILD A PAYLOAD-SHAPED HOLE. If this file is being parsed at all then WITH_MESA_SOURCE
# was set and stage-features.sh already verified lib64/ exists — so finding NOTHING here is a
# contradiction, and it means the paths in this file are wrong rather than that there is nothing to
# install. That exact contradiction shipped once: a bad LOCAL_PATH made every wildcard empty, the
# companion patch removed the PREBUILT anyway, and the image built CLEANLY with no radeon Vulkan
# driver at all. A missing driver is not a smaller version of a working one, and the build should
# never again be able to produce that quietly.
ifeq ($(strip $(MESA_SRC_VK_HAVE))$(strip $(mesa_src_gl_have)),)
$(error mesa_source: the payload under $(LOCAL_PATH)/lib64 yielded NO drivers, neither Vulkan ICDs \
nor a GL cluster. The feature is enabled, so this is a PATH bug in this file, not an empty payload \
— and continuing would remove the prebuilts and install nothing in their place. Check that \
$(LOCAL_PATH) really is where stage-features.sh put the payload)
endif

PRODUCT_PACKAGES += $(foreach d,$(MESA_SRC_VK_HAVE),remora_mesa_vulkan_$(d))
PRODUCT_PACKAGES_REMOVE += $(foreach d,$(MESA_SRC_VK_HAVE),vulkan.$(d))

# vulkan.virtio and vulkan.lvp are deliberately NOT reachable here and never will be: neither comes
# from the prebuilt GPU project. virtio (Venus) comes from the prebuilt Venus guest set and is
# the driver actually in use on the NVIDIA host; lvp is built elsewhere. Verified by content —
# both are in the image and neither matched any prebuilt hash.

$(call inherit-product-if-exists, $(LOCAL_PATH)/mesa-local.mk)
