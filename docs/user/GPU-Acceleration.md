# GPU Acceleration

This page explains how Android gets a real GPU: Intel and AMD render nodes directly, NVIDIA
through the Venus render server, and software rendering when there is no GPU. It also covers
hardware video decode and encode inside Android, and what the GPU means for the mirror's codec.

## Choose the mode

| `gpu_mode` | what renders | notes |
|---|---|---|
| `guest` (default) | software (SwiftShader) | works everywhere, needs nothing on the host |
| `host` | the docker host's GPU | the host's render node directly, or NVIDIA through Venus |

Both `bare` and `remote` targets support `gpu_mode=host`. On a `remote` target the GPU is the
docker host's, probed over ssh. Only `host` and `guest` are valid values.

## Intel and AMD

With `gpu_mode=host`, Remora probes the docker host's render nodes at each deploy and passes the
chosen one to the container. It never bakes a node path into the profile, because node numbers can
change between boots.

| key | use it to |
|---|---|
| `gpu_driver` | pick the GPU by driver name (`xe`, `i915`, `amdgpu`) on a multi-GPU host |
| `gpu_node` | name an exact node such as `/dev/dri/renderD128`. It wins over `gpu_driver` |

On a multi-GPU host, the nodes of the GPUs you did not select are hidden from the container, so
the Android side cannot land on a GPU nobody chose.

Remora then fills in the per-driver pieces. You can override each, but you rarely need to:

| setting | Intel (`xe`, `i915`) | AMD (`amdgpu`) | other drivers |
|---|---|---|---|
| `vulkan` (Vulkan driver) | `intel` | `radeon` | nothing, unless the driver is one Remora maps |
| `va_driver` (VA-API driver for video) | iHD, the image default | `radeonsi` | nothing: video runs in software |
| `gralloc` | `minigbm_intel` on `xe` | `cros` | the image default |

For a driver Remora does not map, it deliberately sets nothing, so that the gap shows up in
`remora check` instead of hiding behind a plausible wrong name. Set the key yourself if you know
the right value.

## NVIDIA through the Venus render server

On an NVIDIA host, Android renders on the proprietary driver by sending its rendering out to the
host: ANGLE, then Vulkan, then Venus inside the container, over a unix socket to a patched
virglrenderer render server on the host, and from there to the NVIDIA driver.

### Build and install the host half

```sh
remora venus-build
```

This clones and patches virglrenderer, compiles it, fetches the prebuilt guest libraries (pinned
by tag and SHA256-verified) and installs everything under `~/.local/share/remora/venus`, or
`$REMORA_VENUS_DIR`. It is a separate command, not part of `remora up`, because cloning and
compiling is not something a connect should do behind your back.

| option | meaning |
|---|---|
| `--dir <dir>` | install root, instead of the default |
| `--tag <version>` | guest-library release to use. The default is the pinned release, `v0.1.2`. `--tag latest` resolves the newest upstream release and reports how far it is from the pin |

The recipe needs `git`, `meson`, `ninja`, `pkg-config`, `curl`, `tar`, `sha256sum`, zstd support,
and the Vulkan and libdrm headers.

### What happens at deploy

`gpu_mode=host` turns Venus on automatically, but only when all three are present: an NVIDIA
render node, the render server, and a complete set of guest libraries. Otherwise the Intel, AMD
or software path is used unchanged. A partially populated library directory counts as absent,
because a missing library boots to a black screen instead of to an error.

| `venus` | effect |
|---|---|
| unset | automatic, as above |
| `false` | pinned off |
| `true` | forced on. The preflight then names whatever is missing |

Remora starts the render server in its own systemd scope, so it outlives the workspace, and
generates the mounts and properties the container needs.

**The image must carry the matching gralloc.** A deploy refuses to proceed if the image has no
`gralloc.minigbm_gbm_mesa.so`. Build the image with the `minigbm_gbm_mesa_vtest` source patch,
which is off by default, or set `venus=false`.

Under Venus, video does not travel over the render socket. It still uses the local VA-API node,
for example the Intel GPU on a machine that has both.

### Is it worth it

The project measured one profile with only `venus` toggled. At 1280x720 the two paths were
indistinguishable. At 3760x1992, Remora's default display size, frame times through NVIDIA stayed
at 5 ms from p50 to p99 with 1 ms of GPU time, the same as at 720p. Its own summary is that Venus
buys nothing on a small display and a great deal on a large one. These are figures from one
machine. More detail is in
[the Venus README](../../vendor/host-prereqs/venus-nvidia/README.md).

## Checking the GPU

```sh
remora check --gpu-host
```

The GPU rows appear automatically for a profile with `gpu_mode=host`. They never block a deploy.
They exist to explain what the deploy will render on.

| row | if it fails |
|---|---|
| host render node | no `/dev/dri/renderD*` with a readable driver was found |
| VA-API driver for the probed GPU | video falls back to software. Set `va_driver=` if a driver exists |
| NVIDIA render node present | load the proprietary driver |
| NVIDIA kernel module matches userspace | the driver was updated without a reboot. Reboot before deploying |
| Venus render server installed | run `remora venus-build` |
| Venus guest libraries complete | run `remora venus-build` |
| host frame encoder built | rebuild Remora with FFmpeg 7 or newer and the Vulkan headers |
| ffmpeg can encode HEVC on Vulkan | host encode needs an ffmpeg with Vulkan Video encode |
| Venus render server publishes frames | rebuild the render server with `remora venus-build` |

`remora status` prints a warning when a `gpu_mode=host` instance is in fact rendering in software.

## Software rendering

With `gpu_mode=guest`, or on a host with no GPU, Android renders with SwiftShader. Remora selects
no GPU for it. The hardware video encoders are not available in this mode, so the mirror streams
H.264 instead of H.265, and `remora plan` says so.

## Hardware video inside Android

Video decode and encode inside an instance run on the host GPU through **c2-va**, Remora's own
VA-API Codec2 HAL. Apps that decode through MediaCodec get hardware decoders, and the mirror's
default codec, h265, is encoded by it.

| codec | decode | encode |
|---|---|---|
| H.264 / AVC | hardware | software (the image's own encoder) |
| H.265 / HEVC | hardware | hardware |
| VP9 | hardware, 8-bit profile 0 | none |
| AV1 | hardware, 8-bit | none |

The software codecs stay installed as the fallback. Three conditions make the hardware path
active, and all are visible in `remora plan`:

1. The Codec2 pipeline is on: `use_codec2`, which is on by default.
2. There is a host GPU: `gpu_mode=host`.
3. The GPU has a VA-API driver that Remora maps, or one you name with `va_driver=`.

Every image built from Remora's device tree includes c2-va. The `hw_video_decode` build feature
names the `-hwc2` image variant, on which the mirror requests the HEVC encoder by name. Widevine L3
decrypts in software and is capped at SD, so the real benefit is on unencrypted video.

### Decoding on the host

NVIDIA's VA-API driver cannot run inside Android. `host_decode=true` decodes on the host instead:
Remora starts a helper on the docker host at each connect, after probing that the host can decode,
and Android's decoder sends it the compressed stream. If the helper is missing or does not answer,
decoding falls back to the local path, so a problem costs the optimisation and never the video. It
needs `gpu_mode=host` and the helper on the docker host. The Remora build compiles it when it
finds libva and FFmpeg's development files, and `make -C vendor/native host-decoder` builds it on
its own for another docker host. The helper uses whatever VA-API driver the host has, so it also
works on Intel and AMD hosts.

## What the GPU means for the mirror

| situation | mirror codec |
|---|---|
| Intel or AMD with VA-API encode | h265, encoded in hardware inside Android |
| `gpu_mode=guest` | falls back to h264 |
| a GPU whose VA-API driver cannot encode, such as NVIDIA's decode-only driver | falls back to h264, unless `host_encode` is on |

`host_encode=true` encodes the mirror on the host GPU instead of inside Android. It needs
`gpu_mode=host`, the Venus stack, and the host frame encoder. The `.deb` does not include that
encoder: see [Installation](Installation.md). More on these settings is in
[Mirroring](Mirroring.md#video-and-audio-settings) and
[HW-VIDEO-DECODE.md](../HW-VIDEO-DECODE.md).
