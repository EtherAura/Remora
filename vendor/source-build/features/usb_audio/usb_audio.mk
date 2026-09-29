# USB microphone (and USB audio output) for the Remora image — bd remora-4ei.37.
#
# NOTHING HERE IS A HAL WE WROTE, exactly as with the external camera (bd remora-4ei.9). AOSP
# already ships a tinyalsa-backed USB audio HAL at hardware/libhardware/modules/usbaudio, and
# android.hardware.audio@7.0-impl.so — already running in this image — is precisely the HIDL
# wrapper that dlopens legacy audio.<module>.default.so. The image simply never selected it.
#
# THE SYMPTOM THIS FIXES IS SILENCE, NOT AN ERROR, which is why it went unnoticed for so long.
# Android already advertises an input: dumpsys media.audio_policy lists "Built-In Mic"
# (AUDIO_DEVICE_IN_BUILTIN_MIC), so AudioSource.MIC resolves and an AudioRecord succeeds. But that
# device is served by audio.primary.default, which is AOSP's STUB — hardware/libhardware/modules/
# audio/audio_hw.c in_read() does memset(buffer, 0, bytes) after a usleep() that fakes the pacing
# of a real device. An app therefore records a perfectly timed stream of zeros and has nothing to
# report. Adding a real HAL is the whole fix.
#
# THE OTHER TWO STEPS THIS BEAD ORIGINALLY LISTED DO NOT EXIST HERE:
#  - /dev/snd is already inside the container (it runs privileged), including the capture nodes
#    pcmC3D0c and pcmC4D0c, and Android sees every card in /proc/asound. No bringup work.
#  - Host->guest transport was VM-ONLY and the vm backend was deleted in bd remora-d5v. On bare
#    the container runs on the same host the microphone is plugged into.
# NO LOCAL_PATH: this payload copies nothing out of its own directory any more. Every module below
# is an AOSP one selected by name, and the one file that used to be copied from here — a top-level
# audio policy — is gone with the approach that needed it (see the note further down).

# The HAL itself, plus the policy fragment that describes its ports. Both are AOSP modules;
# usb_audio_policy_configuration.xml lives in the frameworks/av/services/audiopolicy/config soong
# namespace, which device-remora/remora.mk already registers and already draws five other configs
# from — so no new namespace is needed.
PRODUCT_PACKAGES += \
    audio.usb.default \
    usb_audio_policy_configuration.xml \

# tinycap, so the capture path can be proven from a shell without an app in the way. This is the
# audio counterpart of the synthetic v4l2loopback source used to isolate the camera's image half
# from its transport half (bd remora-4ei.9): if tinycap records a non-silent WAV from card 3 but an
# app still hears nothing, the fault is above the HAL, and vice versa. Debugging silence with no
# instrument tells you nothing about which layer failed.
PRODUCT_PACKAGES += tinycap

# THE DECLARATION THAT MAKES ALL OF THE ABOVE DO ANYTHING IS NOT HERE — it is the
# `usb_audio_policy` SOURCE PATCH, which adds one <xi:include> to AOSP's
# audio_policy_configuration_generic.xml. AudioPolicyManager opens exactly the modules the
# top-level policy includes, and the generic file lists only primary and r_submix, so without that
# patch everything installed here is present and never opened. The feature declares the patch in
# requiresPatches, so a profile that enables one without the other is reported rather than shipped.
#
# TWO EARLIER ATTEMPTS LIVED ON THIS LINE AND BOTH COST A FULL BUILD (bd remora-4ei.37). Recorded
# so neither is retried:
#
#  1. PRODUCT_PACKAGES_REMOVE += audio_policy_configuration.xml, paired with a PRODUCT_COPY_FILES
#     of our own top-level policy. That variable DOES NOT EXIST in this build system — `grep -r
#     PRODUCT_PACKAGES_REMOVE build/make/` is empty — so it was an assignment nothing ever read,
#     and the build died on the collision its own comment claimed to prevent:
#         build/make/core/Makefile:139: error: overriding commands for target
#         `out/target/product/remora_x86_64/vendor/etc/audio_policy_configuration.xml'
#
#  2. Moving the claim into remora.mk behind $(wildcard vendor/usb_audio/usb_audio.mk). That
#     WORKED as product config — the dumped PRODUCT_PACKAGES no longer contained the module — and
#     the build failed identically, because soong emits an install rule for every prebuilt_etc in
#     a REGISTERED NAMESPACE regardless of PRODUCT_PACKAGES, and remora.mk registers
#     frameworks/av/services/audiopolicy/config. Proven on a2dp_audio_policy_configuration.xml:
#     an install rule, no PRODUCT_PACKAGES entry, required by nothing in the tree.
#
# So NO PRODUCT_COPY_FILES onto that destination, ever. Patching the module's own src is the only
# route that keeps one rule for one target.

# NO FEATURE DECLARATION HERE, deliberately — this is where the camera feature needed one and
# this one does not. There is no android.hardware.microphone.xml in frameworks/native/data/etc at
# all; the feature is declared inside the core hardware sets (handheld_core_hardware.xml and its
# pc/tablet/car/wearable siblings), which this product already inherits. Confirmed on the live
# image: `pm list features` ALREADY reports android.hardware.microphone, with only the stub HAL
# installed. Adding a copy line would have named a source that does not exist, which fails the
# build outright — the same way the camera feature's first draft broke on a non-existent
# android.hardware.camera.any.xml. camera.external genuinely is absent from the core sets, which
# is why that feature needs its declaration and this one must not have one.
#
# Note what that means for the symptom: the feature bit has always been true, so apps have always
# believed a microphone was present. Nothing about app-visible capability changes here — only
# whether the samples are real.
