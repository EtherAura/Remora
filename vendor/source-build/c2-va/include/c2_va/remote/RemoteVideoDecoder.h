// Copyright 2026 The Remora Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_REMOTE_REMOTE_VIDEO_DECODER_H
#define ANDROID_C2_VA_REMOTE_REMOTE_VIDEO_DECODER_H

#include <stdint.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

#include <base/memory/weak_ptr.h>
#include <base/sequenced_task_runner.h>
#include <ui/Rect.h>
#include <ui/Size.h>

#include <v4l2_codec2/common/VideoTypes.h>
#include <v4l2_codec2/components/BitstreamBuffer.h>
#include <v4l2_codec2/components/VideoDecoder.h>
#include <v4l2_codec2/components/VideoFrame.h>
#include <v4l2_codec2/components/VideoFramePool.h>

namespace android {

// A VideoDecoder that decodes on the HOST instead of in the guest (bd remora-e5x.3).
//
// WHY THIS EXISTS. NVIDIA's VA-API driver is decode-only and glibc-only: it dlopens proprietary
// libcuda/libnvcuvid, so no bionic build of it can ever exist and an in-guest NVDEC VA driver is
// off the table permanently. Rather than port the unportable, the bitstream crosses a unix socket
// to `remora-frame-decoder` on the host — the compose encoder run in reverse, on the same kernel,
// using the plumbing the encoder path already proves daily.
//
// THAT CHOICE DISSOLVES TWO BLOCKERS AT ONCE, which is the reason for the shape:
//  - no bionic port, because the helper is ordinary host glibc; and
//  - VaapiDevice's one-VADisplay-per-process limit stops mattering, because encode keeps the local
//    Intel VADisplay while decode is remote. c2-va could not otherwise put decode on NVIDIA and
//    encode on Intel simultaneously.
// It also permanently disarms the kRenderNodes ordering trap (bd remora-e5x.2): no in-guest NVIDIA
// VA driver ever exists, so the guest encoder can never bind a decode-only device by accident.
// And it generalises — LIBVA_DRIVER_NAME=radeonsi on the AMD hosts is the same helper, not a
// second implementation.
//
// THIS IS A SIBLING OF VaapiVideoDecoder, NOT A BACKEND INSIDE IT. That decoder is a full
// stateless VA core: H.264/HEVC/VP9/AV1 bitstream parsing, a DPB, POC derivation, reference-list
// construction. A remote decoder needs none of it — the host's libavcodec owns all of that — so
// threading a second path through those 2400 lines would carry state that can never apply. The
// two share only the VideoDecoder interface and the output-pool pattern.
//
// WIRE FORMAT: the structs below are a deliberate THIRD copy, matching
// vendor/native/remora-frame-decoder.c and vendor/host-prereqs/nvdec-vaapi/decode-test-client.c
// field for field. Per-side copies are the convention here, not an oversight — the guest
// hwcomposer's frame_export.cpp does exactly the same for the encoder's publisher protocol across
// this same boundary. A shared header cannot span the two build contexts (deploy-time host native
// vs AOSP-staged guest) without a staging rule whose byte-identity would then need enforcing.
//
// MODE: LINEAR only today. The helper downloads NV12 into a per-frame memfd and this copies it
// into the pool's gralloc buffer — the universally-consumable floor, costing one memcpy per frame
// (the same cost the existing VA path already pays via vaDeriveImage). EXPORT mode returns
// dma_buf fds for zero-copy import and is the real prize, but it is deliberately second: correct
// first, then measured. See bd remora-e5x.3 item (4).
class RemoteVideoDecoder : public VideoDecoder {
public:
    // Same signature as VaapiVideoDecoder::Create() so VaapiDecodeComponent can pick between them
    // with one branch. Returns nullptr (self-disable) when the socket property is unset, the
    // helper is not answering, or the codec is one the helper did not accept — every one of which
    // must fall back to the local VA decoder rather than fail the component.
    static std::unique_ptr<VideoDecoder> Create(
            uint32_t debugStreamId, const VideoCodec& codec, size_t inputBufferSize,
            size_t minNumOutputBuffers, GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb,
            scoped_refptr<::base::SequencedTaskRunner> taskRunner, bool isSecure);

    ~RemoteVideoDecoder() override;

    // VideoDecoder implementation. All three run on |mTaskRunner|.
    void decode(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB decodeCb) override;
    void drain(DecodeCB drainCb) override;
    void flush() override;

    // True when a remote decoder could plausibly be built: the socket property is set and names a
    // path that exists. Cheap and side-effect free, for the component's selection branch. Does NOT
    // prove the helper is alive — that is Create()'s job, and liveness is a connect() not an
    // inode test (bd remora-e5x.13).
    static bool configured();

private:
    // ---- wire protocol (mirror of vendor/native/remora-frame-decoder.c) ----
    static constexpr uint32_t kHelloMagic = 0x524d4448u;    // "RMDH"
    static constexpr uint32_t kAcceptMagic = 0x524d4441u;   // "RMDA"
    static constexpr uint32_t kPacketMagic = 0x524d4450u;   // "RMDP"
    static constexpr uint32_t kFrameMagic = 0x524d4446u;    // "RMDF"
    static constexpr uint32_t kReleaseMagic = 0x524d4452u;  // "RMDR"
    static constexpr uint32_t kDrainedMagic = 0x524d4444u;  // "RMDD"
    static constexpr uint32_t kErrorMagic = 0x524d4445u;    // "RMDE"

    static constexpr uint32_t kModeLinear = 1;
    static constexpr uint32_t kModeExport = 2;
    static constexpr uint32_t kPktEos = 1u << 0;

    // Native byte order and padding: both ends are the same machine by construction.
    struct DecHello {
        uint32_t magic, version, codec, mode;
        uint32_t max_w, max_h;
    };
    struct DecAccept {
        uint32_t magic, version, mode, reserved;
    };
    struct DecPacket {
        uint32_t magic, size;
        uint64_t pts;
        uint32_t flags, reserved;
    };
    struct DecFrame {
        uint32_t magic, res_id;
        uint32_t width, height;
        uint32_t fourcc, mode;
        uint64_t modifier;
        uint32_t n_planes;
        uint32_t offset[3], pitch[3];
        uint32_t plane_fd[3];
        uint64_t pts;
        uint32_t n_fds, total_size;
    };
    struct DecSimple {
        uint32_t magic, value;
    };

    RemoteVideoDecoder(uint32_t debugStreamId, VideoCodec codec,
                       scoped_refptr<::base::SequencedTaskRunner> taskRunner);

    bool start(const VideoCodec& codec, size_t minNumOutputBuffers, GetPoolCB getPoolCb,
               OutputCB outputCb, ErrorCB errorCb);

    // Connect + hello/accept handshake. Returns false without side effects on any failure so
    // Create() can self-disable cleanly.
    bool connectAndNegotiate(const VideoCodec& codec);

    // The reader thread body: recvmsg loop over RMDF / RMDD / RMDE, posting each result onto
    // |mTaskRunner|. Runs until the socket closes or |mReaderStop| is set.
    void readerLoop();

    // One decoded frame arrived (task-runner side). |data| is the NV12 payload already copied out
    // of the helper's memfd, with |pitch|/|offset| describing its layout.
    void onRemoteFrame(uint64_t generation, int32_t bitstreamId, ui::Size size, uint32_t pitchY,
                       uint32_t pitchUV, uint32_t offsetY, uint32_t offsetUV,
                       std::shared_ptr<std::vector<uint8_t>> data);
    void onRemoteDrained(uint64_t generation);
    void onRemoteError(uint64_t generation, int32_t averror);
    void onSocketClosed(uint64_t generation);

    // Output pool plumbing, the same shape VaapiVideoDecoder uses: the pool is created lazily once
    // the first frame size is known, and every in-flight fetch carries a generation so a callback
    // that outlives a flush is dropped instead of popping a frame it no longer owns.
    void pumpOutput();
    void onVideoFrameReady(uint64_t fetchGeneration,
                           std::optional<VideoFramePool::FrameWithBlockId> frameWithBlockId);
    // Completes a pending drain, but only once every frame the helper sent before its RMDD has
    // actually been handed to the component. See mRemoteDrained.
    void maybeCompleteDrain();

    // Layout of the pool's gralloc buffer, read from the cros_gralloc handle (no CPU mapping and
    // no usage bits needed). Mirrors VaapiVideoDecoder::queryOutputBufferInfo.
    struct OutputBufferInfo {
        uint32_t pitchY = 0;
        uint32_t pitchUV = 0;
        uint32_t offsetY = 0;
        uint32_t offsetUV = 0;
        uint64_t totalSize = 0;
        bool fromHandle = false;
    };
    OutputBufferInfo queryOutputBufferInfo(VideoFrame& frame, const ui::Size& codedSize);

    void onError();

    // A decoded frame waiting for a pool buffer, in the order the helper produced it (the host
    // decoder already emits in display order, so no reordering is needed here).
    struct PendingFrame {
        int32_t bitstreamId = -1;
        ui::Size size;
        uint32_t pitchY = 0, pitchUV = 0, offsetY = 0, offsetUV = 0;
        std::shared_ptr<std::vector<uint8_t>> data;
    };

    const uint32_t mDebugStreamId;
    VideoCodec mCodec;
    scoped_refptr<::base::SequencedTaskRunner> mTaskRunner;

    GetPoolCB mGetPoolCb;
    OutputCB mOutputCb;
    ErrorCB mErrorCb;
    size_t mMinNumOutputBuffers = 0;

    int mSock = -1;
    std::thread mReader;
    std::atomic<bool> mReaderStop{false};

    // Bumped by flush() and by teardown. Everything the reader posts carries the generation it was
    // read under, so in-flight work from before a flush is dropped rather than applied to the
    // stream that replaced it.
    uint64_t mGeneration = 0;

    // Correlates helper frames back to input buffers. The helper echoes the pts it was given, so
    // the bitstreamId rides across as the pts and comes back unchanged.
    std::queue<PendingFrame> mPendingFrames;
    std::unique_ptr<VideoFramePool> mVideoFramePool;
    std::optional<OutputBufferInfo> mOutputInfo;
    ui::Size mOutputPoolSize{0, 0};
    bool mOutputFetchInFlight = false;
    uint64_t mFetchGeneration = 0;

    DecodeCB mDrainCb;
    // RMDD has arrived, but the drain is NOT finished until mPendingFrames is empty. The helper
    // flushes its whole reorder queue as a burst of RMDF immediately before RMDD, while this side
    // can only surface one frame per pool round-trip, so at RMDD there are typically
    // output-delay-many frames still queued (bd remora-e5x.37.1).
    bool mRemoteDrained = false;
    bool mErrored = false;

    ::base::WeakPtr<RemoteVideoDecoder> mWeakThis;
    ::base::WeakPtrFactory<RemoteVideoDecoder> mWeakThisFactory{this};
};

}  // namespace android

#endif  // ANDROID_C2_VA_REMOTE_REMOTE_VIDEO_DECODER_H
