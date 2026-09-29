#!/bin/sh
# Build the nb_sigfix Zygisk module (bd remora-4ei.99) from src/ into zygisk/x86_64.so.
#
# This is a Remora-authored native module — NOT a third-party payload — so its source lives here
# and the committed zygisk/x86_64.so is reproducible from it. Rebuild it whenever src/ changes.
#
# It is x86_64-only on purpose: the target (TikTok, com.zhiliaoapp.musically) runs as a 64-bit
# process under berberis, so the 64-bit zygote is the only one that needs to load it. No x86.so is
# shipped; ReZygisk simply does not inject into 32-bit processes for this module.
#
# Usage:  build.sh <aosp-tree>
#   <aosp-tree> supplies the clang, the NDK sysroot (out/soong/ndk/sysroot, present after a build)
#   and jni.h. Defaults to $REMORA_SOURCE_TREE.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="${1:-${REMORA_SOURCE_TREE:?usage: build.sh <aosp-tree>}}"

CLANGXX="$(ls "$TREE"/prebuilts/clang/host/linux-x86/*/bin/clang++ 2>/dev/null | tail -1)"
SYSROOT="$TREE/out/soong/ndk/sysroot"
JNIINC="$TREE/libnativehelper/include_jni"
for p in "$CLANGXX" "$SYSROOT/usr/include" "$JNIINC/jni.h"; do
    [ -e "$p" ] || { echo "nb_sigfix build: missing $p (build the tree first?)" >&2; exit 1; }
done

mkdir -p "$HERE/zygisk"
"$CLANGXX" -target x86_64-linux-android31 --sysroot="$SYSROOT" \
    -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fno-exceptions -fno-rtti -nostdlib++ \
    -fno-threadsafe-statics \
    -I"$HERE/src" -I"$JNIINC" \
    -o "$HERE/zygisk/x86_64.so" "$HERE/src/module.cpp" -llog \
    -Wl,--no-undefined -Wl,-soname,nb_sigfix_zygisk.so

echo "nb_sigfix: built $HERE/zygisk/x86_64.so"
