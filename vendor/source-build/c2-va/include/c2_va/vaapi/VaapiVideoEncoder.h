// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_VAAPI_VAAPI_VIDEO_ENCODER_H
#define ANDROID_C2_VA_VAAPI_VAAPI_VIDEO_ENCODER_H

#include <stdint.h>

#include <memory>
#include <optional>
#include <queue>
#include <vector>

#include <base/memory/weak_ptr.h>
#include <base/sequenced_task_runner.h>
#include <ui/Size.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <v4l2_codec2/common/VideoPixelFormat.h>
#include <v4l2_codec2/common/VideoTypes.h>
#include <v4l2_codec2/components/BitstreamBuffer.h>
#include <v4l2_codec2/components/VideoEncoder.h>

#include <c2_va/vaapi/VaapiDevice.h>

namespace android {

// A VAAPI-backed VideoEncoder, encoding NV12 frames on the Intel xe
// SR-IOV VF via libva + the in-tree iHD driver.
//
// This is the ENCODE sibling of VaapiVideoDecoder. It mirrors the VideoEncoder
// interface from external/v4l2_codec2 (VideoEncoder::create() has the same
// signature as V4L2Encoder::create()) and implements H.265 (HEVC,
// VAProfileHEVCMain, VAEntrypointEncSlice) end to end:
//
//   * one-time setup: query packed-header support, create a VAConfig with the
//     requested rate-control mode (CBR/VBR) + packed headers, create a context
//     sized to the coded resolution, allocate an NV12 encode-source surface, a
//     reconstructed (reference) surface and a coded (bitstream) buffer;
//   * per frame: import the InputFrame's dma-buf as a DRM_PRIME VA surface,
//     VPP-convert to NV12 into the encode-source surface if the input is not
//     already NV12 (scrcpy Surface input is typically RGBA), build the
//     sequence/picture/slice VAEnc*ParameterBufferHEVC buffers, submit the
//     misc rate-control parameter and — on key frames — the packed VPS/SPS/PPS
//     headers, then vaBeginPicture / vaRenderPicture / vaEndPicture;
//   * output: vaSyncSurface, vaMapBuffer(VACodedBufferSegment) to read the
//     coded NAL bytes, copy them into a C2 linear block fetched via
//     FetchOutputBufferCB and hand it to OutputBufferDoneCB.
//
// The GOP is a simple IDR-then-P structure (ip_period == 1, no B-frames) with a
// single short-term reference, which keeps the DPB/ref-list management trivial.
// The keyframe cadence is keyFramePeriod frames; requestKeyframe() forces the
// next frame to be an IDR. setBitrate()/setFramerate() re-arm the misc
// rate-control parameter for subsequent frames.
//
// H.264 (VAProfileH264{ConstrainedBaseline,Main,High} by requested C2 profile) shares all of
// the scaffolding (config/context/surfaces/coded buffer/import/VPP/output) and implements the
// same GOP shape through its own param-buffer + packed-header builders (see buildH264* /
// renderPackedH264Headers): POC type 2, frame_num-driven, CABAC except on Constrained
// Baseline (bd remora-oac7 — these were TODO stubs, which is why the encoder 'could not
// start' while the driver offered full H264 EncSlice/EncSliceLP all along).
//
// All public methods and internal callbacks run on |mTaskRunner|, mirroring the
// VideoEncoder threading contract (EncodeComponent's encoder task runner).
class VaapiVideoEncoder : public VideoEncoder {
public:
    // Factory mirroring V4L2Encoder::create(). Returns nullptr (self-disable) if
    // no VAAPI device is available or the profile/entrypoint is unsupported.
    static std::unique_ptr<VideoEncoder> create(
            C2Config::profile_t profile, std::optional<uint8_t> level,
            const ui::Size& visibleSize, uint32_t stride, uint32_t keyFramePeriod,
            C2Config::bitrate_mode_t bitrateMode, uint32_t bitrate,
            std::optional<uint32_t> peakBitrate, FetchOutputBufferCB fetchOutputBufferCb,
            InputBufferDoneCB inputBufferDoneCb, OutputBufferDoneCB outputBufferDoneCb,
            DrainDoneCB drainDoneCb, ErrorCB errorCb,
            scoped_refptr<::base::SequencedTaskRunner> taskRunner);

    ~VaapiVideoEncoder() override;

    VaapiVideoEncoder(const VaapiVideoEncoder&) = delete;
    VaapiVideoEncoder& operator=(const VaapiVideoEncoder&) = delete;

    // VideoEncoder implementation. All run on |mTaskRunner|.
    bool encode(std::unique_ptr<InputFrame> frame) override;
    void drain() override;
    void flush() override;

    bool setBitrate(uint32_t bitrate) override;
    bool setPeakBitrate(uint32_t peakBitrate) override;
    bool setFramerate(uint32_t framerate) override;
    void requestKeyframe() override;

    VideoPixelFormat inputFormat() const override { return kInputPixelFormat; }
    const ui::Size& visibleSize() const override { return mVisibleSize; }
    const ui::Size& codedSize() const override { return mCodedSize; }
    // We VPP-convert any input (RGBA/BGRA/NV12) to the NV12 encode surface ourselves, so the
    // component must not build a gralloc-backed FormatConverter (which the image's gbm gralloc rejects).
    bool handlesInputFormatConversion() const override { return true; }

private:
    // Frame coding type. This encoder only emits IDR and P frames (ip_period 1).
    enum class FrameType { IDR, P };

    VaapiVideoEncoder(C2Config::profile_t profile, std::optional<uint8_t> level,
                      const ui::Size& visibleSize, uint32_t stride, uint32_t keyFramePeriod,
                      C2Config::bitrate_mode_t bitrateMode, uint32_t bitrate,
                      std::optional<uint32_t> peakBitrate, FetchOutputBufferCB fetchOutputBufferCb,
                      InputBufferDoneCB inputBufferDoneCb, OutputBufferDoneCB outputBufferDoneCb,
                      DrainDoneCB drainDoneCb, ErrorCB errorCb,
                      scoped_refptr<::base::SequencedTaskRunner> taskRunner);

    // ---- lifecycle ----
    // Second-phase init: opens the device, resolves the VA profile/entrypoint,
    // creates config + context + surfaces + coded buffer. Returns false to make
    // create() self-disable.
    bool initialize();
    void destroyVAObjects();

    // ---- per-frame pump (posted from encode()) ----
    void encodeTask(std::unique_ptr<InputFrame> frame);
    // Import + (optionally) VPP the input into |mSrcSurface|. Returns false on
    // failure. |importedSurface| receives any DRM_PRIME surface that must be
    // destroyed after the encode; VA_INVALID_SURFACE if none. |syncVpp| = false
    // leaves the VPP unsynced (pipelined mode).
    bool prepareSourceSurface(const InputFrame& frame, VASurfaceID* importedSurface, bool syncVpp);
    // Import an InputFrame's dma-buf as an external DRM_PRIME VA surface. With
    // |atCodedHeight| the surface claims the CTB-aligned coded height — only
    // valid when inputCoversCodedReads() said the BO physically covers it — so
    // the imported surface matches the VDENC source walker's read extent.
    VASurfaceID importInputAsSurface(const InputFrame& frame, bool atCodedHeight = false);
    // VPP-convert |src| into the NV12 |mSrcSurface|. |syncAfter| = false submits without the
    // CPU sync (pipelined mode: the encode submission is driver-ordered behind the VPP write).
    bool convertToNV12(VASurfaceID src, bool syncAfter);
    // Whether this frame takes the direct-RGB encode path (mRgbDirect && an
    // RGB-family input format).
    bool useRgbDirect(const InputFrame& frame) const;
    // Whether the input dma-buf physically covers the VDENC source read
    // footprint, (codedHeight-1)*pitch + codedWidth*4 (the walker reads full
    // 64x64 CTBs). 64-aligned-height BOs from the minigbm encoder fix do;
    // under-allocated visible-size BOs (pre-fix allocations, other allocators)
    // must take the VPP path instead — reading past the BO CAT-faults the VCS.
    bool inputCoversCodedReads(const InputFrame& frame);
    // Destroy the imported DRM_PRIME surface and hand the input dma-buf back to
    // the producer. Direct-RGB path only (the VPP path releases inline, earlier).
    void releaseDirectInput(VASurfaceID* surface, uint64_t index);

    // ---- HEVC param-buffer + packed-header builders ----
    void buildHEVCSequenceParam(FrameType type, VAEncSequenceParameterBufferHEVC* seq) const;
    void buildHEVCPictureParam(FrameType type, VAEncPictureParameterBufferHEVC* pic) const;
    void buildHEVCSliceParam(FrameType type, const VAEncPictureParameterBufferHEVC& pic,
                             VAEncSliceParameterBufferHEVC* slice) const;
    // Packed header emitters (RBSP + start code, has_emulation_bytes=1). Each
    // vaCreateBuffer()s a param+data pair, vaRenderPicture()s them and destroys
    // them. |buf| collects created buffer ids for destruction after EndPicture.
    // The packed slice header is emitted for EVERY frame (the driver relies on it for the slice
    // NAL type + POC/RPS); the VPS/SPS/PPS parameter sets are emitted only when |includeParamSets|
    // (key frames). Emitting them per-frame is harmless but wasteful; omitting the slice header on
    // P-frames makes the driver auto-generate a malformed (IDR-typed) slice → CABAC desync.
    bool renderPackedHEVCHeaders(const VAEncSequenceParameterBufferHEVC& seq,
                                 const VAEncPictureParameterBufferHEVC& pic,
                                 const VAEncSliceParameterBufferHEVC& slice, FrameType type,
                                 bool includeParamSets, std::vector<VABufferID>* buffers);

    // ---- H.264 param-buffer + packed-header builders ----
    // The H.264 rendering of the same one-slice IDR-then-P shape: POC type 2 (derived from
    // frame_num — no POC syntax anywhere), CABAC on Main/High, CAVLC on Constrained Baseline.
    void buildH264SequenceParam(FrameType type, VAEncSequenceParameterBufferH264* seq) const;
    void buildH264PictureParam(FrameType type, VAEncPictureParameterBufferH264* pic) const;
    void buildH264SliceParam(FrameType type, const VAEncPictureParameterBufferH264& pic,
                             VAEncSliceParameterBufferH264* slice) const;
    bool renderPackedH264Headers(const VAEncSequenceParameterBufferH264& seq,
                                 const VAEncPictureParameterBufferH264& pic,
                                 const VAEncSliceParameterBufferH264& slice, FrameType type,
                                 bool includeParamSets, std::vector<VABufferID>* buffers);

    // The per-frame param submission ladder, dispatched on the codec: sequence (key frames) →
    // rate control → packed headers → picture → slice.
    bool renderFrameParams(FrameType type, bool keyFrame, VASurfaceID reconSurface,
                           std::vector<VABufferID>* buffers);

    // ---- rate control ----
    // Build the VAEncMiscParameter{RateControl,FrameRate,HRD} buffers for the
    // current bitrate/framerate and render them. |buffers| collects the ids.
    bool renderRateControl(std::vector<VABufferID>* buffers);

    // Create a param buffer, remembering its id for later destruction.
    bool createParamBuffer(VABufferType type, size_t size, const void* data,
                           std::vector<VABufferID>* buffers, VABufferID* out);
    void destroyBuffers(std::vector<VABufferID>* buffers);

    // ---- output ----
    // Map the coded buffer, copy the bitstream into a fetched C2 block and emit
    // it via OutputBufferDoneCB. Returns false on failure.
    bool emitCodedBuffer(int64_t timestamp, bool keyFrame);

    void onError();

    // Immutable config.
    const C2Config::profile_t mProfile;
    const std::optional<uint8_t> mLevel;
    const ui::Size mVisibleSize;
    const ui::Size mCodedSize;  // CTB-aligned; the VA context/surface size.
    // Engine CTB/LCU size: 32 for EncSlice (VME), 64 for VDENC (which rejects CTB-32 streams).
    uint32_t mCtbSize = 32;
    const uint32_t mStride;
    const uint32_t mKeyFramePeriod;

    // Rate control (mutable via setters).
    C2Config::bitrate_mode_t mBitrateMode;
    uint32_t mBitrate;
    std::optional<uint32_t> mPeakBitrate;
    uint32_t mFramerate = 30;

    // Callbacks (called on |mTaskRunner|).
    FetchOutputBufferCB mFetchOutputBufferCb;
    InputBufferDoneCB mInputBufferDoneCb;
    OutputBufferDoneCB mOutputBufferDoneCb;
    DrainDoneCB mDrainDoneCb;
    ErrorCB mErrorCb;

    scoped_refptr<::base::SequencedTaskRunner> mTaskRunner;

    // VA objects.
    std::unique_ptr<VaapiDevice> mDevice;
    VAProfile mVAProfile = VAProfileNone;
    VAEntrypoint mVAEntrypoint = VAEntrypointEncSlice;
    VAConfigID mVAConfig = VA_INVALID_ID;
    VAContextID mVAContext = VA_INVALID_ID;
    // NV12 encode source (VPP/import destination + encode input).
    VASurfaceID mSrcSurface = VA_INVALID_SURFACE;
    // Reconstructed picture (reference) surfaces. [0] = current, [1] = previous.
    VASurfaceID mReconSurfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    VABufferID mCodedBuffer = VA_INVALID_ID;
    uint32_t mCodedBufferSize = 0;
    // Cached VPP (colour-convert) config/context — created lazily on the first frame, reused for
    // every subsequent one (per-frame context creation measured 10-20 ms at 4K).
    VAConfigID mVppConfig = VA_INVALID_ID;
    VAContextID mVppContext = VA_INVALID_ID;

    // Whether the driver advertised packed VPS/SPS/PPS + slice header support.
    bool mPackedHeaders = false;
    // Encode quality level / target usage (0 = unset → driver default; higher = faster).
    uint32_t mQualityLevel = 0;
    // Resolved VA rate-control mode. The VDENC (EncSliceLP) path always runs VA_RC_CQP — its
    // CBR/VBR needs the HuC the VF lacks (BRC drift = smeary inter frames); CQP is HuC-free.
    uint32_t mRcMode = VA_RC_CBR;
    // Direct-RGB encode source (vendor.remora.encoder_rgb_direct, LP-only): the imported RGBA
    // dma-buf is the encode source itself (driver-internal CSC) — no VPP pass, no mid-frame
    // sync. The input buffer then stays live until the encode completes.
    // The engine reads the source at CTB-64 granularity, past a visible-size BO (kmsg 'Engine
    // memory CAT error class=vcs') — minigbm now 64-aligns encoder RGB allocations, and
    // inputCoversCodedReads() gates each frame: under-allocated BOs fall back to the VPP path.
    bool mRgbDirect = false;
    // One-shot log guard for the under-allocated-input VPP fallback.
    bool mWarnedShortInput = false;
    // Pipelined VPP (vendor.remora.encoder_pipelined): submit VPP + encode back-to-back with no
    // CPU sync in between (single sync before emit). The input buffer is then held until the
    // encode completes, like the direct path.
    bool mPipelinedVpp = false;
    // Fixed QP for the CQP path (vendor.remora.encoder_qp, clamped 1-51).
    uint8_t mQp = 26;

    // ---- GOP / reference / POC state ----
    // Frame counter within the current GOP (0 == IDR).
    uint32_t mFrameInGop = 0;
    // Whether a keyframe was explicitly requested for the next frame.
    bool mForceKeyframe = false;
    // POC of the current picture (2 * display order within the GOP).
    int32_t mCurrentPoc = 0;
    // The previous reconstructed reference: surface + its POC. Valid iff
    // mHasReference is set.
    VASurfaceID mRefSurface = VA_INVALID_SURFACE;
    int32_t mRefPoc = 0;
    bool mHasReference = false;
    // log2_max_pic_order_cnt_lsb_minus4 == 8 → 12-bit POC lsb.
    static constexpr int kLog2MaxPocLsbMinus4 = 8;
    int32_t maxPocLsb() const { return 1 << (kLog2MaxPocLsbMinus4 + 4); }
    // H.264: idr_pic_id, bumped per IDR (consecutive IDRs must differ; wraps harmlessly).
    uint16_t mIdrPicId = 0;

    ::base::WeakPtr<VaapiVideoEncoder> mWeakThis;
    ::base::WeakPtrFactory<VaapiVideoEncoder> mWeakThisFactory{this};
};

}  // namespace android

#endif  // ANDROID_C2_VA_VAAPI_VAAPI_VIDEO_ENCODER_H
