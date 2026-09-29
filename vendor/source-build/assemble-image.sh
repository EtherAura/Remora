#!/bin/sh
# Package a completed source build's product partitions into a container-bootable docker image.
# Runs on the docker host (NOT inside the builder container). Args: <tree> <tag>
#
# root/ (ramdisk skeleton, /init -> /system/bin/init) + system/ + vendor/ (+ optional dlkm) under
# out/target/product/<device>/ become the container rootfs (FROM scratch, one layer/partition).
# NOTE: this yields a bootable image ONLY when the tree includes the container-compat core
# patches (patched init/frameworks). A stock LineageOS init boots on a real device but exits in
# ~90ms as a container PID1. See the container_compat_patches feature.
set -e
TREE="$1"
TAG="$2"
[ -n "$TREE" ] && [ -n "$TAG" ] || { echo "usage: assemble-image.sh <tree> <tag>"; exit 2; }

# Which product dir. PRODUCT_DIR (or a derivable BUILD_LUNCH) names it exactly; the glob is
# only trusted when it is UNAMBIGUOUS. A tree that has ever built a second product keeps that
# product's dir under out/, and taking the alphabetically first one (the old `head -1`) silently
# assembled a STALE rootfs under a fresh tag (bd remora-28ix.4) — refuse and name the fix instead.
PROD=""
[ -n "${PRODUCT_DIR:-}" ] && PROD="$TREE/out/target/product/$PRODUCT_DIR"
[ -z "$PROD" ] && [ -n "${BUILD_LUNCH:-}" ] && PROD="$TREE/out/target/product/${BUILD_LUNCH%%-*}"
if [ -z "$PROD" ]; then
    set -- "$TREE"/out/target/product/*/system.img
    if [ "$#" -gt 1 ]; then
        echo "assemble: MULTIPLE product dirs have a system.img:" >&2
        for p in "$@"; do echo "    $(dirname "$p")" >&2; done
        echo "  Guessing here once shipped the wrong rootfs under a fresh tag. Name the one you" >&2
        echo "  mean: PRODUCT_DIR=<device> (e.g. remora_x86_64) or BUILD_LUNCH=<lunch-config>." >&2
        exit 1
    fi
    PROD=$(dirname "$1")
fi
[ -f "$PROD/system.img" ] || { echo "assemble: no system.img under $PROD"; exit 1; }

# THE CPU TUNING THIS BUILD ACTUALLY USED, stamped onto the image so a deploy can refuse a host that
# cannot execute it (bd remora-bm7.13). A TARGET_ARCH_VARIANT image does not run SLOWER on a CPU
# without the ISA — it SIGILLs, and the log names libart or a vendor HAL rather than the processor.
# Delivering an image between machines is `docker save | ssh docker load`, which has no step where
# anything would notice; the alderlake image here was one command from landing on two
# Zen 1 hosts that lack every Alder Lake addition.
#
# READ FROM THE BOARDCONFIG, NEVER FROM THE TAG. Before eae38f6 the engine's arch-variant sed
# rewrote a BoardConfig the A17 product does not build, so every image tagged -alderlake in that
# window was compiled at BASELINE — a tag-derived stamp would mark those as tuned and get them
# refused on hardware that runs them perfectly well. The BoardConfig is what the build read.
#
# BOARD_CONFIG_DIR comes from the engine's version registry (the same field the sed uses), because
# which device tree a release builds from is stated there and derivable nowhere else — that is the
# whole lesson of bd remora-28ix.4 R1. The glob is the manual-invocation fallback and is trusted
# only when it resolves to exactly one file.
BC=""
[ -n "${BOARD_CONFIG_DIR:-}" ] && BC="$TREE/$BOARD_CONFIG_DIR/BoardConfig.mk"
if [ -z "$BC" ]; then
    set -- "$TREE"/device/*/"$(basename "$PROD")"/BoardConfig.mk
    [ "$#" -eq 1 ] && [ -f "$1" ] && BC="$1"
fi
ARCH_VARIANT=""
if [ -n "$BC" ] && [ -f "$BC" ]; then
    ARCH_VARIANT=$(sed -n 's/^TARGET_ARCH_VARIANT *:= *//p' "$BC" | head -1 | tr -d ' \t\r')
fi
if [ -n "$ARCH_VARIANT" ]; then
    echo "=== arch variant: $ARCH_VARIANT (from $BC) ==="
else
    # A WARNING AND NOT A FAILURE. An unstamped image is indistinguishable from one built before
    # this existed, and the host-side guard waves both through — refusing to assemble here would
    # trade a real, shipped image for a missing label. Baseline builds DO get stamped (the device
    # tree's own line says x86_64), so an absent label means "predates this", not "untuned".
    echo "assemble: WARNING could not read TARGET_ARCH_VARIANT (BoardConfig ${BC:-not found})" >&2
    echo "assemble: the image will carry no arch-variant stamp, so no deploy can check it" >&2
fi

# Widevine L3: prebuilt AIDL blob + its vendor dependency closure,
# harvested from the proven golden image — DRM apps (Pluto/Netflix &co) get black video without
# it. Overlaid as an extra COPY layer (SELinux is disabled in the container, so no sepolicy work
# is needed).
WVDIR="$(cd "$(dirname "$0")" && pwd)/features/widevine"
# The tag is a contract, both ways: '-wv' promises the CDM is baked in, no '-wv' promises it
# isn't. When the feature dir is absent the overlay would silently no-op, shipping an
# aspirational -wv image that fails only at DRM playback (twice now — bd remora-lud): refuse up
# front. And since the vendored dir always exists in the repo, gate the overlay on the tag, not
# the dir, so widevine_l3=off actually omits the CDM (bd remora-0go).
# native_vulkan is GONE (bd remora-xqt1) and nothing here writes ro.hardware.vulkan any more.
# The note it used to carry was right that nothing set the property at runtime on the HOST path —
# gpu_config.sh's setup_vulkan reads /sys/kernel/debug/dri, which is not mounted in a Remora
# container, so it silently set nothing — but pinning it at BUILD time was the wrong repair. ro.*
# is write-once and build.prop is read before any init action, so the pin beat every later
# decision: both AMD hosts ran amdgpu while reading intel, and guest mode could not boot at all
# because gpu_setup_guest's pastel lost to it. The driver depends on the deploy, not the image
# (with Venus the selected node is Intel while rendering leaves for NVIDIA), so the host resolves
# it per boot and sends androidboot.remora_vulkan, which remora.props.rc maps onto the property.

WV_ON=0
case "$TAG" in
*-wv|*-wv-*)
  # The CDM itself, not the directory: its .rc and VINTF fragment are text and could be present
  # without it, which would overlay an init service pointing at a binary that is not there.
  [ -f "$WVDIR/vendor/lib64/libwvaidl.so" ] || {
    echo "assemble: tag $TAG promises Widevine but the CDM is missing from $WVDIR/vendor" >&2
    echo "assemble: it is a third-party payload (vendor/PAYLOADS.md, vendor/fetch-payloads.sh" >&2
    echo "          widevine) — supply it, or build without widevine_l3" >&2
    exit 1
  }
  WV_ON=1 ;;
esac
# The three overlays below are DELIBERATELY always-on when vendored (no tag marker, no toggle):
# they are container-viability plumbing every image wants, not à-la-carte features (bd remora-0go).
#
# ARM translation payload. TWO halves, both overlaid into /system:
#  (1) guest payload (guest/system): bin/arm64 app_process64+linker64, lib64/arm64 guest system
#      libs, libberberis_exec_region, etc/ld.config.arm64.txt — the guest-ARM runtime the AOSP
#      build does NOT produce despite TARGET_NATIVE_BRIDGE_ARCH.
#  (2) host native-bridge libs (lib64/*.so): libnb.so + libndk_translation.so + the proxy libs.
#      These are the x86_64 side that ART dlopens via ro.dalvik.vm.native.bridge=libnb.so. They
#      also arrive through the WITH_NDK build-module path (ndk.mk/Android.mk), but an image that
#      lacks them crashes every ARM app with 'UnsatisfiedLinkError ... is for EM_AARCH64 instead of
#      EM_X86_64' (bd remora-rve), and A17 once built without that path at all.
# Overlaying half (2) here makes ARM translation self-sufficient regardless of the build path, the
# same way widevine overlays /vendor. Harvested from the proven golden image.
NDKROOT="$(cd "$(dirname "$0")" && pwd)/features/ndk_translation"
NDKGUEST="$NDKROOT/guest"
# Device-compat overlay (GPS feature decl etc.) — merged into /product.
DEVCOMPAT="$(cd "$(dirname "$0")" && pwd)/features/device_compat"
# Extra hardware-feature declarations (GPS &co): Play gates app installs on device features.
HWFEAT="$(cd "$(dirname "$0")" && pwd)/features/hw_features"
# Play spoof (Zygisk/PIF): bake the patched ReZygisk + PlayIntegrityFix stack into /system so an
# image can spoof Play to a certified Pixel *inside GMS only* (system build.prop stays real). Baked
# dormant — bringup installs it to /data + activates only when the play_spoof runtime option is on.
# Enabled at build time via WITH_PLAY_SPOOF=true (the play_spoof build feature).
ZPIF="$(cd "$(dirname "$0")" && pwd)/features/zygisk_pif"
PS_ON=0
# Asked for and absent is a refusal, not a skip: the tag does not say whether the stack is in, so a
# silent skip ships an image whose play_spoof option then does nothing. Checked by the binaries,
# because the module scripts beside them are text and ship without them (vendor/PAYLOADS.md).
need_zygisk() {  # <what> <file>...
  _what="$1"; shift
  for _f in "$@"; do
    [ -f "$_f" ] && continue
    echo "assemble: $_what is enabled but $_f is missing" >&2
    echo "assemble: it is a third-party payload (vendor/PAYLOADS.md, zygisk_pif row) — supply" >&2
    echo "          it, or build without that feature" >&2
    exit 1
  done
}
if [ "$WITH_PLAY_SPOOF" = true ]; then
  need_zygisk play_spoof "$ZPIF/rez-mod/lib64/libzygisk.so" "$ZPIF/rez-mod/bin/zygiskd64" \
      "$ZPIF/pifork/zygisk/x86_64.so"
  [ "$WITH_SHAMIKO" = true ] && need_zygisk shamiko "$ZPIF/shamiko/zygisk/x86_64.so"
  PS_ON=1
fi
if [ "$PS_ON" = 1 ]; then
  PSDIR=$(mktemp -d)
  # Staged read-only under /system/etc/remora-spoof/; the presence of this dir IS the image gate.
  mkdir -p "$PSDIR/system/etc/remora-spoof"
  cp -a "$ZPIF/rez-mod"   "$PSDIR/system/etc/remora-spoof/rez-mod"
  cp -a "$ZPIF/pifork"    "$PSDIR/system/etc/remora-spoof/playintegrityfix"
  cp    "$ZPIF/pif.json"  "$PSDIR/system/etc/remora-spoof/pif.json"
  cp    "$ZPIF/install.sh" "$PSDIR/system/etc/remora-spoof/install.sh"
  [ -d "$ZPIF/devicespoof" ] && cp -a "$ZPIF/devicespoof" "$PSDIR/system/etc/remora-spoof/devicespoof"
  # Shamiko — unmounts Magisk's modifications from denylisted processes, which is the one thing
  # devicespoof cannot do. install.sh has copied it conditionally since it was written; nothing
  # ever staged it, so that branch had never once been taken (bd remora-4ei.50).
  # Its OWN gate, not play_spoof's: bd remora-4ei.16 named Shamiko a prime suspect for breaking
  # Play Store verification on A16, so "PIF but no Shamiko" has to stay expressible.
  if [ "$WITH_SHAMIKO" = true ]; then
    cp -a "$ZPIF/shamiko" "$PSDIR/system/etc/remora-spoof/shamiko"
    echo "=== play_spoof: + Shamiko (denylist unmount) ==="
  fi
  # nb_sigfix — the berberis app workarounds (bd remora-4ei.99 TikTok signal, bd remora-4ei.100
  # Amazon CPU-affinity pin; the id stays narrow, see its REMORA.md). Rides the same remora-spoof
  # install path because it is a Zygisk module and ReZygisk is only present when play_spoof is on;
  # additionally gated on libnb.so, which it hooks — the ARM translation overlay below puts it in
  # every image that has it vendored. Remora-authored, so its .so is built from src/ here, with the
  # tree just built as the toolchain, rather than trusted as an opaque prebuilt.
  #
  # The gate used to be WITH_NDK, which the build never passes to this script, so no image Remora
  # built carried the module — the A17 image has no remora-spoof/nb_sigfix.
  NBSIG="$(cd "$(dirname "$0")" && pwd)/features/nb_sigfix"
  if [ -f "$NDKROOT/lib64/libnb.so" ]; then
    # A failed build keeps the committed .so, when there is one; a public clone has none (binaries
    # are not published), so there the module is left out — loudly.
    sh "$NBSIG/build.sh" "$TREE" \
      || echo "=== nb_sigfix: rebuild FAILED — using the committed .so if there is one ===" >&2
    if [ -f "$NBSIG/zygisk/x86_64.so" ]; then
      mkdir -p "$PSDIR/system/etc/remora-spoof/nb_sigfix/zygisk"
      cp "$NBSIG/module.prop"      "$PSDIR/system/etc/remora-spoof/nb_sigfix/module.prop"
      cp "$NBSIG/zygisk/x86_64.so" "$PSDIR/system/etc/remora-spoof/nb_sigfix/zygisk/x86_64.so"
      echo "=== play_spoof: + nb_sigfix (berberis app workarounds: TikTok signal, Amazon affinity) ==="
    else
      echo "=== nb_sigfix: NOT BAKED — no .so; TikTok and Amazon crash under translation ===" >&2
    fi
  fi
  echo "=== play_spoof: baking ReZygisk+PIF into /system/etc/remora-spoof ==="
fi

DF=$(mktemp)
{
  echo "FROM scratch"
  echo "COPY root /"
  echo "COPY system /system"
  echo "COPY vendor /vendor"
  for p in system_dlkm vendor_dlkm product system_ext odm odm_dlkm; do
    [ -d "$PROD/$p" ] && echo "COPY $p /$p"
  done
  [ "$WV_ON" = 1 ] && echo "COPY wv/vendor /vendor"
  [ -d "$NDKGUEST/system" ] && echo "COPY ndkguest/system /system"
  [ -d "$DEVCOMPAT/product" ] && echo "COPY devcompat/product /product"
  [ -d "$HWFEAT/system" ] && echo "COPY hwfeat/system /system"
  # play_spoof: bake the dormant ReZygisk+PIF stack under /system/etc/remora-spoof
  [ "$PS_ON" = 1 ] && echo "COPY ps/system /system"
  # Android's binaries live in /system/bin etc., not the Linux default PATH. Remora's guest
  # scripts run `docker exec <c> sh …` / `getprop …`, which resolve against the container PATH —
  # without this they fail "exec /bin/sh: no such file or directory" and netfix/adb never come up.
  echo 'ENV PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin:/vendor/bin'
  # A LABEL and not a build.prop line, because of WHO has to read it: the deploy preflight, on a
  # host where the image has never been run, possibly over ssh. A label is in the image CONFIG, so
  # one `docker image inspect --format` answers it without creating a container or extracting a
  # layer — and it survives `docker save | docker load`, which is the delivery path this defends.
  [ -n "$ARCH_VARIANT" ] && echo "LABEL remora.arch_variant=\"$ARCH_VARIANT\""
  # THE HARDWARE IDENTITY IS SET HERE AND NOWHERE ELSE (bd remora-28ix.4 R4). Not in
  # device/remora's remora.prop, where it looks like it should live: init's PropertyInit calls
  # ExportKernelBootProps() — which copies ro.boot.hardware to ro.hardware — BEFORE
  # PropertyLoadBootDefaults() reads /vendor/build.prop, and ro.* is write-once, so a build.prop
  # line for it can never take effect. (Nor does dropping the arg help: ExportKernelBootProps
  # defaults it to "unknown", which still wins the race.) Verified against
  # system/core/init/property_service.cpp in the A17 tree, not assumed.
  #
  # SAFE TO FLIP because nothing resolves a HAL through it on a healthy boot: hw_get_module tries
  # ro.hardware.<class> first and gpu_config.sh names gralloc, egl, hwcomposer and vulkan
  # explicitly in both GPU modes. On the failure path where gpu_config never ran, the fallback now
  # lands on gralloc.remora.so / hwcomposer.remora.so — our own modules — rather than on
  # gralloc.default.so. The prebuilt composer keeps working when selected, because it is selected
  # by name via ro.hardware.hwcomposer.
  #
  # Host-side this is already tolerated: Orchestrator's boot-arg staleness check drops
  # "hardware=" from the container side outright, so a pre-flip container does not churn.
  echo 'ENTRYPOINT ["/init", "qemu=1", "androidboot.hardware=remora"]'
} > "$DF"

parts="root system vendor"
for p in system_dlkm vendor_dlkm product system_ext odm odm_dlkm; do
  [ -d "$PROD/$p" ] && parts="$parts $p"
done

echo "=== assembling $TAG from $PROD ($parts) ==="
# The build produces some artifacts as root-only 0600 (e.g. system/etc/llndk.libraries.txt,
# permission XMLs). Tarring the context as the host user silently DROPS unreadable files — and a
# missing llndk.libraries.txt leaves the vendor linker namespace without LLNDK libs, so every
# vendor HAL fails to link liblog.so and the container stalls at boot. Build the context AS ROOT
# inside a throwaway container so every file is included regardless of owner/mode.
DFDIR=$(mktemp -d)
cp "$DF" "$DFDIR/Dockerfile"
parts_extra=""
if [ "$WV_ON" = 1 ]; then
  mkdir -p "$DFDIR/wv"
  cp -a "$WVDIR/vendor" "$DFDIR/wv/vendor"
  parts_extra="wv"
fi
if [ -d "$NDKGUEST/system" ]; then
  mkdir -p "$DFDIR/ndkguest"
  cp -a "$NDKGUEST/system" "$DFDIR/ndkguest/system"
  # Merge the host native-bridge libs (libnb.so etc.) into the overlay's /system/lib64. Kept at the
  # feature root (single source of truth: the WITH_NDK build path installs the SAME files) rather
  # than duplicated under guest/. On A16 this re-places identical bytes (harmless); on A17 it
  # supplies libnb.so the skipped WITH_NDK path would have (bd remora-rve).
  if ls "$NDKROOT"/lib64/*.so >/dev/null 2>&1; then
    mkdir -p "$DFDIR/ndkguest/system/lib64"
    cp "$NDKROOT"/lib64/*.so "$DFDIR/ndkguest/system/lib64/"
  fi
  # A17 builds its OWN arm64 guest payload (guest linker + app_process + guest libs
  # from frameworks/libs/native_bridge_support, pulled in by the device product
  # config). Where the tree produced one, do NOT overlay the harvested Android-16
  # payload on top: an A16 guest userspace running against A17's host bionic/ART is
  # exactly what left ARM apps crashing on a near-null pointer inside translated
  # code (bd remora-rve). Keep only what the tree cannot build — the berberis JIT
  # (libnb.so and friends) and the guest ld.config.
  if [ -f "$PROD/system/bin/arm64/linker64" ]; then
    echo "=== ndk_translation: tree has its own arm64 guest payload; overlaying host bridge libs only ==="
    rm -rf "$DFDIR/ndkguest/system/bin" "$DFDIR/ndkguest/system/lib64/arm64"
  fi
  # ARM apps need two byte patches to these prebuilts on A17 (guest bionic_tls is
  # NULL, and a native-bridge trampoline registers a NULL thread_local dtor) —
  # without them every guest dlopen, and then every app startup, segfaults. The
  # patcher runs on the STAGED copies so the vendored prebuilts stay exactly as
  # harvested; it verifies a SHA-256 per file and the original bytes at every
  # site, and fails the build if a prebuilt was re-harvested (bd remora-rve).
  if [ -f "$DFDIR/ndkguest/system/bin/arm64/linker64" ] && \
     [ -f "$DFDIR/ndkguest/system/lib64/libnb.so" ]; then
    python3 "$(cd "$(dirname "$0")" && pwd)/patch-arm-translation.py" \
        "$DFDIR/ndkguest/system/bin/arm64/linker64" \
        "$DFDIR/ndkguest/system/lib64/libnb.so" || exit 1
  fi
  # berberis runtime flag (bd remora-4ei.100). Amazon's lost-SIGSEGV and NIKKE's GC-suspend race
  # are signal deliveries landing on imprecise generated-code state; accurate-sigsegv synchronises
  # all registers (not just PC) on recovery, and its own source comment says it "should be the
  # default on emulators". Measured: it moves Amazon from dying on EVERY launch to ~5/6
  # surviving — a real but PARTIAL, probabilistic mitigation, NOT a fix (the crash is a genuine
  # race). NIKKE similarly improves. Sonar + Nintendo (translated apps that always worked) are
  # unaffected. TikTok's deterministic :42 frame corruption is a DIFFERENT defect, fixed by the
  # nb_sigfix Zygisk module below, not by this flag.
  #
  # ONLY accurate-sigsegv is set: the other precise-state flags in the AOSP berberis source
  # (all-jumps-exit-gen-code, disable-link-jumps-between-regions) do NOT EXIST in this A16-vintage
  # prebuilt libnb.so — it logs "Unrecognized config flag ... ignoring" and drops them. Verified by
  # enumerating the flag strings actually present in the shipped binary. Do not add flags back
  # without checking `strings libnb.so` first.
  #
  # Written as an on-the-fly init rc INSIDE the ndkguest overlay, not into the vendored payload
  # (which stays exactly as harvested, same rule as the byte patcher above), and not by setprop
  # (runtime-only: the first container recreate would silently shed it).
  mkdir -p "$DFDIR/ndkguest/system/etc/init"
  cat > "$DFDIR/ndkguest/system/etc/init/remora-berberis.rc" <<'RC'
# Remora: precise-state signal delivery for the berberis native bridge (bd remora-4ei.100).
# Only accurate-sigsegv is recognised by the shipped libnb.so — see assemble-image.sh.
on init
    setprop ro.berberis.flags accurate-sigsegv
RC
  parts_extra="$parts_extra ndkguest"
fi
if [ -d "$DEVCOMPAT/product" ]; then
  mkdir -p "$DFDIR/devcompat"
  cp -a "$DEVCOMPAT/product" "$DFDIR/devcompat/product"
  parts_extra="$parts_extra devcompat"
fi
if [ -d "$HWFEAT/system" ]; then
  mkdir -p "$DFDIR/hwfeat"
  cp -a "$HWFEAT/system" "$DFDIR/hwfeat/system"
  parts_extra="$parts_extra hwfeat"
fi
if [ "$PS_ON" = 1 ]; then
  cp -a "$PSDIR" "$DFDIR/ps"
  parts_extra="$parts_extra ps"
fi
# --mode='go+rX': the raw out/ dir carries build-host modes, NOT the fs_config perms a real
# system.img gets — some files land 0600 root-only (llndk.libraries.txt again). Boot survives
# (system procs are root) but ART's nativeloader reads llndk.libraries.txt from APP uids, so
# every app with native libs (GMS!) aborts 'Permission denied'. Grant world-read like fs_config
# would; X keeps directories traversable without marking plain files executable.
# When our RemoraUpdater is present it REPLACES the stock LineageOS updater (Settings > System >
# System updates) — exclude the stock Updater priv-app from the image so only ours remains. (A
# full source build drops it via PRODUCT_PACKAGES_REMOVE; this covers the assemble-only path.)
# system_ext may be a separate partition OR nested under system/, depending on the board — cover
# both layouts for detection and for the exclude path.
EXCLUDES=""
if [ -d "$PROD/system_ext/app/RemoraUpdater" ] || [ -d "$PROD/system/system_ext/app/RemoraUpdater" ]; then
  EXCLUDES="--exclude=system_ext/priv-app/Updater --exclude=system/system_ext/priv-app/Updater"
fi
# Stock AOSP apps that lineage.mk already drops via PRODUCT_PACKAGES_REMOVE (Camera2, Gallery2,
# QuickSearchBox — see device/remora's lineage.mk). That directive only takes effect on a
# CLEAN build: an INCREMENTAL build leaves the already-compiled APKs in out/, and this script would
# copy them straight back into the image. That is exactly how QuickSearchBox and Gallery2 reappeared
# after the incremental ARM-translation build — they had been deleted from out/ by hand
# once already, and a manual rm in out/ is not durable. This is (bd remora-bm7.7). Same reasoning
# and mechanism as the Updater exclusion above; keep this list in sync with PRODUCT_PACKAGES_REMOVE.
# Both layouts are covered because product may be its own partition or nested under system/.
for app in Camera2 Gallery2 QuickSearchBox; do
  EXCLUDES="$EXCLUDES --exclude=product/app/$app --exclude=system/product/app/$app"
  EXCLUDES="$EXCLUDES --exclude=app/$app --exclude=system/app/$app"
done
# The same treatment for the two prebuilt BINARIES device/remora drops (bd remora-28ix.4): uinputd
# and vncserver both self-gate on a boot property Remora never emits, so they are
# runtime-dead — and each ships its own init rc, which must go with the binary or init spends the
# container's life retrying a service whose executable is absent. Listed here for the incremental
# case exactly as the apps above are: PRODUCT_PACKAGES_REMOVE drops them from a CLEAN build, and
# out/ still holds them after any other kind.
for bin in uinputd vncserver; do
  EXCLUDES="$EXCLUDES --exclude=vendor/bin/$bin --exclude=vendor/etc/init/$bin.rc"
done
EXCLUDES="$EXCLUDES --exclude=vendor/lib64/libvncserver.so* --exclude=vendor/lib/libvncserver.so*"
# libevdev rides out with uinputd, its only consumer (bd remora-ykhz). Same incremental-build
# reason as everything else in this list: PRODUCT_PACKAGES_REMOVE only takes effect on a CLEAN
# build, so without this line an incremental one leaves it in out/ and assemble copies it back.
#
# THE TRAILING GLOB IS LOAD-BEARING, and its absence is why the first attempt silently shipped the
# library anyway: the image installs the VERSIONED soname (vendor/lib64/libevdev.so.2) and NOT the
# unversioned name, so --exclude=.../libevdev.so matched nothing at all and tar copied the file in.
# Verified on the build that followed the drop — libevdev.so.2 was still in the image while uinputd,
# vncserver and libvncserver.so had all gone, which is what isolated the pattern as the difference.
# This is the same versioned-name trap bd remora-ykhz recorded for the shipped-prebuilt census; it
# bites exclusion patterns too, so match .so* rather than .so anywhere a prebuilt is being dropped.
EXCLUDES="$EXCLUDES --exclude=vendor/lib64/libevdev.so* --exclude=vendor/lib/libevdev.so*"
# The two ARM-only Vulkan ICDs device/remora drops (bd remora-ykhz): VideoCore and Adreno silicon
# exists only in ARM SoCs, and this image is x86_64-only, so no compatible machine can carry the
# GPU. EXACT names, deliberately not a glob: ICDs install unversioned (vulkan.<driver>.so, nothing
# else), so the versioned-name trap above does not apply — and a careless vulkan.* pattern here
# would take the load-bearing intel/radeon/virtio drivers with it.
EXCLUDES="$EXCLUDES --exclude=vendor/lib64/hw/vulkan.broadcom.so --exclude=vendor/lib64/hw/vulkan.freedreno.so"
# gralloc.cros retires WITH the Mesa-26 GL cluster and only with it (bd remora-ykhz.1): on those
# images its amdgpu backend cannot initialise (the DRI interface it dri_init()s against is gone)
# and gpu_config.sh translates cros away, keyed on the wrapper file. This exclusion uses the SAME
# key, read from the staged product tree, so the two halves cannot disagree: a mesa_source build
# drops the file on the incremental path exactly as mesa.mk's REMOVE does on the clean path, and
# a build without the payload keeps shipping real cros untouched.
if [ -f "$PROD/vendor/lib64/libgbm_mesa_wrapper.so" ]; then
  EXCLUDES="$EXCLUDES --exclude=vendor/lib64/hw/gralloc.cros.so"
  # gralloc.gbm goes on the SAME key for a different reason (bd remora-ykhz): not "cannot work
  # here" but "no longer needed here" — it is the last prebuilt holding libc++_shared, and
  # gpu_config.sh translates gbm -> minigbm_gbm_mesa on exactly these images. Excluded here as
  # well as PRODUCT_PACKAGES_REMOVEd in mesa.mk because REMOVE only bites on a CLEAN build, and
  # assemble copies whatever a previous incremental build already left in out/.
  EXCLUDES="$EXCLUDES --exclude=vendor/lib64/hw/gralloc.gbm.so"
fi
# DANGLING VA ALIASES, the same incremental-out/ shape as the 32-bit tree below and MEASURED the
# same way — the first image built after libgallium_drv_video was retired (bd remora-ykhz) shipped
# three of them: nouveau/r600/virtio_gpu_drv_video.so, each still pointing at the prebuilt that no
# longer exists. Suppressing the prebuilt's module stops it INSTALLING those links; it does not
# DELETE what a previous build already put in out/, and assemble copies the leftovers straight back.
#
# KEYED ON DANGLING-NESS, not on the payload, because a symlink with no target is never correct
# whatever produced it — and that is exactly the shape libva dlopens and fails on. -e FOLLOWS the
# link, so the aliases the payload does supply (radeonsi_drv_video.so -> libgallium_dri.so) resolve
# and are kept; only the broken ones are dropped. Names are not hardcoded: which drivers Mesa
# compiled in decides which aliases are real, and a hardcoded list would go stale the first time
# that set changed.
for _l in "$PROD"/vendor/lib64/dri/*_drv_video.so; do
  [ -L "$_l" ] || continue
  [ -e "$_l" ] && continue
  EXCLUDES="$EXCLUDES --exclude=vendor/lib64/dri/$(basename "$_l")"
  echo "=== dropping dangling VA alias $(basename "$_l") -> $(readlink "$_l") ==="
done
# THE WHOLE 32-BIT VENDOR LIB TREE, since the image went 64-bit-only (bd remora-ykhz). Same
# incremental-build reason as everything else here, but it bites harder for a dropped ARCH than for
# a dropped package: removing TARGET_2ND_ARCH stops soong INSTALLING 32-bit libraries, and deleting
# a module stops it installing its symlinks, but neither DELETES what a previous multilib build
# already put in out/. assemble then copies the leftovers straight back in.
#
# Measured, not assumed: the first 64-bit-only image shipped five DANGLING symlinks —
# vendor/lib/libdrm.so.2 and the four backend links — each pointing at a 32-bit library that no
# longer existed. They survived deleting the modules AND deleting the files by hand, because any
# build from an older out/ state re-materialises them.
#
# /system/lib is deliberately NOT excluded: AOSP genuinely installs four bionic stubs there
# (libc, libdl, libdl_android, libm) even on a 64-bit-only product, and they are in installed-files.
EXCLUDES="$EXCLUDES --exclude=vendor/lib/*.so --exclude=vendor/lib/*.so.[0-9]*"
# AOSP's own su, which userdebug builds install at /system/xbin/su mode 0755 — world-executable and
# on $PATH, so 'which su' resolves it from any app. Magisk CANNOT hide it: the denylist works by
# unmounting Magisk's modifications from a denylisted process, and this one ships in the image, so
# there is nothing to unmount. Every denylisted app has been able to see an executable su
# (bd remora-4ei.49).
# DROPPING IT COSTS NOTHING HERE, verified on the live A17 instance rather than assumed:
#   - it does not work. '/system/xbin/su 0 id' from adb shell -> "su: setgid failed: Operation not
#     permitted". It cannot elevate in this container, so nothing can be relying on it to.
#   - real root is Magisk's, at /sbin/su -> ./magisk in the root mount namespace, untouched by this.
#   - nothing in Remora invokes it (no `su` call site in src/ or vendor/guest-scripts), and the ADB
#     root toggle of bd remora-4ei.32.4 is about ro.adb.secure/adbd, not this binary.
# So it is a pure emulator/root tell with no compensating utility. Same mechanism as the exclusions
# above; a source build should also carry PRODUCT_PACKAGES_REMOVE += su, and this covers the
# assemble-only and incremental paths where out/ still holds an already-compiled copy.
EXCLUDES="$EXCLUDES --exclude=xbin/su --exclude=system/xbin/su"
docker run --rm -v "$PROD":/prod:ro -v "$DFDIR":/ctx:ro ubuntu:22.04 \
    tar -c --mode='go+rX' $EXCLUDES -C /ctx Dockerfile $parts_extra -C /prod $parts \
    | docker build -t "$TAG" -
rm -rf "$DF" "$DFDIR"
# Verify the promise end-to-end: the pre-flight guard can't catch an empty/half-harvested feature
# dir, so check the built image itself. docker create+cp inspects the layer without executing
# anything from the image.
case "$TAG" in
*-wv|*-wv-*)
  CID=$(docker create "$TAG")
  if docker cp "$CID:/vendor/lib64/libwvaidl.so" - >/dev/null 2>&1; then
    echo "=== verified: Widevine CDM present in $TAG ==="
    docker rm -f "$CID" >/dev/null
  else
    docker rm -f "$CID" >/dev/null
    echo "assemble: built $TAG but /vendor/lib64/libwvaidl.so is MISSING from the image" >&2
    exit 1
  fi ;;
esac
# Same end-to-end check for the arch stamp, and it earns its place for the same reason the sed's
# anchor grep does: a silently unapplied variant is this mechanism's characteristic failure, and it
# is invisible until an image SIGILLs on a machine that should have refused it.
if [ -n "$ARCH_VARIANT" ]; then
  GOT=$(docker image inspect --format '{{index .Config.Labels "remora.arch_variant"}}' "$TAG" 2>/dev/null)
  [ "$GOT" = "$ARCH_VARIANT" ] || {
    echo "assemble: built $TAG but its remora.arch_variant reads '$GOT', not '$ARCH_VARIANT'" >&2
    exit 1
  }
  echo "=== verified: $TAG declares arch variant $ARCH_VARIANT ==="
fi
echo "=== assembled $TAG ==="
