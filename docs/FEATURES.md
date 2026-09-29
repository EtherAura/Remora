# Remora feature catalog

Every knob Remora has is optional and defaulted: leave it unset and the resolver applies the
proven default, so an empty profile still resolves to a bootable instance. Knobs come in two
stages:

- **Build features** are baked into the Android image. A profile lists them in `features=`; the
  GUI shows them on the **Image** page. Changing one means a different image.
- **Runtime settings** are `remorarc` keys applied at deploy or connect time. Most reach Android as
  `androidboot.remora_*` boot properties; a few configure the host side or the mirror window.

This page is written from the code, and the code wins where they disagree: build features are
`buildFeatures()` in `src/core/Features.cpp`, source patches are `sourcePatches()` in
`src/core/SourcePatches.cpp`, runtime keys are what `src/store/Store.cpp` reads and writes, and
their defaults come from `src/core/Resolver.cpp`. `remora plan` prints what a profile resolves to.

**Contents:** [1. Deploy targets](#1-deploy-targets) ·
[2. Host prerequisites](#2-host-prerequisites) · [3. Build features](#3-build-features) ·
[4. Source patches](#4-source-patches) ·
[5. From a feature selection to an image](#5-from-a-feature-selection-to-an-image) ·
[6. Runtime settings](#6-runtime-settings) · [7. Picture-in-picture](#7-picture-in-picture) ·
[8. Mirror navigation bindings](#8-mirror-navigation-bindings)

---

## 1. Deploy targets

There are two, and they run the same container with the same `docker run` arguments. They differ
only in where docker lives and in a few default paths.

| target | where docker runs | chosen when | adb target |
|---|---|---|---|
| **`bare`** | this machine | `ssh_host` is empty | `localhost:<host_adb_port>` |
| **`remote`** | any docker host reachable over key-based ssh | `ssh_host=user@host` is set | `<host>:<host_adb_port>` |

The target is inferred from `ssh_host`. An explicit `backend=bare|remote` (or `--backend` on the
CLI) overrides the inference; an unset or unrecognised value means `bare`.

`backend=vm` is **refused at preflight**. Remora no longer starts virtual machines: start the guest
yourself, then point a `remote` profile at it with `ssh_host=user@host`. Re-check `gpu_mode`,
`network_mode` and `host_adb_port` after switching — the old `vm` defaults differed from `remote`'s
on all three.

Both targets can render on a real GPU (`gpu_mode=host`): the docker host's own render node, or an
NVIDIA card through the Venus render server. The default is `gpu_mode=guest`, software rendering.

**Per-target defaults.** A profile created in the GUI is given its own container name
(`remora-<profile>`), data directory (`~/.remora-<profile>`) and the lowest free host adb port from
5555 upward, written into the profile. A hand-written profile that sets none of these gets:

| key | `bare` | `remote` |
|---|---|---|
| `container_name` | `remora-<profile>` | `remora-<profile>` |
| `data_dir` | `~/.remora-bm-data` | `~/remora-data` (on the remote host) |
| `data_base_dir` | `~/.remora-data-base` | `~/remora-data-base` (on the remote host) |
| `host_adb_port` | 5555 | 5555 |
| `network_mode` | `bridge` | `bridge` |

With `network_mode=macvlan` the adb target is the container's own LAN address on port 5556 (the
in-container adb forwarder), not a published host port.

---

## 2. Host prerequisites

Remora **probes** the host read-only and reports what is missing, with a remedy for each row; it
does not install anything. Run `remora check` (add `--gpu-host` for the GPU rows, `--backend
remote --ssh-host user@host` for a remote target). The host is ready when every *required* row
passes; the others are informational.

| row | applies to | severity | remedy |
|---|---|---|---|
| `adb` present | all | required | install android-tools |
| user in the `docker` group | bare | required | `usermod -aG docker $USER`, re-login |
| docker daemon reachable | bare | required | start docker, or unset a stale `DOCKER_HOST` |
| IBT-fixed `ashmem_linux.ko` loaded | bare | required | load the module |
| remote reachable over ssh | remote | required | key-based ssh to `ssh_host` |
| docker usable on the remote | remote | required | install docker, add the remote user to its group |
| host LAN readable | `network_mode=macvlan` | required | set `macvlan_parent/subnet/gateway`, or use `bridge` |
| `python3` on the docker host | `shared_inputs` set | optional | install python3 |
| host render node | `gpu_mode=host` | gpu | a `/dev/dri/renderD*` with a readable driver |
| VA-API driver for the GPU | `gpu_mode=host` | gpu | set `va_driver=` if one exists; otherwise video is software |
| NVIDIA render node / module matches userspace | `gpu_mode=host` | gpu | load the proprietary driver; reboot after a driver update |
| Venus render server / guest libraries | `gpu_mode=host` | gpu | `remora venus-build` |
| host frame encoder, ffmpeg `hevc_vulkan`, render-server frame publisher | `gpu_mode=host` | gpu | `make -C vendor/native host-encoder`; an ffmpeg with Vulkan encode; `remora venus-build` |

The host kernel must also provide binder (binderfs), which `remora check` does not probe.

For NVIDIA acceleration see [vendor/host-prereqs/venus-nvidia/README.md](../vendor/host-prereqs/venus-nvidia/README.md);
for reaching a macvlan container from its own host see
[vendor/host-prereqs/macvlan-shim/README.md](../vendor/host-prereqs/macvlan-shim/README.md).
Building images from source has its own host settings, listed in the README.

---

## 3. Build features

**Status**, as the Image page shows it: **OK** works; **CAVEAT** works, with the limitation in its
note; **MANDATORY** is always on (the checkbox is forced). The registry also knows **PARKED**
(selecting it blocks the build) and **DEFERRED** (warns); no current feature uses either.

`requires` is closed automatically when an image is selected (asking for `gapps` pulls in
`arm_translate`); `conflicts` blocks the combination.

| key | label | section | default | requires | conflicts | status |
|---|---|---|---|---|---|---|
| `lineage_frameworks` | LineageOS frameworks | Base system | on | | | OK |
| `container_compat_patches` | Container-compat patch set | Base system | on | | | MANDATORY |
| `mirror_agent` | In-image mirror agent | Base system | on for Android 17 | | | OK |
| `gapps` | Google Apps (Play Store/Services) | Apps and Google services | on | `arm_translate` | `microg` | OK |
| `microg` | microG (de-Googled Play services) | Apps and Google services | off | | `gapps` | CAVEAT |
| `arm_translate` | ARM app translation (native bridge) | Compatibility | on | | | CAVEAT |
| `system_updater` | In-Android system updater (host-applied) | Compatibility | off | patch `settings_hide_stock_updater` | | OK |
| `play_spoof` | Play spoof (Zygisk/PIF, per-app Pixel) | Compatibility | off | `magisk_root` | | OK |
| `webview_pairip_fix` | WebView PairIP x86_64 fix | Compatibility | on | `arm_translate` | | CAVEAT |
| `magisk_root` | Magisk root (su + DenyList) | Root and hiding | on | | | OK |
| `shamiko` | Shamiko denylist unmount | Root and hiding | off | `magisk_root`, `play_spoof` | | CAVEAT |
| `camera_v4l2` | Camera (external UVC/V4L2 provider) | Hardware and media | off | | | CAVEAT |
| `usb_audio` | Microphone (USB audio HAL) | Hardware and media | off | patch `usb_audio_policy` | | CAVEAT |
| `widevine_l3` | Widevine L3 DRM | Hardware and media | off | | | OK |
| `mesa_source` | Mesa from source (replaces the 24.0.8 prebuilts) | Hardware and media | off | | | OK |
| `hw_video_decode` | HW video decode (VA Codec2) | Hardware and media | off | | | CAVEAT |
| `sensors` | Sensors (accel/gyro, host-injectable) | Hardware and media | off | patches `sensors_hal`, `sensors_product` | | OK |

A "requires patch" is a source patch (§4) the feature needs in a from-scratch build. The Image
page enables it along with the feature; a hand-edited profile that lacks it gets a warning when the
build starts.

**What each one does**

- **`lineage_frameworks`** — LineageOS's framework parts on top of AOSP, rather than plain AOSP.
- **`container_compat_patches`** — binder device allocation, a stubbed SELinux, seccomp off and
  read-only property override: what lets Android boot in a container at all.
- **`mirror_agent`** — bakes the Remora agent into the image as a boot service. It is the device
  half of the mirror (capture, input, audio, app list), so an image without it cannot be mirrored.
  Default-on for Android 17. On 16 it is off by default because no registered Android 16 tag carries
  it; enable it for a 16 source build. See [MIRROR_AGENT.md](MIRROR_AGENT.md).
- **`gapps`** — Play Store and Play services. The GApps payload is arm64, hence the native-bridge
  requirement. One of the two features that can be layered onto an existing image (§5).
- **`microg`** — GmsCore, its Companion and the Aurora Store instead of GApps. Push, check-in,
  location and Aurora installs work; signature spoofing is not wired, so apps that verify the Play
  signature through the GMS client library still refuse. Source build only.
- **`arm_translate`** — the ARM native bridge that runs arm64-only apps on x86_64. The caveat is
  ABI-list ordering: an app can be started under the wrong ABI. The payload is proprietary.
- **`system_updater`** — adds *Settings → System → Remora Updater*; *Apply* signals the host, where
  `remora update-watch` does the update. Needs the patch that hides the stock *System update* entry,
  which would otherwise shadow it.
- **`play_spoof`** — bakes the ReZygisk + PlayIntegrityFork stack so Google Play sees a certified
  Pixel inside GMS and Play only; system properties and adb are untouched. Dormant until the
  `play_spoof=true` runtime setting turns it on. `remora certify` reports the result.
- **`webview_pairip_fix`** — a capability label inherited from earlier images. Remora carries no
  implementation of it: a source build installs nothing for it, and toggling it changes nothing
  about what a source build produces.
- **`magisk_root`** — upstream Magisk v30.7: `su` and the native DenyList unmount.
- **`shamiko`** — unmounts Magisk's modifications from denylisted processes. Staged into the
  play_spoof stack, hence the requirement. Shipped without upstream's `service.sh`, which would set
  `ro.debuggable=0` and `ro.adb.secure=1` and take adb with it. Shamiko is closed-source with no
  redistribution licence: supply the payload yourself, and do not share an image that contains it.
- **`camera_v4l2`** — AOSP's own external camera provider plus its config. The provider enumerates
  `/dev/video*` at runtime, so the container must also be given camera nodes: `camera_devices`
  (§6) does that, automatically when this feature is on. A camera plugged in later needs the
  container recreated.
- **`usb_audio`** — AOSP's USB audio HAL. Without it the microphone Android reports records
  perfectly timed silence from the stub HAL. The `usb_audio_policy` patch declares the module and
  the two only work together; a build with the patch but not the feature is refused, because it
  breaks all audio.
- **`widevine_l3`** — the Widevine L3 CDM (software DRM). L1 is impossible without a TEE. The other
  feature that can be layered onto an existing image.
- **`mesa_source`** — Mesa 26.x built for Android and installed as the image's GL and Vulkan
  drivers (about 120 MB larger). **Required for every source build**: the device tree has no
  prebuilt GL or Vulkan stack to fall back on, so `remora build --source` refuses a profile without
  it. Build the payload first with `vendor/host-prereqs/mesa-android/build-mesa.sh`. It stays
  default-off so the default feature set keeps resolving to a registered image tag.
- **`hw_video_decode`** — hardware video through Remora's VA-API Codec2 HAL
  (`c2.remora.vaapi.*`); selects the `-hwc2` image variant. Needs `gpu_mode=host` on a GPU with a
  VA-API driver. See [HW-VIDEO-DECODE.md](HW-VIDEO-DECODE.md).
- **`sensors`** — an accelerometer and gyroscope whose values the host injects; drives auto-rotate
  and `remora rotate`. Needs both sensors patches in a from-scratch build.

**Removed keys.** `native_vulkan` is gone (the Vulkan driver is now chosen per deploy — see
`vulkan=` in §6), as is `zygisk_hiding`. A removed key left in an old profile's `features=` is
dropped when the recipe is built.

**Payloads.** GApps, microG, the native bridge, Magisk, Widevine and the Zygisk/PIF stack are
third-party binaries the repository does not carry. `vendor/fetch-payloads.sh` reports what is
present and fetches the ones with a recorded, hash-verifiable upstream; the rest you supply. See
[vendor/PAYLOADS.md](../vendor/PAYLOADS.md).

---

## 4. Source patches

A source build also applies Remora's vendored patches (`vendor/source-patches/`) to the Android
tree. Each has a key, and the Image page lists them under the feature sections with a tooltip each.
`source_patches=` pins an explicit list; unset means the defaults for the profile's Android
version. A pinned list does not pick up defaults added later, and the build warns about each
default it leaves out.

| key | label | default |
|---|---|---|
| `minigbm_rendernode` | minigbm forced render node | on |
| `minigbm_gbm_mesa_vtest` | minigbm gbm_mesa/vtest gralloc (NVIDIA) | off — needs the Venus path |
| `minigbm_amdgpu_no_dri` | minigbm amdgpu: tolerate a Mesa-26 DRI loader (AMD hosts) | off — only with `mesa_source` |
| `minigbm_gralloc0_metadata` | minigbm: give gralloc0 buffers a metadata region | off |
| `venus_device_loss` | Venus: device loss instead of abort on a fatal ring | off |
| `v4l2_encoder` | v4l2_codec2 encoder path | on |
| `v4l2_av1_enum` | v4l2_codec2 AV1 codec enum | on |
| `intel_media_driver_dri_path` | iHD VA driver install path | on for 17 |
| `v4l2_framepool_null` | v4l2_codec2 frame-pool fixes | on |
| `libchrome_fix` | libchrome build fix | on |
| `buildprop_touch_rotation` | build.prop: emit RecoveryDefaultTouchRotation | on for 17 |
| `soong_gomemlimit` | Bound soong_build's Go heap | on |
| `lineage_platform_backport` | LineageOS platform backport (EXPERIMENTAL) | off — LineageOS tree only |
| `no_lineage_sepolicy` | Skip the LineageOS sepolicy | on for 17 |
| `lineage_backport_deps` | LineageOS backport prerequisites (frameworks/base) | on for 17 |
| `lineage_soong` | lineage soong config | on |
| `lineage_sdk_features` | Honest Lineage SDK feature set | on for 17 |
| `drop_lineage_updater` | Drop the LineageOS Updater app | on for 17 |
| `lineage_platform_res` | Load the LineageOS SDK resources | on for 17 |
| `surfaceview_secure` | SurfaceView secure capture | on |
| `shell_update_owner` | Shell can claim app update ownership | on |
| `statusbar_hide_list` | Status bar icon hide list (clock, battery, tuner entry) | on for 17 |
| `cursor_on_motion` | Mouse cursor only on motion | on |
| `submix_all_output` | Route all audio to the remote submix | on for 17 |
| `usb_audio_policy` | Declare the USB audio module in the audio policy | off — only with `usb_audio` |
| `keep_kernel_modprobe` | Keep host kernel.modprobe (module autoload) | on |
| `settings_hide_stock_updater` | Hide the stock Settings 'System update' entry | off — pairs with `system_updater` |
| `sensors_hal` | Sensors HAL (host-injectable accel/gyro) | off — pairs with `sensors` |
| `sensors_product` | Sensors HAL packaging (vendor APEX) | off — pairs with `sensors` |
| `boot_animation` | LineageOS boot animation (host-side) | off |

"On for 17" patches compensate for gaps in the Android 17 base and are not defaults on 16. The
LineageOS entries patch projects an AOSP tree (`source_kind=aosp`) does not have. The Image page
hides the patches that cannot apply to the current profile (no assets for its Android version, an
AOSP tree, a feature or Venus path it does not use), and ticking a feature there also ticks the
patches that feature requires.

---

## 5. From a feature selection to an image

**Android version.** `android_version=16` (LineageOS 23, the default when unset) or `17`
(LineageOS 24). Both build from the same Remora device tree and lunch the product
`remora_x86_64`. Versions 8–15 are named in the registry but have no build assets, so selecting one
is refused.

**Resolution**, in order:

1. An explicit `image_tag=` wins outright.
2. Otherwise the requested features — or the version's default set when `features=` is unset — are
   closed under `requires` and matched against the registered tags for that Android version: the
   tag missing the fewest requested features wins, ties going to the one with the fewest extras.
3. Whatever that tag still lacks decides what happens next. `gapps` and `widevine_l3` can be
   layered on as a Docker overlay: `remora recipe` prints the Dockerfile and `remora build` builds
   it. Anything else missing needs a **source build**: `remora build --source`, or *Build from
   source…* on the Image page.

**Registered tags:**

| tag | Android | provides |
|---|---|---|
| `remora23:x86_64` | 16 | arm_translate, webview_pairip_fix, lineage_frameworks, container_compat_patches |
| `remora23:x86_64-gapps` | 16 | the above + gapps, magisk_root |
| `remora23:x86_64-gapps-wv` | 16 | the above + widevine_l3 |
| `remora23:x86_64-gapps-wv-hwc2` | 16 | the above + hw_video_decode, sensors |
| `remora24:x86_64` | 17 | arm_translate, webview_pairip_fix, lineage_frameworks, container_compat_patches, magisk_root, sensors, system_updater, mirror_agent |
| `remora24:x86_64-gapps` | 17 | the above + gapps, play_spoof |
| `remora24:x86_64-gapps-wv` | 17 | the above + widevine_l3 |
| `remora24:x86_64-gapps-wv-hwc2` | 17 | the above + hw_video_decode |

With nothing set, an Android 16 profile resolves to `remora23:x86_64-gapps`, raised to
`remora23:x86_64-gapps-wv-hwc2` whenever the mirror codec is not h264 (the default codec is h265,
whose hardware encoder lives in that image). The Android 17 default set resolves to
`remora24:x86_64-gapps`.

**Source-built tags.** A source build is tagged
`remora<LineageOS major>:x86_64[-gapps][-microg][-wv][-hwc2][-<arch_variant>]-src` (an AOSP tree
builds `aosp-remora<android>:…`). *Promote to active* on the Image page drops the `-src`, which is
the name the resolver looks for. An `arch_variant` build lands under its own suffix, so a CPU-tuned
image lives beside the portable one.

A source build needs `source_tree` set, `mesa_source` enabled, and the payloads for the features it
bakes (§3). It warns about every default feature or patch a pinned profile leaves out, and about
each feature whose required patch is off.

---

## 6. Runtime settings

Settings live in `~/.config/remorarc` (INI). Each profile is an `[Instance-<name>]` section read over
`[Defaults]`, so a key in `[Defaults]` applies to every profile that does not set it; `[Remora]
active_instance` names the active one. The GUI writes all of this; hand edits are fine. List-valued
keys are space-separated unless noted. Deleting a key returns it to its default.

### Target and storage

| key | default | meaning |
|---|---|---|
| `backend` | inferred | `bare` or `remote` (§1) |
| `ssh_host` | unset | `user@host` of a remote docker host |
| `container_name` | `remora-<profile>` | the container; also how Remora finds a running instance |
| `data_dir` | §1 | host directory holding the device's `/data` (its private overlay layer by default) |
| `data_base_dir` | §1 | shared read-mostly base layer that profiles on one host stand on |
| `host_adb_port` | 5555 | host port published for adb |

### Display and system

| key | default | meaning |
|---|---|---|
| `width` / `height` / `dpi` | 3760 / 1992 / 320 | the device display |
| `fps` | 60 | display refresh rate |
| `max_fps` | unset; 60 when `fps` > 60 | cap on the mirror encoder's frame rate |
| `timezone` | the host's zone | IANA zone for Android |
| `use_memfd` | true | use memfd for shared memory |
| `ro_overrides` | none | `key=value` entries passed as `androidboot.ro.<key>=<value>` |
| `auto_sleep_minutes` | off | freeze the device after this long with no mirror attached; enforced by `remora auto-sleep` (installed by `remora service`) |

### GPU

| key | default | meaning |
|---|---|---|
| `gpu_mode` | `guest` | `guest` = software rendering (SwiftShader); `host` = the docker host's GPU |
| `gpu_node` | probed | exact render node, e.g. `/dev/dri/renderD128`; unset = the live probe's pick at each deploy, never a remembered one |
| `gpu_driver` | probed | choose the render node by driver name (`xe`, `i915`, `amdgpu`, …) on multi-GPU hosts; `gpu_node` wins over it |
| `gralloc` | per driver | gralloc HAL: `minigbm_intel` on xe, `cros` on amdgpu, `minigbm_gbm_mesa` under Venus, image default otherwise |
| `hwcomposer` | image default | composer HAL; Remora's own HWC2 HAL is the only one the image ships |
| `vulkan` | per driver | Vulkan ICD: `intel`, `radeon`, `virtio` (Venus), `broadcom`, `freedreno`, `panfrost`; `pastel` (software) in guest mode; nothing for an unmapped driver |
| `va_driver` | per driver | VA-API driver for the Codec2 HAL: iHD (image default) on Intel, `radeonsi` on amdgpu; nothing for an unmapped driver |
| `venus` | auto | NVIDIA via Venus: on when an NVIDIA render node, the render server and the guest libraries are all present; `false` pins it off |
| `venus_socket_dir` | `~/.cache/remora/venus` | render-server socket directory (per container when `host_encode` is on) |
| `venus_lib_dir` / `venus_server` | probed | guest libraries and render-server binary, as installed by `remora venus-build` |

### Mirror

| key | default | meaning |
|---|---|---|
| `video_codec` | `h265` | `h264`, `h265` or `av1`; falls back to h264, with the reason logged, on a host GPU with no VA-API encode |
| `video_bit_rate` | `30M` | mirror bit rate |
| `max_size` | 0 | longest side of the mirrored video; 0 = the display's full size |
| `audio` | off | mirror audio (opus, captured inside Android) |
| `hw_decode` | on | decode the mirror on this machine's GPU (NVDEC, then VA-API, then software); `false` forces software |
| `host_encode` | off | encode the mirror on the host GPU instead of inside Android; `gpu_mode=host` and the Venus stack only |
| `host_encode_max_size` | 0 | cap on the host encoder's output size, longest side |
| `host_decode` | off | decode in-Android video on the host GPU; `gpu_mode=host` only — see [HW-VIDEO-DECODE.md](HW-VIDEO-DECODE.md) |
| `window_width` / `window_height` / `window_x` / `window_y` | unset | mirror window geometry |
| `window_fullscreen` | off | open the mirror fullscreen |
| `fullscreen_separate_display` | off | *Fullscreen* opens a separate desktop-mode display instead of mirroring |
| `fullscreen_display` / `fullscreen_app` | main size / none | that display's `WxH[/dpi]`, and the app to start on it |
| `desktop_new_window` | off | the desktop shortcut opens a new window each press instead of replacing the last |
| `freeform_display` | `1080x2340/420` | size of the phone-shaped window apps open in |
| `mirror_extra` | none | extra flags passed to `remora mirror` |
| `pip_width` / `pip_height` | 480 / 270 | picture-in-picture size (§7) |
| `mouse_bind` / `shortcut_mod` / `key_bind` | §8 | navigation bindings |

The mirror window's frame belongs to the desktop: there is no border knob. For a frameless mirror,
add a *No titlebar and frame* window rule for the `remora` window class.

### Input devices

| key | default | meaning |
|---|---|---|
| `kbd_identity` | first host keyboard | which host keyboard the in-container keyboard claims to be (Gboard hides its toolbar for a generic one); `none` = generic |
| `shared_inputs` | none | host input devices shared with Android by name, non-exclusively (newline-separated) |
| `camera_devices` | auto | host `/dev/video*` nodes given to the container; auto = every probed node, only when `camera_v4l2` is enabled; an empty value = none |
| `cpufreq_topology` | on | show Android a phone-like CPU frequency topology; some apps crash on a desktop's |

### Network

| key | default | meaning |
|---|---|---|
| `network_mode` | `bridge` | `bridge` publishes the adb port; `macvlan` gives the container its own LAN address |
| `docker_network` | `lan` | macvlan docker network name |
| `macvlan_ip` | assigned by docker | fixed LAN address for the container |
| `macvlan_parent` / `macvlan_subnet` / `macvlan_gateway` | from the docker host's default route | macvlan network shape |
| `macvlan_range` | top /28 of the subnet | pool docker assigns addresses from |
| `macvlan_host_route` | on | add a host-side route so this machine can reach its own macvlan container (matters on `bare`) |
| `macvlan_shim_ip` | one below the range | the host shim's own address |
| `dns` | image default | up to four DNS servers |
| `proxy_type` / `proxy_host` / `proxy_port` | unset | system proxy: `static` with host and port (port defaults to 3128), or `none` |

### Apps and desktop integration

| key | default | meaning |
|---|---|---|
| `desktop_menu` | on | refresh a launcher folder for the profile, one entry per Android app, after each connect |
| `app_modes` | freeform | per app, `<pkg>=desktop` or `<pkg>=freeform` (a phone-sized window) |
| `auto_update_apps` / `auto_update_hours` | off / 24 | run the Play update sweep after a connect when this many hours have passed |
| `arm_apps` | none | packages pinned to their arm64 build |
| `arm_guard` | on | after each connect, re-fetch a pinned package that Play updated back to x86_64 |
| `shared_folders` | none | `<hostDir>[:<name>]`, visible in Android at `/sdcard/<name>`; the path is on the docker host and may not contain spaces or colons |
| `companion_apps` | none | `<hostDir>[:<Name>]` holding `<Name>.apk`, mounted as a system app |

Changing `shared_folders` or `companion_apps` recreates the container on the next connect.

### Root and Play

| key | default | meaning |
|---|---|---|
| `root_hiding` | off | `denylist` (Magisk DenyList), `shamiko` (needs the `shamiko` feature), `full` (both) |
| `denylist_packages` | none | packages root is hidden from |
| `play_spoof` | off | activate the baked Play spoof stack (needs the `play_spoof` feature) |

### Image and source build

| key | default | meaning |
|---|---|---|
| `image_tag` | resolved (§5) | deploy exactly this image |
| `features` | the version's defaults | build features (§3) |
| `android_version` | 16 | 16 or 17 |
| `build_host` | this machine | docker host that builds and holds images |
| `build_dir` | `~/.cache/remora/image-build` | overlay-build context |
| `output_dir` | `~/.local/share/remora/images` | where exported image archives land |
| `source_host` | this machine | host where the source tree lives and compiles |
| `source_tree` | none (required) | the LineageOS/AOSP checkout |
| `source_kind` | `lineage` | `lineage` or `aosp` |
| `source_repo` / `source_ref` | LineageOS manifest / unset | where to sync from, and which revision |
| `source_pinned` | on | sync Remora's pinned manifest (the tested revisions) |
| `source_patches` | the version's defaults | source patches (§4) |
| `arch_variant` | generic x86-64 | CPU tuning: `broadwell`, `haswell`, `skylake`, `alderlake`, `silvermont`, `goldmont`, `tremont`; an image tuned past the running CPU crashes with illegal instructions |
| `build_nice` / `build_jobs` | 15 / all cores | build niceness and parallelism |
| `build_mem_gib` / `soong_mem_gib` | auto | RAM cap for the build container, and for soong's Go heap (needs `soong_gomemlimit`) |

`built_commit` and `built_recipe` are written by Remora to notice when a rebuild is pending.

### Advanced (no GUI control)

| key | default | meaning |
|---|---|---|
| `use_overlayfs` | on | `/data` as a private layer over the shared base; `false` = a plain per-profile bind |
| `use_codec2` | on | enable the Codec2 media pipeline the hardware encoders and decoders need |

**Other sections.** `[Deployment-<name>]` (`profile`, `host`, `image`) are the rows of the Runners
page. `[Source-<name>]` (`enabled`, `tags`, `location`) are the image sources on the Settings page.
Keys from older releases that nothing reads any more are left in place and ignored.

---

## 7. Picture-in-picture

`remora pip [profile]` toggles a second, small, always-on-top mirror beside the running one:
it opens one if none is up and closes it otherwise, so it binds cleanly to a hotkey. Its size is
`pip_width` × `pip_height`. It uses the profile's codec and encoder with a lower bit rate (6M) and a
960-pixel cap, which keeps a second session cheap.

Every profile's launcher folder carries *Android (mirror)*, *Android (desktop)* and
*Android (PIP)* entries, refreshed by `remora apps <profile>` and after each connect while
`desktop_menu` is on — including for a profile whose container is not running. The toggle is per
profile: the window title names the profile (`Remora PIP — <profile>`), so each entry closes only
its own PIP.

Every PIP window has the class `remora-pip`, distinct from the main mirror's `remora`, so one window
rule covers all of them. To keep it on every desktop and above other windows in KWin, add a rule
(System Settings → Window Management → Window Rules) or append to `~/.config/kwinrulesrc`:

```ini
[remora-pip]
Description=Remora PIP
wmclass=remora-pip
wmclasscomplete=false
wmclassmatch=1
desktops=\0
desktopsrule=2
above=true
aboverule=2
```

`desktops=\0` means all desktops and rule value `2` means *Force*. A tiling script such as
Krohnkite should list `remora-pip` among its ignored window classes.

---

## 8. Mirror navigation bindings

The Viewer page's *Navigation* card shows one row per Android action — Back, Home, Recents,
Notifications — with the mouse button, the single key and the modifier chord that trigger it. It
writes three keys; malformed values are refused with an error rather than quietly replaced.

- **`mouse_bind`** — `WXYZ:wxyz`, one character each for the right, middle, 4th and 5th buttons,
  then the same four with Shift held. Characters: `b` Back, `h` Home, `s` Recents, `n`
  Notifications, `+` pass the click through to Android, `-` swallow it. Default `bhsn:++++`
  (right = Back, middle = Home, 4th = Recents, 5th = Notifications; shifted clicks pass through).
  Left click is always touch.
- **`shortcut_mod`** — the modifier that arms the *MOD+b/h/s/n* chords: comma-separated
  alternatives, each a `+`-joined set of `lctrl`, `rctrl`, `lalt`, `ralt`, `lsuper`, `rsuper`.
  Unset means `lalt,lsuper`. The desktop reports modifiers without a side, so `lalt` and `ralt`
  both mean Alt.
- **`key_bind`** — single unmodified keys bound to an action: comma-separated `<key>:<action>`
  pairs using SDL key names and the `b/h/s/n` characters, e.g. `F1:b,F2:h,F3:s`. A bound key is
  consumed by the mirror and never reaches Android, so function and navigation keys are the natural
  choice. Unset means no bindings.
