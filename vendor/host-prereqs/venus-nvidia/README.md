# Venus-on-NVIDIA host renderer

The host half of GPU-accelerated Android containers on an **NVIDIA** GPU: a patched
[virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) `virgl_test_server` that
serves Vulkan over a unix socket to Mesa **Venus** inside the container. Android renders through
ANGLE → Vulkan → Venus → the socket → this server → the proprietary NVIDIA driver.

Unlike the rest of `host-prereqs/`, this one **is** wired into the runtime end to end:

```bash
remora venus-build
```

clones and patches virglrenderer, compiles it, fetches the guest libraries and installs the lot;
after that the `bare` and `remote` backends detect it, start the render server, generate the
`/vendor/build.prop` overlay and emit every mount themselves. The recipe is
[`build-venus.sh`](build-venus.sh) in this directory — the sections below document what it does,
and remain the manual runbook.

Deliberately a separate command rather than part of `remora up`: it clones two repositories and
compiles virglrenderer, which is not something a connect should do behind your back. `--dir`
overrides the install root and `--tag` the guest-library release.

## Install layout Remora looks for

`$REMORA_VENUS_DIR`, or `~/.local/share/remora/venus` by default:

```
bin/virgl_test_server        # built below
bin/virgl_render_server      # built below
lib/libvirglrenderer.so*     # built below, with its soname symlinks
guest-libs/vulkan.virtio.so
guest-libs/libgbm_mesa_wrapper.so
guest-libs/libEGL_angle.so
guest-libs/libGLESv2_angle.so
guest-libs/libGLESv1_CM_angle.so
```

The five `guest-libs/` files come from the prebuilt guest-library release `build-venus.sh` fetches
(pinned by tag, SHA256-verified). Only `gralloc.minigbm_gbm_mesa.so` is inherited from the image,
because that one **is** ours (the `minigbm_gbm_mesa_vtest` source patch). The probe treats a **partially** populated `guest-libs/`
as unavailable rather than half-enabling the stack, since a missing library boots to a black
screen instead of to an error.

> **`vulkan.virtio.so` must be overridden, not inherited.** The image ships a `vulkan.virtio.so` of
> its own (~1.2 MB) and it is *not* equivalent to the prebuilt Venus ICD (~29.7 MB). The in-image
> one reaches NVIDIA and renders correctly — `dumpsys SurfaceFlinger` prints an identical-looking
> NVIDIA/Venus GLES string — but **capture is dead**: `screencap` hangs and the mirror never receives
> a frame, so it presents as a mirroring bug rather than a wrong library. The give-away is the
> version in that same string: the prebuilt reports `Vulkan 1.3.341 … venus-26`, the in-image copy
> `Vulkan 1.1.274 … NVIDIA-24.0.0.8`. A/B on two containers differing only in this mount: 275
> frames recorded with the prebuilt, 0 with the image's.

## How Remora uses it

`gpu_mode=host` on `bare`/`remote` turns Venus on **automatically**, but only when all three of an
NVIDIA render node, `bin/virgl_test_server` and a complete `guest-libs/` are present; otherwise the
xe/SwiftShader path is unchanged. `venus=false` in `remorarc` pins it off; `venus=true` forces it
on and lets the preflight name whatever is missing. Remora then:

- starts the render server (in its own transient systemd scope, so it outlives the GUI) with
  `VTEST_MAPPABLE_GPU=1`, and waits for the socket rather than the pid;
- generates `/vendor/build.prop` as *the image's own copy* plus the properties below, so nothing
  else in that file goes stale;
- emits the socket, library and build.prop mounts, both render nodes, and
  `androidboot.remora_gralloc=minigbm_gbm_mesa`.

It refuses to proceed if the image has no `gralloc.minigbm_gbm_mesa.so` — a stock image ships the
Venus ICD but not that gralloc, and the boot arg would name a HAL that does not exist. Rebuild with
the `minigbm_gbm_mesa_vtest` source patch, or set `venus=false`.

Overriding `venus_lib_dir` / `venus_server` / `venus_socket_dir` in `remorarc` bypasses the layout
above if you keep the artefacts elsewhere.

## What this directory contains

| file | what it is |
|---|---|
| `build-venus.sh` | the whole recipe below, automated — what `remora venus-build` runs |
| `NNNN-*.patch` | the Remora patch series, applied in numeric order |

`build-venus.sh` applies every `[0-9][0-9][0-9][0-9]-*.patch` here by glob, so a new patch is
picked up by dropping it in with the next number — do not re-list them here, the list went stale
once already. Order matters: `0002` fixes a bug that only exists because of `0001`, and later
patches build on earlier ones.

`0003`-`0014`, `0017`-`0019` and `0027` are the render server's retired **frame interception**:
it used to publish composed frames to the host encoder, until Remora's own hwcomposer became the
only frame source. They are inert at runtime — the publisher only starts when
`REMORA_FRAME_SOCKET` is set, and Remora never sets it — and they stay in the series until they
can be removed with the camera, screenshot and memory-pressure validation that removal needs,
because four rendering patches were written on top of them.

Two worth knowing about when debugging:

| file | what it is |
|---|---|
| `0001-vtest-serve-mappable-buffers-as-GPU-images-not-udmab.patch` | serve mappable buffers as GPU images, not udmabufs — SurfaceFlinger cannot texture a udmabuf |
| `0015-venus-an-out-of-memory-allocation-must-not-kill-the-.patch` | an out-of-memory allocation returns a Vulkan error instead of tearing down the ring for every client (bd remora-e5x.24). Carries `VKR_REMORA_FORCE_ALLOC_FAIL=N` to test that path without filling the card |

Everything else — the base commit and four upstream patches — comes from the same pinned release
repository `build-venus.sh` fetches (`WDNV_URL` in the script) and is **not** duplicated here.
`<release>` below is a checkout of it at the pinned tag.

## Building

```sh
git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git && cd virglrenderer
git checkout dc35e4d                                   # the release's pinned BASE
git am      <release>/patches/virglrenderer/0001..0003 # sync_file export, dmabuf blob, backlog
git apply   <release>/patches/virglrenderer/0004-wip-gpu-alloc-and-global-priority.patch
install -m644 <release>/src/virglrenderer-vtest/vtest_gpu_alloc.{c,h} vtest/
for p in <remora>/vendor/host-prereqs/venus-nvidia/[0-9]*.patch; do git apply "$p"; done
meson setup build -Dvenus=true -Drender-server-worker=auto && ninja -C build
```

Build deps are meson, ninja, pkg-config, libdrm and a Vulkan SDK — all already present on a
machine that builds Mesa. Produces `build/vtest/virgl_test_server` and
`build/server/virgl_render_server`.

## Running

```sh
RENDER_SERVER_EXEC_PATH=<build>/server/virgl_render_server \
LD_LIBRARY_PATH=<build>/src \
<build>/vtest/virgl_test_server --venus --multi-clients \
    --socket-path "$XDG_RUNTIME_DIR/remora-venus/venus.sock"
```

`VTEST_MAPPABLE_UDMABUF=1` restores upstream's allocation behaviour (see below).

## Why the patch exists

Upstream routes CPU-mappable gralloc buffers to `memfd`+`udmabuf`. That is correct for **its**
consumer — a hwcomposer that hands the dma_buf straight to KWin — but a Remora container composites in
**SurfaceFlinger via ANGLE**, and ANGLE cannot import a udmabuf as a texture. Every such buffer
costs one `Failed to create a valid texture` abort and takes SurfaceFlinger with it.

The patch serves those buffers as a Vulkan image with an explicit LINEAR modifier instead. The
memory type is unchanged (still host-visible system memory); what matters is that it is a
*dedicated allocation bound at offset 0*, which NVIDIA's RM will export as a dma_buf. A raw
udmabuf it refuses: `NVRM: RM is not supporting sg->offset != 0 use case now.!`

Measured A/B on an RTX 3070 Ti (nvidia-open 610.43.03), identical image, mounts, properties and
workload, only the routing differing:

| server | texture failures | SF crashes | frames | screencap |
|---|---|---|---|---|
| upstream prebuilt | 18 | 18 | 2 | hangs |
| this tree, udmabuf routing | 5 | 5 | 1 | hangs |
| **this tree, GPU-image routing** | **0** | **0** | **21** | **correct pixels** |

The failure count matched the udmabuf allocation count exactly, 1:1.

## Guest side — what must pair with this

The container needs all of the following, or it will fail in ways that look unrelated:

- `gralloc.minigbm_gbm_mesa` — built by the `minigbm_gbm_mesa_vtest` source patch
  (`vendor/source-patches/external-minigbm-gbm-mesa/`), selected with
  `androidboot.remora_gralloc=minigbm_gbm_mesa`
- upstream's `vulkan.virtio.so`, `libgbm_mesa_wrapper.so` and the three ANGLE libraries
- the socket directory bind-mounted at `/dev/venus`
- properties: `ro.hardware.vulkan=virtio`, **`ro.hardware.egl=angle`**,
  `mesa.vtest.socket.name=/dev/venus/venus.sock`,
  **`mesa.vn.debug=…,no_abort`**, `debug.hwui.renderer=skiavk`

Both bolded items are mandatory. Without `egl=angle`, NVIDIA-allocated buffers reach an *Intel*
RenderEngine and SurfaceFlinger aborts in `mapExternalTextureBuffer`. Without `no_abort`, Venus
kills the process on a recoverable error and SurfaceFlinger crash-loops. `ro.hardware.*` are
read-only properties, so both need a `/vendor/build.prop` overlay — `gpu_config.sh` would
otherwise pin `egl=mesa` and `vulkan=intel`.

## Validated end to end

`remora up` on a `bare` profile brings the whole stack up unattended and the guest reports:

```
GLES: Google Inc. (NVIDIA), ANGLE (NVIDIA, Vulkan 1.1.274
      (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 3070 Ti)), NVIDIA-24.0.0.8)
```

and the mirror receives it correctly — 286 H.265 frames recorded over 35 s at 1280x720, with
correct pixels. The image declares no VAAPI **AVC** encoder, so the mirror encodes H.265, which is
why the Intel render node is passed alongside the NVIDIA one: NVIDIA renders, Intel encodes.

## Is it worth it — measured

A/B through Remora itself: one profile, one fixed 14-swipe Settings workload, only `venus`
toggled. HWUI `gfxinfo` framestats, the same methodology as the 30 Hz work (`bd remora-bm7.1`).

| | NVIDIA / Venus | Intel xe / minigbm_intel |
|---|---|---|
| **@ 1280x720** — frame p50–p99 | 5 ms | 5 ms |
| **@ 1280x720** — GPU time | 1 ms | 2 ms |
| **@ 3760x1992** — frames / jank | 93 / **0 %** | 89 / 1.12 % |
| **@ 3760x1992** — frame p50–p95 | **5 ms** | 13 ms |
| **@ 3760x1992** — frame p99 | **5 ms** | 28 ms |
| **@ 3760x1992** — GPU time | **1 ms** | 13 ms |
| **@ 3760x1992** — high input latency | 0 | 2 |

At 720p the two are indistinguishable — the workload does not saturate either GPU, and the only
signal is a 2x in GPU time. The gap opens at the resolution Remora actually defaults to: Intel's
GPU time goes 2 ms → 13 ms while NVIDIA's stays at 1 ms, i.e. **NVIDIA's 4K numbers are identical
to its own 720p numbers**. So the honest summary is that this buys nothing on a small display and
a great deal on a large one.

## The "resolution ceiling" was a 256-byte pitch bug — fixed by `0002`

There is no resolution ceiling. It was never a bandwidth or capture-path limit: it was an
alignment bug in `0001`, and the failing dimension is the row **pitch**, not the size:

| resolution | pitch (w×4) | pitch % 256 | before `0002` | with `0002` |
|---|---|---|---|---|
| 1280x720 | 5120 | 0 | works | works |
| 1920x1080 | 7680 | 0 | works | works |
| **3760x1992** | 15040 | **192** | **Xid 69 → SurfaceFlinger aborts** | **works** |
| 3840x1992 | 15360 | 0 | works | works |

3840 working *before* the fix — while being larger than the 3760 that failed — is what ruled out
size as the variable and pointed at alignment.

The guest-side SIGABRT (`vn_ring_submit_locked` ← `vn_call_vkGetFenceStatus` ← ANGLE
`EGLSyncVk::initialize` ← `SkiaRenderEngine::drawLayersInternal`) is only a symptom. The cause is a
**host GPU fault**:

```
NVRM: Xid (PCI:0000:01:00): 69, name=vkr-ring-1, Class Error:
      channel 0x28, Class 0000c797, Offset 000019d0, ErrorCode 0000000f
```

`0xc797` is the Ampere_B 3D class and `vkr-ring-1` is virglrenderer's Venus ring thread — the
*render server* submitted invalid 3D-class state, losing its context, after which the guest's next
fence poll cannot succeed. `mesa.vn.debug=…,no_abort` does not cover a ring-submit failure. Note
that `dmesg_restrict=1` hides this on the host; the container's logcat carries the kernel log,
which is where it is visible.

Why only this path: `vtest_gpu_alloc_image()` reports whatever `layout.rowPitch` Vulkan returns,
whereas the udmabuf path beside it has always aligned (`ALLOC_ALIGN(width * bpp, 256)`). An A/B at
3760x1992 differing *only* in routing confirms it — `VTEST_MAPPABLE_GPU=1` faults,
`VTEST_MAPPABLE_UDMABUF=1` does not.

**Fix** (`0002`): pad the width for linear images so the driver's own pitch lands on 256, keeping
the real width and reported stride for the guest. Verified end to end at 3760x1992 through
`remora up` — Xid 0, no RenderEngine abort, the mirror receiving 3760x1992 frames. Any width
works now; `bd remora-4ei.66` is closed.

> **Do not stress-test the unaligned case casually** — i.e. do not run without `0002` to "see the
> bug". Provoking these faults repeatedly took this host down hard: the NVIDIA driver first logged
> a stack trace out of `nv_drm_gem_export_dmabuf_memory_ioctl` ("RM is not supporting
> sg->offset != 0"), and ~10 minutes later the machine locked up with an empty pstore and no oops.
> A GPU channel fault is not a contained failure on this driver.

## Known gaps

- The guest libraries are fetched, not built. They are Android cross-builds (ANGLE is a
  Chromium/gn build), so reproducing them locally would cost hours and a multi-GB toolchain to
  arrive at bytes upstream already publishes. Every download is SHA256-verified against the
  release's own `SHA256SUMS`, and the recipe installs the **64-bit** paths explicitly — the
  tarballs carry 32-bit copies under the same filenames.
- `remora venus-build` pins the guest-library release (`--tag`, default `v0.1.2`) and takes the
  virglrenderer base commit from that release's `patches/virglrenderer/BASE`. The pin is what makes
  the build reproducible, so it only moves deliberately: `--tag latest` resolves the newest
  upstream release, reports how far it is from the pin, and builds it — Remora's patches
  fail loudly at the apply step if the new base no longer fits them, at which point they need
  rebasing and the pin a considered bump.
