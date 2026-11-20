#!/bin/sh
# Exercise the installed CLI and X11 application from an empty user profile.
set -eu

expected_version=${1:?usage: freebsd-installed-smoke.sh VERSION}
test "$(id -u)" -ne 0
test "$(command -v remora)" = /usr/local/bin/remora
test "$(remora --version)" = "remora $expected_version"

tmp=$(mktemp -d)
xvfb=
gui=
# shellcheck disable=SC2317
cleanup() {
    for pid in "$gui" "$xvfb"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
export HOME="$tmp"
export XDG_CONFIG_HOME="$tmp/config" XDG_DATA_HOME="$tmp/data" XDG_CACHE_HOME="$tmp/cache"
export XDG_STATE_HOME="$tmp/state" XDG_RUNTIME_DIR="$tmp/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"
adb version

if remora check --backend bare >"$tmp/bare.log" 2>&1; then
    echo "bare preflight unexpectedly passed on FreeBSD" >&2
    exit 1
fi
cat "$tmp/bare.log"
grep -F 'backend=remote with ssh_host=<user>@<host>' "$tmp/bare.log"

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp \
    3>"$tmp/display" >"$tmp/xvfb.log" 2>&1 &
xvfb=$!
for _ in $(jot 50); do
    if [ -s "$tmp/display" ]; then
        break
    fi
    if ! kill -0 "$xvfb" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if [ ! -s "$tmp/display" ]; then
    cat "$tmp/xvfb.log" >&2
    exit 1
fi
DISPLAY=:"$(cat "$tmp/display")"
export DISPLAY QT_QPA_PLATFORM=xcb
remora gui >"$tmp/remora.log" 2>&1 &
gui=$!
for _ in $(jot 50); do
    if xdotool search --onlyvisible --pid "$gui" --name Remora >"$tmp/window"; then
        xdotool getwindowname "$(head -n 1 "$tmp/window")"
        exit 0
    fi
    if ! kill -0 "$gui" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
cat "$tmp/remora.log" "$tmp/xvfb.log" >&2
exit 1
