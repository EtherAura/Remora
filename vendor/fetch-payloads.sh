#!/bin/sh
# Stage the third-party binary payloads a source build needs, into
# vendor/source-build/features/<payload>/ — the directories the public repo deliberately does not
# carry (vendor/PAYLOADS.md).
#
#   vendor/fetch-payloads.sh            # report what is present and what is missing
#   vendor/fetch-payloads.sh <payload>  # stage one
#
# REFUSES TO GUESS. A build script that silently pulls an unidentified binary into an image is
# worse than one that stops, so nothing here constructs a plausible URL. A payload is fetched only
# when PAYLOADS.md records an upstream precise enough to VERIFY what came back — which in practice
# means a hash, not a link.
set -e
SELF=$(cd "$(dirname "$0")" && pwd)
FEAT="$SELF/source-build/features"

# payload:status. THREE states, because "we cannot fetch this" has two different causes and they
# need two different answers (bd remora-28ix.6):
#
#   gh:...      Identified AND fetchable: a public release plus a recorded hash to check it
#               against. Implemented below.
#   identified  We know exactly WHICH artifact this is — repo, commit, often a verified
#               byte-for-byte match — but no download URL is recorded. The fetch is one command
#               away for a human and unguessable for a script, so it prints what to get.
#   unrecorded  We do not know what this is. Supply it yourself.
#
# Keeping 'identified' distinct from 'unrecorded' is the point of this pass: most rows were never
# unknown, only un-URLed, and lumping them together made the archaeology look unfinished.
PAYLOADS="gapps:unrecorded microg:identified ndk_translation:unrecorded magisk:gh
          widevine:identified zygisk_pif:identified"

# --- the one fetchable row, and what makes it fetchable -----------------------------------------
# Upstream Magisk (bd remora-28ix.6): a public GPLv3 release with a stable asset name and a hash we
# recorded when we vendored it. The hash is the whole point — it is what turns a download into a
# verification, and it is why this row can be automated while the others cannot.
MAGISK_VER="v30.7"
MAGISK_URL="https://github.com/topjohnwu/Magisk/releases/download/${MAGISK_VER}/Magisk-${MAGISK_VER}.apk"
MAGISK_APK_SHA256="e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5"

status_of() {
    for e in $PAYLOADS; do
        case "$e" in "$1":*) echo "${e#*:}"; return 0 ;; esac
    done
    echo "unknown"
}

# Does this payload dir hold actual binaries, or only the .mk/README that ship publicly?
has_payload() {
    [ -d "$FEAT/$1" ] || return 1
    find "$FEAT/$1" \( -name '*.apk' -o -name '*.so' -o -name '*.jar' -o -name '*.dex' \) \
        -type f -print -quit 2>/dev/null | grep -q .
}

describe() {
    case "$1" in
        gh)         echo "fetchable: run 'fetch-payloads.sh $2'" ;;
        identified) echo "identified but no URL recorded — see PAYLOADS.md" ;;
        unrecorded) echo "source unrecorded — supply it yourself" ;;
        *)          echo "$1" ;;
    esac
}

report() {
    echo "payload            state"
    echo "-----------------  -----------------------------------------------"
    for e in $PAYLOADS; do
        p=${e%%:*}
        if has_payload "$p"; then printf '%-18s present\n' "$p"
        else printf '%-18s MISSING — %s\n' "$p" "$(describe "${e#*:}" "$p")"; fi
    done
    echo
    echo "Features whose payload is missing will REFUSE at staging rather than build without it."
}

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "fetch-payloads: needs '$1' and it is not on PATH" >&2; exit 1; }
}

# Download, then CHECK BEFORE USE. Verifying after unpacking would be too late: the point of the
# hash is that an unexpected binary never reaches the tree at all.
fetch_verified() {
    _url="$1"; _want="$2"; _dest="$3"
    echo "  fetching $_url"
    curl -fsSL --retry 3 -o "$_dest" "$_url" || {
        echo "fetch-payloads: download failed" >&2; return 1; }
    _got=$(sha256sum "$_dest" | cut -d' ' -f1)
    if [ "$_got" != "$_want" ]; then
        rm -f "$_dest"
        echo "fetch-payloads: HASH MISMATCH — refusing this file and deleting it." >&2
        echo "  expected $_want" >&2
        echo "  got      $_got" >&2
        echo "  Either upstream re-cut the release, or this is not the artifact PAYLOADS.md" >&2
        echo "  records. Do NOT edit the expected hash to make this pass: re-verify what the" >&2
        echo "  file is first, then update PAYLOADS.md and the vendored copy together." >&2
        return 1
    fi
    echo "  sha256 OK"
}

# The extraction table from features/magisk/magisk/PROVENANCE.md, in executable form. Kept in step
# with that file by hand -- it is the human-readable half of this and states what each file is for.
fetch_magisk() {
    need curl; need unzip; need sha256sum
    _out="$FEAT/magisk/magisk"
    mkdir -p "$_out"
    _tmp=$(mktemp -d)
    # shellcheck disable=SC2064
    trap "rm -rf '$_tmp'" EXIT INT TERM

    fetch_verified "$MAGISK_URL" "$MAGISK_APK_SHA256" "$_tmp/magisk.apk" || return 1

    unzip -qo "$_tmp/magisk.apk" -d "$_tmp/x" || { echo "unzip failed" >&2; return 1; }
    for pair in \
        "lib/x86_64/libmagisk.so:magisk" \
        "lib/x86_64/libmagiskboot.so:magiskboot" \
        "lib/x86_64/libmagiskinit.so:magiskinit" \
        "lib/x86_64/libmagiskpolicy.so:magiskpolicy" \
        "lib/x86_64/libbusybox.so:busybox" \
        "assets/stub.apk:stub.apk" \
        "assets/util_functions.sh:util_functions.sh" \
        "assets/boot_patch.sh:boot_patch.sh"
    do
        _src="$_tmp/x/${pair%%:*}"; _dst="$_out/${pair#*:}"
        [ -f "$_src" ] || { echo "fetch-payloads: ${pair%%:*} missing from the apk — upstream changed its layout" >&2; return 1; }
        cp -f "$_src" "$_dst"
    done
    cp -f "$_tmp/magisk.apk" "$_out/magisk.apk"
    chmod 755 "$_out/magisk" "$_out/magiskboot" "$_out/magiskinit" \
              "$_out/magiskpolicy" "$_out/busybox"
    echo "  staged Magisk $MAGISK_VER into $_out"
    echo "  NOTE: this is UPSTREAM topjohnwu Magisk. It needs magisk-setup.sh (one directory up)"
    echo "  to build its tmpfs environment in a container — upstream has no --setup-sbin."
}

fetch_one() {
    p="$1"
    st=$(status_of "$p")
    case "$st" in
        unknown)
            echo "fetch-payloads: '$p' is not a known payload. Known:" >&2
            for e in $PAYLOADS; do echo "  ${e%%:*}" >&2; done
            exit 2 ;;
        gh)
            case "$p" in
                magisk) fetch_magisk ;;
                *) echo "fetch-payloads: '$p' is marked fetchable but has no fetcher" >&2; exit 1 ;;
            esac ;;
        identified)
            echo "fetch-payloads: '$p' IS identified — we know exactly which artifact it is —" >&2
            echo "  but no download URL is recorded, so this script will not guess one." >&2
            echo "  Read the '$p' row in vendor/PAYLOADS.md: it names the upstream project and" >&2
            echo "  version (for widevine, the exact repo AND commit, verified byte-for-byte)." >&2
            echo "  Obtain that artifact, place it at:  $FEAT/$p" >&2
            echo "  then add the URL and sha256 to PAYLOADS.md and promote the row to 'gh' here," >&2
            echo "  so the next person gets a verified download instead of this message." >&2
            exit 1 ;;
        unrecorded)
            echo "fetch-payloads: '$p' has no recorded upstream source, so there is nothing" >&2
            echo "  safe to download. See the '$p' row in vendor/PAYLOADS.md for what the" >&2
            echo "  payload is and what its licence permits, obtain it yourself, and place it" >&2
            echo "  at:  $FEAT/$p" >&2
            echo "  Then record the source + hashes in PAYLOADS.md so the next person is not" >&2
            echo "  in this position (bd remora-28ix.6)." >&2
            exit 1 ;;
        *)
            echo "fetch-payloads: '$p' claims source '$st' but no fetcher is implemented." >&2
            exit 1 ;;
    esac
}

[ $# -eq 0 ] && { report; exit 0; }
fetch_one "$1"
