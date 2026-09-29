# Host-side decode for in-Android video — feasibility record and test tools

The record behind `host_decode=`: in-Android video **playback** (YouTube, Pluto, browser) decoded
by a helper on the docker host — on NVIDIA, on **NVDEC**, the same GPU that composites — instead
of by the in-container Intel iHD driver on a different GPU. Encode is out of scope here and
impossible on NVIDIA's VA driver, which is decode-only.

**It is wired end to end** (bd remora-e5x.37). With `host_decode=true` on a `gpu_mode=host`
profile, the deploy checks the helper with `--probe`, starts `remora-frame-decoder` on the host,
bind-mounts its socket directory at `/dev/remora-decode` and passes
`androidboot.remora_decoder_socket`; c2-va's `remote/RemoteVideoDecoder.cpp` then sends each
stream there. At runtime a helper that is gone or refuses a stream costs nothing but the offload:
c2-va falls back to its local VA decoder, and a container not told the socket never tries. Default
off.

This directory holds the facts that design rests on, the probe that established the keystone one,
and the client that verifies the helper by frame content.

## Facts (i9-12900K + RTX 3070 Ti, driver 610.43.03)

1. **The container already has the full NVIDIA device-node set** — `/dev/nvidia0`, `nvidiactl`,
   `nvidia-uvm`, `nvidia-uvm-tools`, `nvidia-modeset`, `nvidia-caps/*`, and both render nodes
   (mapped in for Venus). Kernel-side, nothing blocks in-container NVDEC; the blocker is purely
   userspace: `libcuda`/`libnvcuvid` are proprietary glibc binaries that the VA driver `dlopen`s,
   so **no bionic port of nvidia-vaapi-driver can exist**. That blocker is terminal, and the
   design below routes around it instead.
2. **In-container decode, without the helper, is Intel**: the c2-va HAL binds `iHD_drv_video.so`
   on `renderD129` (verified via the HAL's maps/fds on the live container). Hardware, but on the
   wrong GPU — decoded frames must cross into the NVIDIA-composited UI, which is where a
   linear-path copy gets paid.
3. **Host NVDEC decode via VA-API works**: `LIBVA_DRIVER_NAME=nvidia` +
   nvidia-vaapi-driver 0.0.17 (direct backend) decodes h264 at 20.9× realtime on `renderD128`.
4. **KEYSTONE — decoded surfaces export as dma_bufs**: `vaExportSurfaceHandle`
   (`DRM_PRIME_2`, `SEPARATE_LAYERS`, read-only and read-write both) returns real per-plane fds:
   NV12, two objects, modifier `0x300000000606014` — the same NVIDIA block-linear family the
   mirror encoder's `import_frame` imports into Vulkan every day. `COMPOSED_LAYERS` is not
   supported (returns `invalid VASurfaceID`); nothing needs it.
5. **ffmpeg's `hwmap` VAAPI→DRM path is broken on this host and must not be relied on**: it
   returns ENOSYS *without reaching libva* — identically for the NVIDIA **and** the Intel iHD
   driver, so it is an ffmpeg build/dispatch quirk, not a driver property. Any helper calls
   `vaExportSurfaceHandle` itself (or decodes via CUDA/cuvid, which host ffmpeg also has).

## vaexport-probe.c

The 70-line probe that settled fact 4. Build and run:

```bash
cc -O1 -Wall vaexport-probe.c -lva -lva-drm -o vaexport-probe
LIBVA_DRIVER_NAME=nvidia ./vaexport-probe
```

Success looks like `vaExportSurfaceHandle[SEPARATE_LAYERS|READ_ONLY]: success` with two
`fd=` objects. It probes an undecoded surface, which is sufficient: allocation and export are
what it asks about, and the direct backend allocates backing images on create. Point it at other
hosts/drivers with `LIBVA_DRIVER_NAME`/the node path to collect new datapoints (on an AMD host:
`radeonsi`).

## Architecture

A **host decode helper** at the Codec2 level — the compose encoder's architecture run in
reverse. c2-va (Remora's own HAL) has a remote-decoder backend: bitstream out over a bind-mounted
unix socket (same kernel, `SCM_RIGHTS` fd passing — the compose publisher proves the pattern),
decoded frames back as fds. This kills both design blockers at once — no bionic port (the helper
is a host glibc binary), no one-VADisplay-per-process conflict (encode keeps the local Intel
display; decode is remote) — and it permanently disarms the `kRenderNodes` ordering trap, because
no in-container NVIDIA VA driver ever exists. On non-NVIDIA hosts the same helper is just a
different `LIBVA_DRIVER_NAME`, so the design generalises instead of special-casing a vendor.

## The helper: vendor/native/remora-frame-decoder.c

`make -C vendor/native host-decoder` (opt-in, like `host-encoder`; links ffmpeg + libva). One
client session at a time, access units in, frames out in a negotiated mode:

- **LINEAR** — the helper downloads NV12 into a memfd per frame and sends it with explicit
  offsets/pitches; c2-va mmaps and copies into its C2 block. Universally consumable, and
  **the mode c2-va uses**.
- **EXPORT** — `vaExportSurfaceHandle` dma_bufs, zero-copy. The client must `RELEASE(res_id)`
  when done: the helper holds the AVFrame reference until then, because the decoder recycles
  the surface memory the moment nothing references the frame. Implemented and tested, but not
  usable by c2-va — see *Where it landed*.

`--probe` answers "can this host hardware-decode h264 at all" (exit 0/1). The deploy runs it
before starting the helper and refuses `host_decode=true` when it fails, rather than silently
decoding somewhere else.

**Wire-format lesson learned the measured way**: the frame message's fds ride the first byte of
its `sendmsg`. A client that reads the 4-byte magic with plain `read()` and only then `recvmsg`s
the rest has already discarded the descriptors — the kernel closes them silently. Receive the
magic itself with a control buffer. The c2-va client honours this.

## decode-test-client.c — end-to-end host verification

Demuxes a real file into AUs (the same framing c2 will send), feeds the helper, validates the
replies. LINEAR mode is judged on **pixels**: `--ref` software-decodes the same file and
compares a Y-plane hash per frame. EXPORT mode validates descriptors and exercises the release
window. Verified on the NVIDIA host, all PASS:

```
h264 LINEAR:  sent=60 got=60 errors=0 ref_matched=60/60   (NVDEC, pixel-exact)
h264 EXPORT:  sent=60 got=60 errors=0 export_descriptors=ok
hevc LINEAR:  sent=60 got=60 errors=0 ref_matched=60/60
probe:        nvidia/renderD128 yes; iHD/renderD129 yes (driver-agnostic)
```

```bash
cc -O2 -Wall decode-test-client.c -o dtc \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil)
remora-frame-decoder --decode-socket /tmp/rmd.sock --driver nvidia --idle-timeout 60 &
./dtc --socket /tmp/rmd.sock --input clip.mp4 --ref       # LINEAR, pixel-compared
./dtc --socket /tmp/rmd.sock --input clip.mp4 --export    # EXPORT, descriptor-checked
```

## Where it landed

- **Container half**: `vendor/source-build/c2-va/remote/RemoteVideoDecoder.cpp`, reading
  `ro.boot.remora_decoder_socket`. h264, hevc and vp9 are content-verified as decoded remotely —
  frames byte-compared against an ffmpeg software reference on a live device. **AV1 is gated off
  the remote path** until a test clip verifies it the same way; it decodes locally meanwhile.
- **Engine**: `ensureHostDecoder` in `src/engine/Chain.cpp` — probe, spawn detached in its own
  scope, health judged on socket plus live process plus not-older-than-the-container. The helper
  exits on its own after an idle timeout with no session attached, so an abandoned one does not
  hold the GPU.
- **LINEAR, not EXPORT, is the design.** VAAPI decode surfaces export tiled on Intel and
  block-linear on NVIDIA, both drivers ignore a requested `DRM_FORMAT_MOD_LINEAR`, and the NVIDIA
  surface is two separate dma_bufs, which cannot map to a single gralloc block. The copy is not
  the bottleneck: for 60 frames of 720p h264, remote decode took ~1.28 s against ~1.62 s local.
  (Those timings were taken on the Intel node; the layout findings were taken against the NVIDIA
  node directly.)
- **Socket permissions matter.** The codec2 HAL runs as uid `media`, so the helper's socket must
  be writable by it; a 0755 socket refuses every connect, which the host encoder never showed
  because its consumer runs as `system`.
