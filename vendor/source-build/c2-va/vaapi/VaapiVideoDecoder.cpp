// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_VIDEO
#define LOG_TAG "VaapiVideoDecoder"

#include <c2_va/vaapi/VaapiVideoDecoder.h>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <C2AllocatorGralloc.h>
#include <base/bind.h>
#include <base/memory/ptr_util.h>
#include <log/log.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <utils/Trace.h>
#include <drm_fourcc.h>
#include <va/va_drmcommon.h>

#include <v4l2_codec2/common/EncodeHelpers.h>
#include <v4l2_codec2/common/H264NalParser.h>
#include <v4l2_codec2/common/NalParser.h>

namespace android {
namespace {

// Number of internal decode-target surfaces beyond the DPB size, to allow the
// current picture plus a couple of in-flight outputs.
constexpr size_t kNumExtraSurfaces = 4;

// Minimum output-pool allocation size. The pool is sized to at least this and
// grows only, so the mid-stream resolution changes typical of adaptive streams
// (which stay within 1080p) reuse the same buffers instead of forcing a pool
// reallocation that stalls on the framework's outstanding buffers. 1920x1088 =
// H.264 up to Level 4.x; larger streams grow the pool once. NV12 at this size
// is ~3 MB/buffer.
constexpr int32_t kOutputPoolHintWidth = 1920;
constexpr int32_t kOutputPoolHintHeight = 1088;

// Slice types per H.264 Table 7-6 (values 5..9 are the same modulo 5).
constexpr uint32_t kSliceTypeP = 0;
constexpr uint32_t kSliceTypeB = 1;
constexpr uint32_t kSliceTypeI = 2;
constexpr uint32_t kSliceTypeSP = 3;
constexpr uint32_t kSliceTypeSI = 4;

uint32_t normalizedSliceType(uint32_t sliceType) { return sliceType % 5; }

bool isPSlice(uint32_t t) { return normalizedSliceType(t) == kSliceTypeP; }
bool isBSlice(uint32_t t) { return normalizedSliceType(t) == kSliceTypeB; }
bool isISlice(uint32_t t) { return normalizedSliceType(t) == kSliceTypeI; }
bool isSPSlice(uint32_t t) { return normalizedSliceType(t) == kSliceTypeSP; }
bool isSISlice(uint32_t t) { return normalizedSliceType(t) == kSliceTypeSI; }

// Default 4x4 / 8x8 scaling lists (H.264 Table 7-3, 7-4). Only the "flat"
// (all 16) fallback is required for correctness of streams that don't send
// scaling matrices; the standard default lists are used when a stream signals
// scaling but uses fall-back rule set A/B. For simplicity we start from flat
// and only override with explicitly-signalled lists.
void setFlatScalingList(uint8_t list4x4[6][16], uint8_t list8x8[6][64]) {
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 16; ++j) list4x4[i][j] = 16;
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 64; ++j) list8x8[i][j] = 16;
}

// Parse an explicitly-coded scaling list of |size| entries (16 or 64) using
// exp-golomb deltas (H.264 7.3.2.1.1.1). |useDefault| is set if the stream
// requests the default list. Values are written in the order they are coded
// (which for VAAPI is the raster/zig-zag-agnostic order the driver expects).
bool parseScalingList(ABitReader* br, uint8_t* list, int size, bool* useDefault) {
    *useDefault = false;
    int lastScale = 8;
    int nextScale = 8;
    for (int j = 0; j < size; ++j) {
        if (nextScale != 0) {
            int32_t deltaScale = 0;
            if (!NalParser::parseSE(br, &deltaScale)) return false;
            nextScale = (lastScale + deltaScale + 256) % 256;
            if (j == 0 && nextScale == 0) {
                *useDefault = true;
            }
        }
        list[j] = (nextScale == 0) ? lastScale : nextScale;
        lastScale = list[j];
    }
    return true;
}

// Strip emulation_prevention_three_byte (7.4.1): every 0x03 that follows two
// zero bytes in the raw NAL payload is removed, yielding the RBSP. Parsing
// against the RBSP makes bit counts exact — NALBitReader's numBitsLeft() is in
// raw-byte space and drops EP bytes at reservoir-fill (read-ahead) time, so
// deltas computed from it drift by 8 bits per EP byte.
std::vector<uint8_t> unescapeRbsp(const uint8_t* data, size_t size) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    size_t zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(b);
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return rbsp;
}

// Mirror of external/minigbm/cros_gralloc/cros_gralloc_handle.h. The layout is
// part of minigbm's gralloc handle ABI (the handle is always allocated at the
// full struct size: num_ints = sizeof(struct) - header - num_fds); validated at
// runtime via |magic| before any field is trusted. Reading the layout and DRM
// format modifier from the handle needs no CPU mapping, so it works for
// buffers without CPU usage bits (where lockYCbCr fails).
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;
constexpr int kCrosMaxPlanes = 4;
constexpr int kCrosMaxFds = kCrosMaxPlanes + 1;
struct CrosGrallocHandle : public native_handle_t {
    int32_t fds[kCrosMaxFds];
    uint32_t strides[kCrosMaxPlanes];
    uint32_t offsets[kCrosMaxPlanes];
    uint32_t sizes[kCrosMaxPlanes];
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format;  // DRM fourcc
    uint32_t tiling;
    uint64_t formatModifier;
    uint64_t useFlags;
    uint32_t magic;
    uint32_t pixelStride;
    int32_t droidFormat;
    int64_t usage;
    uint32_t numPlanes;
    uint64_t reservedRegionSize;
    uint64_t totalSize;
} __attribute__((packed));

// more_rbsp_data() (7.2): data remains iff the current position is before the
// rbsp_stop_one_bit, i.e. the last set bit of the RBSP.
bool moreRbspData(const ABitReader& br, const uint8_t* rbsp, size_t rbspSize) {
    size_t last = rbspSize;
    while (last > 0 && rbsp[last - 1] == 0) --last;
    if (last == 0) return false;  // no stop bit at all: malformed, nothing more
    const int trailingZeroBits = __builtin_ctz(rbsp[last - 1]);
    const size_t stopBitPos = (last - 1) * 8 + (7 - trailingZeroBits);
    const size_t consumed = rbspSize * 8 - br.numBitsLeft();
    return consumed < stopBitPos;
}

}  // namespace

// static
std::unique_ptr<VideoDecoder> VaapiVideoDecoder::Create(
        uint32_t debugStreamId, const VideoCodec& codec, const size_t inputBufferSize,
        const size_t minNumOutputBuffers, GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner, bool isSecure) {
    std::unique_ptr<VaapiVideoDecoder> decoder = ::base::WrapUnique(
            new VaapiVideoDecoder(debugStreamId, codec, std::move(taskRunner)));
    if (!decoder->start(codec, inputBufferSize, minNumOutputBuffers, std::move(getPoolCb),
                        std::move(outputCb), std::move(errorCb), isSecure)) {
        return nullptr;
    }
    return decoder;
}

VaapiVideoDecoder::VaapiVideoDecoder(uint32_t debugStreamId, VideoCodec codec,
                                     scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mDebugStreamId(debugStreamId), mCodec(codec), mTaskRunner(std::move(taskRunner)) {
    ALOGV("%s()", __func__);
    mWeakThis = mWeakThisFactory.GetWeakPtr();
}

VaapiVideoDecoder::~VaapiVideoDecoder() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mWeakThisFactory.InvalidateWeakPtrs();

    // Drop DPB before destroying surfaces.
    mDpb.clear();
    vp9ClearRefSlots();
    av1ClearRefSlots();
    while (!mPendingOutputs.empty()) mPendingOutputs.pop();
    destroyVAContext();
    destroySurfacePool();
    mVideoFramePool.reset();
    mDevice.reset();
}

bool VaapiVideoDecoder::start(const VideoCodec& codec, size_t inputBufferSize,
                              size_t minNumOutputBuffers, GetPoolCB getPoolCb, OutputCB outputCb,
                              ErrorCB errorCb, bool isSecure) {
    ATRACE_CALL();
    ALOGV("%s(codec=%s, minNumOutputBuffers=%zu, isSecure=%d)", __func__, VideoCodecToString(codec),
          minNumOutputBuffers, isSecure);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mMinNumOutputBuffers = minNumOutputBuffers;
    mGetPoolCb = std::move(getPoolCb);
    mOutputCb = std::move(outputCb);
    mErrorCb = std::move(errorCb);
    mCodec = codec;
    mIsSecure = isSecure;

    // Secure decode requires protected surfaces + PAVP; not supported yet.
    if (isSecure) {
        ALOGW("Secure decode not supported by VaapiVideoDecoder.");
        return false;
    }

    // Map codec -> VA profile. Only H.264 has a decode core in this pass.
    switch (codec) {
    case VideoCodec::H264:
        mVAProfile = VAProfileH264High;  // High is a superset of CBP/Main for VLD.
        break;
    case VideoCodec::HEVC:
        mVAProfile = VAProfileHEVCMain;
        break;
    case VideoCodec::VP9:
        mVAProfile = VAProfileVP9Profile0;
        break;
    case VideoCodec::AV1:
        mVAProfile = VAProfileAV1Profile0;
        break;
    default:
        ALOGW("Unsupported codec %s", VideoCodecToString(codec));
        return false;
    }

    mDevice = VaapiDevice::Create();
    if (!mDevice) {
        ALOGW("No VAAPI device; self-disabling for %s.", VideoCodecToString(codec));
        return false;
    }

    if (!mDevice->supportsProfileVLD(mVAProfile)) {
        ALOGW("Driver does not support VLD for profile %d (codec %s).",
              static_cast<int>(mVAProfile), VideoCodecToString(codec));
        return false;
    }

    setState(State::Idle);
    return true;
}

// ---------------------------------------------------------------------------
// VideoDecoder interface
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::decode(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB decodeCb) {
    ATRACE_CALL();
    ALOGV("%s(id=%d)", __func__, buffer ? buffer->id : -1);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Error) {
        mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(decodeCb),
                                                          VideoDecoder::DecodeStatus::kError));
        return;
    }
    if (mState == State::Idle) {
        setState(State::Decoding);
    }

    mDecodeRequests.push(DecodeRequest(std::move(buffer), std::move(decodeCb)));
    pumpDecodeRequests();
}

void VaapiVideoDecoder::drain(DecodeCB drainCb) {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    switch (mState) {
    case State::Idle:
        mTaskRunner->PostTask(
                FROM_HERE, ::base::BindOnce(std::move(drainCb), VideoDecoder::DecodeStatus::kOk));
        return;
    case State::Decoding:
        mDecodeRequests.push(DecodeRequest(nullptr, std::move(drainCb)));
        pumpDecodeRequests();
        return;
    case State::Draining:
    case State::Error:
        mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(drainCb),
                                                          VideoDecoder::DecodeStatus::kError));
        return;
    }
}

void VaapiVideoDecoder::flush() {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Idle || mState == State::Error) {
        return;
    }

    // Abort all pending decode callbacks.
    while (!mDecodeRequests.empty()) {
        auto request = std::move(mDecodeRequests.front());
        mDecodeRequests.pop();
        if (request.decodeCb) {
            std::move(request.decodeCb).Run(VideoDecoder::DecodeStatus::kAborted);
        }
    }
    if (mDrainCb) {
        std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kAborted);
    }

    // Drop all decoder state: DPB, pending outputs, POC history. All internal
    // surfaces become free again (nothing references them). The VA context and
    // surface pool are kept (same resolution).
    mDpb.clear();
    vp9ClearRefSlots();
    av1ClearRefSlots();
    while (!mPendingOutputs.empty()) mPendingOutputs.pop();
    mOutputFetchInFlight = false;
    ++mFetchGeneration;  // a late callback from a pre-flush fetch must be ignored
    mPrevPicOrderCntMsb = 0;
    mPrevPicOrderCntLsb = 0;
    mPrevFrameNum = 0;
    mPrevFrameNumOffset = 0;
    mHasPrevRefFrame = false;
    mMaxLongTermFrameIdx = -1;

    setState(State::Idle);
}

// ---------------------------------------------------------------------------
// Decode pump
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::pumpDecodeRequests() {
    ATRACE_CALL();
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState != State::Decoding) return;

    while (!mDecodeRequests.empty()) {
        DecodeRequest& front = mDecodeRequests.front();

        // Drain marker.
        if (front.buffer == nullptr) {
            DecodeCB cb = std::move(front.decodeCb);
            mDecodeRequests.pop();

            // Flush the DPB: emit all buffered pictures in POC order.
            flushDpb();
            mDrainCb = std::move(cb);
            setState(State::Draining);

            // If nothing is pending, complete the drain immediately.
            if (mPendingOutputs.empty() && !mOutputFetchInFlight) {
                std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kOk);
                setState(State::Idle);
            }
            return;
        }

        // Map the input dma-buf and decode it. The request is only popped once
        // it actually decodes: on kNoSurface it stays at the front and is
        // retried when a pending output completes (back-pressure).
        C2ReadView view = front.buffer->dmabuf.map().get();
        if (view.error() != C2_OK) {
            ALOGE("Failed to map input dma-buf: %d", view.error());
            DecodeCB cb = std::move(front.decodeCb);
            mDecodeRequests.pop();
            std::move(cb).Run(VideoDecoder::DecodeStatus::kError);
            onError();
            return;
        }
        // C2ReadView::data() already points at the block's logical start (map() applies the block
        // offset); do NOT add buffer->offset again — that double-counts. (V4L2Decoder parses from
        // view.data() too, passing offset only to the HW dma-buf path.)
        const uint8_t* data = view.data();
        const size_t size = std::min(front.buffer->size, static_cast<size_t>(view.capacity()));
        const int32_t bitstreamId = front.buffer->id;

        FrameResult result = FrameResult::kError;
        switch (mCodec) {
        case VideoCodec::H264:
            result = decodeH264Buffer(bitstreamId, data, size);
            break;
        case VideoCodec::HEVC:
            result = decodeHEVCBuffer(bitstreamId, data, size);
            break;
        case VideoCodec::VP9:
            result = decodeVP9Buffer(bitstreamId, data, size);
            break;
        case VideoCodec::AV1:
            result = decodeAV1Buffer(bitstreamId, data, size);
            break;
        default:
            ALOGE("Decode core for %s not implemented yet.", VideoCodecToString(mCodec));
            result = FrameResult::kError;
            break;
        }

        if (result == FrameResult::kNoSurface) {
            ALOGV("All decode surfaces busy; waiting for an output to complete.");
            return;
        }

        std::unique_ptr<ConstBitstreamBuffer> buffer = std::move(front.buffer);
        DecodeCB cb = std::move(front.decodeCb);
        mDecodeRequests.pop();

        if (result == FrameResult::kError) {
            std::move(cb).Run(VideoDecoder::DecodeStatus::kError);
            onError();
            return;
        }

        if (result == FrameResult::kSkipped) {
            // kAborted completes the work now, with no output (the component's
            // drop path) — see the FrameResult comment for why kOk would stall.
            std::move(cb).Run(VideoDecoder::DecodeStatus::kAborted);
            continue;
        }

        std::move(cb).Run(VideoDecoder::DecodeStatus::kOk);
    }
}

// ---------------------------------------------------------------------------
// H.264 access-unit decode
// ---------------------------------------------------------------------------

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeH264Buffer(int32_t bitstreamId,
                                                                   const uint8_t* data,
                                                                   size_t size) {
    ATRACE_CALL();

    // Walk the NAL units. Parameter sets update our stores; slices of a single
    // access unit are gathered and decoded together. The C2 framework delivers
    // one access unit (one frame) per buffer, so all slice NALs here belong to
    // the same picture. CSD (SPS/PPS-only) buffers decode to zero slices.
    H264NalParser parser(data, size);

    std::vector<H264SliceHeader> slices;
    const H264SPS* activeSps = nullptr;
    const H264PPS* activePps = nullptr;

    while (parser.locateNextNal()) {
        const uint8_t* nal = parser.data();
        const size_t nalSize = parser.length();
        if (nalSize == 0) continue;
        const uint8_t nalType = parser.type();

        switch (nalType) {
        case H264NalParser::kSPSType: {
            H264SPS sps;
            if (!parseSPS(nal, nalSize, &sps)) {
                ALOGE("Failed to parse SPS.");
                return FrameResult::kError;
            }
            mSPSes[sps.seq_parameter_set_id] = sps;
            break;
        }
        case H264NalParser::kPPSType: {
            H264PPS pps;
            if (!parsePPS(nal, nalSize, &pps)) {
                ALOGE("Failed to parse PPS.");
                return FrameResult::kError;
            }
            mPPSes[pps.pic_parameter_set_id] = pps;
            break;
        }
        case 1:   // non-IDR slice
        case 5: { // IDR slice
            H264SliceHeader sh;
            if (!parseSliceHeader(nal, nalSize, &sh)) {
                ALOGE("Failed to parse slice header.");
                return FrameResult::kError;
            }
            auto ppsIt = mPPSes.find(sh.pic_parameter_set_id);
            if (ppsIt == mPPSes.end()) {
                ALOGE("Slice refers to unknown PPS %u.", sh.pic_parameter_set_id);
                return FrameResult::kError;
            }
            auto spsIt = mSPSes.find(ppsIt->second.seq_parameter_set_id);
            if (spsIt == mSPSes.end()) {
                ALOGE("PPS refers to unknown SPS %u.", ppsIt->second.seq_parameter_set_id);
                return FrameResult::kError;
            }
            activePps = &ppsIt->second;
            activeSps = &spsIt->second;
            slices.push_back(sh);
            break;
        }
        default:
            // Ignore AUD/SEI/filler/etc.
            break;
        }
    }

    if (slices.empty()) {
        // CSD-only buffer (config data). Nothing to decode.
        return FrameResult::kOk;
    }

    return decodeFrameH264(bitstreamId, *activeSps, *activePps, slices);
}

// ---------------------------------------------------------------------------
// H.264 SPS / PPS / slice-header parsing
//
// Each NAL payload is unescaped into its RBSP first and parsed with a plain
// ABitReader, so consumed-bit counts are exact. VAAPI's slice_data_bit_offset
// is defined post-EP-removal (including the NAL header byte), which is exactly
// 8 + bits consumed from the RBSP. Flag reads use getBitsWithFallback (raw
// getBits() aborts the process on over-read); truncation is caught by the
// final overRead() check.
// ---------------------------------------------------------------------------

bool VaapiVideoDecoder::parseSPS(const uint8_t* nal, size_t size, H264SPS* sps) {
    *sps = H264SPS();
    // Skip the 1-byte NAL header, unescape the rest.
    const std::vector<uint8_t> rbsp = unescapeRbsp(nal + 1, size - 1);
    ABitReader br(rbsp.data(), rbsp.size());

    sps->profile_idc = static_cast<uint8_t>(br.getBitsWithFallback(8, 0));
    br.getBitsWithFallback(8, 0);  // constraint_set flags + reserved_zero_2bits
    br.getBitsWithFallback(8, 0);  // level_idc
    if (!NalParser::parseUE(&br, &sps->seq_parameter_set_id)) return false;

    sps->chroma_format_idc = 1;
    if (sps->profile_idc == 100 || sps->profile_idc == 110 || sps->profile_idc == 122 ||
        sps->profile_idc == 244 || sps->profile_idc == 44 || sps->profile_idc == 83 ||
        sps->profile_idc == 86 || sps->profile_idc == 118 || sps->profile_idc == 128 ||
        sps->profile_idc == 138 || sps->profile_idc == 139 || sps->profile_idc == 134 ||
        sps->profile_idc == 135) {
        if (!NalParser::parseUE(&br, &sps->chroma_format_idc)) return false;
        if (sps->chroma_format_idc == 3) {
            sps->separate_colour_plane_flag = br.getBitsWithFallback(1, 0);
        }
        if (!NalParser::parseUE(&br, &sps->bit_depth_luma_minus8)) return false;
        if (!NalParser::parseUE(&br, &sps->bit_depth_chroma_minus8)) return false;
        br.getBitsWithFallback(1, 0);  // qpprime_y_zero_transform_bypass_flag
        sps->seq_scaling_matrix_present_flag = br.getBitsWithFallback(1, 0);
        if (sps->seq_scaling_matrix_present_flag) {
            setFlatScalingList(sps->scaling_list4x4, sps->scaling_list8x8);
            const int numLists = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < numLists; ++i) {
                uint32_t present = br.getBitsWithFallback(1, 0);
                if (!present) continue;
                bool useDefault = false;
                if (i < 6) {
                    if (!parseScalingList(&br, sps->scaling_list4x4[i], 16, &useDefault))
                        return false;
                } else {
                    if (!parseScalingList(&br, sps->scaling_list8x8[i - 6], 64, &useDefault))
                        return false;
                }
            }
        }
    }

    if (!NalParser::parseUE(&br, &sps->log2_max_frame_num_minus4)) return false;
    if (sps->log2_max_frame_num_minus4 > 12) return false;  // 7.4.2.1.1
    if (!NalParser::parseUE(&br, &sps->pic_order_cnt_type)) return false;
    if (sps->pic_order_cnt_type == 0) {
        if (!NalParser::parseUE(&br, &sps->log2_max_pic_order_cnt_lsb_minus4)) return false;
        if (sps->log2_max_pic_order_cnt_lsb_minus4 > 12) return false;  // 7.4.2.1.1
    } else if (sps->pic_order_cnt_type == 1) {
        sps->delta_pic_order_always_zero_flag = br.getBitsWithFallback(1, 0);
        if (!NalParser::parseSE(&br, &sps->offset_for_non_ref_pic)) return false;
        if (!NalParser::parseSE(&br, &sps->offset_for_top_to_bottom_field)) return false;
        if (!NalParser::parseUE(&br, &sps->num_ref_frames_in_pic_order_cnt_cycle)) return false;
        uint32_t n = std::min<uint32_t>(sps->num_ref_frames_in_pic_order_cnt_cycle, 255);
        for (uint32_t i = 0; i < n; ++i) {
            if (!NalParser::parseSE(&br, &sps->offset_for_ref_frame[i])) return false;
        }
    }

    if (!NalParser::parseUE(&br, &sps->max_num_ref_frames)) return false;
    sps->gaps_in_frame_num_value_allowed_flag = br.getBitsWithFallback(1, 0);
    if (!NalParser::parseUE(&br, &sps->pic_width_in_mbs_minus1)) return false;
    if (!NalParser::parseUE(&br, &sps->pic_height_in_map_units_minus1)) return false;
    sps->frame_mbs_only_flag = br.getBitsWithFallback(1, 1);
    if (!sps->frame_mbs_only_flag) {
        sps->mb_adaptive_frame_field_flag = br.getBitsWithFallback(1, 0);
    }
    sps->direct_8x8_inference_flag = br.getBitsWithFallback(1, 0);
    sps->frame_cropping_flag = br.getBitsWithFallback(1, 0);
    if (sps->frame_cropping_flag) {
        if (!NalParser::parseUE(&br, &sps->frame_crop_left_offset)) return false;
        if (!NalParser::parseUE(&br, &sps->frame_crop_right_offset)) return false;
        if (!NalParser::parseUE(&br, &sps->frame_crop_top_offset)) return false;
        if (!NalParser::parseUE(&br, &sps->frame_crop_bottom_offset)) return false;
    }
    // vui_parameters not needed for VAAPI decode params.

    if (br.overRead()) {
        ALOGE("SPS over-read.");
        return false;
    }
    sps->valid = true;
    return true;
}

bool VaapiVideoDecoder::parsePPS(const uint8_t* nal, size_t size, H264PPS* pps) {
    *pps = H264PPS();
    const std::vector<uint8_t> rbsp = unescapeRbsp(nal + 1, size - 1);
    ABitReader br(rbsp.data(), rbsp.size());

    if (!NalParser::parseUE(&br, &pps->pic_parameter_set_id)) return false;
    if (!NalParser::parseUE(&br, &pps->seq_parameter_set_id)) return false;
    pps->entropy_coding_mode_flag = br.getBitsWithFallback(1, 0);
    pps->bottom_field_pic_order_in_frame_present_flag = br.getBitsWithFallback(1, 0);

    uint32_t numSliceGroupsMinus1 = 0;
    if (!NalParser::parseUE(&br, &numSliceGroupsMinus1)) return false;
    if (numSliceGroupsMinus1 > 0) {
        // FMO (slice groups) is not supported by iHD/VAAPI decode; parse-skip
        // the slice_group_map so downstream fields stay aligned, then bail.
        uint32_t mapType = 0;
        if (!NalParser::parseUE(&br, &mapType)) return false;
        ALOGW("PPS uses FMO (num_slice_groups=%u); unsupported.", numSliceGroupsMinus1 + 1);
        return false;
    }

    if (!NalParser::parseUE(&br, &pps->num_ref_idx_l0_default_active_minus1)) return false;
    if (!NalParser::parseUE(&br, &pps->num_ref_idx_l1_default_active_minus1)) return false;
    pps->weighted_pred_flag = br.getBitsWithFallback(1, 0);
    pps->weighted_bipred_idc = br.getBitsWithFallback(2, 0);
    if (!NalParser::parseSE(&br, &pps->pic_init_qp_minus26)) return false;
    if (!NalParser::parseSE(&br, &pps->pic_init_qs_minus26)) return false;
    if (!NalParser::parseSE(&br, &pps->chroma_qp_index_offset)) return false;
    pps->deblocking_filter_control_present_flag = br.getBitsWithFallback(1, 0);
    pps->constrained_intra_pred_flag = br.getBitsWithFallback(1, 0);
    pps->redundant_pic_cnt_present_flag = br.getBitsWithFallback(1, 0);

    // Defaults for the optional trailing part.
    pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;

    // The transform_8x8 / scaling-matrix / 2nd-chroma fields are present only
    // if more_rbsp_data(), i.e. the current position is before the
    // rbsp_stop_one_bit.
    if (moreRbspData(br, rbsp.data(), rbsp.size())) {
        pps->transform_8x8_mode_flag = br.getBitsWithFallback(1, 0);
        pps->pic_scaling_matrix_present_flag = br.getBitsWithFallback(1, 0);
        if (pps->pic_scaling_matrix_present_flag) {
            setFlatScalingList(pps->scaling_list4x4, pps->scaling_list8x8);
            // The number of 8x8 lists depends on transform_8x8 and the SPS
            // chroma format (7.3.2.2): 6 for 4:4:4, else 2. The referenced SPS
            // has normally been parsed by now; fall back to 4:2:0 if not.
            uint32_t chromaFormatIdc = 1;
            auto spsIt = mSPSes.find(pps->seq_parameter_set_id);
            if (spsIt != mSPSes.end()) {
                chromaFormatIdc = spsIt->second.chroma_format_idc;
            } else {
                ALOGW("PPS %u references unparsed SPS %u; assuming 4:2:0 for scaling lists.",
                      pps->pic_parameter_set_id, pps->seq_parameter_set_id);
            }
            const int num8x8 =
                    pps->transform_8x8_mode_flag ? ((chromaFormatIdc != 3) ? 2 : 6) : 0;
            const int numLists = 6 + num8x8;
            for (int i = 0; i < numLists; ++i) {
                uint32_t present = br.getBitsWithFallback(1, 0);
                if (!present) continue;
                bool useDefault = false;
                if (i < 6) {
                    if (!parseScalingList(&br, pps->scaling_list4x4[i], 16, &useDefault))
                        return false;
                } else {
                    if (!parseScalingList(&br, pps->scaling_list8x8[i - 6], 64, &useDefault))
                        return false;
                }
            }
        }
        if (!NalParser::parseSE(&br, &pps->second_chroma_qp_index_offset)) return false;
    }

    if (br.overRead()) {
        ALOGE("PPS over-read.");
        return false;
    }
    pps->valid = true;
    return true;
}

bool VaapiVideoDecoder::parseSliceHeader(const uint8_t* nal, size_t size, H264SliceHeader* sh) {
    *sh = H264SliceHeader();
    sh->nalData = nal;
    sh->nalSize = size;

    const uint8_t headerByte = nal[0];
    sh->nalRefIdc = (headerByte >> 5) & 0x3;
    sh->nalUnitType = headerByte & 0x1f;
    sh->idrPicFlag = (sh->nalUnitType == H264NalParser::kIDRType);

    const std::vector<uint8_t> rbsp = unescapeRbsp(nal + 1, size - 1);
    ABitReader br(rbsp.data(), rbsp.size());
    const size_t rbspTotalBits = rbsp.size() * 8;

    if (!NalParser::parseUE(&br, &sh->first_mb_in_slice)) return false;
    if (!NalParser::parseUE(&br, &sh->slice_type)) return false;
    if (!NalParser::parseUE(&br, &sh->pic_parameter_set_id)) return false;

    auto ppsIt = mPPSes.find(sh->pic_parameter_set_id);
    if (ppsIt == mPPSes.end()) {
        ALOGE("Slice references unknown PPS %u.", sh->pic_parameter_set_id);
        return false;
    }
    const H264PPS& pps = ppsIt->second;
    auto spsIt = mSPSes.find(pps.seq_parameter_set_id);
    if (spsIt == mSPSes.end()) {
        ALOGE("PPS references unknown SPS %u.", pps.seq_parameter_set_id);
        return false;
    }
    const H264SPS& sps = spsIt->second;

    if (sps.separate_colour_plane_flag) {
        br.skipBits(2);  // colour_plane_id
    }
    sh->frame_num = br.getBitsWithFallback(sps.log2_max_frame_num_minus4 + 4, 0);

    if (!sps.frame_mbs_only_flag) {
        sh->field_pic_flag = br.getBitsWithFallback(1, 0);
        if (sh->field_pic_flag) {
            sh->bottom_field_flag = br.getBitsWithFallback(1, 0);
        }
    }

    if (sh->idrPicFlag) {
        if (!NalParser::parseUE(&br, &sh->idr_pic_id)) return false;
    }

    if (sps.pic_order_cnt_type == 0) {
        sh->pic_order_cnt_lsb = br.getBitsWithFallback(sps.log2_max_pic_order_cnt_lsb_minus4 + 4, 0);
        if (pps.bottom_field_pic_order_in_frame_present_flag && !sh->field_pic_flag) {
            if (!NalParser::parseSE(&br, &sh->delta_pic_order_cnt_bottom)) return false;
        }
    } else if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero_flag) {
        if (!NalParser::parseSE(&br, &sh->delta_pic_order_cnt[0])) return false;
        if (pps.bottom_field_pic_order_in_frame_present_flag && !sh->field_pic_flag) {
            if (!NalParser::parseSE(&br, &sh->delta_pic_order_cnt[1])) return false;
        }
    }

    if (pps.redundant_pic_cnt_present_flag) {
        uint32_t redundantPicCnt = 0;
        if (!NalParser::parseUE(&br, &redundantPicCnt)) return false;
    }

    if (isBSlice(sh->slice_type)) {
        sh->direct_spatial_mv_pred_flag = br.getBitsWithFallback(1, 0);
    }

    if (isPSlice(sh->slice_type) || isSPSlice(sh->slice_type) || isBSlice(sh->slice_type)) {
        sh->num_ref_idx_active_override_flag = br.getBitsWithFallback(1, 0);
        if (sh->num_ref_idx_active_override_flag) {
            if (!NalParser::parseUE(&br, &sh->num_ref_idx_l0_active_minus1)) return false;
            if (isBSlice(sh->slice_type)) {
                if (!NalParser::parseUE(&br, &sh->num_ref_idx_l1_active_minus1)) return false;
            }
        } else {
            sh->num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
            sh->num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
        }
        if (sh->num_ref_idx_l0_active_minus1 > 31 || sh->num_ref_idx_l1_active_minus1 > 31) {
            ALOGE("num_ref_idx_active_minus1 out of range.");
            return false;
        }
    }

    // ref_pic_list_modification (7.3.3.1): stored, applied in buildRefPicList*.
    auto parseRefListMods = [&](int list) -> bool {
        uint32_t modFlag = br.getBitsWithFallback(1, 0);
        if (!modFlag) return true;
        uint32_t op = 0;
        do {
            if (!NalParser::parseUE(&br, &op)) return false;
            if (op == 0 || op == 1 || op == 2) {
                uint32_t v = 0;
                if (!NalParser::parseUE(&br, &v)) return false;
                sh->refListMods[list].push_back({op, v});
                // A list can be modified at most num_ref_idx_active (<=32) times.
                if (sh->refListMods[list].size() > 32) return false;
            } else if (op != 3) {
                return false;
            }
        } while (op != 3);
        return true;
    };
    if (!isISlice(sh->slice_type) && !isSISlice(sh->slice_type)) {
        if (!parseRefListMods(0)) return false;
    }
    if (isBSlice(sh->slice_type)) {
        if (!parseRefListMods(1)) return false;
    }

    // pred_weight_table (7.3.3.2): stored, filled into the VA slice param.
    const bool hasWeights =
            (pps.weighted_pred_flag && (isPSlice(sh->slice_type) || isSPSlice(sh->slice_type))) ||
            (pps.weighted_bipred_idc == 1 && isBSlice(sh->slice_type));
    if (hasWeights) {
        sh->hasWeightTable = true;
        if (!NalParser::parseUE(&br, &sh->luma_log2_weight_denom)) return false;
        if (sh->luma_log2_weight_denom > 7) return false;  // 7.4.3.2
        if (sps.chroma_format_idc != 0) {
            if (!NalParser::parseUE(&br, &sh->chroma_log2_weight_denom)) return false;
            if (sh->chroma_log2_weight_denom > 7) return false;
        }
        auto parseWeightList = [&](int list, uint32_t numRefIdxMinus1) -> bool {
            for (uint32_t i = 0; i <= numRefIdxMinus1; ++i) {
                H264SliceHeader::WeightEntry& w = sh->weights[list][i];
                w.lumaPresent = br.getBitsWithFallback(1, 0);
                if (w.lumaPresent) {
                    if (!NalParser::parseSE(&br, &w.lumaWeight)) return false;
                    if (!NalParser::parseSE(&br, &w.lumaOffset)) return false;
                }
                if (sps.chroma_format_idc != 0) {
                    w.chromaPresent = br.getBitsWithFallback(1, 0);
                    if (w.chromaPresent) {
                        for (int j = 0; j < 2; ++j) {
                            if (!NalParser::parseSE(&br, &w.chromaWeight[j])) return false;
                            if (!NalParser::parseSE(&br, &w.chromaOffset[j])) return false;
                        }
                    }
                }
            }
            return true;
        };
        if (!parseWeightList(0, sh->num_ref_idx_l0_active_minus1)) return false;
        if (isBSlice(sh->slice_type)) {
            if (!parseWeightList(1, sh->num_ref_idx_l1_active_minus1)) return false;
        }
    }

    // dec_ref_pic_marking (7.3.3.3): stored, applied in referenceListMarking.
    if (sh->nalRefIdc != 0) {
        if (sh->idrPicFlag) {
            br.getBitsWithFallback(1, 0);  // no_output_of_prior_pics_flag
            sh->long_term_reference_flag = br.getBitsWithFallback(1, 0);
        } else {
            sh->adaptiveRefPicMarking = br.getBitsWithFallback(1, 0);
            if (sh->adaptiveRefPicMarking) {
                uint32_t mmco = 0;
                do {
                    if (!NalParser::parseUE(&br, &mmco)) return false;
                    if (mmco > 6) return false;
                    H264SliceHeader::MmcoOp op;
                    op.op = mmco;
                    if (mmco == 1 || mmco == 3) {
                        // difference_of_pic_nums_minus1
                        if (!NalParser::parseUE(&br, &op.arg1)) return false;
                    }
                    if (mmco == 2) {
                        if (!NalParser::parseUE(&br, &op.arg1)) return false;  // long_term_pic_num
                    }
                    if (mmco == 3 || mmco == 6) {
                        if (!NalParser::parseUE(&br, &op.arg2)) return false;  // long_term_frame_idx
                    }
                    if (mmco == 4) {
                        // max_long_term_frame_idx_plus1
                        if (!NalParser::parseUE(&br, &op.arg1)) return false;
                    }
                    if (mmco != 0) {
                        sh->mmcoOps.push_back(op);
                        if (sh->mmcoOps.size() > 64) return false;  // corrupt-stream guard
                    }
                } while (mmco != 0);
            }
        }
    }

    if (pps.entropy_coding_mode_flag && !isISlice(sh->slice_type) && !isSISlice(sh->slice_type)) {
        if (!NalParser::parseUE(&br, &sh->cabac_init_idc)) return false;
    }
    if (!NalParser::parseSE(&br, &sh->slice_qp_delta)) return false;

    if (isSPSlice(sh->slice_type) || isSISlice(sh->slice_type)) {
        if (isSPSlice(sh->slice_type)) {
            br.getBitsWithFallback(1, 0);  // sp_for_switch_flag
        }
        int32_t sliceQsDelta = 0;
        if (!NalParser::parseSE(&br, &sliceQsDelta)) return false;
    }

    if (pps.deblocking_filter_control_present_flag) {
        if (!NalParser::parseUE(&br, &sh->disable_deblocking_filter_idc)) return false;
        if (sh->disable_deblocking_filter_idc != 1) {
            if (!NalParser::parseSE(&br, &sh->slice_alpha_c0_offset_div2)) return false;
            if (!NalParser::parseSE(&br, &sh->slice_beta_offset_div2)) return false;
        }
    }

    if (br.overRead()) {
        ALOGE("Slice header over-read.");
        return false;
    }

    // Bits consumed for the slice header, measured after EP removal, counting
    // from (and including) the NAL header byte. That is:
    //   8 (NAL header byte) + (bits consumed from the RBSP so far).
    const size_t rbspConsumed = rbspTotalBits - br.numBitsLeft();
    sh->header_bit_size = static_cast<uint32_t>(8 + rbspConsumed);
    sh->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// VA context lifecycle
// ---------------------------------------------------------------------------

bool VaapiVideoDecoder::ensureVAContext(const H264SPS& sps) {
    // Derive coded size from SPS (in macroblocks). frame_mbs_only_flag scales
    // the height.
    const uint32_t widthMbs = sps.pic_width_in_mbs_minus1 + 1;
    const uint32_t heightMapUnits = sps.pic_height_in_map_units_minus1 + 1;
    const uint32_t heightMbs = (sps.frame_mbs_only_flag ? 1 : 2) * heightMapUnits;
    const int codedW = static_cast<int>(widthMbs * 16);
    const int codedH = static_cast<int>(heightMbs * 16);

    // Compute visible rect from cropping.
    int cropUnitX = 1;
    int cropUnitY = sps.frame_mbs_only_flag ? 1 : 2;
    if (sps.chroma_format_idc == 1) {  // 4:2:0
        cropUnitX = 2;
        cropUnitY *= 2;
    } else if (sps.chroma_format_idc == 2) {  // 4:2:2
        cropUnitX = 2;
    }
    int cropLeft = static_cast<int>(sps.frame_crop_left_offset) * cropUnitX;
    int cropRight = static_cast<int>(sps.frame_crop_right_offset) * cropUnitX;
    int cropTop = static_cast<int>(sps.frame_crop_top_offset) * cropUnitY;
    int cropBottom = static_cast<int>(sps.frame_crop_bottom_offset) * cropUnitY;
    Rect visible(cropLeft, cropTop, codedW - cropRight, codedH - cropBottom);

    const ui::Size codedSize(codedW, codedH);

    // DPB size: max_num_ref_frames + working headroom, capped at 16.
    const size_t dpbMaxSize = std::min<size_t>(16, std::max<uint32_t>(sps.max_num_ref_frames, 1) + 1);
    return ensureVAContextForGeometry(codedSize, visible, dpbMaxSize);
}

bool VaapiVideoDecoder::ensureVAContextForGeometry(const ui::Size& codedSize, const Rect& visible,
                                                   size_t dpbMaxSize) {
    const int codedW = codedSize.width;
    const int codedH = codedSize.height;

    // The picture size is always the actual coded size (drives VA decode params,
    // surface import, and the visible-region copy). The VA context and internal
    // decode surfaces, however, are sized to a max hint and grown ONLY.
    mCodedSize = codedSize;
    mVisibleRect = visible;

    // Reuse the existing context when the new picture fits it. A mid-stream
    // resolution change that stays within the current (hint-sized) context does
    // NO vaDestroyContext/vaCreateContext — on the SR-IOV VF that teardown hangs
    // the video engine (decode thread blocks inside VA, picture freezes while
    // audio continues; the recurring adaptive-stream freeze). VA decodes the
    // smaller picture into the top-left of the larger surfaces; the DPB is
    // cleared by the IDR that accompanies every resolution change, and pending
    // outputs from the previous size stay valid (their surfaces aren't freed).
    const ui::Size wantCtx(std::max({codedW, mContextSize.width, kOutputPoolHintWidth}),
                           std::max({codedH, mContextSize.height, kOutputPoolHintHeight}));
    if (mVAContext != VA_INVALID_ID && wantCtx.width == mContextSize.width &&
        wantCtx.height == mContextSize.height) {
        return true;
    }

    // First init, or a picture larger than the current context: (re)build at
    // wantCtx. Everything queued for output references surfaces about to be
    // freed, so drop the pending outputs (a one-time skip), unlatch the pump,
    // bump the fetch generation to invalidate any in-flight callback, and mark
    // the DPB outputted so flushDpb() won't re-queue against the dying surfaces.
    ALOGI("Creating VA context %dx%d (picture %dx%d) visible=[%d,%d,%d,%d]", wantCtx.width,
          wantCtx.height, codedW, codedH, visible.left, visible.top, visible.right, visible.bottom);

    if (mVAContext != VA_INVALID_ID) {
        if (!mPendingOutputs.empty()) {
            ALOGW("Context grow with %zu outputs still pending; dropping them.",
                  mPendingOutputs.size());
            while (!mPendingOutputs.empty()) mPendingOutputs.pop();
        }
        mOutputFetchInFlight = false;
        ++mFetchGeneration;
        for (const auto& e : mDpb) e->outputted = true;
    }
    flushDpb();
    destroyVAContext();
    destroySurfacePool();

    VADisplay dpy = mDevice->display();

    // Request an NV12 (VA_RT_FORMAT_YUV420) VLD config.
    VAConfigAttrib attrib;
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    VAStatus status = vaGetConfigAttributes(dpy, mVAProfile, VAEntrypointVLD, &attrib, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaGetConfigAttributes failed: %s", vaErrorStr(status));
        return false;
    }
    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
        ALOGE("Driver does not support YUV420 for this profile.");
        return false;
    }
    attrib.value = VA_RT_FORMAT_YUV420;

    status = vaCreateConfig(dpy, mVAProfile, VAEntrypointVLD, &attrib, 1, &mVAConfig);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateConfig failed: %s", vaErrorStr(status));
        mVAConfig = VA_INVALID_ID;
        return false;
    }

    mDpbMaxSize = dpbMaxSize;
    const size_t numSurfaces =
            std::max(mDpbMaxSize + kNumExtraSurfaces, mMinNumOutputBuffers + kNumExtraSurfaces);

    if (!allocateSurfacePool(wantCtx, numSurfaces)) {
        ALOGE("Failed to allocate internal surface pool.");
        return false;
    }

    status = vaCreateContext(dpy, mVAConfig, wantCtx.width, wantCtx.height, VA_PROGRESSIVE,
                             mSurfacePool.data(), static_cast<int>(mSurfacePool.size()),
                             &mVAContext);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateContext failed: %s", vaErrorStr(status));
        mVAContext = VA_INVALID_ID;
        return false;
    }

    mContextSize = wantCtx;

    // Output pool: allocate at a max hint and grow only. Recreating the pool on
    // every resolution change stalls — the framework still holds the old-size
    // buffers, so the new pool blocks waiting for them to return (frozen video
    // on adaptive streams that switch mid-stream). Instead, size the pool to
    // cover typical content up front and reuse it: a smaller frame is written
    // into the top-left of a larger buffer and cropped via mVisibleRect. Only a
    // frame larger than the current pool forces a (rare) reallocation.
    ui::Size wantPool(std::max({codedW, mOutputPoolSize.width, kOutputPoolHintWidth}),
                      std::max({codedH, mOutputPoolSize.height, kOutputPoolHintHeight}));
    if (!mVideoFramePool || wantPool.width != mOutputPoolSize.width ||
        wantPool.height != mOutputPoolSize.height) {
        mVideoFramePool.reset();
        mOutputInfo.reset();
        mOutputPoolSize = wantPool;
        mVideoFramePool = mGetPoolCb.Run(mOutputPoolSize, HalPixelFormat::YCBCR_420_888,
                                         mMinNumOutputBuffers + kNumExtraSurfaces);
        if (!mVideoFramePool) {
            ALOGE("Failed to obtain VideoFramePool at %dx%d.", mOutputPoolSize.width,
                  mOutputPoolSize.height);
            return false;
        }
    }

    return true;
}

void VaapiVideoDecoder::destroyVAContext() {
    if (!mDevice) return;
    VADisplay dpy = mDevice->display();
    if (mVppContext != VA_INVALID_ID) {
        vaDestroyContext(dpy, mVppContext);
        mVppContext = VA_INVALID_ID;
    }
    if (mVppConfig != VA_INVALID_ID) {
        vaDestroyConfig(dpy, mVppConfig);
        mVppConfig = VA_INVALID_ID;
    }
    if (mVAContext != VA_INVALID_ID) {
        vaDestroyContext(dpy, mVAContext);
        mVAContext = VA_INVALID_ID;
    }
    if (mVAConfig != VA_INVALID_ID) {
        vaDestroyConfig(dpy, mVAConfig);
        mVAConfig = VA_INVALID_ID;
    }
    mContextSize = ui::Size();
}

bool VaapiVideoDecoder::allocateSurfacePool(const ui::Size& size, size_t count) {
    VADisplay dpy = mDevice->display();
    mSurfacePool.assign(count, VA_INVALID_SURFACE);

    VASurfaceAttrib attrib;
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = VA_FOURCC_NV12;

    VAStatus status = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, size.width, size.height,
                                       mSurfacePool.data(), count, &attrib, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces(%zu @ %dx%d) failed: %s", count, size.width, size.height,
              vaErrorStr(status));
        mSurfacePool.clear();
        return false;
    }
    return true;
}

void VaapiVideoDecoder::destroySurfacePool() {
    if (mDevice && !mSurfacePool.empty()) {
        vaDestroySurfaces(mDevice->display(), mSurfacePool.data(),
                          static_cast<int>(mSurfacePool.size()));
    }
    mSurfacePool.clear();
}

VASurfaceID VaapiVideoDecoder::acquireFreeSurface() {
    // Reclaim any DPB entries that are done first so their surfaces free up.
    reclaimDpbEntries();

    // A surface is in use iff it is the target of a DPB entry or a queued
    // pending output. Scan the fixed pool for the first surface not in use.
    for (VASurfaceID candidate : mSurfacePool) {
        bool inUse = false;
        for (const auto& e : mDpb) {
            if (e->surface == candidate) {
                inUse = true;
                break;
            }
        }
        if (inUse) continue;
        // Pending outputs are processed in FIFO order; check the queue by copy.
        std::queue<PendingOutput> scan = mPendingOutputs;
        while (!scan.empty()) {
            if (scan.front().surface == candidate) {
                inUse = true;
                break;
            }
            scan.pop();
        }
        if (!inUse) return candidate;
    }
    return VA_INVALID_SURFACE;
}

// ---------------------------------------------------------------------------
// Missing-reference skip
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::noteMissingRefSkip(const char* what) {
    // First skip of a run logs loudly (this is the artifact-class replacement:
    // without it the GPU would predict from uninitialized VRAM); afterwards
    // once per 60 so a long GOP cannot spam logcat.
    if (mMissingRefSkips == 0 || mMissingRefSkips % 60 == 0) {
        ALOGW("%s: no decodable reference — skipping until the next key frame "
              "(%u skipped so far).",
              what, mMissingRefSkips + 1);
    }
    ++mMissingRefSkips;
}

void VaapiVideoDecoder::noteMissingRefRecovery(const char* what) {
    if (mMissingRefSkips == 0) return;
    ALOGW("%s: prediction chain restarts after %u skipped frame(s).", what, mMissingRefSkips);
    mMissingRefSkips = 0;
}

// ---------------------------------------------------------------------------
// Per-frame VA submission
// ---------------------------------------------------------------------------

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeFrameH264(
        int32_t bitstreamId, const H264SPS& sps, const H264PPS& pps,
        const std::vector<H264SliceHeader>& slices) {
    ATRACE_CALL();

    if (!ensureVAContext(sps)) {
        return FrameResult::kError;
    }

    const H264SliceHeader& first = slices.front();

    // P/B slices with a reference-free DPB (stream joined mid-GOP, or resumed
    // after a flush on a non-IDR frame) are skipped rather than decoded from
    // VA_INVALID_SURFACE references. A non-IDR I picture is allowed through:
    // it is self-contained and restarts the chain (open-GOP joins recover
    // faster that way, same as ffmpeg).
    if (!first.idrPicFlag) {
        bool needsRefs = false;
        for (const H264SliceHeader& sh : slices) {
            if (!isISlice(sh.slice_type) && !isSISlice(sh.slice_type)) {
                needsRefs = true;
                break;
            }
        }
        if (needsRefs) {
            bool haveRefs = false;
            for (const auto& e : mDpb) {
                if (e->refUsed || e->longTerm) {
                    haveRefs = true;
                    break;
                }
            }
            if (!haveRefs) {
                noteMissingRefSkip("H.264 P/B frame with an empty DPB");
                return FrameResult::kSkipped;
            }
        }
    }
    noteMissingRefRecovery("H.264");

    // On IDR, clear the DPB (all previous references invalid) after flushing.
    if (first.idrPicFlag) {
        flushDpb();
        mPrevPicOrderCntMsb = 0;
        mPrevPicOrderCntLsb = 0;
        mPrevFrameNum = 0;
        mPrevFrameNumOffset = 0;
        mHasPrevRefFrame = false;
    }

    // Compute the picture order count for the current frame.
    int32_t topPoc = 0;
    int32_t bottomPoc = 0;
    computePoc(sps, first, &topPoc, &bottomPoc);

    // Update picNums for existing references (needed for ref list building).
    updatePicNums(sps, first.frame_num);

    VASurfaceID target = acquireFreeSurface();
    if (target == VA_INVALID_SURFACE) {
        // All surfaces are held by references or pending outputs: back-pressure.
        // Everything up to here (POC/picNum updates) is idempotent, so retrying
        // this access unit later recomputes the same state.
        return FrameResult::kNoSurface;
    }

    VADisplay dpy = mDevice->display();
    VAStatus status = vaBeginPicture(dpy, mVAContext, target);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture failed: %s", vaErrorStr(status));
        return FrameResult::kError;
    }

    // Build the current DpbEntry (not yet inserted; used for VA CurrPic + POC).
    DpbEntry current;
    current.surface = target;
    current.bitstreamId = bitstreamId;
    current.frameNum = first.frame_num;
    current.topFieldOrderCnt = topPoc;
    current.bottomFieldOrderCnt = bottomPoc;
    current.picOrderCnt = std::min(topPoc, bottomPoc);
    current.refUsed = (first.nalRefIdc != 0);
    current.codedSize = mCodedSize;
    current.visibleRect = mVisibleRect;

    // Picture parameter buffer.
    VAPictureParameterBufferH264 picParam;
    if (!fillPictureParam(sps, pps, first, target, &picParam)) {
        return FrameResult::kError;
    }
    // CurrPic uses the computed POC.
    picParam.CurrPic.picture_id = target;
    picParam.CurrPic.frame_idx = first.frame_num;
    picParam.CurrPic.flags = 0;
    picParam.CurrPic.TopFieldOrderCnt = topPoc;
    picParam.CurrPic.BottomFieldOrderCnt = bottomPoc;

    std::vector<VABufferID> vaBuffers;
    auto createBuffer = [&](VABufferType type, unsigned int sz, void* payload) -> bool {
        VABufferID id = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(dpy, mVAContext, type, sz, 1, payload, &id);
        if (s != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(type=%d) failed: %s", type, vaErrorStr(s));
            return false;
        }
        vaBuffers.push_back(id);
        return true;
    };

    if (!createBuffer(VAPictureParameterBufferType, sizeof(picParam), &picParam)) {
        return FrameResult::kError;
    }

    // IQ matrix buffer.
    VAIQMatrixBufferH264 iqMatrix;
    fillIQMatrix(sps, pps, &iqMatrix);
    if (!createBuffer(VAIQMatrixBufferType, sizeof(iqMatrix), &iqMatrix)) {
        return FrameResult::kError;
    }

    // Per-slice: slice parameter buffer + slice data buffer.
    for (const H264SliceHeader& sh : slices) {
        VASliceParameterBufferH264 sliceParam;
        if (!fillSliceParam(sps, pps, sh, current.picOrderCnt, &sliceParam)) {
            return FrameResult::kError;
        }
        if (!createBuffer(VASliceParameterBufferType, sizeof(sliceParam), &sliceParam)) {
            return FrameResult::kError;
        }
        // Slice data buffer: the raw NAL bytes (including the NAL header byte,
        // excluding the start code). slice_data_offset is 0.
        if (!createBuffer(VASliceDataBufferType, sh.nalSize,
                          const_cast<uint8_t*>(sh.nalData))) {
            return FrameResult::kError;
        }
    }

    status = vaRenderPicture(dpy, mVAContext, vaBuffers.data(),
                             static_cast<int>(vaBuffers.size()));
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaRenderPicture failed: %s", vaErrorStr(status));
        for (VABufferID b : vaBuffers) vaDestroyBuffer(dpy, b);
        return FrameResult::kError;
    }

    status = vaEndPicture(dpy, mVAContext);
    // Buffers are consumed by EndPicture; destroy our references regardless.
    for (VABufferID b : vaBuffers) vaDestroyBuffer(dpy, b);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaEndPicture failed: %s", vaErrorStr(status));
        return FrameResult::kError;
    }

    // Ensure decode has completed before we copy the surface out later.
    status = vaSyncSurface(dpy, target);
    if (status != VA_STATUS_SUCCESS) {
        ALOGW("vaSyncSurface failed: %s", vaErrorStr(status));
        // Not fatal for submission; the copy path will sync again.
    }

    // Insert the decoded picture into the DPB and run reference marking.
    auto entry = std::make_unique<DpbEntry>(current);
    DpbEntry* entryPtr = entry.get();
    mDpb.push_back(std::move(entry));
    referenceListMarking(sps, first, entryPtr);

    // Emit pictures whose output time has arrived (POC-ordered bumping).
    bumpAndOutputAsNeeded(sps);
    reclaimDpbEntries();

    return FrameResult::kOk;
}

bool VaapiVideoDecoder::fillPictureParam(const H264SPS& sps, const H264PPS& pps,
                                         const H264SliceHeader& first, VASurfaceID target,
                                         VAPictureParameterBufferH264* pic) {
    (void)target;
    memset(pic, 0, sizeof(*pic));

    const uint32_t widthMbs = sps.pic_width_in_mbs_minus1 + 1;
    const uint32_t heightMapUnits = sps.pic_height_in_map_units_minus1 + 1;
    const uint32_t heightMbs = (sps.frame_mbs_only_flag ? 1 : 2) * heightMapUnits;

    pic->picture_width_in_mbs_minus1 = static_cast<uint16_t>(widthMbs - 1);
    pic->picture_height_in_mbs_minus1 = static_cast<uint16_t>(heightMbs - 1);
    pic->bit_depth_luma_minus8 = static_cast<uint8_t>(sps.bit_depth_luma_minus8);
    pic->bit_depth_chroma_minus8 = static_cast<uint8_t>(sps.bit_depth_chroma_minus8);
    pic->num_ref_frames = static_cast<uint8_t>(sps.max_num_ref_frames);

    pic->seq_fields.bits.chroma_format_idc = sps.chroma_format_idc;
    pic->seq_fields.bits.residual_colour_transform_flag = sps.separate_colour_plane_flag ? 1 : 0;
    pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag =
            sps.gaps_in_frame_num_value_allowed_flag ? 1 : 0;
    pic->seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only_flag ? 1 : 0;
    pic->seq_fields.bits.mb_adaptive_frame_field_flag = sps.mb_adaptive_frame_field_flag ? 1 : 0;
    pic->seq_fields.bits.direct_8x8_inference_flag = sps.direct_8x8_inference_flag ? 1 : 0;
    pic->seq_fields.bits.MinLumaBiPredSize8x8 = sps.direct_8x8_inference_flag ? 1 : 0;
    pic->seq_fields.bits.log2_max_frame_num_minus4 = sps.log2_max_frame_num_minus4;
    pic->seq_fields.bits.pic_order_cnt_type = sps.pic_order_cnt_type;
    pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
            sps.log2_max_pic_order_cnt_lsb_minus4;
    pic->seq_fields.bits.delta_pic_order_always_zero_flag =
            sps.delta_pic_order_always_zero_flag ? 1 : 0;

    pic->pic_init_qp_minus26 = static_cast<int8_t>(pps.pic_init_qp_minus26);
    pic->pic_init_qs_minus26 = static_cast<int8_t>(pps.pic_init_qs_minus26);
    pic->chroma_qp_index_offset = static_cast<int8_t>(pps.chroma_qp_index_offset);
    pic->second_chroma_qp_index_offset = static_cast<int8_t>(pps.second_chroma_qp_index_offset);

    pic->pic_fields.bits.entropy_coding_mode_flag = pps.entropy_coding_mode_flag ? 1 : 0;
    pic->pic_fields.bits.weighted_pred_flag = pps.weighted_pred_flag ? 1 : 0;
    pic->pic_fields.bits.weighted_bipred_idc = pps.weighted_bipred_idc;
    pic->pic_fields.bits.transform_8x8_mode_flag = pps.transform_8x8_mode_flag ? 1 : 0;
    pic->pic_fields.bits.field_pic_flag = first.field_pic_flag ? 1 : 0;
    pic->pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra_pred_flag ? 1 : 0;
    pic->pic_fields.bits.pic_order_present_flag =
            pps.bottom_field_pic_order_in_frame_present_flag ? 1 : 0;
    pic->pic_fields.bits.deblocking_filter_control_present_flag =
            pps.deblocking_filter_control_present_flag ? 1 : 0;
    pic->pic_fields.bits.redundant_pic_cnt_present_flag =
            pps.redundant_pic_cnt_present_flag ? 1 : 0;
    pic->pic_fields.bits.reference_pic_flag = (first.nalRefIdc != 0) ? 1 : 0;

    pic->frame_num = static_cast<uint16_t>(first.frame_num);

    // ReferenceFrames[]: the current short/long-term references in the DPB.
    int idx = 0;
    for (const auto& e : mDpb) {
        if (idx >= 16) break;
        if (!e->refUsed && !e->longTerm) continue;
        uint32_t flags = 0;
        if (e->longTerm) {
            flags |= VA_PICTURE_H264_LONG_TERM_REFERENCE;
        } else {
            flags |= VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        }
        fillVAPicture(*e, flags, &pic->ReferenceFrames[idx]);
        ++idx;
    }
    for (; idx < 16; ++idx) {
        pic->ReferenceFrames[idx].picture_id = VA_INVALID_SURFACE;
        pic->ReferenceFrames[idx].flags = VA_PICTURE_H264_INVALID;
    }

    return true;
}

void VaapiVideoDecoder::fillVAPicture(const DpbEntry& e, uint32_t flags, VAPictureH264* out) const {
    memset(out, 0, sizeof(*out));
    out->picture_id = e.surface;
    out->frame_idx = e.longTerm ? e.longTermFrameIdx : e.frameNum;
    out->flags = flags;
    out->TopFieldOrderCnt = e.topFieldOrderCnt;
    out->BottomFieldOrderCnt = e.bottomFieldOrderCnt;
}

void VaapiVideoDecoder::fillIQMatrix(const H264SPS& sps, const H264PPS& pps,
                                     VAIQMatrixBufferH264* iq) {
    memset(iq, 0, sizeof(*iq));
    // PPS scaling matrix takes precedence over SPS; otherwise flat (16).
    const uint8_t(*src4x4)[16] = nullptr;
    const uint8_t(*src8x8)[64] = nullptr;
    if (pps.pic_scaling_matrix_present_flag) {
        src4x4 = pps.scaling_list4x4;
        src8x8 = pps.scaling_list8x8;
    } else if (sps.seq_scaling_matrix_present_flag) {
        src4x4 = sps.scaling_list4x4;
        src8x8 = sps.scaling_list8x8;
    }
    if (src4x4) {
        for (int i = 0; i < 6; ++i) memcpy(iq->ScalingList4x4[i], src4x4[i], 16);
        for (int i = 0; i < 2; ++i) memcpy(iq->ScalingList8x8[i], src8x8[i], 64);
    } else {
        for (int i = 0; i < 6; ++i) memset(iq->ScalingList4x4[i], 16, 16);
        for (int i = 0; i < 2; ++i) memset(iq->ScalingList8x8[i], 16, 64);
    }
}

bool VaapiVideoDecoder::fillSliceParam(const H264SPS& sps, const H264PPS& pps,
                                       const H264SliceHeader& sh, int32_t currPicOrderCnt,
                                       VASliceParameterBufferH264* sp) {
    memset(sp, 0, sizeof(*sp));

    sp->slice_data_size = static_cast<uint32_t>(sh.nalSize);
    sp->slice_data_offset = 0;
    sp->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    sp->slice_data_bit_offset = static_cast<uint16_t>(sh.header_bit_size);
    sp->first_mb_in_slice = static_cast<uint16_t>(sh.first_mb_in_slice);
    sp->slice_type = static_cast<uint8_t>(sh.slice_type);
    sp->direct_spatial_mv_pred_flag = sh.direct_spatial_mv_pred_flag ? 1 : 0;

    if (sh.num_ref_idx_active_override_flag) {
        sp->num_ref_idx_l0_active_minus1 = static_cast<uint8_t>(sh.num_ref_idx_l0_active_minus1);
        sp->num_ref_idx_l1_active_minus1 = static_cast<uint8_t>(sh.num_ref_idx_l1_active_minus1);
    } else {
        sp->num_ref_idx_l0_active_minus1 =
                static_cast<uint8_t>(pps.num_ref_idx_l0_default_active_minus1);
        sp->num_ref_idx_l1_active_minus1 =
                static_cast<uint8_t>(pps.num_ref_idx_l1_default_active_minus1);
    }
    sp->cabac_init_idc = static_cast<uint8_t>(sh.cabac_init_idc);
    sp->slice_qp_delta = static_cast<int8_t>(sh.slice_qp_delta);
    sp->disable_deblocking_filter_idc = static_cast<uint8_t>(sh.disable_deblocking_filter_idc);
    sp->slice_alpha_c0_offset_div2 = static_cast<int8_t>(sh.slice_alpha_c0_offset_div2);
    sp->slice_beta_offset_div2 = static_cast<int8_t>(sh.slice_beta_offset_div2);

    // Initialize ref pic lists as invalid, then fill the ones this slice needs.
    for (int i = 0; i < 32; ++i) {
        sp->RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        sp->RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        sp->RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        sp->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }

    // Holes (invalid modification) and entries past what the DPB could supply
    // are concealed with the closest available reference rather than left
    // VA_INVALID_SURFACE: a slice may index any entry below num_ref_idx_active,
    // and an INVALID entry sends the driver's motion compensation into
    // uninitialized VRAM. Mispredicting from a real frame is the standard
    // fallback (ffmpeg fills its default list the same way).
    auto fillList = [this](const std::vector<const DpbEntry*>& list, size_t numActive,
                           VAPictureH264* out) {
        const int n = std::min<int>(list.size(), 32);
        const DpbEntry* fallback = nullptr;
        for (int i = 0; i < n; ++i) {
            if (list[i]) {
                fallback = list[i];
                break;
            }
        }
        const int limit = std::min<int>(std::max<size_t>(numActive, n), 32);
        for (int i = 0; i < limit; ++i) {
            const DpbEntry* e = (i < n && list[i]) ? list[i] : fallback;
            if (!e) continue;
            const uint32_t flags = e->longTerm ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                               : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            fillVAPicture(*e, flags, &out[i]);
        }
    };

    if (isPSlice(sh.slice_type) || isSPSlice(sh.slice_type)) {
        std::vector<const DpbEntry*> list0;
        buildRefPicListP(sps, sh, &list0);
        fillList(list0, sp->num_ref_idx_l0_active_minus1 + 1u, sp->RefPicList0);
    } else if (isBSlice(sh.slice_type)) {
        std::vector<const DpbEntry*> list0;
        std::vector<const DpbEntry*> list1;
        buildRefPicListB(sps, sh, currPicOrderCnt, &list0, &list1);
        fillList(list0, sp->num_ref_idx_l0_active_minus1 + 1u, sp->RefPicList0);
        fillList(list1, sp->num_ref_idx_l1_active_minus1 + 1u, sp->RefPicList1);
    }
    // I/SI slices have no reference lists.

    // Weighted prediction (explicit): VA wants the full per-entry tables with
    // the inferred defaults (1 << denom, 0) for entries the stream did not
    // signal. For implicit bipred (weighted_bipred_idc == 2) the driver derives
    // weights from POC; the tables stay zeroed.
    sp->luma_log2_weight_denom = static_cast<uint8_t>(sh.luma_log2_weight_denom);
    sp->chroma_log2_weight_denom = static_cast<uint8_t>(sh.chroma_log2_weight_denom);
    if (sh.hasWeightTable) {
        const bool hasChroma = (sps.chroma_format_idc != 0);
        const int32_t defLuma = 1 << sh.luma_log2_weight_denom;
        const int32_t defChroma = 1 << sh.chroma_log2_weight_denom;

        sp->luma_weight_l0_flag = 1;
        sp->chroma_weight_l0_flag = hasChroma ? 1 : 0;
        for (uint32_t i = 0; i <= sh.num_ref_idx_l0_active_minus1; ++i) {
            const H264SliceHeader::WeightEntry& w = sh.weights[0][i];
            sp->luma_weight_l0[i] = static_cast<int16_t>(w.lumaPresent ? w.lumaWeight : defLuma);
            sp->luma_offset_l0[i] = static_cast<int16_t>(w.lumaPresent ? w.lumaOffset : 0);
            if (hasChroma) {
                for (int j = 0; j < 2; ++j) {
                    sp->chroma_weight_l0[i][j] =
                            static_cast<int16_t>(w.chromaPresent ? w.chromaWeight[j] : defChroma);
                    sp->chroma_offset_l0[i][j] =
                            static_cast<int16_t>(w.chromaPresent ? w.chromaOffset[j] : 0);
                }
            }
        }
        if (isBSlice(sh.slice_type)) {
            sp->luma_weight_l1_flag = 1;
            sp->chroma_weight_l1_flag = hasChroma ? 1 : 0;
            for (uint32_t i = 0; i <= sh.num_ref_idx_l1_active_minus1; ++i) {
                const H264SliceHeader::WeightEntry& w = sh.weights[1][i];
                sp->luma_weight_l1[i] =
                        static_cast<int16_t>(w.lumaPresent ? w.lumaWeight : defLuma);
                sp->luma_offset_l1[i] = static_cast<int16_t>(w.lumaPresent ? w.lumaOffset : 0);
                if (hasChroma) {
                    for (int j = 0; j < 2; ++j) {
                        sp->chroma_weight_l1[i][j] = static_cast<int16_t>(
                                w.chromaPresent ? w.chromaWeight[j] : defChroma);
                        sp->chroma_offset_l1[i][j] =
                                static_cast<int16_t>(w.chromaPresent ? w.chromaOffset[j] : 0);
                    }
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// POC / DPB / reference-list management
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::computePoc(const H264SPS& sps, const H264SliceHeader& sh, int32_t* topPoc,
                                   int32_t* bottomPoc) {
    // All three pic_order_cnt_types (8.2.1). Frame pictures only (field
    // decoding TODO).
    if (sps.pic_order_cnt_type == 0) {
        const int32_t maxPocLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
        int32_t prevPocMsb;
        int32_t prevPocLsb;
        if (sh.idrPicFlag) {
            prevPocMsb = 0;
            prevPocLsb = 0;
        } else {
            prevPocMsb = mPrevPicOrderCntMsb;
            prevPocLsb = mPrevPicOrderCntLsb;
        }
        int32_t pocLsb = static_cast<int32_t>(sh.pic_order_cnt_lsb);
        int32_t pocMsb;
        if (pocLsb < prevPocLsb && (prevPocLsb - pocLsb) >= (maxPocLsb / 2)) {
            pocMsb = prevPocMsb + maxPocLsb;
        } else if (pocLsb > prevPocLsb && (pocLsb - prevPocLsb) > (maxPocLsb / 2)) {
            pocMsb = prevPocMsb - maxPocLsb;
        } else {
            pocMsb = prevPocMsb;
        }
        *topPoc = pocMsb + pocLsb;
        *bottomPoc = *topPoc + sh.delta_pic_order_cnt_bottom;

        // Update state only for reference pictures.
        if (sh.nalRefIdc != 0) {
            mPrevPicOrderCntMsb = pocMsb;
            mPrevPicOrderCntLsb = pocLsb;
        }
    } else if (sps.pic_order_cnt_type == 1) {
        // 8.2.1.2: expected POC from the offset_for_ref_frame cycle.
        const int32_t maxFrameNum = 1 << (sps.log2_max_frame_num_minus4 + 4);
        int32_t frameNumOffset;
        if (sh.idrPicFlag) {
            frameNumOffset = 0;
        } else if (mPrevFrameNum > sh.frame_num) {
            frameNumOffset = mPrevFrameNumOffset + maxFrameNum;
        } else {
            frameNumOffset = mPrevFrameNumOffset;
        }

        const int32_t cycle =
                static_cast<int32_t>(std::min<uint32_t>(sps.num_ref_frames_in_pic_order_cnt_cycle,
                                                        255));
        int32_t absFrameNum = 0;
        if (cycle != 0) absFrameNum = frameNumOffset + static_cast<int32_t>(sh.frame_num);
        if (sh.nalRefIdc == 0 && absFrameNum > 0) absFrameNum -= 1;

        int32_t expectedPoc = 0;
        if (absFrameNum > 0) {
            int32_t expectedDeltaPerCycle = 0;
            for (int32_t i = 0; i < cycle; ++i) expectedDeltaPerCycle += sps.offset_for_ref_frame[i];
            const int32_t cycleCnt = (absFrameNum - 1) / cycle;
            const int32_t frameNumInCycle = (absFrameNum - 1) % cycle;
            expectedPoc = cycleCnt * expectedDeltaPerCycle;
            for (int32_t i = 0; i <= frameNumInCycle; ++i) {
                expectedPoc += sps.offset_for_ref_frame[i];
            }
        }
        if (sh.nalRefIdc == 0) expectedPoc += sps.offset_for_non_ref_pic;

        *topPoc = expectedPoc + sh.delta_pic_order_cnt[0];
        *bottomPoc = *topPoc + sps.offset_for_top_to_bottom_field + sh.delta_pic_order_cnt[1];
        mPrevFrameNumOffset = frameNumOffset;
    } else {  // pic_order_cnt_type == 2
        int32_t frameNumOffset;
        if (sh.idrPicFlag) {
            frameNumOffset = 0;
        } else {
            const int32_t maxFrameNum = 1 << (sps.log2_max_frame_num_minus4 + 4);
            if (mPrevFrameNum > sh.frame_num) {
                frameNumOffset = mPrevFrameNumOffset + maxFrameNum;
            } else {
                frameNumOffset = mPrevFrameNumOffset;
            }
        }
        int32_t tempPoc;
        if (sh.idrPicFlag) {
            tempPoc = 0;
        } else if (sh.nalRefIdc == 0) {
            tempPoc = 2 * (frameNumOffset + static_cast<int32_t>(sh.frame_num)) - 1;
        } else {
            tempPoc = 2 * (frameNumOffset + static_cast<int32_t>(sh.frame_num));
        }
        *topPoc = tempPoc;
        *bottomPoc = tempPoc;
        mPrevFrameNumOffset = frameNumOffset;
    }
    mPrevFrameNum = sh.frame_num;
    if (sh.nalRefIdc != 0) mHasPrevRefFrame = true;
}

void VaapiVideoDecoder::updatePicNums(const H264SPS& sps, uint32_t currFrameNum) {
    const int32_t maxFrameNum = 1 << (sps.log2_max_frame_num_minus4 + 4);
    for (auto& e : mDpb) {
        if (!e->refUsed) continue;
        if (static_cast<int32_t>(e->frameNum) > static_cast<int32_t>(currFrameNum)) {
            e->frameNumWrap = static_cast<int32_t>(e->frameNum) - maxFrameNum;
        } else {
            e->frameNumWrap = static_cast<int32_t>(e->frameNum);
        }
        e->picNum = e->frameNumWrap;
    }
}

void VaapiVideoDecoder::buildRefPicListP(const H264SPS& sps, const H264SliceHeader& sh,
                                         std::vector<const DpbEntry*>* list) {
    list->clear();
    // Initialisation (8.2.4.2.1): short-term references by descending PicNum,
    // then long-term by ascending LongTermFrameIdx.
    std::vector<const DpbEntry*> shortTerm;
    std::vector<const DpbEntry*> longTerm;
    for (const auto& e : mDpb) {
        if (e->longTerm) {
            longTerm.push_back(e.get());
        } else if (e->refUsed) {
            shortTerm.push_back(e.get());
        }
    }
    std::sort(shortTerm.begin(), shortTerm.end(),
              [](const DpbEntry* a, const DpbEntry* b) { return a->picNum > b->picNum; });
    std::sort(longTerm.begin(), longTerm.end(), [](const DpbEntry* a, const DpbEntry* b) {
        return a->longTermFrameIdx < b->longTermFrameIdx;
    });
    list->insert(list->end(), shortTerm.begin(), shortTerm.end());
    list->insert(list->end(), longTerm.begin(), longTerm.end());

    const size_t numActive = sh.num_ref_idx_l0_active_minus1 + 1;
    if (list->size() > numActive) list->resize(numActive);
    applyRefListModification(sps, sh, 0, list);
}

void VaapiVideoDecoder::buildRefPicListB(const H264SPS& sps, const H264SliceHeader& sh,
                                         int32_t currPicOrderCnt,
                                         std::vector<const DpbEntry*>* list0,
                                         std::vector<const DpbEntry*>* list1) {
    list0->clear();
    list1->clear();
    // Initialisation (8.2.4.2.3). list0: short-term with POC < current
    // descending, then POC > current ascending, then long-term. list1 swaps the
    // two short-term halves. |currPicOrderCnt| is the current picture's POC —
    // the current picture itself is not in the DPB yet.

    std::vector<const DpbEntry*> before;  // POC < curr
    std::vector<const DpbEntry*> after;   // POC > curr
    std::vector<const DpbEntry*> longTerm;
    for (const auto& e : mDpb) {
        if (e->longTerm) {
            longTerm.push_back(e.get());
        } else if (e->refUsed) {
            if (e->picOrderCnt < currPicOrderCnt) {
                before.push_back(e.get());
            } else {
                after.push_back(e.get());
            }
        }
    }
    std::sort(before.begin(), before.end(),
              [](const DpbEntry* a, const DpbEntry* b) { return a->picOrderCnt > b->picOrderCnt; });
    std::sort(after.begin(), after.end(),
              [](const DpbEntry* a, const DpbEntry* b) { return a->picOrderCnt < b->picOrderCnt; });
    std::sort(longTerm.begin(), longTerm.end(), [](const DpbEntry* a, const DpbEntry* b) {
        return a->longTermFrameIdx < b->longTermFrameIdx;
    });

    list0->insert(list0->end(), before.begin(), before.end());
    list0->insert(list0->end(), after.begin(), after.end());
    list0->insert(list0->end(), longTerm.begin(), longTerm.end());

    list1->insert(list1->end(), after.begin(), after.end());
    list1->insert(list1->end(), before.begin(), before.end());
    list1->insert(list1->end(), longTerm.begin(), longTerm.end());

    // If list1 has more than one entry and equals list0, swap first two.
    if (list1->size() > 1 && *list0 == *list1) {
        std::swap((*list1)[0], (*list1)[1]);
    }

    const size_t numActive0 = sh.num_ref_idx_l0_active_minus1 + 1;
    const size_t numActive1 = sh.num_ref_idx_l1_active_minus1 + 1;
    if (list0->size() > numActive0) list0->resize(numActive0);
    if (list1->size() > numActive1) list1->resize(numActive1);
    applyRefListModification(sps, sh, 0, list0);
    applyRefListModification(sps, sh, 1, list1);
}

void VaapiVideoDecoder::applyRefListModification(const H264SPS& sps, const H264SliceHeader& sh,
                                                 int listIdx,
                                                 std::vector<const DpbEntry*>* list) {
    const auto& mods = sh.refListMods[listIdx];
    if (mods.empty()) return;

    const size_t numActive = (listIdx == 0 ? sh.num_ref_idx_l0_active_minus1
                                           : sh.num_ref_idx_l1_active_minus1) +
                             1;
    // Frame coding: MaxPicNum = MaxFrameNum, CurrPicNum = frame_num (8.2.4.3).
    const int32_t maxPicNum = 1 << (sps.log2_max_frame_num_minus4 + 4);
    const int32_t currPicNum = static_cast<int32_t>(sh.frame_num);

    // The spec's shift step works on a list of numActive + 1 slots; missing
    // entries stay nullptr ("no reference picture") and are trimmed at the end.
    std::vector<const DpbEntry*> ref(*list);
    ref.resize(std::max(ref.size(), numActive) + 1, nullptr);

    int32_t picNumPred = currPicNum;
    size_t refIdx = 0;
    for (const auto& mod : mods) {
        const DpbEntry* pic = nullptr;
        if (mod.op == 0 || mod.op == 1) {
            const int32_t absDiff = static_cast<int32_t>(mod.value) + 1;
            int32_t noWrap;
            if (mod.op == 0) {
                noWrap = picNumPred - absDiff;
                if (noWrap < 0) noWrap += maxPicNum;
            } else {
                noWrap = picNumPred + absDiff;
                if (noWrap >= maxPicNum) noWrap -= maxPicNum;
            }
            picNumPred = noWrap;
            const int32_t picNum = (noWrap > currPicNum) ? noWrap - maxPicNum : noWrap;
            for (const auto& e : mDpb) {
                if (e->refUsed && !e->longTerm && e->picNum == picNum) {
                    pic = e.get();
                    break;
                }
            }
        } else {  // op == 2: long_term_pic_num (== LongTermFrameIdx for frames)
            for (const auto& e : mDpb) {
                if (e->longTerm && e->longTermFrameIdx == mod.value) {
                    pic = e.get();
                    break;
                }
            }
        }
        if (!pic) {
            ALOGW("ref_pic_list_modification: no reference matches op=%u value=%u.", mod.op,
                  mod.value);
            continue;
        }
        if (refIdx >= numActive) break;
        // Insert |pic| at refIdx, shift the tail down, drop its old occurrence.
        for (size_t c = numActive; c > refIdx; --c) {
            ref[c] = ref[c - 1];
        }
        ref[refIdx++] = pic;
        size_t n = refIdx;
        for (size_t c = refIdx; c <= numActive; ++c) {
            if (ref[c] != pic) ref[n++] = ref[c];
        }
    }

    ref.resize(numActive);
    while (!ref.empty() && ref.back() == nullptr) ref.pop_back();
    *list = std::move(ref);
}

void VaapiVideoDecoder::referenceListMarking(const H264SPS& sps, const H264SliceHeader& sh,
                                             DpbEntry* current) {
    if (sh.nalRefIdc == 0) {
        current->refUsed = false;
        return;
    }

    if (sh.idrPicFlag) {
        // All prior references were cleared on IDR (8.2.5.1).
        if (sh.long_term_reference_flag) {
            current->refUsed = false;
            current->longTerm = true;
            current->longTermFrameIdx = 0;
            mMaxLongTermFrameIdx = 0;
        } else {
            current->refUsed = true;
            current->longTerm = false;
            mMaxLongTermFrameIdx = -1;
        }
        return;
    }

    if (sh.adaptiveRefPicMarking) {
        applyAdaptiveMarking(sps, sh, current);
    } else {
        // Sliding-window marking (8.2.5.3).
        current->refUsed = true;
        current->longTerm = false;
    }

    // Sliding-window retirement; after adaptive marking this only acts as a
    // guard against invalid streams over-filling the DPB (a conformant stream
    // keeps itself within max_num_ref_frames via MMCO).
    // Count short-term + long-term references (excluding the just-added current).
    size_t numRef = 0;
    for (const auto& e : mDpb) {
        if (e.get() == current) continue;
        if (e->refUsed || e->longTerm) ++numRef;
    }

    const size_t maxRef = std::max<uint32_t>(sps.max_num_ref_frames, 1);
    // If we now exceed the window, retire the short-term ref with smallest
    // FrameNumWrap.
    while (numRef >= maxRef) {
        DpbEntry* victim = nullptr;
        for (const auto& e : mDpb) {
            if (e.get() == current) continue;
            if (!e->refUsed || e->longTerm) continue;
            if (!victim || e->frameNumWrap < victim->frameNumWrap) {
                victim = e.get();
            }
        }
        if (!victim) break;
        victim->refUsed = false;
        --numRef;
    }
}

void VaapiVideoDecoder::applyAdaptiveMarking(const H264SPS& sps, const H264SliceHeader& sh,
                                             DpbEntry* current) {
    (void)sps;
    // MMCO operations (8.2.5.4). Frame coding: CurrPicNum = frame_num,
    // picNumX = CurrPicNum - (difference_of_pic_nums_minus1 + 1).
    const int32_t currPicNum = static_cast<int32_t>(sh.frame_num);
    current->refUsed = true;
    current->longTerm = false;

    for (const auto& op : sh.mmcoOps) {
        switch (op.op) {
        case 1: {  // short-term -> unused
            const int32_t picNumX = currPicNum - (static_cast<int32_t>(op.arg1) + 1);
            for (auto& e : mDpb) {
                if (e.get() != current && e->refUsed && !e->longTerm && e->picNum == picNumX) {
                    e->refUsed = false;
                    break;
                }
            }
            break;
        }
        case 2: {  // long-term -> unused
            for (auto& e : mDpb) {
                if (e->longTerm && e->longTermFrameIdx == op.arg1) {
                    e->longTerm = false;
                    break;
                }
            }
            break;
        }
        case 3: {  // short-term -> long-term with idx
            for (auto& e : mDpb) {
                if (e.get() != current && e->longTerm && e->longTermFrameIdx == op.arg2) {
                    e->longTerm = false;
                }
            }
            const int32_t picNumX = currPicNum - (static_cast<int32_t>(op.arg1) + 1);
            for (auto& e : mDpb) {
                if (e.get() != current && e->refUsed && !e->longTerm && e->picNum == picNumX) {
                    e->refUsed = false;
                    e->longTerm = true;
                    e->longTermFrameIdx = op.arg2;
                    break;
                }
            }
            break;
        }
        case 4: {  // set MaxLongTermFrameIdx
            mMaxLongTermFrameIdx = static_cast<int32_t>(op.arg1) - 1;
            for (auto& e : mDpb) {
                if (e->longTerm &&
                    static_cast<int32_t>(e->longTermFrameIdx) > mMaxLongTermFrameIdx) {
                    e->longTerm = false;
                }
            }
            break;
        }
        case 5: {  // clear all references, rebase POC to 0
            // Prior pictures live in the old POC timebase: output them now (in
            // POC order) so bumping never interleaves the two timebases.
            std::vector<DpbEntry*> pending;
            for (const auto& e : mDpb) {
                if (e.get() == current) continue;
                if (!e->outputted) pending.push_back(e.get());
            }
            std::sort(pending.begin(), pending.end(), [](const DpbEntry* a, const DpbEntry* b) {
                return a->picOrderCnt < b->picOrderCnt;
            });
            for (DpbEntry* e : pending) {
                queueForOutput(*e);
                e->outputted = true;
            }
            for (auto& e : mDpb) {
                if (e.get() == current) continue;
                e->refUsed = false;
                e->longTerm = false;
            }
            mMaxLongTermFrameIdx = -1;
            const int32_t tempPoc = current->picOrderCnt;
            current->topFieldOrderCnt -= tempPoc;
            current->bottomFieldOrderCnt -= tempPoc;
            current->picOrderCnt = 0;
            current->frameNum = 0;
            // The next picture's POC prediction restarts from the rebased
            // current picture (8.2.1).
            mPrevPicOrderCntMsb = 0;
            mPrevPicOrderCntLsb = current->topFieldOrderCnt;
            mPrevFrameNum = 0;
            mPrevFrameNumOffset = 0;
            break;
        }
        case 6: {  // current -> long-term with idx
            for (auto& e : mDpb) {
                if (e.get() != current && e->longTerm && e->longTermFrameIdx == op.arg2) {
                    e->longTerm = false;
                }
            }
            current->refUsed = false;
            current->longTerm = true;
            current->longTermFrameIdx = op.arg2;
            break;
        }
        default:
            break;
        }
    }
}

void VaapiVideoDecoder::bumpAndOutputAsNeeded(const H264SPS& sps) {
    // Output pictures in POC order once the DPB is "full enough" (has more than
    // max_num_reorder_frames buffered). We approximate max_num_reorder_frames
    // with max_num_ref_frames.
    bumpAndOutputAsNeeded(static_cast<size_t>(std::max<uint32_t>(sps.max_num_ref_frames, 1)));
}

void VaapiVideoDecoder::bumpAndOutputAsNeeded(size_t reorderDepth) {
    // Count entries not yet outputted.
    auto countPendingOutput = [&]() {
        size_t n = 0;
        for (const auto& e : mDpb)
            if (!e->outputted) ++n;
        return n;
    };

    while (countPendingOutput() > reorderDepth) {
        // Find the not-yet-outputted entry with the smallest POC.
        DpbEntry* next = nullptr;
        for (const auto& e : mDpb) {
            if (e->outputted) continue;
            if (!next || e->picOrderCnt < next->picOrderCnt) {
                next = e.get();
            }
        }
        if (!next) break;
        queueForOutput(*next);
        next->outputted = true;
    }
}

void VaapiVideoDecoder::flushDpb() {
    // Emit every not-yet-outputted picture in POC order, then drop references.
    std::vector<DpbEntry*> pending;
    for (const auto& e : mDpb) {
        if (!e->outputted) pending.push_back(e.get());
    }
    std::sort(pending.begin(), pending.end(),
              [](const DpbEntry* a, const DpbEntry* b) { return a->picOrderCnt < b->picOrderCnt; });
    for (DpbEntry* e : pending) {
        queueForOutput(*e);
        e->outputted = true;
    }
    // All references are now dropped; entries will be reclaimed once their
    // pending output completes.
    for (const auto& e : mDpb) {
        e->refUsed = false;
        e->longTerm = false;
    }
    reclaimDpbEntries();
}

void VaapiVideoDecoder::reclaimDpbEntries() {
    // An entry can be dropped once it is neither a reference nor awaiting
    // output. Its surface is only returned to the free list when no pending
    // output still references it.
    for (auto it = mDpb.begin(); it != mDpb.end();) {
        DpbEntry* e = it->get();
        if (e->refUsed || e->longTerm || !e->outputted) {
            ++it;
            continue;
        }
        // Is this surface still needed by a pending output? If so, keep the
        // free-list return until pumpOutput consumes it.
        it = mDpb.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Output path (async, POC-ordered)
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::queueForOutput(const DpbEntry& e) {
    PendingOutput po;
    po.surface = e.surface;
    po.bitstreamId = e.bitstreamId;
    po.codedSize = e.codedSize;
    po.visibleRect = e.visibleRect;
    mPendingOutputs.push(po);
    pumpOutput();
}

void VaapiVideoDecoder::pumpOutput() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (mOutputFetchInFlight) return;
    if (mPendingOutputs.empty()) return;
    if (!mVideoFramePool) return;

    const PendingOutput& po = mPendingOutputs.front();
    const int32_t bitstreamId = po.bitstreamId;
    const VASurfaceID surface = po.surface;

    mOutputFetchInFlight = true;
    if (!mVideoFramePool->getVideoFrame(::base::BindOnce(&VaapiVideoDecoder::onVideoFrameReady,
                                                         mWeakThis, mFetchGeneration, bitstreamId,
                                                         surface))) {
        // A previous callback is still running; it will re-drive us.
        mOutputFetchInFlight = false;
    }
}

void VaapiVideoDecoder::onVideoFrameReady(
        uint64_t fetchGeneration, int32_t bitstreamId, VASurfaceID surface,
        std::optional<VideoFramePool::FrameWithBlockId> frameWithBlockId) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (fetchGeneration != mFetchGeneration) {
        // Fetched before a flush or a resolution-change pool reset: the pending
        // output it belonged to is gone (and its VA surface may be freed). A
        // fresh fetch, if any, carries the current generation — leave its
        // in-flight latch alone and drop this frame on the floor.
        ALOGV("Ignoring stale getVideoFrame callback (gen %" PRIu64 " != %" PRIu64 ").",
              fetchGeneration, mFetchGeneration);
        return;
    }
    mOutputFetchInFlight = false;

    if (!frameWithBlockId) {
        ALOGE("Got null VideoFrame from pool.");
        onError();
        return;
    }

    std::unique_ptr<VideoFrame> frame;
    uint32_t blockId;
    std::tie(frame, blockId) = std::move(*frameWithBlockId);
    (void)blockId;

    // Copy and crop with the geometry recorded when this picture was decoded:
    // a mid-stream resolution change flips mCodedSize/mVisibleRect at the new
    // SPS while pictures from the previous one are still queued here — stamping
    // those with the current rect shows them shrunken or zoomed, with an
    // edge-garbage strip, for every queued picture at each ABR switch
    // (remora-e97; Pluto flips 1216x684<->1280x720 at every ad boundary).
    ui::Size codedSize = mCodedSize;
    Rect visibleRect = mVisibleRect;
    if (!mPendingOutputs.empty()) {
        codedSize = mPendingOutputs.front().codedSize;
        visibleRect = mPendingOutputs.front().visibleRect;
    }

    if (!copySurfaceToFrame(surface, frame.get(), codedSize, visibleRect)) {
        ALOGE("Failed to copy VA surface into output frame.");
        onError();
        return;
    }

    // Pop the corresponding pending entry (front, since we process in order).
    if (!mPendingOutputs.empty()) {
        mPendingOutputs.pop();
    }

    frame->setBitstreamId(bitstreamId);
    frame->setVisibleRect(visibleRect);
    mOutputCb.Run(std::move(frame));

    // Surfaces may now be reclaimable; continue draining pending outputs and
    // retry any decode request parked on surface exhaustion.
    reclaimDpbEntries();
    pumpOutput();
    pumpDecodeRequests();

    // If we were draining and everything is flushed, finish the drain.
    if (mState == State::Draining && mPendingOutputs.empty() && !mOutputFetchInFlight) {
        if (mDrainCb) {
            std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kOk);
        }
        setState(State::Idle);
    }
}

VaapiVideoDecoder::OutputBufferInfo VaapiVideoDecoder::queryOutputBufferInfo(
        VideoFrame& frame, const ui::Size& codedSize) {
    OutputBufferInfo info;
    // Tight-pack linear defaults (last-resort fallback).
    info.pitchY = static_cast<uint32_t>(codedSize.width);
    info.pitchUV = static_cast<uint32_t>(codedSize.width);
    info.offsetY = 0;
    info.offsetUV = static_cast<uint32_t>(codedSize.width) * codedSize.height;
    info.modifier = 0;

    // Preferred: read layout + modifier straight from the cros_gralloc handle.
    const C2Handle* c2Handle = frame.getGraphicBlock().handle();
    native_handle_t* nh =
            c2Handle ? android::UnwrapNativeCodec2GrallocHandle(c2Handle) : nullptr;
    if (nh) {
        const size_t dataWords = static_cast<size_t>(nh->numFds) + nh->numInts;
        const size_t structWords =
                (sizeof(CrosGrallocHandle) - sizeof(native_handle_t)) / sizeof(int);
        if (dataWords == structWords) {
            const auto* cros = static_cast<const CrosGrallocHandle*>(nh);
            if (cros->magic == kCrosGrallocMagic && cros->numPlanes >= 1 &&
                cros->strides[0] > 0) {
                info.pitchY = cros->strides[0];
                info.pitchUV = cros->numPlanes >= 2 ? cros->strides[1] : 0;
                info.offsetY = cros->offsets[0];
                info.offsetUV = cros->numPlanes >= 2 ? cros->offsets[1] : 0;
                info.drmFormat = cros->format;
                info.modifier = cros->formatModifier;
                info.totalSize = cros->totalSize;
                info.fromHandle = true;
            }
        }
        native_handle_delete(nh);
    }

    if (!info.fromHandle) {
        // Legacy: CPU-lock layout query (only works when the buffer has CPU
        // usage bits; keeps non-minigbm grallocs working).
        const android_ycbcr ycbcr = getGraphicBlockInfo(frame.getGraphicBlock());
        const uintptr_t cb = reinterpret_cast<uintptr_t>(ycbcr.cb);
        const uintptr_t cr = reinterpret_cast<uintptr_t>(ycbcr.cr);
        if (ycbcr.ystride > 0 && (cb != 0 || cr != 0)) {
            info.pitchY = static_cast<uint32_t>(ycbcr.ystride);
            info.pitchUV =
                    static_cast<uint32_t>(ycbcr.cstride > 0 ? ycbcr.cstride : ycbcr.ystride);
            info.offsetUV =
                    static_cast<uint32_t>(std::min(cb != 0 ? cb : cr, cr != 0 ? cr : cb));
            if (ycbcr.chroma_step != 2) {
                ALOGW("Output buffer is not semiplanar (chroma_step=%zu); NV12 import may be "
                      "wrong.",
                      ycbcr.chroma_step);
            }
        } else {
            ALOGW("No cros_gralloc handle and gralloc lock failed; assuming tightly-packed "
                  "NV12.");
        }
    }
    ALOGI("Output buffer info: pitchY=%u pitchUV=%u offsets=%u/%u drmFormat=%.4s "
          "modifier=0x%" PRIx64 " total=%" PRIu64 " fromHandle=%d (coded %dx%d)",
          info.pitchY, info.pitchUV, info.offsetY, info.offsetUV,
          info.drmFormat ? reinterpret_cast<const char*>(&info.drmFormat) : "NV12",
          info.modifier, info.totalSize, info.fromHandle, codedSize.width, codedSize.height);
    return info;
}

VASurfaceID VaapiVideoDecoder::importFrameAsSurface(VideoFrame& frame,
                                                    const ui::Size& codedSize) {
    VADisplay dpy = mDevice->display();

    const std::vector<int>& fds = frame.getFDs();
    if (fds.empty()) {
        ALOGE("VideoFrame has no dma-buf FDs.");
        return VA_INVALID_SURFACE;
    }

    // The layout/modifier is identical for every buffer of one pool generation;
    // query once and cache until the pool is recreated.
    if (!mOutputInfo) {
        mOutputInfo = queryOutputBufferInfo(frame, codedSize);
    }
    const OutputBufferInfo& info = *mOutputInfo;

    // Surface-bound pools allocate RGB (VaapiDecodeComponent picks the format;
    // the handle is the source of truth) — one plane, VPP converts in the blit.
    // Everything else stays NV12.
    uint32_t vaFourcc = VA_FOURCC_NV12;
    uint32_t vaRtFormat = VA_RT_FORMAT_YUV420;
    uint32_t drmFormat = DRM_FORMAT_NV12;
    bool isRgb = false;
    switch (info.drmFormat) {
    case DRM_FORMAT_XBGR8888:
        vaFourcc = VA_FOURCC_RGBX;
        isRgb = true;
        break;
    case DRM_FORMAT_ABGR8888:
        vaFourcc = VA_FOURCC_RGBA;
        isRgb = true;
        break;
    case DRM_FORMAT_XRGB8888:
        vaFourcc = VA_FOURCC_BGRX;
        isRgb = true;
        break;
    case DRM_FORMAT_ARGB8888:
        vaFourcc = VA_FOURCC_BGRA;
        isRgb = true;
        break;
    default:
        break;
    }
    if (isRgb) {
        vaRtFormat = VA_RT_FORMAT_RGB32;
        drmFormat = info.drmFormat;
    }

    // data_size must cover the whole bo; prefer the handle's figure, then the
    // dma-buf itself, then the layout-derived floor.
    const uint32_t minSize =
            isRgb ? info.offsetY + info.pitchY * static_cast<uint32_t>(codedSize.height)
                  : info.offsetUV +
                            info.pitchUV * ((static_cast<uint32_t>(codedSize.height) + 1) / 2);
    uint64_t boSize = info.totalSize;
    if (boSize == 0) {
        const off_t seekEnd = lseek(fds[0], 0, SEEK_END);
        lseek(fds[0], 0, SEEK_SET);
        if (seekEnd > 0) boSize = static_cast<uint64_t>(seekEnd);
    }
    if (boSize < minSize) boSize = minSize;

    VASurfaceID surface = VA_INVALID_SURFACE;
    VAStatus status;

    if (info.fromHandle) {
        // DRM_PRIME_2: carries the true format modifier, so tiled buffers
        // (minigbm-xe picks X-/4-tiled when a buffer has no CPU usage bits)
        // import correctly instead of being scrambled by a linear assumption.
        VADRMPRIMESurfaceDescriptor desc;
        memset(&desc, 0, sizeof(desc));
        desc.fourcc = vaFourcc;
        desc.width = codedSize.width;
        desc.height = codedSize.height;
        desc.num_objects = 1;
        desc.objects[0].fd = fds[0];
        desc.objects[0].size = static_cast<uint32_t>(boSize);
        desc.objects[0].drm_format_modifier = info.modifier;
        desc.num_layers = 1;
        desc.layers[0].drm_format = drmFormat;
        desc.layers[0].num_planes = isRgb ? 1 : 2;
        desc.layers[0].object_index[0] = 0;
        desc.layers[0].offset[0] = info.offsetY;
        desc.layers[0].pitch[0] = info.pitchY;
        if (!isRgb) {
            desc.layers[0].object_index[1] = 0;
            desc.layers[0].offset[1] = info.offsetUV;
            desc.layers[0].pitch[1] = info.pitchUV;
        }

        VASurfaceAttrib attribs[2];
        memset(attribs, 0, sizeof(attribs));
        attribs[0].type = VASurfaceAttribMemoryType;
        attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[0].value.type = VAGenericValueTypeInteger;
        attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
        attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[1].value.type = VAGenericValueTypePointer;
        attribs[1].value.value.p = &desc;

        status = vaCreateSurfaces(dpy, vaRtFormat, codedSize.width, codedSize.height,
                                  &surface, 1, attribs, 2);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateSurfaces(DRM_PRIME_2, modifier=0x%" PRIx64 ") failed: %s",
                  info.modifier, vaErrorStr(status));
            return VA_INVALID_SURFACE;
        }
        return surface;
    }

    // Legacy linear import (non-minigbm grallocs).
    uintptr_t buffers[1] = {static_cast<uintptr_t>(fds[0])};
    VASurfaceAttribExternalBuffers ext;
    memset(&ext, 0, sizeof(ext));
    ext.pixel_format = VA_FOURCC_NV12;
    ext.width = codedSize.width;
    ext.height = codedSize.height;
    ext.num_planes = 2;
    ext.pitches[0] = info.pitchY;
    ext.pitches[1] = info.pitchUV;
    ext.offsets[0] = info.offsetY;
    ext.offsets[1] = info.offsetUV;
    ext.data_size = static_cast<uint32_t>(boSize);
    ext.buffers = buffers;
    ext.num_buffers = 1;
    ext.flags = 0;

    VASurfaceAttrib attribs[2];
    memset(attribs, 0, sizeof(attribs));
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &ext;

    status = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, codedSize.width, codedSize.height,
                              &surface, 1, attribs, 2);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces(import) failed: %s", vaErrorStr(status));
        return VA_INVALID_SURFACE;
    }
    return surface;
}

bool VaapiVideoDecoder::vppBlitSurface(VASurfaceID src, VASurfaceID dst,
                                       const ui::Size& codedSize, const Rect& visibleRect) {
    ATRACE_CALL();
    VADisplay dpy = mDevice->display();

    // Lazy, cached VPP pair (per-frame create/destroy costs 10-20 ms; see the
    // encoder's convertToNV12). Torn down with the VA context on DRC. Created
    // at mContextSize (the grow-only max) so it covers every picture size the
    // decode context can produce.
    if (mVppContext == VA_INVALID_ID) {
        VAStatus s = vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                    &mVppConfig);
        if (s != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateConfig(VPP) failed: %s", vaErrorStr(s));
            return false;
        }
        s = vaCreateContext(dpy, mVppConfig, mContextSize.width, mContextSize.height,
                            VA_PROGRESSIVE, nullptr, 0, &mVppContext);
        if (s != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateContext(VPP) failed: %s", vaErrorStr(s));
            vaDestroyConfig(dpy, mVppConfig);
            mVppConfig = VA_INVALID_ID;
            return false;
        }
    }

    const int width = std::min<int>(codedSize.width, visibleRect.getWidth());
    const int height = std::min<int>(codedSize.height, visibleRect.getHeight());
    VARectangle rect = {0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height)};

    VAProcPipelineParameterBuffer pipeline;
    memset(&pipeline, 0, sizeof(pipeline));
    pipeline.surface = src;
    pipeline.surface_region = &rect;
    pipeline.output_region = &rect;
    // MUST be equal, and must not be left at the memset's VAProcColorStandardNone (bd
    // remora-e5x.38). This blit exists to move and detile pixels, not to reinterpret them, but
    // "None" does not mean "no conversion" — it means "driver decides", and iHD then infers the
    // input and output standards independently and helpfully converts between them. Measured on a
    // 720p clip: every locally-decoded frame came out BT.601->BT.709, blue 41->32, red 81->62,
    // green 145->173, matching the two matrices exactly on all three primaries. Setting both to the
    // same standard makes the matrix an identity; WHICH standard is irrelevant precisely because
    // the decoder must hand back the samples the bitstream defines and let the colour aspects it
    // reports downstream say how to interpret them. The remote decoder never had this bug because
    // it memcpys the host's NV12 into the gralloc buffer and never runs a VPP.
    pipeline.surface_color_standard = VAProcColorStandardBT709;
    pipeline.output_color_standard = VAProcColorStandardBT709;

    VABufferID pipelineBuf = VA_INVALID_ID;
    VAStatus status = vaCreateBuffer(dpy, mVppContext, VAProcPipelineParameterBufferType,
                                     sizeof(pipeline), 1, &pipeline, &pipelineBuf);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer(VPP pipeline) failed: %s", vaErrorStr(status));
        return false;
    }

    bool ok = true;
    status = vaBeginPicture(dpy, mVppContext, dst);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture(VPP) failed: %s", vaErrorStr(status));
        ok = false;
    }
    if (ok) {
        status = vaRenderPicture(dpy, mVppContext, &pipelineBuf, 1);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(VPP) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    if (ok) {
        status = vaEndPicture(dpy, mVppContext);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaEndPicture(VPP) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    if (ok) {
        status = vaSyncSurface(dpy, dst);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaSyncSurface(VPP dst) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }

    vaDestroyBuffer(dpy, pipelineBuf);
    return ok;
}

bool VaapiVideoDecoder::copySurfaceToFrame(VASurfaceID srcSurface, VideoFrame* frame,
                                           const ui::Size& codedSize, const Rect& visibleRect) {
    ATRACE_CALL();
    VADisplay dpy = mDevice->display();

    VAStatus status = vaSyncSurface(dpy, srcSurface);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaSyncSurface failed: %s", vaErrorStr(status));
        return false;
    }

    // Import the destination gralloc buffer as a VA surface (with its true
    // format modifier when the gralloc handle exposes one).
    // Import at the POOL size, not the coded size: the gralloc buffer is
    // allocated at mOutputPoolSize, and its true stride/offsets follow that. A
    // smaller decoded image is copied into the top-left below (bounded by the
    // visible rect), then cropped for display. Declaring the coded size here
    // would derive a stride for the smaller width and scramble the larger buffer.
    VASurfaceID dstSurface = importFrameAsSurface(*frame, mOutputPoolSize);
    if (dstSurface == VA_INVALID_SURFACE) {
        return false;
    }

    // Blit on the GPU, always. The destination is an imported dma-buf, and a
    // CPU write through vaMapBuffer only works when the buffer was allocated
    // on this same device (VM: minigbm_intel on the xe VF) — for a foreign
    // allocation (bare: gralloc buffers come from the host via gbm_mesa) the
    // xe GEM mapping SIGBUSes on first touch (remora-e5x.20). The VPP writes
    // by DMA regardless of the exporter, and also detiles when the buffer
    // carries a tiled modifier. No CPU fallback: on the stacks where the blit
    // could fail, the fallback is the crash this replaces.
    const bool blitOk = vppBlitSurface(srcSurface, dstSurface, codedSize, visibleRect);
    vaDestroySurfaces(dpy, &dstSurface, 1);
    return blitOk;
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::setState(State newState) {
    if (mState == newState) return;
    if (mState == State::Error) return;
    ALOGV("Set state %s => %s", StateToString(mState), StateToString(newState));
    mState = newState;
}

void VaapiVideoDecoder::onError() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    setState(State::Error);
    mErrorCb.Run();
}

// static
const char* VaapiVideoDecoder::StateToString(State state) {
    switch (state) {
    case State::Idle:
        return "Idle";
    case State::Decoding:
        return "Decoding";
    case State::Draining:
        return "Draining";
    case State::Error:
        return "Error";
    }
    return "Unknown";
}

}  // namespace android
