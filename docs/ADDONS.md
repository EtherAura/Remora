# Add-ons: what each build feature puts into an image

For each payload-carrying build feature: every path it installs, the properties it sets, its init
and runtime steps, what it needs from the build itself, where its payload comes from, and whether a
Dockerfile `COPY` layer on the published base `remora24:x86_64-public` could carry it instead.

Everything here is read from the sources named in each section. None of it has been checked by
building, booting or deploying an image; a section saying a `COPY` layer *can* carry a file says
only that nothing in the build does more than place that file.

Sections still to write: `shamiko`, `arm_translate`, the inventory half of `play_spoof` (which so
far records only its acceptance target and limits), and the note that `webview_pairip_fix` has no
implementation and so needs no add-on.

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

Illustrative only. This has not been built, booted or deployed. The build context is
`vendor/source-build/features/magisk/` after `vendor/fetch-payloads.sh magisk`:

```dockerfile
FROM remora24:x86_64-public
COPY magisk/magisk magisk/magiskboot magisk/magiskinit magisk/magiskpolicy magisk/busybox \
     magisk/magisk.apk magisk/stub.apk \
     magisk/util_functions.sh magisk/boot_patch.sh magisk-setup.sh \
     /system/etc/init/magisk/
COPY magisk.rc /system/etc/init/magisk.rc
# The result is no longer a public image: do not keep the public label or a -public tag.
LABEL remora.public_release="false"
```

That fragment copies the same eleven files as the table: ten into `/system/etc/init/magisk/` and
the rc beside them.

Constraints the sources impose, which this document does not change:

- **The public base excludes root as policy.** `public_release` conflicts with `magisk_root` in
  `src/core/Features.cpp` (as policy, not for licence reasons). `stage-features.sh` refuses
  `WITH_MAGISK=true` in a public build. The base carries `LABEL remora.public_release="true"`
  (`assemble-image.sh`), and the release tooling accepts only `remora24:x86_64-public…` tags for
  public images. A derived image has to drop both the label and the tag.
- **Remora's overlay recipe does not offer this.** `src/core/ImageBuild.h` lists `magisk_root` as
  base-baked rather than overlay-able, and describes it as patching init and the boot flow. In
  these sources that boot-flow change is the rc file shown above. Teaching the recipe to overlay
  `magisk_root` would be a code change and is not part of this document.
- **File modes come from the build context.** A source build takes modes from the build's
  filesystem config; a COPY layer takes them from the context. From the sources, nothing needs an
  executable bit under `/system/etc/init/magisk/`: the rc runs both scripts through
  `/system/bin/sh`, and `magisk-setup.sh` copies the binaries into the tmpfs and `chmod`s them
  there.
- **Unproven on a COPY-derived image:** that init on the public base loads the added rc, that the
  `u:r:su:s0` execs succeed, and that root comes up. `PROVENANCE.md` says the root check is
  `su -c id` inside the container.

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
