# device/remora

Remora's own device tree — the container-Android product definition that `remora_x86_64`
lunches, on Android 16 and 17 alike. This is R1 of the reimplementation plan in bd
remora-28ix.4: the device tree is **written**, not forked, so that every line of the device config
is something Remora wrote, understands, and can defend. Like `vendor/remora` it is staged into the
tree by `stage-device-tree.sh`, not synced — no manifest can provide Remora-authored source.

## Provenance and honesty

This tree is Remora-authored. A device tree is not a place to be original: where a value here
matches established container-Android configuration, that is because it is the correct value for
a containerized Android and we know why. Third-party notices are in Remora's `NOTICE`. Beyond
that it draws on two sources, and says so:

- **Remora's own bringup work.** The Android 17 boot fixes (apexd fiemap, SF shader-cache
  priming, sys.use_memfd, software KeyMint, the telecom/anomaly bootclasspath dance), the
  release-config map, the c2-va sepolicy, and the no-arm32 honesty rule were all worked out
  in Remora first and are simply *native* here instead of arriving as patches.
- **Verbatim platform data.** Three files are copied, not rewritten, because rewriting
  them would be theater: `mediacodec.policy.x86` (a seccomp syscall list),
  `android.hardware.bluetooth@1.1.xml` and `manifest.xml` (VINTF declarations dictated by
  the HALs the image ships). They are data about the platform, not design.

## The hardware identity

`ro.hardware` is **remora** since R4 (bd remora-emoe) — but **not from this tree**. It is set
by the image ENTRYPOINT's `androidboot.hardware`, in `assemble-image.sh`, because init copies
`ro.boot.hardware` into `ro.hardware` before it ever reads `/vendor/build.prop` and `ro.*` is
write-once. An `ro.hardware` line in `remora.prop` could therefore never take effect;
`remora.prop` records that as a deliberate absence. No `init.${ro.hardware}.rc` is shipped
either: init imports it from `/vendor/etc/init/hw/`, a directory this image does not have, so a
placeholder anywhere else would be read only by the generic directory scan.

`ro.hardware` is `hw_get_module`'s first variant key. `gpu_config.sh` names
`ro.hardware.hwcomposer` explicitly in both GPU modes, so no HAL reaches that fallback on a
healthy boot, and Remora's own `hwcomposer.remora` is the composer. Audio is not in this set:
`remora.mk` requests AOSP's own audio HALs.

No prebuilt GPU stack is inherited. The Mesa drivers and their dependency closure come from a
source build (the `mesa_source` feature, which a source build of this product requires — see
`vendor/host-prereqs/mesa-android/README.md`), and the composer is `hwcomposer.remora`; a profile
that still pins `hwcomposer=` to a retired prebuilt composer must be repointed at `remora` or left
unset. The *product* identity (lunch target, brand, model, device) is remora's.

The device tree also owns the vendor media configuration (`media_codecs.xml`, `media_profiles.xml`)
and the LineageOS product config (`lineage.mk`, `lineage-overlay/`, `overlay/`). The media list
used to arrive with an optional HAL's makefile, and dropping that HAL took the top-level
`/vendor/etc/media_codecs.xml` with it — the image then advertised no codecs. Device config lives
here, never with an optional component.

## What this tree deliberately leaves out

- The arm64 and `_only` product variants: Remora builds `remora_x86_64`, and a config
  that is never built is a config that silently rots.
- `TARGET_NATIVE_BRIDGE_2ND_*` (32-bit ARM advertisement): never backed by a translator;
  a 32-bit-ARM-only APK must be refused at install, not die at first native call
  (bd remora-rve).
- The standalone `device_sepolicy` / `no_arm32_bridge` source patches: their content is
  native here.

## Building

Android 17 lunches `remora_x86_64-cp2a-userdebug` (cp2a = the genuine Android 17 release config;
the cp1a combo also lunches — the `release/` map carries the WEBAPP flag override it needs).
Android 16 lunches `remora_x86_64-bp2a-userdebug`. Remora composes the lunch string from its
release registry (`lunchConfigFor()` in `src/core/ImageBuild.cpp`), so a normal build never types
it.
