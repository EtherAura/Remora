# The image mirror: which tags may be published, and what each one obliges

Companion to the licensing audit in [ADR-0001](adr/0001-own-the-stack.md). The ADR decides the
*policy*; this file applies it to the actual tag registry (`imageTags()` in
`src/core/Features.cpp`) and states the result tag by tag, because "audited" has to mean a specific
list rather than a principle.

**Nothing here is published yet.** This is the register a publish step must satisfy first.

## The policy, in one paragraph

ADR-0001 decided to publish **both** flavours — audited clean images *and* GApps/Widevine
variants — while keeping `vendor/fetch-payloads.sh` for anyone who would rather add the proprietary
parts on their own machine. That is a deliberate acceptance of the same exposure OpenGApps
and every ROM mirror carry: Google licenses no redistribution of GApps, and the Widevine
CDM's origin licenses none either. It is tolerated, not permitted, and it is a risk decision the
project owner took with the facts in front of them.

Two things do **not** ride under that decision, and the difference is the whole point of this file:

* **GPL obligations are not exposure, they are conditions.** GPLv3 permits redistribution outright
  *provided* corresponding source is offered. That is satisfiable, so it must simply be satisfied —
  see the source-offer table below. This is why Magisk had to move to upstream first (a fork whose
  source had vanished made the obligation unmeetable at any price).
* **A component with no licence at all cannot be published under any framing.** Shamiko is
  closed-source freeware; no licence grants redistribution, so no image containing it may be
  mirrored. Not as accepted exposure, not with a disclaimer.

## The audited register

The Android 17 (`remora24:*`) tags, checked against the registry. "Exposure" = proprietary,
published only under the accepted-exposure decision.

| tag | adds over the row above | exposure | publishable |
|---|---|---|---|
| `remora24:x86_64` | base: mirror agent, system updater, LineageOS frameworks, sensors HAL, container-compat patches, Magisk, **arm_translate** | `arm_translate` | **yes** |
| `remora24:x86_64-gapps` | `gapps`, `play_spoof` | + GApps | **yes** |
| `remora24:x86_64-gapps-wv` | `widevine_l3` | + Widevine CDM | **yes** |
| `remora24:x86_64-gapps-wv-hwc2` | `hw_video_decode` (c2-va, Remora's own) | unchanged | **yes** |

**All four are publishable.** That reverses ADR-0001's earlier reading, which concluded the
publishable set was empty — a conclusion that rested entirely on `magisk_root`, which every tag
carries and whose source had no home. Moving to upstream Magisk v30.7 resolved it.

Two findings worth keeping, because both were assumed the other way at some point:

* **`play_spoof` does not imply Shamiko.** `shamiko` is a separate, default-off feature that
  *requires* `play_spoof`, not the reverse, and no registered tag provides it. A test
  (`publishableTagsCarryNoUnredistributableComponent` in `tests/test_gating.cpp`) fails the moment
  a `remora24:*` tag does. What `play_spoof` actually bakes is ReZygisk + PlayIntegrityFork, both
  GPL-3.0 with live upstreams and both clear.
* **`arm_translate` is the only exposure in the base tag**, and it is the easiest to miss: it is
  neither Google-branded nor opt-in. A genuinely exposure-free image is buildable — nothing blocks
  it — but it does not exist today, needs its own tag, and is **materially less capable**, because
  without the ARM native bridge arm64-only apps do not run at all. If that tag is ever built, say
  that on the download page rather than letting it be discovered.

## What each published tag obliges

A publish step that omits these is not a licensing grey area, it is non-compliance.

| component | licence | carried by | what satisfying it means |
|---|---|---|---|
| Magisk 30.7 | GPLv3 | all four tags | offer `github.com/topjohnwu/Magisk` tag `v30.7` |
| ReZygisk | GPL-3.0 | `-gapps` and above (`play_spoof`) | the pinned commit (`e42886f4`) plus Remora's source patch in `vendor/source-build/features/zygisk_pif/upstream/`, both public; the 64-bit binaries are built from that source by `vendor/host-prereqs/rezygisk/build-rezygisk.sh` |
| PlayIntegrityFork | GPL-3.0 | `-gapps` and above (`play_spoof`) | record the release shipped with the payload; unmodified |
| AOSP / LineageOS | Apache-2.0 | all four tags | attribution — `NOTICE` carries it |

**There is no kernel obligation.** A container shares the host's kernel: a Remora image contains no
kernel image and no kernel modules, so there is no GPLv2 kernel source to offer with it (see
ADR-0001's licensing audit).

## `index.txt`

The mirror's index must say **what each tag carries**, not merely list tags: a user choosing between
a clean image and a GApps one is making the same licensing choice this document just made, and
should be able to see it. Publish this verbatim, updating it from the register above — never
generate it from `Features.cpp` alone, since the registry knows what a tag *contains* but not what
that obliges.

```
# Remora image mirror — index
# Every image below is built from source; see NOTICE for attribution and the source offers
# required by the GPL components they carry (Magisk in all of them; ReZygisk and
# PlayIntegrityFork in the -gapps images).
#
# ALL tags include an ARM native bridge (arm_translate), which is proprietary
# Intel/Google. An image without it would not run arm64-only apps at all.

remora24:x86_64                  Base. No Google apps, no DRM.
remora24:x86_64-gapps            + Google apps and Play services. + Play attestation spoofing.
remora24:x86_64-gapps-wv         + Widevine L3 CDM (DRM video, SD only).
remora24:x86_64-gapps-wv-hwc2    + VA-API hardware video.

# Not mirrored, by policy rather than by omission:
#   any image containing Shamiko — closed-source freeware, no redistribution licence.
```

## Before the first publish

1. Re-run this audit against `Features.cpp`. Tags gain features; a feature added to a tag can change
   its verdict, and only the Shamiko check above is enforced automatically.
2. Confirm the source offers above are actually reachable from the download page.
3. Publish `index.txt` beside the images, not in the repo only — its purpose is to inform the person
   choosing an image.
