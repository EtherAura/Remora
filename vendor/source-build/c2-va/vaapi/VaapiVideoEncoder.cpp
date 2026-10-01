// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiVideoEncoder"

#include <c2_va/vaapi/VaapiVideoEncoder.h>

#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <base/bind.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <utils/Trace.h>
#include <va/va_drmcommon.h>  // VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_vpp.h>

#include <v4l2_codec2/common/Common.h>
#include <v4l2_codec2/common/H264.h>

namespace android {
namespace {

// One-slice-per-frame encode with a single short-term reference (ip_period 1).
// These constants mirror the libva-utils hevcencode.c sample defaults.
constexpr uint32_t kNumRefFrames = 1;
constexpr uint8_t kDefaultQp = 26;
// CTB (coding tree block) size used to derive the number of CTUs in a slice. The two iHD
// encode engines are hard-wired differently: EncSlice (VME dual-pipe) is a 32x32-LCU engine,
// VDENC requires 64x64 — feeding VDENC a CTB-32 stream dies at the first vaEndPicture with
// 'internal encoding error' (bd remora-mzw.2). Resolved per-instance into mCtbSize once the
// entrypoint is known; partial edge CTBs are legal (pic sizes only need min-CB alignment), so
// either CTB size works against any coded size.
constexpr uint32_t kCtbSizeSlice = 32;  // 2^(log2_min_cb=3 + log2_diff=2 + 3) = 32
constexpr uint32_t kCtbSizeVdenc = 64;  // log2_diff = 3

// The coded-size alignment must be chosen at construction, before the VA entrypoint probe —
// read the same property the entrypoint resolution uses (default ON since the
// soak; vendor.remora.encoder_vdenc=0 is the escape hatch back to EncSlice).
static uint32_t chooseCtbSize() {
    return property_get_int32("vendor.remora.encoder_vdenc", 1) ? kCtbSizeVdenc : kCtbSizeSlice;
}

// HEVC NAL unit types (H.265 Table 7-1) used in the packed headers.
enum {
    NALU_TRAIL_R = 1,
    NALU_IDR_W_RADL = 19,
    NALU_VPS = 32,
    NALU_SPS = 33,
    NALU_PPS = 34,
};

// HEVC slice types (H.265 7.4.7.1): B=0, P=1, I=2.
enum { HEVC_SLICE_B = 0, HEVC_SLICE_P = 1, HEVC_SLICE_I = 2 };

// H.264 NAL unit types (H.264 Table 7-1) used in the packed headers.
enum {
    H264_NALU_NON_IDR = 1,
    H264_NALU_IDR = 5,
    H264_NALU_SPS = 7,
    H264_NALU_PPS = 8,
};

// H.264 slice types (H.264 Table 7-6): P=0, B=1, I=2.
enum { H264_SLICE_P = 0, H264_SLICE_B = 1, H264_SLICE_I = 2 };

// log2_max_frame_num_minus4 == 4 → 8-bit frame_num, wrapping at 256. frame_num only has to
// increment by one per reference frame modulo its range, so a GOP longer than 256 is fine.
constexpr int kH264Log2MaxFrameNumMinus4 = 4;
constexpr uint32_t kH264MaxFrameNum = 1u << (kH264Log2MaxFrameNumMinus4 + 4);

// Round |value| up to a multiple of |alignment| (a power of two).
uint32_t alignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// A minimal MSB-first bitstream writer for building packed RBSP payloads. It
// emits the NAL start code + 2-byte NAL header, then the exp-Golomb / fixed
// coded syntax elements, and finally the rbsp_trailing_bits. The VA driver adds
// emulation-prevention bytes itself (has_emulation_bytes = 0 in the packed
// header parameter buffer).
class Bitstream {
public:
    Bitstream() { mBuffer.reserve(256); }

    void putBits(uint32_t value, int numBits) {
        for (int i = numBits - 1; i >= 0; --i) {
            putBit((value >> i) & 1);
        }
    }

    void putUE(uint32_t value) {
        // Exp-Golomb unsigned.
        uint32_t codeNum = value + 1;
        int numBits = 0;
        uint32_t tmp = codeNum;
        while (tmp) {
            tmp >>= 1;
            ++numBits;
        }
        // (numBits - 1) leading zeros, then codeNum in numBits bits.
        putBits(0, numBits - 1);
        putBits(codeNum, numBits);
    }

    void putSE(int32_t value) {
        // Signed exp-Golomb: map to unsigned then putUE.
        uint32_t mapped = value <= 0 ? static_cast<uint32_t>(-2 * value)
                                     : static_cast<uint32_t>(2 * value - 1);
        putUE(mapped);
    }

    // Append the NAL start code (0x00000001) then the 2-byte HEVC NAL header for
    // |nalUnitType| (nuh_layer_id=0, nuh_temporal_id_plus1=1). Must be called on
    // a byte-aligned stream (i.e. at the very start).
    void startNAL(int nalUnitType) {
        // Start code prefix.
        putBits(0, 24);
        putBits(1, 8);
        // forbidden_zero_bit(1)=0, nal_unit_type(6), nuh_layer_id(6)=0,
        // nuh_temporal_id_plus1(3)=1.
        putBits(0, 1);
        putBits(nalUnitType, 6);
        putBits(0, 6);
        putBits(1, 3);
    }

    // The H.264 flavour: start code + the 1-byte NAL header (nal_ref_idc(2), nal_unit_type(5)).
    void startNALH264(int nalUnitType, int nalRefIdc) {
        putBits(0, 24);
        putBits(1, 8);
        putBits(0, 1);  // forbidden_zero_bit
        putBits(nalRefIdc, 2);
        putBits(nalUnitType, 5);
    }

    // rbsp_stop_one_bit + byte alignment.
    void trailingBits() {
        putBit(1);
        while (mBitsInAccum != 0) {
            putBit(0);
        }
    }

    // Byte-align WITHOUT counting the padding: an H.264 slice header ends mid-byte and the
    // driver appends slice data at bitLength() — the pad bits only exist so data() can hand
    // over whole bytes, and counting them would point the driver past the header's real end.
    // (HEVC never needs this: its slice header ends with its own byte_alignment() syntax.)
    void flushUnaligned() {
        mUnalignedBits = static_cast<uint32_t>(mBuffer.size()) * 8 + mBitsInAccum;
        while (mBitsInAccum != 0) {
            putBit(0);
        }
    }

    const uint8_t* data() const { return mBuffer.data(); }
    size_t sizeBytes() const { return mBuffer.size(); }
    // Total number of valid bits written (equals sizeBytes*8 once byte-aligned; the true
    // unpadded count after flushUnaligned()).
    uint32_t bitLength() const {
        if (mUnalignedBits) return mUnalignedBits;
        return static_cast<uint32_t>(mBuffer.size()) * 8 - mBitsInAccum;
    }

private:
    void putBit(uint32_t bit) {
        mAccum = static_cast<uint8_t>((mAccum << 1) | (bit & 1));
        if (++mBitsInAccum == 8) {
            mBuffer.push_back(mAccum);
            mAccum = 0;
            mBitsInAccum = 0;
        }
    }

    std::vector<uint8_t> mBuffer;
    uint8_t mAccum = 0;
    int mBitsInAccum = 0;
    uint32_t mUnalignedBits = 0;
};

// Emit the profile_tier_level() syntax for Main profile, no sub-layers. |tier|
// and |levelIdc| (= 30 * level) are taken from the sequence params.
void putProfileTierLevel(Bitstream* bs, uint8_t tier, uint8_t levelIdc) {
    bs->putBits(0, 2);        // general_profile_space
    bs->putBits(tier, 1);     // general_tier_flag
    bs->putBits(1, 5);        // general_profile_idc (1 = Main)
    // general_profile_compatibility_flag[32]; set bit 1 (Main) only.
    for (int i = 0; i < 32; ++i) {
        bs->putBits(i == 1 ? 1 : 0, 1);
    }
    bs->putBits(1, 1);   // general_progressive_source_flag
    bs->putBits(0, 1);   // general_interlaced_source_flag
    bs->putBits(0, 1);   // general_non_packed_constraint_flag
    bs->putBits(1, 1);   // general_frame_only_constraint_flag
    bs->putBits(0, 16);  // general_reserved_zero_44bits (upper 16)
    bs->putBits(0, 16);  // (middle 16)
    bs->putBits(0, 12);  // (lower 12)
    bs->putBits(levelIdc, 8);  // general_level_idc
}

}  // namespace

// static
std::unique_ptr<VideoEncoder> VaapiVideoEncoder::create(
        C2Config::profile_t profile, std::optional<uint8_t> level, const ui::Size& visibleSize,
        uint32_t stride, uint32_t keyFramePeriod, C2Config::bitrate_mode_t bitrateMode,
        uint32_t bitrate, std::optional<uint32_t> peakBitrate,
        FetchOutputBufferCB fetchOutputBufferCb, InputBufferDoneCB inputBufferDoneCb,
        OutputBufferDoneCB outputBufferDoneCb, DrainDoneCB drainDoneCb, ErrorCB errorCb,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner) {
    ALOGV("%s() profile=%d visible=%dx%d stride=%u keyFramePeriod=%u bitrate=%u mode=%d", __func__,
          profile, visibleSize.width, visibleSize.height, stride, keyFramePeriod, bitrate,
          bitrateMode);

    auto encoder = std::unique_ptr<VaapiVideoEncoder>(new VaapiVideoEncoder(
            profile, level, visibleSize, stride, keyFramePeriod, bitrateMode, bitrate, peakBitrate,
            std::move(fetchOutputBufferCb), std::move(inputBufferDoneCb),
            std::move(outputBufferDoneCb), std::move(drainDoneCb), std::move(errorCb),
            std::move(taskRunner)));
    if (!encoder->initialize()) {
        ALOGE("Failed to initialize VAAPI encoder for profile %d", profile);
        return nullptr;
    }
    return encoder;
}

VaapiVideoEncoder::VaapiVideoEncoder(
        C2Config::profile_t profile, std::optional<uint8_t> level, const ui::Size& visibleSize,
        uint32_t stride, uint32_t keyFramePeriod, C2Config::bitrate_mode_t bitrateMode,
        uint32_t bitrate, std::optional<uint32_t> peakBitrate,
        FetchOutputBufferCB fetchOutputBufferCb, InputBufferDoneCB inputBufferDoneCb,
        OutputBufferDoneCB outputBufferDoneCb, DrainDoneCB drainDoneCb, ErrorCB errorCb,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mProfile(profile),
        mLevel(level),
        mVisibleSize(visibleSize),
        // The VA context / surfaces are sized to a CTB-aligned coded resolution.
        mCodedSize(alignUp(visibleSize.width, chooseCtbSize()),
                   alignUp(visibleSize.height, chooseCtbSize())),
        mCtbSize(chooseCtbSize()),
        mStride(stride),
        // Honor the requested keyframe cadence (clamp 0 → 1). Inter frames were formerly disabled
        // (forced all-intra) because P-slices desynced the decoder; the real causes were the packed
        // slice header being emitted on key frames only (see renderPackedHEVCHeaders) — so P-frames
        // got no header and the driver emitted a malformed IDR-typed slice — and, in a later
        // regression, the omitted (unconditional) short_term_ref_pic_set_sps_flag. Both fixed.
        mKeyFramePeriod(keyFramePeriod == 0 ? 1 : keyFramePeriod),
        mBitrateMode(bitrateMode),
        mBitrate(bitrate),
        mPeakBitrate(peakBitrate),
        mFetchOutputBufferCb(std::move(fetchOutputBufferCb)),
        mInputBufferDoneCb(std::move(inputBufferDoneCb)),
        mOutputBufferDoneCb(std::move(outputBufferDoneCb)),
        mDrainDoneCb(std::move(drainDoneCb)),
        mErrorCb(std::move(errorCb)),
        mTaskRunner(std::move(taskRunner)) {
    mWeakThis = mWeakThisFactory.GetWeakPtr();
}

VaapiVideoEncoder::~VaapiVideoEncoder() {
    ALOGV("%s()", __func__);
    destroyVAObjects();
}

void VaapiVideoEncoder::destroyVAObjects() {
    if (!mDevice) return;
    VADisplay dpy = mDevice->display();

    if (mCodedBuffer != VA_INVALID_ID) {
        vaDestroyBuffer(dpy, mCodedBuffer);
        mCodedBuffer = VA_INVALID_ID;
    }
    for (VASurfaceID& s : mReconSurfaces) {
        if (s != VA_INVALID_SURFACE) {
            vaDestroySurfaces(dpy, &s, 1);
            s = VA_INVALID_SURFACE;
        }
    }
    if (mSrcSurface != VA_INVALID_SURFACE) {
        vaDestroySurfaces(dpy, &mSrcSurface, 1);
        mSrcSurface = VA_INVALID_SURFACE;
    }
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
}

bool VaapiVideoEncoder::initialize() {
    switch (mProfile) {
        case C2Config::PROFILE_HEVC_MAIN:
            mVAProfile = VAProfileHEVCMain;
            break;
        case C2Config::PROFILE_AVC_BASELINE:
        case C2Config::PROFILE_AVC_CONSTRAINED_BASELINE:
            mVAProfile = VAProfileH264ConstrainedBaseline;
            break;
        case C2Config::PROFILE_AVC_MAIN:
            mVAProfile = VAProfileH264Main;
            break;
        default:
            if (isH264Profile(mProfile)) {
                // High and anything above it (High10 requested at 8-bit etc.) encode as High —
                // the profile every H.264 consumer here decodes, and what the driver offers.
                mVAProfile = VAProfileH264High;
            } else {
                ALOGE("Unsupported encode profile: %d", mProfile);
                return false;
            }
            break;
    }

    mDevice = VaapiDevice::Create();
    if (!mDevice) {
        ALOGE("No VAAPI device available for encode.");
        return false;
    }
    VADisplay dpy = mDevice->display();

    // Resolve the encode entrypoint: prefer the low-power EncSliceLP (VDENC) path — it doubles
    // throughput on this VF (benchmarked 124 fps vs 62 fps EncSlice at 3760x1992, bd
    // remora-mzw.2), which is what makes a 120 Hz full-res mirror drain. VDENC's CBR/VBR rate
    // control needs the HuC the VF lacks (BRC drift = smeary inter frames — the original
    // rejection), so the LP path ALWAYS runs CQP (HuC-free; see the wantRc resolution and
    // renderRateControl). vendor.remora.encoder_vdenc=0 forces the old EncSlice+CBR path back.
    mVAEntrypoint = VAEntrypointEncSlice;
    {
        int maxEntrypoints = vaMaxNumEntrypoints(dpy);
        std::vector<VAEntrypoint> entrypoints(std::max(maxEntrypoints, 1));
        int numEntrypoints = 0;
        VAStatus s = vaQueryConfigEntrypoints(dpy, mVAProfile, entrypoints.data(), &numEntrypoints);
        bool hasSlice = false, hasSliceLP = false;
        if (s == VA_STATUS_SUCCESS) {
            for (int i = 0; i < numEntrypoints; ++i) {
                if (entrypoints[i] == VAEntrypointEncSlice) hasSlice = true;
                if (entrypoints[i] == VAEntrypointEncSliceLP) hasSliceLP = true;
            }
        }
        // Default ON since the soak (CTB-64 + GPB validated, quality signed off on
        // the live mirror). vendor.remora.encoder_vdenc=0 is the escape hatch back to
        // EncSlice+CBR if a regression ever needs the old path.
        const bool wantVdenc = property_get_int32("vendor.remora.encoder_vdenc", 1) != 0;
        if (hasSliceLP && (wantVdenc || !hasSlice)) {
            mVAEntrypoint = VAEntrypointEncSliceLP;
        } else if (!hasSlice && !hasSliceLP) {
            ALOGE("Profile %d has no encode entrypoint.", mVAProfile);
            return false;
        }
    }

    // Query the config attributes we care about: RT format, rate control,
    // packed-header support and the encode quality range.
    VAConfigAttrib queried[4];
    queried[0].type = VAConfigAttribRTFormat;
    queried[1].type = VAConfigAttribRateControl;
    queried[2].type = VAConfigAttribEncPackedHeaders;
    queried[3].type = VAConfigAttribEncQualityRange;
    VAStatus status = vaGetConfigAttributes(dpy, mVAProfile, mVAEntrypoint, queried, 4);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaGetConfigAttributes() failed: %s", vaErrorStr(status));
        return false;
    }
    if (!(queried[0].value & VA_RT_FORMAT_YUV420)) {
        ALOGE("Driver does not support YUV420 for encode.");
        return false;
    }

    // Direct-RGB encode source (LP-only): the VF's EncSliceLP advertises VA_RT_FORMAT_RGB32,
    // i.e. VDENC takes RGB32 source surfaces with driver-internal CSC. Encoding straight from
    // the imported RGBA dma-buf drops the per-frame VPP RGBA->NV12 pass and its mid-frame
    // vaSyncSurface stall (bd remora-aja). Default ON since the soak sign-off;
    // vendor.remora.encoder_rgb_direct=0 restores the VPP path. Per-frame the BO-size gate
    // (inputCoversCodedReads) still falls back to VPP for under-allocated inputs.
    mRgbDirect = mVAEntrypoint == VAEntrypointEncSliceLP &&
                 property_get_int32("vendor.remora.encoder_rgb_direct", 1) != 0 &&
                 (queried[0].value & VA_RT_FORMAT_RGB32) != 0;

    // Pipelined VPP: drop the CPU sync between the VPP and the encode submission — the driver
    // orders them on mSrcSurface, exactly as ffmpeg's filter->encode chain does — and sync once
    // before emit. Costs holding the input buffer until the encode completes. Default ON since
    // the validation + soak; vendor.remora.encoder_pipelined=0 restores the synced
    // VPP + early-release behavior.
    mPipelinedVpp = property_get_int32("vendor.remora.encoder_pipelined", 1) != 0;

    // Encode quality level (target usage): 1 = best quality/slowest, higher = faster. The default
    // is the slow best-quality TU; at a 4K mirror's high bitrate the quality delta is negligible
    // but the slow TU dominates per-frame latency, so pick the fastest the driver advertises. This
    // is a pure latency/throughput win at full resolution (no VDENC, no downscale).
    mQualityLevel = 0;  // 0 => don't send the buffer (driver default)
    if (queried[3].value != VA_ATTRIB_NOT_SUPPORTED && queried[3].value > 1) {
        mQualityLevel = queried[3].value;  // fastest supported TU
        ALOGI("Encode quality range max (fastest TU) = %u", mQualityLevel);
    }

    // Pick the rate-control mode from the requested C2 bitrate mode, intersected
    // with what the driver advertises.
    uint32_t wantRc = (mBitrateMode == C2Config::BITRATE_CONST) ? VA_RC_CBR : VA_RC_VBR;
    if (mVAEntrypoint == VAEntrypointEncSliceLP &&
        (queried[1].value == VA_ATTRIB_NOT_SUPPORTED || (queried[1].value & VA_RC_CQP))) {
        // VDENC without HuC: bitrate-based RC drifts, CQP is HuC-free and stable. The C2
        // bitrate becomes advisory on this path — the fixed QP governs size, and the LAN
        // macvlan has the headroom for content-driven spikes.
        wantRc = VA_RC_CQP;
    } else if (queried[1].value != VA_ATTRIB_NOT_SUPPORTED && !(queried[1].value & wantRc)) {
        // Fall back to whatever bitrate-based mode the driver has, else CQP.
        if (queried[1].value & VA_RC_CBR) {
            wantRc = VA_RC_CBR;
        } else if (queried[1].value & VA_RC_VBR) {
            wantRc = VA_RC_VBR;
        } else {
            ALOGW("Driver advertises no CBR/VBR; using CQP.");
            wantRc = VA_RC_CQP;
        }
    }
    mRcMode = wantRc;
    mQp = static_cast<uint8_t>(
            std::clamp(property_get_int32("vendor.remora.encoder_qp", kDefaultQp), 1, 51));
    // Sync the CTB to the resolved entrypoint (the ctor picked by property; a fallback to the
    // other engine must re-pick — partial edge CTBs keep any coded size legal either way).
    mCtbSize = (mVAEntrypoint == VAEntrypointEncSliceLP) ? kCtbSizeVdenc : kCtbSizeSlice;
    ALOGI("Encode entrypoint=%s rc=%s qp=%u ctb=%u rgbDirect=%d pipelined=%d",
          mVAEntrypoint == VAEntrypointEncSliceLP ? "EncSliceLP(VDENC)" : "EncSlice",
          wantRc == VA_RC_CQP ? "CQP" : (wantRc == VA_RC_CBR ? "CBR" : "VBR"), mQp, mCtbSize,
          mRgbDirect, mPipelinedVpp);

    mPackedHeaders = false;
    uint32_t packedMask = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE |
                          VA_ENC_PACKED_HEADER_SLICE;
    if (queried[2].value != VA_ATTRIB_NOT_SUPPORTED &&
        (queried[2].value & packedMask) == packedMask) {
        mPackedHeaders = true;
    } else {
        // Without packed headers the driver must inject VPS/SPS/PPS itself; many
        // do. We still proceed, just skipping our packed-header emission.
        ALOGW("Driver lacks full packed-header support (0x%x); relying on driver headers.",
              queried[2].value);
    }

    // Build the config. The RT-format attribute must cover every surface format the context
    // touches — the direct-RGB path adds RGB32 for its imported source surfaces.
    std::vector<VAConfigAttrib> cfgAttribs;
    cfgAttribs.push_back({VAConfigAttribRTFormat,
                          VA_RT_FORMAT_YUV420 | (mRgbDirect ? VA_RT_FORMAT_RGB32 : 0u)});
    cfgAttribs.push_back({VAConfigAttribRateControl, wantRc});
    if (mPackedHeaders) {
        cfgAttribs.push_back({VAConfigAttribEncPackedHeaders, packedMask});
    }
    status = vaCreateConfig(dpy, mVAProfile, mVAEntrypoint, cfgAttribs.data(),
                            static_cast<int>(cfgAttribs.size()), &mVAConfig);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateConfig() failed: %s", vaErrorStr(status));
        return false;
    }

    // Allocate the reconstructed (reference) surfaces and the NV12 encode source.
    VASurfaceAttrib fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = VASurfaceAttribPixelFormat;
    fmt.flags = VA_SURFACE_ATTRIB_SETTABLE;
    fmt.value.type = VAGenericValueTypeInteger;
    fmt.value.value.i = VA_FOURCC_NV12;

    status = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, mCodedSize.width, mCodedSize.height,
                              &mSrcSurface, 1, &fmt, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces(src) failed: %s", vaErrorStr(status));
        return false;
    }
    status = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, mCodedSize.width, mCodedSize.height,
                              mReconSurfaces, 2, &fmt, 1);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces(recon) failed: %s", vaErrorStr(status));
        return false;
    }

    // The context is created over the reconstructed surfaces (the render targets).
    VASurfaceID renderTargets[2] = {mReconSurfaces[0], mReconSurfaces[1]};
    status = vaCreateContext(dpy, mVAConfig, mCodedSize.width, mCodedSize.height, VA_PROGRESSIVE,
                             renderTargets, 2, &mVAContext);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateContext() failed: %s", vaErrorStr(status));
        return false;
    }

    // The coded (bitstream) buffer. Worst case ~ a few bits/pixel; the libva
    // sample uses (w*h*400)/(16*16). We add slack for headers.
    mCodedBufferSize =
            (mCodedSize.width * mCodedSize.height * 400u) / (16u * 16u) + 4096u;
    status = vaCreateBuffer(dpy, mVAContext, VAEncCodedBufferType, mCodedBufferSize, 1, nullptr,
                            &mCodedBuffer);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer(coded) failed: %s", vaErrorStr(status));
        return false;
    }

    ALOGI("VaapiVideoEncoder ready: profile=%d entrypoint=%d coded=%dx%d packedHeaders=%d",
          mVAProfile, mVAEntrypoint, mCodedSize.width, mCodedSize.height, mPackedHeaders);
    return true;
}

bool VaapiVideoEncoder::encode(std::unique_ptr<InputFrame> frame) {
    if (!frame) return false;
    // Post onto the encoder task runner; encodeTask does all VA work.
    mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(&VaapiVideoEncoder::encodeTask, mWeakThis,
                                                      std::move(frame)));
    return true;
}

void VaapiVideoEncoder::encodeTask(std::unique_ptr<InputFrame> frame) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    ATRACE_CALL();

    const uint64_t index = frame->index();
    const int64_t timestamp = frame->timestamp();
    VADisplay dpy = mDevice->display();

    // Decide the frame type for this picture.
    bool keyFrame = mForceKeyframe || (mFrameInGop % mKeyFramePeriod == 0);
    if (keyFrame) {
        mFrameInGop = 0;
        ++mIdrPicId;  // H.264: consecutive IDRs must carry different idr_pic_id values
        mForceKeyframe = false;
        mHasReference = false;  // IDR resets the DPB.
    }
    mForceKeyframe = false;
    FrameType type = keyFrame ? FrameType::IDR : FrameType::P;
    // POC step of 1 to stay consistent with the emitted short-term RPS (ref = POC-1); a step of 2
    // produced a non-conformant P-frame bitstream.
    mCurrentPoc = keyFrame ? 0 : static_cast<int32_t>(mFrameInGop);

    // Prepare the encode source. Direct-RGB: the imported RGBA dma-buf IS the source (driver
    // CSC); no VPP, no mid-frame sync. Pipelined: VPP into mSrcSurface but submit unsynced —
    // the encode is driver-ordered behind the VPP write. On both paths the input buffer stays
    // live until the encode completes, so its release moves after the final sync. Plain path:
    // import + synced VPP + immediate release, as before.
    const bool rgbDirect = useRgbDirect(*frame) && inputCoversCodedReads(*frame);
    const bool holdInput = rgbDirect || mPipelinedVpp;
    VASurfaceID importedSurface = VA_INVALID_SURFACE;
    VASurfaceID encodeSource = mSrcSurface;
    if (rgbDirect) {
        importedSurface = importInputAsSurface(*frame, /*atCodedHeight=*/true);
        if (importedSurface == VA_INVALID_SURFACE) {
            ALOGE("Failed to import direct-RGB source for frame %llu",
                  static_cast<unsigned long long>(index));
            mInputBufferDoneCb.Run(index);
            onError();
            return;
        }
        encodeSource = importedSurface;
    } else {
        if (!prepareSourceSurface(*frame, &importedSurface, /*syncVpp=*/!mPipelinedVpp)) {
            ALOGE("Failed to prepare source surface for frame %llu",
                  static_cast<unsigned long long>(index));
            if (importedSurface != VA_INVALID_SURFACE) vaDestroySurfaces(dpy, &importedSurface, 1);
            // We own the input; release it and report the error.
            mInputBufferDoneCb.Run(index);
            onError();
            return;
        }

        if (!mPipelinedVpp) {
            // The input dma-buf is fully consumed once the VPP copy into mSrcSurface has synced
            // (inside prepareSourceSurface), so release it to the producer NOW — holding it until
            // the encode finished starved the BufferQueue and halved the frame rate (producer had
            // nothing to render into while we encoded).
            if (importedSurface != VA_INVALID_SURFACE) {
                vaDestroySurfaces(dpy, &importedSurface, 1);
                importedSurface = VA_INVALID_SURFACE;
            }
            mInputBufferDoneCb.Run(index);
        }
    }

    // The reconstructed surface for this picture (ping-pong with the previous).
    VASurfaceID reconSurface = mReconSurfaces[mFrameInGop % 2];

    std::vector<VABufferID> buffers;
    bool ok = true;

    VAStatus status = vaBeginPicture(dpy, mVAContext, encodeSource);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture() failed: %s", vaErrorStr(status));
        ok = false;
    }

    // Sequence (key frames) → rate control → packed headers → picture → slice, per codec.
    if (ok) ok = renderFrameParams(type, keyFrame, reconSurface, &buffers);

    if (ok) {
        status = vaEndPicture(dpy, mVAContext);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaEndPicture() failed: %s", vaErrorStr(status));
            ok = false;
        }
    }

    // The param buffers are consumed by EndPicture; destroy our references.
    destroyBuffers(&buffers);
    // (On the plain VPP path the imported surface + input buffer were already released after the
    // VPP sync; on the direct-RGB/pipelined paths they are released below, once the encode is
    // final.)

    if (!ok) {
        if (holdInput) releaseDirectInput(&importedSurface, index);
        onError();
        return;
    }

    // Wait for the encode to complete and emit the coded bitstream. On the pipelined path this
    // single sync covers the VPP write and the encode that follows it.
    status = vaSyncSurface(dpy, encodeSource);
    // Success or failure, the GPU is done with the input dma-buf — hand it back now.
    if (holdInput) releaseDirectInput(&importedSurface, index);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaSyncSurface() failed: %s", vaErrorStr(status));
        onError();
        return;
    }

    if (!emitCodedBuffer(timestamp, keyFrame)) {
        onError();
        return;
    }

    // Update the reference for the next P frame and advance the GOP counter.
    mRefSurface = reconSurface;
    mRefPoc = mCurrentPoc;
    mHasReference = true;
    ++mFrameInGop;
}

bool VaapiVideoEncoder::prepareSourceSurface(const InputFrame& frame,
                                             VASurfaceID* importedSurface, bool syncVpp) {
    *importedSurface = VA_INVALID_SURFACE;

    VASurfaceID input = importInputAsSurface(frame);
    if (input == VA_INVALID_SURFACE) {
        return false;
    }

    if (frame.pixelFormat() == VideoPixelFormat::NV12) {
        // Already NV12: VPP-copy into the encode-source surface to normalize the
        // layout to the driver-preferred tiling. (A blit is cheaper than a full
        // colour conversion and guarantees the encoder reads a well-formed
        // surface even when the gralloc buffer is tiled/padded.)
        if (!convertToNV12(input, syncVpp)) {
            *importedSurface = input;
            return false;
        }
    } else {
        // RGBA / other: colour-convert to NV12 via VPP.
        if (!convertToNV12(input, syncVpp)) {
            *importedSurface = input;
            return false;
        }
    }

    *importedSurface = input;
    return true;
}

bool VaapiVideoEncoder::useRgbDirect(const InputFrame& frame) const {
    if (!mRgbDirect) return false;
    switch (frame.pixelFormat()) {
        case VideoPixelFormat::RGBA:
        case VideoPixelFormat::BGRA:
        case VideoPixelFormat::ARGB:
        case VideoPixelFormat::XRGB:
        case VideoPixelFormat::ABGR:
        case VideoPixelFormat::XBGR:
            return true;
        default:
            // NV12 (or anything else) still takes the VPP path.
            return false;
    }
}

// dma-buf fds support lseek; probe the BO size and restore the offset for any
// offset-sensitive sharer of the same file description.
static off_t dmabufSize(int fd) {
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return size;
}

bool VaapiVideoEncoder::inputCoversCodedReads(const InputFrame& frame) {
    const std::vector<int>& fds = frame.fds();
    const std::vector<VideoFramePlane>& planes = frame.planes();
    if (fds.empty() || planes.empty()) return false;
    const off_t boSize = dmabufSize(fds[0]);
    if (boSize <= 0) return false;
    const uint64_t pitch = planes[0].mStride;
    const uint64_t need = planes[0].mOffset +
                          static_cast<uint64_t>(mCodedSize.height - 1) * pitch +
                          static_cast<uint64_t>(mCodedSize.width) * 4;
    if (static_cast<uint64_t>(boSize) >= need) return true;
    if (!mWarnedShortInput) {
        mWarnedShortInput = true;
        ALOGW("Direct-RGB input BO too small for the VDENC read footprint (%lld < %llu, "
              "coded %ux%u pitch %llu) — taking the VPP path for such frames.",
              static_cast<long long>(boSize), static_cast<unsigned long long>(need),
              mCodedSize.width, mCodedSize.height, static_cast<unsigned long long>(pitch));
    }
    return false;
}

void VaapiVideoEncoder::releaseDirectInput(VASurfaceID* surface, uint64_t index) {
    if (*surface != VA_INVALID_SURFACE) {
        vaDestroySurfaces(mDevice->display(), surface, 1);
        *surface = VA_INVALID_SURFACE;
    }
    mInputBufferDoneCb.Run(index);
}

VASurfaceID VaapiVideoEncoder::importInputAsSurface(const InputFrame& frame, bool atCodedHeight) {
    VADisplay dpy = mDevice->display();
    const std::vector<int>& fds = frame.fds();
    const std::vector<VideoFramePlane>& planes = frame.planes();
    if (fds.empty() || planes.empty()) {
        ALOGE("InputFrame has no dma-buf fds/planes.");
        return VA_INVALID_SURFACE;
    }

    uint32_t fourcc;
    uint32_t rtFormat;
    switch (frame.pixelFormat()) {
        case VideoPixelFormat::NV12:
            fourcc = VA_FOURCC_NV12;
            rtFormat = VA_RT_FORMAT_YUV420;
            break;
        case VideoPixelFormat::RGBA:
            fourcc = VA_FOURCC_RGBA;
            rtFormat = VA_RT_FORMAT_RGB32;
            break;
        case VideoPixelFormat::BGRA:
            fourcc = VA_FOURCC_BGRA;
            rtFormat = VA_RT_FORMAT_RGB32;
            break;
        // The agent's virtual-display surface reports VideoPixelFormat::ARGB. Empirically (screencap
        // vs our output), the memory is R,G,B,A, so it must be imported as VA_FOURCC_RGBA — labelling
        // it BGRA swapped R<->B. The X* variants and ABGR are the mirror cases.
        case VideoPixelFormat::ARGB:
        case VideoPixelFormat::XRGB:
            fourcc = VA_FOURCC_RGBA;
            rtFormat = VA_RT_FORMAT_RGB32;
            break;
        case VideoPixelFormat::ABGR:
        case VideoPixelFormat::XBGR:
            fourcc = VA_FOURCC_BGRA;
            rtFormat = VA_RT_FORMAT_RGB32;
            break;
        default:
            ALOGE("Unsupported input pixel format %d for VA import.",
                  static_cast<int>(frame.pixelFormat()));
            return VA_INVALID_SURFACE;
    }

    // Direct-encode sources are imported at the CTB-aligned coded height with
    // the BO's real size, so the surface honestly describes the rows the VDENC
    // walker reads (the gate in inputCoversCodedReads verified they exist).
    const uint32_t importHeight = atCodedHeight ? mCodedSize.height : mVisibleSize.height;

    uintptr_t buffers[1] = {static_cast<uintptr_t>(fds[0])};
    VASurfaceAttribExternalBuffers ext;
    memset(&ext, 0, sizeof(ext));
    ext.pixel_format = fourcc;
    ext.width = mVisibleSize.width;
    ext.height = importHeight;
    ext.num_planes = static_cast<uint32_t>(planes.size());
    for (size_t i = 0; i < planes.size() && i < 3; ++i) {
        ext.pitches[i] = planes[i].mStride;
        ext.offsets[i] = static_cast<uint32_t>(planes[i].mOffset);
    }
    // data_size: for single-fd buffers cover the whole surface.
    if (atCodedHeight) {
        const off_t boSize = dmabufSize(fds[0]);
        ext.data_size = boSize > 0 ? static_cast<uint32_t>(boSize)
                                   : planes[0].mStride * importHeight;
    } else if (frame.pixelFormat() == VideoPixelFormat::NV12) {
        ext.data_size = planes[0].mStride * mVisibleSize.height * 3 / 2;
    } else {
        ext.data_size = planes[0].mStride * mVisibleSize.height;
    }
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

    VASurfaceID surface = VA_INVALID_SURFACE;
    VAStatus status = vaCreateSurfaces(dpy, rtFormat, mVisibleSize.width, importHeight,
                                       &surface, 1, attribs, 2);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces(import) failed: %s", vaErrorStr(status));
        return VA_INVALID_SURFACE;
    }
    return surface;
}

bool VaapiVideoEncoder::convertToNV12(VASurfaceID src, bool syncAfter) {
    // Use a dedicated VPP config/context to scale-and-convert |src| into the
    // NV12 mSrcSurface. We create these lazily and tear them down each call to
    // keep the encoder state simple; the conversion is cheap relative to encode.
    VADisplay dpy = mDevice->display();

    VABufferID pipelineBuf = VA_INVALID_ID;
    bool ok = true;

    // The VPP config/context are created once (initialize) and cached: creating them per frame
    // cost 10-20 ms of the ~30 ms frame budget (measured), capping the mirror around 15 fps.
    if (mVppContext == VA_INVALID_ID) {
        VAStatus s = vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                    &mVppConfig);
        if (s != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateConfig(VPP) failed: %s", vaErrorStr(s));
            return false;
        }
        s = vaCreateContext(dpy, mVppConfig, mCodedSize.width, mCodedSize.height, VA_PROGRESSIVE,
                            &mSrcSurface, 1, &mVppContext);
        if (s != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateContext(VPP) failed: %s", vaErrorStr(s));
            vaDestroyConfig(dpy, mVppConfig);
            mVppConfig = VA_INVALID_ID;
            return false;
        }
    }
    VAContextID vppContext = mVppContext;
    VAStatus status = VA_STATUS_SUCCESS;

    VAProcPipelineParameterBuffer pipeline;
    memset(&pipeline, 0, sizeof(pipeline));
    VARectangle srcRect = {0, 0, static_cast<uint16_t>(mVisibleSize.width),
                           static_cast<uint16_t>(mVisibleSize.height)};
    VARectangle dstRect = {0, 0, static_cast<uint16_t>(mVisibleSize.width),
                           static_cast<uint16_t>(mVisibleSize.height)};
    pipeline.surface = src;
    pipeline.surface_region = &srcRect;
    pipeline.output_region = &dstRect;
    pipeline.filter_flags = VA_FILTER_SCALING_HQ;

    status = vaCreateBuffer(dpy, vppContext, VAProcPipelineParameterBufferType, sizeof(pipeline), 1,
                            &pipeline, &pipelineBuf);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer(VPP pipeline) failed: %s", vaErrorStr(status));
        ok = false;
    }

    if (ok) {
        status = vaBeginPicture(dpy, vppContext, mSrcSurface);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaBeginPicture(VPP) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    if (ok) {
        status = vaRenderPicture(dpy, vppContext, &pipelineBuf, 1);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(VPP) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    if (ok) {
        status = vaEndPicture(dpy, vppContext);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaEndPicture(VPP) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    // Pipelined mode skips this sync: the driver orders the encode submission after the VPP
    // write on mSrcSurface (the same cross-context dependency ffmpeg's filter->encode chain
    // relies on), and the single sync before emit covers both.
    if (ok && syncAfter) {
        status = vaSyncSurface(dpy, mSrcSurface);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaSyncSurface(VPP) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }

    if (pipelineBuf != VA_INVALID_ID) vaDestroyBuffer(dpy, pipelineBuf);
    return ok;
}

void VaapiVideoEncoder::buildHEVCSequenceParam(FrameType type,
                                               VAEncSequenceParameterBufferHEVC* seq) const {
    memset(seq, 0, sizeof(*seq));
    seq->general_profile_idc = 1;   // Main.
    // Pick a tier + level that actually admits our peak bitrate and luma sample rate. The Main tier
    // caps bitrate hard (only 40 Mbps at level 5.1); a 150 Mbps daily-driver target blew ~4x past
    // that ceiling and the driver's rate control collapsed to a high fixed QP — sharp at low
    // bitrates, blurry (and worse under motion) above the ceiling. Advertise the High tier and the
    // lowest level whose High-tier limits cover the peak bitrate (which also bounds the CPB) and the
    // resolution × frame-rate sample throughput. Levels ×10: 5.1/5.2/6.1/6.2. High-tier max bitrate
    // = 160/240/480/800 Mbps; max luma sample rate = 534.8/1069.5/2139.1/4278.2 Msample/s.
    seq->general_tier_flag = 1;  // High tier.
    const uint32_t peakBps = mPeakBitrate.value_or(mBitrate);
    const uint64_t sampleRate = static_cast<uint64_t>(mCodedSize.width) *
                                static_cast<uint64_t>(mCodedSize.height) *
                                (mFramerate ? mFramerate : 30u);
    uint8_t lvlx10 = 51;
    auto atLeast = [&](uint8_t v) { if (lvlx10 < v) lvlx10 = v; };
    if (peakBps > 160000000u) atLeast(52);
    if (peakBps > 240000000u) atLeast(61);
    if (peakBps > 480000000u) atLeast(62);
    if (sampleRate > 534773760ull) atLeast(52);
    if (sampleRate > 1069547520ull) atLeast(61);
    if (sampleRate > 2139095040ull) atLeast(62);
    const uint8_t level = mLevel.value_or(lvlx10);
    seq->general_level_idc = static_cast<uint8_t>(level * 30 / 10);

    seq->intra_period = mKeyFramePeriod;
    seq->intra_idr_period = mKeyFramePeriod;
    seq->ip_period = 1;  // No B-frames.
    seq->bits_per_second = mBitrate;

    seq->pic_width_in_luma_samples = static_cast<uint16_t>(mCodedSize.width);
    seq->pic_height_in_luma_samples = static_cast<uint16_t>(mCodedSize.height);

    seq->seq_fields.bits.chroma_format_idc = 1;  // 4:2:0.
    seq->seq_fields.bits.separate_colour_plane_flag = 0;
    seq->seq_fields.bits.bit_depth_luma_minus8 = 0;
    seq->seq_fields.bits.bit_depth_chroma_minus8 = 0;
    seq->seq_fields.bits.scaling_list_enabled_flag = 0;
    seq->seq_fields.bits.strong_intra_smoothing_enabled_flag = 0;
    seq->seq_fields.bits.amp_enabled_flag = 1;
    seq->seq_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    seq->seq_fields.bits.pcm_enabled_flag = 0;
    seq->seq_fields.bits.pcm_loop_filter_disabled_flag = 0;
    seq->seq_fields.bits.sps_temporal_mvp_enabled_flag = 0;

    // Min CB 8x8 (log2_min_cb=3 → minus3=0); max = the engine's CTB (32 EncSlice / 64 VDENC).
    seq->log2_min_luma_coding_block_size_minus3 = 0;
    seq->log2_diff_max_min_luma_coding_block_size = (mCtbSize == kCtbSizeVdenc) ? 3 : 2;
    // TB: min 4x4 (log2=2 → minus2=0), max 32x32 (diff=3).
    seq->log2_min_transform_block_size_minus2 = 0;
    seq->log2_diff_max_min_transform_block_size = 3;
    seq->max_transform_hierarchy_depth_inter = 2;
    seq->max_transform_hierarchy_depth_intra = 2;

    seq->vui_parameters_present_flag = 0;
    (void)type;
}

void VaapiVideoEncoder::buildHEVCPictureParam(FrameType type,
                                              VAEncPictureParameterBufferHEVC* pic) const {
    memset(pic, 0, sizeof(*pic));

    // Reference list: for a P frame the single short-term reference precedes the
    // current picture in POC. IDR frames have none.
    for (int i = 0; i < 15; ++i) {
        pic->reference_frames[i].picture_id = VA_INVALID_SURFACE;
        pic->reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    if (type == FrameType::P && mHasReference) {
        pic->reference_frames[0].picture_id = mRefSurface;
        pic->reference_frames[0].pic_order_cnt = mRefPoc;
        pic->reference_frames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    }

    pic->decoded_curr_pic.picture_id = VA_INVALID_SURFACE;  // filled by caller.
    pic->decoded_curr_pic.pic_order_cnt = mCurrentPoc;
    pic->decoded_curr_pic.flags = 0;

    pic->coded_buf = VA_INVALID_ID;  // filled by caller.
    pic->collocated_ref_pic_index = 0xFF;
    pic->last_picture = 0;
    pic->pic_init_qp = mQp;
    pic->diff_cu_qp_delta_depth = 0;
    pic->pps_cb_qp_offset = 0;
    pic->pps_cr_qp_offset = 0;
    pic->num_tile_columns_minus1 = 0;
    pic->num_tile_rows_minus1 = 0;
    pic->log2_parallel_merge_level_minus2 = 0;
    pic->ctu_max_bitsize_allowed = 0;
    pic->num_ref_idx_l0_default_active_minus1 = 0;
    pic->num_ref_idx_l1_default_active_minus1 = 0;
    pic->slice_pic_parameter_set_id = 0;
    pic->nal_unit_type = (type == FrameType::IDR) ? NALU_IDR_W_RADL : NALU_TRAIL_R;

    pic->pic_fields.bits.idr_pic_flag = (type == FrameType::IDR) ? 1 : 0;
    // I=1, P=2, B=3. The VDENC (EncSliceLP) path has no P-slices — inter pictures are GPB
    // (generalized P-B): coding_type B with both reference lists pointing at the same past frame.
    pic->pic_fields.bits.coding_type =
            (type == FrameType::IDR) ? 1 : (mVAEntrypoint == VAEntrypointEncSliceLP ? 3 : 2);
    pic->pic_fields.bits.reference_pic_flag = 1;  // I and P are references here.
    pic->pic_fields.bits.dependent_slice_segments_enabled_flag = 0;
    pic->pic_fields.bits.sign_data_hiding_enabled_flag = 0;
    pic->pic_fields.bits.constrained_intra_pred_flag = 0;
    pic->pic_fields.bits.transform_skip_enabled_flag = 0;
    // Block-level QP adjustment is a BRC feature: with CQP (the VDENC path) iHD rejects the
    // combination — vaEndPicture dies with 'internal encoding error' (bd remora-mzw.2; ffmpeg's
    // working CQP path gates this off the same way). The packed PPS mirrors this field.
    pic->pic_fields.bits.cu_qp_delta_enabled_flag = (mRcMode == VA_RC_CQP) ? 0 : 1;
    pic->pic_fields.bits.weighted_pred_flag = 0;
    pic->pic_fields.bits.weighted_bipred_flag = 0;
    pic->pic_fields.bits.transquant_bypass_enabled_flag = 0;
    pic->pic_fields.bits.tiles_enabled_flag = 0;
    pic->pic_fields.bits.entropy_coding_sync_enabled_flag = 0;
    pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag = 0;
    pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    pic->pic_fields.bits.scaling_list_data_present_flag = 0;
    pic->pic_fields.bits.screen_content_flag = 0;
    pic->pic_fields.bits.enable_gpu_weighted_prediction = 0;
    pic->pic_fields.bits.no_output_of_prior_pics_flag = 0;
}

void VaapiVideoEncoder::buildHEVCSliceParam(FrameType type,
                                            const VAEncPictureParameterBufferHEVC& pic,
                                            VAEncSliceParameterBufferHEVC* slice) const {
    memset(slice, 0, sizeof(*slice));

    const uint32_t ctuCols = alignUp(mCodedSize.width, mCtbSize) / mCtbSize;
    const uint32_t ctuRows = alignUp(mCodedSize.height, mCtbSize) / mCtbSize;

    slice->slice_segment_address = 0;
    slice->num_ctu_in_slice = ctuCols * ctuRows;
    // VDENC inter = GPB B-slice (see buildHEVCPictureParam); EncSlice inter = plain P.
    const bool gpb = (type == FrameType::P) && (mVAEntrypoint == VAEntrypointEncSliceLP);
    slice->slice_type =
            (type == FrameType::IDR) ? HEVC_SLICE_I : (gpb ? HEVC_SLICE_B : HEVC_SLICE_P);
    slice->slice_pic_parameter_set_id = 0;

    slice->num_ref_idx_l0_active_minus1 = 0;
    slice->num_ref_idx_l1_active_minus1 = 0;
    memset(slice->ref_pic_list0, 0xff, sizeof(slice->ref_pic_list0));
    memset(slice->ref_pic_list1, 0xff, sizeof(slice->ref_pic_list1));
    if (type == FrameType::P) {
        slice->ref_pic_list0[0] = pic.reference_frames[0];
        // GPB: list 1 mirrors list 0 (same single past reference).
        if (gpb) slice->ref_pic_list1[0] = pic.reference_frames[0];
    }

    slice->luma_log2_weight_denom = 0;
    slice->delta_chroma_log2_weight_denom = 0;
    slice->max_num_merge_cand = 5;
    slice->slice_qp_delta = 0;
    slice->slice_cb_qp_offset = 0;
    slice->slice_cr_qp_offset = 0;
    slice->slice_beta_offset_div2 = 0;
    slice->slice_tc_offset_div2 = 0;

    slice->slice_fields.bits.last_slice_of_pic_flag = 1;
    slice->slice_fields.bits.dependent_slice_segment_flag = 0;
    slice->slice_fields.bits.colour_plane_id = 0;
    slice->slice_fields.bits.slice_temporal_mvp_enabled_flag = 0;
    slice->slice_fields.bits.slice_sao_luma_flag = 0;
    slice->slice_fields.bits.slice_sao_chroma_flag = 0;
    slice->slice_fields.bits.num_ref_idx_active_override_flag = (type == FrameType::P) ? 1 : 0;
    slice->slice_fields.bits.mvd_l1_zero_flag = 0;
    slice->slice_fields.bits.cabac_init_flag = 0;
    slice->slice_fields.bits.slice_deblocking_filter_disabled_flag = 0;
    slice->slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
    slice->slice_fields.bits.collocated_from_l0_flag = 1;
}

bool VaapiVideoEncoder::renderPackedHEVCHeaders(const VAEncSequenceParameterBufferHEVC& seq,
                                                const VAEncPictureParameterBufferHEVC& pic,
                                                const VAEncSliceParameterBufferHEVC& slice,
                                                FrameType type, bool includeParamSets,
                                                std::vector<VABufferID>* buffers) {
    VADisplay dpy = mDevice->display();

    auto emit = [&](VAEncPackedHeaderType headerType, const Bitstream& bs) -> bool {
        VAEncPackedHeaderParameterBuffer param;
        memset(&param, 0, sizeof(param));
        param.type = headerType;
        param.bit_length = bs.bitLength();
        param.has_emulation_bytes = 0;  // driver inserts EPB.

        VABufferID paramBuf = VA_INVALID_ID;
        VABufferID dataBuf = VA_INVALID_ID;
        VAStatus status = vaCreateBuffer(dpy, mVAContext, VAEncPackedHeaderParameterBufferType,
                                         sizeof(param), 1, &param, &paramBuf);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(packed param) failed: %s", vaErrorStr(status));
            return false;
        }
        buffers->push_back(paramBuf);
        status = vaCreateBuffer(dpy, mVAContext, VAEncPackedHeaderDataBufferType,
                                static_cast<uint32_t>(bs.sizeBytes()), 1,
                                const_cast<uint8_t*>(bs.data()), &dataBuf);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(packed data) failed: %s", vaErrorStr(status));
            return false;
        }
        buffers->push_back(dataBuf);

        VABufferID renderIds[2] = {paramBuf, dataBuf};
        status = vaRenderPicture(dpy, mVAContext, renderIds, 2);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(packed) failed: %s", vaErrorStr(status));
            return false;
        }
        return true;
    };

    // ---- VPS ---- (parameter sets: key frames only)
    if (includeParamSets) {
        Bitstream bs;
        bs.startNAL(NALU_VPS);
        bs.putBits(0, 4);   // vps_video_parameter_set_id
        bs.putBits(3, 2);   // vps_reserved_three_2bits
        bs.putBits(0, 6);   // vps_reserved_zero_6bits
        bs.putBits(0, 3);   // vps_max_sub_layers_minus1
        bs.putBits(1, 1);   // vps_temporal_id_nesting_flag
        bs.putBits(0xFFFF, 16);  // vps_reserved_0xffff_16bits
        putProfileTierLevel(&bs, seq.general_tier_flag, seq.general_level_idc);
        bs.putBits(0, 1);   // vps_sub_layer_ordering_info_present_flag
        bs.putUE(1);        // vps_max_dec_pic_buffering_minus1[0]
        bs.putUE(0);        // vps_max_num_reorder_pics[0]
        bs.putUE(0);        // vps_max_latency_increase_plus1[0]
        bs.putBits(0, 6);   // vps_max_nuh_reserved_zero_layer_id
        bs.putUE(0);        // vps_num_op_sets_minus1
        bs.putBits(0, 1);   // vps_timing_info_present_flag
        bs.putBits(0, 1);   // vps_extension_flag
        bs.trailingBits();
        if (!emit(VAEncPackedHeaderSequence, bs)) return false;
    }

    // ---- SPS ----
    if (includeParamSets) {
        Bitstream bs;
        bs.startNAL(NALU_SPS);
        bs.putBits(0, 4);   // sps_video_parameter_set_id
        bs.putBits(0, 3);   // sps_max_sub_layers_minus1
        bs.putBits(1, 1);   // sps_temporal_id_nesting_flag
        putProfileTierLevel(&bs, seq.general_tier_flag, seq.general_level_idc);
        bs.putUE(0);  // sps_seq_parameter_set_id
        bs.putUE(seq.seq_fields.bits.chroma_format_idc);
        bs.putUE(seq.pic_width_in_luma_samples);
        bs.putUE(seq.pic_height_in_luma_samples);
        // conformance_window: crop to the visible size if the coded size differs.
        bool conf = (mCodedSize.width != mVisibleSize.width) ||
                    (mCodedSize.height != mVisibleSize.height);
        bs.putBits(conf ? 1 : 0, 1);
        if (conf) {
            // Offsets are in chroma sample units (SubWidthC=SubHeightC=2 for 4:2:0).
            bs.putUE(0);  // conf_win_left_offset
            bs.putUE((mCodedSize.width - mVisibleSize.width) / 2);   // right
            bs.putUE(0);  // top
            bs.putUE((mCodedSize.height - mVisibleSize.height) / 2);  // bottom
        }
        bs.putUE(seq.seq_fields.bits.bit_depth_luma_minus8);
        bs.putUE(seq.seq_fields.bits.bit_depth_chroma_minus8);
        bs.putUE(kLog2MaxPocLsbMinus4);  // log2_max_pic_order_cnt_lsb_minus4
        bs.putBits(0, 1);   // sps_sub_layer_ordering_info_present_flag
        bs.putUE(1);        // sps_max_dec_pic_buffering_minus1[0]
        bs.putUE(0);        // sps_max_num_reorder_pics[0]
        bs.putUE(0);        // sps_max_latency_increase_plus1[0]
        bs.putUE(seq.log2_min_luma_coding_block_size_minus3);
        bs.putUE(seq.log2_diff_max_min_luma_coding_block_size);
        bs.putUE(seq.log2_min_transform_block_size_minus2);
        bs.putUE(seq.log2_diff_max_min_transform_block_size);
        bs.putUE(seq.max_transform_hierarchy_depth_inter);
        bs.putUE(seq.max_transform_hierarchy_depth_intra);
        bs.putBits(seq.seq_fields.bits.scaling_list_enabled_flag, 1);
        bs.putBits(seq.seq_fields.bits.amp_enabled_flag, 1);
        bs.putBits(seq.seq_fields.bits.sample_adaptive_offset_enabled_flag, 1);
        bs.putBits(seq.seq_fields.bits.pcm_enabled_flag, 1);
        bs.putUE(0);        // num_short_term_ref_pic_sets
        bs.putBits(0, 1);   // long_term_ref_pics_present_flag
        bs.putBits(seq.seq_fields.bits.sps_temporal_mvp_enabled_flag, 1);
        bs.putBits(seq.seq_fields.bits.strong_intra_smoothing_enabled_flag, 1);
        bs.putBits(0, 1);   // vui_parameters_present_flag
        bs.putBits(0, 1);   // sps_extension_present_flag
        bs.trailingBits();
        if (!emit(VAEncPackedHeaderSequence, bs)) return false;
    }

    // ---- PPS ----
    if (includeParamSets) {
        Bitstream bs;
        bs.startNAL(NALU_PPS);
        bs.putUE(0);  // pps_pic_parameter_set_id
        bs.putUE(0);  // pps_seq_parameter_set_id
        bs.putBits(pic.pic_fields.bits.dependent_slice_segments_enabled_flag, 1);
        bs.putBits(0, 1);   // output_flag_present_flag
        bs.putBits(0, 3);   // num_extra_slice_header_bits
        bs.putBits(pic.pic_fields.bits.sign_data_hiding_enabled_flag, 1);
        bs.putBits(0, 1);   // cabac_init_present_flag
        bs.putUE(pic.num_ref_idx_l0_default_active_minus1);
        bs.putUE(pic.num_ref_idx_l1_default_active_minus1);
        bs.putSE(pic.pic_init_qp - 26);  // init_qp_minus26
        bs.putBits(pic.pic_fields.bits.constrained_intra_pred_flag, 1);
        bs.putBits(pic.pic_fields.bits.transform_skip_enabled_flag, 1);
        bs.putBits(pic.pic_fields.bits.cu_qp_delta_enabled_flag, 1);
        if (pic.pic_fields.bits.cu_qp_delta_enabled_flag) {
            bs.putUE(pic.diff_cu_qp_delta_depth);
        }
        bs.putSE(pic.pps_cb_qp_offset);
        bs.putSE(pic.pps_cr_qp_offset);
        bs.putBits(0, 1);   // pps_slice_chroma_qp_offsets_present_flag
        bs.putBits(pic.pic_fields.bits.weighted_pred_flag, 1);
        bs.putBits(pic.pic_fields.bits.weighted_bipred_flag, 1);
        bs.putBits(pic.pic_fields.bits.transquant_bypass_enabled_flag, 1);
        bs.putBits(pic.pic_fields.bits.tiles_enabled_flag, 1);
        bs.putBits(pic.pic_fields.bits.entropy_coding_sync_enabled_flag, 1);
        bs.putBits(pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag, 1);
        bs.putBits(0, 1);   // deblocking_filter_control_present_flag
        bs.putBits(pic.pic_fields.bits.scaling_list_data_present_flag, 1);
        bs.putBits(0, 1);   // lists_modification_present_flag
        bs.putUE(pic.log2_parallel_merge_level_minus2);
        bs.putBits(0, 1);   // slice_segment_header_extension_present_flag
        bs.putBits(0, 1);   // pps_extension_present_flag
        bs.trailingBits();
        if (!emit(VAEncPackedHeaderPicture, bs)) return false;
    }

    // ---- Slice header ----
    {
        Bitstream bs;
        int nalUnitType = (type == FrameType::IDR) ? NALU_IDR_W_RADL : NALU_TRAIL_R;
        bs.startNAL(nalUnitType);
        bs.putBits(1, 1);  // first_slice_segment_in_pic_flag
        if (nalUnitType >= 16 && nalUnitType <= 23) {
            bs.putBits(0, 1);  // no_output_of_prior_pics_flag
        }
        bs.putUE(0);  // slice_pic_parameter_set_id
        // first_slice_segment_in_pic_flag == 1: no slice_segment_address.
        bs.putUE(slice.slice_type);
        if (nalUnitType != NALU_IDR_W_RADL) {
            // Non-IDR: POC lsb, then short_term_ref_pic_set_sps_flag (UNCONDITIONAL for non-IDR
            // slices per HEVC 7.3.6.1 — it is NOT gated on num_short_term_ref_pic_sets), then the
            // inline st_ref_pic_set since the flag is 0.
            bs.putBits(mCurrentPoc & (maxPocLsb() - 1), kLog2MaxPocLsbMinus4 + 4);
            bs.putBits(0, 1);  // short_term_ref_pic_set_sps_flag → inline RPS follows
            // Inline st_ref_pic_set(0): one negative pic, referencing POC-1 (matches mRefPoc).
            bs.putUE(1);       // num_negative_pics
            bs.putUE(0);       // num_positive_pics
            bs.putUE(0);       // delta_poc_s0_minus1[0]  (delta 1 → references POC-1)
            bs.putBits(1, 1);  // used_by_curr_pic_s0_flag[0]
            if (0 /*sps_temporal_mvp_enabled_flag*/) {
                bs.putBits(0, 1);  // slice_temporal_mvp_enabled_flag
            }
        }
        // sample_adaptive_offset disabled → no slice_sao flags.
        if (slice.slice_type != HEVC_SLICE_I) {
            bs.putBits(slice.slice_fields.bits.num_ref_idx_active_override_flag, 1);
            if (slice.slice_fields.bits.num_ref_idx_active_override_flag) {
                bs.putUE(slice.num_ref_idx_l0_active_minus1);
                if (slice.slice_type == HEVC_SLICE_B) {
                    bs.putUE(slice.num_ref_idx_l1_active_minus1);
                }
            }
            // ref_pic_lists_modification omitted (lists_modification_present 0).
            if (slice.slice_type == HEVC_SLICE_B) {
                bs.putBits(slice.slice_fields.bits.mvd_l1_zero_flag, 1);
            }
            // cabac_init omitted (cabac_init_present_flag == 0 in PPS).
            // five_minus_max_num_merge_cand.
            bs.putUE(5 - slice.max_num_merge_cand);
        }
        bs.putSE(slice.slice_qp_delta);
        // deblocking_filter_control_present_flag == 0 in PPS → nothing.
        // pps_loop_filter_across_slices_enabled_flag == 1 → slice flag present.
        bs.putBits(slice.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag, 1);
        bs.trailingBits();
        if (!emit(VAEncPackedHeaderSlice, bs)) return false;
    }

    return true;
}

bool VaapiVideoEncoder::renderRateControl(std::vector<VABufferID>* buffers) {
    VADisplay dpy = mDevice->display();

    auto renderMisc = [&](VAEncMiscParameterType type, const void* payload,
                          size_t payloadSize) -> bool {
        VABufferID buf = VA_INVALID_ID;
        size_t total = sizeof(VAEncMiscParameterBuffer) + payloadSize;
        VAStatus status = vaCreateBuffer(dpy, mVAContext, VAEncMiscParameterBufferType, total, 1,
                                         nullptr, &buf);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(misc) failed: %s", vaErrorStr(status));
            return false;
        }
        buffers->push_back(buf);
        VAEncMiscParameterBuffer* misc = nullptr;
        status = vaMapBuffer(dpy, buf, reinterpret_cast<void**>(&misc));
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaMapBuffer(misc) failed: %s", vaErrorStr(status));
            return false;
        }
        misc->type = type;
        memcpy(misc->data, payload, payloadSize);
        vaUnmapBuffer(dpy, buf);
        status = vaRenderPicture(dpy, mVAContext, &buf, 1);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(misc) failed: %s", vaErrorStr(status));
            return false;
        }
        return true;
    };

    // CQP (the VDENC path): QP travels in the picture/slice params — the RateControl and HRD
    // buffers below configure the BRC this path deliberately doesn't run (no HuC on the VF).
    // Keep frame rate (informational) and the quality level (target usage).
    if (mRcMode == VA_RC_CQP) {
        VAEncMiscParameterFrameRate cfr;
        memset(&cfr, 0, sizeof(cfr));
        cfr.framerate = mFramerate ? mFramerate : 30;
        if (!renderMisc(VAEncMiscParameterTypeFrameRate, &cfr, sizeof(cfr))) return false;
        if (mQualityLevel > 0) {
            VAEncMiscParameterBufferQualityLevel cql;
            memset(&cql, 0, sizeof(cql));
            cql.quality_level = mQualityLevel;
            if (!renderMisc(VAEncMiscParameterTypeQualityLevel, &cql, sizeof(cql))) return false;
        }
        return true;
    }

    // Rate control.
    VAEncMiscParameterRateControl rc;
    memset(&rc, 0, sizeof(rc));
    if (mBitrateMode == C2Config::BITRATE_CONST) {
        // CBR: the driver holds a constant rate — target it at the requested bitrate (not the peak).
        rc.bits_per_second = mBitrate ? mBitrate : mPeakBitrate.value_or(0u);
        rc.target_percentage = 100;
    } else {
        // VBR: bits_per_second is the peak ceiling; target_percentage scales it to the average
        // target. Compute in 64-bit — mBitrate * 100 overflows uint32 above ~42 Mbps.
        rc.bits_per_second = mPeakBitrate.value_or(mBitrate);
        rc.target_percentage =
                mBitrate ? std::min<uint32_t>(100, static_cast<uint32_t>(
                                                       (static_cast<uint64_t>(mBitrate) * 100u) /
                                                       rc.bits_per_second))
                         : 100;
    }
    rc.window_size = 1000;  // ms.
    rc.initial_qp = kDefaultQp;
    rc.min_qp = 1;
    rc.max_qp = 51;
    if (!renderMisc(VAEncMiscParameterTypeRateControl, &rc, sizeof(rc))) return false;

    // Frame rate.
    VAEncMiscParameterFrameRate fr;
    memset(&fr, 0, sizeof(fr));
    fr.framerate = mFramerate ? mFramerate : 30;
    if (!renderMisc(VAEncMiscParameterTypeFrameRate, &fr, sizeof(fr))) return false;

    // Encode quality level (target usage) — fastest the driver supports, to minimise per-frame
    // encode latency at full resolution. Only sent if the driver advertised a range (mQualityLevel).
    if (mQualityLevel > 0) {
        VAEncMiscParameterBufferQualityLevel ql;
        memset(&ql, 0, sizeof(ql));
        ql.quality_level = mQualityLevel;
        if (!renderMisc(VAEncMiscParameterTypeQualityLevel, &ql, sizeof(ql))) return false;
    }

    // HRD (buffer model) — size the CPB at ~1s of peak.
    VAEncMiscParameterHRD hrd;
    memset(&hrd, 0, sizeof(hrd));
    hrd.buffer_size = rc.bits_per_second;
    hrd.initial_buffer_fullness = hrd.buffer_size / 2;
    if (!renderMisc(VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd))) return false;

    return true;
}

bool VaapiVideoEncoder::createParamBuffer(VABufferType type, size_t size, const void* data,
                                          std::vector<VABufferID>* buffers, VABufferID* out) {
    VADisplay dpy = mDevice->display();
    *out = VA_INVALID_ID;
    VAStatus status = vaCreateBuffer(dpy, mVAContext, type, static_cast<uint32_t>(size), 1,
                                     const_cast<void*>(data), out);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateBuffer(type=%d) failed: %s", type, vaErrorStr(status));
        return false;
    }
    buffers->push_back(*out);
    return true;
}

void VaapiVideoEncoder::destroyBuffers(std::vector<VABufferID>* buffers) {
    VADisplay dpy = mDevice->display();
    for (VABufferID id : *buffers) {
        if (id != VA_INVALID_ID) vaDestroyBuffer(dpy, id);
    }
    buffers->clear();
}

bool VaapiVideoEncoder::emitCodedBuffer(int64_t timestamp, bool keyFrame) {
    VADisplay dpy = mDevice->display();

    VACodedBufferSegment* segment = nullptr;
    VAStatus status = vaMapBuffer(dpy, mCodedBuffer, reinterpret_cast<void**>(&segment));
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaMapBuffer(coded) failed: %s", vaErrorStr(status));
        return false;
    }

    // Total coded size across the segment chain.
    size_t totalSize = 0;
    for (VACodedBufferSegment* s = segment; s != nullptr;
         s = static_cast<VACodedBufferSegment*>(s->next)) {
        totalSize += s->size;
    }
    if (totalSize == 0) {
        ALOGW("Encoder produced 0 coded bytes for ts=%lld", static_cast<long long>(timestamp));
        vaUnmapBuffer(dpy, mCodedBuffer);
        return true;
    }

    // Fetch a C2 linear output block sized to the coded data.
    std::unique_ptr<BitstreamBuffer> output;
    mFetchOutputBufferCb.Run(static_cast<uint32_t>(totalSize), &output);
    if (!output || !output->dmabuf) {
        ALOGE("Failed to fetch output block (%zu bytes)", totalSize);
        vaUnmapBuffer(dpy, mCodedBuffer);
        return false;
    }

    // Map the C2 block and copy the coded segments into it.
    C2WriteView view = output->dmabuf->map().get();
    if (view.error() != C2_OK) {
        ALOGE("Failed to map output C2 block: %d", view.error());
        vaUnmapBuffer(dpy, mCodedBuffer);
        return false;
    }
    uint8_t* dst = view.data() + output->offset;
    size_t written = 0;
    for (VACodedBufferSegment* s = segment; s != nullptr;
         s = static_cast<VACodedBufferSegment*>(s->next)) {
        if (written + s->size > output->size) {
            ALOGE("Coded data (%zu) exceeds output block (%zu)", written + s->size, output->size);
            break;
        }
        memcpy(dst + written, s->buf, s->size);
        written += s->size;
    }
    vaUnmapBuffer(dpy, mCodedBuffer);

    // Hand the bitstream to the component.
    mOutputBufferDoneCb.Run(written, timestamp, keyFrame, std::move(output));
    return true;
}

void VaapiVideoEncoder::drain() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    // This encoder emits each frame synchronously in encodeTask() (no reorder
    // delay, ip_period 1), so there is nothing queued to flush. Signal done.
    if (mDrainDoneCb) mDrainDoneCb.Run(true);
}

void VaapiVideoEncoder::flush() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    // Reset the GOP/reference state so the next frame starts a fresh IDR.
    mFrameInGop = 0;
    mForceKeyframe = true;
    mHasReference = false;
    mRefSurface = VA_INVALID_SURFACE;
}

bool VaapiVideoEncoder::setBitrate(uint32_t bitrate) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    mBitrate = bitrate;
    // Re-armed on the next frame via renderRateControl().
    return true;
}

bool VaapiVideoEncoder::setPeakBitrate(uint32_t peakBitrate) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    mPeakBitrate = peakBitrate;
    return true;
}

bool VaapiVideoEncoder::setFramerate(uint32_t framerate) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    mFramerate = framerate;
    return true;
}

void VaapiVideoEncoder::requestKeyframe() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    mForceKeyframe = true;
}

void VaapiVideoEncoder::onError() {
    if (mErrorCb) mErrorCb.Run();
}

// ---- codec dispatch ----

bool VaapiVideoEncoder::renderFrameParams(FrameType type, bool keyFrame, VASurfaceID reconSurface,
                                          std::vector<VABufferID>* buffers) {
    VADisplay dpy = mDevice->display();
    // The submission ladder both codecs share — sequence (key frames only) → rate control →
    // packed headers → picture → slice — with only the struct types differing.
    auto render = [&](VABufferType btype, size_t size, const void* data, const char* what) {
        VABufferID buf = VA_INVALID_ID;
        if (!createParamBuffer(btype, size, data, buffers, &buf)) return false;
        VAStatus status = vaRenderPicture(dpy, mVAContext, &buf, 1);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(%s) failed: %s", what, vaErrorStr(status));
            return false;
        }
        return true;
    };

    if (isH264Profile(mProfile)) {
        VAEncSequenceParameterBufferH264 seq;
        VAEncPictureParameterBufferH264 pic;
        VAEncSliceParameterBufferH264 slice;
        buildH264SequenceParam(type, &seq);
        buildH264PictureParam(type, &pic);
        pic.CurrPic.picture_id = reconSurface;
        pic.coded_buf = mCodedBuffer;
        buildH264SliceParam(type, pic, &slice);
        if (keyFrame && !render(VAEncSequenceParameterBufferType, sizeof(seq), &seq, "seq"))
            return false;
        if (!renderRateControl(buffers)) return false;
        if (mPackedHeaders &&
            !renderPackedH264Headers(seq, pic, slice, type, /*includeParamSets=*/keyFrame,
                                     buffers))
            return false;
        if (!render(VAEncPictureParameterBufferType, sizeof(pic), &pic, "pic")) return false;
        return render(VAEncSliceParameterBufferType, sizeof(slice), &slice, "slice");
    }

    VAEncSequenceParameterBufferHEVC seq;
    VAEncPictureParameterBufferHEVC pic;
    VAEncSliceParameterBufferHEVC slice;
    buildHEVCSequenceParam(type, &seq);
    buildHEVCPictureParam(type, &pic);
    pic.decoded_curr_pic.picture_id = reconSurface;
    pic.coded_buf = mCodedBuffer;
    buildHEVCSliceParam(type, pic, &slice);
    if (keyFrame && !render(VAEncSequenceParameterBufferType, sizeof(seq), &seq, "seq"))
        return false;
    if (!renderRateControl(buffers)) return false;
    if (mPackedHeaders &&
        !renderPackedHEVCHeaders(seq, pic, slice, type, /*includeParamSets=*/keyFrame, buffers))
        return false;
    if (!render(VAEncPictureParameterBufferType, sizeof(pic), &pic, "pic")) return false;
    return render(VAEncSliceParameterBufferType, sizeof(slice), &slice, "slice");
}

// ---- H.264 param-buffer + packed-header builders ----
//
// The same one-slice IDR-then-P shape as the HEVC half, expressed in H.264 terms:
// pic_order_cnt_type 2 (POC derived from frame_num — legal because decoding order equals
// output order with no B-frames, and it keeps every POC field out of the slice header),
// frame_num = position in GOP mod 2^8, one short-term reference, CABAC on Main/High and
// CAVLC on Constrained Baseline. Mirrors libva-utils/encode/h264encode.c where the HEVC
// half mirrors hevcencode.c.

void VaapiVideoEncoder::buildH264SequenceParam(FrameType type,
                                               VAEncSequenceParameterBufferH264* seq) const {
    memset(seq, 0, sizeof(*seq));
    seq->seq_parameter_set_id = 0;

    // The lowest level whose frame size, MB throughput and (High-profile) bitrate ceiling admit
    // the stream — same selection shape as the HEVC builder. 3760x1992@60 needs 5.2: 29375
    // MBs/frame (>22080 rules out 5.0) at 1.76 MMB/s (>983040 rules out 5.1).
    const uint32_t mbs = (mCodedSize.width / 16) * (mCodedSize.height / 16);
    const uint64_t mbRate = static_cast<uint64_t>(mbs) * (mFramerate ? mFramerate : 30u);
    const uint32_t peakBps = mPeakBitrate.value_or(mBitrate);
    uint8_t lvlIdc = 51;
    auto atLeast = [&](uint8_t v) { if (lvlIdc < v) lvlIdc = v; };
    if (mbRate > 983040ull) atLeast(52);
    if (mbs > 36864u || mbRate > 2073600ull) atLeast(60);
    if (peakBps > 300000000u) atLeast(61);  // High-profile MaxBR: 300M @5.1-6.0, 600M @6.1
    if (peakBps > 600000000u) atLeast(62);
    seq->level_idc = mLevel.value_or(lvlIdc);

    seq->intra_period = mKeyFramePeriod;
    seq->intra_idr_period = mKeyFramePeriod;
    seq->ip_period = 1;  // No B-frames.
    seq->bits_per_second = mBitrate;
    seq->max_num_ref_frames = kNumRefFrames;
    seq->picture_width_in_mbs = static_cast<uint16_t>(mCodedSize.width / 16);
    seq->picture_height_in_mbs = static_cast<uint16_t>(mCodedSize.height / 16);

    seq->seq_fields.bits.chroma_format_idc = 1;  // 4:2:0.
    seq->seq_fields.bits.frame_mbs_only_flag = 1;
    seq->seq_fields.bits.mb_adaptive_frame_field_flag = 0;
    seq->seq_fields.bits.seq_scaling_matrix_present_flag = 0;
    seq->seq_fields.bits.direct_8x8_inference_flag = 1;
    seq->seq_fields.bits.log2_max_frame_num_minus4 = kH264Log2MaxFrameNumMinus4;
    seq->seq_fields.bits.pic_order_cnt_type = 2;
    seq->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 4;  // unused with POC type 2
    seq->seq_fields.bits.delta_pic_order_always_zero_flag = 0;
    seq->bit_depth_luma_minus8 = 0;
    seq->bit_depth_chroma_minus8 = 0;

    // The coded size is CTB-aligned (32/64, both multiples of the 16-pixel MB), so the visible
    // size is restored by cropping — offsets in chroma units (SubWidthC = SubHeightC = 2).
    if (mCodedSize.width != mVisibleSize.width || mCodedSize.height != mVisibleSize.height) {
        seq->frame_cropping_flag = 1;
        seq->frame_crop_right_offset = (mCodedSize.width - mVisibleSize.width) / 2;
        seq->frame_crop_bottom_offset = (mCodedSize.height - mVisibleSize.height) / 2;
    }
    seq->vui_parameters_present_flag = 0;
    (void)type;
}

void VaapiVideoEncoder::buildH264PictureParam(FrameType type,
                                              VAEncPictureParameterBufferH264* pic) const {
    memset(pic, 0, sizeof(*pic));
    const uint32_t frameNum = mFrameInGop % kH264MaxFrameNum;
    // POC type 2 derives POC = 2 * frame_num; the VA fields must agree with what a decoder
    // will derive or the driver's MV scaling silently disagrees with the stream.
    const int32_t poc = static_cast<int32_t>(2 * frameNum);
    const int32_t refPoc = static_cast<int32_t>(2 * ((frameNum + kH264MaxFrameNum - 1)
                                                     % kH264MaxFrameNum));

    for (auto& ref : pic->ReferenceFrames) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_H264_INVALID;
    }
    if (type == FrameType::P && mHasReference) {
        pic->ReferenceFrames[0].picture_id = mRefSurface;
        pic->ReferenceFrames[0].frame_idx = (frameNum + kH264MaxFrameNum - 1) % kH264MaxFrameNum;
        pic->ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        pic->ReferenceFrames[0].TopFieldOrderCnt = refPoc;
        pic->ReferenceFrames[0].BottomFieldOrderCnt = refPoc;
    }

    pic->CurrPic.picture_id = VA_INVALID_SURFACE;  // filled by caller.
    pic->CurrPic.frame_idx = frameNum;
    pic->CurrPic.TopFieldOrderCnt = poc;
    pic->CurrPic.BottomFieldOrderCnt = poc;
    pic->CurrPic.flags = 0;

    pic->coded_buf = VA_INVALID_ID;  // filled by caller.
    pic->pic_parameter_set_id = 0;
    pic->seq_parameter_set_id = 0;
    pic->frame_num = static_cast<uint16_t>(frameNum);
    pic->pic_init_qp = mQp;
    pic->num_ref_idx_l0_active_minus1 = 0;
    pic->num_ref_idx_l1_active_minus1 = 0;
    pic->chroma_qp_index_offset = 0;
    pic->second_chroma_qp_index_offset = 0;

    pic->pic_fields.bits.idr_pic_flag = (type == FrameType::IDR) ? 1 : 0;
    pic->pic_fields.bits.reference_pic_flag = 1;  // I and P are references here.
    // CABAC everywhere it is legal; Constrained Baseline is the one profile without it.
    pic->pic_fields.bits.entropy_coding_mode_flag =
            (mVAProfile == VAProfileH264ConstrainedBaseline) ? 0 : 1;
    pic->pic_fields.bits.weighted_pred_flag = 0;
    pic->pic_fields.bits.weighted_bipred_idc = 0;
    pic->pic_fields.bits.constrained_intra_pred_flag = 0;
    pic->pic_fields.bits.transform_8x8_mode_flag = (mVAProfile == VAProfileH264High) ? 1 : 0;
    pic->pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pic->pic_fields.bits.redundant_pic_cnt_present_flag = 0;
    pic->pic_fields.bits.pic_order_present_flag = 0;
    pic->pic_fields.bits.pic_scaling_matrix_present_flag = 0;
}

void VaapiVideoEncoder::buildH264SliceParam(FrameType type,
                                            const VAEncPictureParameterBufferH264& pic,
                                            VAEncSliceParameterBufferH264* slice) const {
    memset(slice, 0, sizeof(*slice));
    slice->macroblock_address = 0;
    slice->num_macroblocks = (mCodedSize.width / 16) * (mCodedSize.height / 16);
    slice->macroblock_info = VA_INVALID_ID;
    slice->slice_type = (type == FrameType::IDR) ? H264_SLICE_I : H264_SLICE_P;
    slice->pic_parameter_set_id = 0;
    slice->idr_pic_id = (type == FrameType::IDR) ? mIdrPicId : 0;
    slice->pic_order_cnt_lsb = 0;  // POC type 2: nothing in the slice header.

    slice->num_ref_idx_active_override_flag = 0;
    slice->num_ref_idx_l0_active_minus1 = 0;
    slice->num_ref_idx_l1_active_minus1 = 0;
    for (auto& ref : slice->RefPicList0) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_H264_INVALID;
    }
    for (auto& ref : slice->RefPicList1) {
        ref.picture_id = VA_INVALID_SURFACE;
        ref.flags = VA_PICTURE_H264_INVALID;
    }
    if (type == FrameType::P) {
        slice->RefPicList0[0] = pic.ReferenceFrames[0];
    }

    slice->slice_qp_delta = 0;
    slice->cabac_init_idc = 0;
    slice->direct_spatial_mv_pred_flag = 1;  // B-only field; inert for I/P.
    slice->disable_deblocking_filter_idc = 0;
    slice->slice_alpha_c0_offset_div2 = 0;
    slice->slice_beta_offset_div2 = 0;
}

bool VaapiVideoEncoder::renderPackedH264Headers(const VAEncSequenceParameterBufferH264& seq,
                                                const VAEncPictureParameterBufferH264& pic,
                                                const VAEncSliceParameterBufferH264& slice,
                                                FrameType type, bool includeParamSets,
                                                std::vector<VABufferID>* buffers) {
    VADisplay dpy = mDevice->display();

    auto emit = [&](VAEncPackedHeaderType headerType, const Bitstream& bs) -> bool {
        VAEncPackedHeaderParameterBuffer param;
        memset(&param, 0, sizeof(param));
        param.type = headerType;
        param.bit_length = bs.bitLength();
        param.has_emulation_bytes = 0;  // driver inserts EPB.

        VABufferID paramBuf = VA_INVALID_ID;
        VABufferID dataBuf = VA_INVALID_ID;
        VAStatus status = vaCreateBuffer(dpy, mVAContext, VAEncPackedHeaderParameterBufferType,
                                         sizeof(param), 1, &param, &paramBuf);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(packed param) failed: %s", vaErrorStr(status));
            return false;
        }
        buffers->push_back(paramBuf);
        status = vaCreateBuffer(dpy, mVAContext, VAEncPackedHeaderDataBufferType,
                                static_cast<uint32_t>(bs.sizeBytes()), 1,
                                const_cast<uint8_t*>(bs.data()), &dataBuf);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(packed data) failed: %s", vaErrorStr(status));
            return false;
        }
        buffers->push_back(dataBuf);

        VABufferID renderIds[2] = {paramBuf, dataBuf};
        status = vaRenderPicture(dpy, mVAContext, renderIds, 2);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(packed) failed: %s", vaErrorStr(status));
            return false;
        }
        return true;
    };

    const bool high = mVAProfile == VAProfileH264High;
    const bool cbp = mVAProfile == VAProfileH264ConstrainedBaseline;
    const uint8_t profileIdc = high ? 100 : (cbp ? 66 : 77);

    // ---- SPS ---- (parameter sets: key frames only)
    if (includeParamSets) {
        Bitstream bs;
        bs.startNALH264(H264_NALU_SPS, 3);
        bs.putBits(profileIdc, 8);
        // constraint_set0..5 + reserved_zero_2bits. Constrained Baseline = Baseline profile_idc
        // with constraint_set1 (and 0) asserted.
        bs.putBits(cbp ? 0xC0 : 0x00, 8);
        bs.putBits(seq.level_idc, 8);
        bs.putUE(seq.seq_parameter_set_id);
        if (profileIdc == 100) {
            bs.putUE(seq.seq_fields.bits.chroma_format_idc);
            bs.putUE(seq.bit_depth_luma_minus8);
            bs.putUE(seq.bit_depth_chroma_minus8);
            bs.putBits(0, 1);  // qpprime_y_zero_transform_bypass_flag
            bs.putBits(seq.seq_fields.bits.seq_scaling_matrix_present_flag, 1);
        }
        bs.putUE(seq.seq_fields.bits.log2_max_frame_num_minus4);
        bs.putUE(seq.seq_fields.bits.pic_order_cnt_type);  // 2 → no further POC syntax
        bs.putUE(seq.max_num_ref_frames);
        bs.putBits(0, 1);  // gaps_in_frame_num_value_allowed_flag
        bs.putUE(seq.picture_width_in_mbs - 1);
        bs.putUE(seq.picture_height_in_mbs - 1);  // frame_mbs_only: map units == MBs
        bs.putBits(seq.seq_fields.bits.frame_mbs_only_flag, 1);
        bs.putBits(seq.seq_fields.bits.direct_8x8_inference_flag, 1);
        bs.putBits(seq.frame_cropping_flag, 1);
        if (seq.frame_cropping_flag) {
            bs.putUE(0);                             // left
            bs.putUE(seq.frame_crop_right_offset);   // right
            bs.putUE(0);                             // top
            bs.putUE(seq.frame_crop_bottom_offset);  // bottom
        }
        bs.putBits(0, 1);  // vui_parameters_present_flag
        bs.trailingBits();
        if (!emit(VAEncPackedHeaderSequence, bs)) return false;
    }

    // ---- PPS ----
    if (includeParamSets) {
        Bitstream bs;
        bs.startNALH264(H264_NALU_PPS, 3);
        bs.putUE(pic.pic_parameter_set_id);
        bs.putUE(pic.seq_parameter_set_id);
        bs.putBits(pic.pic_fields.bits.entropy_coding_mode_flag, 1);
        bs.putBits(pic.pic_fields.bits.pic_order_present_flag, 1);
        bs.putUE(0);  // num_slice_groups_minus1
        bs.putUE(pic.num_ref_idx_l0_active_minus1);
        bs.putUE(pic.num_ref_idx_l1_active_minus1);
        bs.putBits(pic.pic_fields.bits.weighted_pred_flag, 1);
        bs.putBits(pic.pic_fields.bits.weighted_bipred_idc, 2);
        bs.putSE(static_cast<int32_t>(pic.pic_init_qp) - 26);  // pic_init_qp_minus26
        bs.putSE(0);  // pic_init_qs_minus26
        bs.putSE(pic.chroma_qp_index_offset);
        bs.putBits(pic.pic_fields.bits.deblocking_filter_control_present_flag, 1);
        bs.putBits(pic.pic_fields.bits.constrained_intra_pred_flag, 1);
        bs.putBits(pic.pic_fields.bits.redundant_pic_cnt_present_flag, 1);
        if (high) {
            // The more_rbsp_data() tail — present only when something in it is non-default.
            bs.putBits(pic.pic_fields.bits.transform_8x8_mode_flag, 1);
            bs.putBits(pic.pic_fields.bits.pic_scaling_matrix_present_flag, 1);
            bs.putSE(pic.second_chroma_qp_index_offset);
        }
        bs.trailingBits();
        if (!emit(VAEncPackedHeaderPicture, bs)) return false;
    }

    // ---- Slice header ----
    {
        Bitstream bs;
        const bool idr = type == FrameType::IDR;
        bs.startNALH264(idr ? H264_NALU_IDR : H264_NALU_NON_IDR, idr ? 3 : 2);
        bs.putUE(0);  // first_mb_in_slice
        bs.putUE(slice.slice_type);
        bs.putUE(slice.pic_parameter_set_id);
        bs.putBits(pic.frame_num, kH264Log2MaxFrameNumMinus4 + 4);
        if (idr) {
            bs.putUE(slice.idr_pic_id);
        }
        // POC type 2: no pic_order_cnt syntax at all.
        if (slice.slice_type == H264_SLICE_P) {
            bs.putBits(slice.num_ref_idx_active_override_flag, 1);
            bs.putBits(0, 1);  // ref_pic_list_modification_flag_l0
        }
        // dec_ref_pic_marking (nal_ref_idc != 0 on every frame here).
        if (idr) {
            bs.putBits(0, 1);  // no_output_of_prior_pics_flag
            bs.putBits(0, 1);  // long_term_reference_flag
        } else {
            bs.putBits(0, 1);  // adaptive_ref_pic_marking_mode_flag → sliding window
        }
        if (pic.pic_fields.bits.entropy_coding_mode_flag && slice.slice_type != H264_SLICE_I) {
            bs.putUE(slice.cabac_init_idc);
        }
        bs.putSE(slice.slice_qp_delta);
        // deblocking_filter_control_present_flag == 1 in the PPS.
        bs.putUE(slice.disable_deblocking_filter_idc);
        bs.putSE(slice.slice_alpha_c0_offset_div2);
        bs.putSE(slice.slice_beta_offset_div2);
        // NO trailing bits: unlike HEVC (whose slice header carries its own byte_alignment),
        // an H.264 slice header ends mid-byte and the driver continues the slice data at
        // bit_length — flush pads the last byte without counting it.
        bs.flushUnaligned();
        if (!emit(VAEncPackedHeaderSlice, bs)) return false;
    }

    return true;
}

}  // namespace android
