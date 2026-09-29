// Copyright 2026 The Remora Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//#define LOG_NDEBUG 0
#define LOG_TAG "RemoteVideoDecoder"

#include <c2_va/remote/RemoteVideoDecoder.h>

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/bind.h>
#include <cutils/properties.h>
#include <log/log.h>

namespace android {
namespace {

// The socket path is handed in by the deploy, exactly as the encoder's is (see
// vendor-remora/hwcomposer/frame_export.cpp reading ro.boot.remora_frame_socket). A boot property
// rather than a fixed path because the socket is bind-mounted from the host and its name carries
// the instance, so two containers on one host cannot collide.
constexpr const char* kSocketProp = "ro.boot.remora_decoder_socket";

// Mirror of external/minigbm/cros_gralloc/cros_gralloc_handle.h, the same subset and for the same
// reason as VaapiVideoDecoder's copy: reading the layout from the handle needs no CPU mapping, so
// it works for buffers with no CPU usage bits. |magic| is validated before any field is trusted.
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
    uint32_t format;
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

// Spelled out from the characters rather than written as a hex literal, because a hand-computed
// little-endian fourcc is exactly the kind of constant that can be wrong and look right: AV1 was
// 0x31306176 here against the helper's 0x31307661 — 'a' and 'v' transposed, i.e. "va01". Nothing
// caught it, because the negotiation treats an unrecognised codec as a legitimate refusal and falls
// back to the local decoder, so AV1 silently never used the host at all (bd remora-e5x.37 item 6).
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

uint32_t codecFourcc(VideoCodec codec) {
    // Little-endian fourccs, matching the helper's `codec:` field ("h264" "hevc" "vp9 " "av01").
    switch (codec) {
    case VideoCodec::H264: return fourcc('h', '2', '6', '4');
    case VideoCodec::HEVC: return fourcc('h', 'e', 'v', 'c');
    case VideoCodec::VP9: return fourcc('v', 'p', '9', ' ');
    case VideoCodec::AV1: return fourcc('a', 'v', '0', '1');
    default: return 0;
    }
}

// The three that were already right must stay right: a fourcc helper that silently changed byte
// order would swap a working codec for a refusal, which is the same silent fallback that hid the
// AV1 bug.
static_assert(fourcc('h', '2', '6', '4') == 0x34363268u, "h264 fourcc changed");
static_assert(fourcc('h', 'e', 'v', 'c') == 0x63766568u, "hevc fourcc changed");
static_assert(fourcc('v', 'p', '9', ' ') == 0x20397076u, "vp9 fourcc changed");
static_assert(fourcc('a', 'v', '0', '1') == 0x31307661u, "av01 fourcc must match the helper");

// read()/write() loops. A short count on a stream socket is normal, not an error.
bool readAll(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        const ssize_t r = TEMP_FAILURE_RETRY(::read(fd, p, n));
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool writeAll(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        const ssize_t w = TEMP_FAILURE_RETRY(::write(fd, p, n));
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

}  // namespace

// static
bool RemoteVideoDecoder::configured() {
    char path[PROPERTY_VALUE_MAX] = {};
    return property_get(kSocketProp, path, "") > 0 && path[0] != '\0';
}

// static
std::unique_ptr<VideoDecoder> RemoteVideoDecoder::Create(
        uint32_t debugStreamId, const VideoCodec& codec, size_t /*inputBufferSize*/,
        size_t minNumOutputBuffers, GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner, bool isSecure) {
    // Secure playback never leaves the guest: handing protected content to a host helper over a
    // unix socket would defeat the whole point of the secure path.
    if (isSecure) return nullptr;
    if (codecFourcc(codec) == 0) return nullptr;
    // PER-GPU VERIFICATION STATUS, measured against an ffmpeg software reference, because
    // "the codec works" is not one fact — it is one per driver (bd remora-e5x, GOVERNING LESSON):
    //                     Intel iHD (renderD129)      NVIDIA NVDEC (renderD128)
    //     h264            60/60 byte-identical        60/60 byte-identical
    //     hevc            60/60 byte-identical        60/60 byte-identical
    //     vp9            100/100 byte-identical         7/100  <- NOT bit-exact
    // The vp9 deviation on NVIDIA is real but tiny and structured: the frames that DO match are
    // exactly 0,15,30,45,... on a -g 15 clip, i.e. every keyframe, so inter prediction drifts and
    // each GOP resets it. 0.01% of luma samples differ, by at most 3/255. It is not gated, on the
    // judgement that it is visually undetectable and that gating per-GPU would need machinery that
    // does not exist here (the driver is chosen at runtime, this decision is per-codec) — but it IS
    // a departure from this project's byte-exactness bar and should be a deliberate choice, not a
    // discovery. Revisit if vp9 artefacts are ever reported on an NVIDIA host.
    //
    // AV1 IS NOT CONTENT-VERIFIED, so it does not go remote yet (bd remora-e5x.37 item 6, whose rule
    // is that a codec is advertised as remote only after frames have been byte-compared against a
    // software reference — h264 60/60, hevc 60/60 and vp9 100/100 have been, AV1 has not). It is
    // gated here rather than by returning 0 from codecFourcc(), so the wire constant keeps its
    // static_assert against the helper's and cannot rot while the codec is switched off. Both GPUs
    // on the reference host advertise AV1Profile0 decode and the helper's ffmpeg can drive it; the
    // only thing missing is a stream — neither GPU can ENCODE AV1 and this ffmpeg has no software
    // AV1 encoder, so no test clip could be produced locally. Delete these two lines once an AV1
    // clip has been through remora-decode-test and matched the reference.
    if (codec == VideoCodec::AV1) return nullptr;

    std::unique_ptr<RemoteVideoDecoder> decoder(
            new RemoteVideoDecoder(debugStreamId, codec, std::move(taskRunner)));
    if (!decoder->start(codec, minNumOutputBuffers, std::move(getPoolCb), std::move(outputCb),
                        std::move(errorCb))) {
        return nullptr;
    }
    return decoder;
}

RemoteVideoDecoder::RemoteVideoDecoder(uint32_t debugStreamId, VideoCodec codec,
                                       scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mDebugStreamId(debugStreamId), mCodec(codec), mTaskRunner(std::move(taskRunner)) {
    mWeakThis = mWeakThisFactory.GetWeakPtr();
}

RemoteVideoDecoder::~RemoteVideoDecoder() {
    mWeakThisFactory.InvalidateWeakPtrs();
    mReaderStop = true;
    // shutdown() rather than close(): the reader is parked in recvmsg and only a half-close wakes
    // it. Closing the fd out from under a blocked reader is how you get a use-after-close on a
    // descriptor number the kernel has already handed to someone else.
    if (mSock >= 0) ::shutdown(mSock, SHUT_RDWR);
    if (mReader.joinable()) mReader.join();
    if (mSock >= 0) ::close(mSock);
}

bool RemoteVideoDecoder::start(const VideoCodec& codec, size_t minNumOutputBuffers,
                               GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb) {
    mMinNumOutputBuffers = minNumOutputBuffers;
    mGetPoolCb = std::move(getPoolCb);
    mOutputCb = std::move(outputCb);
    mErrorCb = std::move(errorCb);

    if (!connectAndNegotiate(codec)) return false;

    mReader = std::thread(&RemoteVideoDecoder::readerLoop, this);
    ALOGI("[%u] remote decoder up for %s", mDebugStreamId, VideoCodecToString(codec));
    return true;
}

bool RemoteVideoDecoder::connectAndNegotiate(const VideoCodec& codec) {
    char path[PROPERTY_VALUE_MAX] = {};
    if (property_get(kSocketProp, path, "") <= 0 || path[0] == '\0') {
        ALOGV("[%u] %s unset — no remote decoder", mDebugStreamId, kSocketProp);
        return false;
    }

    const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        ALOGE("[%u] socket(): %s", mDebugStreamId, strerror(errno));
        return false;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    // LIVENESS IS A connect(), NOT AN INODE TEST (bd remora-e5x.13): a stale socket file outlives
    // the helper that made it, so `test -e` reports a decoder that is not there and the component
    // disables its working local fallback in favour of a corpse.
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ALOGI("[%u] no decoder helper at %s (%s) — using the local VA decoder", mDebugStreamId,
              path, strerror(errno));
        ::close(s);
        return false;
    }

    const DecHello hello = {kHelloMagic, 1, codecFourcc(codec), kModeLinear, 0, 0};
    DecAccept accept = {};
    if (!writeAll(s, &hello, sizeof(hello)) || !readAll(s, &accept, sizeof(accept)) ||
        accept.magic != kAcceptMagic || accept.mode != kModeLinear) {
        // A refusal is legitimate: the helper rejects codecs its ffmpeg cannot decode, and that
        // must fall back rather than fail the component.
        ALOGI("[%u] decoder helper refused %s — using the local VA decoder", mDebugStreamId,
              VideoCodecToString(codec));
        ::close(s);
        return false;
    }

    mSock = s;
    return true;
}

void RemoteVideoDecoder::readerLoop() {
    // Everything here runs OFF the task runner and touches nothing the decoder owns except the
    // socket: results are posted, never applied in place.
    while (!mReaderStop) {
        // THE SCM_RIGHTS TRAP, recorded in remora-frame-decoder.c and non-negotiable: the frame's
        // descriptors are delivered with the FIRST BYTE of the sendmsg. Reading the 4-byte magic
        // with a plain read() and only then recvmsg()ing the rest has ALREADY discarded them — the
        // kernel closes them silently, and the symptom is n_fds == 0 with nothing else wrong. So
        // the magic itself must be received with a control buffer attached.
        uint32_t magic = 0;
        iovec iov = {&magic, sizeof(magic)};
        union {
            cmsghdr align;
            char buf[CMSG_SPACE(sizeof(int) * 4)];
        } control = {};
        msghdr mh = {};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = control.buf;
        mh.msg_controllen = sizeof(control.buf);

        const ssize_t n = TEMP_FAILURE_RETRY(::recvmsg(mSock, &mh, 0));
        if (n != static_cast<ssize_t>(sizeof(magic))) break;  // closed or short: session over

        int fds[4];
        int nFds = 0;
        for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm != nullptr; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            nFds = static_cast<int>((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (nFds > 4) nFds = 4;
            memcpy(fds, CMSG_DATA(cm), sizeof(int) * nFds);
        }

        if (magic == kFrameMagic) {
            DecFrame f = {};
            f.magic = magic;
            if (!readAll(mSock, reinterpret_cast<uint8_t*>(&f) + sizeof(f.magic),
                         sizeof(f) - sizeof(f.magic))) {
                for (int i = 0; i < nFds; ++i) ::close(fds[i]);
                break;
            }
            // LINEAR: one memfd holding the whole NV12 image. Copy it out here, on this thread,
            // and close the fd immediately — the helper recycles the surface behind it as soon as
            // the next frame is produced, and holding descriptors across a post would pin them for
            // an unbounded time.
            std::shared_ptr<std::vector<uint8_t>> data;
            if (nFds >= 1 && f.total_size > 0) {
                void* p = ::mmap(nullptr, f.total_size, PROT_READ, MAP_SHARED, fds[0], 0);
                if (p != MAP_FAILED) {
                    data = std::make_shared<std::vector<uint8_t>>(
                            static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + f.total_size);
                    ::munmap(p, f.total_size);
                }
            }
            for (int i = 0; i < nFds; ++i) ::close(fds[i]);
            if (!data) {
                ALOGE("[%u] could not map remote frame payload", mDebugStreamId);
                continue;
            }
            mTaskRunner->PostTask(
                    FROM_HERE,
                    ::base::BindOnce(&RemoteVideoDecoder::onRemoteFrame, mWeakThis, mGeneration,
                                     static_cast<int32_t>(f.pts),
                                     ui::Size(static_cast<int32_t>(f.width),
                                              static_cast<int32_t>(f.height)),
                                     f.pitch[0], f.pitch[1], f.offset[0], f.offset[1],
                                     std::move(data)));
            continue;
        }

        for (int i = 0; i < nFds; ++i) ::close(fds[i]);  // no other message carries descriptors

        if (magic == kDrainedMagic || magic == kErrorMagic) {
            DecSimple s = {};
            s.magic = magic;
            if (!readAll(mSock, reinterpret_cast<uint8_t*>(&s) + sizeof(s.magic),
                         sizeof(s) - sizeof(s.magic))) {
                break;
            }
            if (magic == kDrainedMagic) {
                mTaskRunner->PostTask(FROM_HERE,
                                      ::base::BindOnce(&RemoteVideoDecoder::onRemoteDrained,
                                                       mWeakThis, mGeneration));
            } else {
                // A per-AU decode failure is a NOTICE, not a session death — the helper keeps
                // going and so must we, or one corrupt access unit kills playback.
                mTaskRunner->PostTask(FROM_HERE,
                                      ::base::BindOnce(&RemoteVideoDecoder::onRemoteError,
                                                       mWeakThis, mGeneration,
                                                       static_cast<int32_t>(s.value)));
            }
            continue;
        }

        ALOGE("[%u] bad magic 0x%08x from the decoder helper", mDebugStreamId, magic);
        break;
    }

    mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(&RemoteVideoDecoder::onSocketClosed,
                                                      mWeakThis, mGeneration));
}

void RemoteVideoDecoder::decode(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB decodeCb) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (mErrored) {
        std::move(decodeCb).Run(VideoDecoder::DecodeStatus::kError);
        return;
    }

    // Input access units already arrive as mappable dma-bufs, so this is a map + write rather than
    // a second copy of the bitstream through the C2 layer.
    C2ReadView view = buffer->dmabuf.map().get();
    if (view.error() != C2_OK) {
        ALOGE("[%u] failed to map input dma-buf: %d", mDebugStreamId, view.error());
        onError();
        std::move(decodeCb).Run(VideoDecoder::DecodeStatus::kError);
        return;
    }
    // C2ReadView::data() ALREADY points at the block's logical start — map() applies the block
    // offset — so adding buffer->offset here would double-count it. The same trap is called out
    // in VaapiVideoDecoder::pumpDecodeRequests.
    const DecPacket ph = {kPacketMagic, static_cast<uint32_t>(buffer->size),
                          static_cast<uint64_t>(buffer->id), 0, 0};
    const bool ok = writeAll(mSock, &ph, sizeof(ph)) && writeAll(mSock, view.data(), buffer->size);

    if (!ok) {
        ALOGE("[%u] writing an AU to the decoder helper failed", mDebugStreamId);
        onError();
        std::move(decodeCb).Run(VideoDecoder::DecodeStatus::kError);
        return;
    }
    // The callback reports that the INPUT was consumed, not that a frame came out — the frames
    // arrive asynchronously on the reader thread.
    std::move(decodeCb).Run(VideoDecoder::DecodeStatus::kOk);
}

void RemoteVideoDecoder::drain(DecodeCB drainCb) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (mErrored) {
        std::move(drainCb).Run(VideoDecoder::DecodeStatus::kError);
        return;
    }
    mDrainCb = std::move(drainCb);
    const DecPacket ph = {kPacketMagic, 0, 0, kPktEos, 0};
    if (!writeAll(mSock, &ph, sizeof(ph))) {
        ALOGE("[%u] writing EOS to the decoder helper failed", mDebugStreamId);
        onError();
        if (mDrainCb) std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kError);
        return;
    }
    // Completion waits for RMDD — the helper drains its reorder queue first, so returning here
    // would drop the tail of every stream.
}

void RemoteVideoDecoder::flush() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    // The session survives a flush by design (the helper's EOS is drain + flush, seek-ready), so
    // there is nothing to send: bumping the generation is what makes every frame still in flight —
    // on the reader thread, in a posted task, or waiting on a pool fetch — belong to the stream
    // that is being abandoned rather than the one about to start.
    ++mGeneration;
    ++mFetchGeneration;
    mOutputFetchInFlight = false;
    while (!mPendingFrames.empty()) mPendingFrames.pop();
    // A flush abandons the stream the drain belonged to, so a deferred completion must not survive
    // into the next one and report kOk for frames that were just dropped.
    mRemoteDrained = false;
    if (mDrainCb) std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kAborted);
}

void RemoteVideoDecoder::onRemoteFrame(uint64_t generation, int32_t bitstreamId, ui::Size size,
                                       uint32_t pitchY, uint32_t pitchUV, uint32_t offsetY,
                                       uint32_t offsetUV,
                                       std::shared_ptr<std::vector<uint8_t>> data) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (generation != mGeneration || mErrored) return;  // from before a flush

    if (!mVideoFramePool || mOutputPoolSize.width < size.width ||
        mOutputPoolSize.height < size.height) {
        // Grow only, for the same reason the VA path does: an adaptive-stream switch that shrank
        // the pool would stall waiting for the framework to hand back the larger buffers.
        mOutputPoolSize.width = std::max(mOutputPoolSize.width, size.width);
        mOutputPoolSize.height = std::max(mOutputPoolSize.height, size.height);
        mVideoFramePool = mGetPoolCb.Run(mOutputPoolSize, HalPixelFormat::YCBCR_420_888,
                                         mMinNumOutputBuffers);
        mOutputInfo.reset();
        ++mFetchGeneration;
        mOutputFetchInFlight = false;
        if (!mVideoFramePool) {
            ALOGE("[%u] could not create the output pool", mDebugStreamId);
            onError();
            return;
        }
    }

    mPendingFrames.push(PendingFrame{bitstreamId, size, pitchY, pitchUV, offsetY, offsetUV,
                                     std::move(data)});
    pumpOutput();
}

void RemoteVideoDecoder::onRemoteDrained(uint64_t generation) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (generation != mGeneration) return;
    // RMDD means the HELPER has no more frames, not that WE have delivered them. Completing the
    // drain here unconditionally reported EOS to the framework with mPendingFrames still full, and
    // every frame left in it was silently abandoned — 40 of 60 delivered on h264, 84 of 100 on vp9,
    // the shortfall tracking the reorder window each time (bd remora-e5x.37.1).
    mRemoteDrained = true;
    maybeCompleteDrain();
}

void RemoteVideoDecoder::maybeCompleteDrain() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (!mRemoteDrained || !mDrainCb) return;
    // An in-flight fetch will deliver another frame and re-drive us from onVideoFrameReady, so it
    // counts as outstanding work exactly as a queued frame does.
    if (!mPendingFrames.empty() || mOutputFetchInFlight) return;
    mRemoteDrained = false;
    std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kOk);
}

void RemoteVideoDecoder::onRemoteError(uint64_t generation, int32_t averror) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (generation != mGeneration) return;
    // Deliberately not fatal: the helper reports per-AU failures and keeps the session, so a
    // single bad access unit costs a frame rather than the stream.
    ALOGW("[%u] remote decode error %d on one access unit — continuing", mDebugStreamId, averror);
}

void RemoteVideoDecoder::onSocketClosed(uint64_t generation) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (generation != mGeneration) return;
    if (mErrored) return;
    ALOGE("[%u] the decoder helper closed the session", mDebugStreamId);
    onError();
}

void RemoteVideoDecoder::pumpOutput() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (mOutputFetchInFlight || mPendingFrames.empty() || !mVideoFramePool) return;

    mOutputFetchInFlight = true;
    if (!mVideoFramePool->getVideoFrame(::base::BindOnce(&RemoteVideoDecoder::onVideoFrameReady,
                                                         mWeakThis, mFetchGeneration))) {
        mOutputFetchInFlight = false;  // a previous callback is still running; it re-drives us
    }
}

void RemoteVideoDecoder::onVideoFrameReady(
        uint64_t fetchGeneration, std::optional<VideoFramePool::FrameWithBlockId> frameWithBlockId) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (fetchGeneration != mFetchGeneration) return;  // fetched before a flush or a pool reset
    mOutputFetchInFlight = false;
    if (mPendingFrames.empty()) return;

    if (!frameWithBlockId) {
        ALOGE("[%u] null VideoFrame from the pool", mDebugStreamId);
        onError();
        return;
    }
    std::unique_ptr<VideoFrame> frame;
    uint32_t blockId;
    std::tie(frame, blockId) = std::move(*frameWithBlockId);
    (void)blockId;

    PendingFrame pf = std::move(mPendingFrames.front());
    mPendingFrames.pop();

    const OutputBufferInfo info = queryOutputBufferInfo(*frame, pf.size);
    const int outFd = frame->getFDs().empty() ? -1 : frame->getFDs()[0];
    if (outFd < 0 || info.pitchY == 0) {
        ALOGE("[%u] output buffer has no usable fd/layout", mDebugStreamId);
        onError();
        return;
    }

    const size_t mapSize = info.totalSize > 0
                                   ? static_cast<size_t>(info.totalSize)
                                   : info.offsetUV + info.pitchUV * (pf.size.height / 2);
    void* dst = ::mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, outFd, 0);
    if (dst == MAP_FAILED) {
        ALOGE("[%u] mmap of the output buffer failed: %s", mDebugStreamId, strerror(errno));
        onError();
        return;
    }

    // Row-at-a-time because the two pitches rarely match: the helper packs to the frame width and
    // gralloc aligns to its own stride, so a flat memcpy would shear the image.
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = pf.data->data();
    const uint32_t copyY = std::min(pf.pitchY, info.pitchY);
    for (int32_t y = 0; y < pf.size.height; ++y) {
        memcpy(d + info.offsetY + static_cast<size_t>(y) * info.pitchY,
               s + pf.offsetY + static_cast<size_t>(y) * pf.pitchY, copyY);
    }
    const uint32_t copyUV = std::min(pf.pitchUV, info.pitchUV);
    for (int32_t y = 0; y < pf.size.height / 2; ++y) {
        memcpy(d + info.offsetUV + static_cast<size_t>(y) * info.pitchUV,
               s + pf.offsetUV + static_cast<size_t>(y) * pf.pitchUV, copyUV);
    }
    ::munmap(dst, mapSize);

    frame->setVisibleRect(Rect(0, 0, pf.size.width, pf.size.height));
    frame->setBitstreamId(pf.bitstreamId);
    mOutputCb.Run(std::move(frame));

    pumpOutput();        // more may be queued
    maybeCompleteDrain();  // ...and if not, a drain waiting on them can finish now
}

RemoteVideoDecoder::OutputBufferInfo RemoteVideoDecoder::queryOutputBufferInfo(
        VideoFrame& frame, const ui::Size& codedSize) {
    if (mOutputInfo) return *mOutputInfo;

    OutputBufferInfo info;
    const C2ConstGraphicBlock block = frame.getGraphicBlock();
    const native_handle_t* h = block.handle();
    if (h != nullptr && h->numInts >= 0) {
        const auto* cros = reinterpret_cast<const CrosGrallocHandle*>(h);
        if (cros->magic == kCrosGrallocMagic && cros->numPlanes >= 2) {
            info.pitchY = cros->strides[0];
            info.pitchUV = cros->strides[1];
            info.offsetY = cros->offsets[0];
            info.offsetUV = cros->offsets[1];
            info.totalSize = cros->totalSize;
            info.fromHandle = true;
        }
    }
    if (!info.fromHandle) {
        // No cros handle: assume tightly packed NV12 at the coded width. Correct for the linear
        // allocations this path asks for, and the only thing that can be assumed without a map.
        info.pitchY = static_cast<uint32_t>(codedSize.width);
        info.pitchUV = static_cast<uint32_t>(codedSize.width);
        info.offsetY = 0;
        info.offsetUV = info.pitchY * static_cast<uint32_t>(codedSize.height);
        info.totalSize = info.offsetUV + info.pitchUV * (codedSize.height / 2);
    }
    mOutputInfo = info;
    return info;
}

void RemoteVideoDecoder::onError() {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    if (mErrored) return;
    mErrored = true;
    if (mDrainCb) std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kError);
    mErrorCb.Run();
}

}  // namespace android
