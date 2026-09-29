#!/system/bin/sh
# Drop the `su` / `resetprop` / `supolicy` applet symlinks out of /system/xbin (bd remora-4ei.101).
#
# THE PROBLEM. Magisk magic-mounts a tmpfs over /system/xbin so it can add its own binaries beside
# the one the image ships there (overlay_remounter), and the applet symlinks it creates land in a
# directory that IS ON EVERY APP'S PATH — the image's own ENV line ends
# "…:/system/bin:/system/xbin:/vendor/bin". So `which su` resolves from any app, which is the single
# most common root probe there is. Measured: NIKKE and Reddit had byte-identical mount namespaces
# (179 mounts each, 8 magisk-related each) and both could stat /system/xbin/su.
#
# WHY THE DENYLIST DOES NOT COVER IT, checked rather than assumed. Shamiko loads and runs here
# (zygiskd64 logs "Sending companion fd socket of module zygisk_shamiko" and its companion answers),
# apps DO get their own mount namespaces (zygote 4026536619 vs NIKKE 4026536671 vs Reddit
# 4026536664), and the container is privileged with CAP_SYS_ADMIN — so unmounting is both possible
# and permitted. It still does not happen, because the eight remaining mounts are Magisk CORE
# (MAGISKTMP plus this /system/xbin applet tmpfs), not module magic-mounts, and unmounting module
# modifications is the whole of what Shamiko does. Also ruled out by measurement: denylist=0 (the
# enforce-off setting Shamiko's own docs ask for), denylist=1, and creating
# /data/adb/zygisksu/no_mount_znctl (the "known unknown" shamiko/REMORA.md flagged for exactly this
# symptom). None of the three changed a single mount.
#
# SO REMOVE THE EXPOSURE INSTEAD OF TRYING TO HIDE IT. Root still works — it just stops advertising
# itself on the app PATH.
#
# TIMING: service.d, not post-fs-data.d. Module post-fs-data scripts run bare `magisk -V` and
# `su -V` (Shamiko's does, and takes a wrong branch that can `touch disable` if magisk is not
# resolvable), so these links must survive the post-fs-data pass and only go afterwards. service.d
# runs at late_start, long before any app can launch.
#
# WHAT STILL WORKS, verified live before this script was written:
#   - Remora's own root probe: it already exports PATH=/debug_ramdisk:/sbin:$PATH (Deployers.cpp),
#     so `magisk`, `magiskd` and `su -c true` all still resolve. Unchanged by this.
#   - AppTransfer's `su -c` over adb: it did NOT prepend that PATH and DID break — fixed in the same
#     change, the same way.
# `magisk`/`magiskpolicy` themselves are deliberately left in place: module scripts reach for them
# by bare name, and a binary called `magisk` is a far less common probe target than `su`.

XBIN=/system/xbin
[ -e "$XBIN/su" ] || [ -e "$XBIN/resetprop" ] || [ -e "$XBIN/supolicy" ] || exit 0

# The tmpfs is mounted read-only; flip it just long enough to unlink, then put it back.
mount -o remount,rw "$XBIN" 2>/dev/null || {
    log -t remora_hide_applets "cannot remount $XBIN rw — applet symlinks left in place"
    exit 0
}
rm -f "$XBIN/su" "$XBIN/resetprop" "$XBIN/supolicy" 2>/dev/null
mount -o remount,ro "$XBIN" 2>/dev/null

log -t remora_hide_applets "removed su/resetprop/supolicy from $XBIN (remaining: $(ls "$XBIN" | tr '\n' ' '))"
