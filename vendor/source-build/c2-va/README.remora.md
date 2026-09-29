# c2-va — VAAPI Codec2 hardware video for Remora

`hardware/remora/c2-va`: the HIDL `c2@1.2` Codec2 service that gives the container **hardware
video** through VA-API on the render node the deploy hands it — HEVC and AVC *encode* (HEVC is
what the mirror carries at full resolution), and AVC, HEVC, VP9 and AV1 *decode* for playback. The
VA driver name comes from the deploy (`androidboot.va_driver`, iHD when absent) and is applied by
the HAL itself in `VaapiDevice::Create()`.

The codec2 pipeline it serves is switched on by `ro.boot.use_remora_c2=1`, which Remora passes
whenever `use_codec2` resolves true (the default); `vendor/remora/remora.c2.rc` then enables
CCodec, but only when the kernel offers a DMA heap the C2 buffer pool can use.

With `host_decode=true`, decode can also run on the docker host: `remote/RemoteVideoDecoder.cpp`
sends a stream to the host decode helper named by `ro.boot.remora_decoder_socket`, and falls back
to the local VA decoder when there is none. See `vendor/host-prereqs/nvdec-vaapi/README.md`.

## Why this lives in the Remora repo

This component is Remora's own source, and **no repo manifest carries it**, so a freshly synced
tree simply does not have it — unlike everything under `container-patches/`, there is no upstream
base to patch against. It is therefore vendored whole and copied into the tree by
`stage-features.sh`. Pipeline order:

```sh
vendor/source-build/apply-container-patches.sh <TREE> vendor/source-build/container-patches/android-17.0.0
vendor/source-build/stage-features.sh          <TREE> vendor/source-build/features   # copies this tree in
vendor/source-build/do-build.sh                                                       # inside the builder
```

Its build dependencies are vendored as patches next door and applied by step 1:

- `container-patches/android-17.0.0/external/v4l2_codec2/` — the `VideoEncoder::handlesInputFormatConversion`
  hook plus the decoder/encoder fixes this component compiles against.
- `container-patches/android-17.0.0/external/libchrome/` — restores `c2-va` on libchrome's
  visibility list (A17 dropped it).
- `device/remora`'s `remora.mk` inherits `va.mk`, which supplies
  `PRODUCT_SOONG_NAMESPACES += external/v4l2_codec2 hardware/remora/c2-va`. **Without that import
  soong parses the `.bp` files but emits no modules**, and the build dies with
  `unknown target …-service-vaapi`.

## Codec status

| Codec | Decode | Encode |
|-------|--------|--------|
| H.264 / AVC | ✅ `c2.remora.vaapi.avc.decoder` | ✅ `c2.remora.vaapi.avc.encoder` (rank 261, deliberately behind HEVC; exists so h264-only consumers such as screenrecord work at full resolution) |
| HEVC / H.265 | ✅ `c2.remora.vaapi.hevc.decoder` | ✅ `c2.remora.vaapi.hevc.encoder` (the mirror path) |
| VP9 | ✅ `c2.remora.vaapi.vp9.decoder` (8-bit profile 0; profile 2 gated off — NV12-only pool) | — |
| AV1 | ✅ `c2.remora.vaapi.av1.decoder` (8-bit; 10-bit gated off for the same reason) | — |

The HEVC **decode** core (`vaapi/VaapiVideoDecoderHEVC.cpp`, bd `remora-zbt`) implements the full
stateless path: SPS/PPS parsing, `short_term_ref_pic_set` including inter-RPS prediction
(7-59/7-60), the slice-segment header, POC derivation (8.3.1), RPS→DPB resolution (8.3.2),
reference-list construction (8.3.4) and the VA picture/slice/IQ buffer fill. The codec-agnostic
machinery (VA context creation, internal surface pool, DPB bumping/reclaim, async output path)
lives in `vaapi/VaapiVideoDecoder.cpp` and is shared by every codec.

Validated on an Android 17 image on Intel UHD 770 graphics (xe kernel driver, iHD 24.3.4): a 720p
HEVC Main clip with B-frames and 3 reference frames decodes at full rate with zero parse/VA/RPS
errors and visually clean motion.

`tools/` holds two test programs: `remora-decode-test.cpp` decodes a stream through a *named*
Codec2 component and writes the frames out, so a decoder can be verified by frame content;
`va-prime-import-test.c` reproduces the HAL's `vaCreateSurfaces(DRM_PRIME_2)` import on the host.

## Things that cost real time — do not re-learn them

- **Module name is `android.hardware.media.c2@1.2-service-vaapi`**, *not* the `-64` suffixed name;
  `-64` is the installed binary variant, and `m …-64` fails with `unknown target`.
- **`#define ATRACE_TAG ATRACE_TAG_VIDEO` must precede `<utils/Trace.h>`** in every translation
  unit. Without it `ATRACE_TAG` defaults to `ATRACE_TAG_NEVER` and all `ATRACE_CALL()`s silently
  become no-ops — the decoder runs fine while appearing completely absent from `atrace`.
- **`media_codecs_c2_va.xml` is a `PRODUCT_COPY_FILES` entry**, so a module-only `m` does *not*
  refresh it under `out/`. Either do a full `m`, or copy it in through a root container (`out/` is
  root-owned).
- **`slice_data_byte_offset` is measured in RBSP space** (emulation-prevention bytes removed),
  counting from and including the 2-byte NAL header — while the slice data buffer handed to the
  hardware is the *original escaped* bitstream. `unescapeRbspTracked()` keeps an RBSP→escaped index
  map so `slice_data_num_emu_prevn_bytes` falls out as `origPos[p] - p`.
- **The iHD driver must be built with the xe backend and installed to `/vendor/lib64/dri`**, and
  the service `.rc` must point `LIBVA_DRIVERS_PATH` there. `va.mk` requests `iHD_drv_video` from
  `external/intel-media-driver`, and the `intel_media_driver_dri_path` source patch moves its
  install into `dri/` — without it the driver is built, shipped and never found.
- **Android init does not expand `${prop:-default}` in a service `setenv`.** That is why the driver
  *name* is applied in `VaapiDevice::Create()` rather than in the `.rc`: the `.rc` once set
  `LIBVA_DRIVER_NAME` that way, and libva received the literal string.
