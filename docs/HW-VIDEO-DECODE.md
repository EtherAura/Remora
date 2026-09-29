# Hardware video inside Android

Video decode and encode inside a Remora instance run on the host GPU through **c2-va**, Remora's own
VA-API Codec2 HAL. Its source is `vendor/source-build/c2-va/`; a source build copies it into the
Android tree at `hardware/remora/c2-va`. It serves two jobs:

- **Playback**: apps that decode video through MediaCodec (players, browsers, streaming apps) get
  hardware decoders.
- **The mirror**: the mirror's default codec, h265, is encoded by c2-va's HEVC encoder. The image
  ships no software HEVC encoder, which is why the codec falls back to h264 on a GPU without VA-API
  encode.

## Components

| codec | decode | encode |
|---|---|---|
| H.264 / AVC | `c2.remora.vaapi.avc.decoder` | software: the image's `c2.android.avc.encoder` (c2-va lists an AVC encoder but does not implement it, so the mirror pins the software one) |
| H.265 / HEVC | `c2.remora.vaapi.hevc.decoder` | `c2.remora.vaapi.hevc.encoder` |
| VP9 | `c2.remora.vaapi.vp9.decoder` (8-bit, profile 0) | — |
| AV1 | `c2.remora.vaapi.av1.decoder` (8-bit) | — |

The hardware components rank ahead of AOSP's software codecs, which stay installed as the fallback
for formats and conditions the hardware path does not cover.

## How it gets into an image

c2-va is a native Codec2 service built against the exact platform it runs on, so it is compiled with
the image and cannot be layered onto an existing one. Every image built from Remora's device tree
includes it. The **`hw_video_decode`** build feature names the `-hwc2` image variant (for example
`remora24:x86_64-gapps-wv-hwc2`): the image registry records which tags carry hardware video, and on
a `-hwc2` image the mirror requests the HEVC encoder by name rather than leaving the choice to
Android. Build it with `remora build --source`, or *Build from source…* on the Image page; see
[FEATURES.md](FEATURES.md) §5.

## When it is active

Three runtime conditions, all visible in `remora plan` (the docker arguments it prints carry the
boot properties):

1. **The Codec2 pipeline is on.** `use_codec2` (default on) emits `androidboot.use_remora_c2=1`,
   which starts the service. An image without the HAL ignores it.
2. **There is a host GPU.** Use `gpu_mode=host`. With the default `gpu_mode=guest`, Android renders
   in software and Remora selects no GPU for it.
3. **The GPU has a VA-API driver.** The service loads the driver named by `va_driver`, which the
   resolver fills from the probed GPU: iHD (the image default) for Intel `xe` and `i915`,
   `radeonsi` for `amdgpu`. For any other driver it emits nothing, and `remora check --gpu-host`
   shows the *VA-API driver for the probed GPU* row failing — video then runs in software. Set
   `va_driver=` yourself if the GPU has a driver Remora does not map.

On a GPU whose VA-API driver has no encode entrypoint — NVIDIA's is decode-only — an h265 mirror
cannot start its encoder, so the resolver switches the mirror to h264 and says why in `remora plan`.
`host_encode` is the alternative there: it encodes the mirror on the host GPU instead (see
[FEATURES.md](FEATURES.md) §6).

## Decoding on the host (`host_decode`)

NVIDIA's VA-API driver cannot run inside Android at all: it loads NVIDIA's proprietary userspace
libraries, which are built for the host's C library rather than Android's. `host_decode=true`
routes around that by decoding on the host instead:

- Remora starts `remora-frame-decoder` on the docker host at each connect, after its own `--probe`
  confirms the host can decode and export frames, and bind-mounts its socket directory into the
  container.
- c2-va's remote decoder sends the compressed stream over that socket and receives decoded frames
  back — as shared GPU buffers where the consumer can import them, or copied into shared memory
  otherwise.
- If the helper is missing or its socket does not answer, c2-va falls back to its local VA-API
  decoder. A problem here costs the optimisation, never the video.

It needs `gpu_mode=host` and the helper built on the docker host:
`make -C vendor/native host-decoder` (requires the ffmpeg development headers). The helper is
driver-agnostic — it uses whatever VA-API driver the host has — so it works on Intel and AMD hosts
as well as NVIDIA. Background and the host-side verification tools are in
[vendor/host-prereqs/nvdec-vaapi/README.md](../vendor/host-prereqs/nvdec-vaapi/README.md).

## Scope

- **Clear content first.** Widevine L3 decrypts in software into ordinary buffers — there is no
  TEE and no protected video path — so nothing about L3 blocks a hardware decoder, but L3 is also
  capped at SD, so the gain on protected streams is small. The real benefit is on unencrypted video.
- **Software codecs remain** as the fallback on format switches and decoder initialisation
  failures.
