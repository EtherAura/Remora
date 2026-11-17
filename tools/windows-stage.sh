#!/usr/bin/env bash
# Stage a runnable Windows folder from a cross-build (bd remora-7oih):
#
#   tools/windows-stage.sh build-win dist-win
#   tools/windows-vm.sh put dist-win C:/remora && tools/windows-vm.sh ssh C:\\remora\\remora.exe --version
#
# remora.exe plus every DLL it needs, found by walking PE import tables recursively through the SDK
# (Qt, FFmpeg, the llvm-mingw runtime) — the list cannot go stale the way a hand-kept one does.
# Qt's plugins are loaded at run time, invisible to that walk, so their directories are copied
# whole and walked in turn. adb comes from the SDK's platform-tools. Windows' own DLLs are left out.
set -euo pipefail

BUILD="${1:?usage: windows-stage.sh <build-dir> <out-dir>}"
OUT="${2:?usage: windows-stage.sh <build-dir> <out-dir>}"
SDK="${REMORA_WINDOWS_SDK:-$HOME/.cache/remora-windows-sdk}"
OBJDUMP="$SDK/llvm-mingw/bin/llvm-objdump"
SEARCH=("$SDK/qt/bin" "$SDK/ffmpeg/bin" "$SDK/llvm-mingw/x86_64-w64-mingw32/bin")
PLUGINS=(platforms styles imageformats iconengines tls multimedia networkinformation)

[[ -x "$OBJDUMP" ]] || { echo "windows-stage: no SDK at $SDK — run tools/windows-sdk.sh" >&2; exit 1; }
[[ -f "$BUILD/remora.exe" ]] || { echo "windows-stage: $BUILD/remora.exe not built" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD/remora.exe" "$OUT/"
for p in "${PLUGINS[@]}"; do
    [[ -d "$SDK/qt/plugins/$p" ]] && mkdir -p "$OUT/$p" && cp "$SDK/qt/plugins/$p"/*.dll "$OUT/$p/"
done
cp "$SDK/platform-tools"/{adb.exe,AdbWinApi.dll,AdbWinUsbApi.dll} "$OUT/"
# Loaded by name at run time when the system OpenGL is too old for Qt, so no import names it.
cp "$SDK/qt/bin/opengl32sw.dll" "$OUT/"

# Breadth-first over import tables. A DLL found in none of the SDK dirs is the system's (KERNEL32,
# USER32, the UCRT api-ms-win-* sets…) and stays out.
declare -A seen=()
queue=()
while IFS= read -r -d '' f; do queue+=("$f"); done < <(find "$OUT" -name '*.exe' -print0 -o -name '*.dll' -print0)
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
