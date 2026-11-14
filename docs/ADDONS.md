# Add-ons: what each build feature puts into an image

For each payload-carrying build feature: every path it installs, the properties it sets, its init
and runtime steps, what it needs from the build itself, where its payload comes from, and whether a
Dockerfile `COPY` layer on the published base `remora24:x86_64-public` could carry it instead.

Everything here is read from the sources named in each section. None of it has been checked by
building, booting or deploying an image; a section saying a `COPY` layer *can* carry a file says
only that nothing in the build does more than place that file.

Sections still to write: `play_spoof`, `shamiko`, `arm_translate`, and the note that
`webview_pairip_fix` has no implementation and so needs no add-on.

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
