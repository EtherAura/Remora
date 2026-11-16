# Add-ons: what each build feature puts into an image

For each payload-carrying build feature: every path it installs, the properties it sets, its init
and runtime steps, what it needs from the build itself, where its payload comes from, and whether a
Dockerfile `COPY` layer on the published base `remora24:x86_64-public` could carry it instead.

Everything here is read from the sources named in each section. A section saying a `COPY` layer
*can* carry a file says only that nothing in the build does more than place that file. Where a
layer has been built and booted, the section says so and records what was observed.

Sections still to write: the inventory half of `play_spoof` (which so far records only its
acceptance target and limits), and the note that `webview_pairip_fix` has no implementation and so
needs no add-on.

## magisk_root

Upstream Magisk v30.7 (su + DenyList), driven from init rather than from a patched boot image.
Sources: `vendor/source-build/features/magisk/` (`magisk.mk`, `Android.mk`, `magisk.rc`,
`magisk-setup.sh`, `magisk/PROVENANCE.md`), `vendor/source-build/stage-features.sh`,
`vendor/source-build/assemble-image.sh`, `vendor/PAYLOADS.md`, `vendor/fetch-payloads.sh`.

### How it reaches the build

`magisk_root` sets `WITH_MAGISK=true` (`src/engine/SourceBuild.cpp`). `stage-features.sh` then
refuses unless the feature directory holds a payload (any `*.apk`, `*.so`, `*.jar` or `*.dex`),
copies it to `vendor/magisk` in the source tree, and appends
`$(call inherit-product-if-exists, vendor/magisk/magisk.mk)` to the product makefile. With the
feature off it removes `vendor/magisk` again. `assemble-image.sh` has no Magisk overlay of its own:
every file below arrives inside `system.img`.

### Installed paths

Two mechanisms, and only two. `Android.mk` declares seven ETC-class `BUILD_PREBUILT` modules
(listed in `PRODUCT_PACKAGES` by `magisk.mk`) with `LOCAL_CHECK_ELF_FILES := false`, so the ELF
and apk checks do not apply. `magisk.mk` adds `PRODUCT_COPY_FILES` for the shell scripts and the rc.

| payload file (staged as `vendor/magisk/…`) | image path | mechanism |
|---|---|---|
| `magisk/magisk` | `/system/etc/init/magisk/magisk` | prebuilt `remora_magisk_bin` |
| `magisk/magiskboot` | `/system/etc/init/magisk/magiskboot` | prebuilt `remora_magisk_boot` |
| `magisk/magiskinit` | `/system/etc/init/magisk/magiskinit` | prebuilt `remora_magisk_init` |
| `magisk/magiskpolicy` | `/system/etc/init/magisk/magiskpolicy` | prebuilt `remora_magisk_policy` |
| `magisk/busybox` | `/system/etc/init/magisk/busybox` | prebuilt `remora_magisk_busybox` |
| `magisk/magisk.apk` | `/system/etc/init/magisk/magisk.apk` | prebuilt `remora_magisk_apk` |
| `magisk/stub.apk` | `/system/etc/init/magisk/stub.apk` | prebuilt `remora_magisk_stub` |
| `magisk/util_functions.sh` | `/system/etc/init/magisk/util_functions.sh` | copy, `magisk/*.sh` wildcard |
| `magisk/boot_patch.sh` | `/system/etc/init/magisk/boot_patch.sh` | copy, `magisk/*.sh` wildcard |
| `magisk-setup.sh` | `/system/etc/init/magisk/magisk-setup.sh` | copy, explicit line |
| `magisk.rc` | `/system/etc/init/magisk.rc` | copy, explicit line |

That is eleven unique destinations: seven binaries and apks, three shell scripts and one init rc.
No destination is declared twice. `magisk-setup.sh` sits beside the `magisk/` payload directory,
not inside it, so the `magisk/*.sh` wildcard does not match it and its explicit line is its only
declaration. The wildcard is evaluated against the staged tree, so today it picks up exactly the
two upstream scripts. `PROVENANCE.md`, `Android.mk` and `magisk.mk` are not installed.

Per `PROVENANCE.md`, `magiskboot`, `magiskinit`, `boot_patch.sh` and `stub.apk` are boot-image
tooling that a container never runs. They ship anyway so the payload is a complete copy of one
upstream release.

### Properties

None. Nothing in `magisk.mk`, `Android.mk`, `magisk.rc` or `magisk-setup.sh` sets a property.
The rc only *triggers on* properties (`vold.decrypt`, `sys.boot_completed`, `init.svc.zygote`).
The `resetprop` applet it provides is something other features use at runtime, not a property this
feature sets.

### Init and runtime steps

Init loads `/system/etc/init/magisk.rc`. Every step below runs at boot, and none of it exists in
the image as a file:

- **`on post-fs-data`**: `start logd`, then `magisk-setup.sh` via `/system/bin/sh`. This script
  takes the place of what `magiskinit` would build from a patched boot image:
  - mounts a tmpfs (mode 0755) at `/debug_ramdisk` (`MAGISKTMP`), which upstream hardcodes;
  - copies `magisk` to `magisk64` and `magisk`, copies `magiskpolicy`, and `chmod 755`s all three;
  - creates the `su` and `resetprop` symlinks to `./magisk`;
  - creates `.magisk/{modules,device,preinit,rootdir}`, copies `busybox` into `.magisk/`, and
    writes `.magisk/config` if it is absent;
  - creates `/data/adb/{modules,post-fs-data.d,service.d}` and sets `/data/adb` to mode 700.

  Next comes `magiskpolicy --live --magisk`. It runs only if `/sys/fs/selinux/policy` exists, and
  exits 0 otherwise. Then `magisk --post-fs-data`.
- **`on nonencrypted`** and **`on property:vold.decrypt=trigger_restart_framework`**:
  `magisk --service`.
- **`on property:sys.boot_completed=1`**: `mkdir /data/adb/magisk 755`, `magisk --boot-complete`,
  then `pm install /system/etc/init/magisk/magisk.apk` unless `/data/data/com.topjohnwu.magisk`
  exists. A `/data` that still holds a differently signed Magisk app fails here with
  `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.
- **`on property:init.svc.zygote=restarting|stopped`**: `magisk --zygote-restart`.

The execs run in the context `u:r:su:s0`. The sources call that context inert, because the
container runs with SELinux disabled.

### Security: an su grant is host root

The Android container always runs `--privileged` (`buildDockerRunArgv` in `src/core/Builders.cpp`;
see `SECURITY.md`). Granting a guest app `su` must therefore be treated as granting it host-level
privileges: it reaches the host kernel and devices through the container. The DenyList and Shamiko
only hide root from apps; they do not contain it and are not a security boundary. This is the
threat model a privileged container implies, not a demonstrated flaw in Remora's own client.

### What it needs from the build, and what it does not

The build only places files: the seven prebuilts and the four copy pairs above. There is no
build-time sepolicy, no compiled module and no property. SELinux is handled at runtime by
`magiskpolicy --live`, and that call is skipped while the container has no `/sys/fs/selinux`.
`magisk-setup.sh` says SELinux labelling would have to be added there if the container ever ran
enforcing.

### Payload source

- **Fetched from a recorded upstream, with a hash:** the seven binaries and apks and the two
  upstream scripts. `vendor/fetch-payloads.sh magisk` downloads
  `github.com/topjohnwu/Magisk/releases/download/v30.7/Magisk-v30.7.apk` and refuses it unless it
  matches `MAGISK_APK_SHA256` (`e0d32d21…`). It then extracts `lib/x86_64/lib{magisk,magiskboot,
  magiskinit,magiskpolicy,busybox}.so`, `assets/stub.apk`, `assets/util_functions.sh` and
  `assets/boot_patch.sh`, and keeps the apk itself as `magisk.apk`. `magisk/PROVENANCE.md` records
  a sha256 prefix for each of these files. The two GPL-3.0 scripts are carried in the public tree;
  the binaries and apks are not.
- **Remora's own:** `magisk.rc` and `magisk-setup.sh`.
- **Supplied by the user:** nothing.

`vendor/PAYLOADS.md` covers licensing. Magisk is GPLv3, and upstream source is live at tag
`v30.7`, so a mirrored image carrying it has to offer that source.

### A COPY layer

The files can all be carried by a COPY layer. Everything `magisk_root` installs is a plain file
under `/system/etc/init/`, and the build does nothing to them beyond placing them. Whether an image
built this way would also *work* is a separate, unproven question. The tmpfs, the `/data/adb`
tree, the app install and the guarded policy call are boot-time work done by init from the copied
rc. A COPY layer cannot carry any of that, but it has no need to.

This is the layer `remora recipe --base remora24:x86_64-public` prints for a profile with
`magisk_root` (bd remora-6laf.2). The build context's `magisk/` is
`vendor/source-build/features/magisk/` after `vendor/fetch-payloads.sh magisk`:

```dockerfile
FROM remora24:x86_64-public

COPY magisk/magisk/magisk magisk/magisk/magiskboot magisk/magisk/magiskinit \
     magisk/magisk/magiskpolicy magisk/magisk/busybox \
     magisk/magisk/magisk.apk magisk/magisk/stub.apk \
     magisk/magisk/util_functions.sh magisk/magisk/boot_patch.sh \
     magisk/magisk-setup.sh /system/etc/init/magisk/
COPY magisk/magisk.rc /system/etc/init/magisk.rc

LABEL remora.public_release="false"
```

It copies the same eleven files as the table: ten into `/system/etc/init/magisk/` and the rc
beside them. The label is on every layer Remora builds, so the result never carries the base's
public claim.

**Built and booted.** The layer above was built over `remora24:x86_64-public`
(image `08c62ac56c4d`) and started on a throwaway container with an empty `/data`, the GPU in host
mode on an Intel xe node. It reached `sys.boot_completed=1` with
`sys.system_server.start_count=1`. `magiskd` was running, `/debug_ramdisk` held `magisk`,
`magisk64`, `magiskpolicy`, `resetprop` and `su`, and `su -c id` returned `uid=0(root)`. `magisk
-v` reported `30.7:MAGISK:R`, the Magisk app installed itself at boot completion, and `magisk
--denylist enable` then reported the DenyList enforced. That boot did not include an app
requesting `su` through the Magisk app's prompt, Play, or the hiding stack.

Constraints the sources impose, which this document does not change:

- **The public base excludes root as policy.** `public_release` conflicts with `magisk_root` in
  `src/core/Features.cpp` (as policy, not for licence reasons). `stage-features.sh` refuses
  `WITH_MAGISK=true` in a public build. The base carries `LABEL remora.public_release="true"`
  (`assemble-image.sh`), and the release tooling accepts only `remora24:x86_64-public…` tags for
  public images. The layer sets the label to `"false"`; its tag is whatever `--tag` names, so do
  not give it a `-public` one.
- **The overlay recipe offers it.** `magisk_root` is in `kOverlayable` in
  `src/core/ImageBuild.cpp`, so `remora recipe --base` and `remora build --base` layer it instead
  of asking for a source build.
- **File modes come from the build context.** A source build takes modes from the build's
  filesystem config; a COPY layer takes them from the context. From the sources, nothing needs an
  executable bit under `/system/etc/init/magisk/`: the rc runs both scripts through
  `/system/bin/sh`, and `magisk-setup.sh` copies the binaries into the tmpfs and `chmod`s them
  there.
- **Proven on a COPY-derived image** by the boot recorded above: init on the public base loads the
  added rc, the `u:r:su:s0` execs succeed, and `su -c id` inside the container returns uid 0, the
  check `PROVENANCE.md` names.

## arm_translate

The arm64 native bridge: a proprietary berberis-derived translator (`libnb.so`,
`libndk_translation.so`), its proxy libraries, and an arm64 guest userspace, which together let
x86_64 Android run apps that ship only arm64 native code. Sources:
`vendor/source-build/features/ndk_translation/` (`ndk.mk`, `Android.mk`), `ndk-overlay.sh`,
`assemble-image.sh`, `patch-arm-translation.py`, `stage-arm-layer.sh`,
`device-remora/remora_x86_64/{device.mk,BoardConfig.mk}`, and the A17 tree's
`system/core/init/property_service.cpp`.

### How it reaches the build

`arm_translate` sets `WITH_NDK=true`. `stage-features.sh` writes `REMORA_NATIVE_BRIDGE_ENABLED`
for the device tree, which then sets the bridge properties (below) and `TARGET_NATIVE_BRIDGE_*`.
`ndk.mk` installs the host-side libraries, the runner and the cpuinfo file as prebuilts. Then
`assemble-image.sh` copies the payload's guest tree and the same host libraries over `/system` in a
`COPY` step of its own, byte-patching two of them and writing one rc on the way.

**The tree builds no ARM code.** `ndk.mk` says the guest system libraries are built from source,
but nothing builds them. The device product never includes
`frameworks/libs/native_bridge_support/native_bridge_support.mk`, so the A17 tree produces no
`/system/bin/arm64` and no `/system/lib64/arm64`. An experiment that did include it built a native
A17 guest userspace, and the older translator could not drive it: zygote crash-looped. The guest
runtime every working image runs is the payload's `guest/system` tree.

### Installed paths

93 files, all from the payload except two that Remora writes:

| payload (`features/ndk_translation/…`) | image path | count |
|---|---|---|
| `guest/system/bin/arm64/{app_process64,linker64}` | `/system/bin/arm64/` | 2 |
| `guest/system/lib64/arm64/*` (guest libc, libm, libdl, libEGL, …, `cpuinfo`) | `/system/lib64/arm64/` | 60 |
| `guest/system/lib64/libberberis_exec_region.so` | `/system/lib64/` | 1 |
| `guest/system/etc/ld.config.arm64.txt` | `/system/etc/` | 1 |
| `lib64/*.so` (`libnb.so`, `libndk_translation.so`, its exec region, 22 proxies) | `/system/lib64/` | 25 |
| `bin/ndk_translation_program_runner_binfmt_misc_arm64` | `/system/bin/` | 1 |
| `etc/cpuinfo.arm64.txt` | `/system/etc/` | 1 |
| written by `finish_ndk_overlay` | `/system/etc/init/remora-berberis.rc` | 1 |
| written by `stage_arm_layer` (layer only) | `/odm_dlkm/etc/build.prop` | 1 |

The payload's `lib64/arm64/` is a byte-identical duplicate of `guest/system/lib64/arm64/` and is not
copied twice. Its `etc/binfmt_misc/arm64_{dyn,exe}` are not installed, in a source build or a
layer. No image installs a binfmt_misc registration or the rc that would load one. If one did,
the `--privileged` container would register an arm64 handler in the host kernel.

`libnb.so` and the guest `linker64` are installed patched, never as vendored.
`patch-arm-translation.py` checks a whole-file SHA-256 and the original bytes at every site, then
neuters the guest linker's systrace hooks (guest bionic TLS is NULL under this translator), makes
`AppProcessPostInit` exit with a raw syscall, and drops one guest-stack assertion. A different
build of either file is refused, which is why only the payload `vendor/PAYLOADS.md` pins can be
used.

### Properties

The source build sets these in `/vendor/build.prop` through `device.mk`, and derives the ABI list
from `TARGET_NATIVE_BRIDGE_ABI`. A `public_release` build sets the opposite: `ro.dalvik.vm.native.bridge=0`
in `/system/build.prop` and `/vendor/build.prop`, and `x86_64` alone in each partition's
`ro.<partition>.product.cpu.abilist`.

| property | with the bridge | public image |
|---|---|---|
| `ro.product.cpu.abilist`, `…abilist64` | `x86_64,arm64-v8a` | `x86_64` (derived) |
| `ro.dalvik.vm.native.bridge` | `libnb.so` | `0` |
| `ro.dalvik.vm.isa.arm64` | `x86_64` | unset |
| `ro.berberis.flags` | `accurate-sigsegv`, set by `remora-berberis.rc` at `on init` | unset |

`device.mk` also sets `ro.enable.native.bridge.exec=1` and `ro.dalvik.vm.isa.arm=x86`. The layer
leaves both out. The first is read only by the binfmt_misc rc, which no image installs, and the
second maps a 32-bit ABI that the 64-bit-only image does not advertise.

### Init and runtime steps

None beyond the berberis flag rc. zygote loads the bridge named by `ro.dalvik.vm.native.bridge`
for any app whose primary ABI is `arm64-v8a`, and the guest linker resolves the app's libraries
through `ld.config.arm64.txt`.

### What it needs from the build, and what it does not

Only the properties, and a layer can set those itself. In A17's `PropertyLoadBootDefaults`, a
property read from a later file replaces the value from an earlier one, read-only properties
included (the source says so in a comment). The order is `/system/build.prop`, the `system_ext`
props, `/system_dlkm/etc/build.prop`, `/vendor/default.prop`, `/vendor/build.prop`,
`/vendor_dlkm/etc/build.prop`, `/odm_dlkm/etc/build.prop`, then the `odm` and `product` props.
The public image has an empty `/odm_dlkm/etc/`, so the layer adds `/odm_dlkm/etc/build.prop` and
replaces none of the base's files. It sets `ro.product.cpu.abilist` and its 32- and 64-bit
variants outright: `property_initialize_ro_cpu_abilist` derives them only when they are unset, and
otherwise the odm list, read after this file, would win with `x86_64`.

### Payload source

**Supplied by the user.** The source is unrecorded, and one candidate archive is ruled out by hash
(`vendor/PAYLOADS.md`). It is proprietary, cannot be redistributed, and is never fetched. The only
Remora-authored parts are the rc, the prop file, the patcher and the staging functions.

### A COPY layer

The layer carries it all. `vendor/source-build/stage-arm-layer.sh <build_dir>` stages the payload
into `<build_dir>/ndkguest`, runs the patcher on the staged copies and writes the rc. It writes the
prop file last, so the stage check, which looks for that file, refuses a context that was copied
raw or whose patching failed. The patch-and-rc step is `finish_ndk_overlay` in `ndk-overlay.sh`,
the same function `assemble-image.sh` calls, so a source build and a layer cannot drift apart.
`remora recipe --base remora24:x86_64-public` prints the layer for a profile with
`arm_translate`:

```dockerfile
FROM remora24:x86_64-public

COPY ndkguest/system/ /system/
COPY ndkguest/odm_dlkm/ /odm_dlkm/

LABEL remora.public_release="false"
```

**Built and booted.** The layer, together with the `magisk_root` one, was built over
`remora24:x86_64-public` (image `08c62ac56c4d`) and started on a throwaway container with an empty
`/data`. The GPU was in host mode on an Intel xe node. It reached `sys.boot_completed=1` with
`sys.system_server.start_count=1`, `ro.product.cpu.abilist=x86_64,arm64-v8a`,
`ro.dalvik.vm.native.bridge=libnb.so` and `ro.berberis.flags=accurate-sigsegv`, and
`su -c id` returned uid 0. The test app was a NativeActivity APK whose only code is one
`lib/arm64-v8a` library, built with NDK r27c. It installed with `primaryCpuAbi=arm64-v8a`.
nativeloader loaded the library, and the app process mapped `libnb.so` and the translation
proxies. The arm64 code logged and wrote a file in the app's data directory. The activity stayed
resumed with no crash or tombstone. As a control, the same APK on the plain public image was
refused with `INSTALL_FAILED_NO_MATCHING_ABIS`.

That boot showed that translation runs. It did not run any of the apps the earlier fixes were for
(TikTok, NIKKE, Amazon), or anything with Java code calling into arm64 JNI. The TikTok frame fix,
`nb_sigfix`, is a Zygisk module, so it rides the `play_spoof` stack and is not in this layer.

## play_spoof

This section records the acceptance target and its limits only. Installed paths, properties, init
steps, payload source and a COPY layer are still to write. Sources: `docs/FEATURES.md`,
`vendor/source-build/features/zygisk_pif/` (`README.md`, `devicespoof/post-fs-data.sh`),
`src/core/Features.cpp`, `src/core/Gating.h`, and Google's
[verdict definitions](https://developer.android.com/google/play/integrity/verdicts).

### The acceptance target, which is not a result

`play_spoof` is aimed at two things, and only two:

- the device verdict `MEETS_BASIC_INTEGRITY`;
- a Google account signing in to the Play Store, and apps then installing from it.

That is the chosen target, not a measurement. No instance has been shown to meet it: no Play
Integrity verdict, authenticated sign-in or Store install has been recorded for this feature. An
earlier check of the optional Play installer on the public base saw the Store open to its signed-out
screen, without any spoofing. That shows the Store starts. It says nothing about sign-in, installs
or a verdict, and it is not evidence for this target.

Even the basic verdict is not promised. Google describes `MEETS_BASIC_INTEGRITY` as basic system
checks that allow a locked or unlocked bootloader and verified or unverified boot, but on Android 13
and later it also requires a Google-provided attestation root of trust. Nothing in these sources
shows that a container can provide one. On Android 13 and later the basic and strong labels are
also optional labels, returned only when the app requesting the verdict is licensed (installed or
updated by Google Play). A response with no such label has not shown the target met.

### Device and strong integrity are not expected

Per Google's definitions, on Android 13 and later:

- `MEETS_DEVICE_INTEGRITY` requires hardware-backed proof of a locked bootloader and of a certified
  manufacturer's OS image;
- `MEETS_STRONG_INTEGRITY` requires `MEETS_DEVICE_INTEGRITY` plus security updates within the last
  year for every partition of the device.

A container on a desktop kernel has no hardware that can attest either. `devicespoof` sets the
boot-state properties apps read (`ro.boot.verifiedbootstate=green` and the like), which changes what
an app sees, not what hardware attests; the script itself says it cannot make Play Integrity pass.
So neither verdict is expected, and **apps that require the device or strong verdict are not
expected to work.** Some banking, payment and DRM-protected apps require one of them. Others check
less, or check something else, and an app's requirements can change on the server without an
update. Whether a particular app works is unknown until it is tried, and an app refusing for want
of device or strong integrity is the limit stated here, not a bug to fix with a stronger spoof.

### Never bundle, never fetch a keybox

Remora never bundles and never fetches an attestation keybox: the device-unique attestation key and
certificate chain provisioned into a real device's secure hardware. One used anywhere else has been
taken from the device it was issued to. Faking device or strong integrity needs one, which is why
the target stops at basic.

- **Never bundled:** no keybox in the public tree, a payload directory, an image, an add-on or a
  release, as a file or embedded in anything else.
- **Never fetched:** no script, build step, deploy step, add-on or runtime option downloads one,
  from any source, on any user's behalf.

`pifork/autopif4.sh`, the upstream refresher that does not run here (see the `zygisk_pif` README),
has an optional path for a separate key-attestation module. Remora ships no such module, and a
change that adds one, or a keybox for it, is refused.

### Before anyone says the target is met

None of this has been done. It is the evidence a later validation on real hardware has to produce,
and until it exists on `remora-6laf.6`, nothing (docs, changelog, release notes) says the target is
met:

1. **A fresh instance**: new, empty `/data`, of the public image with the optional add-on under
   test, and nothing else changed. Record exactly which image and add-ons. `public_release`
   conflicts with `play_spoof` in `src/core/Features.cpp`, so the record has to say how the spoof
   stack got onto that instance.
2. **The verdict as observed**: the device-recognition labels exactly as returned, saved
   unedited, with the app that requested them and how it was installed. Not a summary, and not an
   inference from an app's behaviour.
3. **Sign-in and installs**: a Google account signed in to the Play Store, and at least one app
   installed from it and launched.
4. **The record**: all of the above attached to `remora-6laf.6`, a failure recorded as a failure.

## shamiko

Shamiko v1.2.5 (414), a closed-source Zygisk module from LSPosed. It unmounts Magisk's module
modifications from processes on the DenyList. Sources: `vendor/source-build/features/zygisk_pif/`
(`shamiko/REMORA.md`, `shamiko/{module.prop,post-fs-data.sh,cleanup.sh,uninstall.sh,sepolicy.rule,
service.sh.disabled}`, `install.sh`, `60-hide-magisk-applets.sh`), `vendor/source-build/assemble-image.sh`,
`vendor/source-build/stage-features.sh`, `vendor/PAYLOADS.md`, `src/core/Features.cpp`,
`src/core/ImageBuild.cpp`, `src/engine/Deployers.cpp`, `.gitattributes`.

### How it reaches the build

`shamiko` requires `magisk_root` and `play_spoof` (`Features.cpp`). It sets `WITH_SHAMIKO=true`,
and only `assemble-image.sh` acts on it. There is no `.mk` and no `Android.mk` for it, and
`stage-features.sh` reads `WITH_SHAMIKO` only to refuse it in a public build. When
`WITH_PLAY_SPOOF=true`, `assemble-image.sh` refuses unless `shamiko/zygisk/x86_64.so` exists. If it
does, it runs `cp -a` on the whole `zygisk_pif/shamiko/` directory into the play_spoof staging
tree. That tree goes into the image as `COPY ps/system /system`. With `WITH_PLAY_SPOOF` unset the
block never runs, so `WITH_SHAMIKO` alone stages nothing. A public release refuses
`WITH_SHAMIKO=true` in both scripts.

### Installed paths

There are three stages, and only the first is image content. The image carries the directory as
it is. At deploy time, `install.sh` copies it into `/data` under the module id. At boot, the module's
own script generates more files.

| payload file (`zygisk_pif/shamiko/…`) | image path (`/system/etc/remora-spoof/shamiko/…`) | `/data` path after `install.sh` (`/data/adb/modules/zygisk_shamiko/…`) |
|---|---|---|
| `module.prop` | `module.prop` | `module.prop` |
| `post-fs-data.sh` | `post-fs-data.sh` | `post-fs-data.sh` |
| `cleanup.sh` | `cleanup.sh` | `cleanup.sh` |
| `uninstall.sh` | `uninstall.sh` | `uninstall.sh` |
| `sepolicy.rule` | `sepolicy.rule` | `sepolicy.rule` |
| `machikado` | `machikado` | `machikado` |
| `zygisk/x86.so` | `zygisk/x86.so` | `zygisk/x86.so` |
| `zygisk/x86_64.so` | `zygisk/x86_64.so` | `zygisk/x86_64.so` |
| `service.sh.disabled` | `service.sh.disabled` | removed by `install.sh` |
| `REMORA.md` | `REMORA.md` | removed by `install.sh` |

The image gets ten files in two directories. `/data` keeps eight of them. Of the ten, nine
are upstream bytes. `REMORA.md` notes two renames: `machikado` was `machikado.x86` in the zip,
and `service.sh.disabled` was `service.sh`. `REMORA.md` itself is Remora's own. `assemble-image.sh`
does not filter the directory, so the note and the disabled script reach the image. `install.sh`
is the step that drops them.

**Ownership and modes.** In the image, the tree is copied with `cp -a` and then through a
Dockerfile `COPY` with no `--chown`, so it is owned by 0:0. Modes are whatever the staged files had.
Git records all ten as `100644`. On `/data`, `install.sh` runs `chown -R 0:0`, sets every
directory to 0755 and every file to 0644, then removes any `disable` marker. That reproduces the
`set_perm_recursive "$MODPATH" 0 0 0755 0644` that `REMORA.md` quotes from upstream's
`customize.sh`. No file needs an executable bit, because the deploy runs `post-fs-data.sh` through
`sh`.

**Dropped from the upstream zip** (per `REMORA.md`): `META-INF/`, `customize.sh`, `verify.sh`, the
`.sha256` files, `README.md`, and `zygisk/{arm64-v8a,armeabi-v7a,riscv64}.so`. The container has
no ARM zygote for an ARM Zygisk library to load into.

**Generated at boot, never in the image.** `post-fs-data.sh` writes
`/data/adb/post-fs-data.d/.shamiko_cleanup.sh` from `cleanup.sh` (and `chmod +x`) if it is absent.
That copy runs only while `disable` exists. It resets `module.prop`'s `description` and then
deletes itself. `post-fs-data.sh` also touches `disable` when its version gate fails, below. It
creates `/data/adb/zygisksu/no_mount_znctl` only if `/data/adb/modules/zygisksu` exists. This image
installs ReZygisk as `rezygisk`, so that branch is not taken. `uninstall.sh` removes
`/data/adb/shamiko` and the cleanup copy. Nothing in Remora runs it: `install.sh` replaces the
module with `rm -rf` and `cp -a`. These sources do not show what creates `/data/adb/shamiko`.

**A checkout without the large-file objects.** `.gitattributes` stores `**/*.so` under the
features tree in git LFS. A clone that has not fetched those objects holds two 132-byte pointer
files at `zygisk/*.so`. `assemble-image.sh`'s check is `[ -f ]`, so a pointer passes it.
`machikado` is not an LFS path.

### Properties

None. Nothing that runs sets an Android property:

- `post-fs-data.sh` only reads versions (`magisk -V`, `su -V`) and writes files.
- Upstream's `service.sh`, the one script that would set properties (`resetprop -n` on
  `ro.debuggable`, `ro.secure`, `ro.adb.secure`, `ro.build.type`, `ro.build.tags`, the boot-state
  properties and more), ships only as `service.sh.disabled`. It never runs: `install.sh` deletes it
  from `/data`, and the deploy loop in `Deployers.cpp` starts a module's `service.sh` only if
  `[ -f ]` finds one. `remora_devicespoof` is the single source of property spoofing (`REMORA.md`).
- `module.prop` is Magisk module metadata, not a property file.

**Not to be confused with the above:** `install.sh` also writes two Magisk *database* settings
through `magisk --sqlite`: `zygisk=0` (ReZygisk replaces Magisk's built-in Zygisk) and
`denylist=1`. It adds the DenyList entries `com.google.android.gms`, its
`com.google.android.gms.unstable` process, `com.android.vending` and `com.google.android.gsf`.
These live in Magisk's database, not in the property area. They are written for the whole
play_spoof stack, with or without Shamiko.

### Init and runtime steps

Shamiko has no init rc, and the image adds nothing to `/system/etc/init/` for it. All activation
happens at deploy time and after, and only when the `play_spoof` runtime option is on
(`ctx.rc.playSpoof` in `Deployers.cpp`):

1. **Install.** `spoofModulesReady` requires `/system/etc/remora-spoof`, then runs a drift check.
   For `shamiko/` that check compares `post-fs-data.sh` byte for byte, compares the `versionCode`
   in `module.prop`, and compares each `zygisk/*` file against `/data/adb/modules/zygisk_shamiko`.
   On any difference, or if that directory is missing, it runs `install.sh`.
2. **Re-enable.** Every deploy removes a `disable` marker from `zygisk_shamiko` and the other spoof
   modules, and reports how many it cleared.
3. **Root gate.** The step fails unless `magisk` resolves, `magiskd` runs and `su -c true`
   succeeds. These are the `magisk_root` runtime steps.
4. **Module scripts.** After the framework is up, the deploy runs `post-fs-data.sh` for every
   enabled module except `rezygisk` and `playintegrityfix`, with
   `PATH=/debug_ramdisk:/sbin:$PATH`. Shamiko's is among them. Its version gate passes with
   Magisk 30.7: it disables the module below `27005`. If `magisk -V` printed nothing, it would take
   the Zygisk Next branch and disable itself. That is why the deploy puts the Magisk tmpfs on
   `PATH`, and why `60-hide-magisk-applets.sh` waits until `service.d` before it removes the `su`
   links.
5. **Loading.** The Zygisk implementation loads `zygisk/x86_64.so` and `zygisk/x86.so`, the two
   x86 builds. Here that implementation is ReZygisk, which comes from `play_spoof`. No Remora source loads it
   directly. `sepolicy.rule` has no Remora consumer. With SELinux disabled in the container it
   would have no effect anyway (see `magisk_root`).

The sources disagree on one point and do not settle it. `magisk.rc` runs `magisk --post-fs-data`
at boot, which upstream uses to run module scripts. A comment in `Deployers.cpp` says "this
container has no module lifecycle at all". Whether Magisk also runs Shamiko's `post-fs-data.sh` at
boot is not shown here.

On effect, the record is source comments from earlier sessions, not a test run for this document.
`60-hide-magisk-applets.sh` records that Shamiko loaded (the ReZygisk daemon logged its companion
socket) but did not remove the Magisk core mounts, because it only unmounts module modifications.
Neither `denylist=0`, `denylist=1` nor `no_mount_znctl` changed a mount.

### What it needs from the build, and what it does not

Only file placement. There is no compiled module, no sepolicy, no property and no init rc. Every
runtime step is done by `install.sh` and the deploy, both shipped by `play_spoof`, on top of the
`magisk_root` stack.

### Payload source

- **Supplied by the user.** `vendor/fetch-payloads.sh` has no Shamiko entry, and nothing fetches
  it. `REMORA.md` records that the vendored copy was taken from a copy of the release zip already
  at hand, not downloaded.
- **Recorded hash: the release archive only.** `Shamiko.zip` sha256
  `308d31b2f52a80e49eb58f46bc4c764a6588a79e4b8d101b44860832023f88b4`, with `module.prop` reading
  `id=zygisk_shamiko`, `version=v1.2.5 (414)`, `versionCode=414`. `REMORA.md` says all 18
  `.sha256` files in the zip were checked against the extracted contents (18 verified, 0
  mismatches) and the vendored files are byte-identical to the archive. Those per-file hashes are
  not recorded. The repository's LFS pointers record a sha256 and size for the two libraries:
  `zygisk/x86_64.so` `acaee7aa…`, 3434016 bytes, and `zygisk/x86.so` `c1e83a16…`, 2644456 bytes.
  Those identify the committed objects. They have not been checked against the zip's `.sha256`
  files here.
- **Licence: none.** `REMORA.md`, `vendor/PAYLOADS.md` and `NOTICE` record that there is no
  published source and no licence. Nothing grants redistribution. No Shamiko file except
  `REMORA.md` is in the public tree, and no mirrored image may carry it.
- **Remora's own:** `REMORA.md`, plus `install.sh`'s Shamiko branch and the deploy steps above,
  which belong to `play_spoof`.

### A COPY layer

No complete fragment is possible on `remora24:x86_64-public` today. Shamiko's own files can be
carried by a COPY layer, but on their own they install and activate nothing:

- **The play_spoof stack is a prerequisite and has no layer.** The install path is `install.sh`
  under `/system/etc/remora-spoof/`, and `spoofModulesReady` gates on that directory. The public
  base has neither, because a public release refuses `WITH_PLAY_SPOOF`. `install.sh` also
  needs ReZygisk, PlayIntegrityFork and `pif.json` beside it, and that inventory is still to write
  (see `play_spoof`). `play_spoof` and `shamiko` are not in `kOverlayable` (`ImageBuild.cpp`), so
  `remora recipe --base` prints no fragment for either and asks for a source build.
- **`magisk_root` is a prerequisite**, and its layer is above.
- **What COPY cannot supply:** the copy into `/data/adb/modules/zygisk_shamiko`, the ownership and
  mode reset, the removed files, the cleared `disable` marker, the Magisk database settings and
  DenyList entries, and the boot-generated cleanup script. All of these are runtime writes to
  `/data`, which is a volume and not part of the image. None needs a base-file edit.
- **Redistribution:** any image carrying these files is private by construction.

Shamiko's own part of such a layer would look like this. It is illustrative, not something Remora
prints. The build context's `zygisk_pif/` is `vendor/source-build/features/zygisk_pif/` with the
payload supplied:

```dockerfile
# Illustrative only: needs the magisk_root layer and a play_spoof layer that does not exist yet.
COPY zygisk_pif/shamiko/ /system/etc/remora-spoof/shamiko/
```

It places the same ten files as the image column of the table, with modes taken from the build
context. Like every Remora layer, the complete one would also need
`LABEL remora.public_release="false"`. No layer carrying Shamiko has been built or booted for
this document.
