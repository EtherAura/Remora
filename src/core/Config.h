#pragma once
#include <QString>
#include <QStringList>
#include <optional>

#include "core/Backend.h"

namespace remora {

// The user-facing config — the §04 groups. Every field optional (std::nullopt = "unset → the
// resolver applies the proven default"). An empty RemoraConfig resolves to a bootable instance.
struct GpuConfig {
    std::optional<GpuMode> mode;
    // Set by the loader when gpu_mode= is neither host nor guest (the retired "auto" included).
    // Not a config knob and never written back — it exists so preflight can refuse, naming the
    // valid values, instead of the profile quietly deploying as guest.
    std::optional<QString> unrecognisedMode;
    std::optional<QString> gpuNode;
    // Select the render GPU by DRIVER name ("xe", "amdgpu", ...) on multi-GPU hosts: the resolver
    // picks the live node with this driver at each deploy, so the choice survives the node-number
    // rotation that a baked path would not. gpu_node= (exact path) still wins over this; unset =
    // the probe's first pick. SwiftShader is not a driver — it is gpu_mode=guest (bd remora-e5x.1).
    std::optional<QString> gpuDriver;
    // gralloc HAL name → ro.hardware.gralloc, via androidboot.remora_gralloc (gpu_config.sh's
    // gpu_setup_host reads ro.boot.remora_gralloc directly and defaults it to "gbm"). Unset → the resolver
    // picks minigbm_intel whenever the resolved node is the live xe node, because gbm_gralloc
    // mis-allocates xe buffers (dequeueBuffer -19, then SIGFPE in gralloc_gbm_bo_create).
    std::optional<QString> gralloc;
    // Composer HAL name → ro.hardware.hwcomposer, via androidboot.remora_hwc (gpu_config.sh reads
    // ro.boot.remora_hwc and DEFAULTS IT TO "remora" — Remora's own HWC2 HAL, bd remora-emoe /
    // 28ix.4 R4). THERE IS NO LONGER AN ALTERNATIVE TO SELECT: the closed prebuilt composer left
    // the image with bd remora-28ix.4.3, so a profile naming it now resolves a library that is not
    // installed and hw_get_module fails with no composer at all rather than degrading. What used to
    // be a restart-cost A/B is now a pin that must be cleared. Unset → the image default, i.e.
    // Remora's HAL, which is also the only one. The caveat that motivated the pin still stands —
    // the HAL has only been proven on NVIDIA/Venus, never on cros gralloc plus gallium — it simply
    // can no longer be hedged by selecting the old binary.
    std::optional<QString> hwcomposer;
    // Vulkan driver name → ro.hardware.vulkan, via androidboot.remora_vulkan (bd remora-xqt1).
    // Android's loader dlopens /vendor/lib64/hw/vulkan.<value>.so, so a wrong or missing value is
    // the difference between a working GPU and none. THE HOST HAS TO DECIDE THIS, for the reason
    // gpu_config.sh states about node selection: the guest sees passed-through nodes, not the
    // whole picture. Two facts prove it cannot be inferred in-image — gpu_config's own
    // setup_vulkan reads /sys/kernel/debug/dri, which is NOT mounted in a Remora container, so it
    // has never set anything; and on a Venus host the selected node is the INTEL one (gralloc
    // allocates there) while rendering goes to NVIDIA over vtest, so the node's own driver names
    // the wrong ICD. Unset → nothing emitted, deliberately: an unmapped driver must surface as a
    // missing driver rather than hide behind a plausible wrong name, the va_driver rule below.
    std::optional<QString> vulkan;
    // VA-API driver name → LIBVA_DRIVER_NAME, via androidboot.va_driver (the c2-va service .rc
    // reads ro.boot.va_driver, iHD when unset). Unset → the resolver maps the probed host driver
    // (amdgpu → radeonsi); Intel needs no override because iHD IS the image default, and an
    // unmapped driver deliberately emits nothing so the software fallback surfaces in `check`
    // instead of hiding behind a wrong driver name (bd remora-4ei.80).
    std::optional<QString> vaDriver;
    // NVIDIA acceleration for the docker-argv backends via Mesa Venus (bd remora-4ei.56): guest
    // Vulkan → vtest over a unix socket → a host virglrenderer render server → NVIDIA's
    // proprietary Vulkan, with ANGLE supplying GL on top. nullopt = AUTO, i.e. on exactly when the
    // probe found an NVIDIA render node *and* both host prerequisites resolved — so auto can never
    // switch a working box onto a stack whose pieces are missing. false pins it off.
    std::optional<bool> venus;
    // Host dir bind-mounted at /dev/venus; the render server's socket lives in it. A directory
    // rather than the socket file itself so the server may be restarted (recreating the inode)
    // without the container losing the mount.
    std::optional<QString> venusSocketDir;
    std::optional<QString> venusLibDir;      // the prebuilt guest .so files (ANGLE + gbm wrapper)
    std::optional<QString> venusServerPath;  // built virgl_test_server binary
};

struct DisplayConfig {
    std::optional<int> width, height, dpi, fps;
    // Encoder frame cap (--max-fps → the c2-va component drop, remora-rjh). Unset →
    // resolver default: none at ≤60 fps, 60 when fps is raised above 60.
    std::optional<int> maxFps;
    std::optional<bool> useMemfd;
    // IANA zone for the guest (persist.sys.timezone). Unset = follow THIS host's zone, filled
    // engine-side because the core cannot read the machine; a pin is for a profile deliberately
    // living elsewhere (bd remora-c8t).
    std::optional<QString> timezone;
};

struct ImageConfig {
    std::optional<QString> imageTag;  // explicit override; else features→tag (gating) / per-backend default
    QStringList features;             // requested build features
    std::optional<int> androidVersion;  // 8..17 (default 16); selects the base image family
    // Custom-image pipeline: build the feature recipe yourself, or fetch a hosted image instead.
    std::optional<QString> buildHost;   // DOCKER host: docker build/save/images/run; unset = local
    std::optional<QString> buildDir;    // docker build context (Dockerfile + staged payloads)
    std::optional<QString> sourceHost;  // SOURCE host: where the AOSP tree lives + compiles
    std::optional<QString> sourceTree;  // AOSP/Lineage checkout for source rebuilds
    std::optional<QString> sourceKind;  // lineage | aosp — selects the rebuild command hint
    std::optional<QString> sourceRepo;  // git/repo URL or manifest to clone the source tree from
    std::optional<QString> sourceRef;   // branch/tag/manifest revision to check out
    std::optional<bool> sourcePinned;   // sync the vendored pinned manifest (exact tested SHAs)
    std::optional<QString> builtCommit; // source HEAD at the last build (pending-build detection)
    std::optional<QString> builtRecipe; // input fingerprint of the last build (identical-rebuild detection)
    QStringList sourcePatches;          // vendored source-patch keys to apply (unset = defaults)
    std::optional<QString> outputDir;   // where the built image archive (.tar) lands
    std::optional<int> buildNice;       // source-build niceness 0..19 (default 15)
    std::optional<int> buildJobs;       // source-build -j parallelism (unset = all cores)
    std::optional<int> buildMemGiB;     // source-build container hard RAM cap (unset = auto from
                                        // MemAvailable at launch). See bd remora-4ei.27.
    std::optional<int> soongMemGiB;     // SOONG_GOMEMLIMIT for soong_build's Go heap, GiB
                                        // (unset = auto, 65% of the cap). Needs the
                                        // soong_gomemlimit source patch to have any effect.
    std::optional<QString> archVariant; // source-build TARGET_ARCH_VARIANT ("broadwell",
                                        // "alderlake", ...): CPU-tuned native code for a chosen
                                        // host class, and a variant-suffixed image tag so tuned
                                        // builds coexist with the portable one. Unset = the
                                        // tree's generic x86-64 baseline (bd remora-bm7.11).
};

struct NetworkConfig {
    std::optional<QString> networkMode;  // "macvlan" (own LAN IP) | "bridge" (port-forward);
                                         // unset -> bridge on both backends
    std::optional<QString> dockerNetwork, macvlanIp;  // macvlanIp unset -> docker IPAM assigns
    // macvlan network shape. ALL unset by default: the deploy derives them from the docker host's
    // own primary LAN interface, so a macvlan deployment needs no network typing at all and stays
    // correct when the machine changes network (bd remora-400). Set one to override that one.
    std::optional<QString> macvlanParent;   // parent NIC ("UMN", "eno1")
    std::optional<QString> macvlanSubnet;   // "192.168.0.0/24"
    std::optional<QString> macvlanGateway;  // "192.168.0.1"
    // The slice of the subnet docker IPAM may auto-assign from. Unset -> the top /28, which keeps
    // auto-assigned addresses clear of the router's DHCP pool; a pinned macvlanIp is unaffected by
    // it (docker validates a pinned address against the subnet, not the range).
    std::optional<QString> macvlanRange;
    // The host-side route that lets THIS machine reach its own macvlan containers (bd remora-400).
    // On by default and only meaningful for `bare`: the kernel isolates a macvlan child from its
    // parent NIC, so without it the docker host — which on bare is also where adb and the mirror run —
    // cannot talk to the container it just started. Set false to manage host routing yourself.
    std::optional<bool> macvlanHostRoute;
    // The shim interface's own LAN address. Unset -> one below the auto-IP range. It has to be a
    // free address on the real network, so a host whose derived pick is taken states one here.
    std::optional<QString> macvlanShimIp;
    QStringList dns;
    std::optional<QString> proxyType, proxyHost;
    std::optional<int> proxyPort;
};

struct InputConfig {
    // keyboard_mode / mouse_mode are RETIRED keys: sdk-style input is built into the mirror and
    // there is no alternative to select. Old values are read by nobody and rewritten by nobody.
    std::optional<QString> mouseBind;
    // Which modifier arms the mirror's navigation chords (MOD+b back, MOD+h home, MOD+s recents,
    // MOD+n notifications) — the keyboard half of the Viewer's Navigation card, where mouseBind is
    // the mouse half. A '+'-joined subset of lctrl/rctrl/lalt/ralt/lsuper/rsuper, ','-separated for
    // alternatives. nullopt = the inherited "lalt,lsuper", and no --shortcut-mod is emitted.
    std::optional<QString> shortcutMod;
    // Single arbitrary keys bound straight to nav actions, no modifier — the mirror's --key-bind
    // (bd remora-pdu). A ','-separated list of <SDL key name>:<action char> pairs reusing the
    // --mouse-bind action characters, e.g. "F1:b,F2:h,F3:s". A bound key is consumed by the
    // client, never forwarded to Android. nullopt = no bindings, no --key-bind emitted.
    std::optional<QString> keyBind;
    // Which of the HOST's real keyboards the guest's uinput device impersonates. Gboard refuses
    // physical-keyboard mode for a keyboard advertising generic identifiers, so a passthrough that
    // claims nobody in particular gets no toolbar (bd remora-a41). nullopt = auto, i.e. the first
    // real keyboard the host reports; "none" = advertise the generic identity and accept that;
    // anything else is matched against the keyboard's name. This describes the operator's desk
    // rather than the Android image, so it is deliberately not per-image state.
    std::optional<QString> kbdIdentity;
    // Host V4L2 camera nodes handed to the container (--device), for the external camera
    // provider the camera_v4l2 feature builds into the image (bd remora-4ei.9). nullopt = AUTO:
    // every live-probed /dev/video* on the docker host, but ONLY when the profile carries the
    // camera_v4l2 feature — a camera reaching a container nobody asked for would be a quiet
    // privacy hole. An explicit list is exact; an explicitly EMPTY list is "off".
    std::optional<QStringList> cameraDevices;
    // Host input devices (keyboard/mouse/controller, USB or Bluetooth) to share with Android by
    // NAME. Shared non-exclusively: the container opens the SAME evdev node the desktop reads
    // (mknod'd into its /dev/input — bare and remote share the docker host's kernel), and nothing
    // EVIOCGRABs, so the device keeps working on the host and Android sees its real bus, vendor
    // and product (bd remora-4ei.36). Empty = share nothing.
    QStringList sharedInputs;
    // Present a phone-like cpufreq topology to the container instead of the host's own
    // (bd remora-6279). nullopt = ON, because a desktop's topology is outside what Android native
    // code is written for and the consequences are memory corruption, not a slow app: this host
    // exposes 24 cpufreq policies (one per logical CPU under intel_cpufreq), every phone exposes
    // 2-3, and TikTok's big-core detector fills a SIXTEEN-slot table indexed by policy ordinal
    // with no bounds check — 640 bytes of stack overrun, two crash signatures, days of diagnosis
    // (bd remora-e5x.28). Fixed per-core tables sized for phone topologies are ordinary in game
    // engines, protection libs and perf SDKs, so this is a class rather than one app.
    // false keeps the host's real policies visible — for measuring the host's own cpufreq from
    // inside the guest, which is the one thing the trimmed view cannot answer.
    std::optional<bool> cpufreqTopology;
};

struct MirrorConfig {
    std::optional<QString> videoBitRate, videoCodec;  // videoCodec: h264|h265|av1
    // audio_codec / audio_buffer are RETIRED keys: the mirror plays opus and has no buffer knob
    // (REMORA_AUDIO_BUFFER_MS tunes its sink), and nothing ever emitted either. Old values are
    // read by nobody and rewritten by nobody.
    // Encode the mirror on the HOST GPU instead of inside Android (bd remora-e5x.6). The only route
    // to hardware h265 on NVIDIA, whose VA-API driver has no encode entrypoint at all. Off unless
    // asked for: it needs the patched Venus renderer, so it cannot be a silent default.
    std::optional<bool> hostEncode;
    // DECODE in-Android video on the host GPU instead of in-guest (bd remora-e5x.37). Independent
    // of hostEncode in both directions: encode goes guest->host so the mirror can use NVENC-class
    // hardware, decode goes host->guest so playback can use NVDEC, and a profile may sensibly want
    // either alone. Off unless asked for — it needs remora-frame-decoder installed on the host, so
    // like hostEncode it cannot be a silent default.
    std::optional<bool> hostDecode;
    // composeSource is GONE (bd remora-emoe). It selected where compose mode's frames came from —
    // "venus", intercepting them in the patched render server, or "composer", taking them from
    // Remora's own hwcomposer — and the comparison it existed to enable is settled. The composer
    // receives exactly one client target per frame and IS the display by definition, where the
    // interception published every composited surface and had to be RANKED into a guess. Measured:
    // the composer encodes ~1.0 frames per frame the device composed against the interception's
    // ~1.6, for about a third less host CPU. Nothing is left to select between, and a selector with
    // one legal value is only a way to ask for a machine that no longer exists — so compose_source=
    // in an existing remorarc is read by nobody and ignored.
    // mirrorServerPath is GONE (bd remora-28ix.5): the v1 jar path is deleted and the device
    // half is the agent the image bakes. mirror_server_path= in remorarc is a retired key — read
    // by nobody, rewritten by nobody.
    // Cap on the ENCODED size for host encode, longest side, 0/unset = the display's own size.
    // Not the same as max_size, which sets the DISPLAY size: this scales only what the host
    // encoder produces, so the device still composes at full resolution. It exists because the
    // Vulkan pipeline must be sized to fit up front — ffmpeg crashes rather than reporting an
    // out-of-memory, so there is no recovering afterwards.
    std::optional<int> hostEncodeMaxSize;
    // Client-side GPU decode (CUDA/NVDEC first, then VAAPI). Unset = on; hw_decode=false forces
    // software on a host whose decoder misbehaves.
    std::optional<bool> hwDecode;
    // Mirror audio (bd remora-28ix.2.2). Off by default: opt-in until it has earned its place.
    std::optional<bool> audio;
    std::optional<int> maxSize;
    // Window geometry (opt-in: emitted only when set). On Wayland the compositor clamps a bordered
    // window to the output's workarea — pin an explicit size (e.g. 1880x996) to get a deterministic
    // video area on a roomy monitor. Fullscreen is the only mode whose physical size is identical
    // on every output/refresh-rate (mirrored/mixed-scale setups negotiate windowed sizes away).
    std::optional<int> windowWidth, windowHeight, windowX, windowY;
    // Picture-in-picture: a SECOND, small, always-on-top mirror that runs alongside the main one
    // for multitasking. Its own size so shrinking the PIP cannot disturb the main window
    // (bd remora-4ei.38).
    std::optional<int> pipWidth, pipHeight;
    // No window_borderless: the mirror is an ordinary decorated window and its frame belongs to
    // the desktop, not to Remora (KWin's "No titlebar and frame" rule, or the equivalent
    // elsewhere, is where a borderless mirror is asked for). `--window-borderless` can still be
    // passed per-profile through mirror_extra.
    std::optional<bool> windowFullscreen;
    // The "Fullscreen" action. Default = genuine mirrored fullscreen of the main display, always
    // stretched (the client has no other fit) so the mirror fills a monitor whose aspect differs
    // from the device — letterboxing is not a concept Remora keeps. Windowed is unaffected: the
    // mirror sizes that window to the device aspect, so nothing stretches.
    // Opt-in "desktop mode": instead of mirroring, Fullscreen spins up a SEPARATE Android display
    // via --new-display at fullscreenDisplay ("<W>x<H>[/<dpi>]", empty → main size) and auto-launches
    // fullscreenApp on it (a fresh display is otherwise blank).
    std::optional<bool> fullscreenSeparateDisplay;
    std::optional<QString> fullscreenDisplay, fullscreenApp;
    // Freeform/popup app windows (menu launches remembered as "freeform"): the --new-display
    // geometry of the phone-sized window. Unset → the resolver's phone-like default.
    std::optional<QString> freeformDisplay;
    // Desktop shortcut (Alt+D) behavior: by default it replaces the current desktop window (reopen);
    // with this set it opens a new window each press (multiple desktop displays allowed).
    std::optional<bool> desktopNewWindow;
    QStringList extra;
};

struct BackendConfig {
    std::optional<Backend> backend;
    std::optional<QString> containerName, dataDir, rezmodsDir;
    // Overlayfs multi-instance (use_overlayfs): the shared lower layer mounted at
    // /data-base; dataDir then becomes the per-instance /data-diff (upper+work) instead of a
    // plain /data bind. Unset → a per-backend default shared by all profiles on that host.
    std::optional<QString> dataBaseDir;
    std::optional<int> hostAdbPort;
    // Host address the bridge-mode adb port is published on (adb_bind=). Unset → the resolver's
    // per-backend default: loopback on bare, every interface on remote. "0.0.0.0" publishes on
    // every interface explicitly.
    std::optional<QString> adbBindAddress;
    std::optional<QString> sshHost;
    // Set by the loader when the profile on disk still says backend=vm. Not a config knob and never
    // written back — it exists so preflight can refuse with migration instructions (bd remora-d5v).
    bool retiredVmBackend = false;
};

struct IntegrationConfig {
    // Desktop app-menu integration: a launcher folder named after the profile with one entry per
    // installed Android app (real icons). On (default) it is refreshed after every successful
    // connect; off disables the auto-refresh (existing entries are managed manually).
    std::optional<bool> desktopMenu;
    // Per-app window mode for menu launches: "<pkg>=desktop" (the configured desktop resolution)
    // or "<pkg>=freeform" (a phone-sized popup display; resizes reflow the app).
    // "freeform-stretch" is a legacy spelling of freeform from when letterboxed popups existed.
    // An app without an entry gets freeform, recorded at first sight (list or launch).
    QStringList appModes;
    // Automatic app updates: after each successful Connect, run the ABI-preserving Play update
    // sweep when at least autoUpdateHours have passed since the last one (0 = every Connect,
    // default 24). Off unless autoUpdateApps is set.
    std::optional<bool> autoUpdateApps;
    std::optional<int> autoUpdateHours;
    // Packages pinned to arm64: apps whose x86_64 build is absent or broken, so they were
    // sideloaded as arm64 and must STAY arm64. Play silently auto-updates them back to the
    // x86_64 split it thinks the device wants, which reinstates the crash — the arm-guard
    // (Connect step, gated on armGuard) re-fetches any pinned package that lost its arm64 ABI.
    // Entries are added automatically the first time an app is repaired or installed as arm.
    QStringList armApps;
    // The arm-guard itself: on (default) it repairs pinned apps after each Connect. Off leaves
    // the pins as a record only — the Apps page still flags them.
    std::optional<bool> armGuard;
    // Shared folders: "<hostDir>[:<name>]" entries visible at
    // /data/media/0/<name> — i.e. under the device's /sdcard (name defaults to the dir's
    // basename). The path is on the DOCKER HOST and must not contain spaces or colons. Applied
    // at docker run; a change makes the next Connect recreate the container. Compatible with the
    // default overlayfs /data: the binds stage at /remora/share/<name> and provision re-binds
    // them into the merged view after boot.
    QStringList sharedFolders;
    // Companion apps: Remora-built APKs bind-mounted into the image as SYSTEM apps, "<hostDir>"
    // or "<hostDir>:<Name>". The directory (containing <Name>.apk) is bound read-only at
    // /system/system_ext/app/<Name>, so PackageManager scans it from a system partition and the
    // app gets FLAG_SYSTEM — which is what SettingsLib's TileUtils requires before it will show a
    // Settings tile (bd remora-fgj.1 proved this end to end). Path is on the DOCKER HOST, like
    // sharedFolders. Baked at docker run, so a change needs a container recreate.
    QStringList companionApps;
};

struct AdvancedConfig {
    // Overlayfs /data — ON by default and not user-facing: every profile keeps a private diff
    // layer (its data dir) over a shared read-mostly base (data_base_dir), so several instances
    // can stand on one seeded /data. use_overlayfs=false in remorarc opts back into the
    // plain per-instance /data bind (legacy layout).
    std::optional<bool> useOverlayfs;
    std::optional<bool> useCodec2;  // enable the codec2 pipeline (HW decode + HEVC encode)
    // Auto-sleep: freeze the device (docker pause) after this many minutes with no local mirror
    // client attached, so an unwatched device stops burning host CPU/GPU. Enforced by the `remora
    // auto-sleep` watcher (installed alongside `remora service`); unset/0 = off. Any Connect wakes
    // the frozen device in ~1s.
    std::optional<int> autoSleepMinutes;
    QStringList roOverrides;         // "key=val" → androidboot.ro.<key>=<val>
    std::optional<QString> rootHiding;  // off | shamiko | denylist | full
    QStringList denylistPackages;
    // Play spoof: run the patched ReZygisk + PlayIntegrityFix so Google Play sees a certified
    // Pixel *inside GMS/Play only* (system build.prop untouched → adb/mirror stay stable). When
    // set, bringup writes the ~/rezmods/remora-spoof marker and starts the injector. nullopt/false
    // leaves the device as its real (uncertified) self. Requires the zygisk_pif modules installed.
    std::optional<bool> playSpoof;
};

struct RemoraConfig {
    GpuConfig gpu;
    DisplayConfig display;
    ImageConfig image;
    NetworkConfig network;
    InputConfig input;
    MirrorConfig mirror;
    BackendConfig backend;
    IntegrationConfig integration;
    AdvancedConfig advanced;
};

// The deploy target is INFERRED from the docker host, not chosen: a blank host is this
// machine's docker, anything else is a docker host reached over ssh. An explicit backend= key
// (or the CLI's --backend) still wins, but the GUI neither offers nor writes it, and Remora does
// not start VMs: a guest is just another host once it is running.
inline Backend inferBackend(const RemoraConfig &c) {
    if (c.backend.backend) return *c.backend.backend;
    return c.backend.sshHost.value_or(QString()).trimmed().isEmpty() ? Backend::Bare
                                                                     : Backend::Remote;
}

}  // namespace remora
