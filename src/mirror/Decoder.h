#pragma once
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <memory>
#include <optional>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace remora::mirror {

// A decoded frame, still in the decoder's own pixel format, reference-counted so passing it
// around copies nothing. Convert to QImage only where an actual image is needed (--shot).
struct VideoFrame {
    std::shared_ptr<AVFrame> av;
    QSize size;
    bool isNull() const { return !av; }
    // Software conversion to RGB — the old per-frame path, now used only on demand.
    QImage toImage() const;
};

// libavcodec wrapper: merged media-packet payloads in, RGB frames out. Hardware decode follows
// --hwdec, which the resolver turns on by default; "auto" tries CUDA/NVDEC before VAAPI because
// on a mixed NVIDIA+Intel host the VAAPI surface download fails ENOSYS after one frame (bd
// memory client-gpu-hwdec-crossgpu) — decode must live on the GPU the frames leave through.
class Decoder {
public:
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    ~Decoder();

    // hwdec: "" / "off" / "0" = software; "auto" / "1" = CUDA then VAAPI probe;
    // "cuda" / "nvdec"; "vaapi" (render-node probe, nvidia/nouveau nodes rejected);
    // anything else = an explicit /dev/dri node for VAAPI. Hw setup failure falls back to
    // software with a warning — only a mid-stream download failure is fatal.
    bool init(quint32 codecId, const QString &hwdec, QString *error);
    // Build the hw device context now rather than at the first packet. Optional and idempotent:
    // init() does it itself if this was never called.
    void warmHardware(const QString &hwdec);
    // Zero-copy hand-off (bd remora-28ix.2.3): keep decoded CUDA frames on the GPU instead of
    // downloading them — the render window copies them device-to-device into its textures. The
    // window arms this once its GL context proves willing, and disarms it if interop fails
    // mid-stream. Honored only for CUDA; VAAPI keeps the download path (Option B, unproven on
    // the cross-GPU host this runs on).
    void setKeepHwFrames(bool keep) { keepHwFrames_ = keep; }
    // The decoder device's CUcontext for GL interop registration; null unless hw decode is CUDA.
    void *cudaContext() const;
    // One demuxed payload (config already merged for h264/h265) → zero or more frames.
    //
    // Frames come out as the decoder produced them (NV12 in practice) and are NOT converted here.
    // Converting cost 7.4 ms/frame of CPU colour conversion plus a 3.0 ms 30 MiB copy at
    // 3760x1992, measured — and then the result was uploaded to the GPU anyway, at 4 bytes/pixel
    // instead of 1.5. scrcpy never paid any of it: SDL took the YUV planes and the GPU converted.
    // The renderer does the same now, so this just hands over a reference (bd remora-28ix.2.1).
    QList<VideoFrame> decode(const QByteArray &payload, QString *error);

private:
    void setupHw(const QString &hwdec);
    bool tryHwDevice(int deviceType, const QByteArray &node);  // AVHWDeviceType, widened

    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *swFrame_ = nullptr;  // hw-surface download target
    AVPacket *pkt_ = nullptr;
    AVBufferRef *hwDevice_ = nullptr;
    int hwPixFmt_ = -1;  // AVPixelFormat the get_format callback selects
    bool keepHwFrames_ = false;  // zero-copy: skip the download, hand out the CUDA frame
};

}  // namespace remora::mirror
