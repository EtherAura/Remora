#include "core/SourcePatches.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>

#include "core/Features.h"  // androidRelease() — the per-version registry (bd remora-82c.2)

namespace remora {
namespace {

// Wrap a step so it runs only while the project's mark file is ABSENT ("absent = needs work", the
// same polarity as the global skip mark). A BRACE GROUP, NOT A SUBSHELL: the patch steps set $d in
// one step and dereference it in the next, and ( ) put those assignments in a subshell, so $d came
// back empty and every patch resolved to "/0005-….patch" — "can't open patch". The trailing ';' is
// stripped first, because "cmd; ; }" is a syntax error.
QString guarded(const QMap<QString, QString> &projectMarks, const QString &projectPath,
                const QString &body) {
    const QString mark = projectMarks.value(projectPath);
    if (mark.isEmpty()) return body;
    QString b = body.trimmed();
    while (b.endsWith(QLatin1Char(';'))) b.chop(1);
    return QStringLiteral("[ -f %1 ] || { %2 ; }").arg(mark, b.trimmed());
}

}  // namespace

const QVector<SourcePatch> &sourcePatches() {
    static const QVector<SourcePatch> v = {
        // NO hw_encoder ENTRY, DELIBERATELY (bd remora-4ei.42). c2-va used to have two sources of
        // truth, and they had silently diverged: a patch series under vendor/source-patches/, and
        // vendor/source-build/c2-va/, the live component that stage-features.sh `rm -rf`s the tree
        // copy and replaces with. The staged copy always won — it runs after the patch apply — so
        // the series was never what built, and it drifted: measured, reconstructing the
        // registry's 14 patches into a fresh repo and diffing against the vendored tree gave 290
        // differing lines plus an ENTIRE 1449-line vaapi/VaapiVideoDecoderHEVC.cpp present only in
        // the vendored copy. A 15th patch had also appeared on disk that this list never named, so
        // it applied nowhere. Nothing could have caught the drift, either: hardware/remora/c2-va is
        // in NO repo manifest (verified against .repo/projects/), so neither the global `repo sync
        // --force-sync` nor the targeted one resets it, and the apply loop skips any patch whose
        // subject slug is already in `git log --format=%f`. So the vendored tree is now the ONLY
        // source, staged unconditionally, and the old series was removed: it had drifted, nothing
        // applied it, and in the apply path it could only ever build something older than what
        // ships. Content is still change-detected: imageBuildFingerprint walks vendor/source-build
        // and content-hashes every file under 1 MiB, which covers all of c2-va.
        {"minigbm_rendernode", "minigbm forced render node",
         "cros_gralloc: honor ro.boot.remora_gpu_node + fix the gralloc0 alloc path (VF gralloc). "
         "Plus the VDENC direct-RGB allocation contract: encoder RGB32 BOs at 64-aligned height "
         "with tail slack, and the encoder-usage combos that keep cros_gralloc from stripping the "
         "flag (bd remora-ftt).",
         "external/minigbm", "external-minigbm",
         {"0001-cros_gralloc-honor-forced-render-node-fix-gralloc0-a.patch",
          "0002-xe-64-aligned-height-tail-slack-for-VDENC-direct-RGB.patch",
          "0003-xe-advertise-HW_VIDEO_ENCODER-on-linear-RGB32-combos.patch"}, true, true, "gpu"},
        // OPT-IN, and deliberately not part of the golden stack: the gralloc it builds only
        // allocates once a Venus vtest render server is listening on the host, so enabling it by
        // default would hand every image a gralloc that fails to allocate. Pair it with
        // ro.hardware.vulkan=virtio + mesa.vn.debug=vtest + mesa.vtest.socket.name and the host's
        // Venus vtest render server (bd remora-4ei.56).
        {"minigbm_gbm_mesa_vtest", "minigbm gbm_mesa/vtest gralloc (NVIDIA)",
         "Adds the external-allocator gralloc (gralloc.minigbm_gbm_mesa): allocation is "
         "delegated over a socket to "
         "libgbm_mesa_wrapper.so instead of a local DRM device, so buffers can be created HOST-side. "
         "That is what makes NVIDIA rendering correct — with the stock Intel gralloc, Venus writes "
         "NVIDIA block-linear into an Intel-tiled dmabuf and the screen is shredded while the "
         "geometry stays right. Carries the two core hooks it needs: DRV_EXTERNAL (fixed backend, "
         "no DRM probe) and bo_get_map_stride (host layout, linear mapping), both confined to this "
         "variant's own cflags. Needs a running Venus vtest server; useless without one.",
         "external/minigbm", "external-minigbm-gbm-mesa",
         {"0001-gbm_mesa-vtest-external-allocator-gralloc.patch",
          // Ports the xe backend's VDENC direct-RGB contract (bd remora-ftt) so the NVIDIA path
          // gets the same zero-copy encode: without it every frame takes a VPP copy.
          "0002-gbm_mesa-64-aligned-height-tail-slack-for-VDENC-direct-RGB.patch",
          // Surface-bound c2-va sessions decode into RGBX so SF never GL-imports an NV12 dma-buf
          // (fatal on Venus/NVIDIA — remora-e5x.21); without decoder-usage combos on RGB32 the
          // RGBX output pool cannot allocate and playback stalls frameless.
          "0003-gbm_mesa-advertise-HW_VIDEO_DECODER-on-linear-RGB32-combos.patch",
          // 0003 declares those combos linear, but only to minigbm's own bookkeeping — the
          // external allocator never sees it, and served codec buffers NVIDIA block-linear,
          // which iHD cannot import (remora-cq3: vaCreateSurfaces fails, the decoder errors on
          // its first frame, the app restarts playback once per segment and no video ever
          // appears). Carries the constraint over the wire. PAIRS WITH venus-nvidia 0023 and
          // guest-patches/0001 — struct alloc_args is the ABI between this gralloc and
          // libgbm_mesa_wrapper.so, so both must be rebuilt and shipped together.
          "0004-gbm_mesa-tell-the-external-allocator-codec-buffers-must-be-linear.patch",
          // A composited SCANOUT bo is necessarily block-linear (NVIDIA refuses RENDERABLE on
          // a linear modifier), so SF's screenshot path CPU-read tiled bytes as linear —
          // screencap and Recents thumbnails were striped noise (remora-c1e). A SW_READ lock
          // of a block-linear bo now fetches a GPU-detiled copy through the vtest server;
          // PAIRS WITH venus-nvidia 0024 (VCMD_RESOURCE_READ_GPU), and falls back to the raw
          // tiled map when the server lacks the command. No alloc_args ABI change.
          "0005-gbm_mesa-shadow-read-block-linear-bos-through-the-vtest-detile.patch",
          // The camera is the third device whose buffers cannot be block-linear, and the only
          // one whose producer is the CPU itself: AOSP's external camera HAL ORs CPU_WRITE_OFTEN
          // into every stream's usage, so force_linear is already true — but a preview buffer is
          // composited too, and the allocator ranks linear_required > SCANOUT > MAPPABLE, so
          // scanout won and the HAL wrote linear rows into a tiled bo. Green frames, because a
          // zeroed YUV buffer is green (bd remora-4ei.9).
          "0006-gbm_mesa-camera-buffers-are-CPU-written-so-they-must-be-linear.patch",
          // NVIDIA maps dedicated YCbCr dmabuf imports at 64 KiB granularity, so a chroma
          // plane at minigbm's tight offset samples as zeros — the green LIVE preview even
          // after 0006 made the bytes linear and correct (bd remora-e5x.35). Re-stack every
          // plane after the first onto a 64 KiB boundary; consumers all walk the per-plane
          // offsets this metadata carries. PAIRS WITH venus-nvidia 0026, which allocates the
          // blob via GBM-on-nvidia-drm (sysmem re-imports are corrupt regardless of offsets)
          // and admits dedicated YCbCr imports exactly when the offsets are aligned.
          "0007-gbm_mesa-64K-align-YUV-plane-offsets-for-NVIDIA-import.patch",
          // DIAGNOSTIC, not a fix (bd remora-e5x.36): one line per allocation naming the fourcc,
          // the usage bits, the route taken (native gbm vs the spoofed-R8 path) and the shape
          // requested from the external allocator. It exists to settle whether c2-va's NV12 output
          // buffers and its working RGBX ones are allocated differently — the last untested
          // variable behind iHD refusing the NV12 import. Drop it once that is answered.
          // The spoofed format only has to carry bytes — the real layout is in bo->meta — but
          // which format the external allocator is asked for decides whether iHD can import the
          // result. XB24 from this allocator imports; the R8 it produced for NV12 outputs never
          // did, at any geometry or descriptor shape (bd remora-e5x.36).
          "0009-gbm_mesa-spoof-as-XBGR8888-not-R8-for-VA-importability.patch",
          // Still capture died where preview never did (bd remora-4ei.9): a JPEG is
          // HAL_PIXEL_FORMAT_BLOB, which has no DRM format and so takes 0009's spoof path, but the
          // camera block still forced SCANOUT on it — an RPi4 CSI alignment workaround upstream
          // applies to every CAMERA_* bo — and forced it STRONGLY, which suppresses the
          // non-scanout retry. The host refused a 1024xN scanout buffer and there was no second
          // try, so the capture request failed while the preview, whose buffers are real 2D
          // images, went on working.
          "0010-gbm_mesa-a-spoofed-1D-buffer-is-never-scanout.patch",
          // 0010 ALONE FIXED NOTHING, and the reason is worth keeping: it cleared scanout on the
          // spoof branch, but a blob never reaches that branch. HAL_PIXEL_FORMAT_BLOB resolves to
          // a REAL drm format (R8 — see gbm_mesa_init's own comment on the R8 combination), so
          // drm_format is non-zero and the still-capture buffer was handed to the host verbatim as
          // "alloc 929664x1 fmt=R8 ... host status 22". 0011 routes blobs onto the spoof path,
          // which is where the 1D-to-2D reshape and the true-format metadata already live, and
          // which makes 0010's scanout clearing load-bearing rather than dead.
          "0011-gbm_mesa-route-BLOB-through-the-spoof-path-R8-is-a-real-format.patch",
          // open_drm_dev globs /dev/dri/renderD* and dereferenced drmGetVersion without
          // checking it. Remora masks the GPU nodes an instance must not use by bind-mounting
          // /dev/null over them — openable, matches the glob, answers no DRM version — so on
          // any host with a node to mask the allocator HAL segfaulted on its FIRST allocation
          // and took SurfaceFlinger with it (bd remora-ykhz, hit on amd-host-b:
          // two render nodes, renderD129 masked; amd-host-a has one node, nothing to mask, and
          // ran the same image for two days). Note gralloc.gbm.device cannot work around it —
          // that property is honoured by the CALLER, which runs open_drm_dev anyway, and the
          // crash is inside its loop before the callback is ever reached.
          "0012-gbm_mesa-skip-glob-hits-that-are-not-DRM-devices.patch"},
         false, false, "gpu", {17}, {.venus = true}},
        // ITS OWN ENTRY, not a fourth file in the minigbm_rendernode series, for two reasons. The
        // series resolves per version and the -a<ver> directory REPLACES the shared one rather than
        // extending it, so a file added to external-minigbm-a17 alone desyncs the shared entry's
        // declared list on A16 — the same shape the omx entry was split out for. And this must not
        // inherit that series' default-on: it is UNVALIDATED ON AMD HARDWARE (see below), and
        // folding it in would ship an allocation change to the golden stack unmeasured.
        //
        // OFF BY DEFAULT AND DELIBERATELY, despite fixing a real hole. What it does is sound and is
        // a strict improvement on the path it touches — without it amdgpu_init returns -ENODEV on a
        // Mesa-26 image, so gralloc.cros has no amdgpu backend AT ALL, and with it the backend comes
        // up LINEAR-only. But it changes how buffers are allocated on amdgpu, this project owns no
        // AMD host (the nearest are amd-host-a/amd-host-b), and defaultOnFor is documented as a
        // statement that someone CHECKED a release. Compiling is not checking. Turn it on once
        // render AND VCE HEVC encode are proven on an AMD host running a Mesa-26 image — encode
        // specifically, because that is the path that most wants the tiling this gives up.
        {"minigbm_amdgpu_no_dri", "minigbm amdgpu: tolerate a Mesa-26 DRI loader (AMD hosts)",
         "Lets the amdgpu gralloc backend come up when the DRI loader has no driver extensions to "
         "offer. Mesa 26 exports no __driDriverGetExtensions_* at all (48 in the 24.0.8 prebuilt, "
         "zero in 26.1) and dri_init fails, which made amdgpu_init return -ENODEV — so on an AMD "
         "host running a from-source Mesa image gralloc.cros had no amdgpu backend whatsoever. With "
         "this the backend initialises and allocates LINEAR: tiling and DCC are given up, plain "
         "DRM_AMDGPU_GEM_CREATE still works, and a non-linear modifier request is refused honestly "
         "instead of dereferencing a NULL dri. ONLY MATTERS with Mesa from source, and ONLY on an "
         "AMD host — every other backend ignores it. NOT YET VALIDATED ON AMD HARDWARE (bd "
         "remora-ykhz.1): it compiles and the reasoning is checked, but nothing has run it.",
         "external/minigbm", "external-minigbm-mesa26",
         {"0001-amdgpu-survive-a-Mesa-26-DRI-loader-that-exports-no-.patch"}, true, false, "gpu",
         {}, {.feature = QStringLiteral("mesa_source")}},
        // ITS OWN ENTRY rather than a fourth file in minigbm_rendernode, for the reason that entry's
        // own comment gives: the series resolves per version and external-minigbm-a17 REPLACES the
        // shared directory rather than extending it, so a file added to one alone desyncs the
        // declared list on the other. The three shared/a17 pairs already differ file-for-file, and
        // this fix has only been generated against the A17 tree, so it gets its own version-scoped
        // entry instead of a fourth name both directories would have to carry.
        //
        // OFF BY DEFAULT, on the same standard minigbm_amdgpu_no_dri is held to: the defect and its
        // cause are measured, but nothing has run the fix. It also changes allocation for EVERY
        // gralloc0 buffer in the image, which is not a change to make default on an unvalidated
        // reading.
        {"minigbm_gralloc0_metadata", "minigbm: give gralloc0 buffers a metadata region",
         "Makes the gralloc0 allocation path request the metadata reserved region that the gralloc4 "
         "path already requests. Without it every buffer the HIDL allocator@2.0 service mints has no "
         "metadata fd, so every gralloc4 metadata call on that buffer fails — get_dataspace AND "
         "set_dataspace, so the dataspace can never be recorded either, not merely never read. "
         "MEASURED, not reasoned about (bd remora-ykhz): a ten-second video playback on an AMD host "
         "logged 433 'Buffer does not have reserved region' / 'Failed to get_dataspace' pairs, and "
         "the same pair appears from surfaceflinger, system_server and systemui while idle. That "
         "disproves the assumption in cros_gralloc_driver::allocate()'s own comment, which skips "
         "metadata init on the grounds that these are 'gralloc4 mapper APIs the gralloc0 path never "
         "invokes' — consumers ask regardless of which allocator minted the buffer. Costs one memfd "
         "and one handle fd per buffer, exactly what the gralloc4 path already spends; the handle is "
         "already sized for it. WHETHER IT FIXES VIDEO DISPLAY IS UNPROVEN: the errors are real and "
         "the cause is certain, but the AMD hosts' black-video symptom has not been traced to them.",
         "external/minigbm", "external-minigbm-gralloc0-metadata",
         {"0001-gralloc0-allocate-the-metadata-reserved-region.patch"}, true, false, "gpu", {17}},
        // NEEDS virtio IN THE MESA BUILD TO DO ANYTHING, which is the whole reason this became
        // possible: build-mesa.sh's REMORA_MESA_VULKAN must include virtio, or external/mesa3d is
        // patched and no vulkan.virtio.so is produced from it. bd remora-brhq was marked blocked on
        // "stand up a Venus build from Mesa source", when a configure proved our
        // existing cross build emits libvulkan_virtio.so with vn_ring.c in it.
        //
        // OFF BY DEFAULT, and this one is stricter than the usual "compiles is not checked": the
        // path CANNOT be exercised here at all. Reaching it needs an NVIDIA/Venus host AND a fatal
        // ring, which bd remora-brhq's own chain shows arrives via VRAM exhaustion and an
        // Xid. The change compiles clean against Mesa 26.1 with the NDK cross toolchain; nothing has
        // run it.
        //
        // AND ON THE NVIDIA HOST THE IMAGE COPY IS SHADOWED: the host prerequisites' prebuilt
        // vulkan.virtio.so is bind-mounted over it at deploy, so shipping this without also dropping
        // that mount patches a driver the machine never loads — the verify-the-artifact trap.
        {"venus_device_loss", "Venus: device loss instead of abort on a fatal ring",
         "Replaces the abort() in vn_ring_submit_internal with VK_ERROR_DEVICE_LOST out of "
         "vn_ring_submit_locked. Upstream Mesa aborts when the host marks the ring fatal, which on "
         "Android kills whichever process submitted — SurfaceFlinger — and restarts zygote and the "
         "foreground app with it (bd remora-brhq measured exactly that: a phantom app failure with no "
         "tombstone of its own). ANGLE has a device-loss path, so reporting the error gives the caller "
         "a chance to recover; the frames are lost either way and only the blast radius changes. "
         "Carries the cleanup abort() never needed: the submit is not yet on ring->submits at that "
         "point, so the fatal path unrefs its shmems and returns it to ring->free_submits, mirroring "
         "vn_ring_retire_submits. NOT RUN — see the comment above.",
         "external/mesa3d", "external-mesa3d-venus-device-loss",
         {"0001-venus-report-device-loss-instead-of-aborting-on-a-fatal-ring.patch"},
         true, false, "gpu", {17}},
        {"v4l2_encoder", "v4l2_codec2 encoder path",
         "Support the VAAPI HEVC encoder path in the v4l2_codec2 component, and keep the "
         "decoder's output buffers gralloc-LINEAR (tiled output silently fails to sample in SF's "
         "render engine on the xe VF: audio plays, picture frozen). Also completes drop-path works "
         "immediately so c2-va's missing-reference frame skip cannot stall the input pipeline "
         "(0005; A17 carries the same fix in its container-patches v4l2_codec2 series, so the "
         "blanket -a17 SKIP loses nothing).",
         "external/v4l2_codec2", "external-v4l2_codec2",
         {"0001-Support-the-VAAPI-HEVC-encoder-path.patch",
          "0002-Decoder-output-buffers-request-CPU_READ-so-gralloc-a.patch",
          "0003-DecodeComponent-report-picture-size-changes-via-conf.patch",
          "0004-EncodeComponent-max-fps-frame-dropping-at-the-input-.patch",
          "0005-DecodeComponent-set-FLAG_DROP_FRAME-on-kAborted-so-s.patch"}, true, true, "gpu"},
        // Its OWN entry, not file five of v4l2_encoder (bd remora-22n). The two differ in VERSION
        // SCOPE: 0001-0004 are subsumed upstream on A17 (that entry's -a17 dir is a SKIP), while
        // the AV1 enum member is needed on every release — the unconditionally staged c2-va
        // references VideoCodec::AV1 (bd remora-e5x.22). As the series' fifth file it either
        // drowned under the blanket SKIP (the A17 build breaker) or, listed
        // alone in the -a17 override, made the loop hunt for the other four filenames and print
        // four expected "error: can't open patch" lines — exactly the noise that hid the original
        // failure. Ordered AFTER v4l2_encoder: they share a project, and registry order is apply
        // order, so on releases where both apply the enum patch lands on top of the series just
        // as it did as its fifth file. Existing profiles keep the capability without re-pinning:
        // canonicalPatchKeys() expands the old key to cover both entries.
        {"v4l2_av1_enum", "v4l2_codec2 AV1 codec enum",
         "Adds VideoCodec::AV1 to the shared codec enum so c2-va's AV1 decoder can register "
         "(bd remora-e5x.22). Applies on every version: the A17 device tree subsumes the rest of "
         "the v4l2_codec2 series, but not this.",
         "external/v4l2_codec2", "external-v4l2_codec2-av1",
         {"0005-Add-VideoCodec-AV1-to-the-shared-codec-enum.patch"}, true, true, "gpu"},
        // Its OWN entry rather than a sixth file in the shared series, which the -a17 SKIP would
        // swallow whole — the exact failure that directory's README documents (bd remora-22n).
        // Every version: the crash is in upstream code the A17 device-tree commit does not touch.
        // THE VA DRIVER'S INSTALL PATH (bd remora-28ix.7). external/intel-media-driver builds
        // iHD_drv_video.so, which every hardware video path goes through. AOSP DELETED the project
        // in Android 17, so the A16 revision is pinned across releases in pinned-manifest-a17.xml —
        // and the checkout that pin produces is PRISTINE, while the hand-copied directory it
        // replaces carried this one local edit. Without it the module installs to /vendor/lib64
        // rather than /vendor/lib64/dri, which is where LIBVA_DRIVERS_PATH looks and where the old
        // prebuilt lived, so libva finds no driver and hardware video silently disappears.
        // defaultOn, because an image without it is not a smaller image — it is a broken one.
        {"intel_media_driver_dri_path", "iHD VA driver install path",
         "Installs iHD_drv_video.so into /vendor/lib64/dri instead of /vendor/lib64. The VA loader "
         "only looks in dri/, so without this the driver is built, shipped, and never found — "
         "hardware decode and encode fall back or fail with no obvious cause. Pairs with the "
         "cross-release manifest pin that makes the project reproducible at all (bd remora-28ix.7).",
         "external/intel-media-driver", "external-intel-media-driver",
         {"0001-install-iHD_drv_video-into-vendor-lib64-dri.patch"}, false, true, "gpu", {17}},
        {"v4l2_framepool_null", "v4l2_codec2 frame-pool fixes",
         "Stops the c2-va HAL dying whenever a decoder block arrives with no bufferpool data — "
         "an unchecked deref that killed the service on every video an app played, and made the "
         "mirror flicker black as it respawned. Falls back to the dma-buf id so hardware decode "
         "keeps working rather than merely not crashing (bd remora-3fe6). Also bounds the "
         "surplus-buffer drop retry, an unbounded repost that stopped a no-surface decode dead at "
         "exactly getOutputDelay+9 frames with no error at all (bd remora-e5x.38).",
         "external/v4l2_codec2", "external-v4l2_codec2-framepool",
         {"0001-VideoFramePool-survive-a-block-with-no-pool-data.patch",
          "0002-VideoFramePool-bound-the-surplus-buffer-drop-retry.patch"},
         true, true, "gpu"},
        // Its OWN entry, not a second file on device_sepolicy: the apply loop is ATOMIC PER ENTRY,
        // so bundling would let a conflict silently discard both (the lineage_version_props
        // lesson). The overlay dir itself is already wired — the product .mk carries
        // DEVICE_PACKAGE_OVERLAYS := $(LOCAL_PATH)/overlay upstream — so this one file is the whole
        // change. It sat on disk since the updater UI landed, registered by NOTHING, so no source
        // build ever applied it and the stock entry kept shadowing the RemoraUpdater tile
        // (bd remora-82c.9).
        {"settings_hide_stock_updater", "Hide the stock Settings 'System update' entry",
         "Overlays config_show_system_update_settings=false: the stock preference resolves to "
         "GMS's OTA client, which knows nothing about Remora images, re-enables itself after "
         "every pm disable, and shadows the injected RemoraUpdater tile (bd remora-3ho). "
         "Inert without the system_updater feature — hiding the only updater would leave none.",
         "device/remora", "device-remora", {"hide-stock-updater-entry.patch"}, false, false,
         "extras", {}, {.feature = QStringLiteral("system_updater")}},
        {"libchrome_fix", "libchrome build fix",
         "external/libchrome Android.bp fix needed by the c2-va build graph.",
         "external/libchrome", "external-libchrome", {"working.patch"}, false, true, "gpu"},
        // EXPERIMENTAL, default OFF (bd remora-cbd). LineageOS cut lineage-24.0 as a pure AOSP-17
        // tracking branch and have not forward-ported their platform work, so the A17 image ships
        // AOSP's Settings and nothing reads the ro.lineage.* props we set. This replays the
        // community's own commits from the previous release branch onto our newer base.
        // Regenerate with vendor/source-build/gen-lineage-backport.sh; the EMPTY patches list
        // means "every NNNN-*.patch in the directory, sorted", so the series can be refreshed
        // without editing this file. Expect conflicts: these were written against AOSP 16, and
        // carrying them is the standing merge burden that comes with not waiting for upstream.
        {"lineage_platform_backport", "LineageOS platform backport (EXPERIMENTAL)",
         "Replays LineageOS's own packages/apps/Settings commits from lineage-23.2 onto the "
         "AOSP-17 base, including the About-phone LineageOS version/build-date entries. 96 "
         "upstream patches + 7 Remora forward-ports, written against AOSP 16 — expect conflicts, "
         "and expect to regenerate after every upstream sync. OFF by default; turn it on only "
         "when you are prepared to fix fallout. Once LineageOS forward-port their work to 24.0 "
         "this becomes unnecessary — re-check with check-lineage-backport.sh, which counts on the "
         "manifest ref. Do not re-check by looking at the checkout: this series lands on HEAD and "
         "git am keeps LineageOS's authorship, so both 'lineage files present' and "
         "'LineageOS-authored commits' read positive off our own work (bd remora-0hl).",
         "packages/apps/Settings", "lineage-backport-packages-apps-Settings", {}, true, false,
         "lineage", {}, {.lineageTree = true}},
        // Pairs with the lineage-backport fixup that forward-ports LineageLegalPreference:
        // that preference calls isAvailable() -> false on an empty ro.lineagelegal.url, so without
        // this property the ported "LineageOS legal" entry never appears. Upstream sets it in
        // vendor/lineage/config/common_mobile.mk, which the device tree does not inherit.
        {"no_lineage_sepolicy", "Skip the LineageOS sepolicy (does not build on AOSP 17)",
         "Neuters device/lineage/sepolicy/common/sepolicy.mk to a no-op comment. "
         "build/make/core/config.mk:1315 includes that file on a bare $(wildcard) test, so merely "
         "having device/lineage on disk pulls LineageOS's whole sepolicy in — no product config, no "
         "inherit, no LINEAGE_BUILD. On an AOSP 17 base it does not build: private/file.te uses "
         "attribute sdcard_posix_contextmount_type, which nothing here declares (upstream Lineage "
         "carries it in its own system/sepolicy fork; ours is AOSP's), and declaring it only exposes "
         "the next layer — libsepol reports 2 neverallow failures, i.e. Lineage's rules conflict "
         "with AOSP 17's. Chasing that rule by rule is open-ended for zero benefit here: this tree "
         "already applies the 'ignore selinux' patch, so the policy buys nothing. Emptied rather "
         "than deleted so the wildcard still finds a file and the include is a clean no-op. "
         "VERIFIED: selinux_policy failed before and builds after (181 steps, exit 0).",
         "device/lineage/sepolicy", "device-lineage-sepolicy",
         {"no-lineage-sepolicy.patch"}, false, true, "lineage", {17}, {.lineageTree = true}},
        // Also pairs with the backport, but this one is load-bearing rather than cosmetic: without
        // it soong ANALYSIS fails outright ("org.lineageos.platform ... depends on
        // //frameworks/base/services:services.impl which is not visible to this module"), so the
        // build dies four minutes in, before compiling anything.
        {"lineage_backport_deps", "LineageOS backport prerequisites (frameworks/base)",
         "Grants lineage-sdk visibility of services.impl — AOSP split services into "
         "services + services.impl and LineageOS's grant did not follow — and widens "
         "LockPatternUtils' getBoolean/setBoolean to protected so LineageLockPatternUtils can "
         "subclass them. Inert without the backport: a visibility entry naming an unbuilt package "
         "costs nothing and two protected methods change no behaviour.\n\n"
         "Also adds the four pattern-visibility accessors Settings backport 0078 calls — "
         "isVisibleDotsEnabled/setVisibleDotsEnabled and isShowErrorPath/setShowErrorPath. Those "
         "callers take the BASE LockPatternUtils, so the protected widening above is necessary but "
         "not sufficient: they never go through the LineageLockPatternUtils subclass. Without them "
         "Settings-core javac fails with exactly four 'cannot find symbol' errors; with them it "
         "builds clean (verified by a Settings-core module build, bd remora-4ei.32).",
         "frameworks/base", "frameworks-base-lineage-backport",
         {"0001-A17-let-lineage-sdk-see-services.impl-and-subclass-L.patch",
          "0002-A17-add-the-LineageOS-pattern-visibility-accessors-t.patch"}, true, true, "lineage", {17},
         {.patch = "lineage_platform_backport", .lineageTree = true}},
        {"buildprop_touch_rotation", "build.prop: emit RecoveryDefaultTouchRotation",
         "build/soong's gen_build_prop.py reads config[\"RecoveryDefaultTouchRotation\"] with a HARD "
         "subscript, but build/make's soong_extra_config.mk never emits the key — a producer/consumer "
         "split between two LineageOS forks, not a device-config gap. Without this, system, "
         "system_ext and product build.prop generation all die with "
         "KeyError: 'RecoveryDefaultTouchRotation' at ~78% of the build, after two hours of "
         "compiling. Emitted rather than guarded on the Python side because the sibling AOSP keys "
         "are emitted unconditionally too; add_json_str writes an empty string when unset, which is "
         "exactly what the consumer's falsiness check expects. Inert on any tree whose build/make "
         "already emits it.",
         "build/make", "build-make",
         {"0001-A17-emit-RecoveryDefaultTouchRotation-which-gen_buil.patch"}, true, true, "build", {17}},
        {"soong_gomemlimit", "Bound soong_build's Go heap",
         "Lets SOONG_GOMEMLIMIT reach soong_build. The bootstrap ninja launches it via 'env -i', "
         "so it starts with an EMPTY environment and GOMEMLIMIT cannot be set from the container "
         "— soong_ui's invocationEnv is the only channel in. Without it Go does not collect until "
         "the heap hits GOGC=100's default 2x live set: a ~19.4 GiB live set means no collection "
         "before ~38.8 GiB, so any container memory cap below that OOM-kills the build and "
         "raising the cap only moves the kill. Inert unless SOONG_GOMEMLIMIT is set.",
         "build/soong", "build-soong",
         {"0001-soong-let-SOONG_GOMEMLIMIT-bound-soong_build-s-Go-he.patch"}, true, true, "build"},
        {"lineage_soong", "lineage soong config",
         "vendor/lineage soong + common.mk changes for the custom modules.",
         "vendor/lineage", "vendor-lineage", {"working.patch"}, false, true, "lineage", {},
         {.lineageTree = true}},
        // The a17 working.patch ALSO carries vendor/lineage's former bake (bd remora-fooj):
        // 431eabb9 "port lineage_generator plugin to lineage-24.0 build/soong" was baked into the
        // pinned revision with no patch-file counterpart anywhere, so re-pinning to the upstream
        // base would have silently dropped the plugin port. It was exported into THIS patch rather
        // than a new registry entry for a reason worth keeping: source_patches= in a profile is an
        // EXPLICIT list, so a brand-new key is off for every existing profile no matter what
        // defaultOn says — the build says "NOT APPLIED (not enabled in this profile)" and carries
        // on. A new entry would therefore have reintroduced the very drop it was meant to prevent,
        // silently, on the next build. Folding it into an already-enabled entry needs no profile
        // edit. It belongs here on the merits too: generator.go is a soong change, which is what
        // this entry is for.
        {"lineage_sdk_features", "Honest Lineage SDK feature set",
         "Ship only the Lineage SDK feature XMLs the image actually backs. lineage-24.0 is a "
         "pure AOSP-17 tracking branch with no LineageSystemServer hook, so six declared "
         "services (globalactions, hardware, health, livedisplay, profiles, trust) never run — "
         "hasSystemFeature() lied, LineageHardwareManager Log.wtf'd on every init, and "
         "lineageos.preference rows tombstoned instead of hiding. org.lineageos.settings stays: "
         "LineageSettingsProvider ships and answers queries. Verified live: removing the six "
         "XMLs drops the features, kills the wtf, and LineageParts stays healthy "
         "(bd remora-4ei.62).\n\n"
         "A16 SKIPs this (the -a16 marker): lineage-23.0 HAS the SystemServer hook at the pin "
         "and all seven service halves, and its keyguard calls LineageGlobalActions without a "
         "feature check — so there the strip was the one thing keeping the service from starting "
         "and SystemUI crash-looped on a genuine Lineage base (bd remora-sgrk).",
         "vendor/lineage", "vendor-lineage-sdk-features",
         {"0001-lineage-sdk-drop-unbacked-feature-xmls.patch"}, false, true, "lineage", {17},
         {.lineageTree = true}},
        // MUST stay AFTER lineage_soong and lineage_sdk_features: all three edit vendor/lineage, and
        // this patch's context is config/common.mk as those two leave it.
        {"drop_lineage_updater", "Drop the LineageOS Updater app",
         "Removes Updater (and its init.lineage-updater.rc) from PRODUCT_PACKAGES in "
         "vendor/lineage/config/common.mk — the ONLY line that adds it. LineageOS's OTA updater is "
         "meaningless in a container (no A/B slot pair, no OTA payload to stream) and Remora ships "
         "its own in-Android updater (the system_updater feature), which is what actually services "
         "this image.\n\n"
         "It is a HARD BUILD BREAK, not a preference: packages/apps/Updater's BatteryMonitor.kt "
         "calls UpdateEngine.setPerformanceMode(), a LineageOS addition to android.os.UpdateEngine "
         "carried in their frameworks/base patches. The app suite pins to lineage-23.2 where that "
         "exists; this platform frameworks/base is lineage-24.0, still pristine AOSP with those "
         "patches not forward-ported (bd remora-cbd.1), so Kotlin fails with 'unresolved reference "
         "setPerformanceMode'. It surfaced the moment the completed A17 pin began syncing "
         "packages/apps/Updater at all — it was one of the 27 projects the original capture dropped, "
         "so it had never been built before.\n\n"
         "PATCHED HERE AND NOT VIA PRODUCT_PACKAGES_REMOVE: that variable DOES NOT EXIST in this "
         "build system. It appears nowhere under build/ or vendor/lineage/, so the removal block in "
         "the device tree's lineage.mk is silently inert and adding Updater to it changed nothing — "
         "measured, after a full build reached the same Kotlin error a second time. The other names "
         "in that block (Camera2/Gallery2/QuickSearchBox) are absent from the image because nothing "
         "adds them, not because they are removed.",
         "vendor/lineage", "vendor-lineage-drop-updater", {"working.patch"}, false, true, "lineage",
         {17}, {.lineageTree = true}},
        {"surfaceview_secure", "SurfaceView secure capture",
         "Ignore SurfaceView.setSecure() so DRM video (Pluto/Netflix &co) isn't blanked black on "
         "the virtual display the mirror captures. No physical secure display exists in a container.",
         "frameworks/base", "frameworks-base",
         {"0001-surfaceview-ignore-secure.patch"}, false, true, "container"},
        {"shell_update_owner", "Shell can claim app update ownership",
         "Let a shell --update-ownership install claim ownership even when pm uninstall -k kept "
         "the PackageSetting (uid + app data). Without this a data-preserving reinstall is never "
         "\"initial\" and the claim is silently dropped — so manually injected arm64 apps could "
         "not be migrated under shell ownership (which is what blocks Play from silently "
         "auto-updating them back onto their broken x86_64 splits) without wiping their data.",
         "frameworks/base", "frameworks-base",
         {"0002-pm-let-shell-claim-update-ownership-over-a-kept-Pack.patch"}, true, true, "container"},
        // ONE capability, formerly four checkboxes (bd remora-82c.5). They shared a project, a
        // directory, a style, a default and a version scope, and — decisively — every one of them
        // is INERT until icon_blacklist is set. So no combination of three-of-four was ever a
        // thing to want: the hide list either works or it does not. The four retired keys are
        // mapped onto this one by canonicalPatchKeys(), so existing profiles keep the capability.
        {"statusbar_hide_list", "Status bar icon hide list (clock, battery, tuner entry)",
         "Make the icon_blacklist secure setting real again on the A17 status bar, end to end. "
         "Inert until icon_blacklist is set; setting it is what turns any of this on.\n\n"
         "• Icons: A17 ships new_status_bar_icons=enabled, and the new Compose pipeline's "
         "blocklist interactor only read the device config array — so the hide list the legacy "
         "status bar honored became a silent no-op for every migrated icon.\n"
         "• Clock: registers itself as a tunable for ICON_HIDE_LIST, but onTuningChanged only "
         "ever handled clock_seconds, so the callback was delivered and dropped.\n"
         "• Battery: renders outside SystemStatusIcons (the addBatteryComposable ComposeView and "
         "the inline UnifiedBattery in the icons Row), so it never reaches the per-slot filter "
         "and must opt in at each site. This is the only one that removes a view from a laid-out "
         "container; the padding question was settled visually (bd remora-fgj.9) by A/B "
         "screenshot on the live 3760x1992 instance — all icons 48px right margin, battery "
         "hidden 60px, hiding ETHERNET instead leaves it at 48px. The 12px is the right-bearing "
         "of whichever icon ends up last, not a padding defect.\n"
         "• Tuner entry: LineageParts' Settings > System > Status bar > \"System icons\" fires "
         "com.android.settings.action.STATUS_BAR_TUNER, a LineageOS SystemUI action lineage-24.0 "
         "never forward-ported, and androidx Preference.performClick calls startActivity with no "
         "try/catch — so that row was a fatal ActivityNotFoundException. Routed through an "
         "activity-alias rather than by enabling TunerActivity, so the activity stays disabled "
         "and adds no \"System UI Tuner\" tile. It is the screen that WRITES icon_blacklist, "
         "which the three above are what make real.",
         // OWN DIRECTORY SINCE (bd remora-31sq). This shared frameworks-base with three
         // other keys, and a version override is per-DIRECTORY: skipping this series on A16 would
         // have skipped surfaceview_secure and shell_update_owner with it, which apply there fine.
         "frameworks/base", "frameworks-base-statusbar",
         {"0001-statusbar-honor-icon_blacklist-in-the-Compose-icon-p.patch",
          "0002-statusbar-let-the-icon-hide-list-hide-the-clock-agai.patch",
          "0003-statusbar-let-the-icon-hide-list-hide-the-battery.patch",
          "0004-statusbar-give-the-tuner-s-icon-list-an-entry-point-.patch"},
         true, true, "container", {17}},
        {"lineage_platform_res", "Load the LineageOS SDK resources",
         "Put org.lineageos.platform-res.apk (reserved package id 0x3f) into the zygote's shared "
         "system asset set, beside framework-res.apk. Without it the SDK ships half-reachable: "
         "Settings and LineageParts static-link org.lineageos.platform.internal, so the classes "
         "and their generated R class compile in, but the 0x3f resources those IDs name are in a "
         "separate APK that nothing puts on any app's asset path — so Settings > Display died on "
         "inflate with Resources$NotFoundException 0x3f020004, and so would the other six screens "
         "carrying one of the 13 lineageos.preference widgets.\n\n"
         "Not a <library>/uses-library fix: no such declaration exists anywhere in the tree and a "
         "shared-library entry adds only the jar to the classpath. Deleting the offending "
         "preferences does not work either — ConstraintsHelper's constructor calls "
         "checkConstraints unconditionally, so removing one widget just moves the crash to the "
         "next (which is exactly what one attempt did, bd remora-gk8).\n\n"
         "Needs no LineageSystemServer: once the resources resolve, requiresFeature="
         "\"lineagehardware:*\" reaches LineageHardwareManager.isSupported(), the AIDL/HWC2 "
         "lookups come up empty on a container with no Lineage HAL, and each widget tombstones "
         "itself. Loading is best-effort — the enclosing catch would turn a missing APK into a "
         "boot loop — so it is inert on a tree that ships no lineage-sdk.",
         // Own directory too, same reason as the statusbar series above.
         "frameworks/base", "frameworks-base-lineage-res",
         {"0001-res-load-the-LineageOS-platform-resource-APK-into-ev.patch"}, true, true, "lineage", {17},
         {.lineageTree = true}},
        {"cursor_on_motion", "Mouse cursor only on motion",
         "Don't draw the Android mouse pointer when a mouse merely connects — boot-time virtual "
         "mice (Sunshine passthrough uinput, console tablets, the mirror's uhid device) painted a "
         "stray motionless cursor at screen center during boot. The cursor still appears the "
         "moment a mouse actually moves — Moonlight sessions and uhid mice are unaffected.",
         "frameworks/native", "frameworks-native",
         {"0001-inputflinger-don-t-show-the-mouse-cursor-on-device-c.patch"}, true, true, "container"},
        {"submix_all_output", "Route all audio to the remote submix",
         "The container has no speaker, so every strategy AOSP sends there is discarded. "
         "primary_audio_policy_configuration.xml attaches a Speaker and names it "
         "defaultOutputDevice, but the primary HAL behind it is AOSP's stub — it accepts every "
         "write and plays nothing. The one output that leaves the container is the remote "
         "submix, which the mirror captures.\n\n"
         "AOSP withholds the submix from the sonification strategies deliberately (\"no "
         "sonification on remote submix (e.g. WFD)\" in getDevicesForStrategyInt, plus an "
         "explicit REMOTE_SUBMIX removal for STRATEGY_SONIFICATION_RESPECTFUL in "
         "filterOutputDevicesForStrategy). On a phone that is right — the speaker is real and "
         "you want the ringtone in your pocket, not on the TV. Here it means ringtones, alarms, "
         "notifications and call audio are silent while media and the UI sounds that share "
         "STRATEGY_MEDIA play, which reads as half the sounds being broken (bd remora-8g24).\n\n"
         "This substitutes at the single funnel every output routing decision passes through: a "
         "strategy that resolved to a lone speaker while a submix is available goes to the "
         "submix instead. Only ever a replacement for the speaker, so a strategy that found a "
         "real device — or one pinned through setPreferredDeviceForStrategy — is untouched. "
         "Self-scoping: no capture means no submix device means nothing to substitute, so an "
         "unmirrored container behaves exactly as upstream. Runtime escape hatch: "
         "persist.remora.submix_all_output=0 plus an audioserver restart.\n\n"
         "Note the preferred-device role is NOT an alternative here. It is consulted before the "
         "sonification guard, so it does rescue ring and alarm, but filterOutputDevicesForStrategy "
         "strips the submix out before the preference is even read for notifications — measured, "
         "not assumed. Notifications need this patch.",
         "frameworks/av", "frameworks-av",
         {"0001-audiopolicy-send-stub-speaker-output-to-the-remote-s.patch"}, true, true,
         "container", {17}},
        {"usb_audio_policy", "Declare the USB audio module in the audio policy",
         "One <xi:include> into AOSP's audio_policy_configuration_generic.xml, which is what the "
         "top-level /vendor/etc/audio_policy_configuration.xml is built from. AudioPolicyManager "
         "opens exactly the modules that file includes, and the generic one lists only primary "
         "and r_submix — so audio.usb.default installs, is never opened, and capture keeps coming "
         "from the primary STUB, whose in_read() memsets the buffer to zero on a faked clock. The "
         "symptom is perfectly timed silence with no error anywhere (bd remora-4ei.37).\n\n"
         "PATCHED RATHER THAN REPLACED, and the alternative was tried and cannot work: remora.mk "
         "registers the frameworks/av/services/audiopolicy/config soong namespace, and soong "
         "emits an install rule for EVERY prebuilt_etc in a registered namespace regardless of "
         "PRODUCT_PACKAGES (proven on a2dp_audio_policy_configuration.xml — a rule, no "
         "PRODUCT_PACKAGES entry, required by nothing). A PRODUCT_COPY_FILES onto the same "
         "destination is therefore always a second rule for one target, which kati refuses at "
         "parse time. This file is the module's own src, so editing it leaves one rule and "
         "changes what it installs.\n\n"
         "NEVER ENABLE THIS WITHOUT THE 'usb_audio' FEATURE. The include names a fragment that "
         "only that feature installs, and an xi:include of a missing file fails the whole policy "
         "parse — that breaks ALL audio, not just capture. The build refuses the combination.",
         "frameworks/av", "frameworks-av-usb-audio",
         {"0001-audiopolicy-declare-the-USB-module-in-the-generic-po.patch",
          "0002-audiopolicy-attach-the-USB-capture-device-at-boot.patch"},
         true, false, "container", {}, {"usb_audio"}},
        {"keep_kernel_modprobe", "Keep host kernel.modprobe (module autoload)",
         "Drop init.rc's early-init blanking of kernel.modprobe (and kernel.sysrq): neither "
         "sysctl is namespaced, so in the privileged container the writes escape to the HOST "
         "kernel on every container start. The blank disables kernel module autoload host-wide "
         "— veth creation fails with EOPNOTSUPP and docker bridge networking on the container "
         "host breaks (bd remora-ofn). Android's reason (stop kernel-side autoload behind "
         "init's back) is meaningless in a container: the kernel resolves /sbin/modprobe in "
         "the host filesystem anyway.",
         "system/core", "system-core",
         {"0001-rootdir-keep-host-kernel.modprobe-and-kernel.sysrq-i.patch"}, true, true, "container"},
        {"sensors_hal", "Sensors HAL (host-injectable accel/gyro)",
         "AOSP's example AIDL sensors HAL taught to read debug.remora.sensor.accel/gyro — "
         "\"x,y,z\" properties the host sets over adb (`remora rotate` publishes the matching "
         "gravity vector). Falls back to the stock static payloads when unset, so the HAL "
         "behaves exactly like upstream until told otherwise.",
         "hardware/interfaces", "hardware-interfaces",
         {"0001-sensors-example-HAL-remora-host-injectable-accel-gyr.patch"}, true, false, "extras", {},
         {.feature = "sensors"}},
        {"sensors_product", "Sensors HAL packaging (vendor APEX)",
         "device/remora: ship the com.android.hardware.sensors vendor APEX (example service + "
         "init rc + vintf fragment) in the x86_64 product. Pairs with sensors_hal.",
         "device/remora", "device-remora-sensors", {"sensors.patch"}, false, false, "extras", {},
         {.feature = "sensors"}},
        {"boot_animation", "LineageOS boot animation (host-side)",
         "Build the real, display-sized LineageOS bootanimation.zip into the image for Remora's "
         "host-side viewer (watch-boot / the mirror's --boot-animation pulls it and plays it on the host "
         "GPU). The device tree leaves TARGET_SCREEN_* unset so the animation would render tiny; this sizes "
         "it via PRODUCT soong_config. debug.sf.nobootanimation=1 is deliberately KEPT: on the "
         "SR-IOV VF, SurfaceFlinger's render engine crash-loops (\"no suitable EGLConfig\") if it "
         "renders the animation before Mesa is up, so the device ships the zip but never draws it.",
         "device/remora", "device-remora-bootanim",
         {"0001-lineage-boot-animation.patch"}, false, false, "extras"},
    };
    return v;
}

const QVector<QPair<QString, QString>> &patchGroups() {
    // Display order, and deliberately NOT the registry's order — the registry is ordered by apply
    // dependency (see SourcePatch::group), which is the wrong order to read a page in. Labels say
    // what the group is FOR, not which projects it touches: "external/minigbm + external/v4l2_codec2
    // + …" is an accurate description of the first group and tells a reader nothing about whether
    // they want it.
    static const QVector<QPair<QString, QString>> v = {
        {QStringLiteral("gpu"), QStringLiteral("Graphics, media and codecs")},
        {QStringLiteral("build"), QStringLiteral("Build fixes")},
        {QStringLiteral("lineage"), QStringLiteral("LineageOS on an AOSP base")},
        {QStringLiteral("container"), QStringLiteral("Container and device behaviour")},
        {QStringLiteral("extras"), QStringLiteral("Optional extras")},
    };
    return v;
}

// defaultOn scoped to one release. An empty defaultOnFor means "every version" — the common case,
// for patches that fix hardware or build infrastructure rather than a release's own gaps.
bool patchIsDefaultFor(const SourcePatch &p, int androidVersion) {
    return p.defaultOn && (p.defaultOnFor.isEmpty() || p.defaultOnFor.contains(androidVersion));
}

QStringList defaultSourcePatchSet(int androidVersion) {
    QStringList out;
    for (const SourcePatch &p : sourcePatches())
        if (patchIsDefaultFor(p, androidVersion)) out << p.key;
    return out;
}

QStringList unappliedDefaultPatches(const QStringList &enabled, int androidVersion) {
    if (enabled.isEmpty()) return {};  // unset ⇒ defaults apply; nothing is pinned away
    QStringList out;
    for (const SourcePatch &p : sourcePatches())
        if (patchIsDefaultFor(p, androidVersion) && !enabled.contains(p.key)) out << p.key;
    return out;
}

QStringList sourcePatchProjects() {
    QStringList out;
    for (const SourcePatch &p : sourcePatches())
        if (!out.contains(p.projectPath)) out << p.projectPath;
    return out;
}

QString patchProjectSlug(const QString &projectPath) {
    QString s = projectPath;
    return s.replace(QLatin1Char('/'), QLatin1Char('-'));
}

QStringList canonicalPatchKeys(const QStringList &keys) {
    // retired key -> the entry that absorbed it. Entries here are permanent: a profile saved
    // before a consolidation can surface at any time, so a mapping is never removed once added.
    static const QHash<QString, QString> absorbed{
        // Four checkboxes for one capability. Each was inert on its own — they all do nothing
        // until icon_blacklist is set — and they share a project, a directory, a default and a
        // version scope, so nobody could sensibly want three of the four (bd remora-82c.5).
        {QStringLiteral("statusbar_clock_hide"), QStringLiteral("statusbar_hide_list")},
        {QStringLiteral("statusbar_battery_hide"), QStringLiteral("statusbar_hide_list")},
        {QStringLiteral("statusbar_tuner_entry"), QStringLiteral("statusbar_hide_list")},
    };
    // The reverse direction: a key whose entry was SPLIT stands for every piece afterwards. A pin
    // that named the old key chose the capability of the whole series, so dropping the new key
    // from it would silently un-apply part of what the profile demonstrably built with — the exact
    // pin trap this function exists for. Permanent like `absorbed`, and deliberately sticky: the
    // pieces cannot be pinned apart through the old key, which is correct here because c2-va is
    // staged unconditionally and references the enum member, so "encoder without the AV1 enum"
    // does not compile anyway (bd remora-22n).
    static const QHash<QString, QStringList> expanded{
        {QStringLiteral("v4l2_encoder"),
         {QStringLiteral("v4l2_encoder"), QStringLiteral("v4l2_av1_enum")}},
    };
    QStringList out;
    for (const QString &k : keys) {
        const QString c = absorbed.value(k, k);
        for (const QString &e : expanded.value(c, {c}))
            if (!out.contains(e)) out << e;
    }
    return out;
}

bool patchAppliesTo(const SourcePatch &p, int androidVersion, const VendorReader &read) {
    const QString base = QStringLiteral("source-patches/") + p.dir;
    // Same precedence as the apply loop: the per-version override wins when it has content.
    QList<QPair<QString, QByteArray>> files =
        read(base + QStringLiteral("-a%1").arg(androidVersion));
    if (files.isEmpty()) files = read(base);
    if (files.isEmpty()) return false;
    // SKIP means "this release needs nothing here" — the series exists but is absorbed upstream,
    // so offering it would be offering a no-op.
    for (const QPair<QString, QByteArray> &e : files)
        if (e.first.endsWith(QLatin1String("/SKIP"))) return false;
    return true;
}

QStringList applicableSourcePatchKeys(int androidVersion, const VendorReader &read) {
    QStringList out;
    for (const SourcePatch &p : sourcePatches())
        if (patchAppliesTo(p, androidVersion, read)) out << p.key;
    return out;
}

QVector<PatchAssetGap> auditPatchAssets(int androidVersion, const VendorReader &read) {
    QVector<PatchAssetGap> gaps;
    // Per RESOLVED directory, across every entry that resolves to it: which .patch files the
    // registry names (several entries share one dir — frameworks-base carries four — so a file is
    // "unknown" only if NO sharing entry names it), which keys share it, and whether any of them
    // globs (a glob consumes every NNNN-*.patch, so nothing in that dir can be unknown).
    struct DirClaim {
        QSet<QString> declared;
        QStringList keys;
        QStringList present;  // relative to the dir, .patch files only
        bool globbed = false;
        bool skip = false;
    };
    QMap<QString, DirClaim> claims;
    for (const SourcePatch &p : sourcePatches()) {
        const QString base = QStringLiteral("source-patches/") + p.dir;
        // The apply loop's resolution, exactly: the per-version override wins when it EXISTS.
        // Tested by content because VendorReader reports a directory by its files — so an override
        // that exists but is empty reads as absent here and the shared series is audited instead.
        // That is the documented patchAppliesTo edge and it stays consistent: an empty override is
        // a broken asset either way, and the apply loop still refuses, just later.
        const QString verDir = base + QStringLiteral("-a%1").arg(androidVersion);
        QList<QPair<QString, QByteArray>> files = read(verDir);
        const QString dir = files.isEmpty() ? base : verDir;
        if (files.isEmpty()) files = read(base);
        // NEITHER DIRECTORY EXISTS — and this is the headline stale-install shape, not a case to
        // skip: a registry entry added after the tree was installed has no directory there at all.
        // patchAppliesTo answers "inapplicable" (correctly, from the assets alone — it cannot tell
        // a version that ships nothing from a tree that is simply old), so the UI hides the row
        // while a profile that already pinned the key still drives the apply loop straight into
        // its hard-fail. Reported here because the CALLERS can tell the difference: both scope the
        // refusal to enabled entries, and `check` can say it plainly. In a healthy tree this never
        // fires — every registry entry is asset-backed, which patchAssetAuditFindsAStaleTree pins
        // against the real tree.
        if (files.isEmpty()) {
            gaps.append({p.key, dir, {QStringLiteral("<no patch directory>")}, {}});
            continue;
        }
        bool skip = false;
        QStringList present;
        for (const QPair<QString, QByteArray> &e : files) {
            const QString name = e.first.section(QLatin1Char('/'), -1);
            if (name == QLatin1String("SKIP")) skip = true;
            present << name;
        }
        DirClaim &claim = claims[dir];
        claim.keys << p.key;
        claim.skip = claim.skip || skip;
        if (p.patches.isEmpty()) claim.globbed = true;
        for (const QString &f : p.patches) claim.declared.insert(f);
        if (claim.present.isEmpty())
            // DIR-RELATIVE, not the flat basename `present` uses: the apply loop reads files at
            // the dir's top level only, so a .patch nested in a subdirectory is just as unapplied
            // as an unnamed one and must not hide behind a basename that happens to be declared.
            for (const QPair<QString, QByteArray> &e : files)
                if (e.first.endsWith(QLatin1String(".patch")))
                    claim.present << e.first.mid(dir.size() + 1);
        if (skip) continue;  // deliberately absent for this release
        QStringList missing;
        if (p.patches.isEmpty()) {
            // A generated series is globbed, so the registry names no files — "at least one
            // NNNN-*.patch" is the whole contract, and an empty match is this case's missing file.
            bool any = false;
            for (const QString &n : present)
                if (n.endsWith(QLatin1String(".patch")) && !n.isEmpty()
                    && n.at(0).isDigit())
                    any = true;
            if (!any) missing << QStringLiteral("<no NNNN-*.patch>");
        } else {
            for (const QString &f : p.patches)
                if (!present.contains(f)) missing << f;
        }
        if (!missing.isEmpty()) gaps.append({p.key, dir, missing, {}});
    }
    // The unknown-file pass, after every entry has stated its claim on its dir. One row PER
    // SHARING KEY, not per dir, so the callers' scoping ("does this build enable that key")
    // keeps working unchanged; the duplication is bounded by how many entries share a dir and
    // in a healthy tree the pass emits nothing at all. Directories the registry does not claim
    // at all are deliberately NOT audited — they are in no apply path.
    for (auto it = claims.cbegin(); it != claims.cend(); ++it) {
        const DirClaim &c = it.value();
        if (c.globbed || c.skip) continue;
        QStringList unknown;
        for (const QString &rel : c.present)
            if (!c.declared.contains(rel)) unknown << rel;
        if (unknown.isEmpty()) continue;
        for (const QString &k : c.keys) gaps.append({k, it.key(), {}, unknown});
    }
    return gaps;
}

QVector<VendorDriftRow> compareSourcePatchTrees(const VendorReader &mine,
                                                const VendorReader &other) {
    const QString root = QStringLiteral("source-patches");
    // relpath under source-patches -> bytes; both readers key by the same vendor-relative path.
    const auto flatten = [&root](const VendorReader &read) {
        QMap<QString, QByteArray> out;
        const QList<QPair<QString, QByteArray>> files = read(root);
        for (const QPair<QString, QByteArray> &e : files)
            out.insert(e.first.mid(root.size() + 1), e.second);
        return out;
    };
    const QMap<QString, QByteArray> a = flatten(mine);
    const QMap<QString, QByteArray> b = flatten(other);
    // Group per top-level patch dir; loose files directly under source-patches (none today, but a
    // comparison that silently ignored one would be this bead all over again) get "<root>".
    QMap<QString, VendorDriftRow> rows;
    const auto rowFor = [&rows](const QString &rel) -> VendorDriftRow & {
        const int cut = rel.indexOf(QLatin1Char('/'));
        const QString dir = cut < 0 ? QStringLiteral("<root>") : rel.left(cut);
        VendorDriftRow &r = rows[dir];
        r.dir = dir;
        return r;
    };
    const auto within = [](const QString &rel) {
        const int cut = rel.indexOf(QLatin1Char('/'));
        return cut < 0 ? rel : rel.mid(cut + 1);
    };
    for (auto it = a.cbegin(); it != a.cend(); ++it) {
        const auto o = b.constFind(it.key());
        if (o == b.cend())
            rowFor(it.key()).onlyInMine << within(it.key());
        else if (o.value() != it.value())
            rowFor(it.key()).differs << within(it.key());
    }
    for (auto it = b.cbegin(); it != b.cend(); ++it)
        if (!a.contains(it.key())) rowFor(it.key()).onlyInOther << within(it.key());
    QVector<VendorDriftRow> out;
    for (const VendorDriftRow &r : rows) out << r;  // QMap iterates key-sorted, so rows are stable
    return out;
}

QString containerPatchSet(int androidVersion) {
    // From the per-version registry (bd remora-82c.2). EXACT per version, never a range. The
    // `>= 17 ? a17 : a16` this once was contradicted its own header — an Android 18 tree silently
    // got the A17 set, every version below 16 the A16 one — and the consumer could not catch it:
    // apply-container-patches.sh only checks the directory EXISTS, and android-17.0.0 does. An
    // unknown version (or a prebuilt-only one) returns EMPTY, and the caller turns that into a hard
    // failure. A fallback to the nearest set is precisely the bug (bd remora-p4p, bd remora-cbd).
    const AndroidRelease *r = androidRelease(androidVersion);
    return r ? r->containerPatchSet : QString();
}

QStringList buildResetCommands(const QStringList &enabled, const QString &sourceDir,
                               const QMap<QString, QString> &projectMarks) {
    QStringList cmds;
    QSet<QString> seen;  // several patches can share one project — reset it once
    for (const SourcePatch &p : sourcePatches()) {
        if (!enabled.contains(p.key)) continue;
        if (seen.contains(p.projectPath)) continue;
        seen.insert(p.projectPath);
        const QString proj = sourceDir + QLatin1Char('/') + p.projectPath;
        cmds << guarded(projectMarks, p.projectPath,
                        QStringLiteral("{ [ -d %1/.git ] && { git -C %1 am --abort 2>/dev/null; "
                                       "git -C %1 reset --hard -q 2>/dev/null; "
                                       "git -C %1 clean -fdq 2>/dev/null; }; true; }")
                            .arg(proj));
    }
    return cmds;
}

QStringList buildApplyPatchCommands(const QStringList &enabled, const QString &sourceDir,
                                    const QString &patchRoot, int androidVersion,
                                    const QMap<QString, QString> &projectMarks,
                                    const QMap<QString, QString> &requiredBy) {
    QStringList cmds;
    // Registered-but-not-enabled default-on patches, announced BEFORE anything applies. The
    // enabled-set filter is the one drop in this whole pipeline that was silent: EXCLUDE drops and
    // missing files both name themselves, but a patch absent from the pinned list produces no
    // output at all, because the loop below never reaches it (bd remora-4ei.38). NOT guarded by
    // projectMarks — the condition is about the profile, not about any project's state, so an
    // incremental run that skips every project must still say it.
    for (const QString &key : unappliedDefaultPatches(enabled, androidVersion))
        cmds << QStringLiteral("echo \"NOT APPLIED (not enabled in this profile): %1 — tick it on "
                               "the Image page\"")
                    .arg(key);
    // THE MIRROR CASE, and it was silent for the same reason: a key the PROFILE names that the
    // REGISTRY no longer has. The loop below walks the registry and filters by `enabled`, so such
    // a key is never visited and produces no output whatsoever — the profile asks for a patch,
    // nothing applies it, and the build says nothing. Found on the live remorarc (bd remora-31sq):
    // [Instance-AMD build] still lists six entries deleted when the projects they patched left the
    // manifests, so six of its source_patches are phantoms.
    //
    // A WARNING RATHER THAN A REFUSAL, deliberately. A stale key is stale CONFIG, not a broken
    // build: the patch is gone because its content became native to device/remora or retired with
    // the project it targeted, so there is nothing missing from the image. The case where a
    // vanished patch WOULD ship a feature silently absent is already fatal one layer up — a
    // feature's required series that fails to land is bd remora-82c.1's hard failure, and
    // requiredBy carries exactly those owners. Refusing here would instead break every existing
    // profile the moment a dead entry is cleaned out of the registry, which is the wrong direction
    // for a purely bookkeeping divergence.
    QSet<QString> registered;
    for (const SourcePatch &p : sourcePatches()) registered.insert(p.key);
    for (const QString &key : enabled) {
        if (registered.contains(key)) continue;
        cmds << QStringLiteral(
                    "echo \"STALE PROFILE ENTRY: source patch '%1' is enabled in this profile but "
                    "is not in the registry — nothing applies it. It was most likely deleted with "
                    "the project it targeted; remove it from the profile's source_patches to "
                    "silence this.\"")
                    .arg(key);
    }
    QSet<QString> resetProjects;  // reset a shared project once, then stack patches on top
    for (const SourcePatch &p : sourcePatches()) {
        if (!enabled.contains(p.key)) continue;
        const QString proj = sourceDir + QLatin1Char('/') + p.projectPath;
        // Version-aware series. A patch written against one Android release frequently will not
        // apply to the next: upstream absorbs part of it (A17's cros_gralloc already carries the
        // enable_metadata_fd guard the A16 minigbm patch adds) or the surrounding file simply
        // drifts. A series may therefore ship a per-version override directory <dir>-a<N> beside
        // the shared <dir>; the override wins when present, otherwise the shared series applies
        // unchanged, so only genuinely divergent series need a variant. Resolution is done in the
        // shell, not here, because patchRoot is on the BUILD host — which may be a remote box
        // reached over ssh — so C++-side directory probing would test the wrong filesystem.
        // Brace-wrapped for the same reason the mbox step below is: callers join with " && ", so a
        // bare `a; b` here would split the chain — the assignment would inherit the previous
        // command's failure and be skipped while the patch step after the `;` still ran, applying
        // this group's patches out of the PREVIOUS group's directory.
        // Announce the RESOLVED directory. With both <dir> and <dir>-a<N> on disk nothing said
        // which one won, so a series applied out of the wrong place — or not at all — looked
        // identical in the log to one that worked (bd remora-fgj.11).
        cmds << guarded(projectMarks, p.projectPath,
                        QStringLiteral("{ d=%1/%2-a%3; [ -d \"$d\" ] || d=%1/%2; "
                                       "echo \"patches: %4 <- $d\"; "
                                       "[ -f \"$d/SKIP\" ] && echo \"SKIP %2 on A%3: "
                                       "$(cat \"$d/SKIP\")\"; true; }")
                            .arg(patchRoot, p.dir)
                            .arg(androidVersion)
                            .arg(p.key));
        // A project ABSENT from this tree is a SKIP or a FATAL, never a `git init`
        // (bd remora-31sq). The init that used to sit here existed for series that CREATE a
        // project (c2-va), but that series left the apply path (the hw_encoder note at the top
        // of this registry) and no current entry creates one. What the init actually did on an
        // absent project was manufacture an EMPTY repo so every per-file step could fail with
        // "does not exist in index" — 30 lines of red for a situation with one honest line in
        // it — and leave zero-commit litter that no manifest owns and a later glob could mistake
        // for a real project. A REQUIRED series hard-fails instead, the remora-82c.1 contract: a
        // feature whose patches cannot land must stop the build, and "the project is not even in
        // this tree" is the strongest form of cannot-land. $np carries the verdict to every later
        // step of THIS series — callers join with " && " so it is one shell — each of which no-ops
        // on it exactly the way the $d/SKIP marker already short-circuits the per-file steps.
        if (requiredBy.contains(p.key)) {
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ np=; [ -d %1/.git ] || { echo \"FATAL: %2 targets "
                                           "project %3, which is not in this tree — the requested "
                                           "feature(s) %4 require the series, and a green build "
                                           "would ship them missing\" >&2; false; }; }")
                                .arg(proj, p.key, p.projectPath, requiredBy.value(p.key)));
        } else {
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ np=; [ -d %1/.git ] || { np=1; echo \"SKIP %2: "
                                           "project %3 is not in this tree — nothing to patch on "
                                           "this manifest\"; }; true; }")
                                .arg(proj, p.key, p.projectPath));
        }
        // the contract: patches apply onto a clean checkout. Drop leftover conflicts, staged
        // content and stray files from earlier runs (committed patches survive — the mbox
        // skip below recognizes them). Reset a given project only ONCE — a later patch sharing
        // the project must land on top of the earlier one's changes, not on a wiped tree.
        if (!resetProjects.contains(p.projectPath)) {
            resetProjects.insert(p.projectPath);
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ [ -n \"$np\" ] || { git -C %1 am --abort 2>/dev/null; "
                                           "git -C %1 reset --hard -q 2>/dev/null; "
                                           "git -C %1 clean -fdq 2>/dev/null; }; true; }")
                                .arg(proj));
        }
        // Empty list = glob the directory. Generated series (gen-lineage-backport.sh) run to
        // dozens of files and are refreshed wholesale on every upstream sync; enumerating them
        // in C++ would guarantee the two drift apart. Same idempotency contract as the explicit
        // path below: a commit already in history is skipped by its subject slug.
        if (p.patches.isEmpty()) {
            const QString dir = QStringLiteral("$d");
            // Same hard-fail for a globbed series: an EMPTY glob is the missing-file case for
            // generated series, and the loop below would just iterate zero times and report
            // success (bd remora-fgj.11).
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ [ -n \"$np\" ] || [ -f \"$d/SKIP\" ] || "
                                           "ls $d/[0-9]*.patch >/dev/null "
                                           "2>&1 || { echo \"MISSING PATCH SERIES: %1 found no "
                                           "NNNN-*.patch in $d — the installed patch tree is out "
                                           "of sync with this binary's registry; reinstall "
                                           "vendor/source-patches\" >&2; false; }; }")
                                .arg(p.key));
            // Union-merge the resource files that every Lineage feature appends to. Without this
            // the series conflicts on res/values/cm_strings.xml (touched by 27 of the 98 patches)
            // and ~24 patches are lost: measured 52/98 applied with the default driver, 76/98
            // with union. Written to the project's own git dir, not .gitattributes, so the
            // source tree stays pristine.
            // '%s', NOT '%%s' (bd remora-4ei.41). QString::arg does not collapse %% — only
            // QString::asprintf does — so '%%s' reached the shell verbatim, and a printf format
            // with NO conversion specification prints the format ONCE and drops its arguments:
            // info/attributes became the single line "%s" and the union driver was inert. It
            // looked fine because info/attributes lives under .repo/projects/<p>.git/, which
            // survives both `git reset --hard` and `repo sync`, so a hand-written copy from the
            // day the feature landed stayed on disk and kept working. A bare %s is safe here:
            // arg() only matches '%' followed by digits.
            // --absolute-git-dir, NOT --git-path (bd remora-eoy.12) — the SAME failure a second
            // time, masked the same way. `git -C <proj> rev-parse --git-path info/attributes`
            // prints the bare RELATIVE ".git/info/attributes", with nothing tying it to <proj>, so
            // the mkdir and the redirect landed in the SHELL's cwd instead. That cwd is the tree
            // ROOT, because repoBringupCmd does `cd <tree>` and every step after it shares the one
            // &&-joined shell. The result: a husk .git at the tree root holding only
            // info/attributes — which then broke build-marker recording, since `[ -d .git ]`
            // accepted it as a repository (bd remora-eoy.11). And again it LOOKED fine, because
            // the copy under .repo/projects/<p>.git/ from the day this worked survives both
            // `git reset --hard` and `repo sync`. Measured on the live tree: the correct file was
            // dated 07-25 10:52 while the root husk was being rewritten on every build since.
            // The absolute form is cwd-independent, and it FAILS on a non-repo instead of
            // manufacturing one.
            cmds << guarded(
                projectMarks, p.projectPath,
                QStringLiteral("{ [ -n \"$np\" ] || { "
                               "g=$(git -C %1 rev-parse --absolute-git-dir) && "
                               "a=\"$g/info/attributes\" && mkdir -p "
                               "\"$(dirname \"$a\")\" && printf '%s\\n' "
                               "'res/values/cm_strings.xml merge=union' "
                               "'res/values/lineage_config.xml merge=union' "
                               "'res/values/lineage_dimens.xml merge=union' "
                               "'res/values/lineage_arrays.xml merge=union' > \"$a\"; }; }")
                    .arg(proj));
            // A FAILED patch must leave NO trace. `git apply --3way` writes conflict markers
            // into the working tree even when it exits non-zero, so a naive
            // "apply || warn" loop silently produces a tree full of <<<<<<< markers that
            // cannot compile — and every later patch then applies onto that broken file.
            // Measured on the Settings backport: 19 failures left 17 files poisoned. So on
            // failure we hard-reset back to the last good commit, making the patch a clean
            // skip. Successful 3-way results are committed immediately so the reset can never
            // discard earlier work.
            const QString loop = QStringLiteral(
                        "for f in %2/[0-9]*.patch; do [ -e \"$f\" ] || continue; "
                        // EXCLUDE is honoured HERE, at apply time, not only when the series is
                        // generated. Generation-time exclusion alone has a hole: the build stages
                        // patches from the INSTALLED vendor tree, and cmake's install(DIRECTORY)
                        // copies without pruning — so deleting a patch from the repo leaves the
                        // installed copy in place and it is applied anyway. Measured: 0009 was
                        // removed from git and from the generated series, the fingerprint duly
                        // changed, and the next build still staged and applied the stale installed
                        // copy. Checking the list where the patches are CONSUMED closes that, and
                        // also covers remote builds, where the series is scp'd to the build
                        // host and can go stale independently.
                        // Deliberately NO `case ... in pat)` here, idiomatic though it would be: a
                        // case arm's ')' has no opener, and sourcePatchCommandsAreBalanced() counts
                        // parens across the whole command to catch malformed shell. Weakening that
                        // guard to fit this loop would trade a real check for a stylistic one, so
                        // the glob matching uses ${var##pattern} instead — empty result means the
                        // pattern consumed the whole string, i.e. a full-string glob match.
                        "b=$(basename \"$f\"); "
                        "if [ -f %2/EXCLUDE ]; then x=; "
                        "while IFS= read -r p; do [ -n \"$p\" ] || continue; "
                        "[ -z \"${p##\\#*}\" ] && continue; "
                        "[ -z \"${b##$p}\" ] && { x=1; break; }; done < %2/EXCLUDE; "
                        "[ -n \"$x\" ] && { echo \"backport patch EXCLUDED (see EXCLUDE): $b\"; "
                        "continue; }; fi; "
                        // Trailing dashes are stripped too: format-patch truncates the filename at
                        // 52 characters and can land mid-word, leaving a dangling '-' that %f does
                        // not reproduce (0078-...-pattern-visibility-settings-2-.patch vs a commit
                        // slug ending "-2"), which would defeat the prefix match below.
                        "s=$(basename \"$f\" .patch | sed 's/^[0-9]*-//; s/-*$//'); "
                        // PREFIX match, like the explicit-list path below — never -qx. $s comes
                        // from the FILENAME, which format-patch truncates to 52 characters, while
                        // %f expands the full subject slug. An exact match therefore fails for
                        // every patch whose subject is longer than that, i.e. most of the series,
                        // so the skip never fired and all 100 patches were re-attempted each run.
                        "git -C %1 log --format=%f 2>/dev/null | grep -q \"^$s\" && continue; "
                        "if git -C %1 am --keep-cr \"$f\" >/dev/null 2>&1; then :; else "
                        "git -C %1 am --abort >/dev/null 2>&1; "
                        "if git -C %1 apply --3way \"$f\" >/dev/null 2>&1; then "
                        "git -C %1 add -A && git -C %1 -c user.email=remora@local "
                        // Commit under the BARE slug. The skip above greps `git log --format=%f`,
                        // which re-slugifies the subject — so a "backport: $s" subject yields
                        // "backport-$s", never matches, and the patch is re-applied on every run.
                        // Measured on a real tree: 14 fallback commits for 10 distinct patches,
                        // Add-toggle-to-enable-ADB-root landing five times.
                        // NOT the cause of dedupe-res's output: 27 patches in this series touch
                        // res/values/cm_strings.xml, so the union driver produces duplicate entries
                        // within a single clean pass and dedupe-res runs regardless. Confirmed on a
                        // freshly reset tree — it still dropped 12 entries per locale.
                        "-c user.name=Remora commit -qm \"$s\"; else "
                        "git -C %1 reset --hard -q; git -C %1 clean -fdq; "
                        "echo \"WARN backport patch SKIPPED (conflicts): $s\"; fi; fi; "
                        "done")
                        .arg(proj, dir);
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ [ -n \"$np\" ] || { %1; }; }").arg(loop));
            // Union merge is textual and cannot see that it is merging XML: where two patches
            // carry the same block in their context it keeps both copies. That yields duplicate
            // resource names, which aapt2 rejects outright ("resource string/x already defined")
            // at packaging time, with an error that points nowhere near patch application.
            // Measured: 6 duplicates on the Settings series. Semantic fix, so it survives a
            // regenerated series in a way a diff-based fixup would not.
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ [ -n \"$np\" ] || "
                                           "python3 %2/../source-build/dedupe-res.py %1; }")
                                .arg(proj, patchRoot));
            continue;
        }
        for (const QString &f : p.patches) {
            const QString patch = QStringLiteral("$d/") + f;
            // HARD-FAIL on an absent patch file. Without this the file simply is not there, `git
            // am` fails to open it, the 3-way fallback fails too, and the chain prints
            // "WARN patch SKIPPED (conflicts)" — a warning, and a WRONG diagnosis — then builds a
            // green image without the patch. That is how statusbar_tuner_entry shipped enabled,
            // ticked in the GUI and listed in remorarc, but absent from the image: the binary had
            // been reinstalled and /usr/share/remora/vendor/source-patches had not (bd
            // remora-fgj.11). Callers join with " && ", so `false` stops the build here.
            // SKIP is honoured first — a series deliberately skipped for this Android version is
            // allowed to be incomplete.
            cmds << guarded(projectMarks, p.projectPath,
                            QStringLiteral("{ [ -n \"$np\" ] || [ -f \"$d/SKIP\" ] || [ -f %1 ] || "
                                           "{ echo \"MISSING "
                                           "PATCH: %2 expects %1 — the installed patch tree is out "
                                           "of sync with this binary's registry; reinstall "
                                           "vendor/source-patches\" >&2; false; }; }")
                                .arg(patch, p.key));
            // Only NNNN-*.patch files are mailbox patches; a working.patch in an am-style group
            // is still a plain diff (git am would die with "Patch format detection failed").
            const bool mbox = p.amStyle && f.startsWith(QLatin1Char('0'));
            // The last-resort arm, reached when git am AND the 3-way both fail. For a patch some
            // requested FEATURE depends on, the soft skip IS the bug: the echo exits 0, callers
            // join with " && ", and the build completes green hours later with the feature's code
            // absent from the image — dumpsys archaeology territory (bd remora-82c.1, and the
            // sensors incident of bd remora-4ei.53 arriving by the other door). So a required
            // patch fails HERE, named, before the build burns those hours; anything no feature
            // asked for keeps the warn-and-continue, which is what lets a series being ported
            // limp usefully. Both arms rewind identically — the tree hygiene is not the variable.
            // The SKIP-marker guard wraps this whole command, so a series a version deliberately
            // absorbs upstream never reaches either arm.
            const QString failArm =
                requiredBy.contains(p.key)
                    ? QStringLiteral(
                          "echo \"FATAL: patch %1 of %2 FAILED to apply — the requested "
                          "feature(s) %3 require it, and a green build would ship them missing. "
                          "Tree left clean; port the series for this Android version or disable "
                          "the feature\" >&2; false")
                          .arg(f, p.key, requiredBy.value(p.key))
                    : QString();
            if (mbox) {
                // Idempotency for commits: the format-patch file name is the commit's subject
                // slug (git log --format=%f), so a commit already in history is skipped even
                // when later patches sit on top of it (reverse-check can't see through those).
                QString slug = f;
                slug.remove(QRegularExpression(QStringLiteral("^\\d+-")));
                slug.chop(6);  // ".patch"
                // brace-wrapped: callers join commands with " && ", and a bare top-level ||
                // would otherwise swallow an earlier step's failure and run anyway
                // The 3-way fallback must COMMIT, for the same reason the plain-diff branch does:
                // it stages what it applies, and the next mbox patch on this project would then
                // die with "Dirty index: cannot apply patches". Without this the dirt accumulates
                // and every later mailbox patch in the project fails in turn — observed across
                // frameworks/base, where one fallback took the whole statusbar series down with it.
                // Commit under the patch's own slug so the idempotency check above still
                // recognises it on a re-run (git log --format=%f re-slugifies the subject, and a
                // slug is already in that form, so it round-trips).
                // A FAILED 3-way must leave NO trace: `git apply --3way` writes conflict
                // markers AND unmerged index entries even when it exits non-zero, so without
                // this the tree carries <<<<<<< into the compiler and every later patch applies
                // onto the wreckage. Observed live: gpu_config.sh left "U" and the build reported
                // only "apply failed". reset --hard is safe precisely because successful applies
                // above are committed, so it rewinds the failure and nothing else.
                const QString am = QStringLiteral(
                            "{ [ -n \"$np\" ] || [ -f \"$d/SKIP\" ] || { "
                            "git -C %1 log --format=%f 2>/dev/null | grep -q ^%3 || "
                            "git -C %1 am --keep-cr %2 || { git -C %1 am --abort 2>/dev/null; "
                            "{ git -C %1 apply --3way %2 && git -C %1 add -A && "
                            "{ git -C %1 diff --cached --quiet || "
                            "git -C %1 -c user.email=remora@local -c user.name=Remora "
                            "commit -qm %3; }; } || { git -C %1 reset --hard -q; "
                            "git -C %1 clean -fdq; %4; }; }; }; }")
                            .arg(proj, patch, slug,
                                 failArm.isEmpty()
                                     ? QStringLiteral("echo \"WARN patch SKIPPED (conflicts), "
                                                      "tree left clean: %1\"")
                                           .arg(slug)
                                     : failArm);
                cmds << guarded(projectMarks, p.projectPath, am);
            } else {
                // plain diff: already-applied ⇔ reverse applies cleanly.
                // Commit the result, because `git apply --3way` STAGES what it applies and a
                // later mbox patch on the SAME project then dies with "Dirty index: cannot apply
                // patches" — git am refuses to run against a dirty index, which is exactly what a
                // project carrying a plain-diff patch followed by a mailbox patch hits. Committing
                // also keeps the reset contract intact (buildResetCommands resets to HEAD, so
                // committed patches survive) and the reverse-check above still
                // recognises the change on a re-run.
                // Same failure contract as the mbox branch above: rewind on conflict so a
                // failed patch is a clean skip rather than a poisoned working tree.
                const QString diff =
                    QStringLiteral("{ [ -n \"$np\" ] || [ -f \"$d/SKIP\" ] || { "
                                   "git -C %1 apply --reverse --check %2 2>/dev/null || "
                                   "{ { git -C %1 apply --3way %2 && git -C %1 add -A && "
                                   "{ git -C %1 diff --cached --quiet || "
                                   "git -C %1 -c user.email=remora@local -c user.name=Remora "
                                   "commit -qm \"patch: %3\"; }; } || "
                                   "{ git -C %1 reset --hard -q; git -C %1 clean -fdq; %4; "
                                   "}; }; }; }")
                        .arg(proj, patch, p.key,
                             failArm.isEmpty()
                                 ? QStringLiteral("echo \"WARN patch SKIPPED (conflicts), "
                                                  "tree left clean: %1\"")
                                       .arg(p.key)
                                 : failArm);
                cmds << guarded(projectMarks, p.projectPath, diff);
            }
        }
    }
    return cmds;
}

QString aospKindConflict(const QString &sourceRef, const QStringList &enabledPatches) {
    QStringList why;
    // The ref schemes refAndroidVersion() parses: only lineage-<NN> is lineage-exclusive;
    // android-* tags are what an AOSP tree legitimately syncs (the retired upstream ref form
    // parses to nothing).
    if (sourceRef.startsWith(QLatin1String("lineage-")))
        why << QStringLiteral("source_ref '%1' names a LineageOS branch an AOSP tree cannot sync")
                   .arg(sourceRef);
    QStringList lp;
    for (const SourcePatch &p : sourcePatches())
        if (p.needs.lineageTree && enabledPatches.contains(p.key)) lp << p.key;
    if (!lp.isEmpty()) {
        const int n = lp.size();
        if (lp.size() > 4) {
            lp = lp.mid(0, 4);
            lp << QStringLiteral("…");
        }
        why << QStringLiteral("%1 enabled patch(es) need the LineageOS projects an AOSP tree "
                              "never syncs (%2)")
                   .arg(n)
                   .arg(lp.join(QStringLiteral(", ")));
    }
    if (why.isEmpty()) return {};
    return QStringLiteral(
               "source_kind=aosp contradicts this profile: %1. Set the source kind to LineageOS "
               "to build it as configured, or clear the lineage pieces if an AOSP build is "
               "really intended — building anyway would skip the manifest sync and every "
               "container-compat patch and produce the wrong image without saying so.")
        .arg(why.join(QStringLiteral("; ")));
}

// ---- incremental patch state ------------------------------------------------------------------

PatchStateFiles patchStateFiles(const QString &tree) {
    PatchStateFiles f;
    f.repoState = tree + QStringLiteral("/.remora-patch-state");
    f.stateDir = tree + QStringLiteral("/.remora-patch-state.d");
    f.skipMark = tree + QStringLiteral("/.remora-incremental");
    f.markDir = tree + QStringLiteral("/.remora-incremental.d");
    f.syncList = tree + QStringLiteral("/.remora-resync-list");
    return f;
}

QMap<QString, QString> patchProjectMarks(const QString &tree) {
    const PatchStateFiles f = patchStateFiles(tree);
    QMap<QString, QString> marks;
    for (const QString &proj : sourcePatchProjects())
        marks.insert(proj, f.markDir + QLatin1Char('/') + patchProjectSlug(proj));
    return marks;
}

namespace {

// Field-terminated, so "ab" + "c" can never hash the same as "a" + "bc".
void fpAdd(QCryptographicHash &h, const QByteArray &b) {
    h.addData(b);
    h.addData(QByteArrayLiteral("\x1f"));
}

QString fpDigest(QCryptographicHash &h) {
    return QString::fromLatin1(h.result().toHex().left(16));
}

// Paths are hashed VENDOR-RELATIVE so the digest survives an install-prefix change: a dev-tree
// build and an installed build of the same content must agree, or every switch forces a full pass.
void fpAddPath(QCryptographicHash &h, const VendorReader &read, const QString &relPath) {
    const QList<QPair<QString, QByteArray>> files = read(relPath);
    for (const QPair<QString, QByteArray> &e : files) {
        fpAdd(h, e.first.toUtf8());
        fpAdd(h, e.second);
    }
}

// "<slug>|<project path>|<fingerprint>" per project, space-separated for `for e in $RP_E`. No field
// can contain a space or a '|', so plain word splitting is enough.
QString fpEntries(const QMap<QString, QString> &projectFp) {
    QStringList out;
    for (auto it = projectFp.cbegin(); it != projectFp.cend(); ++it)
        out << patchProjectSlug(it.key()) + QLatin1Char('|') + it.key() + QLatin1Char('|')
                   + it.value();
    return out.join(QLatin1Char(' '));
}

// "<slug>|<project path>" per container-patched project, for `for e in $RP_C`. Two fields, not
// three: these projects have no input fingerprint of their own (their inputs ride the repo
// fingerprint), only the post-apply HEAD record the prologue verifies.
//
// THE SLUG HERE IS NOT patchProjectSlug(), AND THAT IS DELIBERATE (bd remora-taab). It must match
// apply-container-patches.sh's stamp_file(), which writes "cp-" + the project path with slashes
// turned to UNDERSCORES into the SAME state directory:
//     stamp_file() { echo "$STATE/cp-$(echo "$1" | tr '/' '_')"; }
// patchProjectSlug turns slashes into HYPHENS and adds no prefix, so while this used it the two
// components named the same project differently in one directory — and the prologue, which
// requires every file it finds to match a known slug, rejected all 24 container stamps as
// "unrecognised record" on EVERY build. That forced a full reset, sync and re-apply each time,
// which is precisely the work the incremental state exists to skip: a 39-minute build whose
// compile was only 1178 ninja steps. The stale names were not left over from an older Remora as
// the prologue's comment guesses; the shell script writes them back during every pass.
// The h- HEAD records the epilogue writes become h-cp-<underscored>, which the prologue's
// `n=${b#h-}` strip reduces back to this same slug, so all three agree.
QString cpEntries(const QStringList &projects) {
    QStringList out;
    for (const QString &p : projects) {
        QString stamp = p;
        stamp.replace(QLatin1Char('/'), QLatin1Char('_'));
        out << QLatin1String("cp-") + stamp + QLatin1Char('|') + p;
    }
    return out.join(QLatin1Char(' '));
}

}  // namespace

QString repoStateFingerprint(const QString &repoUrl, const QString &ref, bool pinned,
                             int androidVersion, const VendorReader &read) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    fpAdd(h, repoUrl.toUtf8());
    fpAdd(h, ref.toUtf8());
    fpAdd(h, QByteArray::number(androidVersion));
    fpAdd(h, pinned ? QByteArrayLiteral("pinned") : QByteArrayLiteral("unpinned"));
    // The container-compat patches belong HERE, not in a per-project fingerprint, even though they
    // are patches: they land on ~30 projects that have no source patches and therefore no per-project
    // record of their own, so the only safe response to a change is a global sync.
    fpAdd(h, QByteArrayLiteral("container"));
    fpAddPath(h, read, QStringLiteral("source-build/apply-container-patches.sh"));
    fpAddPath(h, read,
              QStringLiteral("source-build/container-patches/") + containerPatchSet(androidVersion));
    // Only the manifest that is actually used: a pinned build deletes the local manifests, so
    // hashing them anyway would force full passes for edits that cannot affect the tree.
    fpAdd(h, QByteArrayLiteral("manifest"));
    fpAddPath(h, read,
              pinned ? QStringLiteral("source-build/pinned-manifest-a%1.xml").arg(androidVersion)
                     : QStringLiteral("source-build/local_manifests"));
    return fpDigest(h);
}

QMap<QString, QString> patchProjectFingerprints(const QStringList &enabled, int androidVersion,
                                                const VendorReader &read) {
    QMap<QString, QString> out;
    for (const QString &proj : sourcePatchProjects()) {
        QCryptographicHash h(QCryptographicHash::Sha256);
        fpAdd(h, QByteArray::number(androidVersion));
        for (const SourcePatch &p : sourcePatches()) {
            if (p.projectPath != proj || !enabled.contains(p.key)) continue;
            fpAdd(h, p.key.toUtf8());
            // The DECLARED file list, not only the directory content: dropping a filename from the
            // registry while leaving the file in place is a real change to what gets applied.
            fpAdd(h, p.patches.join(QLatin1Char(',')).toUtf8());
            // Both the shared series and the per-version override are hashed. Which one wins is
            // decided in the shell on the build host, so C++ cannot know — and hashing both means
            // an edit to either forces the re-apply.
            fpAddPath(h, read, QStringLiteral("source-patches/") + p.dir);
            fpAddPath(h, read,
                      QStringLiteral("source-patches/%1-a%2").arg(p.dir).arg(androidVersion));
            // A globbed series is post-processed by dedupe-res.py, so its content is an input too.
            if (p.patches.isEmpty())
                fpAddPath(h, read, QStringLiteral("source-build/dedupe-res.py"));
        }
        out.insert(proj, fpDigest(h));
    }
    return out;
}

QStringList containerPatchProjects(int androidVersion, const VendorReader &read) {
    const QString set = containerPatchSet(androidVersion);
    if (set.isEmpty()) return {};
    const QString rel = QStringLiteral("source-build/container-patches/") + set;
    QSet<QString> projs;
    for (const QPair<QString, QByteArray> &e : read(rel)) {
        if (!e.first.endsWith(QLatin1String(".patch"))) continue;
        // e.first = "<rel>/<project path>/<file>.patch" — the same dir⇒project mapping
        // apply-container-patches.sh walks.
        const QString sub = e.first.mid(rel.size() + 1);
        const int slash = sub.lastIndexOf(QLatin1Char('/'));
        if (slash > 0) projs.insert(sub.left(slash));
    }
    QStringList out(projs.cbegin(), projs.cend());
    out.sort();
    return out;
}

QString buildPatchStatePrologue(const QString &tree, const QString &repoFp,
                                const QMap<QString, QString> &projectFp,
                                const QStringList &containerProjects) {
    const PatchStateFiles f = patchStateFiles(tree);
    // ONE brace group: callers join steps with " && ", and the first cut of the global version of
    // this step ended with a bare `rm -f X; if …; fi`, which splits the chain — everything after the
    // ';' ran even when an earlier step had failed.
    //
    // A matching input fingerprint is NOT enough to skip a project: the record describes what a
    // past build APPLIED, and the tree can lose that behind the bookkeeping's back — an interrupted
    // full pass, a reset that never reached its re-apply, a manual repo sync. That is bd
    // remora-bahe, measured twice: frameworks/base kept one container commit but none of its five
    // source patches, and the prebuilt GPU-driver project sat at the bare pin while the record
    // said applied, which shipped a duplicate iHD_drv_video into kati. So every skip is gated on
    // the project still sitting at the post-apply HEAD the epilogue recorded (h-<slug>); a mismatch
    // — or a record this state has never written — goes down the existing re-sync + re-apply road.
    // Container-patched projects carry no input fingerprint of their own (those inputs ride the
    // repo fingerprint), so the HEAD record is their ONLY per-project state, which is exactly why
    // $RP_C exists: without it a lost container patch had no record capable of contradicting the
    // "nothing was re-synced" verdict.
    //
    // Projects with no readable HEAD record the SENTINEL "no-git" instead (a SHA can never equal
    // it): hardware/remora/c2-va is staged, not synced, and has no git repo, so demanding a SHA
    // there would evict it from the fast path on every build. "Could not verify then, cannot
    // verify now" is the old input-only trust, kept exactly where nothing stronger exists.
    return QStringLiteral(
               "{ RP_E='%1'; RP_C='%9'; mkdir -p %2 %3 || exit 1; rm -f %3/*; : > %4 || exit 1; "
               "RP_FULL=; "
               "if [ -f %5 ] && [ \"$(cat %5 2>/dev/null)\" = '%6' ]; then :; else RP_FULL=1; fi; "
               // An unrecognised record means a project this build no longer manages, or a tree
               // written by a different Remora. Its patches cannot be reasoned about, so take the
               // safe path instead of guessing. h-<slug> HEAD records are recognised by the slug
               // they carry.
               "for q in %2/*; do [ -e \"$q\" ] || continue; b=${q##*/}; n=${b#h-}; k=; "
               "for e in $RP_E $RP_C; do [ \"$n\" = \"${e%%|*}\" ] && k=1; done; "
               "[ -n \"$k\" ] && continue; RP_FULL=1; "
               "echo \"patch state: unrecognised record $b — forcing a full pass\"; done; "
               "if [ -n \"$RP_FULL\" ]; then rm -f %2/*; "
               "echo 'patch inputs changed (or first build) — full reset, sync and apply'; "
               "else touch %7; "
               "for e in $RP_E; do s=${e%%|*}; r=${e#*|}; p=${r%%|*}; g=${e##*|}; ok=; "
               "if [ -f %2/\"$s\" ] && [ \"$(cat %2/\"$s\" 2>/dev/null)\" = \"$g\" ]; then "
               "hc=$(git -C %8/\"$p\" rev-parse HEAD 2>/dev/null) || hc=no-git; "
               "if [ -f %2/h-\"$s\" ] && [ \"$hc\" = \"$(cat %2/h-\"$s\")\" ]; then ok=1; "
               "else echo \"patch state: $p is not at its post-apply commit — the tree lost or "
               "rewrote its patches, re-applying (bd remora-bahe)\"; fi; fi; "
               "if [ -n \"$ok\" ]; then touch %3/\"$s\"; else rm -f %2/\"$s\" %2/h-\"$s\"; "
               // MEMBERSHIP IS project.list, NOT the existence of a .repo/projects/<p>.git store
               // (bd remora-ykhz). A store outlives its manifest entry: device/remora had one left
               // over from before the device tree became a STAGED project, so this test passed, the
               // path went on the sync list, and the build died on `error: project device/remora not
               // found` — repo cannot sync what its manifest does not contain. .repo/project.list is
               // what repo itself rewrites on every sync, so it is the only list that answers "is
               // this project in the CURRENT manifest".
               "if grep -qxF \"$p\" %8/.repo/project.list 2>/dev/null; then echo \"$p\" >> %4; "
               "else echo \"patch state: $p is in no manifest, so repo cannot reset it — its "
               "patches re-apply onto whatever is already committed there\"; fi; fi; done; "
               "for e in $RP_C; do s=${e%%|*}; p=${e#*|}; "
               "hc=$(git -C %8/\"$p\" rev-parse HEAD 2>/dev/null) || hc=no-git; "
               "if [ -f %2/h-\"$s\" ] && [ \"$hc\" = \"$(cat %2/h-\"$s\")\" ]; then :; "
               "else rm -f %2/h-\"$s\"; "
               // A project can be in BOTH sets, and the RP_E loop above has already run and may
               // have TOUCHED ITS SKIP MARK. If the container check now sends the project for a
               // re-sync, that mark is a lie: the reset wipes the enabled patches too, and the
               // apply pass would skip re-applying them. That is bd remora-bahe's exact shape, and
               // it is not hypothetical — it broke the first build that ever reached this branch.
               // external/v4l2_codec2 was re-synced here while its own enabled patch was marked
               // applied, so 0005-Add-VideoCodec-AV1 never went back on and c2-va failed to
               // compile against a VideoCodec enum that no longer had AV1. Invalidate every RP_E
               // record and mark for the same project, so the enabled patches re-apply with the
               // container ones. Harmless when the project is in no manifest: the container pass
               // re-applies there too, which moves HEAD and would invalidate the record anyway.
               "for x in $RP_E; do xs=${x%%|*}; xr=${x#*|}; xp=${xr%%|*}; "
               "[ \"$xp\" = \"$p\" ] && rm -f %3/\"$xs\" %2/\"$xs\" %2/h-\"$xs\"; done; "
               // Same project.list test as the RP_E loop above, and for the same reason.
               "if grep -qxF \"$p\" %8/.repo/project.list 2>/dev/null; then "
               "grep -qx \"$p\" %4 || { echo \"$p\" >> %4; "
               "echo \"patch state: $p is not at its container-compat post-apply commit — "
               "re-syncing (bd remora-bahe)\"; }; "
               "else echo \"patch state: $p is in no manifest — its container patches re-apply "
               "onto whatever is already committed there\"; fi; fi; done; "
               "if [ -s %4 ]; then echo \"re-sync needed for: $(tr '\\n' ' ' < %4)— changed inputs "
               "or tree drift; Soong keeps its analysis of the rest\"; "
               "else echo 'patches unchanged and the tree still carries them — skipping reset, "
               "sync and re-apply so Soong can reuse its analysis (delete %5 to force a full "
               "pass)'; "
               "fi; fi; }")
        .arg(fpEntries(projectFp), f.stateDir, f.markDir, f.syncList, f.repoState, repoFp,
             f.skipMark, tree, cpEntries(containerProjects));
}

QString repoSyncVerifyCommand(const QString &projects) {
    // -e so one bad project fails the forall; the check itself never trusts repo's own verdict.
    // Both rev-parses inside the guard chain: an unreadable HEAD or an unresolvable REPO_LREV is a
    // failure too, not a silent pass. REPO_LREV is the manifest revision (the pin's SHA under a
    // pinned manifest), verified live against repo v2.65.
    return QStringLiteral(
               "repo forall %1-e -j8 -c 'h=$(git rev-parse HEAD) && "
               "m=$(git rev-parse \"$REPO_LREV\") && [ \"$h\" = \"$m\" ] || "
               "{ echo \"sync verify: $REPO_PATH is at ${h:-unreadable} but the manifest wants "
               "$REPO_LREV — the sync did not deliver what it reported\"; exit 1; }'")
        .arg(projects.isEmpty() ? QString() : projects + QLatin1Char(' '));
}

QString buildTargetedSyncCommand(const QString &tree) {
    const PatchStateFiles f = patchStateFiles(tree);
    // `cd` on its own account: the global bring-up (which cds into the tree) did not run on this
    // path. LFS is pulled for the synced projects only — a force-sync can restore an LFS-backed
    // file to its 134-byte pointer, and the global path pays for `repo forall` over ~1200 projects
    // precisely because it cannot know which. --fail-fast and the verify carry bd remora-4ei.81:
    // a sync that failed must stop the build here, not surface three layers later.
    return QStringLiteral("if [ -f %1 ] && [ -s %2 ]; then cd %3 && "
                          "repo sync -c -j8 --fail-fast --force-sync --verbose $(cat %2) && "
                          "%4 && "
                          "repo forall $(cat %2) -c 'git lfs pull 2>/dev/null || true'; fi")
        .arg(f.skipMark, f.syncList, tree,
             repoSyncVerifyCommand(QStringLiteral("$(cat %1)").arg(f.syncList)));
}

QString buildContainerPatchCommand(const QString &tree, const QString &sourceBuildDir,
                                   int androidVersion) {
    const PatchStateFiles f = patchStateFiles(tree);
    // No set for this version → refuse, loudly, with the remedy. Never fall through to another
    // version's set: applying the A16 set to an A17 tree left conflict markers inside PID 1's C++
    // (bd remora-cbd), and the same shape is what bd remora-p4p forbids for the pinned manifest.
    const QString set = containerPatchSet(androidVersion);
    if (set.isEmpty())
        return QStringLiteral(
                   "{ echo \"no container-compat patch set for Android %1 — add "
                   "vendor/source-build/container-patches/android-%1.0.0 and register it in "
                   "containerPatchSet(), or build a version that has one\"; exit 1; }")
            .arg(androidVersion);
    const QString run = QStringLiteral("sh %1/apply-container-patches.sh %2 %1/container-patches/%3")
                            .arg(sourceBuildDir, tree, set);
    // Restricted to the force-synced projects when the global sync was skipped. Six of these
    // projects carry BOTH a container patch and one of ours (frameworks/base, frameworks/native,
    // system/core, build/make, external/v4l2_codec2 …), so a targeted sync drops
    // their container-compat commits together with ours and they must be re-applied. Restricting it
    // is not just an optimisation: re-running the script over an already-patched project is a code
    // path the script has never taken (a full sync always precedes it today), and it would rewrite
    // files Soong has already analysed.
    // The empty-syncList branch says so rather than vanishing (bd remora-4ei.82). "no project needed
    // re-patching" and "apply nothing" were the same silent code path, which is the wrong shape for
    // a step whose failure mode is a quietly under-patched image. The script itself now hard-fails
    // an outright empty pass; this line covers the legitimately-empty incremental case, where doing
    // nothing is correct but should still be visible in the log.
    return QStringLiteral("if [ -f %1 ]; then if [ -s %2 ]; then %3 $(cat %2); "
                          "else echo 'container-compat: no project was re-synced, so none needs "
                          "re-patching (incremental pass)'; fi; else %3; fi")
        .arg(f.skipMark, f.syncList, run);
}

QString buildPatchStateEpilogue(const QString &tree, const QString &repoFp) {
    const PatchStateFiles f = patchStateFiles(tree);
    // $RP_E and $RP_C come from the prologue — same shell, so they are still set. If they ever
    // were not, this writes the repo record and no project records, and the next build re-syncs
    // every project: wasteful, never stale, which is the direction this has to fail in.
    //
    // Beside each input fingerprint, the project's POST-APPLY HEAD (h-<slug>) — the record the
    // prologue verifies the tree against before trusting a skip (bd remora-bahe). Recorded for the
    // container-patched projects too: they have no other per-project state, and they are where the
    // bahe hole actually bit (a prebuilts project at the bare pin, record claiming applied).
    // A project with no readable HEAD records the "no-git" sentinel — see the prologue for why
    // that keeps the staged, git-less projects on the fast path instead of evicting them forever.
    return QStringLiteral("{ printf '%s' '%1' > %2 || exit 1; mkdir -p %3 || exit 1; "
                          "for e in $RP_E; do s=${e%%|*}; g=${e##*|}; "
                          "printf '%s' \"$g\" > %3/\"$s\" || exit 1; done; "
                          "for e in $RP_E $RP_C; do s=${e%%|*}; r=${e#*|}; p=${r%%|*}; "
                          "h=$(git -C %7/\"$p\" rev-parse HEAD 2>/dev/null) || h=no-git; "
                          "printf '%s' \"$h\" > %3/h-\"$s\" || exit 1; done; "
                          "rm -f %4 %5; rm -f %6/*; echo 'patch state recorded'; }")
        .arg(repoFp, f.repoState, f.stateDir, f.skipMark, f.syncList, f.markDir, tree);
}

}  // namespace remora
