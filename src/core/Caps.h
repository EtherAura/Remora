#pragma once
#include <QList>
#include <QString>
#include <optional>

#include "core/Parsers.h"  // RenderNode, LanInterface

namespace remora {

// Read-only host-capability snapshot (populated by the live probe). resolve() consumes it and,
// per the capability-drift rule, only ever uses a LIVE render node — it never bakes one.
struct HostCapabilities {
    // Scanned on the DOCKER HOST: the render node the probe nominates for the host-GPU path — the
    // first one the container could actually open, any driver but nvidia (reached through Venus
    // below) and vgem — with the DRM driver that identified it kept alongside: the resolver's
    // gralloc rule is a driver decision, not a node-identity one.
    std::optional<QString> hostRenderNode;
    std::optional<QString> hostRenderDriver;  // DRM driver of hostRenderNode ("xe")
    // EVERY probed render node with its driver, nvidia and all — the multi-GPU picker's data
    // (bd remora-e5x.1). hostRenderNode above stays the automatic first pick; gpu_driver= selects
    // from this list by driver name, resolved live at each deploy because node NUMBERS rotate
    // across boots while drivers do not.
    QList<RenderNode> renderNodes;
    // Live /dev/video* on the docker host — the camera auto-pass source (bd remora-4ei.9).
    QStringList videoNodes;
    // NVIDIA Venus path (bd remora-4ei.56). The guest never talks to the NVIDIA node directly —
    // its Vulkan goes over a unix socket to a host virglrenderer render server that owns the
    // proprietary driver — so these three are what decide whether the path is *available*, and
    // resolve() reads them instead of guessing. All three present = the stack can come up.
    std::optional<QString> hostNvidiaRenderNode;  // first renderD* whose DRM driver is "nvidia"
    // Loaded NVIDIA module vs installed userspace (bd remora-81cp): a driver update without a
    // reboot leaves these disagreeing, and the whole Venus path then fails per-client with
    // 'NVRM: API mismatch' while every installed piece above still reports present.
    NvidiaVersions nvidiaVersions;
    std::optional<QString> venusServerPath;       // built virgl_test_server, if it exists
    std::optional<QString> venusLibDir;           // dir holding ALL the prebuilt guest .so files
    // Host-side video encode (bd remora-e5x.10). Reported even when off, because otherwise there
    // is no way to discover the feature or see which piece is short — the same reasoning as the
    // Venus trio above. A host missing any of these otherwise finds out at connect, as a log line.
    std::optional<QString> hostEncoderPath;  // built remora-frame-encoder, if it exists
    bool ffmpegHevcVulkan = false;           // this ffmpeg can encode HEVC on Vulkan
    bool venusCapturePatched = false;        // the installed render server publishes frames
    // The DOCKER HOST's primary IPv4 LAN attachment — the macvlan network's parent/subnet/gateway
    // (bd remora-400). Probed live for the same reason the render node is: a subnet baked into a
    // config file is wrong the first time the machine moves network, and the failure (a container
    // on a subnet the LAN does not route) looks like a broken deployment rather than stale config.
    std::optional<LanInterface> hostLan;
    bool modAshmem = false;
    bool inDockerGroup = false;
    // The LOCAL docker daemon answered `docker version` — group membership alone said nothing
    // about a daemon that is down or a DOCKER_HOST pointing nowhere, and `check` read READY.
    std::optional<bool> dockerDaemonOk;
    bool adbPresent = false;                // local clients every target's connect launches
    // python3 on the DOCKER HOST — remora-input-share.py's interpreter (bd remora-4ei.36). Probed
    // where the devices live: locally for bare, over ssh for remote.
    bool python3Present = false;
    std::optional<bool> sshReachable;       // remote only
    std::optional<bool> remoteDockerOk;
};

}  // namespace remora
