#include "core/Resolver.h"

#include <QDir>
#include <QRegularExpression>

#include "core/Features.h"
#include "core/Gating.h"

namespace remora {

template <class T>
static T pick(const std::optional<T> &v, T def) {
    return v.has_value() ? *v : def;
}

bool driverHasVaEncode(const QString &drmDriver) {
    // Intel (both the modern xe and the older i915) via iHD, and AMD via radeonsi, expose encode
    // entrypoints. NVIDIA deliberately does NOT: its VA driver is NVDEC, decode-only. Anything
    // unrecognised is treated as no-encode — the conservative direction, because guessing "yes"
    // costs a mirror that never connects while guessing "no" costs h264.
    return drmDriver == QLatin1String("xe") || drmDriver == QLatin1String("i915") ||
           drmDriver == QLatin1String("amdgpu");
}

// Host-GPU node + gralloc HAL for the backends that deploy through the docker argv (bare/remote).
// The guest's gpu_config.sh cannot auto-select an xe node — its scan's case list is
// i915|amdgpu|nouveau|virtio_gpu|v3d|vc4|msm_drm|panfrost, so on an Intel xe box it reports "NO
// qualified render node found" and silently falls back to SwiftShader. Host mode on xe therefore
// works only when Remora names the node explicitly. Never baked: the node is the live probe's, or
// the one the user configured (bd remora-4ei.57).
// The Vulkan ICD for a selected DRM driver — gpu_config.sh's own setup_vulkan table, moved to the
// side that can actually evaluate it (bd remora-xqt1). nouveau/nvidia-drm are deliberately ABSENT:
// in-image they map to virtio, but that is only right when Venus is carrying the rendering, which
// is a host fact decided above, not a property of the node. An unlisted driver returns nullopt so
// nothing is emitted and the gap surfaces, rather than shipping a plausible wrong ICD name.
static std::optional<QString> vulkanIcdFor(const std::optional<QString> &driver) {
    if (!driver.has_value()) return std::nullopt;
    const QString &d = *driver;
    if (d == QLatin1String("xe") || d == QLatin1String("i915"))
        return QStringLiteral("intel");
    if (d == QLatin1String("amdgpu")) return QStringLiteral("radeon");
    if (d == QLatin1String("virtio_gpu")) return QStringLiteral("virtio");
    if (d == QLatin1String("v3d") || d == QLatin1String("vc4"))
        return QStringLiteral("broadcom");
    if (d == QLatin1String("msm_drm")) return QStringLiteral("freedreno");
    if (d == QLatin1String("panfrost")) return QStringLiteral("panfrost");
    return std::nullopt;
}

struct HostGpu {
    std::optional<QString> node, gralloc, vaDriver, vulkan;
    QStringList masked;  // non-selected GPUs' /dev/dri entries, to hide (bd remora-n4qx)
    bool venus = false;
    std::optional<QString> venusNode, venusSocketDir, venusLibDir, venusServerPath;
};
static HostGpu resolveHostGpu(const GpuConfig &g, const std::optional<HostCapabilities> &caps,
                              GpuMode mode) {
    if (mode != GpuMode::Host) {
        // Guest mode has no node and no gralloc override — but it DOES have a Vulkan answer, and
        // it is the one thing the image cannot supply for itself: gpu_setup_guest's own
        // `setprop ro.hardware.vulkan pastel` loses to /vendor/build.prop, which is loaded before
        // any init action runs and makes the ro.* write-once (bd remora-xqt1).
        HostGpu g0;
        g0.vulkan = g.vulkan.has_value() ? g.vulkan : std::optional<QString>(QStringLiteral("pastel"));
        return g0;
    }
    HostGpu h;
    // Which node, and — for every per-driver rule below — which DRIVER the choice lands on.
    // Precedence: gpu_node= (exact path, ultimate control) > gpu_driver= (pick by driver name,
    // the multi-GPU selector — stable across boots because node NUMBERS rotate while drivers do
    // not, bd remora-e5x.1) > the probe's first pick. The selected driver comes from the probe's
    // full node list wherever possible, so an explicitly named node still gets its driver's
    // gralloc/VA treatment instead of anonymous defaults.
    std::optional<QString> selDriver;
    if (g.gpuNode.has_value()) {
        h.node = g.gpuNode;
        if (caps.has_value()) {
            for (const RenderNode &n : caps->renderNodes)
                if (n.node == *g.gpuNode) selDriver = n.driver;
            if (!selDriver.has_value() && caps->hostRenderNode == h.node)
                selDriver = caps->hostRenderDriver;
        }
    } else if (g.gpuDriver.has_value()) {
        // The driver name is the user's statement even when no such node is live right now —
        // the node stays unset and the preflight names the gap, but the VA/gralloc rules still
        // follow the requested driver rather than silently reverting to another GPU's.
        selDriver = g.gpuDriver;
        if (caps.has_value())
            for (const RenderNode &n : caps->renderNodes)
                if (n.driver == *g.gpuDriver) {
                    h.node = n.node;
                    break;
                }
    } else {
        h.node = caps.has_value() ? caps->hostRenderNode : std::nullopt;
        selDriver = caps.has_value() ? caps->hostRenderDriver : std::nullopt;
    }
    // VA driver selection (bd remora-4ei.80): explicit override, else mapped from the SELECTED
    // driver. Only amdgpu needs naming — iHD is the .rc's own default, so Intel emits nothing —
    // and an UNMAPPED driver also deliberately emits nothing, so `check` can report the software
    // fallback instead of a wrong driver name hiding it. Venus does not change this: video still
    // runs on the local VA node (the Intel one on a hybrid box), never over the vtest socket.
    // Set BEFORE the early returns below so the venus path carries it too.
    h.vaDriver = g.vaDriver.has_value()
                     ? g.vaDriver
                     : ((selDriver == std::optional<QString>(QStringLiteral("amdgpu")))
                            ? std::optional<QString>(QStringLiteral("radeonsi"))
                            : std::nullopt);

    // NVIDIA Venus (bd remora-4ei.56). Resolved BEFORE gralloc because it overrides the choice.
    // AUTO is deliberately conservative: it requires the live probe to have found an NVIDIA node
    // AND both host prerequisites to have resolved, so enabling this by default cannot break a box
    // where the render server was never built. An explicit venus=true is honoured regardless — the
    // preflight then names whichever piece is missing rather than the resolver silently declining.
    h.venusServerPath = g.venusServerPath.has_value()
                            ? g.venusServerPath
                            : (caps.has_value() ? caps->venusServerPath : std::nullopt);
    h.venusLibDir = g.venusLibDir.has_value()
                        ? g.venusLibDir
                        : (caps.has_value() ? caps->venusLibDir : std::nullopt);
    const bool venusAvailable = caps.has_value() && caps->hostNvidiaRenderNode.has_value() &&
                                h.venusServerPath.has_value() && h.venusLibDir.has_value();
    h.venus = g.venus.value_or(venusAvailable);
    if (h.venus) {
        h.venusNode = caps.has_value() ? caps->hostNvidiaRenderNode : std::nullopt;
        // A directory, not the socket file: the server recreates the inode on every restart, and a
        // bind of the file itself would leave the container holding a deleted one.
        h.venusSocketDir = pick(g.venusSocketDir, QDir::homePath() + "/.cache/remora/venus");
    }

    // The Vulkan ICD, decided here rather than in-image, for two independent reasons
    // (bd remora-xqt1). VENUS WINS OUTRIGHT: rendering leaves for the host NVIDIA GPU over vtest,
    // so the ICD is virtio whatever the selected node's driver says — and on that very path the
    // selected node is the INTEL one, because that is where gralloc allocates, so reading the
    // node's driver would name the wrong ICD. Without Venus it follows the selected driver.
    // Set BEFORE the no-node return below only in the sense that both arms are already known; a
    // host with nothing to point at still falls through with nullopt and is reported by preflight.
    h.vulkan = g.vulkan.has_value()
                   ? g.vulkan
                   : (h.venus ? std::optional<QString>(QStringLiteral("virtio"))
                              : vulkanIcdFor(selDriver));

    if (!h.node.has_value() && !h.venus)
        return h;  // host mode with nothing to point at — preflight reports it
    // Every OTHER GPU's nodes get hidden behind /dev/null binds (bd remora-n4qx): --device only
    // STATES the selection — the privileged container sees all of /dev/dri, and the in-container
    // stack discovers devices itself, so on a multi-GPU host it can land on a GPU nobody chose
    // (amd-host-b rendered on a TeraScale relic while everything Remora controls named the RX 550).
    // The venus node is exempt — that path deliberately passes a second GPU. Single-GPU hosts
    // produce an empty list and an untouched argv.
    if (h.node.has_value() && caps.has_value()) {
        for (const RenderNode &n : caps->renderNodes) {
            if (n.node == *h.node) continue;
            if (h.venus && h.venusNode == std::optional<QString>(n.node)) continue;
            h.masked << n.node;
            if (!n.card.isEmpty()) h.masked << n.card;
        }
    }
    // Venus routes every allocation through the gbm_mesa backend (that is what reaches the render
    // server); the xe rule below would otherwise claim this node for minigbm_intel and the NVIDIA
    // path would never be taken.
    if (h.venus) {
        h.gralloc = g.gralloc.has_value() ? g.gralloc
                                          : std::optional<QString>(
                                                QStringLiteral("minigbm_gbm_mesa"));
        return h;
    }
    // gbm_gralloc mis-allocates xe buffers (dequeueBuffer -19, then SIGFPE in
    // gralloc_gbm_bo_create), so an xe node needs minigbm_intel. Claimed only when the SELECTED
    // driver is xe — on other drivers guessing minigbm_intel would be wrong (bd remora-4ei.58).
    //
    // amdgpu needs its own override for a subtler reason (bd remora-4ei.69, measured on an
    // RX 580): the image default RENDERS fine, but the encoder's YUV allocations SIGFPE in the
    // very same gralloc_gbm_bo_create (video formats derive 0 bytes-per-pixel, integer divide),
    // crash-looping allocator@2.0 and killing every mirror session while the home screen looks
    // perfect. cros — generic minigbm, native amdgpu backend — runs the whole path clean, VCE
    // HEVC encode included. NOT minigbm_gbm_mesa: without the venus guest pieces it takes
    // SystemUI down (drawRenderNode on a context with no surface).
    //
    // ALL OF THAT WAS MEASURED ON THE Mesa-24 IMAGE, and cros CANNOT WORK on a Mesa-26 one
    // (bd remora-ykhz.1): minigbm's amdgpu backend hard-fails init without
    // __driDriverGetExtensions_radeonsi, which Mesa 26 no longer exports.
    //
    // RESOLVED, and the default here deliberately DOES NOT MOVE: minigbm_gbm_mesa was re-measured
    // on the coherent Mesa-26 stack (RX 580 — boot, radeonsi GL, RADV, VAAPI encode under motion,
    // zero fatal signals) and a mesa_source image now TRANSLATES gralloc=cros to it in
    // gpu_config.sh, keyed on the payload files that make the translation true. So this cros
    // default is correct for BOTH image generations — old images get real cros, new ones
    // translate — and flipping it here would break any pre-Mesa-26 image redeployed on AMD (no
    // wrapper exists in those at all). It follows the step-4 retirement set: flip only when no
    // pre-Mesa-26 image remains deployable.
    const bool xe = selDriver == std::optional<QString>(QStringLiteral("xe"));
    const bool amd = selDriver == std::optional<QString>(QStringLiteral("amdgpu"));
    h.gralloc = g.gralloc.has_value()
                    ? g.gralloc
                    : (xe    ? std::optional<QString>(QStringLiteral("minigbm_intel"))
                       : amd ? std::optional<QString>(QStringLiteral("cros"))
                             : std::nullopt);
    return h;
}

// One assignment point so bare and remote cannot drift apart as the host-GPU story grows.
static void applyHostGpu(ResolvedConfig &r, const HostGpu &h) {
    r.gpuNode = h.node;
    r.maskedGpuNodes = h.masked;
    r.gralloc = h.gralloc;
    r.vulkan = h.vulkan;
    r.vaDriver = h.vaDriver;
    r.venus = h.venus;
    r.venusNode = h.venusNode;
    r.venusSocketDir = h.venusSocketDir;
    r.venusLibDir = h.venusLibDir;
    r.venusServerPath = h.venusServerPath;
}

// Networking, resolved identically for every backend so a mode that works on one cannot silently
// mean nothing on another. It used to be a vm-only choice: bare and remote overwrote whatever the
// profile said with "bridge" and dropped docker_network/macvlan_ip on the floor, so a bare profile
// asking for a LAN address got a bridged container and no diagnostic anywhere (bd remora-400).
// Only the DEFAULT is passed in (bridge — publish a host port); macvlan is an explicit opt-in that
// gives the container its own LAN address.
//
// The macvlan shape (parent NIC, subnet, gateway) has NO baked default, the same rule the GPU node
// follows: it is either stated in the config or read from the docker host's LIVE primary interface.
// A subnet baked here would be one developer's LAN, and being wrong about it produces a container
// that is up, addressed, and unreachable — the worst kind of failure to debug.
static void applyNetwork(ResolvedConfig &r, const NetworkConfig &n,
                         const std::optional<HostCapabilities> &caps, const QString &defaultMode) {
    r.networkMode = pick(n.networkMode, defaultMode);
    if (r.networkMode != QLatin1String("macvlan")) {
        r.dockerNetwork = std::nullopt;
        r.macvlanIp = std::nullopt;
        r.macvlanParent = r.macvlanSubnet = r.macvlanGateway = r.macvlanRange = std::nullopt;
        r.macvlanHostRoute = false;
        r.macvlanShimIp = std::nullopt;
        return;
    }
    r.dockerNetwork = pick(n.dockerNetwork, QStringLiteral("lan"));
    r.macvlanIp = n.macvlanIp;
    const std::optional<LanInterface> lan = caps.has_value() ? caps->hostLan : std::nullopt;
    const auto fromLan = [&](const std::optional<QString> &set, QString LanInterface::*field)
        -> std::optional<QString> {
        if (set.has_value()) return set;
        if (!lan.has_value() || (*lan).*field == QString()) return std::nullopt;
        return (*lan).*field;
    };
    r.macvlanParent = fromLan(n.macvlanParent, &LanInterface::parent);
    r.macvlanSubnet = fromLan(n.macvlanSubnet, &LanInterface::subnet);
    r.macvlanGateway = fromLan(n.macvlanGateway, &LanInterface::gateway);
    // Auto-assignment pool. Derived from whichever subnet won above, so an overridden subnet
    // carries its own range rather than one computed from the host's.
    if (n.macvlanRange.has_value())
        r.macvlanRange = n.macvlanRange;
    else if (r.macvlanSubnet.has_value())
        r.macvlanRange = ipv4TopRange(*r.macvlanSubnet, 28);
    else
        r.macvlanRange = std::nullopt;
    // Host-side reachability. Defaults ON: a macvlan deployment nobody can reach from the machine
    // running the mirror is not a working deployment, and the shim is the only way to have both.
    r.macvlanHostRoute = pick(n.macvlanHostRoute, true);
    // One below the auto-IP pool, so the shim and the containers never contend for an address.
    if (n.macvlanShimIp.has_value())
        r.macvlanShimIp = n.macvlanShimIp;
    else if (r.macvlanRange.has_value())
    {
        const QString derived = ipv4Offset(*r.macvlanRange, -1);
        r.macvlanShimIp = derived.isEmpty() ? std::nullopt : std::make_optional(derived);
    }
    else
        r.macvlanShimIp = std::nullopt;
}

ResolvedConfig resolve(const RemoraConfig &c, Backend backend,
                       const std::optional<HostCapabilities> &caps) {
    const auto &d = c.display;
    const auto &im = c.image;
    const auto &n = c.network;
    const auto &inp = c.input;
    const auto &sc = c.mirror;
    const auto &bk = c.backend;
    const auto &g = c.gpu;
    const auto &adv = c.advanced;

    ResolvedConfig r;
    r.backend = backend;
    r.width = pick(d.width, 3760);
    r.height = pick(d.height, 1992);
    r.dpi = pick(d.dpi, 320);
    r.useMemfd = pick(d.useMemfd, true);
    r.hostAdbPort = pick(bk.hostAdbPort, 5555);
    // The mouse half of the Navigation card, one action char per secondary button
    // (right/middle/4th/5th). The inherited default, and every action distinct: the old bhbn put
    // Back on two buttons and left Recents unreachable without editing the vector by hand.
    r.mouseBind = pick(inp.mouseBind, QStringLiteral("bhsn:++++"));
    // The keyboard half. Empty = emit nothing and let the mirror's lalt,lsuper stand.
    r.shortcutMod = pick(inp.shortcutMod, QString());
    // Direct key bindings (--key-bind). Empty = none; chords alone remain the default.
    r.keyBind = pick(inp.keyBind, QString());
    // 480x270 keeps 16:9 and is small enough to sit beside real work — the point of PIP.
    r.pipWidth = pick(sc.pipWidth, 480);
    r.pipHeight = pick(sc.pipHeight, 270);
    r.kbdIdentity = inp.kbdIdentity;  // nullopt = auto-pick the host's first keyboard
    r.sharedInputs = inp.sharedInputs;
    // Cameras: explicit list is exact (empty = off); AUTO passes the live-probed host nodes but
    // only when the profile opted into the camera feature — a camera nobody asked for reaching
    // the container would be a quiet privacy hole (bd remora-4ei.9).
    if (inp.cameraDevices.has_value()) {
        r.cameraDevices = *inp.cameraDevices;
    } else if (im.features.contains(QStringLiteral("camera_v4l2"))) {
        if (caps.has_value()) r.cameraDevices = caps->videoNodes;
        // Asked for, none arrived. Recorded rather than left as a bare empty list so the deploy
        // can say WHY (see cameraWantedButNoneFound). Also true when there was no live probe at
        // all: "the feature is on and no camera reached the container" is the operator-visible
        // fact either way.
        r.cameraWantedButNoneFound = r.cameraDevices.isEmpty();
    }
    // USB audio: no probe and no node list, unlike cameras. /dev/snd arrives wholesale because the
    // container is privileged, so the only decision is whether this profile wants Android's
    // audioserver given access to it (bd remora-4ei.37).
    r.usbAudio = im.features.contains(QStringLiteral("usb_audio"));
    // ON unless the profile says otherwise (bd remora-6279). Default-on because the host's own
    // topology is not a neutral truth to pass through: a desktop exposes one cpufreq policy per
    // logical CPU (24 here), every phone exposes 2-3, and Android native code sizes fixed
    // per-cluster tables for the latter — the observed cost was a 16-slot table overrun by 640
    // bytes and two crash signatures (bd remora-e5x.28). The trimmed view invents nothing: real
    // frequencies, real CPU membership, fewer entries. Off is for measuring the host's own cpufreq
    // from inside the guest, which is the one question the trimmed view cannot answer.
    r.cpufreqTopology = pick(inp.cpufreqTopology, true);
    r.videoBitRate = pick(sc.videoBitRate, QStringLiteral("30M"));
    r.mirrorAudio = sc.audio.value_or(false);
    // h265, not h264: full-hardware H.265 is always the default and top priority (user
    // directive). This is safe to default BECAUSE of two gates below: an unset A16
    // tag auto-bumps to the -hwc2 image that carries the HEVC encoder, and the capability gate
    // downgrades to h264 — loudly, with the reason recorded — on a host whose GPU has no VA-API
    // encode entrypoint. A profile that pins video_codec still wins outright.
    r.videoCodec = pick(sc.videoCodec, QStringLiteral("h265"));
    // Client-side GPU decode, on unless a profile says otherwise. It has no UI: the mirror picks
    // NVDEC or VAAPI by itself and falls back to software when the host has no usable decoder, so
    // the only thing an off switch ever bought was a slower mirror. hw_decode=false in remorarc is
    // the way out on a host whose decoder misbehaves.
    r.hwDecode = pick(sc.hwDecode, true);
    r.maxSize = pick(sc.maxSize, 0);
    r.mirrorExtra = sc.extra;
    r.windowWidth = sc.windowWidth;
    r.windowHeight = sc.windowHeight;
    r.windowX = sc.windowX;
    r.windowY = sc.windowY;
    // No border knob: the mirror opens as an ordinary decorated window and the desktop decides
    // whether it keeps its frame (a KWin window rule, and its equivalents). Remora forcing
    // SDL_WINDOW_BORDERLESS was the one state the DE could NOT undo.
    r.windowFullscreen = pick(sc.windowFullscreen, false);
    r.fullscreenSeparateDisplay = pick(sc.fullscreenSeparateDisplay, false);
    r.fullscreenDisplay = sc.fullscreenDisplay;
    r.fullscreenApp = sc.fullscreenApp;
    // Freeform popup: a phone-shaped display (portrait, phone dpi) so apps render their handset
    // layout in a compact floating window.
    r.freeformDisplay = pick(sc.freeformDisplay, QStringLiteral("1080x2340/420"));
    r.desktopNewWindow = pick(sc.desktopNewWindow, false);
    r.desktopMenu = pick(c.integration.desktopMenu, true);
    r.sharedFolders = c.integration.sharedFolders;
    r.companionApps = c.integration.companionApps;
    // R3 runtime knobs (opt-in)
    // Refresh rate: 60 is a BAKED default, not opt-in. Leaving this as `d.fps` meant a blank field
    // emitted no fps boot arg at all, so the guest applied its own built-in 30 — the whole
    // UI ran at half rate while the GUI placeholder, its tooltip, the card subtitle and this
    // comment all promised "blank = the proven default (60)". The A17 instance sat at 30 Hz
    // undetected until a frame-timing baseline caught it (bd remora-bm7.1). The default belongs
    // here, not in remorarc: baking it into config would only fix instances written after the fact.
    r.fps = pick(d.fps, 60);
    // Composer HAL: a straight pass-through with NO default (bd remora-emoe). Deliberately not
    // routed through applyHostGpu, which returns nothing in guest mode — gpu_config.sh selects the
    // composer in both modes, so tying it to the host-GPU path would make the option silently
    // inert on SwiftShader. Unset stays unset so the image keeps choosing the proven prebuilt.
    r.hwcomposer = g.hwcomposer;
    // Encoder frame cap (bd remora-rjh/mzw.1): at ≤60 fps stay unset — no --max-fps emitted, the
    // proven default. Above 60 the cap defaults ON at 60: a render rate over the encoder drain
    // rate (~70 fps at 4K) backpressures SurfaceFlinger globally, so fresher frames are only safe
    // because the c2-va component now drops at its input queue.
    r.maxFps = d.maxFps;
    if (!r.maxFps && r.fps && *r.fps > 60) r.maxFps = 60;
    // Guest timezone: only the PIN resolves here. Unset means "follow the host", and the host's
    // zone is an engine fill (applyVendorPaths) — the core cannot read the machine, and a zone
    // baked at resolve time would go stale exactly the way a baked GPU node would (bd remora-c8t).
    r.timezone = d.timezone.value_or(QString());
    r.autoSleepMinutes = adv.autoSleepMinutes;  // opt-in — no baked default
    // Overlay by default: profiles share one read-mostly base and keep private diff layers, so
    // several instances can stand on the same seeded /data. remorarc-only opt-out
    // (use_overlayfs=false) keeps the old plain-bind layout.
    r.useOverlayfs = pick(adv.useOverlayfs, true);
    // codec2 pipeline: ON for every profile, same shape as useOverlayfs above — remorarc-only
    // opt-out (use_codec2=false), no GUI control. It used to be conditional on the image
    // carrying hw_video_decode or on a non-h264 mirror codec, which made it a knob the user had to
    // reason about: the HEVC/AV1 encoders are codec2-only, so every path that wanted them had to
    // remember to turn the pipeline on first, and getting it wrong produced a container whose
    // encoders die NAME_NOT_FOUND with nothing on screen saying why. An image without the c2
    // codecs ignores the boot arg — the c2-va .rc is gated on it, not the other way round — so
    // there is no image this is wrong for, only images it does nothing for.
    r.useCodec2 = pick(adv.useCodec2, true);
    r.roOverrides = adv.roOverrides;
    // At most four DNS servers: ipconfigstore stores every remora_net_dnsN it is given, but four is
    // the most Android's resolver queries, so a fifth entry would emit fine and silently never be
    // used — truncated HERE so `plan` shows exactly what the container will use.
    r.dns = n.dns.mid(0, 4);
    r.proxyType = n.proxyType;
    r.proxyHost = n.proxyHost;
    r.proxyPort = n.proxyPort;
    r.rootHiding = adv.rootHiding;
    r.denylistPackages = adv.denylistPackages;
    r.playSpoof = adv.playSpoof.value_or(false);  // default off — spoofing is opt-in

    // Custom-image pipeline. Paths are on the build host when build_host is set — keep them
    // $HOME-relative-friendly defaults that exist on any machine.
    r.buildHost = im.buildHost;
    r.buildDir = pick(im.buildDir, QDir::homePath() + "/.cache/remora/image-build");
    // Under ~/.local/share, not the visible ~/remora-images it was after the identity pass (bd
    // remora-28ix.4): image archives are generated artifacts, and generated artifacts do not get a
    // top-level folder in $HOME. Still safe to move unlike the instance fallbacks below — these are
    // cold archives, nothing binds them, and a profile that pinned output_dir keeps its own path.
    r.outputDir = pick(im.outputDir, QDir::homePath() + "/.local/share/remora/images");

    // Image tag: explicit override wins; else the requested features (or a non-A16 version)
    // select a tag via gating; else the per-backend A16 default.
    const int androidVersion = im.androidVersion.value_or(16);
    r.androidVersion = androidVersion;
    // A CPU-tuned source build lands under a variant-suffixed tag — sourceBuiltTag() appends
    // "-<variant>" before "-src", and Promote drops only the "-src". The deploy side has to follow
    // it there or the tuned image is built and then never run: `up` silently keeps the portable
    // image, which costs a full build to notice. That is the same silent-mismatch the build side
    // already refuses to allow (it HARD-FAILS when the TARGET_ARCH_VARIANT anchor is missing rather
    // than ship a baseline image under a tuned tag), so the two sides now agree (bd remora-bm7.12).
    //
    // Applied only to a gated/derived tag, never to an explicit one: an explicit image_tag is a
    // literal the user typed and already names whatever it names. Setting arch_variant is itself
    // the opt-in — only a source-building profile can set it meaningfully, and for a source family
    // the gated tag is exactly sourceBuiltTag()'s promoted form, so appending reproduces it.
    const QString archVariant = im.archVariant.value_or(QString());
    const auto withVariant = [&archVariant](const QString &tag) -> QString {
        return archVariant.isEmpty() ? tag : tag + QLatin1Char('-') + archVariant;
    };
    const auto imageTag = [&](const QString &def) -> QString {
        if (im.imageTag.has_value()) return *im.imageTag;
        if (!im.features.isEmpty())
            return withVariant(selectImage(QSet<QString>(im.features.begin(), im.features.end()),
                                           androidVersion)
                                   .tag);
        if (androidVersion != 16)
            return withVariant(selectImage(defaultBuildSet(androidVersion), androidVersion).tag);
        return withVariant(def);
    };
    const QString home = QDir::homePath();

    if (backend == Backend::Bare) {
        r.imageTag = imageTag(QStringLiteral("remora23:x86_64-gapps"));
        r.containerName = pick(bk.containerName, QStringLiteral("remora-bm"));
        r.dataDir = pick(bk.dataDir, home + "/.remora-bm-data");
        // static default → naturally shared by every bare profile on this host
        r.dataBaseDir = pick(bk.dataBaseDir, home + "/.remora-data-base");
        // No baked default: it names a shared staging tree on the DOCKER HOST, which only the
        // operator knows the layout of. Unset means companion apps are named by their own path and
        // push-app has nothing to copy to (the local docker host is already reachable).
        r.rezmodsDir = bk.rezmodsDir;
        r.gpuMode = pick(g.mode, GpuMode::Guest);
        // gpu_mode=host on a local docker host: the live render node the probe picked instead of
        // SwiftShader — or, when the NVIDIA Venus prerequisites are present, that stack instead.
        applyHostGpu(r, resolveHostGpu(g, caps, r.gpuMode));
        applyNetwork(r, n, caps, QStringLiteral("bridge"));
        r.containerAdbPort = 5556;  // in-container fwd shim (A16 adbd is IPv6-only)
        // LOOPBACK BY DEFAULT. A bare instance is reached from this machine only, and a plain
        // `-p 5555:5556` publishes adb — a root shell into the device — on every interface the
        // host has, so anyone on the same network could connect to it. adb_bind=0.0.0.0 restores
        // the old reach for a profile that wants it; macvlan publishes nothing either way.
        r.adbBindAddress = pick(bk.adbBindAddress, QStringLiteral("127.0.0.1"));
        r.target = QStringLiteral("localhost:%1").arg(r.hostAdbPort);
    } else {  // Backend::Remote — generic docker host over ssh, bare-style bringup
        r.imageTag = imageTag(QStringLiteral("remora23:x86_64-gapps"));
        r.containerName = pick(bk.containerName, QStringLiteral("remora-remote"));
        r.dataDir = pick(bk.dataDir, QStringLiteral("~/remora-data"));  // remote shell expands ~
        r.dataBaseDir = pick(bk.dataBaseDir, QStringLiteral("~/remora-data-base"));
        r.rezmodsDir = bk.rezmodsDir;  // as bare: no baked default, and no remorarc key sets it
        r.gpuMode = pick(g.mode, GpuMode::Guest);
        // same host-GPU path as bare, scanned over ssh on the remote docker host
        applyHostGpu(r, resolveHostGpu(g, caps, r.gpuMode));
        applyNetwork(r, n, caps, QStringLiteral("bridge"));
        r.containerAdbPort = 5556;
        // Every interface by default: the client connects from ANOTHER machine, so loopback would
        // be unreachable. Narrow it with adb_bind= to the interface the client comes in on.
        r.adbBindAddress = pick(bk.adbBindAddress, QString());
        r.sshHost = pick(bk.sshHost, QString());
        const QString host = r.sshHost->contains('@') ? r.sshHost->section('@', -1)
                             : (r.sshHost->isEmpty() ? QStringLiteral("localhost") : *r.sshHost);
        r.target = QStringLiteral("%1:%2").arg(host).arg(r.hostAdbPort);
    }

    // MACVLAN OVERRIDES THE TARGET on every backend. There is no published host port in this mode,
    // so the address is the container's own and the port is the one the container LISTENS on —
    // NOT hostAdbPort, which describes a bind that does not exist here. The two differ (the
    // in-container fwd shim listens on 5556), and reaching for the wrong one produced a container
    // that was up, addressed and unreachable. No pinned IP → empty, and the engine reads the
    // IPAM-assigned address off the running container at connect time.
    if (r.networkMode == QLatin1String("macvlan"))
        r.target = r.macvlanIp.has_value()
                       ? QStringLiteral("%1:%2").arg(*r.macvlanIp).arg(r.containerAdbPort)
                       : QString();

    // The VAAPI HW video encoder (sharp full-res h265/av1 mirroring — vs the software HEVC encoder that
    // AOSP hard-caps at 512x512) lives in the -hwc2 image, alongside the HW decoder. Bump a resolved
    // A16 gapps/gapps-wv image to -hwc2 when a HW-encoded mirror codec is selected (unless the tag
    // was set explicitly). useCodec2 is already on in that case, activating the codec2 pipeline.
    if (!im.imageTag.has_value() && r.videoCodec != QLatin1String("h264") &&
        (r.imageTag == QLatin1String("remora23:x86_64-gapps-wv") ||
         r.imageTag == QLatin1String("remora23:x86_64-gapps")))
        r.imageTag = QStringLiteral("remora23:x86_64-gapps-wv-hwc2");

    // Whether the image BAKES the mirror agent as a boot service (bd remora-28ix.3.4): true when
    // the profile requests the mirror_agent build feature — a source build stages the agent
    // whether or not any tag advertises it yet, which is exactly the pre-cutover validation
    // window — or when the resolved tag is registered as providing it, the post-cutover shape
    // where the default set carries the feature and profiles leave features= unset. Builders
    // reads this to DROP the dev rc/dex bind mounts (see the transition note in the baked
    // remora-agent.rc). A variant-suffixed tag is looked up under its registered name — the
    // suffix marks a CPU tuning of the same build, not a different feature set.
    QString registryTag = r.imageTag;
    if (!archVariant.isEmpty() && registryTag.endsWith(QLatin1Char('-') + archVariant))
        registryTag.chop(archVariant.size() + 1);
    const ImageTag *taggedImage = findImageTag(registryTag);
    r.agentBaked =
        im.features.contains(QStringLiteral("mirror_agent")) ||
        (taggedImage && taggedImage->provides.contains(QStringLiteral("mirror_agent")));

    // HW-ENCODE CAPABILITY GATE, applied LAST and at ONE point so bare and remote cannot drift
    // (bd remora-e5x.4). Everything above may have selected h265, which Builders then pins to
    // c2.remora.vaapi.hevc.encoder — the image's ONLY HEVC encoder, since it ships no software
    // one. On a host with no VA encode entrypoint that component cannot start, and the deploy
    // reports nothing more useful than "the mirror exited immediately after 5 attempts". A host whose
    // only GPU is NVIDIA is exactly that case: its VA driver is NVDEC, decode-only.
    //
    // So fall back to h264, which the image CAN always encode (its software H.264 encoder).
    // Degraded, but connected — and named, so it reads as a trade rather than as the user having
    // asked for h264.
    //
    // Only for host mode: guest mode is SwiftShader (software all the way). An explicit va_driver=
    // is treated as the user asserting a capability we cannot see, and suppresses the downgrade.
    const bool hostGpuBackend = r.gpuMode == GpuMode::Host;

    // Host encode: opt-in, and only where there is a host GPU to encode on. It exists precisely
    // for the case the downgrade below handles — a GPU that renders but cannot VA-API encode — so
    // it is resolved first and suppresses the downgrade.
    // No device jar to resolve any more: the purge deleted the v1 path (bd remora-28ix.5), and
    // the device half is the agent the image bakes. Host encode driving the mirror used to be
    // gated on a compose-capable jar being named; the agent composes natively, so hostEncode
    // alone decides (a per-window opt-out — PIP — still clears the flag on its own copy).
    r.hostEncode = sc.hostEncode.value_or(false) && hostGpuBackend;
    // Host DECODE, gated the same way and for the same reason (bd remora-e5x.37): there must be a
    // host GPU for the helper to decode on. Deliberately NOT tied to hostEncode — they are
    // opposite directions through different helpers, and running decode remote is exactly what
    // frees the local VADisplay for encode. A profile can want either alone.
    r.hostDecode = sc.hostDecode.value_or(false) && hostGpuBackend;
    if (r.hostDecode) r.hostDecodeSocket = hostDecodeSocketFor(r.containerName);
    // There is no compose-source choice left to resolve (bd remora-emoe): Remora's own hwcomposer
    // is the sole publisher, and the render server's interception is never armed — see the
    // REMORA_FRAME_SOCKET comment in Chain.cpp, which withholds it unconditionally.
    if (r.hostEncode) {
        r.hostEncodeFrameSocket = hostEncodeFrameSocketFor(r.containerName);
        r.hostEncodeVideoSocket = hostEncodeVideoSocketFor(r.containerName);
        r.hostEncodeMaxSize = sc.hostEncodeMaxSize.value_or(0);
        r.hostEncodeDrivesMirror = true;
        // A CAPTURING profile gets its own venus socket dir, hence its own render server
        // (bd remora-4u4.9). This is not a compromise between sharing and not sharing: frame
        // capture is fixed at server start (REMORA_FRAME_SOCKET is read once), so a capturing
        // profile needs a server to itself BY DEFINITION. Deriving the path from the container
        // name makes two of them impossible to collide, with no probing of what is running.
        //
        // Render-only profiles are untouched and keep sharing the one default server — that is
        // where sharing is real (--multi-clients) and where a second server would cost GPU memory
        // for nothing. An explicit venus_socket_dir still wins; if two capturing profiles are
        // pointed at the SAME one by hand, ensureVenusServer still refuses and says why.
        if (r.venus && !c.gpu.venusSocketDir.has_value())
            r.venusSocketDir = QDir::homePath() +
                               QStringLiteral("/.cache/remora/venus-%1").arg(r.containerName);
    }

    if (hostGpuBackend && !r.hostEncode && r.videoCodec == QLatin1String("h265") &&
        !g.vaDriver.has_value() &&
        caps.has_value() && caps->hostRenderDriver.has_value() &&
        !driverHasVaEncode(*caps->hostRenderDriver)) {
        r.videoCodecFallback =
            QStringLiteral("h265 needs the VAAPI HEVC encoder and '%1' has no VA-API encode "
                           "entrypoint (the image ships no software HEVC encoder) — using h264")
                .arg(*caps->hostRenderDriver);
        r.videoCodec = QStringLiteral("h264");
    }
    // Guest rendering is the same dead end from the other side (bd remora-hsdz): its gralloc cannot
    // allocate the YUV buffers the VAAPI encoders take, and the image ships no software HEVC
    // encoder. Measured on a guest-mode A17 container,: c2.remora.vaapi.hevc.encoder was
    // created, then three GraphicBufferAllocator failures and the mirror ended within 15 s; the
    // software c2.android.avc.encoder — which the hwc2 vector pins for h264 — streamed with none.
    if (!r.hostEncode && r.gpuMode == GpuMode::Guest && r.videoCodec == QLatin1String("h265")) {
        r.videoCodecFallback =
            QStringLiteral("h265 needs the VAAPI HEVC encoder, and gpu_mode=guest renders with no "
                           "host GPU to run it on (the image ships no software HEVC encoder) — "
                           "using h264");
        r.videoCodec = QStringLiteral("h264");
    }

    return r;
}

QString hostEncodeFrameSocketDirFor(const QString &containerName) {
    // A DIRECTORY, because the composer reaches this socket through a bind mount and the encoder
    // recreates the socket INODE every time it restarts (bd remora-emoe phase 2). Binding the file
    // would leave a container holding a deleted one — the same trap venusSocketDir already exists
    // to avoid, and its comment records: "the server recreates that inode on restart".
    return QStringLiteral("/tmp/remora-frames-%1").arg(containerName);
}

QString hostEncodeFrameSocketFor(const QString &containerName) {
    return hostEncodeFrameSocketDirFor(containerName) + QStringLiteral("/frame.sock");
}

QString hostEncodeVideoSocketFor(const QString &containerName) {
    return QStringLiteral("/tmp/remora-video-%1.sock").arg(containerName);
}

QString hostDecodeSocketDirFor(const QString &containerName) {
    // A DIRECTORY, for the same reason hostEncodeFrameSocketDirFor is one: the helper recreates the
    // socket INODE on every restart, so a container that bind-mounted the FILE would be left
    // holding a deleted one and every reconnect would fail against a socket nobody is listening
    // on. Mount the directory and let the path be resolved per connect.
    return QStringLiteral("/tmp/remora-decode-%1").arg(containerName);
}

QString hostDecodeSocketFor(const QString &containerName) {
    return hostDecodeSocketDirFor(containerName) + QStringLiteral("/decode.sock");
}

QString hostEncodeStatusFileFor(const QString &containerName) {
    // The encoder publishes its shape at <frame socket>.status (bd remora-e5x.18.3); derived from
    // the frame-socket rule so the writer and every reader agree by construction.
    return hostEncodeFrameSocketFor(containerName) + QStringLiteral(".status");
}

QString profileSlug(const QString &profileName) {
    QString slug = profileName.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (slug.startsWith(QLatin1Char('-'))) slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-'))) slug.chop(1);
    return slug.isEmpty() ? QStringLiteral("profile") : slug;
}

InstanceIdentity allocateInstanceIdentity(const QString &profileName, const QString &homeDir,
                                          const QSet<int> &takenPorts, int basePort) {
    const QString slug = profileSlug(profileName);
    // Lowest free port at or above the base. Linear rather than hashed on purpose: a hash can
    // collide, and "the second profile gets 5556" is something a user can predict and read in
    // `plan`. The ceiling is a runaway guard, not a real limit.
    int port = basePort;
    while (takenPorts.contains(port) && port < basePort + 1000) ++port;
    // The data dir is named for the PROFILE and nothing else: ~/.remora-<slug>. It is the one
    // path a user has to recognise on disk — "which folder is Android 17's /data" has to be
    // answerable by reading the name, not by knowing which engine writes it.
    // remora-<slug> since the identity rename (bd remora-28ix.4). CREATION-TIME only: an existing
    // profile keeps whatever container_name it pinned at its own creation, and a pin later edited
    // to the new spelling has its container carried over by adoptLegacyContainer at provision.
    return InstanceIdentity{QStringLiteral("remora-%1").arg(slug),
                            homeDir + QStringLiteral("/.remora-%1").arg(slug), port};
}

}  // namespace remora
