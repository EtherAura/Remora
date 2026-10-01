#pragma once
#include <QString>
#include <QStringList>
#include <optional>

#include "core/Backend.h"

namespace remora {

// The fully-concrete config the builders consume — no Optionals to reason about, except the
// live-capability fields (gpuNode, gralloc) which are legitimately absent without a
// probe.
struct ResolvedConfig {
    Backend backend;
    QString imageTag, containerName, dataDir;
    // The Android release the profile names (android_version=, default 16) — the same number the
    // image tag was derived from. Asked where the release changes what the HOST must provide.
    int androidVersion = 16;
    QString dataBaseDir;  // overlayfs shared lower layer — mounted only when useOverlayfs
    std::optional<QString> rezmodsDir;  // shared staging tree on the docker host (opt-in)
    int width, height, dpi;
    bool useMemfd;
    GpuMode gpuMode;
    std::optional<QString> gpuNode;
    // /dev/dri entries of the NON-selected GPUs (render nodes + card siblings), each to be hidden
    // behind a /dev/null bind. A privileged container sees every node regardless of --device, and
    // the in-container stack does its own device discovery — on amd-host-b it rendered on a
    // TeraScale card while every Remora-side selection named the RX 550 (bd remora-n4qx). Empty
    // on single-GPU hosts, so their argv is untouched. The venus node is never in this list.
    QStringList maskedGpuNodes;
    // Absent → the image's own default applies (gpu_config.sh: gbm). Only ever set for host mode.
    std::optional<QString> gralloc;
    // Composer HAL name (bd remora-emoe). Absent → the image's own default, Remora's own HWC2 HAL
    // (the prebuilt left the image with bd remora-28ix.4.3). Unlike gralloc this is NOT
    // host-mode-only: gpu_config.sh honours it in both modes, because the composer is selected the
    // same way whichever GPU path is in use.
    std::optional<QString> hwcomposer;
    // Vulkan ICD name → ro.hardware.vulkan (bd remora-xqt1). Set in BOTH modes, unlike gralloc:
    // guest mode resolves the software rasterizer, host mode the driver behind the selected node
    // (or virtio when Venus carries rendering to NVIDIA). Absent → nothing emitted, and since the
    // native_vulkan pin was retired with the feature nothing else writes the property either: an
    // unknown GPU shows up as a missing ICD rather than as a plausible wrong one.
    std::optional<QString> vulkan;
    // LIBVA_DRIVER_NAME for the c2-va HAL. Absent → the service .rc's own default (iHD) applies.
    std::optional<QString> vaDriver;
    // NVIDIA Venus stack (bd remora-4ei.56). venus=false leaves every field below unset and the
    // emitted argv byte-identical to before the feature existed. gpuNode stays the *xe* node even
    // here: NVIDIA is reached only through venusSocketDir, while the Intel node is what the gbm
    // wrapper and the VAAPI encoder actually open.
    bool venus = false;
    std::optional<QString> venusNode;       // the NVIDIA render node, passed as an extra --device
    std::optional<QString> venusSocketDir;  // host dir bound at /dev/venus
    std::optional<QString> venusLibDir;     // source dir of the prebuilt guest .so files
    // /vendor/build.prop overlay, set for venus AND for gpu_mode=guest — both exist for the same
    // underlying reason: the image bakes ro.hardware.vulkan=intel, ro.* props are first-wins and
    // applied before gpu_config.sh runs, so its setprop silently loses and the wrong ICD survives
    // into SurfaceFlinger (venus: bd remora-4ei.56; guest: the chooseEglConfig crash loop of bd
    // remora-e5x.33). GENERATED per-deploy by the engine (the image's own build.prop with the
    // Remora-owned GPU keys replaced) rather than vendored: ro.* is immutable, so setprop cannot
    // substitute, and a checked-in copy would stale-bake every other prop in the file.
    std::optional<QString> buildPropOverlay;
    // Not part of the docker argv — the engine starts and supervises this before bringup.
    std::optional<QString> venusServerPath;
    // Host paths for the netfix pair (the v4→v6 adb forwarder and the script that starts it),
    // bind-mounted into the container instead of docker-cp'd after boot — Android remounts the
    // rootfs read-only, which makes a post-boot cp impossible. Filled by the engine's
    // applyVendorPaths(); absent in a context built without it (tests).
    std::optional<QString> netfixFwdPath, netfixScriptPath;
    // The forwarder's Android INIT SERVICE, bound into /vendor/etc/init so init starts and
    // supervises it on every container boot — including restarts Remora knows nothing about,
    // which used to come back with Android healthy and no forwarder at all (bd remora-yn4).
    std::optional<QString> netfixInitRcPath;
    // remora.props.rc, bound to /vendor/etc/init on the same road as the forwarder's rc. NO
    // LONGER AN ADAPTER (bd remora-28ix.4 step 4): it used to map the remora_* argv onto the
    // pre-migration names in-image consumers read, and every such consumer is gone. What remains
    // are its two genuine derivations — ro.sf.lcd_density from remora_dpi (init cannot arm one
    // early-init property action from another, so it must be set in the same action) and
    // ro.hardware.vulkan from remora_vulkan (a per-deploy fact, bd remora-xqt1). The mount
    // shadows the image's identical baked copy, which is harmless by construction.
    std::optional<QString> propsInitRcPath;
    // The mirror agent: its dex and its own init service, bound in the same two ways for the same
    // two reasons (bd remora-28ix.3). Absent when the dex has not been built — since the v1 purge
    // an image without the bake is refused outright, so these mounts are the DEV road only.
    std::optional<QString> agentDexPath, agentInitRcPath;
    // The image BAKES the agent as a boot service (the mirror_agent build feature, bd
    // remora-28ix.3.4) — resolved from the profile's feature request or the tag's registered
    // capability. Builders then emits NO agent mounts even when the paths above resolved: the
    // baked rc already defines `service remora_agent`, init discards a second definition as a
    // duplicate, and which of the two it read first is a parse-order accident.
    bool agentBaked = false;
    // Host path for the phantom-keyboard helper, bind-mounted alongside the netfix pair and for
    // the same reason. Started detached after boot; see the uinputKbd step in Deployers.cpp.
    std::optional<QString> uinputKbdPath;
    QString networkMode;                // "macvlan" (own LAN IP) | "bridge" (publish a host port)
    std::optional<QString> dockerNetwork, macvlanIp;  // macvlan: no IP -> docker IPAM assigns
    // The macvlan network's shape, for the `docker network create` the deploy runs when the
    // network is missing. Present only in macvlan mode, and only when the host LAN was actually
    // read (config override, else the live probe) — absent means "cannot create it, say so"
    // rather than "create it on a guessed subnet" (bd remora-400).
    std::optional<QString> macvlanParent, macvlanSubnet, macvlanGateway, macvlanRange;
    // Host-side reachability for macvlan (bare). macvlanShimIp is the address the shim interface
    // carries; absent when nothing could be derived, which is the "cannot build one" signal.
    bool macvlanHostRoute = false;
    std::optional<QString> macvlanShimIp;
    int hostAdbPort, containerAdbPort;  // container: 5556, the in-container fwd shim
    QString mouseBind, videoBitRate, videoCodec;
    // The mirror's shortcut modifier (MOD+b/h/s/n = back/home/recents/notifications). Empty = leave
    // the inherited lalt,lsuper alone and emit nothing, so the default argv stays stable.
    QString shortcutMod;
    // The mirror's --key-bind list ("F1:b,F2:h,..."): single keys bound to nav actions, consumed
    // client-side. Empty = emit nothing.
    QString keyBind;
    // Set when videoCodec was DOWNGRADED to h264 because the probed GPU has no VA-API encode
    // entrypoint (bd remora-e5x.4). Carries the reason so `check`/`plan` can state the trade rather
    // than let it look like the user asked for h264. Absent = the requested codec was kept.
    std::optional<QString> videoCodecFallback;
    // Encode the mirror on the host GPU rather than inside Android (bd remora-e5x.6). When set,
    // `up` starts remora-frame-encoder alongside the container; frameSocket is where the patched
    // Venus renderer publishes composed frames, videoSocket is where the encoded h265 is served.
    bool hostEncode = false;
    QString hostEncodeFrameSocket, hostEncodeVideoSocket;
    int hostEncodeMaxSize = 0;  // 0 = encode at the display's own size
    // DECODE video on the host GPU rather than in-guest (bd remora-e5x.37, the compose encoder run
    // in reverse). Separate from hostEncode and NOT implied by it: they are opposite directions
    // through different helpers, and the whole point of going remote for decode is that encode can
    // stay on the local Intel VADisplay at the same time — c2-va has one VADisplay per process, so
    // NVDEC decode and VDENC encode could never share it. NVIDIA's VA driver is decode-only
    // (measured, bd remora-e5x.3), so this is the only direction that can use it at all.
    bool hostDecode = false;
    QString hostDecodeSocket;
    // Whether THIS window takes its video from the host encoder. Follows hostEncode since the
    // v1 purge (the agent composes natively — no compose-capable jar to gate on), but stays its
    // own flag because it is per-window: PIP clears it on its own copy so a second consumer
    // never fights the mirror for the one composed stream (bd remora-0xh).
    bool hostEncodeDrivesMirror = false;
    int pipWidth = 0, pipHeight = 0;  // the PIP window's own size (bd remora-4ei.38)
    // Which host keyboard the guest's uinput device impersonates; passes through unresolved
    // because the candidate list only exists on the machine running the deploy (bd remora-4ei.34).
    std::optional<QString> kbdIdentity;
    // Host input devices shared with the container by name, non-exclusively (bd remora-4ei.36).
    // Names, not nodes: event numbers churn across replugs, so the deploy and its watcher resolve
    // them against the docker host's /proc/bus/input/devices at run time.
    QStringList sharedInputs;
    // Host V4L2 nodes handed to the container for the external camera provider — empty unless
    // the profile carries camera_v4l2 or names camera_devices= (bd remora-4ei.9).
    QStringList cameraDevices;
    // Should the container see a trimmed, phone-like cpufreq topology (bd remora-6279)? The
    // DECISION, which is profile policy and belongs to the resolver.
    bool cpufreqTopology = true;
    // …and the host directory the trimmed tree is generated into, which is not: it differs
    // between a local docker host and one reached over ssh, so makeContext fills it the same way
    // it fills uinputKbdPath. Empty = nothing is mounted, whatever the flag says — so a context
    // built without host paths (plan output, tests) cannot emit a bind to a directory that was
    // never generated. Contents are built per deploy from the live host, the buildPropOverlay split:
    // core cannot read /sys, and a checked-in copy would describe whatever machine it came from.
    QString cpufreqDir;
    // Set when the profile ASKED for cameras (the camera_v4l2 feature, no explicit list) and the
    // live probe found no /dev/video* — so cameraDevices is empty for a reason the operator wants
    // to hear, not because nobody wanted a camera. Without this the two are indistinguishable
    // downstream and a deploy reports a cheerful "nothing to do" for a capability that was
    // requested and silently did not arrive. Observed: a USB hub carrying the webcam
    // was unplugged between two deploys and only the wording changed. An explicitly EMPTY
    // camera_devices= is deliberate "off" and is NOT flagged.
    bool cameraWantedButNoneFound = false;
    // The profile asked for the USB audio HAL (bd remora-4ei.37), so the deploy must hand
    // /dev/snd to Android's audioserver. Purely the DECISION — the nodes themselves are not
    // enumerated here the way cameraDevices are, because /dev/snd arrives wholesale with the
    // privileged container rather than through an explicit per-node grant.
    bool usbAudio = false;
    bool hwDecode = false;
    bool mirrorAudio = false;  // opt-in device audio for the main mirror window
    int maxSize;
    QStringList mirrorExtra;
    // window geometry (opt-in: an argument is emitted only when set)
    std::optional<int> windowWidth, windowHeight, windowX, windowY;
    bool windowFullscreen = false;
    // Fullscreen action: mirrored (default, always stretched — the client has no other fit) vs the
    // separate-display "desktop mode" (--new-display + --start-app).
    bool fullscreenSeparateDisplay = false;
    std::optional<QString> fullscreenDisplay, fullscreenApp;
    QString freeformDisplay;  // phone-sized --new-display for freeform/popup app windows
    bool desktopNewWindow = false;  // Alt+D: new window each press vs reopen/replace (default)
    bool desktopMenu = true;        // refresh the desktop app-menu folder after each connect
    QStringList sharedFolders;      // "<hostDir>[:<name>]" → bound at /data/media/0/<name>
    QStringList companionApps;      // "<hostDir>[:<Name>]" → /system/system_ext/app/<Name>
    QString target;                     // "localhost:5555" | "192.168.0.77:5555" | "host:5555"
    std::optional<QString> sshHost;
    // runtime knobs (opt-in: a boot arg is emitted only when set)
    std::optional<int> fps;
    // Guest timezone (persist.sys.timezone), applied by the connect-time device tune. The
    // resolver only carries a profile PIN through; unset is filled engine-side from this host's
    // zone (applyVendorPaths) — the core cannot read the machine it runs on (bd remora-c8t).
    QString timezone;
    // --max-fps (encoder-side drop via the c2-va vendor tuning); emitted only when set
    std::optional<int> maxFps;
    std::optional<int> autoSleepMinutes;  // idle minutes before auto-freeze (unset/0 = off)
    bool useOverlayfs = false;
    // codec2 pipeline: enables the c2.* codecs (our VAAPI HW decoder + the sw HEVC encoder for
    // --video-codec=h265). Off → the legacy OMX path (no codec2, no HEVC encode).
    bool useCodec2 = false;
    QStringList roOverrides, dns;
    std::optional<QString> proxyType, proxyHost;
    std::optional<int> proxyPort;
    std::optional<QString> rootHiding;
    QStringList denylistPackages;
    bool playSpoof = false;  // ReZygisk+PIF per-app Play spoof (see AdvancedConfig::playSpoof)
    // custom-image pipeline
    std::optional<QString> buildHost;  // unset = build locally
    QString buildDir, outputDir;
};

}  // namespace remora
