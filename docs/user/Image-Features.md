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

These statuses describe technical behaviour, not distribution rights or Google certification.
Public images must satisfy the [distribution policy](../IMAGE_MIRROR.md). A local GApps overlay
does not establish permission to obtain or use Google's apps on Remora. The Google APKs support
x86_64; ARM-only third-party apps need a separately obtained compatible translator.

A profile lists its features in `features=`, and the workspace shows them on the **Image** page.
Leave the key unset and the defaults for the Android version apply.

## The features at a glance

| key | what it is | default | requires | conflicts | status |
|---|---|---|---|---|---|
| `lineage_frameworks` | LineageOS frameworks | on | | | OK |
| `container_compat_patches` | Container-compat patch set | on | | | MANDATORY |
| `mirror_agent` | In-image mirror agent | on for Android 17 | | | OK |
| `gapps` | Google Apps (Play Store and Play services) | on | | `microg` | OK |
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
| `public_release` | Public release profile (redistributable) | off | `mirror_agent`, `mesa_source` | `gapps`, `microg`, `arm_translate`, `webview_pairip_fix`, `widevine_l3`, `magisk_root`, `play_spoof`, `shamiko` | CAVEAT |

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
- **`public_release`** builds the image Remora publishes, `remora24:x86_64-public`. It is x86_64
  only, and carries no Google apps, ARM translation, Widevine, root or Play spoofing. It refuses
  every one of those features, and the image stops advertising the ARM ABI. The build clears the
  previous build's installed partitions first, and ends with a release audit that fails the build
  on any excluded payload. ARM-only apps refuse to install on it. Most people download this image
  with `remora images fetch` rather than build it; see [CLI Reference](CLI-Reference.md#official-images).

## Apps and Google services

The **Built images** card has **Install Google Play…** and **Remove Play overlay…** actions.
Select a Docker image row, then accept the displayed provider/terms notice. Installation supports
a Google-free Android 17 x86_64 base and creates a separate `remora-local:` image; select it for
a fresh instance. Download and verification happen directly on that image's Docker host. The
installer rejects an existing Google/microG installation and never runs the third-party scripts.
The remove action refuses any image used by a running or stopped container and retains the base
and Android data. These actions do not change existing profiles or log into Google. Details and
headless/remote usage are in [CLI Reference](CLI-Reference.md#build).

- **`gapps`** adds the Play Store and Play services and supports layering onto an existing image.
  The [APK audit](../GOOGLE_PLAY_SOURCE_TERMS.md) found x86_64 native code, and an isolated
  Android 17 boot opened the Play Store without an ARM bridge. The feature no longer requires
  `arm_translate`. This establishes startup, not Google certification, account access or app compatibility.
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
