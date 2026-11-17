#!/usr/bin/env bash
# Run a Windows build under Wine against the SDK's runtime (bd remora-7oih):
#
#   tools/windows-wine.sh build-win/remora.exe plan --backend remote
#   QT_QPA_PLATFORM=offscreen tools/windows-wine.sh build-win/remora.exe mirror -s <host>:5555 \
#       --hwdec=off --record "Z:$PWD/out.mkv"
#   tools/windows-wine.sh adb.exe connect <host>:5555      # the SDK's adb, on the Wine server
#
# Host paths in arguments need a Z: in front; Wine maps Z: to /.
#
# A smoke test, not a sign-off: Wine runs the host's own Linux binaries when a Windows program asks
# for one, so a `sh` or `docker` that real Windows lacks is found here anyway, and GPU decode,
# audio and window behaviour are Wine's rather than Windows'. Those belong on a Windows machine.
#
# adb, two rules that each cost something the first time:
#   - Its own server port (5038). Windows adb on 5037 reaches the HOST's server, and a version
#     mismatch makes it kill and restart that server, which drops every forward it holds, including
#     a live mirror's.
#   - The host's adb key, installed before the first adb runs. Otherwise Wine's adb makes a key of
#     its own, the device puts up "Allow USB debugging?" for it, and that prompt sits on top of
#     whatever the device is showing until somebody dismisses it.
set -euo pipefail

SDK="${REMORA_WINDOWS_SDK:-$HOME/.cache/remora-windows-sdk}"
[[ -d "$SDK/qt" ]] || { echo "windows-wine: no SDK at $SDK — run tools/windows-sdk.sh" >&2; exit 1; }
command -v wine >/dev/null || { echo "windows-wine: needs wine" >&2; exit 1; }

export WINEPREFIX="${WINEPREFIX:-$SDK/wineprefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"  # no Mono/Gecko install prompts
export WINEPATH="Z:$SDK/qt/bin;Z:$SDK/ffmpeg/bin;Z:$SDK/llvm-mingw/x86_64-w64-mingw32/bin;Z:$SDK/platform-tools"
export QT_PLUGIN_PATH="Z:$SDK/qt/plugins"
export ANDROID_ADB_SERVER_PORT="${ANDROID_ADB_SERVER_PORT:-5038}"

if [[ ! -d "$WINEPREFIX/drive_c/users" ]]; then
    wineboot --init >/dev/null 2>&1
fi
if [[ -f "$HOME/.android/adbkey" ]]; then
    for u in "$WINEPREFIX"/drive_c/users/*/; do
        [[ "$(basename "$u")" == Public ]] && continue
        mkdir -p "$u.android"
        if ! cmp -s "$HOME/.android/adbkey" "$u.android/adbkey"; then
            install -m600 "$HOME/.android/adbkey" "$HOME/.android/adbkey.pub" "$u.android/"
        fi
    done
fi

exe="$1"
shift
[[ "$exe" == adb.exe ]] && exe="$SDK/platform-tools/adb.exe"
exec wine "$exe" "$@"
