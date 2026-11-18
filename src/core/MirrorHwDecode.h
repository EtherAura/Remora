#pragma once
#include <QList>
#include <QString>
#include <QtGlobal>

// Hardware-decode device selection for the mirror client: a --hwdec value → the devices the
// decoder tries, in order. Pure, so which APIs `auto` reaches on which platform is pinned in
// test_mirrorproto.cpp with no FFmpeg and no GPU; src/mirror/Decoder.cpp maps each API to its
// AVHWDeviceType and does the trying (bd remora-7oih.4).

namespace remora::mirror {

enum class HwDecodeApi : quint8 { Cuda, D3d11va, Vaapi };

// The two platforms the client builds for. They differ in which decode APIs exist at all: VAAPI
// and its /dev/dri render nodes are Linux's, D3D11VA is Windows'. CUDA is both.
enum class HwDecodePlatform : quint8 { Linux, Windows };
#ifdef Q_OS_WIN
inline constexpr HwDecodePlatform kHostHwDecodePlatform = HwDecodePlatform::Windows;
#else
inline constexpr HwDecodePlatform kHostHwDecodePlatform = HwDecodePlatform::Linux;
#endif

// One device to try. node is VAAPI's render node, and empty there means "probe the render nodes"
// (the probe reads /dev and /sys, so it is the decoder's, not this layer's). For D3D11VA it is the
// DXGI adapter index, and empty means Windows' default adapter. CUDA takes none: FFmpeg opens the
// first CUDA device.
struct HwDecodeAttempt {
    HwDecodeApi api;
    QString node;
};

// ""/"off"/"0" → false: software by request, which is nothing to warn about.
bool hwDecodeRequested(const QString &hwdec);

// The devices to try; the first that opens wins. Empty for a requested mode means nothing on this
// platform can serve it (vaapi on Windows, d3d11va on Linux, an unknown name on Windows), and the
// decoder says so and decodes in software.
QList<HwDecodeAttempt> hwDecodeAttempts(const QString &hwdec, HwDecodePlatform platform);

}  // namespace remora::mirror
