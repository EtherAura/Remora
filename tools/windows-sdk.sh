#!/usr/bin/env bash
# Fetch the pinned Windows SDK the cross-build links against (bd remora-7oih):
#
#   tools/windows-sdk.sh                       # into ${REMORA_WINDOWS_SDK:-~/.cache/remora-windows-sdk}
#   cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake
#   cmake --build build-win
#   tools/windows-wine.sh build-win/remora.exe --version
#
# What it holds, and why each pin is the one it is:
#   qt/             Qt 6.11.3, the official llvm-mingw build (qtbase, qtsvg, qtmultimedia). The
#                   host's Qt has to be the same version, because moc/rcc/uic run here. Plus
#                   Qt's Mesa opengl32sw.dll, which Qt falls back to where Windows' own OpenGL is
#                   only 1.1, as on any machine (or VM) without a GPU driver.
#   llvm-mingw/     the Linux-hosted llvm-mingw release Qt 6.11.3 itself was built with, so the
#                   program and Qt share one libc++.
#   ffmpeg/         BtbN's LGPL shared build of FFmpeg 8.1, the libavcodec the Linux build uses,
#                   plus nv-codec-headers for the CUDA interop's dynamic loader.
#   platform-tools/ adb for Windows, which the mirror drives exactly as it drives adb on Linux.
#
# Every archive is checked against a SHA-256 recorded here before anything is unpacked. BtbN
# prunes old autobuilds, so its URL will eventually 404: pick a current n8.1 build from
# github.com/BtbN/FFmpeg-Builds/releases and update the URL and its checksum together.
set -euo pipefail

SDK="${REMORA_WINDOWS_SDK:-$HOME/.cache/remora-windows-sdk}"
DL="$SDK/dl"

QT_REPO=https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6113/qt6_6113_llvm_mingw
QT_VER=6.11.3-0-202610220312
QT_SUFFIX=-Windows-Windows_11_24H2-Clang-Windows-Windows_11_24H2-X86_64.7z

# name  url  sha256
ARCHIVES=(
    "qtbase.7z $QT_REPO/qt.qt6.6113.win64_llvm_mingw/${QT_VER}qtbase$QT_SUFFIX f6b4e75f397623ef4e8dfd99693da683d714d6794c8217836c95dff34da30200"
    "qtsvg.7z $QT_REPO/qt.qt6.6113.win64_llvm_mingw/${QT_VER}qtsvg$QT_SUFFIX cb90784087c27c8f0531c11aeb0f579228f484b9c7810aea580a0baea9433b72"
    "qtmultimedia.7z $QT_REPO/qt.qt6.6113.addons.qtmultimedia.win64_llvm_mingw/${QT_VER}qtmultimedia$QT_SUFFIX 0da0a97dccfd23cfab734e7b7762678f79c87e503bce93c7af56685d0c836663"
    "opengl32sw.7z $QT_REPO/qt.qt6.6113.win64_llvm_mingw/${QT_VER}opengl32sw-64-mesa_11_2_2-signed_sha256.7z dc4bbd49a23aab35df9aa2e706e6cc97fbb6b4e018db66bd72e6f5400c5297c8"
    "llvm-mingw.tar.xz https://github.com/mstorsjo/llvm-mingw/releases/download/20231128/llvm-mingw-20231128-ucrt-ubuntu-20.04-x86_64.tar.xz 0f21ff3be90311092fe32e0e30878ef3ae9d9437b8d9ac25ef279e0d84e9bb8e"
    "ffmpeg.zip https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-11-15-12-40/ffmpeg-n8.1.3-35-gf1acb0a266-win64-lgpl-shared-8.1.zip c11b3c0e91eda3b85dc6c4d5fda8e8c53d18e1cbe2b3ad82d785049f2f58b6a5"
    "nv-codec-headers.tar.gz https://github.com/FFmpeg/nv-codec-headers/releases/download/n13.0.19.0/nv-codec-headers-13.0.19.0.tar.gz 13da39edb3a40ed9713ae390ca89faa2f1202c9dda869ef306a8d4383e242bee"
    "platform-tools.zip https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip 45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99"
)

for tool in curl sha256sum 7z tar; do
    command -v "$tool" >/dev/null || { echo "windows-sdk: needs $tool" >&2; exit 1; }
done

# The pins, hashed: a stamp that matches means this exact SDK is already unpacked.
STAMP=$(printf '%s\n' "${ARCHIVES[@]}" | sha256sum | cut -c1-16)
if [[ -f "$SDK/.stamp" && "$(cat "$SDK/.stamp")" == "$STAMP" ]]; then
    echo "windows-sdk: up to date at $SDK"
    exit 0
fi

mkdir -p "$DL"
for entry in "${ARCHIVES[@]}"; do
    read -r name url sum <<<"$entry"
    if [[ ! -f "$DL/$name" ]] || ! echo "$sum  $DL/$name" | sha256sum -c --quiet - 2>/dev/null; then
        echo "windows-sdk: fetching $name"
        curl -fL --retry 3 -o "$DL/$name.part" "$url"
        mv "$DL/$name.part" "$DL/$name"
    fi
    echo "$sum  $DL/$name" | sha256sum -c --quiet - || {
        echo "windows-sdk: $name does not match its pinned checksum — refusing to unpack it" >&2
        exit 1
    }
done

# Unpack into a fresh directory and swap it in, so an interrupted run never leaves a half SDK
# that the stamp would later vouch for.
NEW="$SDK/.new"
rm -rf "$NEW"
mkdir -p "$NEW/qt"
for m in qtbase qtsvg qtmultimedia; do 7z x -y -o"$NEW/qt" "$DL/$m.7z" >/dev/null; done
7z x -y -o"$NEW/qt/bin" "$DL/opengl32sw.7z" >/dev/null
tar -xf "$DL/llvm-mingw.tar.xz" -C "$NEW"
mv "$NEW"/llvm-mingw-* "$NEW/llvm-mingw"
7z x -y -o"$NEW" "$DL/ffmpeg.zip" >/dev/null
mv "$NEW"/ffmpeg-* "$NEW/ffmpeg"
tar -xzf "$DL/nv-codec-headers.tar.gz" -C "$NEW"
cp -r "$NEW"/nv-codec-headers-*/include/ffnvcodec "$NEW/ffmpeg/include/"
rm -rf "$NEW"/nv-codec-headers-*
7z x -y -o"$NEW" "$DL/platform-tools.zip" >/dev/null

for d in qt llvm-mingw ffmpeg platform-tools; do
    rm -rf "${SDK:?}/$d"
    mv "$NEW/$d" "$SDK/$d"
done
rmdir "$NEW"
echo "$STAMP" >"$SDK/.stamp"
echo "windows-sdk: ready at $SDK"
