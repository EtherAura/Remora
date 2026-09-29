# ADR-0001: Own the stack — absorb the Android bringup, replace scrcpy end to end

**Status:** Accepted, and implemented — the P0 spine shipped and is in daily use (bd remora-28ix)
**Record:** bd remora-28ix.1
**Deciders:** the maintainer

> **Reading this record.** It keeps its history: statements that later work overtook are marked
> **Superseded** or **Update** in place, with what is true now, rather than rewritten away.

## Context

Remora stood on two upstreams, both effectively dead to us:

- **The upstream device tree** — inactive. The image was already built from source off a pinned
  manifest, with the device tree synced from our own fork at a pinned SHA, so upstream inactivity
  cost us nothing *operationally* — but every asset still carried the upstream's name and shape,
  and nothing new would ever come down that pipe.
- **scrcpy** — we ran a fork (`remora-lowlatency`, scrcpy 4.0 + 37 commits): its client on the
  desktop, its server jar pushed to the device per session. Tracking upstream meant rebasing a
  divergent fork forever; not tracking meant owning a codebase written for *someone else's*
  Android — hidden-API reflection, version gates, and a push-per-session bring-up, all of which
  exist only because stock scrcpy cannot assume anything about the device. We build the device.

The trigger was the decision to **release Remora publicly**: a coherent identity and independence
from dead upstreams, not a legal necessity (both upstreams are Apache-2.0; forks may ship with
attribution).

## Decision

Own the stack — with **scope honesty** about what "own" means:

| Piece | "Own" means | Bead |
|---|---|---|
| AOSP/LineageOS base | **Stays.** It IS Android; the pinned manifest + source patches already freeze it under our control. "From scratch" was never on the table here. | — |
| Mirror client | In-house `remora mirror` (Qt/FFmpeg, `src/mirror/`) replaces scrcpy's client | remora-28ix.2 ✅ |
| Device half | In-image **Remora agent** (`agent/`, a real init service baked by the `mirror_agent` build feature) replaces the pushed server jar | remora-28ix.3 ✅ |
| Wire protocol | Versioned Remora protocol: v2 connection roles + session kinds over v1's byte layouts (control/device messages, stream framing — deliberately reused, `docs/MIRROR_PROTOCOL.md` kept as their spec) | remora-28ix.3 ✅ |
| Device tree | Remora-owned `device/remora` **replacing** the upstream device tree (not forked): the product `remora_x86_64`, the A17 bringup fixes, the vendor media config, sepolicy and overlays | remora-28ix.4 R1 ✅ |
| Vendor project | Remora-owned `vendor/remora` replacing the upstream vendor project: the container init rc, `gpu_config.sh`, the boot-property adapter, and the three native modules (`remora_binder_alloc`, `remora_ipconfigstore`, `gralloc.remora`) | remora-28ix.4 R2 ✅ (recorded as "built, not yet booted"; every image since boots on it) |
| Boot namespace | `androidboot.remora_*` is Remora's own contract. **Update:** every Remora-owned consumer now reads `ro.boot.remora_*` directly; the adapter (`remora.props.rc`) is left mapping only onto AOSP's own property names (`ro.sf.lcd_density`, `ro.hardware.vulkan`) | remora-28ix.4 ✅ |
| Display HAL | The upstream hwcomposer was a closed prebuilt with **no source anywhere** — the last inherited binary, and the reason `ro.hardware` still carried the upstream's name. Only 49 KB, and it performed no composition (see bd remora-emoe for the measured contract). **Superseded — done at R4:** Remora's own HWC2 HAL (`hwcomposer.remora`, in `vendor/remora`) replaced it, the prebuilt left the image, and `ro.hardware` reads `remora` (set through the image entrypoint's `androidboot.hardware`). **Audio was never in this row** (corrected): there is no prebuilt primary audio HAL in the A17 image, and `device/remora` requests AOSP's own `audio.r_submix.default` + `android.hardware.audio.service`, so audio is Apache-2.0 source. | remora-emoe / 28ix.4 R4 ✅ |

**Cutover strategy: strangler-fig.** Nothing is purged until its replacement is verified in daily
use. Purge is the LAST phase — a hard purge first would have left every instance undeployable for
however long the rewrite took.

**Target scope: A17 only.** A16 was dropped from the plan mid-execution: the golden
A16 images carry no agent and never will, so the verification gate, the feature default, and the
tag capabilities are all A17-scoped (`Feature::defaultOnFor` exists because of exactly this).
**Superseded in part:** A16 was later carried over onto the same device tree, so both releases now
lunch `remora_x86_64` from `device/remora` and an A16 source build can bake the agent. The
`mirror_agent` *default* stays A17-only, because no registered A16 tag carries the agent.

## Options considered

### A — Keep tracking the upstreams
Rebase the fork on scrcpy releases; hope the device-tree upstream revives.
**Pros:** zero rewrite cost; upstream fixes for free (if any came).
**Cons:** the free fixes had stopped coming; every fork feature (compose, flex display, the
latency work) is a permanent rebase burden; the device half stays built on reflection against an
Android we compile ourselves; the released product's core is someone else's dead project.

### B — Rewrite first, purge first
Delete the scrcpy path, then build the replacement in the vacuum.
**Pros:** no transition code (no probe, no fallback, no carve-outs).
**Cons:** every instance undeployable for the whole rewrite; no A/B against the proven path
(the audio parity check, the list-apps correctness comparison, and the PIP argv validation all
ran agent-vs-jar on live sessions); a "big bang" cutover with no verified fallback.

### C — Strangler-fig absorb *(chosen)*
Build the replacement beside the incumbent, verify each session kind live against it, flip the
defaults, then delete the incumbent and archive its repo.
**Pros:** daily driving never broke; every step verified against the thing it replaced; the
transition code (probe-first, jar fallback, audio carve-out) was explicitly marked and deleted on
schedule. **Cons:** temporary double surface (two backends in the client for two days); the
transition scaffolding itself had bugs to manage (the `WITH_AGENT` dead toggle shipped one
agent-less build before verification caught it).

## How the cutover actually ran (the order, as executed)

1. **Client** (28ix.2): in-house mirror at parity — video, input, audio, compose, splash hand-off.
2. **Agent sessions** (28ix.3.x): skeleton → input → video → compose/new-display → audio (as a
   connection role, fixing v1's socket-ordering fragility) → list-apps. Each verified live
   against the jar before the next started.
3. **Bake** (28ix.3.4): soong module + baked rc via the staged `mirror_agent` feature;
   probe-first client with jar fallback so unbaked images kept working.
4. **Mount drop**: `ResolvedConfig::agentBaked` — a baked image rides with no dev rc/dex mounts,
   because two definitions of `service remora_agent` are a parse-order accident waiting to happen.
5. **The flip**: `mirror_agent` default-on for A17 alone, paired with the A17 tags advertising it
   (the `defaultsSelectGappsTag` tripwire is what forces the pairing).
6. **The purge** (28ix.5): v1 deleted end to end — client backend, protocol builders, jar
   preflight, `--server-path`, store keys — and the fork archived as a restore-tested tarball (a
   git bundle *verified but failed its clone-back test* on the shallow checkout, which is why the
   archive is a tarball).

Precedent honoured throughout: **refuse, don't reinterpret** (the `backend=vm` rule). An image
without the agent gets a refusal naming the rebuild, not a silent substitute; a `source_kind`
contradicting the profile gets a refusal naming the conflict.

## Licensing audit

What the rewrite changed legally: nothing was ever blocked, but the obligations move from
"redistribute two Apache-2.0 forks with attribution" to the table below. **The unresolved rows
gate the public image mirror, not the code release** — they are 28ix.6's checklist.

| Component | License | Obligation / status |
|---|---|---|
| AOSP + LineageOS base | Apache-2.0 | Attribution in the release NOTICE. Standing obligation, unchanged by the rewrite. |
| Linux kernel | GPLv2 | **Not an obligation** (corrected). Remora ships no kernel: a container shares the host's. Verified on the live A17 container — `uname -r` is byte-identical to the host's, and `find` over `/system` and `/vendor` returns zero `.ko` files and zero kernel images. The original row is a fossil of the removed `vm` backend, which booted a guest kernel; `bare` and `remote` never do. Nothing to offer, because nothing is distributed. |
| scrcpy fork | Apache-2.0 | Retired from the product. v2 reuses v1's byte *layouts*; `MIRROR_PROTOCOL.md` attributes them. Keep the attribution as long as that spec stands. Archive kept for provenance. |
| Upstream container base (container patches; the upstream prebuilts project's binaries, incl. the hwcomposer and audio HALs) | Apache-2.0 | Attribution (NOTICE). The device and vendor trees are no longer inherited — `device/remora` and `vendor/remora` replaced them (28ix.4 R1/R2). **Update:** the prebuilt binaries are gone too — R3 builds Mesa from source (`mesa_source`) and the prebuilts project left the manifests, and R4 replaced the hwcomposer. What still derives from that upstream is the container-compat patch set. |
| Remora agent, client, engine | ours | **Decided at 28ix.6: GPLv3** (`LICENSE`). |
| Magisk (baked by `magisk_root`, default-on) | GPLv3 | **Audited, then resolved — no longer blocks the image mirror.** The audit found the shipped build was Magisk *Delta* (HuskyDG) 30.6 and that the HuskyDG GitHub *account* no longer exists (the user, not just the repos, 404s), leaving the corresponding source with no upstream home; GPLv3 §6 makes source availability a condition of *distributing the binary*, so that build could never be mirrored. Fixed by moving the payload to upstream topjohnwu v30.7, whose source is live — the obligation is now met by offering the tag with the release. Never affected the code release: the payload is not in the public tree. See below. |
| GApps | proprietary Google | As first recorded: never publicly mirrored, GApps a local-build feature only. **Superseded** by the decision below to publish both flavours — GApps images are mirrored under an explicitly accepted exposure. |
| Widevine L3 blobs (`widevine_l3`) | proprietary | As first recorded: local-build only. **Superseded** the same way as GApps. |
| Shamiko, PlayIntegrityFork, ReZygisk stack (opt-in root-hiding) | mixed — **audited per component, see below** | Two of three are clean GPLv3 with live upstreams; **Shamiko has no published source and blocks the mirror**. As first recorded, publishable images excluded the whole opt-in hiding stack. **Superseded:** only Shamiko is excluded; ReZygisk and PlayIntegrityFork ship in the published `-gapps` tags (`play_spoof`). |

### Hiding stack, per component (audited)

| Component | Licence | Upstream | Verdict for a mirrored image |
|---|---|---|---|
| ReZygisk | GPL-3.0 | `PerformanC/ReZygisk`, active | **Clear** — and cleaner since the source build replaced the binary edit. We pin an exact commit (`e42886f4`) and build the 64-bit binaries from it with one source patch in the public tree — so corresponding source is a commit SHA plus a diff, the ordinary GPL shape. It used to be a commit SHA plus a script that machine-edited the binary; that is gone (bd remora-4ei.98). |
| PlayIntegrityFork | GPL-3.0 | `osm0sis/PlayIntegrityFork`, active | **Clear**, unmodified; record the release with the payload. |
| Shamiko | **none — closed-source freeware** | LSPosed publish no Shamiko source (verified: no such repo in the org) | **Blocks.** No licence grants redistribution, so it cannot ship in a mirrored image regardless of the GPL questions. Keep it a supply-it-yourself payload. |
| Magisk **upstream 30.7** | GPL-3.0 | `topjohnwu/Magisk`, tag `v30.7`, active | **Clear** — resolved by option (b) below. Was the wider of the two blockers, being the default-on *root* feature rather than the opt-in hiding stack. Offer the tag alongside the release and the GPLv3 §6 obligation is met. |

Consequences that follow, and they are decisions rather than research: a mirrored image can carry
**Shamiko under no circumstances**, and Magisk freely.

Magisk **was** the harder of the two. The ways out were (a) host the Delta 30.6 source ourselves,
if a trustworthy copy could be found and verified against the shipped binaries, (b) move
`magisk_root` to upstream Magisk, or (c) mirror only images built without `magisk_root`.

**Option (b) was taken and is done.** It was real work rather than a swap, exactly as
this ADR predicted: upstream has no `--setup-sbin`, the Delta-only command that built Magisk's
tmpfs environment and the sole reason Remora ran a fork at all. `magisk-setup.sh` now builds that
environment directly — around forty lines of shell against a licence problem that had no other
solution — and root is re-verified in a container (`su -c id` gives uid=0). The wider point is that
the fork was never buying us anything but that one flag.

Practical consequence for the mirror, corrected against the actual tag registry:

**Re-checked after the upstream-Magisk swap.** The earlier reading of this section —
"no currently registered tag is publishable, and the no-GApps ladder is an empty set" — was correct
when written and is now out of date, because it rested entirely on `magisk_root`: all four A17 tags
in `Features.cpp` list it, including the no-GApps base `remora24:x86_64`. With the obligation
satisfiable, `magisk_root` stops excluding anything.

That leaves the base tag `remora24:x86_64` **one** exclusion away from clean-publishable rather
than blocked outright, and the remaining one is `arm_translate` — proprietary Intel/Google, in the
provides of every A17 tag, and the easiest to miss because it is neither a "Google" component by
name nor opt-in. Shamiko and `play_spoof` are not in the base tag at all.

A qualifying image must be built without **four** things:

| excluded | why |
|---|---|
| `gapps` | proprietary Google, never mirrorable |
| `widevine_l3` | proprietary CDM |
| `arm_translate` | the ARM native bridge is proprietary Intel/Google (`vendor/PAYLOADS.md`) — this one is easy to miss because it is not a "Google" component and it is default-on |
| `play_spoof` + `shamiko` | the hiding stack; Shamiko is closed-source freeware |

What survives is Remora's own work plus Apache/MIT upstreams: the agent, the updater, the c2-va
media stack, Mesa (then `native_vulkan`; since retired, and Mesa is now built from source by
`mesa_source`), the sensors HAL, the Lineage framework work and the container-compat patches. That is a real image and it is honestly mirrorable — but it is
**materially less capable** than anything Remora builds today, because dropping `arm_translate`
means arm64-only apps do not run at all. That is the trade a public mirror costs, and it should be
stated on the download page rather than discovered.

Nothing blocks building such a variant; it simply has never been built, and it needs its own tag
rather than a re-use of an existing one. The mirror's `index.txt` should carry only tags audited
against this table.

**DECIDED — publish BOTH flavours, and keep the local-fetch path.** The mirror serves
audited clean images *and* GApps/Widevine variants — a vanilla build alongside a GApps one —
while `vendor/fetch-payloads.sh` remains for anyone who would rather add the proprietary parts on
their own machine. This is a deliberate acceptance of the same exposure OpenGApps and every ROM
mirror carry: Google licenses no redistribution of GApps, and the Widevine CDM's origin licenses
none either. It is tolerated, not permitted, and that is a
risk decision the project owner has taken with the facts in front of them — not a licence, and
not something this document should pretend is one.

**Magisk is NOT covered by that precedent, and must not be filed under it — it is now simply
compliant.** The distinction is worth keeping even though the blocker is gone, because it is what
made the fix mandatory rather than optional. GPLv3 *permits* redistribution outright, provided
corresponding source is offered: shipping Magisk was never a risk to accept, it was an obligation
to meet, and with Delta's upstream 404ing it could not be met at any price. That is why this one
could not ride along under the accepted-exposure decision the way GApps does, and why "ship it and
see" was never on the table. Resolved by moving `magisk_root` to upstream v30.7; the
release must offer the matching source (tag `v30.7`) alongside the image, which is now the whole
of the requirement.

The same distinction applies to `arm_translate`: proprietary Intel/Google, so it rides in a
published image only under the accepted-exposure decision above, exactly like GApps.

`index.txt` must therefore say what each tag carries, not merely list tags — a user choosing
between a clean image and a GApps one is making the same licensing choice this section just made,
and should be able to see it.

**The honest gap in this decision, which is the part to fix rather than paper over:** as first
recorded, only `native_vulkan` and `widevine_l3` were overlayable onto a prebuilt image
(`kOverlayable` in `ImageBuild.cpp`). Widevine genuinely works this way — the payload is fetched
locally and applied as one `COPY` layer. GApps, `arm_translate` and `magisk_root` were **not**
overlayable and needed a full source rebuild, so for those three "fetch locally" meant "spend
hours building", which is exactly what a mirror exists to spare people. Making GApps overlayable
was what would make this decision deliver what it promises.

**Update — the GApps half is closed.** `kOverlayable` is now `{widevine_l3, gapps}`: the GApps
overlay was verified to install exactly the files a source build does (byte-identical against a
booting source-built image), so a clean image plus a locally fetched GApps payload is one `COPY`
layer away. `native_vulkan` left the set with the feature itself — the Vulkan driver is a
per-deploy boot property now, not an image property. `arm_translate` and `magisk_root` remain
source-build only.

## Deliberately not ours

"Own the stack" is a claim about the *Remora layer*, not about everything an image touches. These
are the pieces we consciously do **not** intend to author, recorded so the question is not
re-litigated each time it looks tempting. The obvious ones — AOSP/LineageOS, Qt, FFmpeg, Mesa, and
the proprietary blobs (GApps, Widevine, the ARM native bridge) — are covered by the tables above.
The non-obvious ones:

| Piece | Why not | What we do instead |
|---|---|---|
| **Zygote injection** (ReZygisk) | It tracks Android's *internal* zygote signatures release by release — upstream added the A17 `nativeForkAndSpecialize` variant only in August 2026, and corrected its `gid` position days later. Owning it means chasing those internals forever, for a component that is not a Remora differentiator. | Stay on the GPL-3.0 upstream and pin an exact commit; our only delta is a source patch applied at build time by `build-rezygisk.sh` (bd remora-4ei.98). |
| **Play Integrity spoofing** (PIF) | Adversarial and *server-side*. The verdict is Google's to change; a spoof profile goes stale on their clock regardless of how good our code is, and no amount of ownership converts this into a solved problem. | Track upstream, rotate the profile, and treat staleness as routine maintenance (the 6-month warning at build time). |
| **Root** (Magisk) | Writing a root solution is disproportionate to what Remora actually needs from one — and note Remora already starts the injector itself rather than through `magiskd`. | Reduce the dependency rather than reimplement it. The licence scare (Delta's upstream vanished, stranding a default-on GPLv3 payload) was survivable only because the dependency was shallow — one Delta-only flag, replaced by a shell script. That is the argument for *needing less of it* rather than *owning it*. |
| **Chromium / WebView** | The manifest takes LineageOS's prebuilt WebView for a reason: a Chromium build is out of all proportion to the product, and WebView bugs we have hit were not WebView's. | Fix the layer that is actually wrong — bd remora-j85 attacks A17 WebView tile rendering at Mesa/iris, which is the correct altitude. |
| **virglrenderer / Venus** | We carry 26 host patches, which is real engineering — but a permanent private fork of a moving Mesa-adjacent target is the expensive way to hold it. | Upstream what is upstreamable; a merged patch costs nothing to carry. Owning the hwcomposer settled it: since bd remora-emoe the interception is **never armed** — `REMORA_FRAME_SOCKET` is withheld unconditionally, so the render server renders and publishes nothing. That deleted the whole class of "which context is the screen" guessing (`pick_display`). Roughly 15 of the 26 patches are now dead weight carried only because the surviving *rendering* fixes share a helper header with them (`vkr_remora_track.h`); splitting that is bd remora-emoe.4. |
| **gralloc / minigbm** | Already at the right level: we *build it from source* with our patches (`gralloc.minigbm_gbm_mesa.so` is what SurfaceFlinger loads live). Authoring an allocator from scratch buys nothing. | Keep patching and source-building; that is ownership enough. |
| **VA-API drivers** (iHD, nvidia-vaapi-driver) and libva | Full GPU media drivers. Not a place to compete. | Wrap them. `remora-frame-decoder` calls `vaExportSurfaceHandle` directly and stays driver-agnostic, which is what makes the same helper work on NVIDIA, Intel and AMD (bd remora-e5x.3). |

The through-line: own the **seams** — the device tree, the vendor layer, the HALs, the protocol,
the client, the codec plumbing — and rent the **oceans**.

## Consequences

**Easier now:** the device half is plain platform code — real APIs, no reflection, no version
gate, no per-session push (bring-up dropped its most expensive step); the protocol is ours to
extend (roles and session kinds already exceed what v1 could express); one identity to release.

**Harder now:** device-half changes need an image rebuild (the `agent/build.sh` dev loop exists
but the baked jar is what ships); no upstream to blame or borrow from — capture/encode bugs on
new Android majors are ours alone; A16 and older run the registry's prebuilt images without the
agent, and that split is permanent. **Superseded:** the upstream prebuilt images were removed from
the registry, so Android 8–15 are named but refused, and A16 builds from `device/remora` like A17
and can bake the agent.

**To revisit:**
- 28ix.4 — R3/R4: replace the remaining upstream prebuilt binaries, then the closed HALs;
  `ro.hardware` could flip to `remora` only at R4, because those prebuilts were loaded by name
  from it. **Done:** R3 builds Mesa from source and dropped the prebuilts; R4 replaced the
  hwcomposer and `ro.hardware` reads `remora`. 28ix.5's upstream-naming sliver rode with R1.
- **No forks.** The direction taken is that the image half is reimplemented, not
  forked or mirrored: what Remora ships is authored here, and what it cannot author (the
  AOSP/Lineage base, and the third-party payloads in `vendor/PAYLOADS.md`) is named plainly as
  inherited.
- 28ix.6 — the licensing table's *verify* rows, NOTICE/attribution, outbound license, kernel
  source offer; the mirror's audited tag set. **Update:** the outbound licence is GPLv3; there is
  no kernel source offer to make, because no kernel ships; the audited tag set is
  `docs/IMAGE_MIRROR.md`.
- A18 — the first new Android major where "no upstream to borrow from" gets tested for real;
  also the moment `androidReleases()` gains a row and the A17-scoped defaults get re-judged.
