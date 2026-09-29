# Magisk (su + DenyList). ELF binaries + apks are ETC-class BUILD_PREBUILT modules (Android.mk);
# the shell helpers + init rc are plain PRODUCT_COPY_FILES. Runtime SELinux via magiskpolicy
# --live (no build-time sepolicy). WITH_MAGISK=true.
MAGISK_ROOT := vendor/magisk

PRODUCT_PACKAGES += remora_magisk_bin remora_magisk_boot remora_magisk_init \
    remora_magisk_policy remora_magisk_busybox remora_magisk_apk remora_magisk_stub

# only the non-binary helpers (*.sh) + the init rc
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(MAGISK_ROOT)/magisk/*.sh),\
    $(f):$(TARGET_COPY_OUT_SYSTEM)/etc/init/magisk/$(notdir $(f)))
PRODUCT_COPY_FILES += \
    $(MAGISK_ROOT)/magisk.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/magisk.rc

# Remora's own tmpfs setup, which upstream Magisk cannot do for itself (bd remora-28ix.6).
# Copied EXPLICITLY, not by the wildcard above: that one globs $(MAGISK_ROOT)/magisk/*.sh, i.e.
# inside the vendored payload, and this script is ours and lives beside it rather than in it —
# keeping Remora's code out of the directory whose provenance is "exactly what upstream shipped".
# Without this line the rc execs a file that was never installed, and root silently never starts.
PRODUCT_COPY_FILES += \
    $(MAGISK_ROOT)/magisk-setup.sh:$(TARGET_COPY_OUT_SYSTEM)/etc/init/magisk/magisk-setup.sh
