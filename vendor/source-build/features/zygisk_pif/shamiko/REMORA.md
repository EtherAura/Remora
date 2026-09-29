# Shamiko — vendored for Remora (bd remora-4ei.50)

Root-hiding for denylisted apps. Shamiko unmounts Magisk's modifications from processes on the
denylist *before* they start, which is the part `install.sh` already assumed existed when it wrote
"keep gms/vending denylisted for Shamiko". Enabled by the `shamiko` build feature, which requires
`play_spoof`.

**This note is the only Shamiko file in the public tree.** Shamiko is closed-source freeware from
LSPosed with no published source and no licence, so nothing grants its redistribution: its files
are kept out of the public repository and out of every mirrored image. To build with it, obtain
v1.2.5 (414) yourself, check it against the hash below, and lay it out as the table under
*What is shipped* describes.

## Provenance

Not downloaded. Taken from a copy of the release zip already on the docker host.

```
Shamiko.zip  sha256 308d31b2f52a80e49eb58f46bc4c764a6588a79e4b8d101b44860832023f88b4
module.prop  id=zygisk_shamiko  version=v1.2.5 (414)  versionCode=414  author=LSPosed Developers
```

The zip ships a `.sha256` beside every payload file. All 18 were checked against the extracted
contents before anything was copied here: **18 verified, 0 mismatches**. The files below are
byte-identical to that archive.

> **Extraction gotcha.** 13 of the 41 entries use zip compression **method 95 (xz)**, which
> Info-ZIP's `unzip` and Python's `zipfile` both silently skip — `unzip -q` exits 0 having written
> only the other 28, so `module.prop`, `customize.sh` and every `zygisk/*.so` just quietly do not
> appear. `unzip -t` reports it as "13 files skipped because of unsupported compression or
> encoding", not as an error. Use `bsdtar -xf` (libarchive) or `7z`.

## What is shipped (in the image), and why it is not just the zip

Magisk never runs its installer here: Remora's `install.sh` copies this directory straight into
`/data/adb/modules`. So this directory reproduces exactly what `customize.sh` would have left in
`$MODPATH`, and nothing else.

| Shipped | Note |
|---|---|
| `module.prop`, `sepolicy.rule`, `post-fs-data.sh`, `uninstall.sh`, `cleanup.sh` | verbatim, the five files `customize.sh` extracts |
| `machikado` | **renamed from `machikado.x86`.** `customize.sh` does this rename itself; copying the zip name through would leave Shamiko looking for a file that is not there |
| `zygisk/x86.so`, `zygisk/x86_64.so` | the two `customize.sh` extracts for `ARCH=x86/x64` |
| `service.sh.disabled` | upstream `service.sh`, kept for review, **deliberately not executed** — see below |

Dropped: `META-INF/`, `customize.sh`, `verify.sh` (the flashable-zip installer path, unreachable
here), the `.sha256` files (nothing consumes them once `verify.sh` is gone — the verification is
recorded above instead), and `README.md`.

Also dropped: `zygisk/arm64-v8a.so`, `zygisk/armeabi-v7a.so`, `zygisk/riscv64.so` (12.6 MB). A
Zygisk library is loaded by a zygote, and this container has no ARM one — `ps -A` on the live A17
instance shows `zygote64`, `zygote`, `zygote_next` and `webview_zygote`, all x86. ARM apps run
under native-bridge translation *inside* those x86 processes, so there is no ARM zygote for a
Zygisk module to be loaded into. If that ever changes, add the file back.

## `service.sh` is disabled, and this is the load-bearing decision

Upstream's `service.sh` resets the verified-boot property surface. Six of its lines would break
Remora outright:

```
check_reset_prop "ro.debuggable"       "0"
check_reset_prop "ro.force.debuggable" "0"
check_reset_prop "ro.secure"           "1"
check_reset_prop "ro.adb.secure"       "1"
check_reset_prop "ro.build.type"       "user"
check_reset_prop "ro.build.tags"       "release-keys"
```

Remora drives the guest entirely over adb. `remora_devicespoof` (bd remora-4ei.52) spoofs the same
identity surface and *deliberately* leaves `ro.debuggable` and `ro.build.type` real, with the reason
written into its own script: adb root has to keep working. Shipping `service.sh` would undo that
decision from a second file.

That is also the anti-pattern bd remora-4ei.42 was filed for — two sources of truth for one thing,
where the one that happens to run last silently wins. **`remora_devicespoof` is the single source of
truth for property spoofing on this image.** Shamiko is here for the one thing devicespoof cannot
do: unmounting Magisk's own modifications from denylisted processes.

The overlap is nearly total anyway — devicespoof already covers `verifiedbootstate`,
`flash.locked`, `veritymode`, `vbmeta.device_state`, `warranty_bit` and `bootmode`. What upstream's
`service.sh` covers and devicespoof does not is `sys.oem_unlock_allowed`, `ro.vendor.warranty_bit`,
`vendor.boot.*` and the MIUI/Realme lock-state props. Folding those into devicespoof is bd
remora-ckl; do it there, not by re-enabling this file.

## Install-time contract this directory depends on

- **Module id is `zygisk_shamiko`, not `shamiko`.** Magisk keys a module off its *directory name*,
  and upstream's `cleanup.sh` hardcodes `/data/adb/modules/zygisk_shamiko`. `install.sh` therefore
  copies this directory to that name. The staged directory here keeps the short name because
  `install.sh`'s existing `[ -d "$SRC/shamiko" ]` gate was written that way.
- **Permissions**: `customize.sh` ends with `set_perm_recursive "$MODPATH" 0 0 0755 0644`;
  `install.sh` reproduces it.
- **`disable` marker**: `install.sh` clears it, exactly as it already does for rezygisk and
  playintegrityfix — a stray `disable` file is what kept ReZygisk from ever starting once before.
- **Magisk version gate**: `post-fs-data.sh` self-disables below Magisk `27005`. This image ships
  upstream Magisk `30.7` / `MAGISK_VER_CODE=30700`, so it passes. If Magisk is ever downgraded,
  Shamiko will silently turn itself off by touching its own `disable`.

## Known unknown

`post-fs-data.sh` special-cases Zygisk Next by checking for `/data/adb/modules/zygisksu`, and this
image installs ReZygisk at `/data/adb/modules/rezygisk`. That branch therefore does not fire, so
`/data/adb/zygisksu/no_mount_znctl` is never created. ReZygisk is a Zygisk Next derivative, so it
may or may not want that marker. Untested — check it if denylist unmounting misbehaves.
