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
#   qt/             Qt 6.11.3, the official llvm-mingw build (qtbase, qtsvg, qtmultimedia). Plus
#                   Qt's Mesa opengl32sw.dll, which Qt falls back to where Windows' own OpenGL is
#                   only 1.1, as on any machine (or VM) without a GPU driver.
#   host-qt/        Qt 6.11.3 for Linux, qtbase and the ICU its rcc links: moc, rcc and uic run on the
#                   build machine and
#                   must match the target Qt's version exactly, which no distribution's Qt promises
#                   (Ubuntu 24.04 ships 6.4). Always this one, so every machine builds alike.
#   llvm-mingw/     the Linux-hosted llvm-mingw release Qt 6.11.3 itself was built with, so the
#                   program and Qt share one libc++.
#   ffmpeg/         BtbN's LGPL shared build of FFmpeg 8.1, the libavcodec the Linux build uses,
#                   plus nv-codec-headers for the CUDA interop's dynamic loader.
#   platform-tools/ adb for Windows, which the mirror drives exactly as it drives adb on Linux.
#   qemu/           QEMU for Windows (Stefan Weil's build, the one qemu.org links), trimmed to
#                   qemu-system-x86_64 and qemu-img, the DLLs their import tables name, and the
#                   firmware a direct kernel boot reads. It runs the Linux guest Android lives in
#                   (bd remora-7oih.5). GPLv2: its COPYING travels with it, and the source is the
#                   tagged QEMU release this build names.
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
    "host-qtbase.7z https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_6113/qt6_6113/qt.qt6.6113.linux_gcc_64/${QT_VER}qtbase-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z ce282da4d8a83dc216ed96b715202872d655177189c16205dc5fab09b41a9f09"
    "host-icu.7z https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_6113/qt6_6113/qt.qt6.6113.linux_gcc_64/${QT_VER}icu-linux-Rhel8.6-x86_64.7z 16f1e8f409bd9f0b248e36d10deab498d0c1b33d25e0a515a076b1f6b5a1ab28"
    "opengl32sw.7z $QT_REPO/qt.qt6.6113.win64_llvm_mingw/${QT_VER}opengl32sw-64-mesa_11_2_2-signed_sha256.7z dc4bbd49a23aab35df9aa2e706e6cc97fbb6b4e018db66bd72e6f5400c5297c8"
    "llvm-mingw.tar.xz https://github.com/mstorsjo/llvm-mingw/releases/download/20231128/llvm-mingw-20231128-ucrt-ubuntu-20.04-x86_64.tar.xz 0f21ff3be90311092fe32e0e30878ef3ae9d9437b8d9ac25ef279e0d84e9bb8e"
    "ffmpeg.zip https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-11-15-12-40/ffmpeg-n8.1.3-35-gf1acb0a266-win64-lgpl-shared-8.1.zip c11b3c0e91eda3b85dc6c4d5fda8e8c53d18e1cbe2b3ad82d785049f2f58b6a5"
    "nv-codec-headers.tar.gz https://github.com/FFmpeg/nv-codec-headers/releases/download/n13.0.19.0/nv-codec-headers-13.0.19.0.tar.gz 13da39edb3a40ed9713ae390ca89faa2f1202c9dda869ef306a8d4383e242bee"
    "qemu-setup.exe https://qemu.weilnetz.de/w64/qemu-w64-setup-20260811.exe f98a8aeb5f7faea9765b6dee28316c266cd179d80354a2fed8e50176f9a2e59f"
    "platform-tools.zip https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip 45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99"
)

for tool in curl sha256sum 7z bsdtar tar; do
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
# bsdtar, not 7z: the archive's library symlinks point at other symlinks, which 7-Zip refuses as
# "dangerous" and then fails the whole extraction over.
mkdir -p "$NEW/host-qt"
bsdtar -xf "$DL/host-qtbase.7z" -C "$NEW/host-qt"
bsdtar -xf "$DL/host-icu.7z" -C "$NEW/host-qt/lib"
tar -xf "$DL/llvm-mingw.tar.xz" -C "$NEW"
mv "$NEW"/llvm-mingw-* "$NEW/llvm-mingw"
7z x -y -o"$NEW" "$DL/ffmpeg.zip" >/dev/null
mv "$NEW"/ffmpeg-* "$NEW/ffmpeg"
tar -xzf "$DL/nv-codec-headers.tar.gz" -C "$NEW"
cp -r "$NEW"/nv-codec-headers-*/include/ffnvcodec "$NEW/ffmpeg/include/"
rm -rf "$NEW"/nv-codec-headers-*
7z x -y -o"$NEW" "$DL/platform-tools.zip" >/dev/null

# QEMU's installer holds every target and a GTK UI; Remora needs one target, headless. Keep the
# two programs and walk their import tables for the DLLs, as tools/windows-stage.sh does.
7z x -y -o"$NEW/qemu-all" "$DL/qemu-setup.exe" >/dev/null
mkdir -p "$NEW/qemu/share"
cp "$NEW/qemu-all"/{qemu-system-x86_64.exe,qemu-img.exe,COPYING,COPYING.LIB} "$NEW/qemu/"
cp "$NEW/qemu-all/share"/{bios-256k.bin,linuxboot_dma.bin,kvmvapic.bin} "$NEW/qemu/share/"
queue=("$NEW/qemu/qemu-system-x86_64.exe" "$NEW/qemu/qemu-img.exe")
while ((${#queue[@]})); do
    f="${queue[0]}"
    queue=("${queue[@]:1}")
    while read -r dll; do
        if [[ -f "$NEW/qemu-all/$dll" && ! -e "$NEW/qemu/$dll" ]]; then
            cp "$NEW/qemu-all/$dll" "$NEW/qemu/"
            queue+=("$NEW/qemu/$dll")
        fi
    done < <("$NEW/llvm-mingw/bin/llvm-objdump" -p "$f" | sed -n 's/^ *DLL Name: //p')
done
rm -rf "$NEW/qemu-all"

for d in qt host-qt llvm-mingw ffmpeg platform-tools qemu; do
    rm -rf "${SDK:?}/$d"
    mv "$NEW/$d" "$SDK/$d"
done
rmdir "$NEW"
echo "$STAMP" >"$SDK/.stamp"
echo "windows-sdk: ready at $SDK"
