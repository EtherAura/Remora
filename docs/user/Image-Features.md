# Image Features

This page summarises the build features you can bake into an Android image, with the status the
project gives each one. Build features are fixed when the image is built: changing one means a
different image. The full catalogue, including source patches and runtime settings, is
[FEATURES.md](../FEATURES.md), and the code wins where the two disagree.

## How to read the status

| status | meaning |
|---|---|
| **OK** | works |
| **CAVEAT** | works, with the limitation described in its note |
| **MANDATORY** | always on. The checkbox on the Image page is forced |

The registry also knows **PARKED** (selecting it blocks the build) and **DEFERRED** (warns). No
current feature uses either.

A profile lists its features in `features=`, and the workspace shows them on the **Image** page.
Leave the key unset and the defaults for the Android version apply.

## The features at a glance

| key | what it is | default | requires | conflicts | status |
|---|---|---|---|---|---|
| `lineage_frameworks` | LineageOS frameworks | on | | | OK |
| `container_compat_patches` | Container-compat patch set | on | | | MANDATORY |
| `mirror_agent` | In-image mirror agent | on for Android 17 | | | OK |
| `gapps` | Google Apps (Play Store and Play services) | on | `arm_translate` | `microg` | OK |
| `microg` | microG, de-Googled Play services | off | | `gapps` | CAVEAT |
| `arm_translate` | ARM app translation (native bridge) | on | | | CAVEAT |
| `system_updater` | In-Android system updater, applied by the host | off | patch `settings_hide_stock_updater` | | OK |
| `play_spoof` | Play spoof (Zygisk and PIF, per-app Pixel) | off | `magisk_root` | | OK |
| `webview_pairip_fix` | WebView PairIP x86_64 fix | on | `arm_translate` | | CAVEAT |
| `magisk_root` | Magisk root (su and DenyList) | on | | | OK |
| `shamiko` | Shamiko denylist unmount | off | `magisk_root`, `play_spoof` | | CAVEAT |
| `camera_v4l2` | Camera (external UVC/V4L2 provider) | off | | | CAVEAT |
| `usb_audio` | Microphone (USB audio HAL) | off | patch `usb_audio_policy` | | CAVEAT |
| `widevine_l3` | Widevine L3 DRM | off | | | OK |
| `mesa_source` | Mesa from source | off | | | OK |
| `hw_video_decode` | Hardware video decode (VA Codec2) | off | | | CAVEAT |
| `sensors` | Sensors (accelerometer and gyroscope, host-injectable) | off | patches `sensors_hal`, `sensors_product` | | OK |

A "requires patch" entry is a source patch the feature needs in a from-scratch build. The Image
page enables it along with the feature. A hand-edited profile that lacks it gets a warning when the
build starts, because the build would succeed and ship the feature doing nothing.

## Base system

- **`lineage_frameworks`** puts LineageOS's framework parts on top of AOSP.
- **`container_compat_patches`** is what lets Android boot in a container at all: binder device
  allocation, a stubbed SELinux, seccomp off and a read-only property override.
- **`mirror_agent`** bakes the Remora agent into the image as a boot service. It is the device half
  of the mirror, so an image without it cannot be mirrored. It is off by default on Android 16:
  enable it for a 16 source build. See [Mirroring](Mirroring.md).

## Apps and Google services

- **`gapps`** adds the Play Store and Play services. The payload is arm64, which is why it requires
  the native bridge. It is one of the two features that can be layered onto an existing image.
- **`microg`** installs GmsCore, its Companion and the Aurora Store instead. Push, check-in,
  location and Aurora installs work. Signature spoofing is not wired, so apps that verify the Play
  signature through the GMS client library still refuse. Source build only.

## Compatibility

- **`arm_translate`** is the ARM native bridge that runs arm64-only apps on x86_64. Its caveat is
  ABI-list ordering: an app can be started under the wrong ABI. The payload is proprietary.
- **`system_updater`** adds *Settings, System, Remora Updater*. *Apply* signals the host, where
  `remora update-watch` does the update.
- **`play_spoof`** bakes a ReZygisk and PlayIntegrityFork stack so that Google Play sees a
  certified Pixel inside GMS and Play only. System properties and adb are untouched. It stays
  dormant until the `play_spoof=true` runtime setting turns it on, and `remora certify` reports the
  result.
- **`webview_pairip_fix`** is a capability label inherited from earlier images. Remora carries no
  implementation of it, and toggling it changes nothing about what a source build produces.

## Root and hiding

- **`magisk_root`** is upstream Magisk v30.7: `su` and the native DenyList unmount.
- **`shamiko`** unmounts Magisk's modifications from denylisted processes. It is closed-source with
  no redistribution licence: supply the payload yourself, and do not share an image that contains
  it. It ships without the upstream service script that would take adb away.

The runtime side is the `root_hiding`, `denylist_packages` and `play_spoof` settings: see
[Configuration](Configuration.md#root-and-play).

## Hardware and media

- **`camera_v4l2`** builds AOSP's external camera provider. The provider looks for `/dev/video*`
  at runtime, so the container must also be given camera nodes. `camera_devices` does that
  automatically when the feature is on. A camera plugged in later needs the container recreated.
- **`usb_audio`** builds AOSP's USB audio HAL. Without it, the microphone Android reports records
  perfectly timed silence. It only works together with the `usb_audio_policy` patch, and a build
  with the patch but not the feature is refused because it breaks all audio.
- **`widevine_l3`** is the Widevine L3 CDM, software DRM. L1 is impossible without a TEE. It is the
  other feature that can be layered onto an existing image.
- **`mesa_source`** installs Mesa 26.x, built for Android, as the image's GL and Vulkan drivers.
  It is required for every source build, yet off by default so that the default feature set keeps
  resolving to a registered image tag. See [Building the Image](Building-the-Image.md#build-the-mesa-payload).
- **`hw_video_decode`** selects the `-hwc2` image variant with Remora's VA-API Codec2 HAL. It needs
  `gpu_mode=host` on a GPU with a VA-API driver. See
  [GPU Acceleration](GPU-Acceleration.md#hardware-video-inside-android).
- **`sensors`** adds an accelerometer and gyroscope whose values the host injects. It drives
  auto-rotate and `remora rotate`.

## Removed keys

`native_vulkan` is gone, because the Vulkan driver is now chosen per deploy (the `vulkan` setting).
`zygisk_hiding` is gone too. A removed key left in an old profile's `features=` is dropped when the
recipe is built.

## Features that need a payload

Google apps, microG, the native bridge, Magisk, Widevine and the Zygisk and PIF stack are
third-party binaries that the repository does not carry. You fetch or supply them before the
build: see [Building the Image](Building-the-Image.md#supply-the-third-party-payloads). Several of
them also decide whether you may share the image you built, which [NOTICE](../../NOTICE) records.

## Source patches

A source build also applies Remora's own patches to the Android tree. Each has a key and a default,
and `source_patches=` pins an explicit list. Unset means the defaults for the profile's Android
version. A pinned list does not pick up defaults added later, and the build warns about each
default it leaves out. The table of patches is in [FEATURES.md](../FEATURES.md).
