#include "MirrorHwDecode.h"

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
    const HwDecodeAttempt d3d11va{HwDecodeApi::D3d11va, {}};
    if (mode == QLatin1String("auto") || mode == QLatin1String("1"))
        return windows ? Attempts{cuda, d3d11va} : Attempts{cuda, vaapiProbe};
    if (mode == QLatin1String("cuda") || mode == QLatin1String("nvdec")) return {cuda};
    if (mode == QLatin1String("d3d11va")) return windows ? Attempts{d3d11va} : Attempts{};
    if (mode == QLatin1String("vaapi")) return windows ? Attempts{} : Attempts{vaapiProbe};
    // Anything else names a VAAPI render node, which only Linux has. As given, not lowercased:
    // the nodes are renderD128 and up, and the lowercased path never exists.
    if (windows) return {};
    return {HwDecodeAttempt{HwDecodeApi::Vaapi, hwdec}};
}

}  // namespace remora::mirror
