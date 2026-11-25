#!/system/bin/sh
# Remora boot bootstrap for patched ReZygisk + PlayIntegrityFix.
#
# Runs at post-fs-data (magiskd executes /data/adb/post-fs-data.d/* before zygote),
# so the ptrace tracer is up in time to inject zygote. We do NOT rely on Magisk
# running the module's own post-fs-data.sh — that aborts on this base (set -e + a
# missing companion), which is exactly why ReZygisk never auto-started ("parked").
# Remora's deploy runs this same file through `docker exec`, ahead of zygote and in its
# framework-restart fallback, so whichever start comes first, the other finds the tracer
# here and leaves it, its control socket in $TMP_PATH included (bd remora-4mba).
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
# Every monitor start prepends its status to module.prop's description, and /data outlives the
# container, so start each one from the clean copy baked into the image. The copy is staged whole
# in a file of its own beside module.prop and only then renamed over it (-T: never into a
# directory), so a failed copy leaves the last good file as it was and a link is replaced, not
# written through. Anything at that path that is neither a file nor a link is left alone.
# $1 is the clean copy, $2 the module's directory.
reset_module_prop() {
  CLEAN="$1"
  PROP="$2/module.prop"
  if [ -f "$CLEAN" ] && { [ -L "$PROP" ] || [ -f "$PROP" ] || [ ! -e "$PROP" ]; }; then
    STAGE=$(mktemp "$2/.module.prop.XXXXXX") && {
      { cat "$CLEAN" > "$STAGE" && cmp -s "$CLEAN" "$STAGE" && chmod 0644 "$STAGE" &&
        mv -f -T "$STAGE" "$PROP"; } || rm -f "$STAGE"
    }
  fi
}
reset_module_prop /system/etc/remora-spoof/rez-mod/module.prop "$MODDIR"
cd "$MODDIR" && setsid ./bin/zygisk-ptrace64 monitor >/dev/null 2>&1 </dev/null &
exit 0
