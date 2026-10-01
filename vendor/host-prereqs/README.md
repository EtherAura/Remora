# Host prerequisites

Host-side pieces that sit outside the container image: build recipes for things the docker host
(the `bare` machine, or the `remote` one) needs installed, host patches, and probes. Most are
operator actions — Remora documents and versions them, and `remora check` names what is missing,
but it does not change the host behind your back. The table says which is which.

| directory | what it is | who runs it |
|---|---|---|
| [`venus-nvidia/`](venus-nvidia/README.md) | Remora's virglrenderer patch series and `build-venus.sh`, the recipe for the NVIDIA Venus render server that gives the container real GPU acceleration on an NVIDIA host | **Remora**: `remora venus-build` runs the recipe; `bare` and `remote` then start the server and emit the container wiring themselves |
| [`macvlan-shim/`](macvlan-shim/README.md) | `remora-macvlan-shim`, the host interface that lets a `bare` host reach its own `network_mode=macvlan` containers | **Remora**, on every connect to a macvlan profile on `bare`; the script is for doing it by hand (`macvlan_host_route=false`) or inspecting it |
| [`mesa-android/`](mesa-android/README.md) | `build-mesa.sh`: Mesa cross-built for Android x86_64, the payload of the `mesa_source` build feature, plus the cross-file and `llvm-config` shim it needs | **Operator**, before a source build. A source build of `remora_x86_64` requires this payload and refuses without it |
| [`rezygisk/`](rezygisk/build-rezygisk.sh) | `build-rezygisk.sh`: builds ReZygisk's 64-bit binaries from the pinned upstream commit with Remora's carried patch (`features/zygisk_pif/upstream/`) and installs them into the `play_spoof` module | **Operator**, when staging or bumping the `play_spoof` stack |
| [`nvdec-vaapi/`](nvdec-vaapi/README.md) | the feasibility record, `vaexport-probe.c` and `decode-test-client.c` behind `host_decode=` (in-Android video decoded on the host GPU by `vendor/native/remora-frame-decoder`) | **Operator**, as diagnostics; the feature itself is wired and Remora runs the helper |
| [`ffmpeg-nvidia/`](ffmpeg-nvidia/README.md) | a host FFmpeg patch that stops `remora-frame-encoder` dying on a null Vulkan image view under the NVIDIA driver | **Operator** (a distro package patch); nothing depends on it being present |

Also a host prerequisite on `bare` for **Android 16 images only**, and **not** vendored here: the
`ashmem_linux` kernel module, in a build that loads on an IBT-enabled kernel. Android 17 images run
on memfd and need no module. `remora check` reports whether it is loaded when it is needed.
