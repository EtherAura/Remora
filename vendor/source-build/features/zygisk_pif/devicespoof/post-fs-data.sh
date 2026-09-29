#!/system/bin/sh
# System-wide device identity, bd remora-4ei.52.
#
# WHY THIS EXISTS ALONGSIDE PIF: PlayIntegrityFix injects only into GMS, vending and gsf, so every
# OTHER app still reads ro.product.model=remora17_x86_64, ro.hardware=remora, ro.build.tags=
# test-keys. Any app doing its own root/emulator check therefore sees a container regardless of how
# perfect the GMS-scoped spoof is. This closes that gap for the rest of the system.
#
# DELIBERATELY NOT SPOOFED — read before adding any:
#   ro.debuggable    leaving it 1 keeps adb root working, which Remora depends on. PIF already
#                    presents 0 to GMS, which is where it is actually checked.
#   ro.build.type    same reason; userdebug is what makes adb root available.
#   ro.serialno      empty here; inventing one risks colliding with real-device logic.
# Everything below is identity only, so the mirror, adb and Remora's probes keep working.
#
# NOTE this cannot make Play Integrity pass — the CPU is an Intel i9 claiming to be a Tensor,
# SELinux reads Disabled, and the kernel is a desktop one (bd remora-4ei.51). It is for the much
# larger class of apps that do simpler checks.
[ "$(getprop ro.remora.devicespoof)" = "0" ] && exit 0

# Resolve resetprop explicitly. It is a magisk applet and /sbin is not reliably on PATH for
# every caller (a plain `docker exec sh` has no /sbin), so a bare `resetprop` silently no-ops.
RP=
for c in /sbin/resetprop /debug_ramdisk/resetprop "$(command -v resetprop 2>/dev/null)" /sbin/magisk /debug_ramdisk/magisk; do
  [ -x "$c" ] && { case "$c" in *magisk) RP="$c resetprop" ;; *) RP="$c" ;; esac; break; }
done
[ -n "$RP" ] || { log -t remora_devicespoof "resetprop not found — identity spoof SKIPPED"; exit 0; }

set_p() { $RP -n "$1" "$2"; }

# identity
set_p ro.product.model         "Pixel 8 Pro"
set_p ro.product.manufacturer  Google
set_p ro.product.brand         google
set_p ro.product.name          husky
set_p ro.product.device        husky
set_p ro.build.product         husky
set_p ro.product.board         husky
# ro.board.platform / ro.hardware / ro.boot.hardware / ro.hardware.egl ARE NOT SPOOFED, and this is
# not an oversight — they are HAL SELECTORS, not identity. Android resolves gralloc/hwcomposer/EGL
# by name from them, so claiming husky/zuma/mali sends SurfaceFlinger looking for Pixel HALs this
# image does not contain. Measured: with them set, surfaceflinger stayed up but
# system_server NEVER STARTED (0 processes) and servicemanager looped on "SurfaceFlingerAIDL could
# not be found"; the device booted to sys.boot_completed=1 with no framework, no settings service
# and no sensorservice. Recovered only by disabling the module.
# ORDERING DOES NOT RESCUE THIS. Running the module after boot leaves the already-loaded HALs
# alone, so it looks fine — until the next framework restart, which Remora does routinely (fps
# pinning, input re-registration, every reconnect). Tried and reproduced: the framework came back
# dead the moment it bounced.
# That is also why bd remora-4ei.52's "VERIFIED LIVE" reading held — it was a hand-run on a live
# device whose HALs were already resolved, a state that cannot survive a restart.
# ro.soc.* below is safe: nothing resolves a HAL from it.
set_p ro.soc.manufacturer      Google
set_p ro.soc.model             "Tensor G3"
set_p ro.build.characteristics nosdcard
set_p ro.bootloader            husky-15.2-13158461
set_p ro.boot.bootloader       husky-15.2-13158461
set_p gsm.version.baseband     g5300q-260701-260705-B-13158461

# BUILD FINGERPRINT — the single most-read identity string there is, and it was the ONE thing this
# module left honest. Every *.build.fingerprint read
#   remora/remora_x86_64/remora_x86_64:17/<id>/eng.root:userdebug/test-keys
# while ro.build.type next to it already said "user" and ro.build.tags said "release-keys". That
# self-contradiction is a STRONGER tamper signal than an unspoofed fingerprint would have been: an
# app comparing the two learns the device is lying. Measured with a root-artifact probe
# run as a real app uid, the fingerprint was one of only four leaks left after the DenyList had
# removed su and the magisk binaries.
#
# COMPOSED, NOT HARDCODED. The value is built from the properties this script has already set plus
# the image's own release/id/incremental, so it cannot drift out of step with them the way a pasted
# literal would. Format is brand/product/device:release/id/incremental:type/tags.
#
# NOT COPIED FROM PIF ON PURPOSE. PIF's pif.json ships a husky_beta CANARY fingerprint for GMS; its
# version field is "CANARY" where this image's ro.build.version.release is "17", so adopting it
# system-wide would trade one contradiction for another. PIF stays GMS-scoped and this stays
# system-scoped; each is internally coherent, which is what a checker actually tests.
#
# SAFE, unlike the HAL selectors above: nothing resolves a driver or HAL from a fingerprint.
_rel=$(getprop ro.build.version.release)
_id=$(getprop ro.build.id)
_inc=$(getprop ro.build.version.incremental)
_fp="google/husky/husky:${_rel}/${_id}/${_inc}:user/release-keys"
for _p in ro.build.fingerprint ro.system.build.fingerprint ro.vendor.build.fingerprint \
          ro.product.build.fingerprint ro.odm.build.fingerprint ro.system_ext.build.fingerprint \
          ro.system_dlkm.build.fingerprint ro.vendor_dlkm.build.fingerprint \
          ro.bootimage.build.fingerprint; do
  set_p "$_p" "$_fp"
done
# The same three strings leak the build host and builder by other routes: ro.build.description
# repeats "remora_x86_64-userdebug ... eng.root test-keys" verbatim, ro.build.host is the BUILDER
# CONTAINER'S HOSTNAME (a bare docker hex id, which no real device ever reports) and ro.build.user
# is "root". Aligned with the fingerprint above.
set_p ro.build.description "husky-user ${_rel} ${_id} ${_inc} release-keys"
set_p ro.build.host        abfarm-release
set_p ro.build.user        android-build

# verified boot — these read empty/unknown here, which is itself a giveaway
set_p ro.boot.verifiedbootstate    green
set_p ro.boot.flash.locked         1
set_p ro.boot.veritymode           enforcing
set_p ro.boot.vbmeta.device_state  locked
set_p ro.boot.vbmeta.avb_version   1.3
set_p ro.boot.warranty_bit         0
set_p ro.warranty_bit              0
set_p ro.boot.selinux              enforcing
set_p ro.bootmode                  normal
set_p ro.boot.mode                 normal

# The vendor-namespace twins, folded in from Shamiko's service.sh (bd remora-ckl). Shamiko ships
# with its own service.sh DISABLED here — see shamiko/REMORA.md — so anything of its prop set that
# is worth having has to live in THIS file, which is the single source of truth for property
# spoofing on this image. These matter because the INCONSISTENCY is the tell: an app reading a
# spoofed ro.boot.verifiedbootstate=green next to an honest, empty vendor.boot.verifiedbootstate
# learns more than it would from either alone.
set_p sys.oem_unlock_allowed            0
set_p ro.vendor.warranty_bit            0
set_p ro.vendor.boot.warranty_bit       0
set_p vendor.boot.vbmeta.device_state   locked
set_p vendor.boot.verifiedbootstate     green
# DELIBERATELY NOT TAKEN from Shamiko's list, so nobody "completes" it later by copying upstream:
#   ro.secureboot.lockstate / ro.boot.realmebootstate / ro.boot.realme.lockstate — MIUI and Realme
#     specific. On a device claiming to be a Pixel 8 Pro they are their own small incoherence.
#   ro.bootmode / ro.boot.bootmode / vendor.boot.bootmode "recovery" -> "unknown" — this container
#     never boots from recovery, and bootmode is already pinned to normal above.
#   ro.debuggable / ro.force.debuggable / ro.secure / ro.adb.secure / ro.build.type /
#     ro.build.tags — these would take adb out, which is Remora's entire control path. Never.

# emulator negatives
set_p ro.kernel.qemu 0
set_p ro.boot.qemu   0

log -t remora_devicespoof "device identity spoofed system-wide (model=$(getprop ro.product.model))"
