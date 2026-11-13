# shellcheck shell=sh
# The ARM translation overlay's gate and staging, sourced by assemble-image.sh. A file of its own so
# the engine suite can source the SAME functions against a throwaway fixture without assembling an
# image; nothing here runs when sourced.
#
# WITH_NDK follows the build's feature convention: `true` selects ARM translation. Any other
# non-empty value is an explicit deselection, and the overlay stays out EVEN WHEN the payload is
# vendored — that the directory merely existing put the bridge into images built with
# arm_translate off was the defect this gate replaces. Unset keeps the original presence rule for
# hand invocations that never named a selection.

# ndk_overlay_on <ndkroot>: 0 when the overlay should be staged.
ndk_overlay_on() {
  if [ -n "${WITH_NDK:-}" ] && [ "$WITH_NDK" != true ]; then
    return 1
  fi
  [ -d "$1/guest/system" ]
}

# stage_ndk_overlay <ndkroot> <context-dir>: copy the guest payload into <context-dir>/ndkguest and
# merge the host native-bridge libs (libnb.so etc.) into its /system/lib64. The libs are kept at the
# feature root (single source of truth: the WITH_NDK build path installs the SAME files) rather
# than duplicated under guest/. On A16 this re-places identical bytes (harmless); on A17 it
# supplies libnb.so the skipped WITH_NDK path would have (bd remora-rve).
stage_ndk_overlay() {
  mkdir -p "$2/ndkguest"
  cp -a "$1/guest/system" "$2/ndkguest/system"
  if ls "$1"/lib64/*.so >/dev/null 2>&1; then
    mkdir -p "$2/ndkguest/system/lib64"
    cp "$1"/lib64/*.so "$2/ndkguest/system/lib64/"
  fi
}
