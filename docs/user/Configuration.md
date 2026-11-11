# Configuration

This page documents `~/.config/remorarc`, the file that holds every profile: how it is layered,
how profiles are named and selected, and what each key means and defaults to. The workspace writes
this file for you, and hand edits are fine.

## The file

`remorarc` is a standard INI file with these sections:

| section | holds |
|---|---|
| `[Remora]` | `active_instance`, the name of the active profile |
| `[Defaults]` | keys that apply to every profile that does not set them |
| `[Instance-<name>]` | one profile (one instance) |
| `[Deployment-<name>]`, `[Source-<name>]` | workspace lists, see [Other sections](#other-sections) |

A profile is read in layers: the built-in default, then `[Defaults]`, then `[Instance-<name>]`.
The last one that sets a key wins.

```ini
[Remora]
active_instance=android17

[Defaults]
audio=true

[Instance-android17]
android_version=17
gpu_mode=host

[Instance-lab]
ssh_host=user@host
android_version=17
```

Rules that apply to every key:

- **Every key is optional.** Leave one unset and Remora applies its proven default, so an empty
  profile still resolves to a bootable instance. Deleting a key returns it to its default.
- **Lists are space-separated** unless the table says otherwise.
- **`remora plan` shows what a profile resolves to.** Use it after an edit.
- **Commands take the profile name as an argument** and fall back to `active_instance`. A name
  that matches no `[Instance-<name>]` section is an error, not a silent fall back to the defaults.
- **Keys from older releases** that nothing reads any more are left in place and ignored.

## Target and storage

| key | default | meaning |
|---|---|---|
| `backend` | inferred from `ssh_host` | `bare` or `remote` |
| `ssh_host` | unset | `user@host` of a remote docker host. Setting it selects the `remote` target |
| `container_name` | `remora-<profile>` | the container, and how Remora finds a running instance |
| `data_dir` | per target, see [Deploy Targets](Deploy-Targets.md#per-target-defaults) | host directory holding the device's `/data`: its private layer by default |
| `data_base_dir` | per target, see [Deploy Targets](Deploy-Targets.md#per-target-defaults) | shared read-mostly base layer that profiles on one host stand on |
| `host_adb_port` | 5555 | host port published for adb |
| `adb_bind` | `127.0.0.1` on `bare`, every interface on `remote` | address the adb port is published on. `0.0.0.0` publishes everywhere |

Profiles created in the workspace get their own `container_name`, `data_dir`
(`~/.remora-<profile>`) and `host_adb_port` written in. See [Deploy Targets](Deploy-Targets.md).

## Display and system

| key | default | meaning |
|---|---|---|
| `width` / `height` / `dpi` | 3760 / 1992 / 320 | the device display |
| `fps` | 60 | display refresh rate |
| `max_fps` | unset, or 60 when `fps` is above 60 | cap on the mirror encoder's frame rate |
| `timezone` | the host's zone | IANA zone for Android |
| `use_memfd` | true | use memfd for shared memory |
| `ro_overrides` | none | `key=value` entries passed to Android as `androidboot.ro.<key>=<value>` |
| `auto_sleep_minutes` | off | freeze the device after this long with no mirror attached. Enforced by `remora auto-sleep` |

## GPU

| key | default | meaning |
|---|---|---|
| `gpu_mode` | `guest` | `guest` is software rendering, `host` is the docker host's GPU |
| `gpu_node` | probed at each deploy | exact render node, such as `/dev/dri/renderD128` |
| `gpu_driver` | probed | choose the render node by driver name (`xe`, `i915`, `amdgpu`). `gpu_node` wins over it |
| `gralloc` | per driver | gralloc HAL: `minigbm_intel` on xe, `cros` on amdgpu, `minigbm_gbm_mesa` under Venus |
| `hwcomposer` | image default | composer HAL. Remora's own is the only one the image ships |
| `vulkan` | per driver | Vulkan driver: `intel`, `radeon`, `virtio` (Venus), `broadcom`, `freedreno`, `panfrost`, or `pastel` (software) in guest mode |
| `va_driver` | per driver | VA-API driver for video: iHD on Intel, `radeonsi` on amdgpu |
| `venus` | automatic | NVIDIA through Venus. `false` pins it off, `true` forces it on |
| `venus_socket_dir` | `~/.cache/remora/venus` | render-server socket directory (one per container when `host_encode` is on) |
| `venus_lib_dir` / `venus_server` | probed | guest libraries and render-server binary, as installed by `remora venus-build` |

## Mirror stream

| key | default | meaning |
|---|---|---|
| `video_codec` | `h265` | `h264`, `h265` or `av1` |
| `video_bit_rate` | `30M` | mirror bit rate |
| `max_size` | 0 | longest side of the mirrored video. 0 is the display's full size |
| `audio` | off | mirror audio |
| `hw_decode` | on | decode the mirror on this machine's GPU |
| `host_encode` | off | encode the mirror on the host GPU. `gpu_mode=host` and the Venus stack only |
| `host_encode_max_size` | 0 | cap on the host encoder's output size, longest side |
| `host_decode` | off | decode in-Android video on the host GPU. `gpu_mode=host` only |

## Mirror window and input

| key | default | meaning |
|---|---|---|
| `window_width` / `window_height` / `window_x` / `window_y` | unset | mirror window geometry |
| `window_fullscreen` | off | open the mirror fullscreen |
| `fullscreen_separate_display` | off | *Fullscreen* opens a separate desktop-mode display instead of mirroring |
| `fullscreen_display` / `fullscreen_app` | main size / none | that display's `WxH[/dpi]`, and the app to start on it |
| `desktop_new_window` | off | the desktop shortcut opens a new window each press |
| `freeform_display` | `1080x2340/420` | size of the phone-shaped window apps open in |
| `pip_width` / `pip_height` | 480 / 270 | picture-in-picture size |
| `mirror_extra` | none | extra flags passed to `remora mirror` |
| `mouse_bind` | `bhsn:++++` | `WXYZ:wxyz`: one character each for the right, middle, 4th and 5th buttons, then the same four with Shift. `b` Back, `h` Home, `s` Recents, `n` Notifications, `+` pass the click to Android, `-` swallow it |
| `shortcut_mod` | `lalt,lsuper` | comma-separated alternatives, each a `+`-joined set of `lctrl`, `rctrl`, `lalt`, `ralt`, `lsuper`, `rsuper` |
| `key_bind` | none | comma-separated `<key>:<action>` pairs, such as `F1:b,F2:h,F3:s` |
| `kbd_identity` | first host keyboard | which host keyboard the in-container keyboard claims to be. `none` is generic |
| `shared_inputs` | none | host input devices shared with Android by name, non-exclusively. Newline-separated |
| `camera_devices` | automatic | host `/dev/video*` nodes given to the container. Automatic means every probed node, and only when `camera_v4l2` is enabled. An empty value means none |
| `cpufreq_topology` | on | show Android a phone-like CPU frequency topology. Some apps crash on a desktop's |

Malformed binding values are refused with an error, not quietly replaced.

## Network

| key | default | meaning |
|---|---|---|
| `network_mode` | `bridge` | `bridge` publishes the adb port, `macvlan` gives the container its own LAN address |
| `docker_network` | `lan` | macvlan docker network name |
| `macvlan_ip` | assigned by docker | fixed LAN address for the container |
| `macvlan_parent` / `macvlan_subnet` / `macvlan_gateway` | from the docker host's default route | macvlan network shape |
| `macvlan_range` | top /28 of the subnet | pool docker assigns addresses from |
| `macvlan_host_route` | on | add a host-side route so this machine can reach its own macvlan container (`bare`) |
| `macvlan_shim_ip` | one below the range | the host shim's own address |
| `dns` | image default | up to four DNS servers |
| `proxy_type` / `proxy_host` / `proxy_port` | unset | system proxy: `static` with host and port, or `none` |

## Apps and desktop integration

| key | default | meaning |
|---|---|---|
| `desktop_menu` | on | refresh the profile's launcher folder after each connect |
| `app_modes` | freeform | per app, `<pkg>=desktop` or `<pkg>=freeform` |
| `auto_update_apps` / `auto_update_hours` | off / 24 | run the Play update sweep after a connect when this many hours have passed |
| `arm_apps` | none | packages pinned to their arm64 build |
| `arm_guard` | on | after each connect, re-fetch a pinned package that Play updated back to x86_64 |
| `shared_folders` | none | `<hostDir>[:<name>]`, visible in Android at `/sdcard/<name>`. The path is on the docker host and may not contain spaces or colons |
| `companion_apps` | none | `<hostDir>[:<Name>]` holding `<Name>.apk`, mounted as a system app |

Changing `shared_folders` or `companion_apps` recreates the container on the next connect.

## Root and Play

| key | default | meaning |
|---|---|---|
| `root_hiding` | off | `denylist` (Magisk DenyList), `shamiko` (needs the `shamiko` feature) or `full` (both) |
| `denylist_packages` | none | packages root is hidden from |
| `play_spoof` | off | activate the baked Play spoof stack (needs the `play_spoof` feature) |

## Image and source build

| key | default | meaning |
|---|---|---|
| `image_tag` | resolved from the features | deploy exactly this image |
| `features` | the version's defaults | build features, see [Image Features](Image-Features.md) |
| `android_version` | 16 | 16 or 17 |
| `build_host` | this machine | docker host that builds and holds images |
| `build_dir` | `~/.cache/remora/image-build` | overlay-build context |
| `output_dir` | `~/.local/share/remora/images` | where exported image archives land |
| `source_host` | this machine | host where the source tree lives and compiles |
| `source_tree` | none, required for a source build | the LineageOS or AOSP checkout |
| `source_kind` | `lineage` | `lineage` or `aosp` |
| `source_repo` / `source_ref` | the LineageOS manifest / unset | where to sync from, and which revision |
| `source_pinned` | on | sync Remora's pinned manifest, the tested revisions |
| `source_patches` | the version's defaults | source patches to apply. A set list is a pin |
| `arch_variant` | generic x86-64 | CPU tuning: `broadwell`, `haswell`, `skylake`, `alderlake`, `silvermont`, `goldmont`, `tremont` |
| `build_nice` / `build_jobs` | 15 / all cores | build niceness and parallelism |
| `build_mem_gib` / `soong_mem_gib` | automatic | RAM cap for the build container, and for soong's Go heap |

`built_commit` and `built_recipe` are written by Remora to notice when a rebuild is pending.

## Advanced

These have no control in the workspace.

| key | default | meaning |
|---|---|---|
| `use_overlayfs` | on | `/data` as a private layer over the shared base. `false` is a plain per-profile bind |
| `use_codec2` | on | enable the Codec2 media pipeline that the hardware encoders and decoders need |

## Other sections

| section | keys | used by |
|---|---|---|
| `[Deployment-<name>]` | `profile`, `host`, `image` | the rows of the workspace's Runners page: a profile deployed on a specific docker host, optionally with another image |
| `[Source-<name>]` | `enabled`, `tags`, `location` | the image sources on the workspace's Settings page |

[FEATURES.md](../FEATURES.md) is the authoritative catalogue of these keys and is written from the
code.
