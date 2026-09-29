# vendor/remora

Remora's own vendor project — R2 of the reimplementation in bd remora-28ix.4. It is the source of
the container's init behaviour, GPU bring-up and native helpers, written rather than forked. Staged
into the tree as `vendor/remora` by `stage-device-tree.sh`, and installed by `vendor.mk`.

Everything here reads `ro.boot.remora_*` **directly** — it is Remora's code, so it reads Remora's
namespace rather than an adaptation of it.

## What lives here

| path | what it decides |
|---|---|
| `remora.common.rc` | what init does differently in a container: device-node modes, the `/data` overlay, masking `/proc/vmallocinfo` and `/sys/power/state`, memfd, densities |
| `gpu_config.sh` | which render node, gralloc, composer and EGL/Vulkan drivers the image uses (runs at early boot, before SurfaceFlinger) |
| `remora.props.rc` | the two boot properties that must be *derived* from a `ro.boot.remora_*` value rather than read as one: `ro.sf.lcd_density` and `ro.hardware.vulkan` |
| `remora.c2.rc`, `remora.c2.sh` | switch on the Codec2 pipeline when `ro.boot.use_remora_c2=1` — and only when the kernel offers a DMA heap the C2 buffer pool can use |
| `post-fs-data.remora.sh` | memfd fallback when the container has no ashmem node |
| `binder_alloc/` | `remora_binder_alloc`: creates the binder, hwbinder and vndbinder nodes in the container's empty binderfs |
| `ipconfigstore/` | `remora_ipconfigstore`: writes the framework's static-IP record for `eth0` from the live interface plus Remora's DNS/proxy boot args |
| `gralloc/` | `gralloc.remora`: the software gralloc for `gpu_mode=guest` (SwiftShader). Host mode uses the GPU gralloc the deploy names in `androidboot.remora_gralloc` |
| `hwcomposer/` | `hwcomposer.remora`: Remora's HWC2 composer HAL, the default composer. It forces client composition, paces vsync, and publishes the composed frame to the host encoder over `ro.boot.remora_frame_socket` |
| `vendor.mk` | what the product installs |

The four native modules carry Remora names — as module *and* installed name — because two modules
may not share an install path: a generic name is a hard kati error the moment any other project in
the tree defines the same module. The `init.rc` line that execs `remora_binder_alloc` moved with it, in Remora's
container-compat patch for `system/core`, and is called again from `remora.common.rc` in case that
patch fails to re-apply.

## `remora.props.rc` is not an adapter

Nothing in the image reads a boot property under any other name: `hwcomposer.remora` and
everything else here read `ro.boot.remora_*` themselves, so no key is mapped onto another
namespace. What `remora.props.rc` does is set a *different* property from the one it matches on:
`lcd_density` has to be set in the same early-init action (a setprop in one early-init action
cannot arm another), and the Vulkan ICD is a per-deploy fact, so it arrives as a boot arg and
lands here.

**It must stay byte-identical to `vendor/container-scripts/remora.props.rc`**, which Remora
bind-mounts into `/vendor/etc/init` on every deploy — the mount shadows this baked copy, so a
divergence would mean the image behaves one way alone and another way under Remora. A test
enforces the two files match.

Nothing translates `ro.kernel.*` boot args: Remora has never emitted them.
