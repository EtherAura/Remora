#include "core/Builders.h"

// hostEncodeFrameSocketDirFor: the composer's bind mount and the encoder's own socket path must
// come from ONE rule, or the container mounts a directory the encoder never binds in.
#include "core/Resolver.h"

#include <QRegularExpression>

namespace remora {

const QList<QPair<QString, QString>> &venusGuestLibs() {
    // The gbm wrapper is what gralloc.minigbm_gbm_mesa dlopens to reach the host's Mesa; the ANGLE
    // trio is what ro.hardware.egl=angle resolves to. None is produced by our source build, so all
    // come from the prebuilt set (see vendor/host-prereqs/venus-nvidia/README.md).
    static const QList<QPair<QString, QString>> libs{
        // The ICD must be OVERRIDDEN, not inherited. The image ships a vulkan.virtio.so of its own
        // (~1.2 MB) and it is NOT equivalent to the prebuilt Venus ICD (~29.7 MB): it reaches
        // NVIDIA and renders correctly — the GLES string reads exactly the same — but capture is
        // dead. screencap hangs and the mirror never receives a frame, so the failure looks like a
        // mirroring bug rather than a wrong library. Verified by A/B against a container that
        // differed only in this mount: 275 frames with the prebuilt, 0 with the image's.
        {QStringLiteral("vulkan.virtio.so"), QStringLiteral("/vendor/lib64/hw/vulkan.virtio.so")},
        {QStringLiteral("libgbm_mesa_wrapper.so"),
         QStringLiteral("/vendor/lib64/libgbm_mesa_wrapper.so")},
        {QStringLiteral("libEGL_angle.so"), QStringLiteral("/vendor/lib64/egl/libEGL_angle.so")},
        {QStringLiteral("libGLESv2_angle.so"),
         QStringLiteral("/vendor/lib64/egl/libGLESv2_angle.so")},
        {QStringLiteral("libGLESv1_CM_angle.so"),
         QStringLiteral("/vendor/lib64/egl/libGLESv1_CM_angle.so")},
    };
    return libs;
}

QString sharedFolderBind(const QString &entry) {
    const QString host = entry.section(QLatin1Char(':'), 0, 0);
    QString name = entry.section(QLatin1Char(':'), 1, 1);
    if (name.isEmpty())
        name = host.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
    return host + QStringLiteral(":/data/media/0/") + name;
}

// Overlayfs staging spot for a shared folder: the same bind, parked outside /data so the
// init-time overlay mount cannot shadow it. Provision re-binds it into the merged /data/media/0
// after boot (fixupSharedFolders in Chain.cpp) — that is what keeps shares working now that the
// overlay is the default.
QString stagedShareBind(const QString &entry) {
    QString b = sharedFolderBind(entry);
    return b.replace(QLatin1String(":/data/media/0/"), QLatin1String(":/remora/share/"));
}

static QString companionAppRaw(const QString &entry) {
    return companionAppIsLocal(entry) ? entry.mid(6) : entry;  // strip "local:"
}

bool companionAppIsLocal(const QString &entry) {
    return entry.startsWith(QLatin1String("local:"));
}

QString companionAppName(const QString &entry) {
    const QString raw = companionAppRaw(entry);
    const QString name = raw.section(QLatin1Char(':'), 1, 1);
    if (!name.isEmpty()) return name;
    return raw.section(QLatin1Char(':'), 0, 0)
        .section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
}

QString companionAppSourceDir(const QString &entry) {
    if (!companionAppIsLocal(entry)) return QString();
    return companionAppRaw(entry).section(QLatin1Char(':'), 0, 0);
}

QString companionAppHostDir(const QString &entry, const QString &rezmodsDir) {
    const QString dir = companionAppRaw(entry).section(QLatin1Char(':'), 0, 0);
    if (!companionAppIsLocal(entry)) return dir;
    // Staged entries land in the same shared tree the guest scripts already use. Without a rezmods
    // dir the docker host is this machine, so the source path is already reachable.
    if (rezmodsDir.isEmpty()) return dir;
    return rezmodsDir + QStringLiteral("/apps/") + companionAppName(entry);
}

QString companionAppBind(const QString &entry, const QString &rezmodsDir) {
    // system_ext is NESTED under system/ in this device tree, so the in-container path is
    // /system/system_ext/... — the same place RemoraUpdater lands. Read-only: the image owns
    // /system, and the app is only ever read from here.
    return companionAppHostDir(entry, rezmodsDir) + QStringLiteral(":/system/system_ext/app/")
           + companionAppName(entry) + QStringLiteral(":ro");
}

QString companionAppsRemoteRefusal(const QStringList &companionApps) {
    // "local:" means "this directory lives on THIS machine and Remora stages it" — and the step
    // that staged it was VmDeployer's and went with the vm backend, so on a remote docker host the
    // form now has NO execution path (bd remora-pzq). Deployed anyway, the bind names a path that
    // exists on the wrong machine; docker answers a missing bind source by CREATING it, empty, so
    // the app silently does not install and nothing errors. Refused, like backend=vm and the /data
    // shape mismatch: the failure this prevents is invisible and reads as success.
    QStringList local;
    for (const QString &a : companionApps)
        if (companionAppIsLocal(a)) local << companionAppName(a);
    if (local.isEmpty()) return QString();
    return QStringLiteral(
               "companion app(s) %1 use the \"local:\" form, but nothing stages a local directory "
               "to a remote docker host (the vm staging step is retired — bd remora-pzq). "
               "Deployed as-is, docker would create the bind source EMPTY on the remote and the "
               "app would silently not install. Either copy the directory to the remote host "
               "yourself and name its remote path (drop the \"local:\" prefix), or remove the "
               "entry from companion_apps.")
        .arg(local.join(QStringLiteral(", ")));
}

QStringList buildMacvlanCreateArgv(const ResolvedConfig &cfg) {
    if (cfg.networkMode != QLatin1String("macvlan")) return {};
    const QString name = cfg.dockerNetwork.value_or(QString());
    if (name.isEmpty() || !cfg.macvlanParent || !cfg.macvlanSubnet || !cfg.macvlanGateway)
        return {};
    QStringList a{"docker", "network", "create", "-d", "macvlan",
                  "--subnet", *cfg.macvlanSubnet, "--gateway", *cfg.macvlanGateway};
    if (cfg.macvlanRange.has_value()) a << "--ip-range" << *cfg.macvlanRange;
    a << "-o" << QStringLiteral("parent=%1").arg(*cfg.macvlanParent) << name;
    return a;
}

const char *const kMacvlanShimName = "remora-shim0";
// busybox is only the LAUNCHER — 4 MB, and all that is wanted from it is `chroot`. The `ip` that
// actually runs is the HOST's, reached by chrooting into the host filesystem (mounted read-only;
// the commands touch netlink, not files).
//
// This indirection is load-bearing, not fastidiousness. busybox's OWN `ip` accepts
// "type macvlan mode bridge" and then silently creates the interface in VEPA mode, where sibling
// macvlan devices cannot talk to each other at all — they are expected to be reflected back by an
// 802.1Qbg switch that no home network has. The shim comes up, carries the right address and the
// right routes, `ip link` looks perfect, and nothing can reach anything. Cost a debugging session
// (bd remora-400). The mode is only visible under `ip -d link`, and only real iproute2 sets it.
const char *const kMacvlanShimImage = "busybox";

QStringList macvlanShimRoutes(const ResolvedConfig &cfg) {
    QStringList blocks;
    if (cfg.macvlanRange.has_value()) blocks << *cfg.macvlanRange;
    // Spelled the way `ip route show` PRINTS a host route — bare, no /32 — because the reconcile
    // below compares this list against that output token for token. A "/32" here would match
    // nothing, and every connect would delete the route it had just installed.
    if (cfg.macvlanIp.has_value() &&
        !(cfg.macvlanRange.has_value() && ipv4InSubnet(*cfg.macvlanIp, *cfg.macvlanRange)))
        blocks << *cfg.macvlanIp;
    return blocks;
}

QStringList buildMacvlanShimArgv(const ResolvedConfig &cfg) {
    if (cfg.networkMode != QLatin1String("macvlan") || !cfg.macvlanHostRoute) return {};
    if (!cfg.macvlanParent || !cfg.macvlanShimIp) return {};
    const QStringList blocks = macvlanShimRoutes(cfg);
    if (blocks.isEmpty()) return {};
    const QString shim = QString::fromUtf8(kMacvlanShimName);
    const QString want = blocks.join(QLatin1Char(' '));
    // RECONCILE, don't just create. Remora owns this interface, so every connect brings it to the
    // configured state rather than accepting whatever is already called remora-shim0 — which may
    // be a shim built against a different parent, or left by an older Remora that made it in VEPA
    // mode. A plain `ip link show ... || ip link add` accepts both of those forever, and a VEPA
    // shim reaches nothing while looking perfect (bd remora-400).
    //
    // Recreation is conditional precisely because it is disruptive: it drops the adb connection
    // riding the shim, so a correct interface is left alone and only address and routes are
    // re-asserted, both idempotent.
    QStringList script{
        QStringLiteral("want='%1'").arg(want),
        QStringLiteral("cur=$(ip -d link show %1 2>/dev/null || true)").arg(shim),
        // Two properties are structural and cannot be changed on a live interface: which NIC it
        // hangs off, and the mode that decides whether siblings can talk at all.
        QStringLiteral("case \"$cur\" in *%1@%2:*) parent_ok=1 ;; *) parent_ok= ;; esac")
            .arg(shim, *cfg.macvlanParent),
        QStringLiteral("case \"$cur\" in *'macvlan mode bridge'*) mode_ok=1 ;; *) mode_ok= ;; esac"),
        QStringLiteral("if [ -z \"$cur\" ] || [ -z \"$parent_ok\" ] || [ -z \"$mode_ok\" ]; then "
                       "ip link del %1 2>/dev/null || true; "
                       "ip link add %1 link %2 type macvlan mode bridge; fi")
            .arg(shim, *cfg.macvlanParent),
        // /32: the host already has a connected route for the whole LAN on the parent, and a
        // second one here would make the subnet ambiguous for every other destination on it.
        QStringLiteral("ip addr replace %1/32 dev %2").arg(*cfg.macvlanShimIp, shim),
        QStringLiteral("ip link set %1 up").arg(shim),
        QStringLiteral("for w in $want; do ip route replace $w dev %1 src %2; done")
            .arg(shim, *cfg.macvlanShimIp),
        // …and withdraw what is no longer wanted. Without this a re-pinned container leaves its old
        // /32 behind, and the host keeps routing an address nothing answers on — the stale-state
        // failure this whole feature has been made of.
        QStringLiteral("for r in $(ip -o route show dev %1 2>/dev/null | awk '{print $1}'); do "
                       "case \" $want \" in *\" $r \"*) ;; "
                       "*) ip route del \"$r\" dev %1 2>/dev/null || true ;; esac; done")
            .arg(shim)};
    return QStringList{"docker", "run", "--rm", "--privileged",
                       // the host's own network namespace — that is where the shim has to land
                       "--network", "host",
                       // …and the host's own iproute2, read-only. See kMacvlanShimImage.
                       "-v", "/:/host:ro",
                       QString::fromUtf8(kMacvlanShimImage), "chroot", "/host", "/bin/sh", "-c",
                       script.join(QStringLiteral(" && "))};
}

QStringList buildDockerRunArgv(const ResolvedConfig &cfg) {
    QStringList a{"docker", "run", "-d", "--privileged", "--name", cfg.containerName};

    // /data delivery: plain per-instance bind, or the overlayfs multi-instance pair — the guest's
    // init overlays lowerdir=/data-base + upper/work in /data-diff onto /data at early-init, so
    // dataDir becomes the diff layer and the shared base mounts read-mostly beside it.
    const QStringList dataMounts =
        cfg.useOverlayfs ? QStringList{"-v", cfg.dataBaseDir + ":/data-base", "-v",
                                       cfg.dataDir + ":/data-diff"}
                         : QStringList{"-v", cfg.dataDir + ":/data"};

    // Networking, identical on every backend (bd remora-400): macvlan puts the container on the
    // LAN with its own address and publishes nothing, bridge publishes the adb port on the docker
    // host. The container side is the fwd shim (5556) on both backends.
    if (cfg.networkMode == QLatin1String("macvlan")) {
        a << "--network" << cfg.dockerNetwork.value_or(QString());
        // no pinned IP → omit --ip so docker IPAM assigns a free one from the pool
        if (cfg.macvlanIp.has_value()) a << "--ip" << *cfg.macvlanIp;
    } else {
        // An empty bind or 0.0.0.0 publishes on every interface; anything else is an address.
        const bool everywhere = cfg.adbBindAddress.isEmpty() ||
                                cfg.adbBindAddress == QLatin1String("0.0.0.0");
        a << "-p"
          << (everywhere ? QStringLiteral("%1:%2").arg(cfg.hostAdbPort).arg(cfg.containerAdbPort)
                         : QStringLiteral("%1:%2:%3")
                               .arg(cfg.adbBindAddress)
                               .arg(cfg.hostAdbPort)
                               .arg(cfg.containerAdbPort));
    }

    // host GPU mode: pass the ONE resolved node, not all of /dev/dri. The container also sees the
    // host's other render nodes via --privileged, so this is not the access grant — it is the
    // statement of intent, and it keeps working if the privileged flag is ever dropped. Absent
    // unless a node actually resolved (anti-stale-bake).
    if (cfg.gpuNode.has_value())
        a << "--device" << QStringLiteral("%1:%1").arg(*cfg.gpuNode);
    // Venus: the NVIDIA node alongside the render node above. The guest's Vulkan never touches
    // it — that goes over the socket mounted below — but the gbm wrapper probes the DRI nodes,
    // and naming the device keeps the deployment honest if --privileged is ever dropped.
    if (cfg.venus && cfg.venusNode.has_value())
        a << "--device" << QStringLiteral("%1:%1").arg(*cfg.venusNode);
    // The --device lines STATE the selection; these binds ENFORCE it. A privileged container
    // sees every /dev/dri node, and the in-container stack (gralloc device discovery, Mesa)
    // picks its own — masking each non-selected GPU's nodes with /dev/null is what actually
    // constrains the choice on a multi-GPU host (bd remora-n4qx, measured on amd-host-b).
    for (const QString &n : cfg.maskedGpuNodes)
        a << "-v" << QStringLiteral("/dev/null:%1").arg(n);
    a << dataMounts;
    // netfix pair, read-only. These MUST be mounts rather than a post-boot `docker cp`: init
    // remounts the container rootfs read-only at boot, and cp needs to create a .pivot_root
    // dir at the rootfs root, so it fails for every destination (bd remora-4ei.59). `fwd` is
    // already 755 on the host and the script is run as `sh <path>`, so nothing needs chmod
    // in-container — which is what makes a read-only mount sufficient.
    //
    // Destination is /remora, NOT under /data: with use_overlayfs, init overlays /data at
    // early-init and overlay layers do not traverse mounts, so anything bound beneath /data
    // is shadowed and adb could never be wired (bd remora-4u4.3 — this is also why
    // sharedFolders are omitted under overlayfs). /remora is outside /data and therefore
    // survives identically with and without the overlay, so there is one path, not two.
    // remora-netfix.sh finds `fwd` beside itself via dirname "$0" rather than at a fixed
    // path, so moving this destination does not mean editing the script again.
    if (cfg.netfixFwdPath.has_value())
        a << "-v" << QStringLiteral("%1:/remora/fwd:ro").arg(*cfg.netfixFwdPath);
    if (cfg.netfixScriptPath.has_value())
        a << "-v" << QStringLiteral("%1:/remora/netfix.sh:ro").arg(*cfg.netfixScriptPath);
    // /vendor/etc/init, NOT /remora: this one is read by ANDROID INIT, which only parses .rc files
    // from the partitions' own init dirs, so it is the one piece of the forwarder that cannot live
    // beside the others. Mounting it needs no image change — init picks it up on every boot with
    // the vendor HAL .rc files — and it is what makes an externally restarted container come back
    // with a working forwarder instead of a healthy Android and dead adb (bd remora-yn4).
    if (cfg.netfixInitRcPath.has_value())
        a << "-v"
          << QStringLiteral("%1:/vendor/etc/init/remora-fwd.rc:ro").arg(*cfg.netfixInitRcPath);
    // remora.props.rc (bd remora-28ix.4), same road as the forwarder's rc above: the boot
    // properties that must be DERIVED from a remora_* value (see Resolved.h). It travels in the
    // SAME argv that emits the remora_* boot args below, so there is no reachable deploy where
    // those args meet an image that cannot derive them, however old the image. Images built with
    // the staged vendor/remora project install the same file, and the mount shadows it.
    if (cfg.propsInitRcPath.has_value())
        a << "-v"
          << QStringLiteral("%1:/vendor/etc/init/remora.props.rc:ro").arg(*cfg.propsInitRcPath);
    // The mirror agent travels both roads at once: its dex beside the netfix pair in /remora, its
    // init service beside the forwarder's in /vendor/etc/init. Same read-only mounts, same reason
    // — the rootfs is read-only after boot, so a mount is the only way in (bd remora-28ix.3).
    // UNLESS the image bakes the agent (mirror_agent, bd remora-28ix.3.4): the baked rc already
    // defines `service remora_agent`, init discards a second definition as a duplicate, and which
    // of the two it read first is a parse-order accident — a mounted dev dex could silently
    // shadow the agent the image shipped. Baked images ride with no agent mounts at all.
    if (!cfg.agentBaked) {
        if (cfg.agentDexPath.has_value())
            a << "-v" << QStringLiteral("%1:/remora/agent.dex:ro").arg(*cfg.agentDexPath);
        if (cfg.agentInitRcPath.has_value())
            a << "-v"
              << QStringLiteral("%1:/vendor/etc/init/remora-agent.rc:ro").arg(*cfg.agentInitRcPath);
    }
    // The phantom keyboard rides in the same way, for the same reasons: same /remora destination,
    // same read-only mount, already 755 on the host so nothing needs chmod in-container.
    if (cfg.uinputKbdPath.has_value())
        a << "-v" << QStringLiteral("%1:/remora/uinput-kbd:ro").arg(*cfg.uinputKbdPath);
    // NVIDIA Venus stack (bd remora-4ei.56), in two parts here: the render server's socket
    // DIRECTORY (a directory, not the socket file — the server recreates that inode on every
    // restart) and the guest libraries the image does not ship. Its third part, the /vendor
    // build.prop overlay, is emitted below because it is no longer venus-only.
    if (cfg.venus) {
        if (cfg.venusSocketDir.has_value())
            a << "-v" << QStringLiteral("%1:/dev/venus").arg(*cfg.venusSocketDir);
        if (cfg.venusLibDir.has_value())
            for (const auto &lib : venusGuestLibs())
                a << "-v" << QStringLiteral("%1/%2:%3:ro")
                                 .arg(*cfg.venusLibDir, lib.first, lib.second);
    }

    // COMPOSE MODE, THE COMPOSER HALF (bd remora-emoe phase 2). The Remora hwcomposer publishes the
    // composed client target straight to the host encoder, so the encoder is HANDED the display
    // instead of inferring which render-server context is the screen — pick_display's whole job,
    // and the source of the three bugs its comments record.
    // A DIRECTORY, not the socket file, for the reason the venus bind above states: the encoder
    // recreates the socket inode whenever it restarts, and a file bind would leave the container
    // holding a deleted one.
    // Emitted whenever host encode is on. Both publishers then exist — the Venus interception and
    // the composer — and pick_display arbitrates, which is the deliberate incremental step: the
    // composer's frames win on their own merits (largest extent, always live) before anything is
    // deleted. Removing the interception is a later change, once this is proven end to end.
    if (cfg.hostEncode && !cfg.hostEncodeFrameSocket.isEmpty()) {
        a << "-v" << QStringLiteral("%1:/dev/remora-frames")
                         .arg(hostEncodeFrameSocketDirFor(cfg.containerName));
    }
    // The decode helper's socket, same shape and same directory-not-file rule (bd remora-e5x.37).
    // The mount and the property below MOVE TOGETHER, deliberately: a container that mounted the
    // socket without being told its path would look identical to one with no helper at all, and a
    // container told a path it has not mounted would fail every connect. Pairing them in one
    // resolved flag is what makes those two states unreachable.
    if (cfg.hostDecode && !cfg.hostDecodeSocket.isEmpty()) {
        a << "-v" << QStringLiteral("%1:/dev/remora-decode")
                         .arg(hostDecodeSocketDirFor(cfg.containerName));
    }
    // The generated /vendor/build.prop overlay rides OUTSIDE the venus gate: gpu_mode=guest needs
    // it too — the image bakes ro.hardware.vulkan=intel, ro.* is first-wins over gpu_setup_guest's
    // setprop, and a host with no Intel GPU then loops SurfaceFlinger in chooseEglConfig (bd
    // remora-e5x.33). Re-gated on mode so a populated path can never leak the mount into a plain
    // host-mode deploy.
    if ((cfg.venus || cfg.gpuMode == GpuMode::Guest) && cfg.buildPropOverlay.has_value())
        a << "-v" << QStringLiteral("%1:/vendor/build.prop:ro").arg(*cfg.buildPropOverlay);
    // Cameras for the external camera provider (bd remora-4ei.9). Empty unless the profile
    // carries the camera_v4l2 feature (auto: the live-probed host nodes) or an explicit
    // camera_devices= list — see the resolver for the privacy gate.
    for (const QString &cam : cfg.cameraDevices)
        a << "--device" << QStringLiteral("%1:%1").arg(cam);
    // The trimmed cpufreq view (bd remora-6279), mounted OVER the host's own sysfs directory. A
    // bind at a /sys subpath is applied after the runtime sets /sys up, so the surrounding tree —
    // cpu[0-9]*, nproc, everything else the guest reads — is untouched; only the policy
    // enumeration changes. Verified against a privileged container, which is what the guest runs as.
    // :ro because the guest has no business writing a governor it does not own, and cpufreq under
    // a container is read-only anyway.
    if (!cfg.cpufreqDir.isEmpty())
        a << "-v"
          << QStringLiteral("%1:/sys/devices/system/cpu/cpufreq:ro").arg(cfg.cpufreqDir);
    // Shared folders land under /data/media/0 (= /sdcard). Under overlayfs (the default) the
    // init-time overlay mount covers /data and would shadow any docker sub-bind beneath it
    // (overlay layers don't traverse mounts) — so the binds stage at /remora/share/<name> and
    // provision bind-mounts them into the merged /data after boot. A legacy
    // use_overlayfs=false keeps the direct binds.
    for (const QString &f : cfg.sharedFolders)
        a << "-v" << (cfg.useOverlayfs ? stagedShareBind(f) : sharedFolderBind(f));
    // Companion apps mount into /system, which no init-time overlay covers, so unlike the shared
    // folders above these are emitted regardless of useOverlayfs.
    for (const QString &app : cfg.companionApps)
        a << "-v" << companionAppBind(app, cfg.rezmodsDir.value_or(QString()));

    a << cfg.imageTag;
    a << buildBootArgs(cfg);
    return a;
}

// Boot args go out in the androidboot.remora_* namespace — Remora's own contract as of bd
// remora-28ix.4, and since step 4 the ONLY namespace there is: every in-image consumer reads
// ro.boot.remora_* directly, and remora.props.rc (mounted by the argv above, and baked) is down
// to its two genuine derivations rather than a mapping. The one-namespace-per-boot rule that
// governed the transition still stands as a design constraint — two rc files each firing the
// overlay trigger is how /data's overlay gets mounted twice.
//
// Its own function because the smart-connect gate compares THIS list against a running
// container's .Args (bd remora-wlun): one comparison that grows with the builder, instead of the
// per-arg probe list that lagged it by construction.
QStringList buildBootArgs(const ResolvedConfig &cfg) {
    QStringList a;
    a << QStringLiteral("androidboot.remora_gpu_mode=%1").arg(gpuModeToString(cfg.gpuMode));
    // anti-stale-bake: the gpu_node token is absent entirely unless a live node was resolved.
    if (cfg.gpuNode.has_value())
        a << QStringLiteral("androidboot.remora_gpu_node=%1").arg(*cfg.gpuNode);
    // gralloc HAL override. Absent → gpu_config.sh's own default (gbm) applies, which is correct
    // for every driver except xe; see resolveHostGpu() for why xe gets minigbm_intel.
    if (cfg.gralloc.has_value())
        a << QStringLiteral("androidboot.remora_gralloc=%1").arg(*cfg.gralloc);
    // Composer HAL selection (bd remora-emoe). Emitted only when asked for: unset means the image
    // picks, and the image picks the inherited prebuilt. Both HALs are installed, so this switches
    // which one hw_get_module loads without touching the image.
    if (cfg.hwcomposer.has_value())
        a << QStringLiteral("androidboot.remora_hwc=%1").arg(*cfg.hwcomposer);
    // Vulkan ICD (bd remora-xqt1). The shim sets ro.hardware.vulkan from this directly, the way
    // the dpi stanza sets ro.sf.lcd_density — there is no pre-migration twin, because gpu_config.sh
    // always wrote the hardware property itself. Absent when the resolver could not name a driver,
    // so an unknown GPU shows up as a missing ICD rather than as a wrong one.
    if (cfg.vulkan.has_value())
        a << QStringLiteral("androidboot.remora_vulkan=%1").arg(*cfg.vulkan);
    // LIBVA_DRIVER_NAME for the c2-va HAL (its .rc reads ro.boot.va_driver, iHD when absent).
    // Absent unless the resolver mapped a non-Intel driver or the user overrode va_driver=
    // (bd remora-4ei.80) — same anti-stale-bake shape as gpu_node above.
    if (cfg.vaDriver.has_value())
        a << QStringLiteral("androidboot.va_driver=%1").arg(*cfg.vaDriver);
    // Where the composer should publish composed frames (bd remora-emoe phase 2). The CONTAINER's
    // view of the path bound above — the HAL reads ro.boot.remora_frame_socket and stays entirely
    // inert when it is absent, which is what keeps this off for every profile that is not running
    // compose mode. No shim stanza: the consumer is Remora's own HAL and it reads the remora_ name
    // natively, which is where the whole namespace migration has been heading.
    if (cfg.hostEncode && !cfg.hostEncodeFrameSocket.isEmpty())
        a << QStringLiteral("androidboot.remora_frame_socket=/dev/remora-frames/frame.sock");
    // Where c2-va's RemoteVideoDecoder should reach the host decode helper — the CONTAINER's view
    // of the directory bound above (bd remora-e5x.37). Absent unless host decode resolved, and the
    // decoder is entirely inert without it: RemoteVideoDecoder::configured() reads this property,
    // finds nothing, and every stream uses the local Intel VA decoder exactly as before. No shim
    // stanza — the consumer is Remora's own HAL and reads the remora_ name natively.
    if (cfg.hostDecode && !cfg.hostDecodeSocket.isEmpty())
        a << QStringLiteral("androidboot.remora_decoder_socket=/dev/remora-decode/decode.sock");
    a << QStringLiteral("androidboot.use_memfd=%1").arg(cfg.useMemfd ? "true" : "false");
    a << QStringLiteral("androidboot.remora_width=%1").arg(cfg.width)
      << QStringLiteral("androidboot.remora_height=%1").arg(cfg.height)
      << QStringLiteral("androidboot.remora_dpi=%1").arg(cfg.dpi);

    // R3 opt-in boot args — only when set, so default vectors are unchanged.
    if (cfg.fps.has_value())
        a << QStringLiteral("androidboot.remora_fps=%1").arg(*cfg.fps);
    if (cfg.useOverlayfs)
        a << "androidboot.use_remora_overlayfs=1";
    if (cfg.useCodec2)  // activate the codec2 pipeline (HW decode + HEVC encode) — needs dma_heap
        a << "androidboot.use_remora_c2=1";
    for (const QString &kv : cfg.roOverrides)
        a << QStringLiteral("androidboot.ro.%1").arg(kv);
    if (!cfg.dns.isEmpty()) {
        // ipconfigstore reads remora_net_dns1..ndns; the resolver caps the list at four, the most
        // Android's resolver queries.
        a << QStringLiteral("androidboot.remora_net_ndns=%1").arg(cfg.dns.size());
        int i = 1;
        for (const QString &server : cfg.dns)
            a << QStringLiteral("androidboot.remora_net_dns%1=%2").arg(i++).arg(server);
    }
    if (cfg.proxyType.has_value()) {
        a << QStringLiteral("androidboot.remora_net_proxy_type=%1").arg(*cfg.proxyType);
        if (cfg.proxyHost.has_value())
            a << QStringLiteral("androidboot.remora_net_proxy_host=%1").arg(*cfg.proxyHost);
        if (cfg.proxyPort.has_value())
            a << QStringLiteral("androidboot.remora_net_proxy_port=%1").arg(*cfg.proxyPort);
    }
    return a;
}

// Fold boot args to namespace-free key=value for the drift compare (bd remora-wlun): the
// androidboot. prefix goes and remora_ inside the KEY folds. Non-androidboot tokens drop: only
// boot args are under comparison.
QStringList foldBootArgs(const QStringList &args) {
    QStringList out;
    for (const QString &arg : args) {
        if (!arg.startsWith(QLatin1String("androidboot."))) continue;
        QString kv = arg.mid(12);
        QString key = kv.section(QLatin1Char('='), 0, 0);
        const QString value = kv.section(QLatin1Char('='), 1);
        // ONE NAMESPACE (bd remora-28ix.4 step 4): only remora_ folds. A container whose boot args
        // spell any other namespace was not baked by this builder, so it reads as different and
        // gets recreated rather than kept alive by a lenient compare.
        if (const int i = int(key.indexOf(QLatin1String("remora_"))); i >= 0) key.remove(i, 7);
        out << key + QLatin1Char('=') + value;
    }
    return out;
}

std::pair<QMap<QString, QString>, QStringList> buildMirrorArgv(const ResolvedConfig &cfg,
                                                               const QString &target,
                                                               bool fullscreenDisplay) {
    // The in-house client (remora mirror, bd remora-28ix.2) needs no environment: the SDL video
    // driver and Wayland-scale workarounds died with SDL, and the hwdec/server-path env pair
    // became flags. The env map stays in the signature for the spawn sites — mirrorSpawnEnv
    // still layers the Alt+D toggle variables on top of it.
    QMap<QString, QString> env;
    // A --new-display session is an ADDITIONAL window, not the mirror. The host encoder composes
    // and publishes the device's MAIN display and serves ONE consumer, so feeding that socket to a
    // new-display window shows the main display (the launcher) at the main display's size — and it
    // dies shortly after, fighting the mirror for the stream (bd remora-0xh). Such a window takes
    // its video from the device encoder like any non-host-encode profile, which also restores the
    // codec options the host path drops. PIP opts out for the same reason (buildPipMirrorArgv).
    const bool newDisplay = fullscreenDisplay && cfg.fullscreenSeparateDisplay;
    const bool hostEncoded = cfg.hostEncodeDrivesMirror && !newDisplay;
    // argv[0] is the contract; the spawn sites substitute the running binary's absolute path so
    // a dev build launches itself, not whatever PATH resolves first.
    QStringList a{"remora",
                  "mirror",
                  "-s",
                  target,
                  QStringLiteral("--max-size=%1").arg(cfg.maxSize),
                  QStringLiteral("--video-bit-rate=%1").arg(cfg.videoBitRate)};
    // No --server-path since the v1 purge (bd remora-28ix.5): the device half is the agent the
    // image bakes, and the client refuses plainly when an image predates the bake.
    // Decode on the desktop GPU — on unless hw_decode=false. auto tries CUDA first (decode on the
    // GPU the frames leave through — the cross-GPU lesson), then probes VAAPI nodes; software is
    // the fallback.
    if (cfg.hwDecode) a << QStringLiteral("--hwdec=auto");
    // Opt-in device audio, main mirror only: a PIP or app window with its own audio stream would
    // double-play everything the main window already plays.
    if (cfg.mirrorAudio) a << QStringLiteral("--audio");
    if (hostEncoded) {
        // The video comes from the HOST encoder, so none of the device-encoder options apply —
        // emitting --video-codec/--video-encoder here would configure a codec2 encoder that this
        // session never starts. The client reads the stream from this socket and tells the
        // agent to compose the display without encoding it (video_compose).
        a << QStringLiteral("--external-video-socket=%1").arg(cfg.hostEncodeVideoSocket);
    }
    // Opt-in: only emit --video-codec when non-default (h265/av1), so the default vector is stable.
    // NB: h265 needs an on-device HEVC encoder → deploy with the codec2 pipeline on (use_codec2).
    else if (!cfg.videoCodec.isEmpty() && cfg.videoCodec != QLatin1String("h264")) {
        a << QStringLiteral("--video-codec=%1").arg(cfg.videoCodec);
        // Pin the VAAPI HW HEVC encoder on the -hwc2 image. Auto-select for h265 probes the
        // vendor encoder, its capability-based configure intermittently fails, and it silently
        // falls back to the software c2.android.avc.encoder. Pinning skips that and uses the HW
        // VDENC path directly. Only h265 has a vaapi encoder; av1 keeps the server's selection.
        // FLIPPED to the remora spelling, the condition this comment used to set
        // having been checked rather than assumed (bd remora-28ix.4). Every image in the field was
        // verified to REGISTER c2.remora.vaapi.hevc.encoder at runtime — the deployed image, the
        // kept rollback, and both AMD hosts, all four on hwc2 tags so this branch applies to them.
        // Registration is not function, so it was also run: the component was created live with no
        // errors ("Created component [c2.remora.vaapi.hevc.encoder]"). Images carry BOTH spellings
        // while the dual registration lasts, so this is reversible by editing one string.
        if (cfg.videoCodec == QLatin1String("h265") && cfg.imageTag.contains(QLatin1String("hwc2")))
            a << QStringLiteral("--video-encoder=c2.remora.vaapi.hevc.encoder");
    }
    // The -hwc2 image ranks the vendor VAAPI AVC encoder first for h264, but that vendor AVC
    // entry cannot start (only the HEVC path is implemented) — pin plain h264 to the software
    // encoder so the default codec keeps working on the accelerated image.
    else if (cfg.imageTag.contains(QLatin1String("hwc2")))
        a << QStringLiteral("--video-encoder=c2.android.avc.encoder");
    // Encoder frame cap: forwarded to the c2-va component (vendor tuning), which drops at its
    // input queue (remora-rjh). Emitted only when resolved — unset at ≤60 fps.
    if (cfg.maxFps) a << QStringLiteral("--max-fps=%1").arg(*cfg.maxFps);
    // Mouse bindings: emitted only when they differ from what the client already runs, so the
    // default argv vector stays stable. The client refuses a malformed vector rather than falling
    // back, which is why the resolver's value travels verbatim instead of being normalised here.
    if (!cfg.mouseBind.isEmpty() && cfg.mouseBind != QLatin1String("bhsn:++++"))
        a << QStringLiteral("--mouse-bind=%1").arg(cfg.mouseBind);
    // The keyboard half of the same card. Both are emitted only when set: an unset shortcut_mod
    // means the mirror's own default (Alt or Super), which is what the client already runs.
    if (!cfg.keyBind.isEmpty()) a << QStringLiteral("--key-bind=%1").arg(cfg.keyBind);
    if (!cfg.shortcutMod.isEmpty()) a << QStringLiteral("--shortcut-mod=%1").arg(cfg.shortcutMod);
    // --audio (above) is the only audio flag: the client plays opus and has no codec or buffer
    // option to pass. No keyboard/mouse mode either — sdk-style input is built in.
    if (newDisplay) {
        // Desktop mode: a SEPARATE Android display at its own resolution. A fresh display is
        // blank, so launch the chosen app onto it.
        a << (cfg.fullscreenDisplay && !cfg.fullscreenDisplay->isEmpty()
                  ? QStringLiteral("--new-display=%1").arg(*cfg.fullscreenDisplay)
                  : QStringLiteral("--new-display=auto"));
        // Measured on the hwc2 image (bd remora-28ix.2): a new virtual display composes NOTHING
        // without flex_display — the server looks healthy while the encoder starves and the
        // client dies on its first-frame timeout. Every new-display session carries it.
        a << QStringLiteral("--server-param=flex_display=true");
        // Opens WINDOWED at the mirror's frame so it matches the windowed mirror; fullscreen is
        // one keypress away (Alt+F). No --fullscreen.
        if (cfg.windowWidth) a << QStringLiteral("--window-width=%1").arg(*cfg.windowWidth);
        if (cfg.windowHeight) a << QStringLiteral("--window-height=%1").arg(*cfg.windowHeight);
        if (cfg.fullscreenApp && !cfg.fullscreenApp->isEmpty())
            a << QStringLiteral("--start-app=%1").arg(*cfg.fullscreenApp);
    } else {
        // Mirror the main display. The Fullscreen action starts fullscreen; the windowed Connect
        // gets window geometry. The client always renders stretched, so a monitor whose aspect ≠
        // the device is filled with no bars — no --render-fit flag exists to emit.
        if (fullscreenDisplay) {
            a << QStringLiteral("--fullscreen");
        } else {
            // Window geometry (opt-in, keeps the default vector stable)
            if (cfg.windowWidth) a << QStringLiteral("--window-width=%1").arg(*cfg.windowWidth);
            if (cfg.windowHeight) a << QStringLiteral("--window-height=%1").arg(*cfg.windowHeight);
            if (cfg.windowX) a << QStringLiteral("--window-x=%1").arg(*cfg.windowX);
            if (cfg.windowY) a << QStringLiteral("--window-y=%1").arg(*cfg.windowY);
            if (cfg.windowFullscreen) a << QStringLiteral("--fullscreen");
        }
    }
    a << cfg.mirrorExtra;
    return {env, a};
}

QString imageArchivePath(const QString &outputDir, const QString &tag) {
    QString name = tag;
    name.replace(QLatin1Char(':'), QLatin1Char('-')).replace(QLatin1Char('/'), QLatin1Char('-'));
    return outputDir + QLatin1Char('/') + name + QStringLiteral(".tar");
}

// The node + driver the decode helper should use. ONE RULE, shared by the session argv and the
// --probe argv, because a probe that answers for a different device than the session opens is
// worse than no probe: it green-lights a configuration nobody tested. Measured on the NVIDIA host
// — `--probe --drm-node renderD128` alone exits 1 (libva defaults to iHD, which cannot drive that
// node) while the same probe WITH `--driver nvidia` exits 0. Splitting these would have gated host
// decode off on precisely the hardware it exists for.
static QStringList hostDecoderDeviceArgs(const ResolvedConfig &cfg) {
    QStringList a;
    // THE NVIDIA NODE WHEN THERE IS ONE — this is the one place in the system that WANTS the node
    // everything else deliberately avoids. venusNode IS that node (see Resolved.h: "the NVIDIA
    // render node"), and gpuNode is deliberately NOT a substitute: it stays the *xe* node, so
    // passing it would put decode back on Intel and cost exactly the same-GPU win this feature
    // exists for. Falling back to it anyway is still right when NVIDIA is absent — the helper's
    // --probe passes on the Intel node with no driver named — so a non-NVIDIA host decodes
    // remotely rather than refusing, it just gains less.
    // NOTE venusNode is only populated when venus=true. An NVIDIA host with venus off therefore
    // gets the xe node here; correct but pointless, and the reason gating should eventually ask
    // the capability probe directly rather than borrowing Venus's answer.
    const std::optional<QString> node = cfg.venusNode ? cfg.venusNode : cfg.gpuNode;
    if (node && !node->isEmpty()) a << QStringLiteral("--drm-node") << *node;
    // Only when the resolver actually named one: unset means libva picks, which is what makes the
    // Intel node work with no driver argument and the AMD hosts work through the same helper
    // without a second code path.
    if (cfg.vaDriver.has_value() && !cfg.vaDriver->isEmpty())
        a << QStringLiteral("--driver") << *cfg.vaDriver;
    return a;
}

QStringList buildHostDecoderProbeArgv(const ResolvedConfig &cfg) {
    return QStringList{QStringLiteral("remora-frame-decoder"), QStringLiteral("--probe")} +
           hostDecoderDeviceArgs(cfg);
}

QStringList buildHostDecoderArgv(const ResolvedConfig &cfg) {
    // The decoder LISTENS on one socket; c2-va's RemoteVideoDecoder connects per stream.
    QStringList a{QStringLiteral("remora-frame-decoder"), QStringLiteral("--decode-socket"),
                  cfg.hostDecodeSocket};
    a += hostDecoderDeviceArgs(cfg);
    // Same reasoning as the encoder's: reap an ABANDONED helper rather than let it hold GPU memory
    // forever. The timer only runs while no session is attached and resets when one arrives, so a
    // device that is actually playing video never idles out. Longer than a single stream's gap
    // between apps, short enough that a closed player does not pin NVDEC all afternoon.
    a << QStringLiteral("--idle-timeout") << QString::number(600);
    return a;
}

QStringList buildHostEncoderArgv(const ResolvedConfig &cfg) {
    // The encoder LISTENS on both sockets: the Venus render server connects to the frame socket
    // (one connection per render-server fork, since only the receiver can tell which one is
    // driving the screen), and the video consumer connects to the video socket.
    QStringList a{QStringLiteral("remora-frame-encoder"),
                  QStringLiteral("--frame-socket"),
                  cfg.hostEncodeFrameSocket,
                  QStringLiteral("--video-socket"),
                  cfg.hostEncodeVideoSocket,
                  // GENERIC names on purpose: the encoder resolves the fastest backend for the
                  // GPU it lands on (NVENC for hevc on NVIDIA — the Vulkan driver pins HEVC
                  // sessions to a slow preset, 54.8 vs 126.9 fps at 4K measured — Vulkan Video
                  // everywhere else). Naming a backend here would bake one host's answer into
                  // every host's argv (bd remora-e5x.18.1).
                  QStringLiteral("--codec"),
                  cfg.videoCodec == QLatin1String("h264") ? QStringLiteral("h264")
                                                          : QStringLiteral("hevc"),
                  // Reap an ABANDONED encoder instead of letting it hold GPU memory forever (bd
                  // remora-e5x.13: one was found alive after seven hours holding ~2.9 GB, which
                  // starved every later encoder into VK_ERROR_INITIALIZATION_FAILED and left the
                  // mirror unable to start at all). The window must clear doConnect's worst case —
                  // the encoder is started BEFORE the boot gate, and bootWaitMs is 240s for a cold
                  // first boot — so anything tighter would reap a healthy encoder mid-boot. An
                  // encoder actually in use never idles: the timer only runs while NO consumer is
                  // attached, and resets the moment one is.
                  QStringLiteral("--idle-timeout"),
                  QStringLiteral("600")};
    if (!cfg.videoBitRate.isEmpty()) {
        // The mirror spells bit rates "30M"; ffmpeg wants a plain integer.
        QString br = cfg.videoBitRate;
        qint64 mult = 1;
        if (br.endsWith(QLatin1Char('M'), Qt::CaseInsensitive)) {
            mult = 1000000;
            br.chop(1);
        } else if (br.endsWith(QLatin1Char('K'), Qt::CaseInsensitive)) {
            mult = 1000;
            br.chop(1);
        }
        bool ok = false;
        const qint64 v = br.toLongLong(&ok);
        if (ok && v > 0)
            a << QStringLiteral("--bit-rate") << QString::number(v * mult);
    }
    if (cfg.fps.has_value() && *cfg.fps > 0)
        a << QStringLiteral("--fps") << QString::number(*cfg.fps);
    // Sized up front on purpose: ffmpeg's Vulkan code crashes on an out-of-memory instead of
    // reporting it, so a pipeline that does not fit cannot be recovered, only avoided.
    if (cfg.hostEncodeMaxSize > 0)
        a << QStringLiteral("--max-size") << QString::number(cfg.hostEncodeMaxSize);
    // Rate control is NOT set here any more: it is backend-specific (CBR is free bits on
    // hevc_vulkan but costs NVENC ~30% throughput, both measured), and the encoder is what knows
    // which backend it resolved — its per-backend defaults carry the measured choices now.
    return a;
}

QStringList buildImageBuildArgv(const QString &tag, const QString &buildDir) {
    return {"docker", "build", "-t", tag, buildDir};
}

QStringList buildImageSaveArgv(const QString &tag, const QString &archivePath) {
    return {"docker", "save", "-o", archivePath, tag};
}

QStringList buildStorageAclArgv(const QString &mediaDir, uint uid, const QString &elevator) {
    // rwX (capital X) gives directories traverse without marking every plain file executable.
    // The d: half is a DEFAULT entry — what new children inherit — and setfacl silently skips it
    // for the files it walks, which is why one -R pass can carry both.
    const QString spec = QStringLiteral("u:%1:rwX,d:u:%1:rwx").arg(uid);
    // `--` so a data dir starting with '-' is a path and not a flag.
    return {elevator, QStringLiteral("setfacl"), QStringLiteral("-R"), QStringLiteral("-m"),
            spec,     QStringLiteral("--"),      mediaDir};
}

QStringList buildDeviceTuneArgv(const ResolvedConfig &cfg, const QString &target) {
    // multi_audio_focus_enabled: media apps keep playing when another starts (independent audio
    // across app windows). Always 1 and not configurable — every app-window layout Remora offers
    // depends on it, and the stock behaviour (one app silences the other) has no use here.
    // system_server reads it at boot, so this connect-time write is in force from the next
    // container (re)start onward.
    // The animation duration scales are deliberately NOT written here. Remora used to pin
    // animator 0 / window+transition 0.5 on every connect, which meant Developer options could
    // not hold a different value for more than one connect. Android's own 1/1/1 is the default
    // now, and changing it is the user's business (bd remora-4ei).
    // verifier_*: Play Protect's VerifyApps holds every adb install of an unknown APK in its
    // upload-consent flow for minutes — sideloads (remora install, mirror drag-drop) hang until
    // it times out. Off for a dev device; on-device Play integrity is unaffected.
    QString s = QStringLiteral("settings put system multi_audio_focus_enabled 1; "
                               "settings put global verifier_verify_adb_installs 0; "
                               "settings put global package_verifier_enable 0");
    // Select a WebView provider when WebViewUpdateService reports none. A17 boots with
    // "Current WebView package is null" while com.android.webview is installed, enabled and
    // reported VALID — so every WebView construction throws and any app that renders HTML dies on
    // inflate. Found as "Gmail keeps stopping" (InflateException on conversation_webview), but the
    // blast radius is every mail client, in-app browser, OAuth/consent screen and Custom Tabs
    // fallback on the device (bd remora-4ei.48).
    // ASSERTED HERE, on every connect, rather than once at first boot: the choice persists in
    // Settings.Global webview_provider, which lives in /data — so it survives on this /data and a
    // fresh /data (or a new instance) starts broken again, which is the actual defect.
    // The provider is READ from dumpsys, not hardcoded: the valid package is com.android.webview
    // on the AOSP-derived images and a Trichrome package where GApps supplies one, and naming the
    // wrong one is a no-op that would leave the device just as broken but now silently "fixed".
    // Guarded on the null line so a device that already has a provider costs one dumpsys and
    // changes nothing — re-selecting restarts relros for every running WebView process.
    s += QStringLiteral(
        "; dumpsys webviewupdate 2>/dev/null | grep -q 'Current WebView package is null' && "
        "{ w=$(dumpsys webviewupdate 2>/dev/null | grep -m1 -oE 'Valid package [a-zA-Z0-9._]+' | "
        "cut -d' ' -f3); [ -n \"$w\" ] && cmd webviewupdate set-webview-implementation \"$w\"; }; "
        "true");
    // Guest timezone (bd remora-c8t): the guest otherwise stays on whatever the image ships (UTC),
    // so timestamps, alarms and app content disagree with the desktop the mirror sits on.
    // persist.sys.timezone lives in /data, so one write sticks across container restarts; guarded
    // on the current value because a CHANGE fans out as ACTION_TIMEZONE_CHANGED to every app,
    // which an identical rewrite must not trigger on each connect. Validated to IANA shape here —
    // this string lands inside a quoted shell word, and a zone that fails the pattern is dropped
    // rather than quoted harder.
    static const QRegularExpression kIanaZone(QStringLiteral("^[A-Za-z0-9_+/-]{1,64}$"));
    if (!cfg.timezone.isEmpty() && kIanaZone.match(cfg.timezone).hasMatch())
        s += QStringLiteral("; [ \"$(getprop persist.sys.timezone)\" = '%1' ] || "
                            "setprop persist.sys.timezone '%1'")
                 .arg(cfg.timezone);
    // fps > 60: the boot arg only unlocks the 120Hz display MODE — SF's render rate stays
    // clamped by the settings-driven policy (render: (0.0 60.0)) until peak/min_refresh_rate
    // are raised too. Both pinned to fps: a VM has no battery to save, and a fixed rate keeps
    // mirror latency consistent. Settings live in /data, so re-assert on every connect; the
    // encoder is protected by the resolver's --max-fps auto-cap (remora-rjh/mzw.1).
    if (cfg.fps && *cfg.fps > 60)
        s += QStringLiteral("; settings put system peak_refresh_rate %1; "
                            "settings put system min_refresh_rate %1")
                 .arg(*cfg.fps);
    // Companion apps: grant WRITE_SECURE_SETTINGS so a tweaks app can actually write the settings
    // it exposes. The package id is NOT in the config — an entry names a directory — so resolve it
    // on-device from the codePath that `pm list packages -f` reports, which is exactly the path we
    // bind-mounted. Failure is expected and ignored for the common case: a platform-signed app
    // already holds the permission by signature and pm grant refuses it as "not a changeable
    // permission type". Appended only when apps are configured, so the default tune vector is
    // untouched.
    for (const QString &app : cfg.companionApps) {
        QString name = app.section(QLatin1Char(':'), 1, 1);
        if (name.isEmpty())
            name = app.section(QLatin1Char(':'), 0, 0)
                       .section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
        s += QStringLiteral("; p=$(pm list packages -f 2>/dev/null | "
                            "sed -n 's|^package:/system/system_ext/app/%1/.*=||p' | head -1); "
                            "[ -n \"$p\" ] && pm grant \"$p\" "
                            "android.permission.WRITE_SECURE_SETTINGS 2>/dev/null; true")
                 .arg(name);
    }
    return {"adb", "-s", target, "shell", s};
}


std::optional<KbdIdentity> selectKbdIdentity(const QList<KbdIdentity> &keyboards,
                                             const std::optional<QString> &selector) {
    const QString want = selector.value_or(QString()).trimmed();
    if (want.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) return std::nullopt;
    if (keyboards.isEmpty()) return std::nullopt;
    // AUTO must not pick a synthetic device. This host reports three "keyboards": a real USB
    // receiver, the operator's Bluetooth keyboard, and Sunshine's own uinput passthrough —
    // impersonating that last one would be circular, and it is what "first" happened to reach on
    // another machine. Raw uinput devices carry no phys and live under /devices/virtual/input;
    // Bluetooth HID also lives under /devices/virtual but via uhid, and always has a phys.
    const auto synthetic = [](const KbdIdentity &k) {
        return k.phys.isEmpty() || k.phys == QLatin1String("py-evdev-uinput")
               || k.sysfs.startsWith(QLatin1String("/devices/virtual/input"));
    };
    if (want.isEmpty()) {
        for (const KbdIdentity &k : keyboards)
            if (!synthetic(k)) return k;
        return std::nullopt;  // only synthetic devices: better none than a self-impersonation
    }
    for (const KbdIdentity &k : keyboards)
        if (k.name == want) return k;
    for (const KbdIdentity &k : keyboards)
        if (k.name.contains(want, Qt::CaseInsensitive)) return k;
    return std::nullopt;
}

QStringList kbdIdentityEnv(const KbdIdentity &k) {
    const auto hex4 = [](unsigned v) { return QStringLiteral("0x%1").arg(v, 4, 16, QLatin1Char('0')); };
    return {QStringLiteral("REMORA_KBD_NAME=%1").arg(k.name),
            QStringLiteral("REMORA_KBD_BUS=%1").arg(hex4(k.bus)),
            QStringLiteral("REMORA_KBD_VENDOR=%1").arg(hex4(k.vendor)),
            QStringLiteral("REMORA_KBD_PRODUCT=%1").arg(hex4(k.product)),
            QStringLiteral("REMORA_KBD_VERSION=%1").arg(hex4(k.version)),
            QStringLiteral("REMORA_KBD_PHYS=%1").arg(k.phys)};
}


QString pipWindowTitle(const QString &instance) {
    return instance.isEmpty() ? QStringLiteral("Remora PIP")
                              : QStringLiteral("Remora PIP — %1").arg(instance);
}

std::pair<QMap<QString, QString>, QStringList> buildPipMirrorArgv(const ResolvedConfig &cfg,
                                                                  const QString &target,
                                                                  const QString &instance) {
    // PIP takes its video from the DEVICE even when the main mirror is host-encoded: the encoder
    // serves one consumer, and a second client on the same socket would either be refused or
    // steal the stream. Building it from a copy with the host path off also restores the codec
    // options the host path deliberately drops.
    ResolvedConfig pipCfg = cfg;
    pipCfg.hostEncodeDrivesMirror = false;
    // No audio on the PIP: the main window already plays it, and a second stream would double it.
    pipCfg.mirrorAudio = false;
    auto [env, argv] = buildMirrorArgv(pipCfg, target, false);

    // Strip what PIP overrides or must not inherit. Prefix matching, because these options carry
    // values: dropping "--window-width" alone would leave "--window-width=1880" behind.
    static const QStringList drop = {QStringLiteral("--window-"), QStringLiteral("--fullscreen"),
                                     QStringLiteral("--new-display"),
                                     QStringLiteral("--server-param"),
                                     QStringLiteral("--start-app"),
                                     QStringLiteral("--video-bit-rate"), QStringLiteral("--max-size")};
    QStringList out;
    for (const QString &a : argv) {
        bool skip = false;
        for (const QString &d : drop)
            if (a.startsWith(d)) { skip = true; break; }
        if (!skip) out << a;
    }

    // Per-profile title. The app_id below stays constant so ONE Krohnkite/KWin class rule covers
    // every profile's PIP, but each profile's menu entry needs its own toggle target — with a
    // shared title, opening profile B's PIP while A's is up would just close A's and open nothing.
    out << QStringLiteral("--window-title=%1").arg(pipWindowTitle(instance))
        << QStringLiteral("--window-width=%1").arg(cfg.pipWidth)
        << QStringLiteral("--window-height=%1").arg(cfg.pipHeight)
        << QStringLiteral("--always-on-top")
        // --video-codec / --video-encoder are deliberately NOT stripped: the PIP mirrors whatever
        // the profile selected, so an h265 profile gets an h265 PIP on the same hardware encoder.
        //
        // This once forced h264 on the belief that the hardware HEVC encoder is single-instance.
        // It is not — measured on a running A16 instance, a second hw HEVC session alongside the
        // full-res mirror initialises and runs: the component is created, "vaInitialize() OK on
        // /dev/dri/renderD129", "VaapiVideoEncoder ready: coded=960x512".
        // Two VAAPI HEVC sessions plus a software one ran concurrently on the VF. Keep the caps
        // below and a second session stays cheap whichever codec it uses.
        //
        // A 480x270 always-on-top window does not need the mirror's 60M or its full 3760x1992, and
        // the cap is what keeps the second session small enough to be free. Note the software HEVC
        // fallback (c2.android.hevc.encoder) is hard-capped at 512x512, so if the VF ever goes away
        // an h265 PIP at max-size 960 fails rather than falling back — the mirror fails with it, so
        // this adds no failure mode the profile did not already have.
        << QStringLiteral("--video-bit-rate=6M")
        << QStringLiteral("--max-size=960")
        // The unique Wayland app_id / X11 class. Without this the PIP and the main mirror share
        // a class, and the one KWin/Krohnkite rule that floats every profile's PIP cannot bind.
        << QStringLiteral("--app-id=remora-pip");
    return {env, out};
}

QString buildCpufreqTopologyCmd(const QString &outDir) {
    // REBUILT FROM SCRATCH EVERY DEPLOY. The tree describes the host's clusters, and a host can
    // gain or lose them — a BIOS E-core toggle, a different machine behind backend=remote, a
    // governor change that moves cpuinfo_max_freq. A stale directory would keep describing the
    // previous answer with nothing to say it had changed, which is the stale-bake this project
    // refuses everywhere else (the GPU node, the venus build.prop).
    //
    // WRITTEN IN PLACE, KEEPING THE DIRECTORY'S INODE. A bind mount resolves to the inode, not the
    // path, so any scheme that REPLACES the directory (write a sibling, mv it over) leaves a
    // running container bound to the directory that was moved aside — and removing that one then
    // empties the container's view to ZERO policies while it runs. Zero is worse than 24: an app
    // that takes cluster[0] or divides by the cluster count gets a fresh failure invented by the
    // fix. Caught during the first live deploy of this feature, where host-prereqs regenerates
    // while the previous container is still up.
    //
    // So: create/overwrite the wanted policies first, then delete only the ones that are no longer
    // wanted. The count never dips below the new set, and a reader between the two halves sees a
    // superset — stale entries, never an empty directory.
    //
    // FAILS LOUDLY AND LEAVES NOTHING HALF-BUILT: the caller treats a non-zero exit as "do not
    // mount", which falls back to the host's real topology — the behaviour before this existed.
    // A partially written tree bound into a container would be worse than no fix at all.
    return QStringLiteral(
               "set -e; "
               "src=/sys/devices/system/cpu/cpufreq; "
               "[ -d \"$src\" ] || { echo 'no cpufreq on this host' >&2; exit 1; }; "
               "out=%1; tmp=$out; keep=; "
               "mkdir -p \"$out\"; "
               // One pass per DISTINCT cpuinfo_max_freq: that is the cluster key. Policies whose
               // max frequency is unreadable are skipped rather than defaulted — a policy claiming
               // 0 kHz would sort as the smallest cluster and could be picked as "little".
               "freqs=$(for p in \"$src\"/policy*; do "
               "  [ -r \"$p/cpuinfo_max_freq\" ] && cat \"$p/cpuinfo_max_freq\"; "
               "done | sort -run); "
               // NEVER FEWER POLICIES THAN THE HOST HAS. If no policy exposes a readable maximum
               // frequency there is nothing to cluster BY, and emitting an empty tree would show
               // the guest zero clusters — a state no phone presents and a fresh way to break an
               // app that takes cluster[0] or divides by the count. Copying the host's own
               // policies verbatim is the honest fallback: identical to mounting nothing, so the
               // worst case of this feature is exactly the behaviour before it existed.
               "[ -n \"$freqs\" ] || { echo 'cpufreq policies expose no readable max frequency — "
               "passing the host topology through unchanged' >&2; "
               "cp -a \"$src\"/policy* \"$tmp\"/ 2>/dev/null || true; }; "
               "for f in $freqs; do "
               "  cpus=; lo=; min=; "
               "  for p in \"$src\"/policy*; do "
               "    [ -r \"$p/cpuinfo_max_freq\" ] || continue; "
               "    [ \"$(cat \"$p/cpuinfo_max_freq\")\" = \"$f\" ] || continue; "
               // related_cpus is the authoritative membership; affected_cpus omits offline CPUs.
               "    for c in $(cat \"$p/related_cpus\" 2>/dev/null); do "
               "      cpus=\"$cpus $c\"; "
               "      [ -z \"$lo\" ] && lo=$c; "
               "      [ \"$c\" -lt \"$lo\" ] && lo=$c; "
               "    done; "
               "    [ -z \"$min\" ] && min=$(cat \"$p/cpuinfo_min_freq\" 2>/dev/null); "
               "  done; "
               "  [ -n \"$lo\" ] || continue; "
               "  d=\"$tmp/policy$lo\"; mkdir -p \"$d\"; "
               "  cpus=$(echo $cpus | tr ' ' '\\n' | sort -n | uniq | tr '\\n' ' '); "
               "  cpus=${cpus%% }; "
               "  echo \"$f\" > \"$d/cpuinfo_max_freq\"; "
               "  echo \"${min:-0}\" > \"$d/cpuinfo_min_freq\"; "
               // scaling_* mirror the cpuinfo_* pair: an app that reads either to rank clusters
               // must get the same ranking, and inside the container these are read-only anyway.
               "  echo \"$f\" > \"$d/scaling_max_freq\"; "
               "  echo \"${min:-0}\" > \"$d/scaling_min_freq\"; "
               "  echo \"$cpus\" > \"$d/related_cpus\"; "
               "  echo \"$cpus\" > \"$d/affected_cpus\"; "
               "  keep=\"$keep policy$lo\"; "
               "done; "
               // Only now, once every wanted policy exists, drop the ones that no longer belong —
               // a host that lost a cluster (a BIOS E-core toggle, a different remote machine)
               // must not keep serving a stale one. Deleting last is what keeps the directory from
               // ever being empty for a container reading it mid-regeneration.
               "for d in \"$out\"/policy*; do "
               "  [ -d \"$d\" ] || continue; "
               "  b=$(basename \"$d\"); found=; "
               "  for k in $keep; do [ \"$k\" = \"$b\" ] && found=1; done; "
               "  [ -n \"$found\" ] || rm -rf \"$d\"; "
               "done")
        .arg(outDir);
}

}  // namespace remora
