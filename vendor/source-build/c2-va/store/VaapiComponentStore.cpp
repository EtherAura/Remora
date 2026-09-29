// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiComponentStore"

#include <c2_va/store/VaapiComponentStore.h>

#include <stdint.h>

#include <memory>
#include <mutex>

#include <C2.h>
#include <C2Config.h>
#include <log/log.h>
#include <media/stagefright/foundation/MediaDefs.h>

#include <c2_va/common/VaapiComponentName.h>
#include <c2_va/store/VaapiComponentFactory.h>
#include <v4l2_codec2/components/ComponentStore.h>

namespace android {

// static
std::shared_ptr<C2ComponentStore> VaapiComponentStore::Create() {
    ALOGV("%s()", __func__);

    static std::mutex mutex;
    static std::weak_ptr<C2ComponentStore> platformStore;

    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<C2ComponentStore> store = platformStore.lock();
    if (store != nullptr) return store;

    auto builder = ComponentStore::Builder("android.componentStore.vaapi");

    builder.decoder(VaapiComponentName::kH264Decoder, VideoCodec::H264,
                    &VaapiComponentFactory::create);
    builder.decoder(VaapiComponentName::kHEVCDecoder, VideoCodec::HEVC,
                    &VaapiComponentFactory::create);
    builder.decoder(VaapiComponentName::kVP9Decoder, VideoCodec::VP9,
                    &VaapiComponentFactory::create);
    builder.decoder(VaapiComponentName::kAV1Decoder, VideoCodec::AV1,
                    &VaapiComponentFactory::create);

    // HEVC encoder is the priority (full-res H.265 mirroring).
    builder.encoder(VaapiComponentName::kHEVCEncoder, VideoCodec::HEVC,
                    &VaapiComponentFactory::create);
    // H.264 encoder shares the same VAAPI backend infrastructure.
    builder.encoder(VaapiComponentName::kH264Encoder, VideoCodec::H264,
                    &VaapiComponentFactory::create);

    // One registration per component, under its c2.remora.vaapi.* name only — see
    // VaapiComponentName.h.

    store = std::shared_ptr<C2ComponentStore>(std::move(builder).build());
    platformStore = store;
    return store;
}

}  // namespace android
