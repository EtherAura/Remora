# Binary payload manifest — what may be published, and what must be fetched

Companion to `PROVENANCE.md`, which records the vendored **scripts**. This file covers the
**binary payloads** under `source-build/features/*/`: prebuilt apks and shared objects that a
build stages into the image.

**Why this file exists.** These payloads are third-party binaries,
several of them proprietary. The public Remora repository **does not carry them**: it carries the
`.mk` files, manifests and READMEs beside them — which are ours — and this manifest. A build that
wants a payload feature fetches or supplies the payload locally.

For four payloads the public tree carries **only Remora's wiring**, because the third-party *text*
that travels with the binaries is not ours to publish either: the GApps permission and sysconfig
XML (`gapps/product/`), Widevine's init rc and VINTF fragment (`widevine/vendor/`), the
native bridge's binfmt and cpuinfo config and guest tree (`ndk_translation/etc/`, `guest/`,
`lib64/`), and every Shamiko file except Remora's own `zygisk_pif/shamiko/REMORA.md`. Those are
excluded along with the binaries, and come back with the payload.

> **Provenance debt, stated plainly.** The blobs entered the tree in bulk, assembled from a local
> catalog before their upstream sources were recorded — no upstream named, no versions, no hashes.
> Several have since been identified from evidence inside the tree and the binaries themselves; the
> `source` column says what is actually known, and says `UNRECORDED` where it is not.
> **`fetch-payloads.sh` refuses to guess:** an `UNRECORDED` payload must be sourced by hand and its
> row completed. Resolving every `UNRECORDED` row is a release gate, not a nice-to-have — a
> published build script that silently pulls an unidentified binary is worse than one that refuses.

## The payloads

| payload | build feature | in-image | redistributable by us? | source |
|---|---|---|---|---|
| `gapps/` | `gapps` | GmsCore, Phonesky, GSF, Calendar/Contacts sync adapters | **NO — proprietary Google.** Never mirrored, never in a published image. | **UNRECORDED distribution; versions recovered** (`aapt2 dump badging`): GmsCore `24.23.37 (190800-659641177)` (`com.google.android.gms`, vc 242337041), Phonesky `41.3.25-31` (`com.android.vending`, vc 84132530), GSF `15`, CalendarSyncAdapter `2024.14.0-622676295-release`.<br><br>**Evidence toward LiteGapps (not proof):** two of the vendored permission XMLs, `com.google.android.gms.xml` and `com.android.vending.xml`, carry a `Copyright (C) 2020 - 2024 The Litegapps Project` header; the others carry AOSP headers or none. That points at a LiteGapps package, but a header can be copied between distributions and no apk has been matched against a LiteGapps release, so the source stays unrecorded until one is.<br><br>**Eliminated:** a MindTheGapps-layout GApps archive (it ships `default-permissions-mtg.xml`) is not the source: none of the seven files compared (the five apks, `google.xml`, `com.google.android.dialer.support.xml`) hash-match. |
| `microg/` | `microg` | MicroGmsCore, MicroGCompanion, Aurora Store | microG GmsCore and Companion are Apache-2.0; Aurora Store is GPL-3.0. A mirrored image carrying them owes Aurora's source offer. | **RECORDED, PROVEN BY HASH** in `microg/VERSIONS`: GmsCore `v0.3.15.250932` (`com.google.android.gms`) and Companion `0.3.15` vc 84022630 (`com.android.vending`), both assets of the `microg/GmsCore` GitHub release `v0.3.15.250932`, plus Aurora Store `4.8.3` (vc 75) from F-Droid. All three vendored apks match the sha256s recorded there. |
| `ndk_translation/` | `arm_translate` | the arm64 native bridge (`libndk_translation`, `libnb`, guest libraries) | **NO — proprietary Google.** | **UNRECORDED**, with one candidate **eliminated with evidence**: it is not the `libndktranslation.zip` source archive of `vendor_google_proprietary_ndk_translation-prebuilt` @ `68734c52556d3d7a6db34c603dd9276915c29f2f`: its `libndk_translation.so` is 2.5 MB against our 5.4 MB — a different build, not a patch difference; zero of 40 files hash-match. Do not record that archive as the source. Two internal anchors pin what we have: `libnb.so` and `libndk_translation.so` are byte-identical to each other, and `patch-arm-translation.py` asserts `LIBNB_SHA256 = fbadc774c989…` and `LINKER_SHA256 = 29c98141e26f…` before patching, so a re-harvest cannot go unnoticed.<br><br>**What it is built from, and why that does not help:** the binary is berberis-derived — `libndk_translation.so` exports `berberis_entry_*` symbols throughout — and AOSP 17 carries berberis in-tree at `frameworks/libs/binary_translation` (Apache-2.0). But that tree's `berberis_config.mk` names `riscv64` 57 times and `arm64` zero times, and its only enable file is `enable_riscv64_to_x86_64.mk`: AOSP's berberis translates RISC-V guests, and Google ships no source for the arm64→x86_64 translator. So this stays a proprietary prebuilt and the base tag's one exposure (`docs/IMAGE_MIRROR.md`), and building it from source is explicitly out of scope (bd remora-ykhz). |
| `magisk/` | `magisk_root` | magisk.apk, stub.apk, magiskboot/magiskinit/magiskpolicy/busybox | **GPLv3, and the source obligation is satisfiable.** Upstream source is live at github.com/topjohnwu/Magisk, tag `v30.7`. A mirrored image MAY carry this build provided the release offers the matching source alongside it. The payload was previously Magisk Delta (HuskyDG) 30.6, whose GitHub account no longer exists, so its corresponding source had no upstream home and GPLv3 §6 was unsatisfiable at any price — moving to upstream is what resolved it. | **UPSTREAM Magisk 30.7 (30700)** — see `magisk/magisk/PROVENANCE.md` for per-file sha256s; `fetch-payloads.sh magisk` downloads the release apk, verifies its hash and extracts that table. The swap needed real work, not a rename: upstream has no `--setup-sbin`, so `magisk-setup.sh` builds the tmpfs environment that magiskinit would otherwise build from a patched boot image. Root verified in a container (`su -c id` → uid=0). |
| `widevine/` | `widevine_l3` | L3 DRM service + HAL | **NO — proprietary Google/Widevine.** | **RECOVERED, PROVEN BY HASH**: the GitHub source archive `vendor_google_proprietary_widevine-prebuilt` at commit `679552343d8b2e8d7a19b6df61c7a03963d0c75b` (archive sha256 `ba7b0ceb9152df3b00f0a5893a52fe954edcbd8b95693c0e65105a34f4c519c2`). Four vendored files are byte-identical to `prebuilts/` in that archive: `libwvaidl.so`, `android.hardware.drm-service-lazy.widevine`, its `.rc` and its VINTF manifest. The other three — `android.hardware.drm-V1-ndk.so`, `android.hardware.drm.common-V1-ndk.so` and `libprotobuf-cpp-lite-3.9.1.so` — are the service's AOSP dependency closure, harvested from an earlier working image rather than taken from the archive. The CDM reports version `4.1.11129.4`. Only the fetch URL (the GitHub org serving that repo) is unwritten; the archive is fully identified without it. |
| `zygisk_pif/` | `play_spoof`, `shamiko` | ReZygisk (patched) + PlayIntegrityFork + Shamiko | **Audited per component**: ReZygisk GPL-3.0 and PlayIntegrityFork GPL-3.0, both live upstreams — clear. **Shamiko has NO published source or licence** (LSPosed publish none), so it cannot be redistributed at all: none of its files are in the public tree, and mirrored images exclude the whole stack. | **RECORDED.** ReZygisk is pinned exactly: **build 537, commit `e42886f4`** of `PerformanC/ReZygisk`, taken from its CI artifact and checksum-verified (all 98 shipped `.sha256`). The 64-bit binaries are ours, built from source (bd remora-4ei.98): `vendor/host-prereqs/rezygisk/build-rezygisk.sh` fetches commit `e42886f4`, applies Remora's bounded-append patch (`zygisk_pif/upstream/`) and installs `zygisk-ptrace64`, `zygiskd64` and `lib64/libzygisk.so`. The 32-bit binaries remain upstream CI artifacts of the same commit (the abort is 64-bit only). **PlayIntegrityFork by osm0sis, v17**, chosen over KOWX712's build because it ships `zygisk/x86_64.so`. **Shamiko v1.2.5 (414)**, LSPosed, closed-source — provenance and hash in `zygisk_pif/shamiko/REMORA.md`; supply it yourself. |

**Ours, and public** — no third-party payload, nothing to audit: `remora_agent/`, `remora_updater/`,
`camera_external/`, `device_compat/`, `hw_features/`, `usb_audio/`, and every `.mk`, `Android.bp`,
manifest and README inside the payload dirs above. Two of ours still produce a binary that the
public tree does not carry:

- `mesa_source/` — the payload is Mesa cross-built by `vendor/host-prereqs/mesa-android/build-mesa.sh`
  and is never committed (it is gitignored); a source build refuses without it.
- `nb_sigfix/` — a Remora-authored Zygisk module whose `.so` `assemble-image.sh` rebuilds from
  `src/` against the tree it has just built; see its `REMORA.md`.

## What a published image may contain

Which built images may be published, and what each carries and obliges, is recorded tag by tag in
[docs/IMAGE_MIRROR.md](../docs/IMAGE_MIRROR.md). This file covers only the payloads themselves.

## Reconstructing an UNRECORDED row

What has worked, in order of yield:

1. **Read the feature dir's own README** — `zygisk_pif/` and `magisk/magisk/` carry real design
   notes, and both named their upstreams.
2. **Ask the binary.** `strings <binary> | grep -E '^[0-9]+\.[0-9]+'` recovered the old Magisk
   payload's `30.6(30600)`. Version banners, build IDs and package names survive stripping more
   often than not.
3. **APK manifests need a tool** — `AndroidManifest.xml` inside an apk is binary-encoded, so
   `strings` yields nothing. Use `aapt dump badging <apk>` (or `apkanalyzer manifest print`) for
   `versionName`/`package`; the LineageOS tree already ships `aapt`.
4. **Read the text files beside the binaries.** A copyright header survives where a binary says
   nothing — it is how the GApps XMLs pointed at LiteGapps. Treat it as a lead, since headers get
   copied between distributions.
5. **Look for the downloader's cache — highest yield of all, when it exists.** Several payloads
   arrived through a download helper that keeps every archive it fetched. Those archives are
   GitHub source tarballs whose top-level directory carries the repo name AND the exact commit:
   extracting `widevine.zip` yields `vendor_google_proprietary_widevine-prebuilt-679552343d8b…`,
   which identified that row. **Then prove it rather than assuming it** — hash the extracted files
   against the vendored ones. For widevine the files the archive carries matched byte-for-byte;
   for `ndk_translation` *none* of 40 did, and for `gapps` none of seven, which is how we know
   those archives are NOT their sources. A cache hit is a lead, not an answer.
6. **A negative result is worth recording too.** Eliminating a plausible source with evidence
   stops the next person spending the same hour on it, and stops a wrong URL being written into
   a fetcher that other people will trust.
7. **The layout is a hint, not proof** — the GApps tree's `product/priv-app` +
   `system_ext/priv-app` split matches the common redistributable layouts, but do not record a
   distribution name on shape alone.
8. **Check whether it is even third-party.** A payload can turn out to be built from source
   beside it — a symbol table naming a class in a `.cpp` next door settles it. An `UNRECORDED`
   row can be a category error rather than a gap.

Record what you find in the table above **with the evidence**, the way the Magisk row does. A row
that says where a file came from without saying how that was established is the same debt again.

## Fetching

`vendor/fetch-payloads.sh` reports which payloads are present and stages the ones this manifest
identifies precisely enough to verify, checking each download against its recorded hash before it
reaches the tree. It refuses any payload whose `source` is `UNRECORDED` rather than guessing a
URL, and for one that is identified but has no recorded URL it says what to get.

A build whose feature is enabled but whose payload directory is absent **refuses at staging** and
names the fetch command — the same failure shape the unbuilt native helpers use, never a silent
image missing the feature it was asked for.
