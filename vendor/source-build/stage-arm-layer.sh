#!/bin/sh
# Stage the build context of the arm_translate layer (bd remora-6laf.5) for `remora build --base`:
#
#   vendor/source-build/stage-arm-layer.sh [<build-dir>] [<ndk_translation payload dir>]
#
# <build-dir> is the profile's build_dir (default ~/.cache/remora/image-build); the layer lands in
# its ndkguest/. The payload defaults to the vendored features/ndk_translation. It is proprietary,
# with an unrecorded source, so it is never fetched: supply it. Only the build vendor/PAYLOADS.md
# names works, because patch-arm-translation.py refuses any other by hash.
set -u
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-${HOME}/.cache/remora/image-build}
ndk=${2:-$here/features/ndk_translation}
# shellcheck source=vendor/source-build/ndk-overlay.sh
. "$here/ndk-overlay.sh"
mkdir -p "$build" || exit 1
stage_arm_layer "$ndk" "$here" "$build" || { echo "stage-arm-layer: failed" >&2; exit 1; }
echo "staged the arm_translate layer in $build/ndkguest"
