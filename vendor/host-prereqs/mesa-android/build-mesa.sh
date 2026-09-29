#!/usr/bin/env bash
# Build Mesa for Android x86_64 from source and install it as the mesa_source build-feature
# payload (bd remora-ykhz R3, jointly with bd remora-bm7.4).
#
# WHY THIS EXISTS AT ALL, in one line: external/mesa3d is in the manifest but its Android.bp builds
# only gfxstream infrastructure — no libEGL_mesa, no vulkan.<driver>, no libgallium_dri, no libgbm,
# no libglapi. Nothing in that project produces a shipped driver, so bumping the manifest pin
# changes nothing that ships. Mesa reaches Android through its OWN meson with an NDK cross-file,
# which is how the prebuilt drivers the image used to inherit were produced. This is that build.
#
# STAGES, each skippable and each resumable (the expensive ones cache in $WORK):
#   1. native mesa-clc + precomp-compiler   — ANV needs CLC, and it must be a NATIVE build
#   2. LLVM 20.1.8 for Android, AMDGPU only — radeonsi only; skipped unless it is in the driver set
#   3. target libdrm .pc files from the AOSP tree
#   4. the meson cross build
#   5. install into the payload dir under the names Android loads
#
# Env:
#   REMORA_MESA_NDK      Android NDK root (r27c proven). Required.
#   REMORA_MESA_TREE     AOSP tree, for the TARGET libdrm. Required (else $REMORA_SOURCE_TREE).
#   REMORA_MESA_WORK     build/cache dir. Default ~/.cache/remora/mesa-android.
#   REMORA_MESA_OUT      payload dir. Default <repo>/vendor/source-build/features/mesa_source/lib64.
#   REMORA_MESA_SRC      Mesa checkout. Default $TREE/external/mesa3d — the AOSP tree's own copy,
#                        so the driver matches the Mesa the image is built against.
#   REMORA_MESA_PIN      Mesa ref for the clone FALLBACK only, and it must be a real tag/branch.
#   REMORA_MESA_GALLIUM  gallium drivers. Default "iris,radeonsi". Empty = skip the GL cluster.
#   REMORA_MESA_VULKAN   vulkan drivers. Default "intel,amd".
#   REMORA_MESA_API      Android API level. Default 34, matching the shipped prebuilts.
#   REMORA_MESA_RUST     Rust release for the NVK build (nouveau only). Default 1.96.1. The
#                        compiler + both stds are fetched from static.rust-lang.org into $WORK —
#                        a distro rustc cannot be used (E0514; see the stage-4 rust block).
#   REMORA_MESA_STAGES   comma list to run. Default all. e.g. REMORA_MESA_STAGES=4,5
#
# NOT YET RUN END TO END AS A SCRIPT. Every command here was executed by hand and its output
# verified (see README.md: eight artifacts, both Vulkan drivers ABI-complete against the
# prebuilts), but the assembly into one script has not itself been exercised. Treat the first run
# as part of the work, not as a regression.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${REMORA_MESA_WORK:-$HOME/.cache/remora/mesa-android}"
OUT="${REMORA_MESA_OUT:-$REPO/vendor/source-build/features/mesa_source/lib64}"
TREE="${REMORA_MESA_TREE:-${REMORA_SOURCE_TREE:?set REMORA_MESA_TREE to the AOSP tree (for its target libdrm)}}"
SRC="${REMORA_MESA_SRC:-$([ -d "$TREE/external/mesa3d" ] && echo "$TREE/external/mesa3d" || echo "$WORK/mesa")}"
PIN="${REMORA_MESA_PIN:-mesa-26.1.0-devel}"
GALLIUM="${REMORA_MESA_GALLIUM-iris,radeonsi}"
VULKAN="${REMORA_MESA_VULKAN-intel,amd}"
API="${REMORA_MESA_API:-34}"
STAGES="${REMORA_MESA_STAGES:-1,2,3,4,5}"
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
LLVM_URL=https://github.com/llvm/llvm-project.git
LLVM_TAG=llvmorg-20.1.8

# The Mesa source, which DEFAULTS TO THE AOSP TREE'S OWN external/mesa3d rather than a clone.
# That is a correctness argument, not a convenience one: the tree's copy is pinned by the manifest
# (android-17.0.0_r1, VERSION 26.1.0-devel) and is the same Mesa the image is built against, so a
# driver built from it cannot drift from the platform it will be loaded into.
#
# The clone path is the fallback and it now FAILS rather than degrading. `mesa-26.1.0-devel` is a
# VERSION STRING, not a git ref — no such tag or branch exists upstream — so `clone -b "$PIN"`
# always failed and the old `|| git clone "$MESA_URL"` caught it and took the default branch
# instead. The first end-to-end run of this script built 26.3.0-devel off main while reporting
# nothing unusual. An unresolvable pin is now an error, because a silently unpinned Mesa is exactly
# the outcome pinning exists to prevent.
have_src() {
    [ -d "$SRC" ] && return 0
    printf 'mesa-build: no Mesa source at %s\n' "$SRC" >&2
    printf '  expected the AOSP tree copy at %s/external/mesa3d\n' "$TREE" >&2
    printf '  or set REMORA_MESA_SRC to a checkout, or REMORA_MESA_PIN to a REAL git ref\n' >&2
    [ -n "${REMORA_MESA_PIN:-}" ] || exit 1
    printf '  cloning at ref %s\n' "$PIN" >&2
    git clone --depth 1 -b "$PIN" "$MESA_URL" "$SRC" \
        || die "could not clone Mesa at ref '$PIN' — is it a real tag or branch?"
}

say()  { printf '  %s\n' "$*"; }
head_() { printf '\n=== %s ===\n' "$*"; }
die()  { printf 'mesa-build: %s\n' "$*" >&2; exit 1; }
want() { case ",$STAGES," in *",$1,"*) return 0;; *) return 1;; esac; }
# Mesa refuses -Dgallium-va=enabled unless the driver set contains one that implements the VA state
# tracker (meson.build:740 names them and errors out otherwise). iris is NOT one of them, so an
# Intel-only gallium set cannot have VA no matter what libva is available — which is why this asks
# the driver set rather than the host.
va_capable() {
    case ",$GALLIUM," in
        *,radeonsi,*|*,r600,*|*,nouveau,*|*,virgl,*) return 0;;
        *) return 1;;
    esac
}
# shellcheck disable=SC2154  # rc is assigned inside the trap string itself
trap 'rc=$?; [ $rc -ne 0 ] && printf "mesa-build: aborted at line %s (exit %s)\n" "$LINENO" "$rc" >&2' EXIT

# --- 0. inputs -------------------------------------------------------------
NDK="${REMORA_MESA_NDK:-}"
[ -n "$NDK" ] || die "REMORA_MESA_NDK is unset — point it at an Android NDK (r27c proven)"
[ -d "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin" ] \
    || die "REMORA_MESA_NDK does not look like an NDK: $NDK"
NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

missing=
for t in git meson ninja cmake pkg-config python3; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
[ -z "$missing" ] || die "missing host tools:$missing"

mkdir -p "$WORK" "$OUT"

# --- 1. native mesa-clc ----------------------------------------------------
# ANV compiles internal OpenCL kernels at BUILD time, so the compiler must run on the host. It is a
# separate native build of the same tree, installed to its own prefix and put on PATH for stage 4.
# Gentoo (and anything that slots LLVM) hides the SPIRV translator off the default pkg-config path,
# which is why PKG_CONFIG_PATH is named rather than assumed.
CLC="$WORK/clc-install"
if want 1; then
    head_ "1. native mesa-clc"
    have_src
    if [ -x "$CLC/bin/mesa_clc" ] || [ -x "$CLC/bin/mesa-clc" ]; then
        say "already built: $CLC"
    else
        llvmpc=$(ls -d /usr/lib/llvm/*/lib64/pkgconfig /usr/lib/llvm-*/lib/pkgconfig 2>/dev/null | tail -1 || true)
        say "SPIRV/LLVM pkgconfig: ${llvmpc:-<default path>}"
        PKG_CONFIG_PATH="${llvmpc:-}${llvmpc:+:}${PKG_CONFIG_PATH:-}" \
        meson setup "$WORK/build-clc" "$SRC" \
            -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
            -Dinstall-mesa-clc=true -Dinstall-precomp-compiler=true \
            -Dmesa-clc=enabled -Dprecomp-compiler=enabled \
            --prefix "$CLC" --wipe 2>/dev/null \
        || PKG_CONFIG_PATH="${llvmpc:-}${llvmpc:+:}${PKG_CONFIG_PATH:-}" \
           meson setup "$WORK/build-clc" "$SRC" \
            -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
            -Dinstall-mesa-clc=true -Dinstall-precomp-compiler=true \
            -Dmesa-clc=enabled -Dprecomp-compiler=enabled --prefix "$CLC"
        ninja -C "$WORK/build-clc" install
    fi
fi

# --- 2. LLVM for Android (radeonsi only) -----------------------------------
# radeonsi compiles shaders through LLVM; RADV does not (it uses ACO), and iris does not. So this
# whole stage is skipped unless radeonsi is in the gallium set — it is the single most expensive
# thing here and the reason libgallium_dri is 90 MB against the prebuilt's 34 MB.
LLVMROOT="$WORK/llvm-android"
if want 2 && [ -n "$GALLIUM" ] && case ",$GALLIUM," in *,radeonsi,*) true;; *) false;; esac; then
    head_ "2. LLVM $LLVM_TAG for Android (AMDGPU only)"
    if [ -x "$LLVMROOT/bin/llvm-config" ]; then
        say "already built: $LLVMROOT"
    else
        [ -d "$WORK/llvm-project" ] || git clone --depth 1 -b "$LLVM_TAG" "$LLVM_URL" "$WORK/llvm-project"
        # A NATIVE tblgen OF THE SAME VERSION is required first: the host's newer llvm-tblgen
        # cannot drive an LLVM 20 build. This is not optional and not substitutable.
        #
        # LLVM_TARGETS_TO_BUILD MUST MATCH THE CROSS BUILD'S, and it is llvm-config that makes this
        # load-bearing rather than tblgen — tblgen is target-agnostic and would build with any set.
        # The native llvm-config is the one Mesa executes (see llvm-config-android.in), and its
        # COMPONENT DATABASE is baked in from this configuration. Built with X86, it answers
        # "amdgpu(missing)" for a cross tree that contains AMDGPU and nothing else, and meson then
        # rejects an LLVM that is actually present and correct:
        #   Run-time dependency LLVM (modules: amdgpu(missing), bitreader, ...) found: NO
        # llvm-config-android.in states this requirement outright; the script did not honour it.
        if [ ! -x "$WORK/llvm-native/bin/llvm-tblgen" ] || [ ! -x "$WORK/llvm-native/bin/llvm-config" ]; then
            say "building native llvm-tblgen + llvm-config (targets=AMDGPU, matching the cross build)"
            cmake -S "$WORK/llvm-project/llvm" -B "$WORK/llvm-native" -G Ninja \
                -DLLVM_TARGETS_TO_BUILD=AMDGPU -DCMAKE_BUILD_TYPE=Release
            ninja -C "$WORK/llvm-native" llvm-tblgen llvm-config
        fi
        cmake -S "$WORK/llvm-project/llvm" -B "$WORK/llvm-cross" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI=x86_64 -DANDROID_PLATFORM="android-$API" \
            -DLLVM_TARGETS_TO_BUILD=AMDGPU \
            -DLLVM_TABLEGEN="$WORK/llvm-native/bin/llvm-tblgen" \
            -DLLVM_HOST_TRIPLE=x86_64-linux-android -DLLVM_ENABLE_PIC=ON \
            -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF \
            -DLLVM_ENABLE_LIBXML2=OFF -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$LLVMROOT" \
            -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TOOLS=ON \
            -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
            -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_UTILS=OFF
        # LLVM_BUILD_TOOLS=OFF is NOT an optimisation, it is required for this build to link at all.
        # llvm/tools/bugpoint-passes builds lib/BugpointPasses.so, a TEST plugin that resolves its
        # LLVM symbols from the bugpoint executable at load time — so it links with deliberately
        # undefined symbols. The NDK toolchain adds -Wl,--no-undefined, which makes that fatal:
        #   FAILED: lib/BugpointPasses.so
        #   ld.lld: error: undefined symbol: llvm::Pass::~Pass()   (and ~100 more)
        # and it killed the build at 2538/2630, after every library Mesa actually needs was already
        # compiled. Its own CMakeLists gates on exactly this flag (NOT LLVM_BUILD_TOOLS ->
        # EXCLUDE_FROM_ALL). Nothing on the TARGET needs an LLVM tool: radeonsi links the libraries,
        # and llvm-config is the native one installed below precisely because it must be executed.
        ninja -C "$WORK/llvm-cross" install
        # llvm-config CANNOT be cross-built usefully, because Mesa has to EXECUTE it. A native
        # llvm-config placed inside the cross install tree reports that tree's paths (it derives
        # its prefix from its own location) while carrying LLVM's real component database — which
        # is what makes `--libs amdgpu` expand to the right 58 libraries in the right order.
        ninja -C "$WORK/llvm-native" llvm-config
        install -m755 "$WORK/llvm-native/bin/llvm-config" "$LLVMROOT/bin/llvm-config"
        say "native llvm-config installed into the cross tree (see README: it must be native)"
    fi
fi

# --- 3. target libdrm .pc --------------------------------------------------
# THE TARGET libdrm MUST COME FROM THE AOSP TREE. Left to the host's /usr/lib64/pkgconfig, meson
# hands a Linux x86_64 libdrm to an Android cross build without complaining, and the result links
# against the wrong ABI. Remora builds libdrm from source into the image (bd remora-ykhz), so the
# headers and the .so are both in the tree; these .pc files point at them.
PCDIR="$WORK/pkgconfig"
if want 3; then
    head_ "3. target libdrm pkg-config"
    [ -d "$TREE/external/libdrm" ] || die "no external/libdrm under $TREE (set REMORA_MESA_TREE)"
    drmlib="$TREE/out/target/product/remora_x86_64/vendor/lib64"
    [ -f "$drmlib/libdrm.so" ] || die "libdrm.so not built yet at $drmlib — build the image first"
    # READ the version out of the tree instead of stating one. This was hardcoded 2.4.120 while the
    # tree shipped 2.4.124, and Mesa 26.1 requires >=2.4.121 — so the generated .pc understated the
    # tree's own library and meson refused a dependency that was actually satisfied. A .pc file is a
    # description of what is there; hardcoding a version makes it a claim about what used to be.
    drmver="$(sed -nE "s/^\s*version\s*:\s*'([0-9.]+)'.*/\1/p" \
        "$TREE/external/libdrm/meson.build" | head -1)"
    [ -n "$drmver" ] || die "could not read the libdrm version from $TREE/external/libdrm/meson.build"
    say "libdrm version from the tree: $drmver"
    mkdir -p "$PCDIR"
    for m in "" _amdgpu _intel _nouveau _radeon; do
        [ -z "$m" ] || [ -f "$drmlib/libdrm$m.so" ] || continue
        cat > "$PCDIR/libdrm$m.pc" <<EOF
prefix=$TREE/external/libdrm
includedir=\${prefix}
libdir=$drmlib

Name: libdrm$m
Description: Direct Rendering Manager library (Android target, from $TREE)
Version: $drmver
Cflags: -I\${includedir} -I\${includedir}/include/drm $([ -n "$m" ] && echo "-I\${includedir}/${m#_}")
Libs: -L\${libdir} -ldrm$m
EOF
        say "wrote libdrm$m.pc"
    done

    # libva, and it is HEADERS ONLY — Mesa takes compile args and include paths from it
    # (meson.build:750 partial_dependency) and never links it, which is exactly why the shipped
    # drv_video carries no libva DT_NEEDED. So the VA frontend needs NO cross-built libva: headers
    # and a .pc are the whole dependency. This was recorded on bd remora-ykhz for a long time as
    # "needs a libva-drm AOSP does not define", which measured false on both halves — no libva-drm
    # is linked at runtime, and the real blocker was only ever a missing pkg-config file.
    #
    # TWO INCLUDE DIRS, because va_version.h and va_drm.h are soong-GENERATED and are NOT in the
    # source dir. Mesa reads VA_VERSION out of va_version.h (meson.build:755), so a .pc naming only
    # external/libva satisfies the dependency and then dies on a missing header.
    #
    # THE VERSION IS THE VA-API VERSION, not the libva release: upstream libva.pc.in publishes
    # @VA_API_VERSION@ (1.22.0) while the project itself is 2.22.0, and Mesa asks for >= 1.8.0.
    # Read from the generated header for the same reason the libdrm version is read from the tree
    # above — a hardcoded version is a claim about what used to be there.
    if va_capable; then
        vagen="$TREE/out/soong/.intermediates/external/libva/libva_gen_headers/gen"
        [ -f "$TREE/external/libva/va/va.h" ] \
            || die "no external/libva under $TREE, but GALLIUM=$GALLIUM wants the VA frontend"
        # Generated headers come from a soong build, so a never-built tree cannot configure VA.
        # die rather than skip: stage 4 asks for -Dgallium-va=enabled on this driver set, so a
        # missing .pc would surface as a meson dependency error naming libva and not the reason.
        [ -f "$vagen/va/va_version.h" ] \
            || die "libva's generated headers are missing at $vagen — they are produced by soong, so build the image tree once before configuring Mesa's VA frontend"
        vaver="$(sed -nE 's/^#define VA_VERSION_S[[:space:]]+"([0-9.]+)".*/\1/p' \
            "$vagen/va/va_version.h" | head -1)"
        [ -n "$vaver" ] || die "could not read VA_VERSION_S from $vagen/va/va_version.h"
        say "VA-API version from the tree: $vaver"
        cat > "$PCDIR/libva.pc" <<EOF
prefix=$TREE/external/libva
includedir=\${prefix}
gendir=$vagen
libdir=$drmlib

Name: libva
Description: Userspace Video Acceleration (VA) core interface (Android target, from $TREE)
Version: $vaver
Libs: -L\${libdir} -lva
Cflags: -I\${includedir} -I\${gendir}
EOF
        say "wrote libva.pc"
    else
        # Leave no stale .pc behind: a VA-less driver set must not inherit a previous run's libva.
        rm -f "$PCDIR/libva.pc"
        say "gallium set $GALLIUM has no VA-capable driver — no libva.pc"
    fi

    # libelf, and ONLY radeonsi needs it — Mesa hard-errors "Gallium driver radeonsi requires
    # libelf" rather than degrading, so a radeonsi build cannot configure without one.
    #
    # IT IS COMPILED HERE WITH THE NDK RATHER THAN TAKEN FROM THE TREE, and the reason is not ABI:
    # AOSP's out/.../libelf.a is LLVM BITCODE, not objects. AOSP builds it with its own clang under
    # LTO, so ld.lld from NDK r27c refuses it outright:
    #   ld.lld: error: libelf.a(crc32_file.o): Invalid attribute group entry
    #                  (Producer: 'LLVM22.0.1' Reader: 'LLVM 18.0.3')
    # A static archive can carry bitcode instead of code, and reasoning about C-vs-C++ ABI does not
    # reach that. Every libelf variant in the tree (obj, obj_x86, .vendor) is the same bitcode.
    # The SOURCE is fine though, so compile it with the same toolchain as the rest of this build.
    if case ",$GALLIUM," in *,radeonsi,*) true;; *) false;; esac; then
        ELFSRC="$TREE/external/elfutils"
        ELFW="$WORK/libelf-ndk"
        [ -f "$ELFSRC/libelf/libelf.h" ] || die "no external/elfutils in $TREE"
        if [ ! -f "$ELFW/libelf.a" ]; then
            say "building libelf with the NDK (AOSP's is LTO bitcode this linker cannot read)"
            mkdir -p "$ELFW/obj" "$ELFW/override"
            # elfutils' own config.h with zstd switched off. The NDK ships no libzstd and AOSP's is
            # another LTO archive; zlib IS in the NDK and covers the compression entry points Mesa
            # reaches. #undef in a wrapper header rather than -U on the command line, because the
            # real config.h #defines these unconditionally and would simply win.
            cat > "$ELFW/override/config.h" <<EOF
#include "$ELFSRC/config.h"
#undef USE_ZSTD
#undef USE_ZSTD_COMPRESS
EOF
            for f in "$ELFSRC"/libelf/*.c "$ELFSRC"/lib/*.c; do
                case "$(basename "$f")" in
                    # dynamicsizehash*.c are #include-d templates, not translation units. color.c
                    # and printversion.c are argp-based CLI helpers; argp is glibc-only and nothing
                    # in libelf's API path reaches them.
                    dynamicsizehash.c|dynamicsizehash_concurrent.c|color.c|printversion.c) continue;;
                esac
                "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android$API-clang" \
                    -c "$f" -o "$ELFW/obj/$(basename "$f" .c).o" -O2 -fPIC -std=gnu99 \
                    -DHAVE_CONFIG_H -D_GNU_SOURCE -DNMNES=1000 -D_FILE_OFFSET_BITS=64 \
                    -include AndroidFixup.h -w \
                    -I"$ELFW/override" -I"$ELFSRC" -I"$ELFSRC/include" -I"$ELFSRC/lib" \
                    -I"$ELFSRC/libelf" -I"$ELFSRC/bionic-fixup" \
                    || die "libelf: failed to compile $f"
            done
            "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar" rcs "$ELFW/libelf.a" "$ELFW"/obj/*.o
            say "libelf.a built: $(du -h "$ELFW/libelf.a" | cut -f1), $(ls "$ELFW"/obj/*.o | wc -l) objects"
        else
            say "libelf already built: $ELFW/libelf.a"
        fi
        cat > "$PCDIR/libelf.pc" <<EOF
includedir=$ELFSRC/libelf
libdir=$ELFW

Name: libelf
Description: ELF access library (Android target, built by build-mesa.sh with the NDK)
Version: 0.190
Cflags: -I\${includedir}
Libs: -L\${libdir} -lelf -lz
EOF
        say "wrote libelf.pc (radeonsi requires it)"
    fi
fi

# --- 4. the cross build ----------------------------------------------------
BUILD="$WORK/build-android"
if want 4; then
    head_ "4. meson cross build (gallium=${GALLIUM:-<none>} vulkan=${VULKAN:-<none>})"
    [ -n "$GALLIUM$VULKAN" ] || die "both driver sets are empty — nothing to build"
    # llvm-config, and it is REQUIRED for radeonsi: Mesa pins method:'config-tool' for its LLVM
    # dependency, so it EXECUTES llvm-config rather than reading a .pc. The cross-built one is an
    # Android binary that cannot run here, and the host's own describes the host's Linux LLVM — so
    # llvm-config-android.in wraps the NATIVE llvm-config installed into the cross prefix (which
    # reports that prefix's paths, because llvm-config derives them from its own location) and
    # filters the host-only libraries out of its output.
    #
    # The cross file used to hardcode /usr/bin/false and this template was never instantiated by the
    # script at all. That was invisible for as long as only Vulkan was built — RADV uses ACO and
    # needs no LLVM — and it is exactly why the first gallium run died at meson configure with
    # "llvm-config found: NO need ['>= 18.0.0']" after LLVM had built successfully.
    LLVMCFG=/usr/bin/false
    if [ -x "$LLVMROOT/bin/llvm-config" ]; then
        sed -e "s|@REAL@|$LLVMROOT/bin/llvm-config|g" \
            "$HERE/llvm-config-android.in" > "$WORK/llvm-config-android"
        chmod 755 "$WORK/llvm-config-android"
        say "llvm-config wrapper -> $WORK/llvm-config-android (real: $LLVMROOT/bin/llvm-config)"
        LLVMCFG="$WORK/llvm-config-android"
    elif case ",$GALLIUM," in *,radeonsi,*) true;; *) false;; esac; then
        die "radeonsi needs LLVM but $LLVMROOT/bin/llvm-config is missing — run stage 2"
    fi
    sed -e "s|@NDK@|$NDK|g" -e "s|@PCDIR@|$PCDIR|g" -e "s|@LLVMCONFIG@|$LLVMCFG|g" \
        "$HERE/android-x86_64.cross.in" > "$WORK/android-x86_64.cross"
    # --- Rust, and ONLY nouveau needs it: NVK's compiler (NAK) is Rust. The cross build needs
    # rustc, the Rust std for x86_64-linux-android, AND the host std (NAK's proc-macro crates —
    # pest_derive etc. — compile for the build machine and are loaded INTO rustc while it compiles
    # the target crates).
    #
    # THE DISTRO RUSTC CANNOT BE USED, and the reason is exact, not a preference: rustc refuses any
    # rlib whose producer version STRING differs from its own (E0514), and Gentoo's identifies as
    # "1.96.1 (31fca3adb ...) (gentoo)" while the official std tarballs say "1.96.1 (31fca3adb ...)"
    # — same commit, still "incompatible". Measured here: the overlay-only first attempt died at
    # meson's sanity check on exactly that. So the whole toolchain — compiler, host std, android
    # std — comes from static.rust-lang.org as one self-consistent set, assembled by each tarball's
    # own install.sh into a PRIVATE prefix under $WORK. Nothing system-wide changes and nothing
    # shadows the distro rust outside this build's PATH.
    #
    # Crate sources themselves arrive as meson wraps at setup time (subprojects/*-rs.wrap; only
    # expat is in packagecache, so the first configure needs the network).
    if case ",$VULKAN," in *,nouveau,*) true;; *) false;; esac; then
        RUSTVER="${REMORA_MESA_RUST:-1.96.1}"
        RUSTTC="$WORK/rust-toolchain-$RUSTVER"
        if [ ! -x "$RUSTTC/bin/rustc" ] || [ ! -d "$RUSTTC/lib/rustlib/x86_64-linux-android" ]; then
            say "assembling private Rust $RUSTVER toolchain -> $RUSTTC"
            for comp in "rustc-$RUSTVER-x86_64-unknown-linux-gnu" \
                        "rust-std-$RUSTVER-x86_64-unknown-linux-gnu" \
                        "rust-std-$RUSTVER-x86_64-linux-android"; do
                if [ ! -x "$WORK/$comp/install.sh" ]; then
                    say "fetching $comp (official dist tarball)"
                    ( cd "$WORK" \
                      && curl -sSfLO "https://static.rust-lang.org/dist/$comp.tar.xz" \
                      && curl -sSfLO "https://static.rust-lang.org/dist/$comp.tar.xz.sha256" \
                      && sha256sum -c "$comp.tar.xz.sha256" >/dev/null \
                      && tar xf "$comp.tar.xz" ) \
                    || die "could not fetch/verify $comp from static.rust-lang.org"
                fi
                "$WORK/$comp/install.sh" --prefix="$RUSTTC" --disable-ldconfig >/dev/null \
                    || die "$comp/install.sh failed"
            done
        fi
        # Into [binaries] — an append lands in whatever section ends the file, where meson
        # silently ignores it and then reports "compiler for language rust not found".
        sed -i "/^pkg-config = /a rust = ['$RUSTTC/bin/rustc', '--target=x86_64-linux-android', '-Clinker=$NDKBIN/x86_64-linux-android$API-clang']" \
            "$WORK/android-x86_64.cross"
        # The BUILD-machine rustc must be the same binary (proc-macro dylibs carry the same
        # version metadata), so the private toolchain fronts PATH for meson's native detection.
        export PATH="$RUSTTC/bin:$PATH"
        # bindgen runs libclang over Android headers; without these it parses them as HOST code.
        export BINDGEN_EXTRA_CLANG_ARGS="--target=x86_64-linux-android$API --sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
        # And it dlopens libclang at RUNTIME — the NDK ships none, and Gentoo slots the host's
        # under /usr/lib/llvm/<ver>/lib64, off bindgen's default search path. Same slotting trap
        # as stage 1's SPIRV pkgconfig, same treatment: name the path rather than assume it.
        if [ -z "${LIBCLANG_PATH:-}" ]; then
            libclang_dir="$(ls -d /usr/lib/llvm/*/lib64 /usr/lib/llvm-*/lib 2>/dev/null | tail -1 || true)"
            [ -n "$libclang_dir" ] && export LIBCLANG_PATH="$libclang_dir"
            say "LIBCLANG_PATH -> ${libclang_dir:-<unset, bindgen default search>}"
        fi
    fi
    # DO NOT set sys_root in the cross file. meson prefixes it onto every -I coming out of
    # pkg-config, so an absolute tree path becomes <ndk-sysroot>/mnt/Build/... and the build dies
    # on a missing xf86drm.h. The NDK's clang driver already knows its own sysroot. The .in file
    # says this too; repeated because it is the trap that costs the most time to diagnose.
    args=(
        --cross-file "$WORK/android-x86_64.cross"
        -Dplatforms=android -Dplatform-sdk-version="$API" -Dandroid-stub=true
        -Dbuildtype=release
        -Dmesa-clc=system -Dprecomp-compiler=system
        # EXPAT MUST BE ABSORBED, not linked. Mesa falls back to an expat SUBPROJECT here (the NDK
        # ships none), and as a shared library that produces a libgallium_dri.so which DT_NEEDs
        # libexpat.so — a library that exists at /system/lib64 but is NOT visible from the vendor
        # sphal namespace, so on device the EGL driver fails to load with:
        #   dlopen failed: library "libexpat.so" not found: needed by
        #   /vendor/lib64/dri/libgallium_dri.so in namespace sphal
        # and SurfaceFlinger boot-loops. The 24.0.8 prebuilt has no expat dependency at all, which
        # is the shape to match. Static also keeps the payload to the files mesa.mk knows about
        # rather than adding a driver-private library with its own install rule.
        -Dexpat:default_library=static
    )
    # egl and gbm are MEMBERS OF THE GL CLUSTER, so they follow the gallium set rather than being
    # always-on. They were hardcoded enabled, which is right for a full build and fatal for a
    # Vulkan-only one: with no gallium driver, src/egl/meson.build dies on an undefined
    # glapi_xml_py_deps. The whole point of the Vulkan-first slice is that an ICD is standalone —
    # so the build that produces it must not drag in the cluster it is meant to avoid.
    #
    # BOTH ARMS SET EVERY OPTION, for the same reason the -Dgallium-drivers comment below gives:
    # `meson setup --wipe` wipes the build DIRECTORY but PRESERVES the previous configure's
    # options. So a Vulkan-only run leaves -Dopengl=false -Dgles1=disabled -Dgles2=disabled behind,
    # and a later gallium run that only says -Dgbm=enabled -Degl=enabled inherits them silently:
    # libgallium_dri.so and libEGL_mesa.so build, libGLESv1_CM/libGLESv2/libglapi do not, and the
    # payload lands 2-of-7 complete. mesa.mk's completeness gate catches that, but only after a
    # full build — an option omitted is an option INHERITED, never an option defaulted.
    if [ -n "$GALLIUM" ]; then
        # THE BACKEND PATH IS PART OF THE ABI (bd remora-ykhz.1): Mesa's libgbm loads dri_gbm.so
        # at RUNTIME from GBM_BACKENDS_PATH or this compiled-in default — there is no bare-name
        # fallback (loader_open_driver_lib always joins path/name), env vars do not reach an
        # Android HAL, and the meson default is /usr/local/lib/gbm, a path no Android linker
        # namespace permits. Left unset, gbm_create_device fails on device for EVERY consumer:
        # the gbm_mesa gralloc wrapper AND gralloc.gbm, the image's default fallback — which is
        # exactly how the gbm fallback shipped silently dead on the first Mesa-26 images.
        args+=(-Dgbm=enabled -Degl=enabled -Dopengl=true -Dgles1=enabled -Dgles2=enabled
               -Dgbm-backends-path=/vendor/lib64/gbm)
    else
        args+=(-Dgbm=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled)
    fi
    # THE VA FRONTEND, set explicitly BOTH WAYS like the pair above rather than left at meson's
    # `auto`. Auto is what hid this for so long: with no libva.pc on the cross pkg-config path the
    # dependency silently failed, with_gallium_va came out false, and the build produced no VA
    # driver at all while reporting success. Enabled, a missing libva is a configure error that
    # names it. Gated on the DRIVER SET because meson.build:740 refuses the frontend without one of
    # its supported drivers, so an iris-only build must ask for disabled rather than enabled.
    if va_capable; then
        # -Dvideo-codecs IS NOT OPTIONAL HERE, and leaving it at meson's default silently produced a
        # VA driver that was useless for the thing the AMD hosts use VA for. meson.options:718
        # defaults it to ['all_free'], which EXCLUDES the patent-encumbered codecs — so the first
        # source-built driver advertised only MPEG2, JPEG and VideoProc, while the 24.0.8 prebuilt it
        # replaced advertised H264 (dec+enc), HEVC Main/Main10 (dec+enc) and VC1. Measured by running
        # vainfo against both on real Polaris hardware (bd remora-ykhz), not inferred: `gallium-va
        # enabled` and `the codecs anyone wants VA for` are different switches, and only the first one
        # shows up in the configure summary.
        #
        # 'all' RESTORES PARITY RATHER THAN ADDING EXPOSURE: the prebuilt this replaces already
        # shipped these codecs, so the image's codec surface is unchanged by building them ourselves.
        args+=(-Dgallium-va=enabled -Dvideo-codecs=all)
    else
        args+=(-Dgallium-va=disabled)
    fi
    # ALWAYS passed, including empty. Omitting the flag does not mean "no drivers", it means meson's
    # own default of `auto` — which turns an empty GALLIUM into EVERY gallium driver, and the build
    # then dies on i915/r300 demanding LLVM. Empty must reach meson as empty.
    args+=("-Dgallium-drivers=$GALLIUM" "-Dvulkan-drivers=$VULKAN")
    if [ -x "$LLVMROOT/bin/llvm-config" ]; then
        # RTTI is OFF in that LLVM build, so Mesa must be told, or it errors out on the mismatch.
        #
        # shared-llvm=disabled is REQUIRED, not a preference. Mesa passes `static : not
        # _shared_llvm` straight into dependency('llvm'), and meson's config-tool handler picks its
        # llvm-config arguments from THAT kwarg — not from what `llvm-config --shared-mode` says.
        # Left at the default, meson asks for `--libs --ldflags --link-shared`, llvm-config answers
        # "libLLVM-20.so is missing" for a cross tree holding 102 static archives and no dylib, and
        # meson still reports "Run-time dependency LLVM ... found: YES" — with an EMPTY library
        # list. The link line then carries -L<llvmdir> and not one -lLLVM, and the whole thing
        # surfaces 1749 targets later as undefined LLVMInitializeAMDGPU* at the final link of
        # libgallium_dri.so. A dependency that reports found:YES and contributes nothing is the
        # worst shape this can fail in, so pin it rather than rely on detection.
        args+=(-Dllvm=enabled -Dcpp_rtti=false -Dshared-llvm=disabled)
        export PATH="$LLVMROOT/bin:$PATH"
    else
        args+=(-Dllvm=disabled)
    fi
    # LIBCLANG_PATH, and ONLY nouveau needs it: NVK generates its Rust bindings with the HOST
    # bindgen, which dlopens libclang at RUNTIME rather than linking it. A distro bindgen with no
    # libclang on the default loader path panics mid-build, ~235 targets in, with
    #   thread 'main' panicked at bindgen/lib.rs: Unable to find libclang: "couldn't find any valid
    #   shared libraries matching: ['libclang.so', ...], set the `LIBCLANG_PATH` environment
    #   variable"
    # and ninja reports it only as "exit 101", which names Rust rather than the missing library.
    # Resolved here rather than left to the caller's environment, because the requirement is a
    # property of the driver set, not of the machine.
    if [ -z "${LIBCLANG_PATH:-}" ] && case ",$VULKAN," in *,nouveau,*) true;; *) false;; esac; then
        LIBCLANG_PATH="$(ls -d /usr/lib/llvm/*/lib64 2>/dev/null | tail -1)"
        [ -n "$LIBCLANG_PATH" ] && [ -e "$LIBCLANG_PATH/libclang.so" ] \
            || die "nouveau (NVK) needs libclang for bindgen and none was found — set LIBCLANG_PATH"
        export LIBCLANG_PATH
        say "LIBCLANG_PATH -> $LIBCLANG_PATH (bindgen dlopens it for NVK's bindings)"
    fi
    # A HARD rm, not `meson setup --wipe`. The old `--wipe 2>/dev/null || setup` pair was
    # non-deterministic in a way that was actually observed: after three runs with different driver
    # sets the dir held a UNION configure (gallium [iris,radeonsi] AND vulkan [intel,amd,nouveau,
    # intel_hasvk]) that no single invocation had asked for, and ninja raced meson's regeneration
    # mid-build — compiles failing on output dirs that did not exist yet and on generated headers
    # whose producers had not run. A driver set is this script's whole input; the configure must be
    # exactly it, every time, at the price of a from-scratch compile.
    rm -rf "$BUILD"
    PATH="$CLC/bin:$PATH" PKG_CONFIG_LIBDIR="$PCDIR" \
        meson setup "$BUILD" "$SRC" "${args[@]}"
    PATH="$CLC/bin:$PATH" ninja -C "$BUILD"
fi

# --- 5. install into the payload -------------------------------------------
# Mesa's own output names are not the names Android loads. The mapping is the whole point of this
# stage, and getting it wrong produces a payload that installs cleanly and is never dlopened.
if want 5; then
    head_ "5. install payload -> $OUT"
    [ -d "$BUILD" ] || die "no build dir at $BUILD — run stage 4 first"
    mkdir -p "$OUT/dri" "$OUT/egl" "$OUT/hw"
    strip="$NDKBIN/llvm-strip"

    put() {  # <built path glob> <dest>
        src=$(ls -1 $1 2>/dev/null | head -1 || true)
        if [ -z "$src" ]; then say "MISSING (skipped): $1"; return 1; fi
        install -m644 "$src" "$2"
        "$strip" --strip-unneeded "$2" 2>/dev/null || true
        say "$(basename "$2")  $(du -h "$2" | cut -f1)"
    }

    put "$BUILD/src/gallium/targets/dri/libgallium*.so"        "$OUT/dri/libgallium_dri.so" || true
    # THERE IS NO SEPARATE VA DRIVER TO INSTALL, and this line used to pretend otherwise. It read
    #   put "$BUILD/src/gallium/targets/va/libgallium_drv_video.so" ... || true
    # against a path that CANNOT EXIST in a build like ours: src/gallium/meson.build:221-234 only
    # descends into targets/va `if not with_dri`, and we build the DRI megadriver. So the glob never
    # matched, `put` said MISSING, and `|| true` swallowed a structurally impossible path rather
    # than a flaky one — which is why enabling VA alone would still have shipped nothing.
    #
    # Mesa 26 serves VA FROM THE MEGADRIVER: targets/dri/meson.build emits one
    # <driver>_drv_video.so per VA-capable driver as a SYMLINK to libgallium_dri.so, the file
    # already installed on the line above. The payload therefore needs no new LIBRARY, only the
    # ALIASES — made at the end of this stage beside the _dri.so set, which Android.mk then derives
    # the module's LOCAL_MODULE_SYMLINKS from.
    #
    # WHAT IS CHECKED HERE is the thing that can silently go missing: whether the megadriver we just
    # installed actually carries the VA entry point. Without this a VA-less libgallium_dri installs
    # cleanly, the aliases point at it, and libva fails to load a driver on device — far from the
    # cause. The symbol is __vaDriverInit_<major>_<minor> (the shipped 24.0.8 prebuilt exports
    # __vaDriverInit_1_20), so match the prefix and stay version-agnostic.
    if va_capable; then
        if "$NDKBIN/llvm-nm" --dynamic --defined-only "$OUT/dri/libgallium_dri.so" 2>/dev/null \
             | grep -q '__vaDriverInit'; then
            say "libgallium_dri.so carries the VA frontend (__vaDriverInit)"
        else
            die "libgallium_dri.so has NO __vaDriverInit despite GALLIUM=$GALLIUM being VA-capable — the VA frontend did not build, so libva would find no driver on device"
        fi
    fi
    put "$BUILD/src/egl/libEGL.so*"                            "$OUT/egl/libEGL_mesa.so" || true
    # PATHS AND NAMES ARE MESA-VERSION-SPECIFIC, and these were written for 24.0.8:
    #  - src/mapi/es1api -> src/mesa/glapi/es1api. Mesa moved mapi under mesa/glapi after 24.0.8.
    #  - libgbm.so -> libgbm_mesa.so. With "External libgbm: NO" Mesa builds its own and names it
    #    libgbm_mesa to avoid colliding with a system gbm.
    # A stale glob here does not fail the build — `put` skips and says MISSING, and the payload
    # lands short, which only mesa.mk's completeness gate catches later.
    put "$BUILD/src/mesa/glapi/es1api/libGLESv1_CM.so*"        "$OUT/egl/libGLESv1_CM_mesa.so" || true
    put "$BUILD/src/mesa/glapi/es2api/libGLESv2.so*"           "$OUT/egl/libGLESv2_mesa.so" || true
    put "$BUILD/src/gbm/libgbm_mesa.so*"                       "$OUT/libgbm.so.1" || true
    # THE GBM BACKEND MOVES WITH libgbm OR NEITHER WORKS: libgbm runtime-loads dri_gbm.so from
    # the compiled-in /vendor/lib64/gbm (see the -Dgbm-backends-path comment in stage 4). Its
    # DT_NEEDED libgallium_dri.so resolves via the $ORIGIN/../dri runpath set in the fixup below
    # — the same treatment the EGL libraries get, applied AFTER put's strip (strip after patchelf
    # corrupts the .dynamic section and bionic refuses the file outright; measured, not theory).
    mkdir -p "$OUT/gbm"
    put "$BUILD/src/gbm/backends/dri/dri_gbm.so"               "$OUT/gbm/dri_gbm.so" || true

    # THE LOCAL-GBM WRAPPER (bd remora-ykhz.1): gralloc.minigbm_gbm_mesa dlopens
    # libgbm_mesa_wrapper.so, and until now the only provider was the venus tarball's VTEST
    # flavor, bind-mounted at deploy on the NVIDIA path — every other host had a gralloc that
    # dlopen-failed its allocator. This builds the LOCAL flavor (allocates through the payload's
    # own libgbm on the fd minigbm hands it) into the payload, so it is BAKED as the image
    # default; the venus deploy's bind-mount shadows it exactly where vtest must win.
    #   - linked against the payload libgbm, whose SONAME (libgbm_mesa.so) differs from its
    #     installed name — patchelf rewrites the NEEDED to libgbm.so.1, the name the image ships;
    #   - static libc++ like every artifact here, or it joins the libc++_shared problem;
    #   - strip FIRST, patchelf last, same reason as above.
    if [ -f "$OUT/libgbm.so.1" ]; then
        say "building libgbm_mesa_wrapper.so (local-gbm flavor)"
        "$NDKBIN/x86_64-linux-android$API-clang++" -shared -O2 -fvisibility=hidden \
            -o "$OUT/libgbm_mesa_wrapper.so" "$HERE/gbm_mesa_wrapper_local.cpp" \
            -I"$SRC/src/gbm/main" \
            -I"$TREE/external/minigbm/gbm_mesa_driver" \
            -I"$TREE/external/libdrm/include/drm" \
            -I"$TREE/system/logging/liblog/include" \
            -L"$OUT" -l:libgbm.so.1 -llog -static-libstdc++ \
            -Wl,-soname,libgbm_mesa_wrapper.so \
            || die "wrapper build failed — the payload would strand every non-venus gbm_mesa host"
        "$strip" --strip-unneeded "$OUT/libgbm_mesa_wrapper.so" 2>/dev/null || true
        patchelf --replace-needed libgbm_mesa.so libgbm.so.1 "$OUT/libgbm_mesa_wrapper.so"
        say "libgbm_mesa_wrapper.so  $(du -h "$OUT/libgbm_mesa_wrapper.so" | cut -f1)"
    fi
    # libglapi IS NOT BUILT BY MESA 26.x AT ALL — shared-glapi is gone. The 24.0.8 prebuilt has a
    # libglapi.so.0 and this build has no counterpart, so it is left to `put`'s MISSING path
    # deliberately rather than hunted for. See the bead: MESA_SRC_GL_SET still lists it.
    put "$BUILD/src/mapi/shared-glapi/libglapi.so*"            "$OUT/libglapi.so.0" || true
    for d in ${VULKAN//,/ }; do
        case "$d" in amd) icd=radeon;; intel) icd=intel;; *) icd="$d";; esac
        # vulkan* not vulkan: hasvk builds under src/intel/vulkan_hasvk/, which a bare
        # src/*/vulkan/ glob misses — and `put` skips a missing glob with a say() rather than
        # failing, so the payload would just land without the driver.
        put "$BUILD/src/*/vulkan*/libvulkan_$icd.so" "$OUT/hw/vulkan.$icd.so" || true
    done

    # THE DRI LOADER SYMLINKS. Every gallium driver compiled into the megalib must be reachable as
    # <driver>_dri.so, because that is the name the loader dlopens — the megalib itself is never
    # opened directly. Android.mk derives the module's symlink list from exactly these, so a
    # missing link here is a driver that silently does not load.
    if [ -f "$OUT/dri/libgallium_dri.so" ]; then
        for d in ${GALLIUM//,/ }; do
            case "$d" in
                iris)     for n in iris i965; do ln -sfn libgallium_dri.so "$OUT/dri/${n}_dri.so"; done;;
                radeonsi) for n in radeonsi r600 kms_swrast swrast; do ln -sfn libgallium_dri.so "$OUT/dri/${n}_dri.so"; done;;
                *)        ln -sfn libgallium_dri.so "$OUT/dri/${d}_dri.so";;
            esac
        done
        say "dri links: $(cd "$OUT/dri" && ls -1 *_dri.so 2>/dev/null | tr '\n' ' ')"
    fi

    # THE VA DRIVER ALIASES, and in Mesa 26 they are the WHOLE of what "install the VA driver"
    # means — see the stage-5 note above: there is no standalone drv_video library any more, so
    # libva has to find the MEGADRIVER under the name it looks up (ro.boot.va_driver, e.g.
    # radeonsi -> radeonsi_drv_video.so). Mesa's own targets/dri/meson.build emits exactly this set.
    #
    # THE NAMES ARE MESA'S, NOT OURS, and two of them are not the driver's own name: virgl's alias
    # is virtio_gpu, and iris has NO VA alias at all — Intel video is served by iHD
    # (external/intel-media-driver), never by Mesa. Do not "fix" that by adding iris here.
    #
    # ONLY FOR DRIVERS ACTUALLY COMPILED IN. An alias pointing at a megadriver that lacks that
    # driver is a lie libva cannot detect: the dlopen SUCCEEDS and the driver then has no backend,
    # which fails further from the cause than the name simply not existing. This is why the prebuilt
    # shipped nouveau/r600/virtio_gpu aliases and a source payload will normally ship only radeonsi.
    #
    # Delete by TYPE, not by glob: `*_drv_video.so` also matches a standalone libgallium_drv_video.so
    # from an older Mesa, which is a real file and not ours to remove.
    find "$OUT/dri" -name '*_drv_video.so' -type l -delete 2>/dev/null || true
    if [ -f "$OUT/dri/libgallium_dri.so" ] && va_capable; then
        for d in ${GALLIUM//,/ }; do
            case "$d" in
                radeonsi) ln -sfn libgallium_dri.so "$OUT/dri/radeonsi_drv_video.so";;
                r600)     ln -sfn libgallium_dri.so "$OUT/dri/r600_drv_video.so";;
                nouveau)  ln -sfn libgallium_dri.so "$OUT/dri/nouveau_drv_video.so";;
                virgl)    ln -sfn libgallium_dri.so "$OUT/dri/virtio_gpu_drv_video.so";;
            esac
        done
        say "va links: $(cd "$OUT/dri" && ls -1 *_drv_video.so 2>/dev/null | tr '\n' ' ')"
    fi

    # WILL IT ACTUALLY dlopen? Everything above proves the driver BUILT. None of it proves the
    # image can load it, and that distinction cost a full build, a 2.6 GB transfer and a deploy:
    # a driver that existed, reported Mesa 26.1 and exported HMI still failed on device with
    #   cannot locate symbol "_ZNSt6__ndk122__libcpp_verbose_abortEPKcz"
    # because r27c's libc++ is newer than the libc++_shared.so the image ships. The cross file now
    # links libc++ statically so the dependency is gone; this checks that it really is gone,
    # HERE, where the answer is free — rather than after an hour of build and deploy.
    # --- RUNPATH FIXUP -----------------------------------------------------------------------
    # meson bakes the BUILD TREE's rpath into every artifact and expects `meson install` to rewrite
    # it. `put` above copies straight out of the build directory instead, so it survives, and every
    # artifact ships with entries like
    #   $ORIGIN/../../../gallium/targets/dri : <tree>/out/.../vendor/lib64
    # — paths that do not exist on a device, plus this build HOST's absolute layout baked into a
    # driver that ships to users.
    #
    # MOSTLY THAT IS HARMLESS AND ONCE IT IS NOT. The Vulkan ICDs carry it too and work fine,
    # because everything they DT_NEED resolves from the default vendor path and the dead rpath
    # entries are simply skipped. The GL cluster is different: Mesa 26's libEGL_mesa and the two
    # libGLES*_mesa hard-DT_NEED libgallium_dri.so — 24.0.8 dlopened it instead — and the megalib
    # installs to /vendor/lib64/dri, which is NOT on the default search path. With the build-tree
    # rpath the loader cannot find it, dlopen of the EGL driver fails, and SurfaceFlinger aborts
    # the whole boot with:
    #   'couldn't find an OpenGL ES implementation, make sure one of persist.graphics.egl,
    #    ro.hardware.egl and ro.board.platform is set'
    # which names a property problem for what is actually a missing library. Measured on
    # remora-xe-test: boot loop, SF restarting, no tombstone.
    #
    # So: strip it everywhere, and give the three EGL/GLES libraries the one relative path that is
    # true after install — $ORIGIN/../dri, i.e. /vendor/lib64/egl/.. /dri.
    head_ "runpath fixup"
    command -v patchelf >/dev/null 2>&1 \
        || die "patchelf not found, and it is required: without it every artifact keeps the build
tree's rpath and the GL cluster cannot locate libgallium_dri.so on device"
    for so in "$OUT"/dri/*.so "$OUT"/hw/*.so "$OUT"/gbm/*.so "$OUT"/*.so*; do
        [ -f "$so" ] || continue
        patchelf --remove-rpath "$so"
    done
    # gbm/dri_gbm.so needs the same reach into ../dri the EGL libraries do: it DT_NEEDs
    # libgallium_dri.so, which installs off the default search path.
    for so in "$OUT"/egl/*.so "$OUT"/gbm/*.so; do
        [ -f "$so" ] || continue
        patchelf --set-rpath '$ORIGIN/../dri' "$so"
        say "$(basename "$so") runpath -> \$ORIGIN/../dri"
    done

    head_ "loadability"
    for so in "$OUT"/hw/*.so "$OUT"/dri/libgallium_dri.so "$OUT"/egl/*.so "$OUT"/gbm/*.so \
              "$OUT"/libgbm_mesa_wrapper.so; do
        [ -f "$so" ] || continue
        # Undefined libc++ symbols are the specific hazard: the image's libc++_shared.so is a
        # PREBUILT we do not control and cannot assume is current.
        # @LIBC-versioned symbols are BIONIC's, satisfied by libc.so, and must not be reported here.
        # __cxa_atexit and __cxa_finalize are the ones that matter: they look like C++ runtime
        # symbols and are not, so a naive /^__cxa/ match reports a clean driver as broken. A check
        # that cries wolf is worse than none — it gets skimmed exactly when it is right.
        undef_cxx="$("$NDKBIN/llvm-readelf" --dyn-syms -W "$so" 2>/dev/null \
            | awk '$7=="UND" && $8 !~ /@LIBC/ && $8 ~ /^_ZNSt|^_ZSt|^__cxa|libcpp|^_ZTI|^_ZTV/ {print $8}' \
            | head -5)"
        if [ -n "$undef_cxx" ]; then
            say "WARN $(basename "$so") still needs libc++ symbols from outside:"
            printf '        %s\n' $undef_cxx
            say "      the image's libc++_shared.so must export these or the driver will not dlopen"
        else
            say "ok   $(basename "$so") — no external libc++ dependency"
        fi
        needed="$("$NDKBIN/llvm-readelf" -d -W "$so" 2>/dev/null \
            | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | tr '\n' ' ')"
        say "      NEEDED: ${needed:-<none>}"
    done

    head_ "payload summary"
    ( cd "$OUT" && find . -type f -o -type l | sort | sed 's/^/  /' )
    cat <<'EOF'

Next: enable the mesa_source build feature and rebuild the image.
NOTHING HERE HAS BEEN VALIDATED ON A GPU. All fifteen Mesa prebuilts read ZERO processes on the
NVIDIA/Venus host that can build this; on the AMD hosts they are the whole GPU stack. Build here,
validate on amd-host-a or amd-host-b, and keep the current image tag as the rollback.
EOF
fi
