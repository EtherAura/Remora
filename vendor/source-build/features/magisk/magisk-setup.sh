#!/system/bin/sh
# Create Magisk's tmpfs environment, so UPSTREAM Magisk can run in a container (bd remora-28ix.6).
#
# WHY THIS EXISTS. Upstream Magisk is installed by patching a boot image: magiskinit runs as PID 1
# and builds this environment before init proper starts. A container has no boot image to patch and
# no PID-1 slot to take, so Remora drives Magisk from init.rc instead — and the one thing upstream
# offers no CLI for is BUILDING the environment. Magisk Delta added `--setup-sbin` for exactly this
# case, which is the sole reason Remora shipped Delta; that fork's source is now gone (HuskyDG
# 404s), leaving us distributing GPLv3 binaries we cannot offer source for. This script replaces
# that one Delta-only command, so magisk_root can move to upstream, whose source is live.
#
# WHAT UPSTREAM ACTUALLY REQUIRES, read out of the v30.7 binary rather than guessed:
#   * the tmpfs is /debug_ramdisk — HARDCODED upstream, and already what Remora uses (bd
#     remora-4ei.94 moved it off /sbin because a bare /sbin directory is itself a root tell)
#   * it validates its own location with "parent should be tmpfs", so a plain directory is refused
#   * it expects magisk64 (and magisk32 on multilib), .magisk/busybox, and the .magisk skeleton:
#     config, modules, preinit, rootdir, device/{log,preinit,socket}
# The daemon then starts with `magisk --daemon`, and the init hooks upstream DOES provide
# (post-fs-data, service, boot-complete, zygote-restart) drive the rest — see magisk.rc.
#
# SELINUX. Delta's `--auto-selinux` is not needed here and has no upstream equivalent: this
# container runs with SELinux disabled (no /sys/fs/selinux), which is why magisk.rc runs
# magiskpolicy --live separately. If Remora ever runs with SELinux enforcing, this script is where
# the labelling would have to be added.
#
# THE `u:r:su:s0` EXEC CONTEXTS in magisk.rc are inert with SELinux off — that part was always
# true. THE --live CALL ITSELF IS NOT, and this comment used to imply otherwise: it aborts on the
# missing policy and writes a tombstone every boot. magisk.rc now guards it on
# /sys/fs/selinux/policy existing; see the reasoning there (bd remora-e5x.28.1).
set -u

MAGISKTMP=/debug_ramdisk
SRC=/system/etc/init/magisk

log() { echo "magisk-setup: $*"; }

[ -d "$SRC" ] || { log "FATAL: no staged Magisk at $SRC"; exit 1; }

# The tmpfs itself. Already-mounted is the normal case on a re-exec, not an error.
if ! mountpoint -q "$MAGISKTMP" 2>/dev/null; then
    mkdir -p "$MAGISKTMP" 2>/dev/null
    mount -t tmpfs -o mode=0755 tmpfs "$MAGISKTMP" || { log "FATAL: cannot mount tmpfs at $MAGISKTMP"; exit 1; }
fi

# The binaries. magisk64 is what the daemon execs; `magisk` is the name every consumer types, so
# both exist and the second is a copy rather than a symlink — a symlink to a path inside the same
# tmpfs is fine, but a copy keeps `magisk --path` honest if the layout is ever rearranged.
for f in magisk magiskpolicy busybox; do
    [ -f "$SRC/$f" ] || { log "FATAL: $SRC/$f missing"; exit 1; }
done
cp -f "$SRC/magisk" "$MAGISKTMP/magisk64" || exit 1
cp -f "$SRC/magisk" "$MAGISKTMP/magisk" || exit 1
cp -f "$SRC/magiskpolicy" "$MAGISKTMP/magiskpolicy" || exit 1
chmod 755 "$MAGISKTMP/magisk64" "$MAGISKTMP/magisk" "$MAGISKTMP/magiskpolicy"

# The applet symlinks. su is the one everything reaches for; resetprop is what the Play-spoof stack
# calls, and magiskpolicy is invoked by name from magisk.rc.
for applet in su resetprop; do
    ln -sf ./magisk "$MAGISKTMP/$applet" 2>/dev/null
done

# The .magisk skeleton the daemon expects to find rather than create.
mkdir -p "$MAGISKTMP/.magisk/modules" "$MAGISKTMP/.magisk/device" \
         "$MAGISKTMP/.magisk/preinit" "$MAGISKTMP/.magisk/rootdir" 2>/dev/null
cp -f "$SRC/busybox" "$MAGISKTMP/.magisk/busybox" 2>/dev/null && chmod 755 "$MAGISKTMP/.magisk/busybox"
# config is read as key=value; an absent one makes the daemon assume defaults that do not match a
# container (it would look for a boot-image install).
[ -f "$MAGISKTMP/.magisk/config" ] || cat > "$MAGISKTMP/.magisk/config" <<EOF
KEEPVERITY=true
KEEPFORCEENCRYPT=true
RECOVERYMODE=false
EOF

# The daemon's own state lives on /data, which init has mounted by post-fs-data.
mkdir -p /data/adb/modules /data/adb/post-fs-data.d /data/adb/service.d 2>/dev/null
chmod 700 /data/adb 2>/dev/null

log "environment ready at $MAGISKTMP ($(ls "$MAGISKTMP" | tr '\n' ' '))"
