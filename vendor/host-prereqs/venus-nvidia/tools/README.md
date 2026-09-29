# Diagnostic reproducers (bd remora-e5x.34 / e5x.35)

Standalone Vulkan programs against the host NVIDIA driver — no guest, no
container, no venus. Build: `gcc -O1 -o nv12chain nv12chain.c -lvulkan`.

**nv12chain.c** — the whole camera/video preview chain in one process:
allocate what vtest allocates (R8 linear blob, host-visible sysmem, dedicated,
exported), CPU-write a self-describing pattern, re-import the fd as the
guest's NV12 image, GPU-copy the planes back out. On driver 5xx/RTX 3070 Ti
it shows the defect patch 0025 routes around: the dedicated multi-planar
import reads plane 1 as ZEROS, a single-plane re-import walks pages in
REVERSE order (`r0<-src@548864, r1<-src@544768, ...`), and the kernel logs
`intermapRegisterDmaMapping: Failed to insert new mapping node` +
gpu_vaspace.c asserts per attempt. The ALIAS case (bind the consumer image to
the producer's own VkDeviceMemory, no re-import) reads every byte correctly —
proving the explicit plane layouts are honored and ONLY the sysmem re-import
is broken. VRAM-backed buffers re-import fine, which is why RGB app surfaces
never showed this. This file is the intended attachment for an NVIDIA report.

**nv12layout.c** — creates linear NV12 with explicit plane layouts (the exact
camera geometry: p0[0:800] p1[480000:800]) and asks the driver to state the
layout back per plane. It matches verbatim, ruling out layout normalization —
useful as the control experiment beside nv12chain.

**gbmchain.c** — the e5x.35 feasibility matrix: the same producer/consumer
chain with the allocation swapped from a Vulkan sysmem export to GBM on
nvidia-drm. Build: `gcc -O1 -o gbmchain gbmchain.c -lgbm -ldrm -lvulkan`.
On driver 610.43.03/RTX 3070 Ti it proves the fix pair patch 0026 ships:
a GBM blob's single-plane re-import is byte-perfect (where sysmem walked
pages in reverse), its dmabuf mmap()s, the fd is the allocation's sole owner
after `gbm_bo_destroy` — and the NV12 import over it is STILL zero-chroma at
the guest's tight UV offset (480000) but byte-perfect at a 64 KiB-aligned one
(524288), matching NVIDIA's own native NV12 layout (UV at 655360). nv12chain
mode 5 ("dedicated ALIGNED") is the control showing alignment alone does not
rescue the sysmem export. NOT reproduced by gbmchain: iHD refusing DRM_PRIME_2
import of nvidia-GBM dmabufs (cross-GPU, measured live) — which is why 0026
routes only R8 spoofed blobs through GBM and leaves the real-fourcc linear
class (codec RGBX/RGB buffers the Intel VA driver imports) on the sysmem path.
