# vendor/remora — Remora's own vendor project (bd remora-28ix.4 R2).
#
# WHAT THIS OWNS: the container's init behaviour, GPU bring-up and boot-property namespace — the
# parts that decide how a Remora container boots and renders. All authored here, reading
# ro.boot.remora_* directly.
#
# ALSO OWNED, since R2b: the three native modules. Each carries a Remora-specific soong module
# name because soong rejects duplicate module names even for modules nothing builds, so a generic
# name collides with any other project in the tree that defines it.
#
# THERE IS NO `stem`, and an earlier version of this comment claimed there was — corrected against
# the tree: binder_alloc/Android.bp declares name "remora_binder_alloc" and no stem at all, and the
# built image installs /vendor/bin/remora_binder_alloc. A stem back to binder_alloc is a hard kati
# error (two modules writing one install path) in any tree that also builds a binder_alloc. The
# INSTALLED name therefore moved too, which is why the caller had to move with it: init's
# exec of that path is retargeted by a container patch AND, because a patch can fail to re-apply
# and once did, called again from remora.common.rc — see the note there.
#
# Guest mode, the one thing here that could not be proven by compiling, is verified on a shipped
# image — booting unaided on gralloc.remora with zero RenderThread crashes (bd remora-28ix.4).
# gpu_config.sh sets ro.hardware.hwcomposer itself in both branches; no container patch does.

PRODUCT_BROKEN_VERIFY_USES_LIBRARIES := true

PRODUCT_PACKAGES += \
    remora_binder_alloc \
    remora_ipconfigstore \
    gralloc.remora \
    hwcomposer.remora \

PRODUCT_COPY_FILES += \
    vendor/remora/gpu_config.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gpu_config.sh \
    vendor/remora/post-fs-data.remora.sh:$(TARGET_COPY_OUT_VENDOR)/bin/post-fs-data.remora.sh \
    vendor/remora/remora.c2.sh:$(TARGET_COPY_OUT_VENDOR)/bin/remora.c2.sh \
    vendor/remora/remora.common.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/remora.common.rc \
    vendor/remora/remora.c2.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/remora.c2.rc \
    vendor/remora/remora.props.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/remora.props.rc \

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.touchscreen.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.xml \
