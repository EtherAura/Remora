#!/usr/bin/env bash
# Does a configure of the real project log the Qt versions it found?
#
#   tools/test-qt-version-report.sh
#
# Offline: nothing is fetched or built. It configures the project, from scratch, into a scratch
# directory with the installed toolchain, and separately configures a throwaway probe project that
# asks CMake for Qt6Core and Qt6WebEngineWidgets directly, each on its own. The project's log must
# name the Core version the probe found, and the probe's WebEngineWidgets version, or "not found"
# when the probe found none.
#
# The checker is then fed logs with the lines missing or wrong (Core reported as the requested
# 6.4 minimum, WebEngine reported as Core's version, a present WebEngine reported absent) and has
# to refuse each, so a pass means the check can fail. Those logs are edited copies in the scratch
# directory: the tree is never touched.
#
# This is evidence of what this host's Qt is and that the configure log says so. Which Qt a CI
# runner has is read from that runner's own configure log.
set -euo pipefail

if [ "$#" -ne 0 ]; then
    printf '%s\n' "usage: tools/test-qt-version-report.sh (no arguments)" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

failures=0
note() { printf '%s\n' "test-qt-version-report: $*"; }
pass() { note "ok   $*"; }
fail() { note "FAIL $*" >&2; failures=$((failures + 1)); }

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

# The probe: the packages' own versions, found without the project's umbrella find_package(Qt6).
mkdir -p "$SCRATCH/probe"
cat > "$SCRATCH/probe/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(qt_version_probe LANGUAGES CXX)  # a language, or CMake searches no lib64 prefix
find_package(Qt6Core CONFIG REQUIRED)
find_package(Qt6WebEngineWidgets CONFIG QUIET)
set(_out "core_version=${Qt6Core_VERSION}\n")
if(Qt6WebEngineWidgets_FOUND)
    string(APPEND _out "webengine_version=${Qt6WebEngineWidgets_VERSION}\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/qt-versions.txt" "${_out}")
EOF
if ! cmake -S "$SCRATCH/probe" -B "$SCRATCH/probe-build" > "$SCRATCH/probe.log" 2>&1; then
    cat "$SCRATCH/probe.log" >&2
    note "the probe could not find Qt6Core: nothing to compare against" >&2
    exit 1
fi
probe() { sed -n "s/^$1=//p" "$SCRATCH/probe-build/qt-versions.txt"; }
CORE_VERSION=$(probe core_version)
WEBENGINE_VERSION=$(probe webengine_version)
note "probe: Qt6Core $CORE_VERSION"
if [ -n "$WEBENGINE_VERSION" ]; then
    note "probe: Qt6WebEngineWidgets $WEBENGINE_VERSION"
else
    note "probe: Qt6WebEngineWidgets not found"
fi
if [[ "$CORE_VERSION" =~ ^6\.[0-9]+\.[0-9]+ ]]; then
    pass "the probe resolved a Qt 6 Core version"
else
    fail "the probe resolved Core version '$CORE_VERSION', not a Qt 6 one"
fi

# check_log <log>: does it report exactly what the probe found, once each?
check_log() {
    local log="$1" want_we ok=0
    if [ "$(grep -cxF -- "-- Qt6Core: $CORE_VERSION" "$log")" != 1 ] \
        || [ "$(grep -c -- '^-- Qt6Core:' "$log")" != 1 ]; then
        ok=1
    fi
    if [ -n "$WEBENGINE_VERSION" ]; then
        want_we="-- Qt6WebEngineWidgets: $WEBENGINE_VERSION"
    else
        want_we="-- Qt6WebEngineWidgets: not found"
    fi
    if [ "$(grep -cxF -- "$want_we" "$log")" != 1 ] \
        || [ "$(grep -c -- '^-- Qt6WebEngineWidgets:' "$log")" != 1 ]; then
        ok=1
    fi
    return "$ok"
}

note "configuring the project from scratch"
if cmake -S "$ROOT" -B "$SCRATCH/build" > "$SCRATCH/configure.log" 2>&1; then
    pass "the project configures"
else
    cat "$SCRATCH/configure.log" >&2
    fail "the project did not configure"
fi
grep -E '^-- Qt6(Core|WebEngineWidgets):' "$SCRATCH/configure.log" || true
if check_log "$SCRATCH/configure.log"; then
    pass "the configure log reports the Qt6Core and Qt6WebEngineWidgets the probe resolved"
else
    fail "the configure log does not report the Qt the probe resolved"
fi

# A version that is not <version>: the same with its last component bumped.
other_than() { awk -F. -v OFS=. '{ $NF = $NF + 1; print }' <<< "$1"; }

# refused <what> <sed script>: the real log, edited by <sed script>, must fail check_log.
refused() {
    local bad="$SCRATCH/refused-$RANDOM.log"
    sed -e "$2" "$SCRATCH/configure.log" > "$bad"
    if cmp -s "$bad" "$SCRATCH/configure.log"; then
        fail "refusal case '$1' did not change the log"
    elif check_log "$bad"; then
        fail "accepted a log with $1"
    else
        pass "refuses a log with $1"
    fi
}
# The edits rewrite what the real configure printed, so escape it for sed.
esc() { printf '%s' "$1" | sed -e 's/[]\/$*.^[]/\\&/g'; }

CORE_RE=$(esc "$CORE_VERSION")
MINIMUM=6.4
if [ "$CORE_VERSION" = "$MINIMUM" ]; then MINIMUM=6.4.0; fi
refused "no Qt6Core line" '/^-- Qt6Core:/d'
refused "no Qt6WebEngineWidgets line" '/^-- Qt6WebEngineWidgets:/d'
refused "Core reported as the requested $MINIMUM" \
    "s/^-- Qt6Core: $CORE_RE\$/-- Qt6Core: $MINIMUM/"
refused "Core reported as another version" \
    "s/^-- Qt6Core: $CORE_RE\$/-- Qt6Core: $(other_than "$CORE_VERSION")/"
refused "Core reported with no version" "s/^-- Qt6Core: $CORE_RE\$/-- Qt6Core: /"
refused "the Qt6Core line twice, once wrong" \
    "/^-- Qt6Core: /{p;s/$CORE_RE\$/$(other_than "$CORE_VERSION")/;}"
if [ -n "$WEBENGINE_VERSION" ]; then
    WRONG_WE=$CORE_VERSION
    if [ "$WRONG_WE" = "$WEBENGINE_VERSION" ]; then WRONG_WE=$(other_than "$WEBENGINE_VERSION"); fi
    refused "WebEngine reported as $WRONG_WE, not its own $WEBENGINE_VERSION" \
        "s/^-- Qt6WebEngineWidgets: $(esc "$WEBENGINE_VERSION")\$/-- Qt6WebEngineWidgets: $WRONG_WE/"
    refused "a present WebEngine reported as not found" \
        's/^-- Qt6WebEngineWidgets: .*/-- Qt6WebEngineWidgets: not found/'
else
    refused "an absent WebEngine reported as found" \
        "s/^-- Qt6WebEngineWidgets: not found\$/-- Qt6WebEngineWidgets: $CORE_VERSION/"
fi

if [ "$failures" -gt 0 ]; then
    note "$failures check(s) failed" >&2
    exit 1
fi
note "all checks passed"
