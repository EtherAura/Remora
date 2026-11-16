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

# finish_ndk_overlay <source-build-dir> <system-dir>: what the staged bridge needs on top of the
# payload, in every image that carries it: assemble-image.sh's source builds and the arm_translate
# layer (stage_arm_layer below). Non-zero when the patcher refuses.
finish_ndk_overlay() {
  # ARM apps need two byte patches to these prebuilts on A17 (guest bionic_tls is NULL, and a
  # native-bridge trampoline registers a NULL thread_local dtor) — without them every guest dlopen,
  # and then every app startup, segfaults. The patcher runs on the STAGED copies so the vendored
  # prebuilts stay exactly as harvested; it verifies a SHA-256 per file and the original bytes at
  # every site, and fails the build if a prebuilt was re-harvested (bd remora-rve).
  if [ -f "$2/bin/arm64/linker64" ] && [ -f "$2/lib64/libnb.so" ]; then
    python3 "$1/patch-arm-translation.py" "$2/bin/arm64/linker64" "$2/lib64/libnb.so" || return 1
  fi
  # berberis runtime flag (bd remora-4ei.100). Amazon's lost-SIGSEGV and NIKKE's GC-suspend race
  # are signal deliveries landing on imprecise generated-code state; accurate-sigsegv synchronises
  # all registers (not just PC) on recovery, and its own source comment says it "should be the
  # default on emulators". Measured: it moves Amazon from dying on EVERY launch to ~5/6
  # surviving — a real but PARTIAL, probabilistic mitigation, NOT a fix (the crash is a genuine
  # race). NIKKE similarly improves. Sonar + Nintendo (translated apps that always worked) are
  # unaffected. TikTok's deterministic :42 frame corruption is a DIFFERENT defect, fixed by the
  # nb_sigfix Zygisk module (assemble-image.sh), not by this flag.
  #
  # ONLY accurate-sigsegv is set: the other precise-state flags in the AOSP berberis source
  # (all-jumps-exit-gen-code, disable-link-jumps-between-regions) do NOT EXIST in this A16-vintage
  # prebuilt libnb.so — it logs "Unrecognized config flag ... ignoring" and drops them. Verified by
  # enumerating the flag strings actually present in the shipped binary. Do not add flags back
  # without checking `strings libnb.so` first.
  #
  # Written as an on-the-fly init rc INSIDE the staged overlay, not into the vendored payload
  # (which stays exactly as harvested, same rule as the byte patcher above), and not by setprop
  # (runtime-only: the first container recreate would silently shed it).
  mkdir -p "$2/etc/init" || return 1
  cat > "$2/etc/init/remora-berberis.rc" <<'RC' || return 1
# Remora: precise-state signal delivery for the berberis native bridge (bd remora-4ei.100).
# Only accurate-sigsegv is recognised by the shipped libnb.so — see ndk-overlay.sh.
on init
    setprop ro.berberis.flags accurate-sigsegv
RC
}

# stage_arm_layer <ndkroot> <source-build-dir> <context-dir>: the build context of the
# arm_translate layer (bd remora-6laf.5), in <context-dir>/ndkguest, for an image built without the
# bridge such as remora24:x86_64-public. Every ARM file a source build with arm_translate installs
# comes from the payload: the guest runtime too, since the A17 tree builds none (native_bridge_
# support is never included). So the layer is those files, patched as a source build patches them,
# plus the properties that build would have set. Non-zero, with nothing half-staged left to build
# from, on any failure.
stage_arm_layer() {
  [ -n "$3" ] || { echo "stage_arm_layer: no context directory" >&2; return 1; }
  for _f in lib64/libnb.so guest/system/bin/arm64/linker64 \
            bin/ndk_translation_program_runner_binfmt_misc_arm64 etc/cpuinfo.arm64.txt; do
    [ -f "$1/$_f" ] && continue
    echo "stage_arm_layer: $1/$_f is missing. The ARM translation payload is proprietary and" >&2
    echo "  its source unrecorded, so Remora cannot fetch it: supply it (vendor/PAYLOADS.md)." >&2
    return 1
  done
  # A re-stage starts from nothing: cp -a into a surviving ndkguest/system would nest a second copy
  # inside it rather than replace it.
  rm -rf -- "$3/ndkguest"
  stage_ndk_overlay "$1" "$3" || return 1
  # What ndk.mk installs beside the libraries in a source build: the binfmt_misc runner and the
  # translated /proc/cpuinfo. Not its binfmt_misc registrations, which no image installs either:
  # nothing in these images registers them, and if anything did, a --privileged container would
  # register an arm64 handler in the HOST kernel.
  if ! { mkdir -p "$3/ndkguest/system/bin" "$3/ndkguest/system/etc" &&
         cp "$1/bin/ndk_translation_program_runner_binfmt_misc_arm64" "$3/ndkguest/system/bin/" &&
         cp "$1/etc/cpuinfo.arm64.txt" "$3/ndkguest/system/etc/" &&
         finish_ndk_overlay "$2" "$3/ndkguest/system"; }; then
    rm -rf -- "$3/ndkguest"
    return 1
  fi
  # The properties, written last so their presence means everything before them succeeded: the
  # recipe's stage check looks for this file. init reads /odm_dlkm/etc/build.prop after
  # /vendor/build.prop, and in PropertyLoadBootDefaults a later file's value REPLACES an earlier
  # one, read-only properties included (system/core/init/property_service.cpp). The public image
  # has the directory and no file in it, so the layer adds a file and replaces none of the base's.
  # ro.product.cpu.abilist is set outright because init derives it from the per-partition lists only
  # when it is unset, and the odm list (x86_64), loaded after this file, would otherwise win.
  # ro.enable.native.bridge.exec is left out: only binfmt_misc registration reads it (above).
  # ro.dalvik.vm.isa.arm is left out: the image is 64-bit only and advertises no 32-bit ABI.
  mkdir -p "$3/ndkguest/odm_dlkm/etc" || return 1
  cat > "$3/ndkguest/odm_dlkm/etc/build.prop" <<'PROP' || return 1
# Remora arm_translate layer: turns on the arm64 native bridge over an image built without it.
ro.product.cpu.abilist=x86_64,arm64-v8a
ro.product.cpu.abilist64=x86_64,arm64-v8a
ro.product.cpu.abilist32=
ro.dalvik.vm.native.bridge=libnb.so
ro.dalvik.vm.isa.arm64=x86_64
PROP
}
