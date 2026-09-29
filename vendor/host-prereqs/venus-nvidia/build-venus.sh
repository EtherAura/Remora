#!/usr/bin/env bash
# Build + install the NVIDIA Venus host renderer that the bare/remote backends detect.
#
# Two halves, deliberately sourced differently:
#   virglrenderer  BUILT from source, because both Remora patches in this directory apply to it.
#   guest .so set  FETCHED from a pinned prebuilt release (WDNV_URL below). They are Android
#                  cross-builds (ANGLE is a Chromium/gn build); compiling them here would cost
#                  hours and a multi-GB toolchain to reproduce bytes upstream already publishes.
#                  Every download is SHA256-verified against the release's own SHA256SUMS.
#                  ONE exception: libgbm_mesa_wrapper.so, when guest-patches/ is non-empty.
#                  It is a single C file, it is the guest half of a host patch here, and a
#                  patched host with an unpatched wrapper is a silently broken pair — so it
#                  is rebuilt from source and the prebuilt is overwritten. See below.
#
# Idempotent: re-running resets the source tree and re-applies, and skips downloads whose
# checksum already matches. Safe to run over an existing install.
#
# Env:
#   REMORA_VENUS_DIR   install root      (default ~/.local/share/remora/venus)
#   REMORA_VENUS_WORK  build tree        (default $REMORA_VENUS_DIR/src)
#   REMORA_VENUS_TAG   guest-library release tag (default the pin below; "latest" resolves the
#                      newest upstream release and reports how far it is from the pin)
#   REMORA_ANDROID_CC  Android/bionic C compiler for the guest wrapper. Required only when
#                      guest-patches/ is non-empty. An NDK clang works, so does the AOSP
#                      tree's prebuilts/clang/host/linux-x86/clang-*/bin/clang.
#   REMORA_MINIGBM_HDR directory holding gbm_mesa_wrapper.h — the PATCHED one from the Android
#                      tree (external/minigbm/gbm_mesa_driver), since struct alloc_args is the
#                      ABI between the wrapper and gralloc.minigbm_gbm_mesa.so.
#   REMORA_ANDROID_SYSROOT  sysroot for the above (NDK layout: <ndk>/toolchains/llvm/prebuilt/
#                      linux-x86_64/sysroot). An AOSP tree's out/soong/ndk/sysroot works.
#   REMORA_ANDROID_API Android API level to link the wrapper against. Default: the highest level
#                      in the sysroot that actually ships crtbegin_so.o, which is NOT always the
#                      highest one present — see the probe below.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${REMORA_VENUS_DIR:-$HOME/.local/share/remora/venus}"
WORK="${REMORA_VENUS_WORK:-$ROOT/src}"
# The pin is what makes the build reproducible — bumping it is a deliberate refresh (bd
# remora-4ei.71): both Remora patches below edit vtest_gpu_alloc.c, which upstream also edits, so
# they must be re-checked against the new release's BASE. The apply step fails loudly if they no
# longer fit.
PIN=v0.1.2
TAG="${REMORA_VENUS_TAG:-$PIN}"
WDNV_URL=https://github.com/Shiro836/waydroid-nvidia
VIRGL_URL=https://gitlab.freedesktop.org/virgl/virglrenderer.git

say() { printf '  %s\n' "$*"; }
die() { printf 'venus-build: %s\n' "$*" >&2; exit 1; }
# `set -e` aborts with no output of its own, which once hid a clean-install failure behind a bare
# non-zero exit. Name the line instead of leaving the caller to guess.
# shellcheck disable=SC2154  # rc is assigned inside the trap string itself
trap 'rc=$?; [ $rc -ne 0 ] && printf "venus-build: aborted at line %s (exit %s)\n" "$LINENO" "$rc" >&2' EXIT

# --- 1. dependencies -------------------------------------------------------
missing=
for t in git meson ninja pkg-config curl tar sha256sum; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
command -v zstd >/dev/null 2>&1 || tar --zstd --help >/dev/null 2>&1 || missing="$missing zstd"
pkg-config --exists vulkan 2>/dev/null || missing="$missing vulkan(headers)"
pkg-config --exists libdrm 2>/dev/null || missing="$missing libdrm(headers)"
[ -n "$missing" ] && die "missing build dependencies:$missing"
say "deps ok (meson $(meson --version), vulkan $(pkg-config --modversion vulkan))"

mkdir -p "$WORK" "$ROOT/bin" "$ROOT/lib" "$ROOT/guest-libs"

# "latest" is resolved through the /releases/latest redirect rather than the API: no token, no
# rate-limit surprises, and the Location header ends in the tag name.
if [ "$TAG" = latest ]; then
    TAG=$(curl -sfI "$WDNV_URL/releases/latest" | tr -d '\r' \
              | sed -n 's#^[Ll]ocation:.*/tag/##p' | head -1)
    [ -n "$TAG" ] || die "could not resolve the latest release from $WDNV_URL/releases/latest"
    if [ "$TAG" = "$PIN" ]; then
        say "latest release is $TAG — same as the pin, nothing has moved"
    else
        say "latest release is $TAG (pin is $PIN) — the Remora patches were only checked against"
        say "the pin's virglrenderer base; if either fails to apply below, they need rebasing"
    fi
fi

# --- 2. the guest-library release repo (patches + the vtest_gpu_alloc sources)
WD="$WORK/guest-release"
if [ -d "$WD/.git" ]; then
    git -C "$WD" fetch --depth 1 --tags origin "$TAG" >/dev/null 2>&1 || true
else
    git clone --depth 1 --branch "$TAG" "$WDNV_URL" "$WD" >/dev/null 2>&1 \
        || git clone --depth 1 "$WDNV_URL" "$WD" >/dev/null 2>&1 \
        || die "could not clone $WDNV_URL"
fi
git -C "$WD" checkout -q "$TAG" 2>/dev/null || say "note: tag $TAG not found, using default branch"
# Guest-side patches, applied to that release checkout itself. Reset first: the clone
# is reused across runs and `git am`/`apply` here is not idempotent.
GP=$(ls "$HERE"/guest-patches/[0-9][0-9][0-9][0-9]-*.patch 2>/dev/null || true)
if [ -n "$GP" ]; then
    git -C "$WD" checkout -q -- . 2>/dev/null || true
    for p in $GP; do
        git -C "$WD" apply "$p" || die "guest patch $(basename "$p") failed to apply"
        say "applied guest patch $(basename "$p")"
    done
fi

P="$WD/patches/virglrenderer"
[ -d "$P" ] || die "$P missing — upstream layout changed"
BASE=$(sed -n 's/^base-commit:[[:space:]]*\([0-9a-f]\{7,\}\).*/\1/p' "$P/BASE" | head -1)
[ -n "$BASE" ] || die "could not read base-commit from $P/BASE"
say "guest-library release $TAG, virglrenderer base $BASE"

# --- 3. virglrenderer, reset to the pinned base ----------------------------
VG="$WORK/virglrenderer"
if [ -d "$VG/.git" ]; then
    git -C "$VG" fetch --quiet origin || true
else
    git clone --quiet "$VIRGL_URL" "$VG" || die "could not clone $VIRGL_URL"
fi
git -C "$VG" am --abort >/dev/null 2>&1 || true
git -C "$VG" reset --hard --quiet "$BASE" || die "base commit $BASE not found"
git -C "$VG" clean -qfd

# --- 4. patches: upstream's four, then this directory's two ----------------
export GIT_COMMITTER_NAME=Remora GIT_COMMITTER_EMAIL=remora@localhost
export GIT_AUTHOR_NAME=Remora GIT_AUTHOR_EMAIL=remora@localhost
git -C "$VG" am -q "$P"/0001-*.patch "$P"/0002-*.patch "$P"/0003-*.patch \
    || die "upstream patches 0001-0003 failed to apply"
git -C "$VG" apply "$P"/0004-wip-*.patch || die "upstream patch 0004 failed to apply"
# 0004's meson.build references these; they are shipped as plain sources, not a patch.
install -m644 "$WD/src/virglrenderer-vtest/vtest_gpu_alloc.c" \
              "$WD/src/virglrenderer-vtest/vtest_gpu_alloc.h" "$VG/vtest/" \
    || die "could not stage vtest_gpu_alloc sources"
# A glob, not an enumeration: this was a hand-written list and a new patch that
# was not added to it built cleanly while doing nothing. Globs expand in sorted
# order, so NNNN- prefixes apply in sequence and a new one is picked up by
# existing.
for p in "$HERE"/[0-9][0-9][0-9][0-9]-*.patch; do
    git -C "$VG" apply "$p" || die "Remora patch $(basename "$p") failed to apply"
    say "applied $(basename "$p")"
done

# --- 5. build --------------------------------------------------------------
if [ ! -d "$VG/build" ]; then
    (cd "$VG" && meson setup build -Dvenus=true -Drender-server-worker=auto >/dev/null) \
        || die "meson setup failed"
fi
(cd "$VG" && ninja -C build >/dev/null) || die "ninja build failed"
say "virglrenderer built"

# --- 6. install the host half ---------------------------------------------
install -m755 "$VG/build/vtest/virgl_test_server"   "$ROOT/bin/"
install -m755 "$VG/build/server/virgl_render_server" "$ROOT/bin/"
rm -f "$ROOT"/lib/libvirglrenderer.so*
# Copy the real object, then recreate the soname links — a glob copy would drag in meson's
# .p/ build directory and miss the symlinks entirely.
real=$(find "$VG/build/src" -maxdepth 1 -name 'libvirglrenderer.so.*.*' -type f | head -1)
[ -n "$real" ] || die "libvirglrenderer.so not found in $VG/build/src"
install -m755 "$real" "$ROOT/lib/"
( cd "$ROOT/lib" && b=$(basename "$real") && ln -sf "$b" libvirglrenderer.so.1 \
                                          && ln -sf "$b" libvirglrenderer.so )
say "installed host renderer -> $ROOT/bin"

# --- 7. guest libraries, from the verified release assets ------------------
DL="$WORK/dl"; mkdir -p "$DL"
BASEURL="$WDNV_URL/releases/download/$TAG"
curl -sfL -o "$DL/SHA256SUMS" "$BASEURL/SHA256SUMS" || die "could not fetch SHA256SUMS"
# The assets are named after the release repository, so derive the prefix from WDNV_URL.
REL="${WDNV_URL##*/}"
for a in "$REL-guest-android-x86_64-$TAG.tar.zst" \
         "$REL-guest-prebuilts-$TAG.tar.zst"; do
    # Compare hashes directly rather than parsing `sha256sum -c` output: the sums file lists
    # entries as "./name", so a match depends on the exact prefix the tool echoes back.
    want=$(awk -v n="$a" '$2 == n || $2 == "./" n { print $1 }' "$DL/SHA256SUMS" | head -1)
    [ -n "$want" ] || die "$a is not listed in SHA256SUMS"
    # NB an `if`, not `have=$([ -f … ] && …)`. Under `set -e` a command substitution that exits
    # non-zero takes the whole script down, so the missing-file case — i.e. every clean install —
    # aborted here silently while cached re-runs sailed through.
    if [ -f "$DL/$a" ]; then have=$(sha256sum "$DL/$a" | cut -d' ' -f1); else have=; fi
    if [ "$have" = "$want" ]; then
        say "cached $a"
    else
        curl -sfL -o "$DL/$a" "$BASEURL/$a" || die "could not fetch $a"
        have=$(sha256sum "$DL/$a" | cut -d' ' -f1)
        [ "$have" = "$want" ] || die "SHA256 mismatch for $a — refusing to install"
        say "fetched + verified $a"
    fi
done
( cd "$DL" && sha256sum -c --ignore-missing SHA256SUMS >/dev/null 2>&1 ) \
    || die "SHA256 verification FAILED for the guest tarballs — refusing to install"
say "guest tarballs verified against SHA256SUMS"

EX="$WORK/guest-extract"; rm -rf "$EX"; mkdir -p "$EX"
for a in "$DL"/*.tar.zst; do tar --zstd -xf "$a" -C "$EX"; done
# 64-bit only, by explicit path. The tarballs carry 32-bit copies of the same names under
# vendor/lib/, and a find-by-name would happily install those into a 64-bit container.
install -m644 "$EX/vendor/lib64/hw/vulkan.virtio.so"        "$ROOT/guest-libs/"
install -m644 "$EX/vendor/lib64/libgbm_mesa_wrapper.so"     "$ROOT/guest-libs/"
install -m644 "$EX/vendor/lib64/egl/libEGL_angle.so"        "$ROOT/guest-libs/"
install -m644 "$EX/vendor/lib64/egl/libGLESv2_angle.so"     "$ROOT/guest-libs/"
install -m644 "$EX/vendor/lib64/egl/libGLESv1_CM_angle.so"  "$ROOT/guest-libs/"

# Rebuild libgbm_mesa_wrapper.so over the prebuilt when guest-patches/ changed its source.
# This DIES rather than falling back: the wrapper is the guest half of a host patch, and
# shipping the unpatched prebuilt against a patched host is the exact failure this project
# keeps re-learning — it builds clean, installs clean, and silently does nothing.
if [ -n "$GP" ]; then
    ACC="${REMORA_ANDROID_CC:-}"
    HDR="${REMORA_MINIGBM_HDR:-}"
    [ -n "$ACC" ] || die "guest-patches/ present but REMORA_ANDROID_CC is unset — the patched
  wrapper cannot be built, and the fetched prebuilt does not carry the patch. Set it to an
  NDK clang or the AOSP tree's prebuilts/clang/host/linux-x86/clang-*/bin/clang."
    [ -x "$ACC" ] || die "REMORA_ANDROID_CC=$ACC is not executable"
    [ -n "$HDR" ] || die "guest-patches/ present but REMORA_MINIGBM_HDR is unset — point it at the
  PATCHED external/minigbm/gbm_mesa_driver in your Android tree; struct alloc_args is the ABI
  between this wrapper and gralloc.minigbm_gbm_mesa.so and the two must agree."
    [ -f "$HDR/gbm_mesa_wrapper.h" ] || die "no gbm_mesa_wrapper.h under REMORA_MINIGBM_HDR=$HDR"
    grep -q 'linear_required' "$HDR/gbm_mesa_wrapper.h" \
        || die "gbm_mesa_wrapper.h at $HDR has no linear_required field — that header is the
  UNPATCHED one. Apply vendor/source-patches/external-minigbm-gbm-mesa/0004-*.patch to the
  Android tree first, or the wrapper and gralloc will disagree about struct alloc_args."
    SR="${REMORA_ANDROID_SYSROOT:-}"
    # Pick the API level from what the sysroot can actually LINK, not from what it can compile
    # against. An AOSP tree's out/soong/ndk/sysroot carries libc.so/liblog.so stubs for every
    # level but crtbegin_so.o/crtend_so.o for only a couple of them, so the obvious choice
    # (newest, or the NDK's usual 34) fails at link with "cannot open crtbegin_so.o" — the libs
    # are there and the startup files are not. Take the highest level that has the CRT.
    API="${REMORA_ANDROID_API:-}"
    if [ -z "$API" ] && [ -n "$SR" ]; then
        # The trailing `:` is load-bearing under `set -o pipefail`: the loop's exit status is
        # its LAST test, which is false whenever the highest-numbered directory is not a match,
        # and that would fail the whole pipeline and abort the script with API already correct.
        API=$({ for d in "$SR"/usr/lib/x86_64-linux-android/*/; do
                    [ -f "$d/crtbegin_so.o" ] && basename "$d"
                done; :; } | sort -n | tail -1)
    fi
    [ -n "$API" ] || die "no API level under $SR/usr/lib/x86_64-linux-android has crtbegin_so.o —
  that sysroot cannot link a shared object. Set REMORA_ANDROID_API to override, or point
  REMORA_ANDROID_SYSROOT at a real NDK sysroot."
    say "rebuilding libgbm_mesa_wrapper.so from patched source (API $API)"
    "$ACC" -O2 -fPIC -shared -Wall --target="x86_64-linux-android$API" \
        ${SR:+--sysroot="$SR"} -I"$HDR" \
        -o "$ROOT/guest-libs/libgbm_mesa_wrapper.so" \
        "$WD/src/minigbm-vtest/vtest_wrapper.c" \
        -llog -Wl,-soname,libgbm_mesa_wrapper.so \
        || die "guest wrapper build failed — see the compiler output above"
    say "guest wrapper rebuilt (prebuilt overwritten)"
fi

# --- 8. verify the install is what the probe looks for ---------------------
[ -x "$ROOT/bin/virgl_test_server" ] || die "install incomplete: bin/virgl_test_server"
for l in vulkan.virtio.so libgbm_mesa_wrapper.so libEGL_angle.so libGLESv2_angle.so \
         libGLESv1_CM_angle.so; do
    [ -f "$ROOT/guest-libs/$l" ] || die "install incomplete: guest-libs/$l"
done
say "guest libs installed -> $ROOT/guest-libs"
printf 'venus-build: OK — %s is complete\n' "$ROOT"
