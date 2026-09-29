#!/system/bin/sh
# Remora GPU bring-up (bd remora-28ix.4 R2). Runs at early-boot, BEFORE SurfaceFlinger and the
# HALs, and decides three things from the boot args: which render node the guest draws on, which
# gralloc allocates its buffers, and which EGL driver backs it. NOT the Vulkan ICD any more — the
# host resolves that and sends it as a boot arg (bd remora-xqt1); see the note above
# setup_render_node for why it cannot be worked out from in here.
#
# Reads Remora's OWN boot namespace (ro.boot.remora_*) directly, which is now the only namespace
# there is: the adapter that mapped those keys onto pre-migration names was reduced to two
# unrelated setprops once its last consumer left the image (bd remora-28ix.4 step 4).

# THE VULKAN ICD IS NOT DECIDED HERE, and a setup_vulkan() that tried to was removed with
# bd remora-xqt1. It mapped the render node's DRM driver onto an ICD name, and it had never once
# run: it read the driver from /sys/kernel/debug/dri, and debugfs is NOT mounted in a Remora
# container, so the lookup produced an empty string and its `*)` arm set nothing on every host
# since it was written. Repairing it against /sys/class/drm/<node>/device/driver, which IS
# readable here, was rejected deliberately — on a Venus host the selected node is the INTEL one
# (that is where gralloc allocates) while rendering leaves for the NVIDIA card over vtest, so the
# node's own driver names the WRONG ICD, and no guest-side lookup can know better. The host
# resolves it and sends androidboot.remora_vulkan, which remora.props.rc maps onto
# ro.hardware.vulkan at early-init — before this script runs, and ro.* is write-once, so anything
# set here would be a dead second opinion even when it happened to agree.
# gpu_setup_guest still sets pastel directly: that is a constant for the software path, not a
# lookup, and it is a no-op whenever the host supplied the property first.

setup_render_node() {
    node=$(getprop ro.boot.remora_gpu_node)
    if [ -n "$node" ]; then
        echo "force render node: $node"

        setprop gralloc.gbm.device "$node"
        chmod 666 "$node"

        return 0
    fi

    # No node named: scan. THE SCAN IS A FALLBACK, not the normal path — Remora resolves the node
    # host-side from a live probe and passes it explicitly, precisely because this scan cannot pick
    # an Intel xe node (upstream's case list predates xe, so an xe box reported "NO qualified
    # render node found" and fell back to SwiftShader with no explanation). xe is listed here now,
    # but the host still decides on a multi-GPU machine — it is the only side that can (the guest
    # sees passed-through nodes, not the whole picture).
    # AND IN A REMORA CONTAINER THE SCAN CANNOT RUN AT ALL: /sys/kernel/debug/dri does not exist,
    # because debugfs is not mounted and nothing in buildDockerRunArgv mounts it. So this always
    # takes the failure branch, which is survivable only because the host names the node — the very
    # arrangement the paragraph above describes. Left inert rather than repaired against
    # /sys/class/drm (which IS readable here) deliberately: a scan that WORKED would pick a node by
    # driver name, and on a multi-GPU host that is precisely the wrong chooser.
    #
    # `|| return 1`, NOT `|| exit`. A failed cd used to kill the whole script — gpu_setup_host and
    # gpu_setup_guest never ran and the container came up with ro.hardware.egl, .gralloc and
    # .hwcomposer all unset, with no message saying why. Returning 1 is what the caller already
    # expects for "no qualified node": auto mode falls through to guest, host mode reports the gap
    # (bd remora-xqt1).
    cd /sys/kernel/debug/dri || return 1
    for d in * ; do
        if [ "$d" -ge "128" ]; then
            driver="$(cut -d' ' -f1 "$d/name")"
            echo "DRI node exists, driver: $driver"
            case $driver in
                i915|xe|amdgpu|nouveau|virtio_gpu|v3d|vc4|msm_drm|panfrost)
                    node="/dev/dri/renderD$d"
                    echo "use render node: $node"
                    setprop gralloc.gbm.device "$node"
                    chmod 666 "$node"
                    return 0
                    ;;
            esac
        fi
    done

    echo "NO qualified render node found"
    return 1
}

gpu_setup_host() {
    echo "use GPU host mode"

    setprop ro.hardware.egl mesa
    # Honour the gralloc the host chose. Default gbm; the host names minigbm_intel for an Intel xe
    # node, whose buffers gbm_gralloc mis-allocates (dequeueBuffer -19, then SIGFPE in
    # gralloc_gbm_bo_create), and minigbm_gbm_mesa for the Venus/NVIDIA path.
    #
    # cros IS TRANSLATED, NOT HONOURED, when this image cannot serve it — and the translation
    # lives HERE, in the image, on purpose (bd remora-ykhz.1): a mesa_source image's Mesa 26
    # removed the __driDriverGetExtensions interface minigbm's amdgpu backend hard-requires
    # (dri_init failure is -ENODEV, not a fallback), so gralloc.cros cannot initialise on any GPU
    # there. minigbm_gbm_mesa allocating through the baked local-gbm wrapper is the measured
    # replacement (RX 580: boot, radeonsi GL, RADV, VAAPI encode under motion, zero fatal
    # signals). Because the image translates, every existing AMD profile and the resolver's
    # amd->cros default keep working unchanged and the switch is atomic with image adoption.
    #
    # THE CONDITION IS THE PAYLOAD FILES, not a version string: the wrapper and the gbm backend
    # ship exactly when the Mesa-26 GL cluster does (mesa.mk's completeness gate), so their
    # presence is a precise runtime proxy for "cros is dead here". On an image built WITHOUT
    # mesa_source both are absent, the branch does not fire, and real cros is honoured — this
    # script ships in every A17 image and must be correct in both.
    #
    # gbm IS TRANSLATED TOO, for a different reason and with the same key (bd remora-ykhz). cros is
    # translated because it CANNOT WORK here; gbm is translated because it does not need to exist
    # here. gralloc.gbm is the last prebuilt holding libc++_shared (drv_video retired,
    # vulkan.virtio is out of scope), and minigbm_gbm_mesa serves the same cases: this default is
    # only ever reached from gpu_setup_host, and gpu_setup dispatches here only with a render node
    # — guest mode sets ro.hardware.gralloc=remora further down and never reaches this line. Both
    # libraries need a node anyway (gralloc.gbm links libgbm and opens /dev/dri/renderD128), so
    # nothing that gbm could serve is lost.
    #
    # NOTE THE ASYMMETRY, deliberately: an image WITHOUT the payload keeps real gbm, because
    # minigbm_gbm_mesa allocates through libgbm_mesa_wrapper.so and gbm/dri_gbm.so and cannot work
    # without them. That is the same reason the condition is the payload files rather than a
    # version string, and it is why gralloc.gbm still ships on non-mesa_source images.
    remora_gralloc="$(getprop ro.boot.remora_gralloc gbm)"
    if [ -f /vendor/lib64/libgbm_mesa_wrapper.so ] \
        && [ -f /vendor/lib64/gbm/dri_gbm.so ]; then
        case "$remora_gralloc" in
        cros)
            echo "gralloc cros is not servable on this image (Mesa 26 dropped minigbm's DRI interface); using minigbm_gbm_mesa"
            remora_gralloc=minigbm_gbm_mesa
            ;;
        gbm)
            echo "gralloc gbm is retired on this image (last libc++_shared holder); using minigbm_gbm_mesa"
            remora_gralloc=minigbm_gbm_mesa
            ;;
        esac
    fi
    setprop ro.hardware.gralloc "$remora_gralloc"
    # hw_get_module resolves the composer via ro.hardware.hwcomposer or ro.hardware; a
    # LineageOS-init container leaves ro.hardware "unknown", so name it explicitly or
    # SurfaceFlinger falls back to the host framebuffer (/dev/fb0, host video gid) and
    # crash-loops. Naming it here is what keeps this script the single decision point, and it
    # stays explicit even though ro.hardware is now "remora" and would resolve our own HAL.
    #
    # R4 (bd remora-emoe) adds hwcomposer.remora.so BESIDE the prebuilt, and this is the switch
    # between them. THE DEFAULT IS NOW REMORA'S OWN HAL — flipped once phase 1 had the evidence the
    # bead asked for, not on the strength of it booting: 16h15m of continuous uptime with the same
    # SurfaceFlinger and composer pids it started with, vsync steady at 60.00 Hz, wake/sleep and
    # sustained load both exercised, and the fd question settled twice over (the composer's dmabuf
    # count is a bounded live set of 12 buffers at 3 fds each, and it read 49 fds at 2h and 49 at
    # 16h — flat). It also survived a rebuild onto a fresh image rather than resting on one lucky
    # container.
    # THERE IS NO LONGER A ROLLBACK COMPOSER, and this note used to say there was. The closed
    # prebuilt left the image with bd remora-28ix.4.3, so the cheap restart-only fallback it
    # described — naming the old composer in androidboot.remora_hwc — now resolves a library that
    # is not installed, and hw_get_module fails with no composer at all rather than degrading.
    # A profile still pinning that value must be repointed at `remora` or left unset.
    # CAVEAT WORTH KEEPING: the HAL has only ever run on this NVIDIA/Venus host, and never against
    # cros gralloc plus the gallium GLES path. That combination remains unproven, but it can no
    # longer be hedged by selecting the old binary — there is nothing left to select.
    setprop ro.hardware.hwcomposer "$(getprop ro.boot.remora_hwc remora)"
    # THE 30 fps FLOOR IS GONE. It set a pre-migration property that only the closed
    # composer prebuilt read, as a floor for when the host named no rate — that binary left the
    # image with bd remora-28ix.4.3, and Remora's own HAL reads ro.boot.remora_fps and applies its
    # own 30 default when the property is absent. Setting it here now writes a name nothing reads.
    # Remora always passes a rate anyway, so this was a no-op on every real deploy.
}

gpu_setup_guest() {
    echo "use GPU guest mode"

    VENDOR_EGL_DIR=/vendor/lib64/egl
    SYSTEM_EGL_DIR=/system/lib64
    EGL_ANGLE=libEGL_angle.so
    EGL_SS=libEGL_swiftshader.so
    egl=

    if [ -f $VENDOR_EGL_DIR/$EGL_ANGLE ] || [ -f $SYSTEM_EGL_DIR/$EGL_ANGLE ]; then
        egl=angle
    elif [ -f $VENDOR_EGL_DIR/$EGL_SS ] || [ -f $SYSTEM_EGL_DIR/$EGL_SS ]; then
        egl=swiftshader
    else
        echo "ERROR no SW egl found!!!"
    fi

    setprop ro.hardware.egl $egl
    # gralloc.remora is Remora's own software allocator (R2b), verified on a shipped image: a
    # guest container boots unaided on it with zero RenderThread crashes (bd remora-28ix.4.1).
    # The composer follows ro.boot.remora_hwc and now DEFAULTS to Remora's own HAL, same as host
    # mode — see there for the evidence and for the rollback.
    setprop ro.hardware.gralloc remora
    setprop ro.hardware.hwcomposer "$(getprop ro.boot.remora_hwc remora)"
    setprop ro.hardware.vulkan pastel
}

gpu_setup() {
    ## mode=(auto, host, guest)
    ## node=(/dev/dri/renderDxxx)

    mode=$(getprop ro.boot.remora_gpu_mode guest)
    if [ "$mode" = "host" ]; then
        setup_render_node
        gpu_setup_host
    elif [ "$mode" = "guest" ]; then
        gpu_setup_guest
    elif [ "$mode" = "auto" ]; then
         echo "use GPU auto mode"
         if setup_render_node; then
            gpu_setup_host
         else
            gpu_setup_guest
         fi
    else
        echo "unknown mode: $mode"
    fi
}

gpu_setup
