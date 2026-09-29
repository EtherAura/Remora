# Which Magisk is this?

**UPSTREAM Magisk (topjohnwu) v30.7 (30700).** Not Magisk Delta — see "History" below, because
this directory used to hold Delta and the swap is the whole point of this file.

Nothing about the files themselves tells you which project they came from: Delta is a fork that
keeps upstream's version string and the `topjohnwu` symbols, so `magisk -v` reports `30.x:MAGISK:R`
either way and cannot distinguish them. Trust this file and the hashes, not the binary.

## Why upstream, and what had to be built to get here

Magisk normally installs by patching a boot image: `magiskinit` runs as PID 1 and builds the tmpfs
environment (`/debug_ramdisk`) before init proper starts. A container has no boot image to patch and
no PID-1 slot to take, so Remora drives Magisk from `init.rc` — and the one thing upstream offers no
CLI for is *building that environment*. Delta added `--setup-sbin` for exactly this case, and that
single flag was the only reason Remora ever shipped a fork.

`magisk-setup.sh` (one directory up) now does that job in ~40 lines of shell, so the fork bought us
nothing and cost us a licence problem. Verified in a container: `magisk -V` → 30700,
`magisk --path` → `/debug_ramdisk`, magiskd running, `su -c id` → `uid=0(root)`, denylist enforced
and accepting packages.

**This also unblocks redistribution.** Magisk is GPLv3, so shipping these binaries obliges us to
offer matching source. Delta's did not survive: `HuskyDG/Magisk-Delta`, `HuskyDG/magisk-files` and
`huskydg/Magisk-Delta` all return 404 — the account itself is gone — leaving no source to offer at
any price. Upstream's
is live at `github.com/topjohnwu/Magisk`, tag `v30.7`. That is a hard requirement, not a preference
— see `vendor/PAYLOADS.md` and ADR-0001.

## History: the swap that failed, and why this one did not

Upstream was tried once before and reverted (`bd remora-82c.13`). It is worth knowing why, because
**every deploy step reported green**. `--auto-selinux --setup-sbin` is Delta-only; upstream answers
`Unrecognized argument` and exits, so the tmpfs was never built and every later `magisk.rc` exec
failed silently. Nothing surfaced it: ReZygisk still injected, because Remora starts that itself
rather than through magiskd. The only symptoms were a longer boot animation, a grey screen until an
input event, and the Magisk app still showing the old version.

The difference this time is that the environment is built explicitly and the rc was rewritten to
upstream's command set. **If you touch either, re-verify `su -c id` inside the container** — a green
build and a running ReZygisk do not mean root works.

Two live consequences of the swap:

* the app package is `com.topjohnwu.magisk`, not `io.github.huskydg.magisk`. The projects sign with
  different keys, so a `/data` that still carries the Delta app gives
  `INSTALL_FAILED_UPDATE_INCOMPATIBLE` — uninstall it, or start from a fresh `/data`.
* `--auto-selinux` has no upstream equivalent. Harmless here (this container runs with SELinux
  disabled, no `/sys/fs/selinux`), which is why `magisk.rc` calls `magiskpolicy --live` separately.
  If Remora ever runs enforcing, `magisk-setup.sh` is where labelling would go.

## Contents

All extracted from the official release apk (`boot_patch.sh` happens to be byte-identical to
Delta's). The public tree carries only the two GPL-3.0 shell scripts; the binaries and apks are
fetched.

| vendored file       | from inside the apk           | sha256 (v30.7)     |
|---------------------|-------------------------------|--------------------|
| `magisk`            | `lib/x86_64/libmagisk.so`     | `a1cc6bea9e8618f8`  |
| `magiskboot`        | `lib/x86_64/libmagiskboot.so` | `a18ecbd798117949`  |
| `magiskinit`        | `lib/x86_64/libmagiskinit.so` | `24557786f5966237`  |
| `magiskpolicy`      | `lib/x86_64/libmagiskpolicy.so`| `0021aaefac5f5a49` |
| `busybox`           | `lib/x86_64/libbusybox.so`    | `060844b0769f7a50`  |
| `magisk.apk`        | the release apk itself        | `e0d32d2123532860`  |
| `stub.apk`          | `assets/stub.apk`             | `f0230e0864be255d`  |
| `util_functions.sh` | `assets/util_functions.sh`    | `efe385ffdb72c7fc`  |
| `boot_patch.sh`     | `assets/boot_patch.sh`        | `20de7208d610a267`  |

`magiskinit`, `magiskboot`, `boot_patch.sh` and `stub.apk` are **boot-image tooling and unused at
runtime here** — a container never patches a boot image. They are kept so the payload is a complete,
checkable copy of one upstream release rather than a subset someone has to reason about.

## Updating

`vendor/fetch-payloads.sh magisk` stages exactly this table from the official release: it downloads
the apk, refuses it unless its sha256 matches the one recorded in the script, and extracts the files
above.

Take the next release from `github.com/topjohnwu/Magisk/releases`, extract the table above, and
record the new version and hashes here. Update `MAGISK_VER` and `MAGISK_APK_SHA256` in
`fetch-payloads.sh` to match, re-verify root in a container as described above, and update the
version in `vendor/PAYLOADS.md`.
