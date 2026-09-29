# Copyright 2020 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Product makefile for the VAAPI Codec2 decode HAL (hardware/remora/c2-va).
# Inherited by device/remora's remora.mk. The path is the same on both releases, so there
# is one soong namespace name to export rather than two (bd remora-28ix.4).

# --- Soong namespaces ----------------------------------------------------------
# c2-va lives in its own soong namespace (hardware/remora/c2-va) and reuses the C2
# framework from external/v4l2_codec2 (also namespaced). Both must be exported or
# soong parses the .bp but emits NO modules -> "unknown target ...-service-vaapi"
# at build time.
PRODUCT_SOONG_NAMESPACES += \
    external/v4l2_codec2 \
    hardware/remora/c2-va

# --- Binaries / driver ---------------------------------------------------------
# The HIDL c2 service, plus the in-tree iHD VAAPI driver
# (external/intel-media-driver) it dlopen()s at runtime. libva/libdrm come in as
# transitive shared_libs of the service binary, so they do not need to be listed
# here.
PRODUCT_PACKAGES += \
    android.hardware.media.c2@1.2-service-vaapi \
    iHD_drv_video

# --- libva driver discovery ----------------------------------------------------
# Force the iHD backend for the Intel xe SR-IOV VF. Mirrors the setenv lines in
# the service .rc; also exported at build/board level so any other libva client
# in the image resolves the same driver. iHD_drv_video installs to
# /vendor/lib64/dri, which is where LIBVA_DRIVERS_PATH points.
BOARD_LIBVA_DRIVER_NAME := iHD

# --- Config files --------------------------------------------------------------
# media_codecs fragments -> /vendor/etc (referenced by the device media_codecs.xml),
# the seccomp extension -> /vendor/etc/seccomp_policy/ under the exact filename
# service.cpp passes to SetUpMinijail as the extended policy, and the .rc/vintf
# fragment which are already installed by the cc_binary (init_rc + vintf_fragments)
# so they are NOT copied again here.
PRODUCT_COPY_FILES += \
    hardware/remora/c2-va/media_codecs_c2_va.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2_va.xml \
    hardware/remora/c2-va/media_codecs_performance_c2_va.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_performance_c2_va.xml \
    hardware/remora/c2-va/c2_va.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2@1.2-service-vaapi-seccomp_policy
