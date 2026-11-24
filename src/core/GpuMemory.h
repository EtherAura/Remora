#pragma once
#include <QList>
#include <QSize>
#include <QString>
#include <optional>

#include "OutputLayout.h"

// Whether a phone session fits in the GPU that drives the desk (docs/PHONE_APP.md).
// Pure: the agent reads the numbers, this decides.
//
// Why it matters, measured on a desk whose 8 GB card sat at 6.9 GB: the NVIDIA driver could not
// allocate a monitor's scanout buffers ("Failed to allocate NVKMS memory for GEM object") and
// the monitor stayed dark until memory was freed. A session adds an encoder and a virtual
// output's buffers to the same card, so it must not start when the desk's own monitors would be
// left without room.

namespace remora::gpumem {

struct VramUsage {
    qint64 usedMiB = 0, totalMiB = 0;
    qint64 freeMiB() const { return totalMiB - usedMiB; }
};

// `nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader,nounits`: "5817, 8192".
std::optional<VramUsage> parseNvidiaSmi(const QString &out);
// amdgpu's sysfs mem_info_vram_used and mem_info_vram_total, both in bytes.
std::optional<VramUsage> fromAmdgpuBytes(const QString &used, const QString &total);
// "0000:01:00.0" (sysfs) → "00000000:01:00.0", the form `nvidia-smi -i` takes.
QString nvidiaBusId(const QString &pciAddress);

// What a session for an output of `pixels` takes from the GPU, plus `reserveMiB` that must stay
// free beside it. The encoder is budgeted from remora-frame-encoder as measured (HEVC on NVENC,
// RTX 3070 Ti): 242 MiB at 1280x678, 297 at 1920x1016, 423 at 2992x1344 — about 192
// fixed (a Vulkan device and the CUDA context derived from it) and 57 a megapixel — and 687 at
// 3760x1992 earlier; KWin's stream buffers and the app's own window are frames of the output.
qint64 sessionNeedMiB(QSize pixels, qint64 reserveMiB);

// What the desk's lit monitors need free while a session runs: their screen buffers (three each,
// four bytes a pixel, eight for an HDR output), twice over, since a mode set — a monitor waking,
// say — allocates the new ones before it frees the old; never less than 256 MiB.
qint64 monitorReserveMiB(const QList<outputs::OutputState> &outputs);

// Empty when there is room; otherwise the refusal a client shows, naming the numbers.
QString vramRefusal(const VramUsage &usage, qint64 needMiB);

}  // namespace remora::gpumem
