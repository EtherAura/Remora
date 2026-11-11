#!/usr/bin/env bash
# Line coverage of src/ by the offline suites, per layer, with a floor for each tested one.
#
#   tools/coverage.sh <build-dir>
#
# <build-dir> is a tree configured with --coverage in the C++ flags and already built:
#
#   cmake -B build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage
#   cmake --build build/coverage
#   QT_QPA_PLATFORM=offscreen tools/coverage.sh build/coverage
#
# It runs the suites itself, between a baseline capture and the real one, so code that no suite
# executes is counted at zero instead of being left out (see .github/workflows/coverage.yml, which
# runs exactly this). Needs lcov.
#
# The floors come from the environment, in percent of lines: FLOOR_CORE, FLOOR_ENGINE, FLOOR_STORE.
# A layer without a floor is reported and not judged. The report goes to stdout and, on a GitHub
# runner, into the run's summary. Exit status: 0, or 1 when a layer is under its floor.
set -euo pipefail

BUILD="${1:?usage: coverage.sh <build-dir>}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$BUILD/coverage"
mkdir -p "$OUT"

# What lcov would otherwise stop for and is not our code's doing: the compiler-probe sources CMake
# compiled and deleted, generated moc files whose line tables disagree with themselves, and
# patterns below that match nothing on a given toolchain.
QUIET=(--quiet --ignore-errors "mismatch,unused,inconsistent,gcov,negative,empty,source")

lcov "${QUIET[@]}" --directory "$BUILD" --zerocounters
lcov "${QUIET[@]}" --directory "$BUILD" --capture --initial --no-external --base-directory "$ROOT" \
    --output-file "$OUT/baseline.info"
ctest --test-dir "$BUILD" --output-on-failure
lcov "${QUIET[@]}" --directory "$BUILD" --capture --no-external --base-directory "$ROOT" \
    --output-file "$OUT/suites.info"
lcov "${QUIET[@]}" --add-tracefile "$OUT/baseline.info" --add-tracefile "$OUT/suites.info" \
    --output-file "$OUT/all.info"

# "<percent> <hit> <total>" of the lines under one directory of the source tree
lines_of() {
    lcov "${QUIET[@]}" --extract "$OUT/all.info" "$ROOT/$1/*" --output-file "$OUT/part.info" >&2
    lcov --ignore-errors "inconsistent,empty" --summary "$OUT/part.info" 2>&1 \
        | sed -n 's/^ *lines\.*: *\([0-9.]*\)% (\([0-9]*\) of \([0-9]*\) lines).*/\1 \2 \3/p'
}

failed=0
report="| layer | lines | covered | floor |"$'\n'"|---|---|---|---|"
row() {   # <label> <directory> [<floor>]
    local label=$1 dir=$2 floor=${3:-} pct hit total verdict
    read -r pct hit total < <(lines_of "$dir") || true
    if [ -z "${pct:-}" ]; then
        echo "coverage: no line data for $dir" >&2
        failed=1
        return
    fi
    verdict="not judged"
    if [ -n "$floor" ]; then
        verdict="$floor %"
        if awk -v p="$pct" -v f="$floor" 'BEGIN { exit !(p + 0 < f + 0) }'; then
            verdict="$floor % (UNDER)"
            echo "::error::$label: $pct % of lines covered, the floor is $floor %"
            failed=1
        fi
    fi
    report+=$'\n'"| $label | $pct % | $hit of $total | $verdict |"
}

row "src/core" src/core "${FLOOR_CORE:-}"
row "src/engine" src/engine "${FLOOR_ENGINE:-}"
row "src/store" src/store "${FLOOR_STORE:-}"
row "src/mirror" src/mirror
row "src/ui" src/ui
row "all of src/" src

printf '%s\n' "$report"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    { echo "## Coverage of src/ by the offline suites"; echo; printf '%s\n' "$report"; } >> "$GITHUB_STEP_SUMMARY"
fi
exit "$failed"
