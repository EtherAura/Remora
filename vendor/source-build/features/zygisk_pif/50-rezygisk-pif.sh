#!/system/bin/sh
# Remora boot bootstrap for patched ReZygisk + PlayIntegrityFix.
#
# Runs at post-fs-data (magiskd executes /data/adb/post-fs-data.d/* before zygote),
# so the ptrace tracer is up in time to inject zygote. We do NOT rely on Magisk
# running the module's own post-fs-data.sh — that aborts on this base (set -e + a
# missing companion), which is exactly why ReZygisk never auto-started ("parked").
MODDIR=/data/adb/modules/rezygisk
[ -x "$MODDIR/bin/zygisk-ptrace64" ] || exit 0
# idempotent: don't start a second tracer
for p in /proc/[0-9]*; do
  [ "$(cat "$p/comm" 2>/dev/null)" = "zygisk-ptrace64" ] && exit 0
done
export TMP_PATH=/data/adb/rezygisk
rm -rf "$TMP_PATH"; mkdir -p "$TMP_PATH"; chmod 555 "$TMP_PATH"
chcon u:object_r:system_file:s0 "$TMP_PATH" 2>/dev/null || true
# PlayIntegrityFix per-module setup (best effort; its .so injection is the real work)
[ -f /data/adb/modules/playintegrityfix/post-fs-data.sh ] && \
  sh /data/adb/modules/playintegrityfix/post-fs-data.sh >/dev/null 2>&1
cd "$MODDIR" && ./bin/zygisk-ptrace64 monitor >/dev/null 2>&1 &
exit 0
