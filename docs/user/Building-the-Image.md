# Building the Image

This page covers the long step: building your Android image from source. It explains the Android
versions, how a feature selection becomes an image, the two inputs you must supply (the Mesa
payload and the third-party payloads), the two host settings that make builds die, and
`remora build --source` itself. Check the disk, memory and time figures in
[Requirements](Requirements.md#to-build-the-android-image) first.

## Why you build it yourself

Remora ships no prebuilt images. It compiles LineageOS from source with exactly the features you
pick, once per Android release and feature set. Some features bake in third-party binaries that
nobody may redistribute, so an image you build can be yours to run without being yours to share:
[NOTICE](../../NOTICE) and [IMAGE_MIRROR.md](../IMAGE_MIRROR.md) spell that out.

## Choose the Android version

| `android_version` | base | notes |
|---|---|---|
| `16` | LineageOS 23 | the default when the key is unset. The docker host also needs the IBT-fixed `ashmem_linux.ko` |
| `17` | LineageOS 24 | needs no kernel module: shared memory runs on memfd |

Both build from the same Remora device tree and lunch the product `remora_x86_64`. Versions 8 to
15 are named in the registry but have no build assets, so selecting one is refused.

The `mirror_agent` feature is on by default for Android 17 only. An image without the agent cannot
be mirrored, so enable it yourself for an Android 16 source build.

## Choose the features

Tick features on the **Image** page, or list them in the profile's `features=` key.
[Image Features](Image-Features.md) describes each one. Three rules matter here:

- **`mesa_source` is required for every source build**, although it is off by default. The device
  tree has no prebuilt GL or Vulkan stack to fall back on, so `remora build --source` refuses a
  profile without it.
- **`features=` is a pin.** Once the key is set it is an explicit list, and a default feature
  added in a later release does not join it. The build warns about each default it leaves out.
- **Requirements close when an image is selected, and conflicts block.** Asking for `gapps` pulls
  in `arm_translate`, and `gapps` cannot be combined with `microg`. In a hand-written `features=`
  line, name the requirements too: a source build stages exactly the features the list names.

## How a selection becomes an image

1. An explicit `image_tag=` wins outright.
2. Otherwise the requested features (or the version's defaults) are matched against the registered
   tags for that Android version. The tag missing the fewest requested features wins.
3. What that tag still lacks decides the rest. `gapps` and `widevine_l3` can be layered on as a
   Docker overlay: `remora recipe` prints the Dockerfile and `remora build` builds it. Anything
   else needs a source build.

The registered tags are `remora23:x86_64` (Android 16) and `remora24:x86_64` (Android 17), each
with `-gapps`, `-gapps-wv` and `-gapps-wv-hwc2` variants. [FEATURES.md](../FEATURES.md) lists what
each provides. They are names the resolver looks for, not downloads.

## Build the Mesa payload

The image's GL and Vulkan drivers are Mesa, cross-built for Android on the host by
`vendor/host-prereqs/mesa-android/build-mesa.sh`. Without the payload the build would finish and
produce an image that cannot composite, so it refuses instead. The payload lands in
`vendor/source-build/features/mesa_source/lib64` and adds about 120 MB to the image.

```sh
REMORA_MESA_NDK=/path/to/android-ndk \
REMORA_MESA_TREE=/path/to/source-tree \
    vendor/host-prereqs/mesa-android/build-mesa.sh
```

| variable | meaning | default |
|---|---|---|
| `REMORA_MESA_NDK` | Android NDK root. Required. r27c is the proven release | none |
| `REMORA_MESA_TREE` | the Android source tree, for the target libdrm. Required | `$REMORA_SOURCE_TREE` |
| `REMORA_MESA_WORK` | build and cache directory | `~/.cache/remora/mesa-android` |
| `REMORA_MESA_GALLIUM` | gallium drivers. Empty skips the GL cluster | `iris,radeonsi` |
| `REMORA_MESA_VULKAN` | Vulkan drivers | `intel,amd` |
| `REMORA_MESA_STAGES` | comma list of stages to run, for example `4,5` | all five |

The script needs `git`, `meson`, `ninja`, `cmake`, `pkg-config` and `python3`. Its stages are
resumable, and the expensive ones cache in the work directory. Two things to know:

- It takes the target libdrm from the source tree's build output
  (`out/target/product/remora_x86_64/vendor/lib64`), and stops with a message telling you to build
  the image first when that output is not there yet.
- A machine that renders through NVIDIA cannot exercise this payload, because Android there uses
  the Venus driver. Validate a new payload on an Intel or AMD host, and keep a rollback image tag
  when you adopt one. [The Mesa README](../../vendor/host-prereqs/mesa-android/README.md) has the
  background.

## Supply the third-party payloads

The repository carries no third-party binaries. A build whose feature is enabled but whose payload
is absent refuses at staging and names the fetch command. It never builds an image that silently
lacks the feature.

```sh
vendor/fetch-payloads.sh            # report what is present and what is missing
vendor/fetch-payloads.sh magisk     # stage one payload
```

| payload | feature | how you get it |
|---|---|---|
| `magisk` | `magisk_root` | fetched and hash-verified by `fetch-payloads.sh` (upstream Magisk v30.7) |
| `microg` | `microg` | identified by version and hash, but no download URL is recorded: get the named releases yourself |
| `widevine` | `widevine_l3` | identified by archive hash, but no download URL is recorded |
| `zygisk_pif` | `play_spoof`, `shamiko` | ReZygisk and PlayIntegrityFork are identified. The 64-bit ReZygisk binaries are built by `vendor/host-prereqs/rezygisk/build-rezygisk.sh`. Shamiko is closed-source: supply it yourself |
| `gapps` | `gapps` | source unrecorded: supply it yourself |
| `ndk_translation` | `arm_translate` | source unrecorded: supply it yourself |

The script refuses to guess a URL for a payload whose source is unrecorded.
[vendor/PAYLOADS.md](../../vendor/PAYLOADS.md) records the versions and hashes that are known.

## Set the two host settings

Both failures point at the wrong thing, so set these before the first build:

```sh
# soong opens a lot of files; too low and it dies with "newosproc", which is NOT an OOM
sudo sysctl -w vm.max_map_count=1048576

# systemd-oomd kills the build container at exit 137 and logs to the JOURNAL, not dmesg
systemctl is-active systemd-oomd && journalctl -u systemd-oomd --since -1h
```

For the second, raise `SwapUsedLimit` in `/etc/systemd/oomd.conf`, or set
`ManagedOOMPreference=avoid` on the slice the build runs in. Remora's build preflight warns about
both before it starts.

## Run the source build

Set `source_tree` in the profile (the *Source build* card on the Image page, or the key in
`remorarc`), then:

```sh
remora build --source            # the active profile
remora build --source <profile>  # a named one
```

The command prints the profile, tree, host, the tag it will build, the patch count, the job count
and the memory cap, then streams the build to the terminal and to `/tmp/remora-build.log`. Ctrl-C
stops the whole chain. Success is judged by the chain's exit status and by the image actually
existing afterwards, never by reading the output.

Before it compiles anything, the command refuses a build that could only fail or mislead:

| refusal | fix |
|---|---|
| no `source_tree` configured | set it |
| `mesa_source` not enabled | enable *Mesa from source*, and build the payload first |
| the `usb_audio_policy` patch without the `usb_audio` feature | enable the feature or drop the patch: the combination breaks all audio |
| the installed patch tree and the binary disagree | reinstall (`cmake --install`), or rebuild Remora, as the message says |
| `source_kind=aosp` with LineageOS settings in the profile | resolve the conflict the message names |

Settings that shape the build (all optional, see [Configuration](Configuration.md#image-and-source-build)):

| key | default | meaning |
|---|---|---|
| `source_host` | this machine | host where the source tree lives and compiles |
| `source_kind` | `lineage` | `lineage` or `aosp` |
| `source_pinned` | on | sync Remora's pinned manifest, the tested revisions |
| `build_jobs` / `build_nice` | all cores / 15 | parallelism and niceness |
| `build_mem_gib` / `soong_mem_gib` | auto | RAM cap for the build container, and for soong's Go heap |
| `arch_variant` | generic x86-64 | CPU tuning, for example `haswell` or `alderlake` |

An image tuned past the CPU that runs it crashes with illegal instructions. `remora check` and
`remora plan` report an image's CPU tuning against the target, and a deploy refuses a mismatch.

## After the build

A source build is tagged
`remora<LineageOS major>:x86_64[-gapps][-microg][-wv][-hwc2][-<arch_variant>]-src`.
*Promote to active* on the Image page drops the `-src`, which is the name the resolver looks for.
An `arch_variant` build lands under its own suffix, so a CPU-tuned image lives beside the portable
one.

Later rebuilds are much shorter than the first: under an hour in the project's measurement. Images
built before release 2026.10.0 need a rebuild, because the mirror protocol changed and the agent
is part of the image.

If a build fails, see [Troubleshooting](Troubleshooting.md#the-image-build).
