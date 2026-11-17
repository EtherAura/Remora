#!/usr/bin/env bash
# Build the Windows installer from a staged folder (bd remora-7oih.6):
#
#   tools/windows-stage.sh build-win dist
#   tools/windows-installer.sh dist remora-setup.exe
#
# packaging/windows/remora.nsi says what installing does. makensis is NSIS's own compiler and runs
# on Linux; the host's is used when there is one, else the one in Ubuntu's nsis package, in a
# pinned container.
set -euo pipefail

DIST="${1:?usage: windows-installer.sh <staged-dir> <installer.exe>}"
OUT="${2:?usage: windows-installer.sh <staged-dir> <installer.exe>}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=docker.io/library/ubuntu:24.04@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8

[[ -f "$DIST/remora-gui.exe" ]] || { echo "windows-installer: $DIST is not a staged folder" >&2; exit 1; }
DIST=$(cd "$DIST" && pwd)
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
VERSION=$(cat "$ROOT/VERSION")

if command -v makensis >/dev/null; then
    makensis -V2 -DDIST="$DIST" -DVERSION="$VERSION" -DOUTFILE="$OUT" \
        "$ROOT/packaging/windows/remora.nsi"
else
    command -v docker >/dev/null || { echo "windows-installer: needs makensis or docker" >&2; exit 1; }
    docker run --rm -v "$ROOT/packaging/windows:/nsi:ro" -v "$DIST:/dist:ro" \
        -v "$(dirname "$OUT"):/out" -e OWNER="$(id -u):$(id -g)" "$IMAGE" sh -c "
            apt-get update -qq >/dev/null && apt-get install -y -qq nsis >/dev/null &&
            makensis -V2 -DDIST=/dist -DVERSION='$VERSION' -DOUTFILE='/out/$(basename "$OUT")' /nsi/remora.nsi &&
            chown \"\$OWNER\" '/out/$(basename "$OUT")'"
fi
echo "windows-installer: $OUT ($(du -h --apparent-size "$OUT" | cut -f1))"
