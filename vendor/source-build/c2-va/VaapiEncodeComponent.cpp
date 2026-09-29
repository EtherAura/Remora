// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiEncodeComponent"

#include <c2_va/VaapiEncodeComponent.h>

#include <base/bind.h>

#include <cutils/properties.h>
#include <log/log.h>

#include <c2_va/vaapi/VaapiVideoEncoder.h>
#include <v4l2_codec2/common/H264.h>
#include <v4l2_codec2/components/BitstreamBuffer.h>
#include <v4l2_codec2/components/EncodeInterface.h>
#include <v4l2_codec2/components/VideoEncoder.h>

namespace android {

// static
std::atomic<int32_t> VaapiEncodeComponent::sConcurrentInstances = 0;

// static
std::shared_ptr<C2Component> VaapiEncodeComponent::create(
        const std::string& name, c2_node_id_t id, std::shared_ptr<EncodeInterface> intfImpl,
        C2ComponentFactory::ComponentDeleter deleter) {
    ALOGV("%s(%s)", __func__, name.c_str());

    static const int32_t kMaxConcurrentInstances =
            property_get_int32("ro.vendor.vaapi_codec2.encode_concurrent_instances", -1);

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (kMaxConcurrentInstances >= 0 && sConcurrentInstances.load() >= kMaxConcurrentInstances) {
        ALOGW("Cannot create additional encoder, maximum number of instances reached: %d",
              kMaxConcurrentInstances);
        return nullptr;
    }

    return std::shared_ptr<C2Component>(new VaapiEncodeComponent(name, id, std::move(intfImpl)),
                                        deleter);
}

VaapiEncodeComponent::VaapiEncodeComponent(const std::string& name, c2_node_id_t id,
                                           std::shared_ptr<EncodeInterface> intfImpl)
      : EncodeComponent(name, id, std::move(intfImpl)) {
    ALOGV("%s():", __func__);
    sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);
}

VaapiEncodeComponent::~VaapiEncodeComponent() {
    ALOGV("%s():", __func__);
    sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
}

bool VaapiEncodeComponent::initializeEncoder() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mEncoderTaskRunner->RunsTasksInCurrentSequence());
    ALOG_ASSERT(!mInputFormatConverter);
    ALOG_ASSERT(!mEncoder);

    mLastFrameTime = std::nullopt;

    // Get the requested profile and level.
    C2Config::profile_t outputProfile = mInterface->getOutputProfile();

    // The VAAPI encoder emits the parameter sets inline in the first keyframe (SPS/PPS for H.264,
    // VPS/SPS/PPS for HEVC). MediaCodec/scrcpy require them delivered as a separate codec-config
    // buffer, so the framework must split them out for BOTH codecs (extractCSDInfo now handles
    // HEVC too). Without this the first packet isn't a config packet and scrcpy rejects the stream.
    mExtractCSD = true;

    // The VAAPI encoder core (VaapiVideoEncoder) selects the concrete VA level
    // itself from the resolution/bitrate, mirroring libva-utils' encoder samples,
    // so we do not translate the C2 level here. (The V4L2 path passes a
    // V4L2-fourcc-encoded level, which is meaningless to libva.) A nullopt level
    // tells the core "derive it".
    std::optional<uint8_t> level;

    // Get the stride used by the C2 framework, as this might differ from the
    // stride the VAAPI encode source surface uses.
    std::optional<uint32_t> stride =
            getVideoFrameStride(VideoEncoder::kInputPixelFormat, mInterface->getInputVisibleSize());
    if (!stride) {
        // The image's gbm gralloc can't allocate a throwaway YCBCR_420_888 probe block (C2_CORRUPTED),
        // but our encoder VPP-imports each real input dma-buf using its own per-frame layout, so the
        // probe stride is only a hint — fall back to a 16-aligned width instead of failing.
        uint32_t w = static_cast<uint32_t>(mInterface->getInputVisibleSize().width);
        stride = (w + 15u) & ~15u;
        ALOGW("Stride probe failed; falling back to 16-aligned width stride=%u", *stride);
    }

    // Get the requested bitrate mode and bitrate. The C2 framework doesn't offer a
    // parameter to configure the peak bitrate, so we use a multiple of the target.
    mBitrateMode = mInterface->getBitrateMode();
    mBitrate = mInterface->getBitrate();

    mEncoder = VaapiVideoEncoder::create(
            outputProfile, level, mInterface->getInputVisibleSize(), *stride,
            mInterface->getKeyFramePeriod(), mBitrateMode, mBitrate,
            mBitrate * VideoEncoder::kPeakBitrateMultiplier,
            ::base::BindRepeating(&VaapiEncodeComponent::fetchOutputBlock, mWeakThis),
            ::base::BindRepeating(&VaapiEncodeComponent::onInputBufferDone, mWeakThis),
            ::base::BindRepeating(&VaapiEncodeComponent::onOutputBufferDone, mWeakThis),
            ::base::BindRepeating(&VaapiEncodeComponent::onDrainDone, mWeakThis),
            ::base::BindRepeating(&VaapiEncodeComponent::reportError, mWeakThis, C2_CORRUPTED),
            mEncoderTaskRunner);
    if (!mEncoder) {
        ALOGE("Failed to create VaapiVideoEncoder (profile: %s)", profileToString(outputProfile));
        return false;
    }

    return true;
}

}  // namespace android
