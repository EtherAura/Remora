#!/system/bin/sh
# remora-netfix — make a bare-metal Remora container reachable from the docker host
# for adb and the mirror. Mounted at /remora/netfix.sh and run by the deploy chain after boot.
#
# Android's netd registers eth0 as a network but (a) leaves rp_filter=1, which drops the
# host's inbound packets since the container has no matching reverse route in its ACTIVE
# table, and (b) leaves the legacy_system/legacy_network route tables empty, so fwmark-0
# reply packets (e.g. adbd's SYN-ACK to the docker bridge) are unroutable. Fix both, then
# start the IPv4->IPv6 forwarder (the A16 image's adbd binds IPv6-only; docker's proxy is IPv4).
#
# `fwd` is looked up beside this script rather than at a fixed path, because the two do not
# live at one location any more: bare/remote mount them at /remora (outside /data, where the
# use_overlayfs init-time overlay cannot shadow them — bd remora-4u4.3), while the remote
# deployer still docker-cp's them into /data/local/tmp. Deriving the directory keeps one
# script correct for both.
DIR=$(dirname "$0")

echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter 2>/dev/null
echo 0 > /proc/sys/net/ipv4/conf/eth0/rp_filter 2>/dev/null

# wait for netd to populate eth0's per-network route table
i=0
while [ $i -lt 30 ]; do
  ip route show table eth0 2>/dev/null | grep -q default && break
  sleep 1; i=$((i + 1))
done

# copy eth0's routes into the legacy tables that fwmark-0 (reply) traffic falls through to
ip route show table eth0 2>/dev/null | while read r; do
  ip route replace $r table legacy_system 2>/dev/null
  ip route replace $r table legacy_network 2>/dev/null
done

# start the v4->v6 adb forwarder (host :5555 -docker-> container :5556 -fwd-> ::1:5555 adbd)
#
# Both the presence check and the started check are load-bearing, not defensive padding. setsid
# runs in the BACKGROUND, so its failure never reached $?: when this script and the caller that
# mounts `fwd` disagreed about where it lives, the script still exited 0 and the deploy still
# reported "forwarder up" while adb was dead. A version skew between the two must be loud.
if [ ! -x "$DIR/fwd" ]; then
  echo "remora-netfix: FATAL: no executable fwd at $DIR/fwd — the netfix pair was mounted somewhere this script does not expect (stale copy of one half?)" >&2
  exit 1
fi
# SUPERVISED, WITH EXACTLY ONE OWNER, and its output kept (bd remora-yn4).
#
# Why supervision at all: fwd was observed dying on its own after anywhere from four minutes to an
# hour, and it is the only thing listening on 5556, so everything downstream died with it — adb's
# :5556 device dropped, and the connect step still reported the mirror's pid on ...:5556 (it is
# spawned detached and not waited on) while the mirror had already exited with "Could not find ADB
# device". The mirror vanished with no error anywhere, and the encoder stayed healthy throughout,
# so the obvious suspect was the wrong one. The cause is STILL unknown, and it was unknowable
# while both streams went to /dev/null; they are kept now so the next death names its signal.
#
# Why only one owner: when init also has the service, two starts is not redundancy but a RESTART
# LOOP — they bind the same 5556, the loser exits (fwd.c returns 1 on bind failure) and whatever
# supervises it starts it again, forever. Measured on a live instance carrying both: 229 init
# restarts of remora_fwd in one session.
#
# So init wins when its .rc is mounted — it starts the service at boot, restarts it on exit, and
# survives a container restart, which the shell loop cannot. The loop below is the FALLBACK for a
# container created before that bind existed, where nothing else would start fwd at all. There,
# restarting blind is safe because fwd holds no state and each connection is an independent forked
# relay; the 2s pause keeps a genuinely unstartable binary from spinning.
FWD_LOG=/data/local/tmp/remora-fwd.log
mkdir -p /data/local/tmp 2>/dev/null
# Retire any supervisor from a previous run whichever branch we take — leaving one alive under
# init's ownership is exactly the two-owner collision above.
# The bracket is NOT decoration. toybox's pkill -f matches its OWN argv, which necessarily contains
# the pattern, so `pkill -f remora-fwd-supervisor` kills the pkill and leaves every supervisor
# running — measured: two netfix runs left two supervisors fighting over one port. Writing the last
# character as a one-character class means the pattern still matches the supervisor while the
# literal text in pkill's own command line ("...superviso[r]") does not match the regex.
pkill -f "remora-fwd-superviso[r]" 2>/dev/null

if [ -f /vendor/etc/init/remora-fwd.rc ]; then
  # Idempotent: ctl.start on an already-running service is a no-op, so a re-run does not disturb a
  # healthy forwarder — which is the whole reason the connect path may call netfix freely.
  setprop ctl.start remora_fwd 2>/dev/null
  sleep 1
  if ! pidof fwd >/dev/null 2>&1; then
    echo "remora-netfix: FATAL: init service remora_fwd did not start — adb will not connect" >&2
    exit 1
  fi
else
  # No .rc: this container predates the init-service bind, so nothing but us will start fwd.
  # The `^$DIR/fwd` pkill needs no bracket guard: it is anchored, and pkill's argv starts "pkill".
  pkill -f "^$DIR/fwd" 2>/dev/null
  setsid sh -c "
    # A distinctive argv so the pkill above can find this loop and nothing else.
    : remora-fwd-supervisor
    while :; do
      \"$DIR/fwd\" 5556 5555 >>\"$FWD_LOG\" 2>&1
      echo \"[\$(date 2>/dev/null)] fwd exited status=\$? — restarting\" >>\"$FWD_LOG\"
      sleep 2
    done
  " </dev/null >/dev/null 2>&1 &
  sleep 1
  if ! pgrep -f "^$DIR/fwd" >/dev/null 2>&1; then
    echo "remora-netfix: FATAL: fwd did not stay up — adb will not connect" >&2
    exit 1
  fi
fi

# Non-privileged app uids can't reach the network at all when Android's "Restricted Networking
# Mode" is left ON: netd's fw_restricted chain drops every packet from uids lacking
# CONNECTIVITY_USE_RESTRICTED_NETWORKS, so only system/shell/root get out. Host adb still works,
# but WebView/Chromium shows net::ERR_INTERNET_DISCONNECTED and Play Store / GMS sign-in can't
# connect. Force it to the platform default (OFF). See beads a17-webview-network-offline.
settings put global restricted_networking_mode 0 2>/dev/null
echo "remora-netfix: rp_filter=$(cat /proc/sys/net/ipv4/conf/eth0/rp_filter) legacy routes set, forwarder up, restricted-net off"
