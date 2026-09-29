// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiDecodeComponent"

#include <c2_va/VaapiDecodeComponent.h>
#include <c2_va/remote/RemoteVideoDecoder.h>
#include <c2_va/vaapi/VaapiVideoDecoder.h>

#include <base/bind.h>
#include <base/callback_helpers.h>

#include <C2PlatformSupport.h>
#include <cutils/properties.h>
#include <utils/Trace.h>
#include <v4l2_codec2/common/Common.h>

namespace android {

namespace {
// CCBC pauses sending input buffers to the component when all the output slots are filled by
// pending decoded buffers. If the available output buffers are exhausted before CCBC pauses sending
// input buffers, CCodec may timeout due to waiting for a available output buffer.
// This function returns the minimum number of output buffers to prevent the buffers from being
// exhausted before CCBC pauses sending input buffers.
size_t getMinNumOutputBuffers(VideoCodec codec) {
    // The constant values copied from CCodecBufferChannel.cpp.
    // (b/184020290): Check the value still sync when seeing error message from CCodec:
    // "previous call to queue exceeded timeout".
    constexpr size_t kSmoothnessFactor = 4;
    constexpr size_t kRenderingDepth = 3;
    // Extra number of needed output buffers for the decoder.
    constexpr size_t kExtraNumOutputBuffersForDecoder = 2;

    // The total needed number of output buffers at pipeline are:
    // - MediaCodec output slots: output delay + kSmoothnessFactor
    // - Surface: kRenderingDepth
    // - Component: kExtraNumOutputBuffersForDecoder
    return DecodeInterface::getOutputDelay(codec) + kSmoothnessFactor + kRenderingDepth +
           kExtraNumOutputBuffersForDecoder;
}
}  // namespace

// static
std::atomic<int32_t> VaapiDecodeComponent::sConcurrentInstances = 0;

// static
std::atomic<uint32_t> VaapiDecodeComponent::sNextDebugStreamId = 0;

// static
std::shared_ptr<C2Component> VaapiDecodeComponent::create(
        const std::string& name, c2_node_id_t id, std::shared_ptr<DecodeInterface> intfImpl,
        C2ComponentFactory::ComponentDeleter deleter) {
    static const int32_t kMaxConcurrentInstances =
            property_get_int32("ro.vendor.vaapi_codec2.decode_concurrent_instances", -1);
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);

    if (kMaxConcurrentInstances >= 0 && sConcurrentInstances.load() >= kMaxConcurrentInstances) {
        ALOGW("Reject to Initialize() due to too many instances: %d", sConcurrentInstances.load());
        return nullptr;
    } else if (sConcurrentInstances.load() == 0) {
        sNextDebugStreamId.store(0, std::memory_order_relaxed);
    }

    uint32_t debugStreamId = sNextDebugStreamId.fetch_add(1, std::memory_order_relaxed);
    return std::shared_ptr<C2Component>(
            new VaapiDecodeComponent(debugStreamId, name, id, std::move(intfImpl)), deleter);
}

VaapiDecodeComponent::VaapiDecodeComponent(uint32_t debugStreamId, const std::string& name,
                                           c2_node_id_t id,
                                           std::shared_ptr<DecodeInterface> intfImpl)
      : DecodeComponent(debugStreamId, name, id, intfImpl) {
    ALOGV("%s(): ", __func__);
    sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);
}

VaapiDecodeComponent::~VaapiDecodeComponent() {
    ALOGV("%s(): ", __func__);
    sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
}

void VaapiDecodeComponent::startTask(c2_status_t* status, ::base::WaitableEvent* done) {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mDecoderTaskRunner->RunsTasksInCurrentSequence());

    ::base::ScopedClosureRunner done_caller(
            ::base::BindOnce(&::base::WaitableEvent::Signal, ::base::Unretained(done)));
    *status = C2_CORRUPTED;

    const auto codec = mIntfImpl->getVideoCodec();
    if (!codec) {
        ALOGE("Failed to get video codec.");
        return;
    }
    const size_t inputBufferSize = mIntfImpl->getInputBufferSize();
    const size_t minNumOutputBuffers = getMinNumOutputBuffers(*codec);

    // ::base::Unretained(this) is safe here because |mDecoder| is always destroyed before
    // |mDecoderThread| is stopped, so |*this| is always valid during |mDecoder|'s lifetime.
    auto getPool = ::base::BindRepeating(&VaapiDecodeComponent::getVideoFramePool,
                                         ::base::Unretained(this));
    auto onOutput = ::base::BindRepeating(&VaapiDecodeComponent::onOutputFrameReady,
                                          ::base::Unretained(this));
    auto onErr = ::base::BindRepeating(&VaapiDecodeComponent::reportError,
                                       ::base::Unretained(this), C2_CORRUPTED);

    // Host-side decode when a helper is configured AND answering (bd remora-e5x.3). Every failure
    // inside Create() — property unset, nothing listening, codec refused, secure playback — returns
    // nullptr and falls through to the local VA decoder, because a missing helper must cost the
    // NVDEC optimisation and not the ability to play video at all.
    if (RemoteVideoDecoder::configured()) {
        mDecoder = RemoteVideoDecoder::Create(mDebugStreamId, *codec, inputBufferSize,
                                              minNumOutputBuffers, getPool, onOutput, onErr,
                                              mDecoderTaskRunner, mIsSecure);
        if (mDecoder) ALOGI("Decoding %s on the host", VideoCodecToString(*codec));
    }
    if (!mDecoder) {
        mDecoder = VaapiVideoDecoder::Create(mDebugStreamId, *codec, inputBufferSize,
                                             minNumOutputBuffers, std::move(getPool),
                                             std::move(onOutput), std::move(onErr),
                                             mDecoderTaskRunner, mIsSecure);
    }
    if (!mDecoder) {
        ALOGE("Failed to create a decoder for %s", VideoCodecToString(*codec));
        return;
    }

    // Get default color aspects on start.
    if (!mIsSecure && *codec == VideoCodec::H264) {
        if (mIntfImpl->queryColorAspects(&mCurrentColorAspects) != C2_OK) return;
        mPendingColorAspectsChange = false;
    }

    *status = C2_OK;
}

std::unique_ptr<VideoFramePool> VaapiDecodeComponent::getVideoFramePool(const ui::Size& size,
                                                                        HalPixelFormat pixelFormat,
                                                                        size_t numBuffers) {
    ALOG_ASSERT(mDecoderTaskRunner->RunsTasksInCurrentSequence());

    auto sharedThis = weak_from_this().lock();
    if (sharedThis == nullptr) {
        ALOGE("%s(): DecodeComponent instance is destroyed.", __func__);
        return nullptr;
    }

    // (b/157113946): Prevent malicious dynamic resolution change exhausts system memory.
    constexpr int kMaximumSupportedArea = 4096 * 4096;
    if (getArea(size).value_or(INT_MAX) > kMaximumSupportedArea) {
        ALOGE("The output size (%dx%d) is larger than supported size (4096x4096)", size.width,
              size.height);
        reportError(C2_BAD_VALUE);
        return nullptr;
    }

    auto poolId = mIntfImpl->getBlockPoolId();
    std::shared_ptr<C2BlockPool> blockPool;
    auto status = GetCodec2BlockPool(poolId, std::move(sharedThis), &blockPool);
    if (status != C2_OK) {
        ALOGE("Graphic block allocator is invalid: %d", status);
        reportError(status);
        return nullptr;
    }

    // Surface-bound sessions (anything but the byte-buffer GRALLOC pool) decode
    // into RGB, not NV12. The frames go straight to SurfaceFlinger, and on the
    // bare/NVIDIA stack its render engine (ANGLE on Venus) cannot re-import an
    // NV12 dma-buf: the host refuses the allocation, the venus ring dies, and
    // SF aborts on the first video frame (remora-e5x.21). RGB buffers take the
    // same composite path as every app window. The decoder reads the actual
    // format back off the gralloc handle and converts in its existing VPP blit,
    // so the csc is free; byte-buffer clients keep NV12 for CPU reads.
    if (!mIsSecure && blockPool->getAllocatorId() != C2PlatformAllocatorStore::GRALLOC) {
        pixelFormat = static_cast<HalPixelFormat>(HPixelFormat::RGBX_8888);
        ALOGI("Surface-bound output pool (allocator %u): decoding into RGBX_8888.",
              blockPool->getAllocatorId());
    }

    return VideoFramePool::Create(std::move(blockPool), numBuffers, size, pixelFormat, mIsSecure,
                                  mDecoderTaskRunner);
}

}  // namespace android
