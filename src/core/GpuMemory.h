#pragma once
#include <QSize>
#include <QString>
#include <optional>

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

// The encoder and its contexts (budgeted from a measured 4K NVENC encoder, 610 MiB), KWin's
// stream buffers for an output of `pixels`, and `reserveMiB` kept free for the desk's own
// monitors.
qint64 sessionNeedMiB(QSize pixels, qint64 reserveMiB);

// Empty when there is room; otherwise the refusal a client shows, naming the numbers.
QString vramRefusal(const VramUsage &usage, qint64 needMiB);

}  // namespace remora::gpumem
