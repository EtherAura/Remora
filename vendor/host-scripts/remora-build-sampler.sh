#!/bin/sh
# remora-build-sampler — sample what the source-build container is actually doing to memory.
#
# WHY THIS EXISTS. Three build failures were diagnosed from reconstruction rather
# than observation, and two of those diagnoses were wrong:
#   - NINJA_HIGHMEM_NUM_JOBS was tuned twice (4 jobs, then 3) before anyone checked whether the
#     highmem pool was referenced by any build statement. It is not (`pool = highmem_pool` appears
#     zero times in the generated ninja), so neither value ever bound anything.
#   - siso_metrics.json cannot fill the gap: siso records only steps that COMPLETE, so the
#     OOM-killed step is absent from it by construction. The 766 steps that finished before one
#     kill peaked at 0.02 GiB each — the heavy ones simply are not in the file.
#   - The anon/file split at the moment of a kill was INFERRED (~11 GiB "probably page cache"),
#     never measured, because nothing was sampling when it happened.
# A post-mortem cannot recover any of this. It has to be sampled while the build runs.
#
# Writes one JSON object per sample to $1 (default /tmp/remora-build-samples.jsonl), so a failure
# leaves a timeline instead of a guess. Cheap: reads a handful of procfs/cgroup files per tick.
#
# Usage:  remora-build-sampler.sh [outfile] [interval_seconds]
# Stops on its own when the build container goes away.

OUT="${1:-/tmp/remora-build-samples.jsonl}"
INT="${2:-2}"
SLICE=/sys/fs/cgroup/remorabuild.slice

: > "$OUT"
echo "sampling to $OUT every ${INT}s — waiting for the build container…" >&2

seen_container=0
while :; do
    SCOPE=$(ls -d "$SLICE"/docker-*.scope 2>/dev/null | head -1)
    if [ -z "$SCOPE" ]; then
        # Exit once the container has appeared and then gone; keep waiting before that.
        [ "$seen_container" = 1 ] && { echo "container gone — $(wc -l < "$OUT") samples in $OUT" >&2; exit 0; }
        sleep "$INT"; continue
    fi
    seen_container=1

    # cgroup: the split that actually decides whether a kill happens. anon is unreclaimable, file
    # is page cache the kernel drops under pressure — conflating them is what made 37.8/38 GiB look
    # like an emergency when most of it was reclaimable.
    CUR=$(cat "$SCOPE/memory.current" 2>/dev/null)
    # memory.max reads the literal string "max" on an UNCAPPED container. Emitting that unquoted
    # produced {"cg_max":max}, which is not JSON — 1598 samples of a 49-minute run were silently
    # unparseable and the analysis reported "no samples". Normalise to -1 so every field stays
    # numeric and an uncapped run is still distinguishable.
    MAX=$(cat "$SCOPE/memory.max" 2>/dev/null)
    case "$MAX" in ''|*[!0-9]*) MAX=-1 ;; esac
    ANON=$(awk '/^anon /{print $2}' "$SCOPE/memory.stat" 2>/dev/null)
    FILE=$(awk '/^file /{print $2}' "$SCOPE/memory.stat" 2>/dev/null)
    KILLS=$(awk '/^oom_kill /{print $2}' "$SCOPE/memory.events" 2>/dev/null)
    MAXEV=$(awk '/^max /{print $2}' "$SCOPE/memory.events" 2>/dev/null)
    PSI=$(awk -F'avg10=' '/^some/{split($2,a," ");print a[1]}' "$SCOPE/memory.pressure" 2>/dev/null)

    # per-process: which step classes are resident, and how many are big RIGHT NOW. This is the
    # concurrency figure siso_metrics structurally cannot provide.
    PROCS=$(ps -eo rss,comm --no-headers 2>/dev/null | awk '
        {rss=$1; c=$2}
        c ~ /siso/          {siso+=rss; next}
        c ~ /^java|metalava|r8|d8|javac|turbine/ {java+=rss; if(rss>1048576) jbig++; if(rss>jmax)jmax=rss; next}
        c ~ /clang|rustc/   {cc+=rss; if(rss>cmax)cmax=rss; next}
        END{printf "%d %d %d %d %d %d", siso+0, java+0, jbig+0, jmax+0, cc+0, cmax+0}')
    set -- $PROCS
    SISO=$1; JAVA=$2; JBIG=$3; JMAX=$4; CC=$5; CMAX=$6

    # NVMe COMPOSITE TEMPERATURE, per drive (bd remora-4ei.20 follow-up). The btrfs read-only
    # incidents -26 were both NVMe command timeouts under sustained build
    # write load, and the two drives that failed are exactly the two with non-zero SMART
    # "Warning Comp. Temperature Time" and thermal-throttle transitions; the third has zero of
    # both and has never failed. That is a correlation with no proven mechanism, and every
    # previous diagnosis on this hardware that was inferred rather than sampled turned out wrong
    # — including "discard=async is the trigger", which the 07-26 timeline disproves outright
    # (the filesystem latched read-only with async discard OFF).
    # So sample it. hwmon exposes the composite temp WITHOUT root, so this costs nothing and the
    # next build settles whether the drives approach their 82 C warning threshold under load.
    NVTEMP=""
    for _nd in /sys/class/nvme/nvme*/; do
      _n=${_nd%/}; _n=${_n##*/}
      _h=$(ls -d ${_nd}hwmon*/ 2>/dev/null | head -1)
      [ -n "$_h" ] && [ -r "${_h}temp1_input" ] || continue
      _c=$(( $(cat "${_h}temp1_input" 2>/dev/null || echo 0) / 1000 ))
      NVTEMP="${NVTEMP}${NVTEMP:+,}\"${_n}\":${_c}"
    done

    HOSTAVAIL=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
    HOSTPSI=$(awk -F'avg10=' '/^some/{split($2,a," ");print a[1]}' /proc/pressure/memory)
    VMPSI=$(awk -F'avg10=' '/^some/{split($2,a," ");print a[1]}' /sys/fs/cgroup/machine.slice/cpu.pressure 2>/dev/null)

    printf '{"t":"%s","cg_current":%s,"cg_max":%s,"cg_anon":%s,"cg_file":%s,"oom_kill":%s,"max_events":%s,"cg_psi10":%s,"siso_kb":%s,"java_kb":%s,"java_over1g":%s,"java_max_kb":%s,"cc_kb":%s,"cc_max_kb":%s,"host_avail_kb":%s,"host_psi10":%s,"vm_cpu_psi10":%s,"nvme_temp_c":{%s}}\n' \
        "$(date -Iseconds)" "${CUR:-0}" "${MAX:-0}" "${ANON:-0}" "${FILE:-0}" "${KILLS:-0}" \
        "${MAXEV:-0}" "${PSI:-0}" "${SISO:-0}" "${JAVA:-0}" "${JBIG:-0}" "${JMAX:-0}" \
        "${CC:-0}" "${CMAX:-0}" "${HOSTAVAIL:-0}" "${HOSTPSI:-0}" "${VMPSI:-0}" \
        "${NVTEMP}" >> "$OUT"

    sleep "$INT"
done
