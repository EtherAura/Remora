#!/bin/sh
# Stage the Play spoof add-on layer's build context (bd remora-6laf.3/.4) for `remora build --base`
# or the addons= profile option:
#
#   vendor/source-build/stage-spoof-layer.sh <build-dir> [--payload <zygisk_pif payload dir>]...
#       [--spoof] [--shamiko] [--nb-sigfix <nb_sigfix dir>]
#
# --spoof writes <build-dir>/spoof (what /system/etc/remora-spoof holds), --shamiko writes
# <build-dir>/shamiko (its shamiko/ subdirectory), --nb-sigfix adds the berberis workaround module
# to spoof/, for an image with ARM translation. With neither --spoof nor --shamiko, --spoof is meant.
#
# The tree is the vendored features/zygisk_pif with the payload's files laid UNDER it: a file the
# vendored tree has is never replaced, so Remora's installer, PIF profile and module scripts are
# always this checkout's, and the payloads supply only what a public clone lacks (the ReZygisk and
# PIF binaries, and all of Shamiko, which is closed source and never fetched). More than one
# --payload may be given, Shamiko's apart from the rest; an earlier one wins a file both have.
set -u
here=$(cd "$(dirname "$0")" && pwd)
build=${1:?usage: stage-spoof-layer.sh <build-dir> [--payload <dir>]... [--spoof] [--shamiko] [--nb-sigfix <dir>]}
shift
payloads=
spoof=0 shamiko=0 nbsig=
while [ $# -gt 0 ]; do
  case "$1" in
    --payload)
      [ -d "${2:-}" ] || { echo "stage-spoof-layer: no payload directory ${2:-}" >&2; exit 1; }
      payloads="$payloads
$(cd "$2" && pwd)"; shift ;;
    --spoof) spoof=1 ;;
    --shamiko) shamiko=1 ;;
    --nb-sigfix) nbsig=${2:?--nb-sigfix needs a directory}; shift ;;
    *) echo "stage-spoof-layer: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
[ "$spoof" = 0 ] && [ "$shamiko" = 0 ] && spoof=1
# shellcheck source=vendor/source-build/spoof-overlay.sh
. "$here/spoof-overlay.sh"

need() {  # <what> <file>...
  _what=$1; shift
  for _f in "$@"; do
    [ -f "$merged/$_f" ] && continue
    echo "stage-spoof-layer: $_what needs $_f, which is in neither the vendored zygisk_pif nor a" >&2
    echo "  --payload directory. It is a third-party payload (vendor/PAYLOADS.md, zygisk_pif row):" >&2
    echo "  supply it." >&2
    exit 1
  done
}

mkdir -p "$build" || exit 1
merged=$(mktemp -d "$build/.zygisk_pif.XXXXXX") || exit 1
trap 'rm -rf -- "$merged"' EXIT INT TERM
cp -a "$here/features/zygisk_pif/." "$merged/" || exit 1
printf '%s\n' "$payloads" | while IFS= read -r p; do
  [ -n "$p" ] && [ "$p" != "$here/features/zygisk_pif" ] || continue
  (cd "$p" && find . -type f) | while IFS= read -r f; do
    [ -e "$merged/$f" ] && continue
    mkdir -p "$merged/$(dirname "$f")" && cp -p "$p/$f" "$merged/$f" || exit 1
  done || exit 1
done || exit 1

# A re-stage starts from nothing, for the same nesting reason stage_spoof_tree gives.
rm -rf -- "$build/spoof" "$build/shamiko"
if [ "$spoof" = 1 ]; then
  need play_spoof rez-mod/lib64/libzygisk.so rez-mod/bin/zygiskd64 rez-mod/bin/zygisk-ptrace64 \
       pifork/zygisk/x86_64.so
  stage_spoof_tree "$merged" "$build/spoof" || exit 1
  if [ -n "$nbsig" ]; then
    stage_spoof_nb_sigfix "$nbsig" "$build/spoof/nb_sigfix" \
      || echo "stage-spoof-layer: nb_sigfix has no built .so in $nbsig — left out, so TikTok and" \
              "Amazon keep crashing under translation" >&2
  fi
fi
if [ "$shamiko" = 1 ]; then
  need shamiko shamiko/zygisk/x86_64.so shamiko/module.prop
  stage_spoof_shamiko "$merged" "$build/shamiko" || exit 1
fi
echo "staged the Play spoof layer in $build ($( [ "$spoof" = 1 ] && printf 'spoof ')$( [ "$shamiko" = 1 ] && printf 'shamiko'))"
