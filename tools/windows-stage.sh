#!/usr/bin/env bash
# Stage a runnable Windows folder from a cross-build (bd remora-7oih):
#
#   tools/windows-stage.sh build-win dist-win [<guest-dir>]
#   tools/windows-vm.sh put dist-win C:/remora && tools/windows-vm.sh ssh C:\\remora\\remora.exe --version
#
# or the test suites, with what they import and run-tests.cmd to run them (bd remora-7oih.7):
#
#   tools/windows-stage.sh --tests build-win dist-tests
#   dist-tests\run-tests.cmd <checkout> [suite ...]       # on Windows, against a clone of this commit
#
# What Windows needs beside remora.exe, all of it built or fetched on this (Linux) machine:
#   - the guest Android runs in (guest/build.sh's output, default build-guest/) and the QEMU that
#     runs it, in guest/ and qemu/ (bd remora-7oih.5);
#   - vendor/: the scripts and container files a deploy pushes to the docker host, as tracked in
#     git, less source-build/ (source builds need a Linux machine), plus fwd and uinput-kbd, the
#     two Linux helpers the container runs, compiled here statically: a Windows tree has no
#     compiler that makes Linux binaries.
#
# remora.exe (the CLI) and remora-gui.exe (the same program as a Windows-subsystem app: what a
# shortcut starts), plus every DLL they need, found by walking PE import tables recursively through the SDK
# (Qt, FFmpeg, the llvm-mingw runtime) — the list cannot go stale the way a hand-kept one does.
# Qt's plugins are loaded at run time, invisible to that walk, so their directories are copied
# whole and walked in turn. adb comes from the SDK's platform-tools. Windows' own DLLs are left out.
set -euo pipefail

USAGE="usage: windows-stage.sh [--tests] <build-dir> <out-dir> [<guest-dir>]"
TESTS=0
if [[ "${1:-}" == --tests ]]; then
    TESTS=1
    shift
fi
BUILD="${1:?$USAGE}"
OUT="${2:?$USAGE}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GUEST="${3:-$ROOT/build-guest}"
SDK="${REMORA_WINDOWS_SDK:-$HOME/.cache/remora-windows-sdk}"
OBJDUMP="$SDK/llvm-mingw/bin/llvm-objdump"
SEARCH=("$SDK/qt/bin" "$SDK/ffmpeg/bin" "$SDK/llvm-mingw/x86_64-w64-mingw32/bin")
# sqldrivers: qsqlite.dll, which the Reddit sign-in edits account databases with (bd remora-7oih.3).
PLUGINS=(platforms styles imageformats iconengines tls multimedia networkinformation sqldrivers)
# The suites are Qt Core, Network and Sql, with no GUI: the engine's network and database plugins.
((TESTS)) && PLUGINS=(tls networkinformation sqldrivers)

[[ -x "$OBJDUMP" ]] || { echo "windows-stage: no SDK at $SDK — run tools/windows-sdk.sh" >&2; exit 1; }
if ((TESTS)); then
    compgen -G "$BUILD/test_*.exe" >/dev/null ||
        { echo "windows-stage: no test_*.exe in $BUILD" >&2; exit 1; }
else
    [[ -f "$BUILD/remora.exe" ]] || { echo "windows-stage: $BUILD/remora.exe not built" >&2; exit 1; }
    [[ -f "$GUEST/system.qcow2" ]] || { echo "windows-stage: no guest image in $GUEST — run guest/build.sh" >&2; exit 1; }
fi

rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for p in "${PLUGINS[@]}"; do
    [[ -d "$SDK/qt/plugins/$p" ]] && mkdir -p "$OUT/$p" && cp "$SDK/qt/plugins/$p"/*.dll "$OUT/$p/"
done
# Qt's FFmpeg media backend links Qt's own FFmpeg 7, a second FFmpeg beside the one Remora decodes
# with (21 MB). Remora's only multimedia is QAudioSink, which Qt serves without either backend, so
# only the native Windows one ships.
rm -f "$OUT/multimedia/ffmpegmediaplugin.dll"
if ((TESTS)); then
    cp "$BUILD"/test_*.exe "$OUT/"
    # CRLF: cmd misreads a batch file with LF line ends, and the checkout keeps this one LF.
    sed 's/$/\r/' "$ROOT/tools/windows-run-tests.cmd" > "$OUT/run-tests.cmd"
else
    cp "$BUILD/remora.exe" "$BUILD/remora-gui.exe" "$OUT/"
    cp "$SDK/platform-tools"/{adb.exe,AdbWinApi.dll,AdbWinUsbApi.dll} "$OUT/"
    # Loaded by name at run time when the system OpenGL is too old for Qt, so no import names it.
    cp "$SDK/qt/bin/opengl32sw.dll" "$OUT/"
    # The guest's hypervisor front end, already trimmed by tools/windows-sdk.sh. Its own directory:
    # its DLLs (an older libstdc++, GLib, GTK) must not be the ones remora.exe finds first.
    cp -r "$SDK/qemu" "$OUT/qemu"
    mkdir -p "$OUT/guest"
    cp "$GUEST"/{vmlinuz,system.qcow2,cmdline,manifest.txt} "$OUT/guest/"

    (cd "$ROOT" && git ls-files -z vendor) | grep -zv '^vendor/source-build/' |
        (cd "$ROOT" && xargs -0 cp --parents -t "$OUT")
    for helper in fwd uinput-kbd; do
        cc -static -O2 "$ROOT/vendor/native/$helper.c" -o "$OUT/vendor/native/$helper"
    done
fi

# Breadth-first over import tables. A DLL found in none of the SDK dirs is the system's (KERNEL32,
# USER32, the UCRT api-ms-win-* sets…) and stays out.
declare -A seen=()
queue=()
while IFS= read -r -d '' f; do queue+=("$f"); done < <(find "$OUT" \( -path "$OUT/qemu" -o -path "$OUT/vendor" \) -prune -o \( -name '*.exe' -o -name '*.dll' \) -print0)
while ((${#queue[@]})); do
    f="${queue[0]}"
    queue=("${queue[@]:1}")
    while read -r dll; do
        key="${dll,,}"
        [[ -n "${seen[$key]:-}" ]] && continue
        seen[$key]=1
        [[ -e "$OUT/$dll" ]] && continue
        for d in "${SEARCH[@]}"; do
            if [[ -f "$d/$dll" ]]; then
                cp "$d/$dll" "$OUT/"
                queue+=("$OUT/$dll")
                break
            fi
        done
    done < <("$OBJDUMP" -p "$f" | sed -n 's/^ *DLL Name: //p')
done

printf 'windows-stage: %s — %s files, %s\n' "$OUT" "$(find "$OUT" -type f | wc -l)" "$(du -sh "$OUT" | cut -f1)"
