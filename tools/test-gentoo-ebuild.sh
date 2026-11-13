#!/usr/bin/env bash
# Does packaging/remora.ebuild.in render to the ebuild a release would carry?
#
#   tools/test-gentoo-ebuild.sh [<version>]        version defaults to the one in VERSION
#
# Offline: no network, no Portage, no tarball. It renders the real template for <version> (and for a
# made-up later one) into a scratch directory and checks the result: the file name
# remora-<version>.ebuild, bash syntax, no placeholder left, EAPI 8, the release's tarball asset as
# SRC_URI and its archive prefix as S, KEYWORDS ~amd64, and a Gentoo equivalent for every package
# the PKGBUILD names. The ebuild is sourced in a subshell with Portage's variables set and its
# eclass calls stubbed, so what is checked is what Portage would read, not the text that spells it.
#
# Rendering is a copy to the right name, once the version is a CalVer one and the template carries
# no @-delimited token: Portage takes ${PV} from the file name.
#
# bash -n and these checks are syntax and content evidence only. Whether Portage builds the package
# is proven by building it from an overlay on a Gentoo host.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TEMPLATE="$ROOT/packaging/remora.ebuild.in"
PKGBUILD="$ROOT/packaging/PKGBUILD"
RELEASE_YML="$ROOT/.github/workflows/release.yml"
VERSION="${1:-$(tr -d '[:space:]' < "$ROOT/VERSION")}"

failures=0
note() { printf '%s\n' "test-gentoo-ebuild: $*"; }
pass() { note "ok   $*"; }
fail() { note "FAIL $*" >&2; failures=$((failures + 1)); }
expect() {  # expect <what> <got> <want>
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: got '$2', want '$3'"; fi
}

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

# render <template> <version> <outdir>: writes <outdir>/remora-<version>.ebuild and prints its path.
render() {
    local template="$1" version="$2" outdir="$3"
    if ! [[ "$version" =~ ^[0-9]{4}\.[0-9]{2}\.[0-9]+$ ]]; then
        note "refusing version '$version': not YYYY.MM.PATCH" >&2
        return 1
    fi
    if grep -nE '@[A-Za-z_][A-Za-z0-9_]*@' "$template" >&2; then
        note "refusing $template: unresolved placeholder" >&2
        return 1
    fi
    cp "$template" "$outdir/remora-$version.ebuild"
    printf '%s\n' "$outdir/remora-$version.ebuild"
}

# ebuild_eval <ebuild> <version> <snippet>: runs <snippet> with the ebuild sourced as Portage would
# see it for that version. inherit and the eclass functions pkg_postinst calls are stubs.
ebuild_eval() {
    bash -c '
        set -eu
        PN=remora PV="$2" PR=r0
        P="$PN-$PV" PF="$PN-$PV" PVR="$PV"
        WORKDIR=/portage-workdir
        INHERITED=""
        inherit() { INHERITED="$*"; }
        xdg_pkg_postinst() { :; }
        optfeature() { shift; printf "%s\n" "$@"; }
        # shellcheck disable=SC1090
        source "$1"
        eval "$3"
    ' ebuild_eval "$1" "$2" "$3"
}

# words <text>: one whitespace-separated word per line.
words() { printf '%s\n' "$1" | tr -s '[:space:]' '\n' | sed '/^$/d'; }
has_word() { words "$1" | grep -qxF -- "$2"; }

# pkgbuild_eval <snippet> [<arg>]: the PKGBUILD's own arrays, read the way makepkg reads them.
# <arg> reaches the snippet as $3.
pkgbuild_eval() {
    bash -c 'set -eu; source "$1"; eval "$2"' pkgbuild_eval "$PKGBUILD" "$1" "${2:-}"
}

# The Gentoo side of each PKGBUILD package: "<variables> <atom>", "eclass <name>" (pulled in by an
# inherited eclass), "system" (the toolchain every Gentoo system has) or "optfeature <atom>".
gentoo_for() {
    case "$1" in
        gcc-libs|glibc)      echo "system" ;;
        qt6-base)            echo "DEPEND,RDEPEND dev-qt/qtbase:6[dbus,gui,network,opengl,widgets]" ;;
        qt6-multimedia)      echo "DEPEND,RDEPEND dev-qt/qtmultimedia:6" ;;
        qt6-webengine)       echo "DEPEND,RDEPEND dev-qt/qtwebengine:6[widgets]" ;;
        ffmpeg)              echo "DEPEND,RDEPEND media-video/ffmpeg:=" ;;
        libva)               echo "DEPEND,RDEPEND media-libs/libva:=" ;;
        android-tools)       echo "RDEPEND dev-util/android-tools" ;;
        cmake|ninja)         echo "eclass cmake" ;;
        ffnvcodec-headers)   echo "DEPEND media-libs/nv-codec-headers" ;;
        vulkan-headers)      echo "DEPEND dev-util/vulkan-headers" ;;
        git)                 echo "BDEPEND dev-vcs/git" ;;
        docker)              echo "optfeature app-containers/docker" ;;
        openssh)             echo "optfeature virtual/openssh" ;;
        nvidia-utils)        echo "optfeature x11-drivers/nvidia-drivers" ;;
        python)              echo "optfeature dev-lang/python" ;;
        unzip)               echo "optfeature app-arch/unzip" ;;
        polkit)              echo "optfeature sys-auth/polkit" ;;
        *)                   return 1 ;;
    esac
}

check_ebuild() {  # check_ebuild <version>
    local version="$1" out ebuild
    out="$SCRATCH/$version"
    mkdir -p "$out"
    note "rendering $version"
    if ! ebuild=$(render "$TEMPLATE" "$version" "$out"); then
        fail "$version: the template did not render"
        return
    fi

    expect "$version: file name" "$(basename "$ebuild")" "remora-$version.ebuild"
    expect "$version: only the ebuild is written" "$(ls "$out")" "remora-$version.ebuild"
    if bash -n "$ebuild"; then pass "$version: bash -n"; else fail "$version: bash -n"; fi
    if grep -qE '@[A-Za-z_][A-Za-z0-9_]*@' "$ebuild"; then
        fail "$version: a placeholder is left"
    else
        pass "$version: no placeholder left"
    fi
    expect "$version: EAPI=8 is the first statement" \
        "$(grep -vE '^[[:space:]]*(#|$)' "$ebuild" | head -n1)" "EAPI=8"

    local asset tarball_url
    asset="https://github.com/EtherAura/Remora/releases/download/v$version/Remora-$version.tar.gz"
    # The PKGBUILD's source= with its pkgver swapped for this version: the same asset, or one of
    # the two packages names the wrong tarball.
    tarball_url=$(pkgbuild_eval 'printf "%s\n" "${source[0]//$pkgver/@V@}"')
    tarball_url=${tarball_url//@V@/$version}
    expect "$version: PKGBUILD source= is the tarball asset" "$tarball_url" "$asset"
    expect "$version: SRC_URI" "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$SRC_URI"')" "$asset"
    expect "$version: S" "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$S"')" \
        "/portage-workdir/Remora-$version"
    expect "$version: KEYWORDS" "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$KEYWORDS"')" "~amd64"
    expect "$version: SLOT" "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$SLOT"')" "0"
    expect "$version: PKGBUILD license" "$(pkgbuild_eval 'printf "%s" "${license[*]}"')" "GPL-3.0-only"
    expect "$version: LICENSE" "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$LICENSE"')" "GPL-3"
    expect "$version: DESCRIPTION is the PKGBUILD's pkgdesc" \
        "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$DESCRIPTION"')" \
        "$(pkgbuild_eval 'printf "%s" "$pkgdesc"')"
    expect "$version: HOMEPAGE is the PKGBUILD's url" \
        "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$HOMEPAGE"')" \
        "$(pkgbuild_eval 'printf "%s" "$url"')"
    expect "$version: eclasses" "$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$INHERITED"')" \
        "cmake optfeature xdg"
    expect "$version: arch is x86_64 only" "$(pkgbuild_eval 'printf "%s" "${arch[*]}"')" "x86_64"

    local -A deps
    local BDEPEND INHERITED OPTFEATURES BDEPEND_FLAT
    deps[DEPEND]=$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$DEPEND"')
    deps[RDEPEND]=$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$RDEPEND"')
    BDEPEND=$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$BDEPEND"')
    deps[BDEPEND]=$BDEPEND
    INHERITED=$(ebuild_eval "$ebuild" "$version" 'printf "%s" "$INHERITED"')
    OPTFEATURES=$(ebuild_eval "$ebuild" "$version" 'pkg_postinst')
    BDEPEND_FLAT=$(words "$BDEPEND" | tr '\n' ' ')
    case "$BDEPEND_FLAT" in
        *"test? ( dev-vcs/git )"*) pass "$version: git is a test-only build dependency" ;;
        *) fail "$version: BDEPEND does not carry test? ( dev-vcs/git ): $BDEPEND_FLAT" ;;
    esac
    expect "$version: tests are restricted unless USE=test" \
        "$(ebuild_eval "$ebuild" "$version" 'printf "%s|%s" "$IUSE" "$RESTRICT"')" "test|!test? ( test )"
    if has_word "$BDEPEND" virtual/pkgconfig; then
        pass "$version: pkg-config (pkg_check_modules) is a build dependency"
    else
        fail "$version: BDEPEND lacks virtual/pkgconfig"
    fi

    local kind name mapping vars atom var
    for kind in depends makedepends checkdepends optdepends; do
        while IFS= read -r name; do
            [ -n "$name" ] || continue
            name=${name%%:*}
            if ! mapping=$(gentoo_for "$name"); then
                fail "$version: PKGBUILD $kind '$name' has no Gentoo equivalent in this test"
                continue
            fi
            vars=${mapping%% *}
            atom=${mapping#* }
            case "$vars" in
                system) pass "$version: $kind $name: part of every Gentoo system" ;;
                eclass)
                    if has_word "$INHERITED" "$atom"; then
                        pass "$version: $kind $name: from the $atom eclass"
                    else
                        fail "$version: $kind $name: the $atom eclass is not inherited"
                    fi ;;
                optfeature)
                    if has_word "$OPTFEATURES" "$atom"; then
                        pass "$version: $kind $name: optfeature $atom"
                    else
                        fail "$version: $kind $name: no optfeature for $atom"
                    fi ;;
                *)
                    for var in ${vars//,/ }; do
                        if has_word "${deps[$var]}" "$atom"; then
                            pass "$version: $kind $name: $var $atom"
                        else
                            fail "$version: $kind $name: $var lacks $atom"
                        fi
                    done ;;
            esac
        done < <(pkgbuild_eval 'declare -n list="$3"; printf "%s\n" "${list[@]}"' "$kind")
    done
}

# The tarball SRC_URI and S assume: release.yml (read here, never changed) names and prefixes it.
# shellcheck disable=SC2016  # the literal $VERSION text of the workflow
if grep -qF 'tarball=Remora-$VERSION.tar.gz' "$RELEASE_YML" \
    && grep -qF -- '--prefix="Remora-$VERSION/"' "$RELEASE_YML"; then
    pass "release.yml builds Remora-<version>.tar.gz with the Remora-<version>/ prefix"
else
    fail "release.yml no longer builds the tarball name or prefix SRC_URI and S assume"
fi

check_ebuild "$VERSION"
# A version that is not the tree's, so nothing above passes by matching a literal in the template.
if [ "$VERSION" != "2099.12.7" ]; then check_ebuild "2099.12.7"; fi

# What the renderer refuses, and that a refusal writes nothing.
refused() {  # refused <what> <template> <version>
    local out="$SCRATCH/refused-$RANDOM"
    mkdir -p "$out"
    if render "$2" "$3" "$out" >/dev/null 2>&1; then
        fail "rendered $1"
    elif [ -n "$(ls -A "$out")" ]; then
        fail "refused $1 but wrote $(ls -A "$out")"
    else
        pass "refuses $1"
    fi
}
cp "$TEMPLATE" "$SCRATCH/placeholder.ebuild.in"
printf '%s\n' 'SLOT="@SLOT@"' >> "$SCRATCH/placeholder.ebuild.in"
refused "a template with an unresolved placeholder" "$SCRATCH/placeholder.ebuild.in" "$VERSION"
for bad in "v$VERSION" "2026.10" "2026.10.0;x" "2026.1.0" ""; do
    refused "version '$bad'" "$TEMPLATE" "$bad"
done

if [ "$failures" -gt 0 ]; then
    note "$failures check(s) failed" >&2
    exit 1
fi
note "all checks passed"
