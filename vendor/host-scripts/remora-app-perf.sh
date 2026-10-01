#!/bin/bash
# remora-app-perf.sh — app-level performance harness for the Remora container (bd remora-9tc).
#
# WHY THIS EXISTS: Geekbench cannot measure any of the image-level work (ART codegen, HWUI
# backend, Mesa, dexopt, heap sizing). It ships its own precompiled native binaries, so nothing in
# the Android image is in its hot path — that is exactly why it was a good instrument for finding
# the VM misconfiguration and a useless one for everything downstream of it. This harness supplies
# the metrics that *are* sensitive to those changes:
#
#   launch   `am start -W -S` cold-start TotalTime, N runs, median + spread. Sensitive to dexopt
#            mode, ART codegen and I/O. This is the number a user feels as "the app opened".
#   frames   `dumpsys gfxinfo` percentiles + jank rate over a fixed scroll. Sensitive to the HWUI
#            backend, Skia and Mesa. This is the number a user feels as "it scrolls smoothly".
#
# Every run prints the CONFIG BLOCK it measured under (vCPUs, ple_gap, ART ISA variant, HWUI
# backend, dexopt mode, heap caps). A result without its config is worthless three weeks later —
# most of this session was spent recovering exactly that kind of missing context.
#
# Usage:
#   remora-app-perf.sh [--tag NAME] [--launches N] [--scrolls N] [pkg ...]
#     --tag NAME      label for this run, echoed into the output (e.g. "baseline", "art-alderlake")
#     --launches N    cold launches per package (default 7; the first is discarded as warm-up)
#     --scrolls N     swipes per frame-stats sample (default 12)
#     pkg ...         packages to measure (default: com.android.settings)
#
#   Append results to a file to build a comparable history:
#     remora-app-perf.sh --tag baseline | tee -a docs/app-perf-results.txt
#
# DETACH THE MIRROR FIRST. A live mirror encodes every frame the compositor produces, which
# both steals CPU and changes the frame pipeline being measured — it is the single largest source
# of noise here. The script refuses to run if it sees one, because a silently-contaminated number
# is worse than no number.
set -u

TAG="untagged"; LAUNCHES=7; SCROLLS=12; PKGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --tag)      TAG="$2"; shift 2;;
    --launches) LAUNCHES="$2"; shift 2;;
    --scrolls)  SCROLLS="$2"; shift 2;;
    -h|--help)  sed -n '2,40p' "$0"; exit 0;;
    *)          PKGS+=("$1"); shift;;
  esac
done
[ ${#PKGS[@]} -eq 0 ] && PKGS=(com.android.settings)

adb get-state >/dev/null 2>&1 || { echo "no adb device — try 'adb connect <ip>:5555'" >&2; exit 1; }

if pgrep -f '[r]emora mirror' >/dev/null 2>&1; then
  echo "REFUSING TO RUN: a mirror is attached." >&2
  echo "  Detach it first (kill the pid, or stop before benchmarking); reattach with" >&2
  echo "  './build/remora reconnect' afterwards." >&2
  exit 1
fi

# median of stdin (one integer per line); also emits min/max so a wide spread is visible
stats() { sort -n | awk '{v[NR]=$1} END {
  if (NR==0) {print "n/a"; exit}
  m = (NR%2) ? v[(NR+1)/2] : int((v[NR/2]+v[NR/2+1])/2)
  printf "%d  (min %d, max %d, n=%d)", m, v[1], v[NR], NR }'; }

gp() { adb shell getprop "$1" 2>/dev/null | tr -d '\r'; }

echo "================================================================"
echo "remora-app-perf  tag=$TAG"
echo "================================================================"
echo "config under measurement:"
printf "  guest vCPUs        %s\n" "$(adb shell nproc 2>/dev/null | tr -d '\r')"
printf "  host ple_gap       %s\n" "$(cat /sys/module/kvm_intel/parameters/ple_gap 2>/dev/null)"
printf "  ART isa variant    %s (features: %s)\n" \
  "$(gp dalvik.vm.isa.x86_64.variant)" "$(gp dalvik.vm.isa.x86_64.features)"
printf "  HWUI vulkan        %s\n" "$(v=$(gp ro.hwui.use_vulkan); echo "${v:-<unset — GL backend>}")"
printf "  dexopt (bg)        %s\n" "$(gp pm.dexopt.bg-dexopt)"
printf "  dalvik heap        growthlimit=%s size=%s\n" "$(gp dalvik.vm.heapgrowthlimit)" "$(gp dalvik.vm.heapsize)"
printf "  GLES renderer      %s\n" \
  "$(adb shell dumpsys SurfaceFlinger 2>/dev/null | grep -m1 'GLES:' | cut -d, -f2- | sed 's/^ *//' | tr -d '\r')"
HZ=$(adb shell dumpsys display 2>/dev/null | grep -m1 -oE 'fps=[0-9.]+' | cut -d= -f2)
BUDGET=$(awk -v h="${HZ:-60}" 'BEGIN{printf "%.1f", 1000/h}')
printf "  display            %s Hz  (frame budget %s ms)\n" "${HZ:-?}" "$BUDGET"
echo
echo "  NB read the percentiles against THAT budget. A p99 far below it means the workload has"
echo "  slack and can only reveal REGRESSIONS, not improvements — and that spare budget is also"
echo "  the signal that a higher refresh rate would be absorbed without jank."
echo

for pkg in "${PKGS[@]}"; do
  adb shell pm list packages 2>/dev/null | grep -q "^package:$pkg\$" \
    || { echo "-- $pkg: NOT INSTALLED, skipped"; echo; continue; }

  echo "-- $pkg"

  # ---- cold launch latency -------------------------------------------------
  # -S force-stops before starting, so every run is a genuine cold start. The first is discarded:
  # it warms the page cache and any lazily-loaded provider, and reads systematically high.
  tmp=$(mktemp)
  for i in $(seq 1 "$LAUNCHES"); do
    t=$(adb shell "am start -W -S $pkg 2>/dev/null" | grep -m1 '^TotalTime:' | awk '{print $2}' | tr -d '\r')
    [ -n "$t" ] && [ "$i" -gt 1 ] && echo "$t" >> "$tmp"
    sleep 2
  done
  printf "   cold launch  %s ms\n" "$(stats < "$tmp")"
  rm -f "$tmp"

  # ---- frame timing over a fixed scroll ------------------------------------
  # Fixed swipe geometry and duration so the workload is identical across runs. 300ms is a
  # deliberate fling: fast enough to keep the pipeline saturated, slow enough to stay on-list.
  # Land on a scroll-heavy screen before sampling. The Settings *homepage* is far too light —
  # it renders a flat 5 ms at every percentile, so it cannot discriminate anything. The all-apps
  # list (long, icon-per-row, real inflation cost) spreads p50/p99 to 5/10 ms and can.
  case "$pkg" in
    com.android.settings)
      adb shell 'am start -a android.settings.MANAGE_ALL_APPLICATIONS_SETTINGS' >/dev/null 2>&1
      sleep 4;;
  esac
  adb shell "dumpsys gfxinfo $pkg reset" >/dev/null 2>&1
  for i in $(seq 1 "$SCROLLS"); do
    adb shell input swipe 1880 1400 1880 500 300 >/dev/null 2>&1
  done
  sleep 1
  adb shell "dumpsys gfxinfo $pkg" 2>/dev/null \
    | grep -E 'Total frames rendered|Janky frames:|percentile|Number Missed Vsync' \
    | sed 's/^/   /' | tr -d '\r'
  echo
done

echo "measured $(date -u '+%Y-%m-%d %H:%M UTC')"
