#include "MirrorHwDecode.h"

#include "MirrorProto.h"

namespace remora::mirror {

bool hwDecodeRequested(const QString &hwdec) {
    const QString mode = hwdec.toLower();
    return !(mode.isEmpty() || mode == QLatin1String("off") || mode == QLatin1String("0"));
}

// `auto` tries CUDA first on both platforms.
//
// On Linux that is the cross-GPU lesson: on a mixed NVIDIA+Intel host the VAAPI surface download
// fails ENOSYS after one frame (bd memory client-gpu-hwdec-crossgpu), so decode must live on the
// GPU the frames leave through, and VAAPI is the fallback for hosts without NVIDIA's driver.
//
// On Windows the reason is the render path. CUDA is the only API with a zero-copy route into the
// window's textures (CudaInterop, which the Windows build carries too); a D3D11VA frame is
// downloaded to system memory and uploaded again. That round trip, measured for CUDA on Linux,
// was the whole gap between ~30 fps drawn and the stream's 58-60 at 3760x1992 (bd
// remora-28ix.2.3). Without NVIDIA's driver nvcuda.dll is absent, the CUDA attempt fails at
// load, and every AMD and Intel host lands on D3D11VA. The price is paid on a hybrid laptop
// whose GL window runs on the integrated GPU: CUDA decodes on the discrete one, interop fails
// at the first frame, and frames cross the bus both ways. That still draws, and
// mirror_extra=--hwdec=d3d11va decodes on Windows' default adapter instead.
QList<HwDecodeAttempt> hwDecodeAttempts(const QString &hwdec, HwDecodePlatform platform) {
    if (!hwDecodeRequested(hwdec)) return {};
    const QString mode = hwdec.toLower();
    const bool windows = platform == HwDecodePlatform::Windows;
    using Attempts = QList<HwDecodeAttempt>;
    const HwDecodeAttempt cuda{HwDecodeApi::Cuda, {}};
    const HwDecodeAttempt vaapiProbe{HwDecodeApi::Vaapi, {}};
    // Windows' default adapter first, then the others by index. The adapter that can decode is not
    // always the default one: on the Windows test VM the default was a display-only virtual
    // adapter, the Intel GPU was adapter 1, and opening only the default meant software decode
    // with a working GPU beside it. Remote Desktop's adapter and a second GPU are the same shape.
    // An index past the last adapter fails to open at once.
    const Attempts d3d11va{{HwDecodeApi::D3d11va, {}},
                           {HwDecodeApi::D3d11va, QStringLiteral("1")},
                           {HwDecodeApi::D3d11va, QStringLiteral("2")},
                           {HwDecodeApi::D3d11va, QStringLiteral("3")}};
    if (mode == QLatin1String("auto") || mode == QLatin1String("1"))
        return windows ? Attempts{cuda} + d3d11va : Attempts{cuda, vaapiProbe};
    if (mode == QLatin1String("cuda") || mode == QLatin1String("nvdec")) return {cuda};
    if (mode == QLatin1String("d3d11va")) return windows ? d3d11va : Attempts{};
    if (mode == QLatin1String("vaapi")) return windows ? Attempts{} : Attempts{vaapiProbe};
    // Anything else names a VAAPI render node, which only Linux has. As given, not lowercased:
    // the nodes are renderD128 and up, and the lowercased path never exists.
    if (windows) return {};
    return {HwDecodeAttempt{HwDecodeApi::Vaapi, hwdec}};
}

std::optional<VideoDecoder> videoDecoderFor(quint8 codecId) {
    switch (codecId) {
        case kCodecH264: return VideoDecoder::H264;
        case kCodecH265: return VideoDecoder::H265;
        case kCodecAv1: return VideoDecoder::Av1;
        default: return std::nullopt;
    }
}

}  // namespace remora::mirror
