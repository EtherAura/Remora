#!/system/bin/sh
# Install the baked ReZygisk + PlayIntegrityFix stack (from /system/etc/remora-spoof) into
# /data/adb/modules (Magisk's volume). Idempotent; run by bringup on the first boot of a fresh
# /data when the play_spoof runtime option is on. Shamiko (root-hiding) is optional — installed
# only if it was staged alongside.
SRC=/system/etc/remora-spoof
mkdir -p /data/adb/modules
# REPLACE, never copy-into. `cp -a src dst` when dst ALREADY EXISTS copies src *inside* it, so a
# RE-install left the module's own files untouched and dropped a useless nested subdirectory
# (/data/adb/modules/remora_devicespoof/devicespoof/). First install worked, every subsequent one
# silently delivered nothing — which is exactly what happened the first time the content-drift gate
# asked for a re-install (bd remora-ckl). /data survives every recreate, so
# "subsequent" is the normal case, not the rare one.
inst() {  # <staged-dir-name> <installed-module-name>
  [ -d "$SRC/$1" ] || return 0
  rm -rf "/data/adb/modules/$2"
  cp -a "$SRC/$1" "/data/adb/modules/$2"
}
inst rez-mod          rezygisk
inst playintegrityfix playintegrityfix
# nb_sigfix — berberis app workarounds (bd remora-4ei.99 / .100). Staged only when ARM translation is in
# the image; a plain Zygisk module, enabled like the others below.
if [ -d "$SRC/nb_sigfix" ]; then
  inst nb_sigfix nb_sigfix
  chown -R 0:0 /data/adb/modules/nb_sigfix 2>/dev/null
  find /data/adb/modules/nb_sigfix -type d -exec chmod 0755 {} \; 2>/dev/null
  find /data/adb/modules/nb_sigfix -type f -exec chmod 0644 {} \; 2>/dev/null
  rm -f /data/adb/modules/nb_sigfix/disable
fi
# Shamiko installs as zygisk_shamiko, NOT shamiko: Magisk keys a module off its directory name, and
# upstream's own cleanup.sh hardcodes /data/adb/modules/zygisk_shamiko. Under the short name that
# script silently edits a path that does not exist. Perms mirror customize.sh's closing
# set_perm_recursive 0 0 0755 0644, and service.sh is shipped disabled — remora_devicespoof is the
# single source of truth for property spoofing here, and upstream's would set ro.debuggable=0 and
# ro.adb.secure=1 and take adb out with it. See shamiko/REMORA.md (bd remora-4ei.50).
if [ -d "$SRC/shamiko" ]; then
  inst shamiko zygisk_shamiko
  rm -f /data/adb/modules/zygisk_shamiko/REMORA.md /data/adb/modules/zygisk_shamiko/service.sh.disabled
  chown -R 0:0 /data/adb/modules/zygisk_shamiko 2>/dev/null
  find /data/adb/modules/zygisk_shamiko -type d -exec chmod 0755 {} \; 2>/dev/null
  find /data/adb/modules/zygisk_shamiko -type f -exec chmod 0644 {} \; 2>/dev/null
fi
# System-wide identity spoof — PIF only injects into GMS, so without this every OTHER app still
# reads remora/userdebug/test-keys and any self-rolled root check sees a container (bd remora-4ei.52).
if [ -d "$SRC/devicespoof" ]; then
  inst devicespoof remora_devicespoof
  chmod 0755 /data/adb/modules/remora_devicespoof/post-fs-data.sh
fi
cp "$SRC/pif.json" /data/adb/modules/playintegrityfix/pif.json
cp "$SRC/pif.json" /data/adb/modules/playintegrityfix/custom.pif.json
chown -R 0:0 /data/adb/modules/rezygisk /data/adb/modules/playintegrityfix 2>/dev/null
find /data/adb/modules/rezygisk /data/adb/modules/playintegrityfix -type d -exec chmod 0755 {} \; 2>/dev/null
find /data/adb/modules/rezygisk/bin -type f -exec chmod 0755 {} \; 2>/dev/null
chmod 0644 /data/adb/modules/rezygisk/lib64/* /data/adb/modules/rezygisk/lib/* /data/adb/modules/playintegrityfix/zygisk/* 2>/dev/null
# ENABLE the modules (a stray 'disable' file is what kept ReZygisk from ever starting).
rm -f /data/adb/modules/rezygisk/disable /data/adb/modules/playintegrityfix/disable \
      /data/adb/modules/zygisk_shamiko/disable
# ReZygisk replaces Magisk's built-in Zygisk; keep gms/vending denylisted for Shamiko (ReZygisk
# still injects them for PIF).
# Resolve the magisk CLI rather than hardcoding /sbin: magisk.rc moved MAGISKTMP to
# /debug_ramdisk (bd remora-4ei.94, so no anomalous /sbin directory exists for an app to stat),
# but images built before that change still keep it at /sbin, and this script runs against both.
# Fail loudly if neither answers — every command below is a silent no-op without it, which is
# exactly how DroidGuard went unlisted for as long as this file existed.
MAGISK=
for m in /debug_ramdisk/magisk /sbin/magisk; do [ -x "$m" ] && { MAGISK="$m"; break; }; done
if [ -z "$MAGISK" ]; then
  echo "remora play_spoof: FATAL magisk CLI not found at /debug_ramdisk or /sbin — root stack is" \
       "not up; denylist and zygisk settings NOT applied" >&2
  exit 1
fi
"$MAGISK" --sqlite "UPDATE settings SET value=0 WHERE key='zygisk'" 2>/dev/null
"$MAGISK" --sqlite "REPLACE INTO settings (key,value) VALUES('denylist',1)" 2>/dev/null
# "<pkg>" or "<pkg> <process>" — the denylist is keyed on the PROCESS, not the package, and
# `--denylist add com.google.android.gms.unstable` is not a shorthand for the second form: magisk
# rejects it outright with "Invalid package / process name" and exits 1. That is how DroidGuard
# came to be missing from the list for as long as this file has existed — the error went to
# /dev/null and the loop carried on, so the list looked deliberate while the ONE process Play uses
# to look for root was never covered by Shamiko (bd remora-82c.6).
#
# com.google.android.gms.unstable is DroidGuard, the process that computes the Play Integrity
# verdict. It is not optional here; it is the whole point of the list.
# Confirmed by MEMBERSHIP, not by exit status: magisk exits non-zero for "Target already exists in
# denylist", so a status check would report a false failure on every re-install — and this script
# is re-run on every deploy, so that is the normal case, not the rare one.
denylist_add() {  # <pkg> [process]
  proc="${2:-$1}"
  "$MAGISK" --denylist add "$1" ${2:+"$2"} >/dev/null 2>&1
  "$MAGISK" --denylist ls 2>/dev/null | grep -qx -- "$1|$proc" && return 0
  echo "remora play_spoof: WARNING could not denylist $proc — root stays visible to it" >&2
}
denylist_add com.google.android.gms
denylist_add com.google.android.gms com.google.android.gms.unstable
denylist_add com.android.vending
denylist_add com.google.android.gsf
echo "remora play_spoof: modules installed to /data/adb/modules"
