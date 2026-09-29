#!/usr/bin/env bash
# Check a built image before deploying it.
#
# WHY THIS EXISTS: a source build finished with exit 0, wrote its tag, and produced an
# image with NO radeon Vulkan driver at all — the prebuilt had been suppressed and the replacement
# was never installed, because one makefile could see the payload and another could not. Nothing in
# the build complained, because from the build's point of view nothing was wrong: it installed
# exactly the set it was asked for. A green build proves the build RAN, not that it produced what
# was asked for, and the gap between those two is where an unbootable or silently degraded image
# gets shipped to a host.
#
# Every check below is something that has actually been wrong here at least once, and each one
# failed SILENTLY when it was. This is not a general-purpose linter; it is a list of scars.
#
#   usage: verify-image.sh [image-tag] [expected-mesa-version]
#          expected-mesa-version is optional — given, the radeon ICD must report it; omitted, the
#          version is reported without being enforced (right for a build with mesa_source off).
set -uo pipefail

IMG="${1:-remora24:x86_64-gapps-wv-hwc2-broadwell-src}"
WANT_MESA="${2:-}"

fail=0
say()  { printf '  %s\n' "$*"; }
bad()  { printf '  FAIL: %s\n' "$*"; fail=1; }
warn() { printf '  warn: %s\n' "$*"; }

printf '\n=== verifying %s ===\n' "$IMG"
docker image inspect "$IMG" >/dev/null 2>&1 || { bad "no such image"; exit 1; }
say "created: $(docker image inspect -f '{{.Created}}' "$IMG" | cut -c1-19)"

cid=$(docker create "$IMG") || exit 1
tmp=$(mktemp -d)
trap 'docker rm -f "$cid" >/dev/null 2>&1; rm -rf "$tmp"' EXIT

# ONE export pass. The image is ~2.6 GB and exporting it per-check is the slow way to do this.
docker export "$cid" 2>/dev/null | tar -x -C "$tmp" \
    --wildcards 'vendor/lib64/hw/vulkan.*' 'vendor/lib64/hw/hwcomposer.*' \
                'vendor/lib64/hw/gralloc.*' 'vendor/lib64/gbm/*' \
                'vendor/lib64/dri/*' 'vendor/lib64/egl/*' 'vendor/etc/media_codecs*.xml' \
                'vendor/lib64/libc++_shared.so' 'vendor/lib64/lib*.so' \
                'system/lib64/libnb.so' 'system/bin/arm64/linker64' \
                'system/etc/init/remora-berberis.rc' \
                'system/etc/remora-spoof/nb_sigfix/*' 'system/build.prop' 2>/dev/null

# --- Vulkan ICDs -------------------------------------------------------------------------------
# An ICD that does not export HMI is dead weight: the Android loader dlopens it, fails to find the
# module struct, and silently never uses it. It installs perfectly and does nothing.
#
# KNOWN-BAD PREBUILTS ARE NOT REGRESSIONS. The prebuilt vulkan.nouveau.so has never
# exported HMI, so failing on it would make this script report FAIL on every image ever built and
# be worth nothing as a gate. It is warned about instead — the distinction being drawn is "this
# build broke something", not "something here is imperfect".
HMI_KNOWN_BAD="nouveau"

# THERE IS NO C++-RUNTIME KNOWN-BAD SET, and the fact that there used to be is the scar here.
# This script shipped with CXX_KNOWN_BAD="lvp pastel" on the reading that both were built against
# upstream libc++ (std::__1) while the image only carries the NDK's __ndk1 runtime, so neither could
# ever dlopen. That reading was WRONG (bd remora-yz8b), and it was wrong because the check below
# consulted the wrong runtime: THE IMAGE SHIPS BOTH.
#   /vendor/lib64/libc++.so         1049 std::__1  symbols   <- platform/vendor libc++
#   /vendor/lib64/libc++_shared.so  1043 std::__ndk1 symbols <- the NDK's
# DT_NEEDED splits the ICDs cleanly along provenance, and that split is the whole story:
#   lvp, pastel                          NEEDED libc++.so         <- built IN-TREE by this build
#   broadcom freedreno intel intel_hasvk
#   nouveau radeon virtio                NEEDED libc++_shared.so  <- NDK-built prebuilts
# So the two "unloadable" drivers are simply the two the platform builds itself (which is also why
# neither matched a prebuilt-project hash — the open question in that bead), and every one
# of their std::__1 references resolves against the libc++.so sitting next to them in
# /vendor/lib64. Verified on remora24:x86_64-gapps-wv-hwc2 (last known-good) and on
# -broadwell-src: all nine ICDs resolve every strong undefined symbol, zero missing.
# lvp is Mesa's lavapipe from external/mesa3d; pastel is SwiftShader-derived (its one weak
# undefined is _ZTHN2rr8Variable23unmaterializedVariablesE — rr:: is Reactor, SwiftShader's JIT).
#
# NOTHING WAS EVER BROKEN, so nothing is muted. Dropping either driver as "dead weight" would have
# been a real regression: remora.mk installs vulkan.pastel deliberately and gpu_config.sh's
# gpu_setup_guest sets ro.hardware.vulkan=pastel, i.e. it IS the software-path Vulkan driver.

printf '\n--- Vulkan ICDs ---\n'
icds=$(cd "$tmp" && ls vendor/lib64/hw/vulkan.*.so 2>/dev/null | sed 's|.*/vulkan\.||;s|\.so$||')
[ -n "$icds" ] || bad "no Vulkan ICDs at all — the image cannot render through Vulkan"
for d in $icds; do
    f="$tmp/vendor/lib64/hw/vulkan.$d.so"
    v=$(strings -a "$f" 2>/dev/null | grep -m1 -oE 'Mesa [0-9]+\.[0-9]+[0-9.]*')
    hmi=$(readelf --dyn-syms -W "$f" 2>/dev/null | grep -c ' HMI$')
    printf '  %-14s %-20s HMI=%s  %s\n' "$d" "${v:-<not Mesa>}" "$hmi" "$(du -h "$f" | cut -f1)"
    if [ "$hmi" != "1" ]; then
        # The exemption is scoped to the PREBUILT generation, not to the driver name: the
        # source-built NVK (Mesa 26.x, bd remora-ykhz) DOES export HMI — the first loadable
        # nouveau ICD this image family has ever shipped — so on a Mesa-26 nouveau a missing HMI
        # is a real regression, and a name-keyed exemption would warn it into the noise.
        case " $HMI_KNOWN_BAD " in
            *" $d "*) case "$v" in
                          "Mesa 26"*) bad "vulkan.$d.so ($v) exports no HMI — the source build regressed; the loader will never use it" ;;
                          *)          warn "vulkan.$d.so exports no HMI — known-bad prebuilt, not a regression" ;;
                      esac ;;
            *)        bad  "vulkan.$d.so exports no HMI — the loader will never use it" ;;
        esac
    fi
done

# The two ARM-only ICDs device/remora drops (bd remora-ykhz): VideoCore and Adreno exist only in
# ARM SoCs, so no machine that can run this x86_64-only image can carry the GPU. Asserted here
# because the drop has TWO halves (PRODUCT_PACKAGES_REMOVE + the assemble exclusion) and only the
# exclusion covers an incremental build — a reappearance means one half was lost.
for d in broadcom freedreno; do
    if [ -e "$tmp/vendor/lib64/hw/vulkan.$d.so" ]; then
        bad "vulkan.$d.so is BACK in the image — the drop lost one of its two halves (remora.mk PRODUCT_PACKAGES_REMOVE / assemble-image.sh exclusion)"
    else
        say "vulkan.$d.so absent, as intended (ARM-only silicon, dropped)"
    fi
done

# --- the gbm allocation path, Mesa-26 images only (bd remora-ykhz.1) ---------------------------
# Conditioned on the GL cluster being the source build, because these files ship with it and an
# older image legitimately has none of them. Three invariants, each with a distinct failure:
#  - libgbm without its runtime backend fails gbm_create_device for EVERY consumer, gralloc.gbm
#    (the image default fallback) included — the shape that shipped silently dead once;
#  - a missing wrapper strands gralloc.minigbm_gbm_mesa on every non-venus host, and the
#    gpu_config cros translation (keyed on these files) silently stops firing;
#  - gralloc.cros present here is a retirement half lost — it cannot initialise on this Mesa and
#    anything still selecting it black-screens at allocator init, far from the cause.
# NOT grep -q, and the reason is a silent-skip trap this gate fell into on its first run: this
# script sets pipefail, grep -q exits at its first match, strings dies on SIGPIPE (141), and the
# pipeline — match found and all — evaluates FALSE. The whole section then self-skips exactly as
# if the image were pre-Mesa-26, printing nothing, and PASS covers it. A plain grep >/dev/null
# reads the stream to the end, so the pipeline's status is grep's own.
if strings -a "$tmp/vendor/lib64/dri/libgallium_dri.so" 2>/dev/null | grep "Mesa 2[6-9]" >/dev/null; then
    for f in gbm/dri_gbm.so libgbm_mesa_wrapper.so; do
        if [ -e "$tmp/vendor/lib64/$f" ]; then
            say "$f present (gbm path complete)"
        else
            bad "$f MISSING on a Mesa-26 image — gbm_create_device fails for every gbm consumer (see mesa.mk's cluster note)"
        fi
    done
    if [ -e "$tmp/vendor/lib64/hw/gralloc.cros.so" ]; then
        bad "gralloc.cros.so still ships on a Mesa-26 image — it cannot initialise here and its retirement lost a half (mesa.mk REMOVE / assemble exclusion)"
    else
        say "gralloc.cros.so absent, as intended (unservable on Mesa 26; gpu_config translates)"
    fi
fi

# --- can the loader actually OPEN these? --------------------------------------------------------
# EXISTS, RIGHT VERSION AND EXPORTS HMI ARE ALL TRUE OF A DRIVER THAT CANNOT BE dlopen'd. That is
# not hypothetical: a 26.1 radeon ICD passed all three, shipped to amd-host-a, and failed with
#   cannot locate symbol "_ZNSt6__ndk122__libcpp_verbose_abortEPKcz"
# because it was built against NDK r27c's libc++ while the image ships an older libc++_shared.so.
# The missing question was never "is the driver right" but "does this IMAGE satisfy it", so ask it
# against the image's own libraries — here, statically, before a 2.6 GB transfer rather than after.
printf '\n--- dynamic linkage (does the image satisfy these drivers?) ---\n'
# RESOLVE AGAINST BOTH RUNTIMES IN /vendor/lib64, and only those. A vendor .so loads in the
# sphal/vendor linker namespace, whose search path is /vendor/lib64 — so libc++.so and
# libc++_shared.so sitting there are exactly what it can bind, and /system/lib64/libc++.so is NOT
# reachable no matter what it exports. The first version of this check listed the system path and
# omitted the vendor libc++.so, which was wrong twice over: the lenient path is unreachable at
# runtime, and the reachable one was never consulted. It also never EXTRACTED system/lib64, so that
# arm was dead code and the effective list was libc++_shared.so alone — which is why the two
# in-tree ICDs (NEEDED libc++.so) were reported unloadable and then muted. See the note above.
cxx_libs=$(ls "$tmp"/vendor/lib64/libc++.so "$tmp"/vendor/lib64/libc++_shared.so 2>/dev/null)

# nm -D, NOT readelf field positions. readelf renders an STT_GNU_IFUNC symbol's type as
# "<OS specific>: 10" — three whitespace-separated tokens where every other row has one — which
# shifts Bind/Vis/Ndx/Name by two and makes $7/$8 read the wrong columns. bionic exports memcpy,
# __memcpy_chk, wmemchr and friends as IFUNCs, so a positional parse silently mis-reads them. That
# cost an hour of chasing three "missing" libc symbols that libc plainly exports.
exports_sym() {
    for c in $cxx_libs; do
        nm -D --defined-only "$c" 2>/dev/null \
            | awk -v S="$1" '{n=$NF; sub(/@@?.*/,"",n); if (n==S) {found=1}} END{exit !found}' && return 0
    done
    return 1
}
if [ -z "$cxx_libs" ]; then
    warn "no libc++ found in /vendor/lib64 — cannot check C++ runtime resolution"
else
    for so in "$tmp"/vendor/lib64/hw/vulkan.*.so; do
        [ -f "$so" ] || continue
        b=$(basename "$so")
        # STRONG undefined only ("U"); weak undefined ("w") never blocks dlopen — the linker leaves
        # it null and the caller is expected to test it. Mesa's per-driver dispatch tables are
        # thousands of weak refs (anv_*, radv's metro_exodus_*, vk_common_*), so counting weak as
        # required reports EVERY Mesa ICD, including the ones running in production, as unloadable.
        #
        # @LIBC-versioned symbols are bionic's, satisfied by libc.so. __cxa_atexit and
        # __cxa_finalize look like C++ runtime symbols and are not; matching them reported a
        # healthy driver as broken during development of this very check.
        missing=""
        for s in $(nm -D -u "$so" 2>/dev/null \
                   | awk '$1=="U" {n=$NF; if (n !~ /@LIBC/ && n ~ /^_ZNSt|^_ZSt|^_ZTISt|libcpp/) {sub(/@@?.*/,"",n); print n}}'); do
            exports_sym "$s" || missing="$missing $s"
        done
        if [ -n "$missing" ]; then
            bad "$b needs C++ runtime symbols this image does not export — it will fail to dlopen:"
            for s in $missing; do printf '        %s\n' "$s"; done
        else
            say "$b — C++ runtime resolves"
        fi
    done
fi

# --- the ones that shipped wrong ---------------------------------------------------------------
printf '\n--- checks that failed silently before ---\n'

# 1. AMD hosts need this driver to EXIST. Removing a prebuilt and installing nothing in its place
#    is worse than leaving the old one, and that is exactly what shipped.
rad="$tmp/vendor/lib64/hw/vulkan.radeon.so"
if [ -f "$rad" ]; then
    rv=$(strings -a "$rad" | grep -m1 -oE 'Mesa [0-9]+\.[0-9]+[0-9.]*')
    if [ -n "$WANT_MESA" ]; then
        case "$rv" in
            *"$WANT_MESA"*) say "vulkan.radeon.so is the source build (${rv})" ;;
            *) bad "vulkan.radeon.so reports '${rv:-unknown}', expected Mesa $WANT_MESA — mesa_source did not take effect" ;;
        esac
    else
        say "vulkan.radeon.so present: ${rv:-unknown} (version not enforced)"
    fi
else
    bad "vulkan.radeon.so is ABSENT — nothing will render Vulkan on an AMD host"
fi

# 2. THE GL/DRI CLUSTER MOVES TOGETHER OR NOT AT ALL. The DRI loader ABI changed between the
#    prebuilt 24.0.8 and 26.x: the old loader wants __driDriverGetExtensions_<driver> and Mesa 26
#    exports none of them. A mixed pair links, installs, and fails at RUNTIME.
printf '\n--- GL/DRI cluster ---\n'
# ONLY COMPARE VERSIONS THAT EXIST. Not every member stamps one — libEGL_mesa.so carries no "Mesa
# x.y" string at all — and treating "no version string" as a version made the first draft of this
# report a MIXED cluster on a perfectly consistent image. A check that cries wolf gets ignored,
# which costs more than not having it.
gl_ver=""; gl_from=""
for f in "$tmp/vendor/lib64/dri/libgallium_dri.so" "$tmp/vendor/lib64/dri/libgallium_drv_video.so" \
         "$tmp/vendor/lib64/egl/libEGL_mesa.so" "$tmp/vendor/lib64/egl/libGLESv2_mesa.so"; do
    [ -f "$f" ] || continue
    v=$(strings -a "$f" 2>/dev/null | grep -m1 -oE 'Mesa [0-9]+\.[0-9]+[0-9.]*')
    say "$(basename "$f"): ${v:-no version string}"
    [ -n "$v" ] || continue
    if [ -z "$gl_ver" ]; then
        gl_ver="$v"; gl_from="$(basename "$f")"
    elif [ "$v" != "$gl_ver" ]; then
        bad "GL cluster is MIXED — $gl_from is $gl_ver but $(basename "$f") is $v; the DRI loader ABI will not match at runtime"
    fi
done
[ -n "$gl_ver" ] || warn "no GL/DRI member carried a version string to compare"

# 3. Remora's composer is the only one (bd remora-28ix.4.3). The closed prebuilt it replaced
#    coming back would mean a profile pinning hwcomposer= to it silently gets that HAL again.
printf '\n--- other regressions worth catching ---\n'
others=""
for hwc in "$tmp"/vendor/lib64/hw/hwcomposer.*.so; do
    [ -e "$hwc" ] || continue
    [ "${hwc##*/}" = hwcomposer.remora.so ] || others="$others ${hwc##*/}"
done
if [ -n "$others" ]; then
    bad "a second hwcomposer HAL is in the image: $others"
else
    say "hwcomposer.remora.so is the only composer, as intended"
fi

# 3b. THE VA DRIVER, because the project that builds it is not in any manifest (bd remora-28ix.7).
#     iHD_drv_video.so is what every hardware decode and encode path goes through, and it comes from
#     external/intel-media-driver — an UNMANAGED directory: AOSP removed the project in Android 17,
#     so the A16 checkout was copied across by hand. Nothing in the repo or the manifest recreates
#     it. A clean tree sync therefore produces a build with no VA driver, and the build does NOT
#     fail: the module simply is not defined and the file is never installed. That is the same
#     silent shape as the radeon hole (bd remora-bm7.14) and the media_codecs.xml drop below, and
#     until the project is made reproducible this check is the only thing standing in front of it.
# 3a. ARM TRANSLATION, IN BOTH HALVES AND ACTUALLY PATCHED (bd remora-rve, bd remora-28ix.7).
#     The payload has a guest half (bin/arm64 + lib64/arm64) and a host half (lib64/libnb.so, what
#     ART dlopens via ro.dalvik.vm.native.bridge). assemble-image.sh patches the prebuilts only
#     `if` both are present — with no else — so a missing half means the patch pass SILENTLY does
#     nothing. That has happened: on A17 the WITH_NDK module path is skipped, which once dropped
#     libnb.so entirely and every ARM app died with "UnsatisfiedLinkError ... is for EM_AARCH64
#     instead of EM_X86_64", a message that names nothing useful.
#     So: absent-together is a legitimate arm_translate=off image and merely reported; one half
#     without the other is the broken state and fails; and when both are there the byte patch is
#     verified rather than assumed, because an UNPATCHED libnb segfaults on every guest dlopen.
_nb="$tmp/system/lib64/libnb.so"; _guest="$tmp/system/bin/arm64/linker64"
if [ ! -f "$_nb" ] && [ ! -f "$_guest" ]; then
    say "ARM translation not in this image (arm_translate off)"
elif [ ! -f "$_nb" ]; then
    bad "guest ARM payload is present but /system/lib64/libnb.so is MISSING — ART cannot load the native bridge; every ARM app fails with UnsatisfiedLinkError (bd remora-rve)"
elif [ ! -f "$_guest" ]; then
    bad "libnb.so is present but the guest payload (/system/bin/arm64/linker64) is MISSING — nothing for the translator to run"
else
    # 0x35551e is the guest-SP assertion site patch-arm-translation.py neuters (Fix 3); patched it
    # reads 6690, pristine 753b. One site is enough to prove the pass ran on this image.
    _sp=$(dd if="$_nb" bs=1 skip=$((0x35551e)) count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
    case "$_sp" in
        6690) say "ARM translation present and patched (libnb.so + guest payload)" ;;
        753b) bad "libnb.so is present but NOT PATCHED (0x35551e reads 753b) — the patch pass was skipped; guest dlopen will segfault (bd remora-rve)" ;;
        *)    bad "libnb.so at 0x35551e reads '$_sp', neither patched (6690) nor pristine (753b) — the prebuilt was re-harvested and patch-arm-translation.py's offsets are stale" ;;
    esac
    # The berberis flags rc rides the same overlay (bd remora-4ei.100): without it, Amazon dies
    # in ~5 s and NIKKE in minutes, so an image with translation but no rc has silently lost a
    # fix. Checked for content, not just existence — an rc with the wrong flag names would set
    # nothing and libnb only logs the unknown token.
    _rc="$tmp/system/etc/init/remora-berberis.rc"
    if [ ! -f "$_rc" ]; then
        bad "berberis flags rc (/system/etc/init/remora-berberis.rc) is MISSING — Amazon/NIKKE lose the accurate-sigsegv mitigation (bd remora-4ei.100)"
    elif ! grep -q 'ro\.berberis\.flags accurate-sigsegv$' "$_rc"; then
        bad "berberis flags rc present but does not set exactly 'accurate-sigsegv' — the other precise-state flags do NOT exist in this libnb.so and are dropped as unrecognised (bd remora-4ei.100)"
    else
        say "berberis flags rc present (accurate-sigsegv)"
    fi
    # nb_sigfix Zygisk module (bd remora-4ei.99) — TikTok's deterministic :42 fix. Baked under
    # remora-spoof only when the ReZygisk stack is also present (it needs Zygisk to load), so this
    # is checked but not required: an ARM-translation image built without play_spoof legitimately
    # has no way to run the module and simply reports its absence.
    _nbmod="$tmp/system/etc/remora-spoof/nb_sigfix/zygisk/x86_64.so"
    if [ -d "$tmp/system/etc/remora-spoof" ]; then
        if [ ! -f "$_nbmod" ]; then
            bad "ReZygisk stack is baked but the nb_sigfix module is MISSING — TikTok's deterministic guest-frame crash returns AND Amazon drops to ~3/8 cold starts (bd remora-4ei.99, bd remora-4ei.100)"
        elif ! grep -aq 'zygisk_module_entry' "$_nbmod" 2>/dev/null; then
            bad "nb_sigfix module is present but exports no zygisk_module_entry — it will not load (bd remora-4ei.99)"
        else
            say "nb_sigfix Zygisk module present (TikTok signal + Amazon affinity)"
        fi
    else
        say "nb_sigfix not applicable (no ReZygisk stack in this image)"
    fi
fi

# x86_64 MUST BE THE PREFERRED ABI, AND arm64-v8a MUST STILL BE THERE (bd remora-4ei.100).
#
# Play picks which native ABI an app installs as by walking this list IN ORDER, so its first entry
# decides how every app on the device runs. Measured with x86_64 first: 16 of 21
# installed third-party apps resolve to primaryCpuAbi=x86_64, and the only 5 that fall back to
# translation are the ones whose developers publish arm64-only builds (verified — Play delivered
# split_config.x86_64.apk for YouTube and split_config.arm64_v8a.apk for NIKKE on the SAME DAY,
# same device, same account).
#
# Put x86_64 anywhere but first and every app silently reinstalls as arm64 on its next update:
# slower, and newly exposed to the berberis guest-signal defect that still breaks Amazon and makes
# NIKKE flaky. NOTHING would report that — the apps keep working, just worse — which is exactly the
# shape this script exists to catch, and the same trap that let a stale primaryCpuAbi outlive an
# image change before.
#
# arm64-v8a staying in the list is NOT a leftover: it is what makes the arm64-only apps installable
# at all. Drop it and Play does not serve x86_64 instead, it marks them incompatible and they
# disappear from the store. So this checks the ORDER, and that both entries are present.
_abi=$(grep -hm1 '^ro\.system\.product\.cpu\.abilist=' "$tmp/system/build.prop" 2>/dev/null | cut -d= -f2)
case "$_abi" in
    "")       bad "ro.system.product.cpu.abilist is MISSING from build.prop — nothing determines which ABI Play serves (bd remora-4ei.100)" ;;
    x86_64,*arm64-v8a*)
              say "abilist prefers x86_64, arm64 fallback intact ($_abi)" ;;
    x86_64)   bad "abilist is 'x86_64' with NO arm64-v8a — the arm64-only apps (TikTok/NIKKE/Amazon/Sonar/Nintendo) become UNINSTALLABLE, not x86_64 (bd remora-4ei.100)" ;;
    *x86_64*) bad "abilist is '$_abi' — x86_64 is present but NOT FIRST, so Play serves arm64 and every app silently installs translated (bd remora-4ei.100)" ;;
    *)        bad "abilist is '$_abi' — no x86_64 at all, so every app installs as translated arm64 (bd remora-4ei.100)" ;;
esac

# THREE LEGITIMATE HARDWARE-VIDEO PROVIDERS, and the check must know all of them. The first two
# were measured by FAILing a healthy broadwell image: the alderlake profile installs iHD
# (external/intel-media-driver — unmanaged, bd remora-28ix.7, which is what this check exists to
# catch losing), while the AMD-profile images have NEVER shipped iHD — the rollback tags all lack
# it — and reach hardware video through the gallium VA frontend with va_driver=radeonsi.
#
# THE THIRD ARRIVED (bd remora-ykhz) and FAILed a healthy image the same way, which is
# why it is spelled out rather than folded in: Mesa 26 builds NO standalone drv_video at all. The VA
# frontend lives INSIDE libgallium_dri.so and is reached through <driver>_drv_video.so symlinks
# beside it, so a source-built AMD image has radeonsi_drv_video.so and no libgallium_drv_video.so.
#
# ONLY dri/ IS SEARCHED, deliberately — that is where libva looks. The AMD profile leaves iHD at
# vendor/lib64/iHD_drv_video.so (it does not enable the intel_media_driver_dri_path patch), and a
# driver outside dri/ is never loaded, so finding one there must NOT count as a provider.
# FAIL only when none is present: that is the no-hardware-video image, whatever the profile.
if [ -f "$tmp/vendor/lib64/dri/iHD_drv_video.so" ]; then
    ver=$(strings -a "$tmp/vendor/lib64/dri/iHD_drv_video.so" 2>/dev/null |
          grep -m1 -oE 'Intel iHD driver for Intel\(R\) Gen Graphics - [0-9.]+' |
          sed 's/.*- //')
    say "iHD_drv_video.so present${ver:+ (}${ver}${ver:+)}"
elif [ -f "$tmp/vendor/lib64/dri/libgallium_drv_video.so" ]; then
    say "iHD absent, libgallium_drv_video.so present — the standalone gallium VA driver (pre-Mesa-26 payloads and the prebuilt)"
else
    # -e follows the symlink, so a DANGLING alias is skipped rather than counted: an alias whose
    # target is missing is precisely the shape libva dlopens and fails on, and counting it would
    # turn this check into the thing it exists to prevent.
    va_alias=""
    for f in "$tmp"/vendor/lib64/dri/*_drv_video.so; do
        [ -e "$f" ] || continue
        va_alias="$f"; break
    done
    if [ -n "$va_alias" ]; then
        say "iHD absent, $(basename "$va_alias") -> $(readlink "$va_alias" || echo '(regular file)') — the Mesa-26 megadriver VA path (va_driver=radeonsi profiles)"
    else
        bad "NO hardware video provider — no iHD_drv_video.so (external/intel-media-driver is unmanaged, bd remora-28ix.7; a clean tree sync loses it silently), no libgallium_drv_video.so, and no resolvable <driver>_drv_video.so alias in vendor/lib64/dri"
    fi
fi

# 4. media_codecs.xml is the top-level list every other codec list hangs off. Dropping it with an
#    unrelated inherit once made the image advertise NO codecs and killed the mirror at session
#    setup — the failure that produced rule 1 in CLAUDE.md.
if [ -f "$tmp/vendor/etc/media_codecs.xml" ]; then
    say "media_codecs.xml present ($(wc -l < "$tmp/vendor/etc/media_codecs.xml") lines)"
else
    bad "vendor/etc/media_codecs.xml MISSING — the image will advertise no codecs and the mirror will die at session setup"
fi

printf '\n'
if [ "$fail" = 0 ]; then printf '  RESULT: PASS\n\n'; else printf '  RESULT: FAIL — DO NOT DEPLOY\n\n'; fi
exit "$fail"
