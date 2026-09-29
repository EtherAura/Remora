#!/usr/bin/env bash
# watch-build.sh — live progress + resource view for a throttled Remora source build.
#
# The build runs inside docker under --memory=<build_mem_gib>g in its own remorabuild.slice, so a
# bust kills the BUILD and not the desktop session. That containment is only reassuring if you can
# see it working, which is what this is for: the cgroup line below is the one that matters.
#
#   tools/watch-build.sh [logfile]     default: /tmp/remora-build.log
#
# Ctrl-C stops watching. It does NOT stop the build — the build is a detached docker container.

set -u
LOG="${1:-/tmp/remora-build.log}"
INTERVAL="${WATCH_INTERVAL:-5}"

hr() { printf '%s\n' "────────────────────────────────────────────────────────────────────"; }

gib() { awk -v b="${1:-0}" 'BEGIN{printf "%.1f", b/1073741824}'; }

# The build container is the one docker run started under our slice. Match on the image the
# builder uses rather than a name, because the name is generated per run.
build_container() {
  docker ps --format '{{.ID}} {{.Image}} {{.Names}}' 2>/dev/null \
    | awk '$2 ~ /^remora-builder(:|$)/ {print $1; exit}'
}

while :; do
  clear 2>/dev/null || true
  printf 'remora build monitor   %s   (refresh %ss, Ctrl-C to stop watching)\n' \
         "$(date '+%H:%M:%S')" "$INTERVAL"
  hr

  # ── build progress ───────────────────────────────────────────────────────────
  if [ -f "$LOG" ]; then
    # soong/ninja emit "[  123/45678] action"; take the newest and turn it into a percentage.
    prog=$(grep -oE '\[ *[0-9]+/ *[0-9]+\]' "$LOG" 2>/dev/null | tail -1)
    if [ -n "$prog" ]; then
      done_n=$(printf '%s' "$prog" | tr -dc '0-9/' | cut -d/ -f1)
      tot_n=$(printf '%s' "$prog" | tr -dc '0-9/' | cut -d/ -f2)
      if [ -n "$tot_n" ] && [ "$tot_n" -gt 0 ] 2>/dev/null; then
        pct=$(awk -v d="$done_n" -v t="$tot_n" 'BEGIN{printf "%.1f", (d*100)/t}')
        bar=$(awk -v p="$pct" 'BEGIN{n=int(p/2.5); for(i=0;i<40;i++) printf (i<n?"#":".")}')
        printf 'ninja    %s  %s%%  (%s/%s)\n' "$bar" "$pct" "$done_n" "$tot_n"
      fi
    else
      # Before ninja starts there is no ratio — say which phase we are in instead of nothing.
      phase=$(grep -oE 'soong_build|Starting Soong|analyzing|Install: |repo sync|LUNCH|Kati' "$LOG" 2>/dev/null | tail -1)
      printf 'phase    %s\n' "${phase:-starting…}"
    fi
    printf 'log      %s   (%s lines, %s)\n' "$LOG" "$(wc -l <"$LOG" 2>/dev/null)" \
           "$(du -h "$LOG" 2>/dev/null | cut -f1)"
  else
    printf 'log      %s  (not created yet)\n' "$LOG"
  fi

  # ── the containment that lets this run alongside everything ──────────────────
  hr
  cid=$(build_container)
  if [ -n "$cid" ]; then
    read -r used lim < <(docker stats --no-stream --format '{{.MemUsage}}' "$cid" 2>/dev/null \
                          | tr -d ' ' | tr '/' ' ')
    cpu=$(docker stats --no-stream --format '{{.CPUPerc}}' "$cid" 2>/dev/null)
    printf 'build    container %s   mem %s / %s   cpu %s\n' "${cid:0:12}" "${used:-?}" "${lim:-?}" "${cpu:-?}"
  else
    printf 'build    (no build container running)\n'
  fi

  # ── host: the numbers that decide whether anything gets hurt ─────────────────
  avail=$(awk '/^MemAvailable/{printf "%.1f", $2/1048576}' /proc/meminfo)
  swtot=$(awk '/^SwapTotal/{printf "%.1f", $2/1048576}' /proc/meminfo)
  swfree=$(awk '/^SwapFree/{printf "%.1f", $2/1048576}' /proc/meminfo)
  swused=$(awk -v t="$swtot" -v f="$swfree" 'BEGIN{printf "%.1f", t-f}')
  printf 'host     avail %s GB   swap used %s / %s GB   load%s\n' \
         "$avail" "$swused" "$swtot" "$(cut -d' ' -f1-3 /proc/loadavg | sed 's/^/ /')"

  # Memory pressure is the honest "is this hurting?" signal — better than free(1), because it
  # measures time actually LOST to reclaim rather than how full RAM happens to look.
  if [ -r /proc/pressure/memory ]; then
    printf 'pressure %s\n' "$(awk '/^some/{print "mem some " $2 " " $3}' /proc/pressure/memory)"
  fi

  # ── the thing you care about staying alive ───────────────────────────────────
  hr
  dev=$(adb devices 2>/dev/null | awk '/device$/{printf "%s ", $1}')
  printf 'device   %s\n' "${dev:-(none connected)}"

  # ── recent output ────────────────────────────────────────────────────────────
  hr
  if [ -f "$LOG" ]; then
    # Anchored//specific on purpose. A loose /error/ matches "ignore project-quota error" and
    # "adds whitespace errors" — benign lines that scroll the real ones off screen. The patterns
    # that matter are the ones that let a build finish GREEN with something missing:
    # "WARN patch SKIPPED (conflicts)" exits 0 and the && chain continues (bd remora-82c.1).
    n=$(grep -acE 'SKIPPED \(conflicts\)' "$LOG" 2>/dev/null)
    [ "${n:-0}" -gt 0 ] && printf ' !! %s patch(es) SKIPPED on conflict — build will still go green\n' "$n"
    grep -aE 'SKIPPED \(conflicts\)|^FAILED|^fatal:|^error:|Killed|Out of memory|No space left' \
      "$LOG" 2>/dev/null | tail -3 | sed 's/^/ !! /' | cut -c1-100
    tail -6 "$LOG" 2>/dev/null | cut -c1-100
  fi

  sleep "$INTERVAL"
done
