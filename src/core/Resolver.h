#pragma once
#include <QSet>
#include <QString>
#include <optional>

#include "core/Caps.h"
#include "core/Config.h"
#include "core/Resolved.h"

namespace remora {

// Does a DRM driver name imply a VA-API driver with ENCODE entrypoints?
//
// Distinct from "has a VA driver at all", and the difference is the whole point: NVIDIA HAS a VA
// driver and it loads cleanly, but it is the NVDEC driver — every profile is VAEntrypointVLD and
// there are ZERO VAEntrypointEnc*, measured with vainfo. So a host whose only GPU is NVIDIA can
// decode but cannot encode, and pinning the VAAPI HEVC encoder there yields a component that never
// starts and a mirror that never connects (bd remora-e5x.4).
//
// Shared by the resolver (which downgrades h265 -> h264) and the preflight (which reports it), so
// the two can never disagree about what the host can do.
bool driverHasVaEncode(const QString &drmDriver);

// Substitute the proven per-backend default for each unset field. gpuNode is only ever a live
// probed node, never a bake — see resolveHostGpu().
ResolvedConfig resolve(const RemoraConfig &cfg, Backend backend,
                       const std::optional<HostCapabilities> &caps = std::nullopt);

// The host-encoder socket pair for a container NAME. Shared by the resolver (which pins the
// paths into the resolved config) and the teardown reap (which has only the name — a conflict
// takeover stops a container whose profile it cannot resolve), so the two can never disagree
// about where a container's encoder lives (bd remora-e5x.19.1).
QString hostEncodeFrameSocketDirFor(const QString &containerName);
QString hostEncodeFrameSocketFor(const QString &containerName);
QString hostEncodeVideoSocketFor(const QString &containerName);
// Host-side decode helper (bd remora-e5x.37). A DIRECTORY for the bind mount plus the
// socket inside it, from one rule so the container mounts what the helper binds.
QString hostDecodeSocketDirFor(const QString &containerName);
QString hostDecodeSocketFor(const QString &containerName);
QString hostEncodeStatusFileFor(const QString &containerName);

// ─────────── per-profile identity, pinned at creation ───────────
//
// The three fields two concurrently-running profiles must never share. They are PINNED into the
// config when a profile is created rather than defaulted in `resolve`, because the resolver never
// sees the profile name and so cannot tell two profiles apart (bd remora-4u4.2). The per-backend
// defaults in `resolve` stay exactly as they are: they are what an EXISTING profile that pins
// nothing already resolves to, and changing them would silently repoint a live instance's /data
// at an empty directory.
//
// Not included: dataBaseDir. Under use_overlayfs it is the SHARED lower layer that every profile
// on the host is meant to bind — per-profile is precisely wrong for it.
struct InstanceIdentity {
    QString containerName;
    QString dataDir;
    int hostAdbPort;
};

// "Android 17" -> "android-17". Lowercased, every run of non-alphanumerics collapsed to one dash,
// no leading/trailing dash. Empty or all-punctuation input yields "profile" so the caller always
// gets a usable path component.
QString profileSlug(const QString &profileName);

// `takenPorts` is the set of host adb ports already spoken for by other profiles that actually
// BIND one (a macvlan profile does not — it has its own LAN address). The allocated port is the
// lowest free one at or above `basePort`, so the first profile on a fresh install still gets 5555
// and a single-instance setup is unchanged.
InstanceIdentity allocateInstanceIdentity(const QString &profileName, const QString &homeDir,
                                          const QSet<int> &takenPorts, int basePort = 5555);

}  // namespace remora
