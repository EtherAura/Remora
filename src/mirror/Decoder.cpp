#include "Decoder.h"

#include <QDateTime>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
// The ffnvcodec dynlink header defines CUDA_VERSION, which keeps hwcontext_cuda.h from pulling
// in the CUDA toolkit's cuda.h — the same trick FFmpeg's own build uses.
#include <ffnvcodec/dynlink_cuda.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include <QFileInfo>

#include "core/MirrorProto.h"

namespace remora::mirror {

Decoder::~Decoder() {
    if (pkt_) av_packet_free(&pkt_);
    if (swFrame_) av_frame_free(&swFrame_);
    if (frame_) av_frame_free(&frame_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
}

// The get_format callback: prefer the hw surface format our device provides; any fallback is
// the decoder telling us hw decode is off the table for this stream.
static AVPixelFormat pickPixelFormat(AVCodecContext *ctx, const AVPixelFormat *fmts) {
    const int want = *static_cast<const int *>(ctx->opaque);  // &Decoder::hwPixFmt_
    for (const AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == want) return *f;
    qWarning("mirror: decoder did not offer the hw surface format — falling back to %s",
             av_get_pix_fmt_name(fmts[0]));
    return fmts[0];
}

bool Decoder::tryHwDevice(int deviceType, const QByteArray &node) {
    AVBufferRef *dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AVHWDeviceType(deviceType),
                               node.isEmpty() ? nullptr : node.constData(), nullptr, 0) < 0)
        return false;
    hwDevice_ = dev;
    hwPixFmt_ = deviceType == AV_HWDEVICE_TYPE_CUDA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
    qInfo("mirror: hw decode via %s%s%s",
          av_hwdevice_get_type_name(AVHWDeviceType(deviceType)), node.isEmpty() ? "" : " on ",
          node.constData());
    return true;
}

// Create the hw device context AHEAD of the stream, so its cost does not land wherever the first
// packet happens to arrive. Measured 226 ms for the CUDA context on this machine — paid, before
// this existed, inside init() on the GUI thread at the exact moment the splash hands off, which
// is most of what made the boot animation's ending announce itself (bd remora-xrlh). The context
// is codec-INDEPENDENT, so it can be built before the codec id is known; init() then reuses it.
void Decoder::warmHardware(const QString &hwdec) {
    if (!hwDevice_) setupHw(hwdec);
}

void Decoder::setupHw(const QString &hwdec) {
    const QString mode = hwdec.toLower();
    if (mode.isEmpty() || mode == QLatin1String("off") || mode == QLatin1String("0")) return;
    const bool autoMode = mode == QLatin1String("auto") || mode == QLatin1String("1");
    if (autoMode || mode == QLatin1String("cuda") || mode == QLatin1String("nvdec"))
        if (tryHwDevice(AV_HWDEVICE_TYPE_CUDA, {})) return;
    if (autoMode || mode == QLatin1String("vaapi")) {
        // Probe render nodes like the fork: nvidia/nouveau expose no usable VAAPI decode.
        for (int n = 128; n <= 135; ++n) {
            const QString node = QStringLiteral("/dev/dri/renderD%1").arg(n);
            if (!QFileInfo::exists(node)) continue;
            const QString driver =
                QFileInfo(QStringLiteral("/sys/class/drm/renderD%1/device/driver").arg(n))
                    .symLinkTarget()
                    .section(QLatin1Char('/'), -1);
            if (driver == QLatin1String("nvidia") || driver == QLatin1String("nouveau")) continue;
            if (tryHwDevice(AV_HWDEVICE_TYPE_VAAPI, node.toLocal8Bit())) return;
        }
    } else if (!autoMode) {
        if (tryHwDevice(AV_HWDEVICE_TYPE_VAAPI, mode.toLocal8Bit())) return;  // explicit node
    }
    qWarning("mirror: no usable hw decode device for '%s' — using software decode",
             qUtf8Printable(hwdec));
}

bool Decoder::init(quint32 codecId, const QString &hwdec, QString *error) {
    if (qEnvironmentVariableIsSet("REMORA_MIRROR_FFDEBUG")) av_log_set_level(AV_LOG_VERBOSE);
    AVCodecID id;
    switch (codecId) {
        case kCodecH264: id = AV_CODEC_ID_H264; break;
        case kCodecH265: id = AV_CODEC_ID_HEVC; break;
        case kCodecAv1: id = AV_CODEC_ID_AV1; break;
        default:
            if (error) *error = QStringLiteral("unsupported video codec id 0x%1").arg(codecId, 8, 16);
            return false;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        if (error) *error = QStringLiteral("no decoder for codec id 0x%1").arg(codecId, 8, 16);
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!ctx_ || !frame_ || !pkt_) {
        if (error) *error = QStringLiteral("decoder allocation failed");
        return false;
    }
    // A live mirror must decode with zero pipeline depth: without LOW_DELAY, NVDEC holds the
    // first frames in its reorder buffer, and a static screen (which emits nothing further)
    // never flushes them out — the stream looks stalled while the decoder waits (the reference
    // client sets the same flag for the same reason).
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // Idempotent: warmHardware() usually paid this already (see its comment), and creating a
    // second CUDA context here would be 226 ms wasted, not a second device.
    if (!hwDevice_) setupHw(hwdec);  // best-effort; software remains the fallback
    if (hwDevice_) {
        ctx_->hw_device_ctx = av_buffer_ref(hwDevice_);
        ctx_->opaque = &hwPixFmt_;
        ctx_->get_format = &pickPixelFormat;
        // NVDEC's surface pool is fixed at open. On the zero-copy path the window holds decoded
        // frames instead of downloaded copies — the one on screen, plus one held through the
        // splash ending — and each held frame is a surface the decoder cannot reuse. Without
        // headroom the pool runs dry and decode stalls with "No decoder surfaces left".
        if (hwPixFmt_ == AV_PIX_FMT_CUDA) ctx_->extra_hw_frames = 4;
    }
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        if (error) *error = QStringLiteral("failed to open the %1 decoder").arg(codec->name);
        return false;
    }
    return true;
}

QList<VideoFrame> Decoder::decode(const QByteArray &payload, QString *error) {
    QList<VideoFrame> out;
    if (av_new_packet(pkt_, payload.size()) < 0) return out;
    memcpy(pkt_->data, payload.constData(), payload.size());
    const int sendRc = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (sendRc < 0) {
        // A refused packet is not fatal mid-stream (the next keyframe recovers); report it and
        // let the caller decide.
        if (error) *error = QStringLiteral("decoder refused a packet (rc %1)").arg(sendRc);
        return out;
    }
    for (;;) {
        const int rc = avcodec_receive_frame(ctx_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            if (error) *error = QStringLiteral("decode failed (rc %1)").arg(rc);
            break;
        }
        AVFrame *src = frame_;
        if (hwDevice_ && frame_->format == hwPixFmt_ &&
            !(keepHwFrames_ && hwPixFmt_ == AV_PIX_FMT_CUDA)) {
            // Download the hw surface. On a cross-GPU split this is where VAAPI dies (ENOSYS
            // after one frame, bd client-gpu-hwdec-crossgpu) — fail LOUDLY, never freeze.
            if (!swFrame_) swFrame_ = av_frame_alloc();
            const int drc = av_hwframe_transfer_data(swFrame_, frame_, 0);
            if (drc < 0) {
                if (error)
                    *error = QStringLiteral(
                                 "hw frame download failed (rc %1) — cross-GPU split? use "
                                 "--hwdec off or cuda")
                                 .arg(drc);
                av_frame_unref(frame_);
                break;
            }
            src = swFrame_;
        }
        // Hand over a REFERENCE, not a conversion. av_frame_ref is a refcount bump on the same
        // buffers, so nothing is copied and nothing is converted on this thread.
        VideoFrame vf;
        vf.av = std::shared_ptr<AVFrame>(av_frame_alloc(),
                                         [](AVFrame *f) { av_frame_free(&f); });
        if (vf.av && av_frame_ref(vf.av.get(), src) == 0) {
            vf.size = QSize(src->width, src->height);
            out << vf;
        }
        if (src == swFrame_) av_frame_unref(swFrame_);
        av_frame_unref(frame_);
    }
    return out;
}

void *Decoder::cudaContext() const {
    if (!hwDevice_ || hwPixFmt_ != AV_PIX_FMT_CUDA) return nullptr;
    auto *dev = reinterpret_cast<AVHWDeviceContext *>(hwDevice_->data);
    return static_cast<AVCUDADeviceContext *>(dev->hwctx)->cuda_ctx;
}

// The old per-frame path, kept for the places that genuinely need an image (--shot). Deliberately
// NOT on the render path: at 3760x1992 this measured 7.4 ms of colour conversion plus a 3.0 ms
// 30 MiB copy, per frame.
QImage VideoFrame::toImage() const {
    if (!av) return {};
    AVFrame *f = av.get();
    // A frame the zero-copy path kept on the GPU downloads here first — acceptable, because
    // this function is on-demand (--shot, the non-NV12 fallback), never the render path.
    std::shared_ptr<AVFrame> downloaded;
    if (f->hw_frames_ctx) {
        downloaded =
            std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame *p) { av_frame_free(&p); });
        if (!downloaded || av_hwframe_transfer_data(downloaded.get(), f, 0) < 0) return {};
        f = downloaded.get();
    }
    SwsContext *sws = sws_getContext(f->width, f->height, AVPixelFormat(f->format), f->width,
                                     f->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                                     nullptr);
    if (!sws) return {};
    // Never hand sws_scale a QImage's own bits: its SIMD writers overrun the destination's tail
    // when the width is not a multiple of the vector stride — measured 16 bytes past the end at
    // 1932 wide, and a QImage buffer ends exactly at height*stride. The overrun is hand-written
    // asm, so ASAN never sees it; glibc read it as heap corruption at the image's free, one
    // whole expression later (bd remora-181t). FFmpeg's own image allocators pad for exactly
    // this, so convert into an av_malloc'd buffer with tail slack and let QImage wrap it
    // zero-copy — the QPainter fallback runs this per frame, a 30 MB defensive copy would tax it.
    constexpr size_t kSwsTailSlack = 256;  // ≥ one vector store of any ISA ffmpeg emits
    const qsizetype bpl = qsizetype(f->width) * 4;
    uint8_t *buf = static_cast<uint8_t *>(av_malloc(size_t(bpl) * f->height + kSwsTailSlack));
    if (!buf) {
        sws_freeContext(sws);
        return {};
    }
    uint8_t *dst[4] = {buf, nullptr, nullptr, nullptr};
    int stride[4] = {int(bpl), 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
    sws_freeContext(sws);
    return QImage(buf, f->width, f->height, bpl, QImage::Format_RGB32,
                  [](void *p) { av_free(p); }, buf);
}

}  // namespace remora::mirror
