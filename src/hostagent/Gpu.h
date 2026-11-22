#pragma once
#include <QString>
#include <optional>

#include "core/GpuMemory.h"

namespace remora::hostagent {

// The render node of the GPU a connector hangs off ("HDMI-A-4" → "/dev/dri/renderD129"), or
// empty. The encoder must import KWin's buffers on the GPU that rendered them, and KWin renders
// on the GPU driving the desk's primary output — which a two-GPU desk makes a real question, not
// a default to assume.
QString renderNodeForConnector(const QString &connector);

// The video memory of the GPU behind a render node: nvidia-smi for NVIDIA, sysfs for amdgpu.
// nullopt for anything else — an integrated GPU shares system memory and has no VRAM to run out
// of — and when the reading fails, which the caller treats as "no limit known".
std::optional<gpumem::VramUsage> vramUsage(const QString &renderNode);

}  // namespace remora::hostagent
