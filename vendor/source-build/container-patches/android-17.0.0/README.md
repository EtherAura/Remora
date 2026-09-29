# Container-compat patches — android-17.0.0 (LineageOS 24 / API 37 tree)

These are the AOSP-tree source patches that make an Android 17 (CINNAMON_BUN / SDK 37) source
build boot as a container. Some are carried forward from earlier container-Android work (each
patch's `From:` line names its author); the rest are Remora's own. With this patch set + the
staged `device/remora` and `vendor/remora` trees, the image boots to `sys.boot_completed=1` and is
usable.

Apply to a freshly synced tree with the sibling script:

```sh
vendor/source-build/apply-container-patches.sh <TREE> \
    vendor/source-build/container-patches/android-17.0.0
```

(`git am --3way`, falling back to `git apply --3way`; re-applying is a no-op.)

## What each patch does

| Project | Fix |
|---------|-----|
| `system/core` | Split-init routing (`REMORA_CONTAINER` → `FirstStageMain`, first-stage built into second-stage binary); own cgroup ns + `/system/etc`→`/etc` bind; skip ramdisk first-stage-mount; first-stage errors non-fatal; second-stage re-exec forwarding `/proc/self/cmdline`; `/dev/ashmem` opened directly; cgroup v1 controllers `Optional`, v2 mount tolerates `EBUSY`; SELinux context computation short-circuited (`se_hack1`); `boot_config` reads `/proc/self/cmdline`; `init.rc`/`ueventd.rc` container tweaks. A second `0001` makes `property_service` parse `androidboot.*` from `/proc/self/cmdline` — in a container `/proc/cmdline` is the host kernel's, so without it no `ro.boot.*` value arrives at all. **0002**: libcutils memfd detection made sandbox-proof — `is_memfd_fd()` asks the fd via `F_GET_SEALS` before the `readlink(/proc/self/fd/*)` probe that Chromium's renderer sandbox denies, and the ashmem rdev probe uses the plain `/dev/ashmem` instead of the never-created `boot_id`-suffixed node; sandboxed WebView renderers failed every region validity check and composited black (bd remora-4ei.76) |
| `system/apex` | `apexd` creates the loop device node unconditionally + `mkdir /dev/block` (no `use_fiemap` data-APEX path) |
| `system/netd` | Make netd tolerate the container's iptables/BPF limits (mirrors the A16 container-compat netd fix): `IptablesRestoreController::execute` ignores iptables errors (`res=0`) so `networkAddInterface`'s incoming-mark rule succeeds and `eth0` gets its routes; `Controllers::init()` iptables + bandwidth non-fatal; `BandwidthController::updateQuota` non-fatal; `TetherController::getTetherStats` returns empty (stops `NetworkStatsService` throwing during `systemReady`, which aborted the ethernet NetworkAgent registration → no route → no adb); `main.cpp` `libnetd_updatable_init` non-fatal. Pairs with the host netfilter modules below. |
| `system/bpf` | bpf `Loader` failures non-fatal (BPF is global/partial in a container) |
| `system/vold` | ignore project-quota `ioctl` error |
| `system/hwservicemanager`, `system/libhwbinder` | boot fixes for the HIDL service manager and binder in a container |
| `system/libvintf` | skip the VINTF compatibility check, and tolerate a failed `/proc/config.gz` read |
| `external/minijail` | seccomp disabled |
| `external/selinux` | libselinux treats SELinux as disabled — the container runs without `/sys/fs/selinux` |
| `packages/modules/Connectivity` | `NetBpfLoad` proc-write / progId checks non-fatal; `ClatCoordinator::verifyClatPerms()` `abort()` → log (clat 464xlat BPF maps aren't pinned in the container, which has no NAT64) |
| `packages/modules/Telephony` | `framework-telecom` `min_sdk_version` 37 → 36 so `derive_classpath` keeps it on the runtime BOOTCLASSPATH at device SDK 36, matching the build boot image (49 == 49) — otherwise `boot.art` component layout mismatches and zygote dies |
| `frameworks/av` | bring back OMX; `SoftAVCEnc` build flags; the software-renderer colour-format workaround for Mesa video playback |
| `frameworks/native` | (0) binder boot fixes in `libbinder`. (1) SurfaceFlinger skips an invalid HWC colour mode (`-22`) instead of failing on it. (2) SurfaceFlinger marks the mirror agent's capture displays — `remora-mirror` and `remora-display` — secure, so protected (Widevine/HDCP) layers composite into the capture instead of blanking to black; DRM video otherwise plays on the secure primary display and shows black in the mirror. The agent runs as `shell` and cannot request `VIRTUAL_DISPLAY_FLAG_SECURE` itself, and the container has no real external output to protect (bd remora-wes) |
| `frameworks/base` | `ProcessList` cgroup write tolerance |
| `build/make` | keep `framework-telecom` on the bootclasspath as a platform `/system` jar (`default_art_config.mk` `RELEASE_TELECOM_MAINLINE_MODULE` else-branch) — `SystemServiceRegistry` hard-references `TelecomManager`, which lives only in that jar |
| `build/release` | cp2a: disable `require_gralloc4_or_newer`, so SurfaceFlinger keeps the gralloc 2 mapper the image's gralloc modules implement |
| `external/v4l2_codec2` | the Remora changes the `c2-va` VAAPI Codec2 component builds against: **0001** squashes the `VideoEncoder::handlesInputFormatConversion` hook for the HEVC encoder path (the compile blocker without it), decoder CPU_READ linear gralloc, `DecodeComponent` configUpdate picture-size and the `EncodeComponent` max-fps drop onto A17's 26Q2 base; **0002** declares AVC levels 5.2–6.2 so a full-resolution AVC encode configures; **0003** completes a skipped frame's work (`FLAG_DROP_FRAME` on `kAborted`) so it cannot stall the input pipeline |
| `external/libchrome` | put `hardware/remora/c2-va` back on libchrome's visibility list (A16 had it, A17 dropped it) — without it the build fails before `c2-va` emits a module |
| `lineage-sdk` | the LineageOS SDK against an AOSP 17 base: `LineageSettingsProvider` constants A17 removed, `LockSettings` visibility, and a `LineagePartsPreference` whose part is absent now removes its row instead of crashing the whole Settings screen (the image ships the Lineage Settings backport but not LineageParts) |
| `packages/apps/Launcher3` | grafts Trebuchet's Trust (hidden/protected apps) feature and branding onto AOSP Launcher3 |

## What lives elsewhere

The **product-level pieces** are native in `device/remora`, the device tree both releases lunch:
`service.sf.prime_shader_cache=false`, `apexd.config.use_fiemap=false`, `sys.use_memfd=true`
(ApplicationSharedMemory ashmem in a SELinux-disabled container), the software KeyMint HAL
(`android.hardware.security.keymint-service.nonsecure`, for Keystore2 `initUserSuperKeys`),
`PRODUCT_APEX_SYSTEM_SERVER_JARS += service-anomaly-detector service-telecom`, the
`PRODUCT_RELEASE_CONFIG_MAPS` for the `RELEASE_WEBAPP_MODULE` override, and the LineageOS product
config (`lineage.mk`, the appearance overlay, the dpi-aware Launcher3 workspaces and the branding).
None of it is a patch here: `device/remora` is the only device tree the image builds from, so
there is nothing else to patch it onto. To change that config, change `device/remora`.

**The GPU stack is not patched here either.** Mesa is built from source, so no prebuilt GPU
project is synced and there is nothing to trim. The GL cluster, the Vulkan ICDs and the VA aliases come from the `mesa_source` feature; the libdrm
backends and `amdgpu.ids` from `external/libdrm`; `iHD_drv_video` from
`vendor/source-build/c2-va/va.mk`; and `vulkan.lvp`, the one package with no other home, is
requested directly in `remora.mk`.

The two `external/` codec patches above only make sense together with **`hardware/remora/c2-va`**,
a whole source component (not a patch) that no repo manifest carries. It is vendored at
`vendor/source-build/c2-va/` and copied into the tree by `stage-features.sh`, so the order is:
`apply-container-patches.sh` → `stage-features.sh` → `do-build.sh`. `device/remora`'s `remora.mk`
inherits its `va.mk`, which is what imports the `c2-va` and `external/v4l2_codec2` soong namespaces
— without that import soong parses the `.bp` but emits no modules, and the build fails with
`unknown target …-service-vaapi`.

## Build requirements (not source patches)

Both are handled without anyone typing them:

- `DISABLE_DEXPREOPT_CHECK := true` is set in `device/remora`'s `BoardConfig.mk` — the declared
  `service-anomaly-detector` / `service-telecom` system-server jars have no dexpreopt artifacts in
  this config.
- `do-build.sh` prunes the orphaned platform ABI dump
  (`prebuilts/abi-dumps/platform/36/…/libcom.android.tethering.dns_helper.so.lsdump`) before
  building; a fresh `repo sync` restores it.

## Host requirement (not a source patch)

The docker host — the `bare` machine or the `remote` one — should have the **legacy iptables
netfilter modules** loaded (`ip_tables`, `iptable_{filter,mangle,nat,raw}` + the ip6 equivalents).
A modern host uses the nft backend and never loads them, while the container's netd uses legacy
iptables. The netd patch above tolerates their absence, but netd then spends seconds of every boot
retrying rules that cannot succeed, and the rules only actually apply when the modules are present.
This is version-independent, so it is not an image patch: the deploy checks for the modules and,
when any are missing, names the `modprobe` line and the `/etc/modules-load.d/remora-netfilter.conf`
file that persists it.

## Test-harness note (important)

When boot-testing, wipe `/data` **as root** between runs, e.g.
`docker run --rm -v <datavol>:/data alpine sh -c 'rm -rf /data/* /data/.[!.]*'`.
A user-level `rm` fails on root-owned files, leaving stale Keystore2 super-key blobs whose
password no longer matches — which surfaces as `initUserSuperKeys: Failed to decrypt key blob`
and looks like a code bug but is not.

## Version identity

Remora builds the `cp2a` release config, which targets SDK 37 REL. Under `cp1a` — which also
lunches — this tree finalizes as **SDK 36 / codename REL / release 16**, because
`prebuilts/sdk/37` is absent and API 37 is therefore unfinalized; the framework source still
contains `Build.VERSION_CODES.CINNAMON_BUN = 37`.
