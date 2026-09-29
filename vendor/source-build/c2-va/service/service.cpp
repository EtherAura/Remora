// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// VAAPI Codec2 HIDL service entry point. Mirrors
// external/v4l2_codec2/service/service.cpp but instantiates the VAAPI
// IComponentStore (hardware/remora/c2-va) instead of the V4L2 one.

//#define LOG_NDEBUG 0
#ifdef V4L2_CODEC2_SERVICE_VAAPI_STORE
#define LOG_TAG "android.hardware.media.c2@1.2-service-vaapi"
#else
#error "V4L2_CODEC2_SERVICE_VAAPI_STORE has to be defined"
#endif

#include <C2Component.h>
#include <base/logging.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
#include <minijail.h>

#ifdef V4L2_CODEC2_SERVICE_VAAPI_STORE
#include <c2_va/store/VaapiComponentStore.h>
#endif

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-default-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// We ship the VAAPI/libdrm specific extra syscalls here (c2_va.policy is copied
// to this path by va.mk) so the base c2 policy is not clobbered.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2@1.2-service-vaapi-seccomp_policy";

int main(int /* argc */, char** /* argv */) {
    ALOGD("Service starting...");

    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    android::hardware::configureRpcThreadpool(16, true /* callerWillJoin */);

#if LOG_NDEBUG == 0
    ALOGD("Enable all verbose logging of libchrome");
    logging::SetMinLogLevel(-5);
#endif

    // Create IComponentStore service.
    {
        using namespace ::android::hardware::media::c2::V1_2;
        android::sp<IComponentStore> store = nullptr;

#ifdef V4L2_CODEC2_SERVICE_VAAPI_STORE
        ALOGD("Instantiating Codec2's VAAPI IComponentStore service...");
        store = new utils::ComponentStore(android::VaapiComponentStore::Create());
#endif

        if (store == nullptr) {
            ALOGE("Cannot create Codec2's IComponentStore service.");
        } else if (store->registerAsService("default1") != android::OK) {
            ALOGE("Cannot register Codec2's IComponentStore service.");
        } else {
            ALOGI("Codec2's VAAPI IComponentStore service created.");
        }
    }

    android::hardware::joinRpcThreadpool();
    ALOGD("Service shutdown.");
    return 0;
}
