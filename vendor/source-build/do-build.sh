#!/bin/bash
# Full image build by default. `--module <name>` (or MODULE=<name>) builds a single module through
# the SAME lunch and the same staged tree, which is the only safe way to do it — see the refusal
# below for why a bare `m <Module>` is not.
MODULE="${MODULE:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --module)   MODULE="$2"; shift 2 ;;
        --module=*) MODULE="${1#*=}"; shift ;;
        *) echo "do-build.sh: unknown argument '$1' (usage: do-build.sh [--module <name>])" >&2
           exit 2 ;;
    esac
done
cd /src || exit 1
source build/envsetup.sh >/dev/null 2>&1
# BUILD_LUNCH: full lunch config override; unset = derived from ANDROID_VER (16→bp2a, 17→cp2a).
# COMMON_LUNCH_CHOICES in the device tree's AndroidProducts.mk is a MENU, not a whitelist — any
# release config the tree carries lunches, so cp2a stands (bd remora-82c.12).
# Both releases lunch remora_x86_64 from device/remora (bd remora-28ix.4). lunchConfigFor() in
# ImageBuild.cpp is the authoritative mapping; this fallback mirrors it for manual runs only (the
# engine always passes BUILD_LUNCH), and the two must not disagree.
case "${ANDROID_VER:-16}" in
    17) rel=cp2a ;;   # genuine Android 17: SDK 37, codename REL, release 17
    *)  rel=bp2a ;;
esac
prod=remora_x86_64
cfg="${BUILD_LUNCH:-${prod}-${rel}-userdebug}"
# NOT silenced, but ALSO NOT in a subshell. `lunch` works by EXPORTING TARGET_PRODUCT /
# TARGET_RELEASE / TARGET_BUILD_VARIANT into the calling shell, so `out=$(lunch ...)` throws all of
# it away — the command substitution runs in a child and the parent sees nothing. Redirect to a
# FILE instead: that keeps lunch in this shell while still capturing what it said.
# The old `>/dev/null 2>&1` had the environment right and the diagnosis wrong; this needs both,
# because lunch can print a fatal-looking error and STILL return 0.
lunch_log=$(mktemp)
lunch "$cfg" >"$lunch_log" 2>&1
lunch_rc=$?
if [ "$lunch_rc" -ne 0 ] || [ -z "${TARGET_RELEASE:-}" ] || [ -z "${TARGET_PRODUCT:-}" ]; then
    echo "LUNCH_FAILED ($cfg): rc=$lunch_rc TARGET_PRODUCT='${TARGET_PRODUCT:-}' TARGET_RELEASE='${TARGET_RELEASE:-}'"
    tail -20 "$lunch_log"
    echo "valid choices for this tree:"
    sed -n '/COMMON_LUNCH_CHOICES/,/^$/p' device/remora/AndroidProducts.mk 2>/dev/null | head -8
    rm -f "$lunch_log"
    exit 1
fi
rm -f "$lunch_log"
echo "=== lunch ok: TARGET_PRODUCT=$TARGET_PRODUCT TARGET_RELEASE=$TARGET_RELEASE ==="
# Reproducibility (bd a17-abi-dump-check): a fresh repo sync restores stale Android-16 (platform/36)
# ABI reference dumps that the A17 check-abi-dump-list rejects near 98% as "Found unexpected ABI
# reference dump files under prebuilts/abi-dumps/platform/36". They are orphaned (the module's ABI
# moved) and unrelated to the container build. Prune them here so a clean checkout builds A17 without
# the manual delete. platform/36 dumps are CORRECT when building A16, so gate on the version.
if [ "${ANDROID_VER:-16}" = "17" ]; then
    find prebuilts/abi-dumps/platform/36 -name 'libcom.android.tethering.dns_helper.so.lsdump' -delete 2>/dev/null
fi
# THE TRAP THIS GUARD EXISTS FOR (bd remora-25u). A single-module build recomputes the install set.
# If the tree has not been staged, that set is missing every feature package, and `m` DELETES the
# files it now believes are no longer installed — "Removed file that is no longer installed: ...".
# On a bare `m SystemUI` pruned 44 files out of system/lib/arm, and assemble-image.sh
# does NOT re-supply them (the ndk_translation feature carries only lib64/arm64), so the next image
# would have shipped without the 32-bit ARM native-bridge libs, silently.
#
# Staging is detectable from the tree itself rather than from the WITH_* env: stage-features.sh
# appends "# --- Remora ..." markers to the product makefile, and the patch reset pass wipes them,
# so their presence is exactly the "has been staged since the last reset" signal.
if [ -n "$MODULE" ]; then
    # The product mk stage-features.sh wires — device/remora on both releases (bd remora-28ix.4).
    MK=device/remora/remora_x86_64.mk
    [ -f "$MK" ] || { echo "REFUSING to build '$MODULE': no $MK under $(pwd)"; exit 3; }
    # grep -q, not `n=$(grep -c ...) || echo 0`: grep -c PRINTS 0 and EXITS 1 on no match, so the
    # `|| echo 0` fallback appends a second 0 and the test silently never fires. Caught in testing.
    if ! grep -q '^# --- Remora' "$MK" 2>/dev/null; then
        echo "REFUSING to build '$MODULE': $MK carries no Remora staging markers, so this tree is"
        echo "un-staged (a patch reset wipes them). Building one module now would prune the install"
        echo "set and silently degrade the next assembled image — bd remora-25u."
        echo "Run stage-features.sh against this tree first; a full build does it for you."
        exit 3
    fi
    echo "=== MODULE BUILD: $MODULE ($(grep -c '^# --- Remora' "$MK") staging markers) ==="
fi
# Bound the KATI highmem pool by the CONTAINER's cap rather than the host's RAM.
#
# WHAT THIS DOES NOT DO, stated first because the original version of this comment got it wrong:
# it does NOT bound metalava, and it did NOT fix the metalava OOM it was written for. Counted in
# the generated ninja: highmem_pool appears 50x in out/build-<product>.ninja (kati) and ZERO
# times in out/soong/build.<product>.ninja (soong). metalava is a soong action, so
# NINJA_HIGHMEM_NUM_JOBS never reaches it. The 34 GiB cgroup kill that took metalava out is
# unaddressed here — see bd remora-4ei.30.
#
# What it IS good for: those 50 kati actions are sized by Soong's HighmemParallel(), which reads
# detectTotalRAM() -> /proc/meminfo, and inside a container /proc/meminfo reports the HOST's memory
# rather than the cgroup limit. So on a 62 GiB host with a 34 GiB container the pool was sized for
# 62. 8 GiB per process is Soong's own minMemPerHighmemProcess constant, so this applies Soong's
# arithmetic to the real number instead of overriding its judgement.
#
# Inert by default since the hard cap became opt-in: with no --memory the cgroup reads "max", the
# case below takes the non-numeric branch, and Soong's own sizing stands. It only bites when a cap
# is set explicitly.
CGROUP_MEM_MAX="${CGROUP_MEM_MAX:-/sys/fs/cgroup/memory.max}"
if [ -z "${NINJA_HIGHMEM_NUM_JOBS:-}" ] && [ -r "$CGROUP_MEM_MAX" ]; then
    lim=$(cat "$CGROUP_MEM_MAX" 2>/dev/null)
    case "$lim" in
        ''|max|*[!0-9]*) : ;;   # unbounded or unparseable — Soong's own sizing is right
        *)
            # SUBTRACT SISO BEFORE DIVIDING. `cap / 8` authorises more concurrency than the
            # container can honour: siso — the build EXECUTOR — holds its own Go heap for the whole
            # compile phase, so the memory available to jobs is (cap - siso), not cap.
            # MEASURED at cap=46 GiB, sampling RSS every 10s through a real run:
            #     siso peak                  20.6 GiB
            #     each metalava/javac step   ~4.0 GiB  (the one OOM-killed was exactly 4.0)
            #     jobs authorised by cap/8   5
            # 20.6 + 5*4.0 = 40.6 GiB before any other step class, and it died at 545/1497 steps.
            # GOMEMLIMIT is what siso is ALLOWED to take, so reserving that is correct by
            # construction rather than fitted to one sample; fall back to the measured 21 when unset.
            gomem="${GOMEMLIMIT%GiB}"
            case "$gomem" in ''|*[!0-9]*) gomem=21 ;; esac
            avail=$(( lim / 1073741824 - gomem ))
            hj=$(( avail / 8 ))
            [ "$hj" -lt 1 ] && hj=1
            export NINJA_HIGHMEM_NUM_JOBS="$hj"
            echo "=== container cap $(( lim / 1073741824 )) GiB - ${gomem} GiB siso = ${avail} GiB for jobs -> NINJA_HIGHMEM_NUM_JOBS=$hj ==="
            ;;
    esac
fi
# BUILD_NUMBER: the fingerprint's incremental slot (bd remora-iwlk). Left unset, soong_ui falls
# back to the CONSTANT "eng.root", so every Remora image ever built carries the IDENTICAL
# ro.build.fingerprint and PackageManager never sees an "upgrade" — per-package state recorded in
# /data (primaryCpuAbi among it) survives across images forever. That is how a multilib-era
# primaryCpuAbi=x86 outlived the 32-bit drop on amd-host-a and stopped the device booting: the
# webview zygote was asked for an ABI the image no longer ships. A fresh UTC stamp per full build
# makes PMS re-derive system-package state on the first boot of every new image, exactly as it does
# across a real OTA; the price is a dexopt pass on that boot. soong_ui writes the value to
# out/soong/build_number.txt and un-exports it for kati, so a changing number does NOT retrigger
# big rebuilds — only the targets that read the file (build.prop, image metadata) re-run.
# A --module build REUSES the tree's current number: minting one there would churn those targets
# for a build that never re-assembles them, and running without the variable would silently regress
# the tree to eng.root between full builds.
if [ -z "${BUILD_NUMBER:-}" ]; then
    if [ -n "$MODULE" ] && [ -s out/soong/build_number.txt ]; then
        BUILD_NUMBER=$(cat out/soong/build_number.txt)
    else
        BUILD_NUMBER=$(date -u +%Y%m%d.%H%M%S)
    fi
fi
export BUILD_NUMBER
echo "=== BUILD START $(date '+%F %T') ($cfg) BUILD_NUMBER=$BUILD_NUMBER ==="
# BUILD_NICE: CPU priority (0 = normal .. 19 = maximum politeness), default 15.
# BUILD_JOBS: parallel jobs for m/ninja; unset = all cores.
nice -n "${BUILD_NICE:-15}" m $MODULE ${BUILD_JOBS:+-j"${BUILD_JOBS}"}
rc=$?
echo "=== BUILD EXIT $rc at $(date '+%F %T') ==="
exit $rc
