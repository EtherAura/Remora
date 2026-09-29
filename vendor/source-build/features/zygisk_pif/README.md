# Zygisk + PlayIntegrityFix (per-app Play spoofing)

The **correct** architecture for making Google Play treat the device as a certified
Pixel **without** touching the system build.prop (so adb and the mirror stay
stable — a whole-system build.prop spoof triggers a GMS re-registration storm
that saturates the adb path).

The public tree carries this directory's scripts and wiring; the binaries (and all
of Shamiko) are payloads — see `vendor/PAYLOADS.md`.

## How it works
1. **ReZygisk** (ptrace-based Zygisk, no kernel support needed) injects `libzygisk.so`
   into zygote. Its `zygisk-ptrace64` daemon crashes on Android 16 bionic
   (`FORTIFY: strcat ... 0-byte buffer`) in a cosmetic version-banner concat — the
   "parked" blocker.

   **The 64-bit binaries are BUILT FROM SOURCE** by
   `vendor/host-prereqs/rezygisk/build-rezygisk.sh`, which fetches the pinned commit,
   applies `upstream/0001-monitor-bound-the-module.prop-status-appends-*.patch` and
   installs the result into `rez-mod/`. That patch replaces the unbounded `strcat`
   appends with `strlcat`, so the abort is fixed *structurally* rather than worked
   around (bd remora-4ei.98).

   **This replaced a binary patch.** Before the source build, we shipped a NOPped
   blob: `patch-rezygisk.py` blanked the three offending `__strcat_chk` call sites and
   `zygisk-ptrace64.patched` was the machine-edited result, committed. Both are gone.
   The binary edit was defeatable by any upstream refactor and had to be re-derived on
   every bump; a source build is neither.

   **`zygisk-ptrace64` is not an upstream file name.** Upstream ships
   `lib/<abi>/libzygisk_ptrace.so`; `customize.sh` renames it into `bin/` at install
   time, and `rez-mod/` is that *installed* layout because we cannot run Magisk's
   installer. The build script performs the same rename.

   **64-bit only, and the 32-bit half stays vendored.** The FORTIFY abort affects only
   the 64-bit daemon; the 32-bit one has always run unpatched and serves requests fine.
   The x86 loader also does not link with NDK r27c (`undefined symbol:
   _GLOBAL_OFFSET_TABLE_` — upstream's `common.mk` expects NDK 29.x), so
   `bin/zygisk-ptrace32`, `bin/zygiskd32` and `lib/libzygisk.so` remain the upstream CI
   artifacts. Each bitness is internally consistent: the 64-bit set comes wholly from
   our build of commit `e42886f4`, the 32-bit set wholly from upstream's build of the
   same commit.

### Bumping ReZygisk

```bash
gh api repos/PerformanC/ReZygisk/actions/artifacts   # pick a release build
```

**Check the artifact is really `main`.** Builds also run for pull requests, and those
are speculative merge commits whose `head_sha` is not on `main` — build 539 is one.
Confirm with `gh api .../actions/artifacts/<id> --jq .workflow_run.head_sha` and match
it against `gh api repos/PerformanC/ReZygisk/commits/main`. Verify the shipped
`.sha256` sums (98 of them). Then move the pin in
`vendor/host-prereqs/rezygisk/build-rezygisk.sh` (`REMORA_REZYGISK_PIN`) and re-run it:
it refetches, re-applies the source patch, rebuilds and reinstalls the 64-bit set, and
REFUSES if the patch no longer applies or if the built binary still carries more than
the one deliberate `__strcat_chk` call site. Copy the 32-bit binaries from the release
zip by hand under `customize.sh`'s installed names. Diff the support scripts too —
`post-fs-data.sh` carries a Remora `chcon` fix that must survive.

Current: **537 (`e42886f-release`)**, the `main` tip when it was pinned. This is the
first build with Android 17 zygote hooks — 515 predated them entirely (added in
`946708e0`, gid argument position corrected in `e42886f4`).

   **Do not migrate this to Zygisk Next** (the standing suggestion in bd remora-4ei.86,
   now reversed there). Zygisk Next dropped GPL-3.0 at v4-0.9.2 — its own README, shipped
   inside the release zip, reserves all rights and forbids modification, redistribution
   "as part of another project", and extracting parts. Remora vendors and bakes these
   binaries into a published image, so that licence rules it out. ReZygisk is GPL-3.0,
   which is what makes the arrangement here lawful, and it is actively developed.
2. **PlayIntegrityFork** (osm0sis, v17 — ships `zygisk/x86_64.so`, unlike KOWX712's
   ARM-only build) is loaded by ReZygisk into `com.google.android.gms.unstable`
   (DroidGuard) and spoofs `Build.*` to the `pif.json` profile *inside that process
   only*. System props are untouched.

## Verified
- `libzygisk.so` mapped into zygote64; PIF `x86_64.so` mapped into gms.unstable.
- `PIF/Java:DG` log shows FINGERPRINT/MODEL/etc rewritten to the Pixel profile.
- adb stays rock-solid (system fingerprint unchanged).

First verified on an x86_64 Android 16 image; the pinned ReZygisk is the first build
with Android 17 zygote hooks.

## Remora integration — two gates, automatic
1. **IMAGE gate** (build): the `play_spoof` build feature bakes this stack into
   `/system/etc/remora-spoof` (`assemble-image.sh`, `WITH_PLAY_SPOOF`; source rebuild).
   "Enabled in the image" = you built it in.
2. **RUNTIME gate** (option): `play_spoof=true` in the profile.
3. **Auto**: the deploy (`src/engine/Deployers.cpp`, for `bare` and `remote` alike)
   acts ONLY when BOTH gates hold. It installs the baked modules into `/data/adb/modules`
   with `install.sh` — again whenever the image's copy has drifted from the one in
   `/data` — clears any `disable` markers Magisk's bootloop protection left, and arms
   the ReZygisk injector before zygote starts, so its monitor catches the fresh zygote;
   the post-fs-data hook and a framework restart cover a boot where that race is lost.
   It also checks that root actually came up and fails loudly if not. Either gate
   off → does nothing. No separate command.

## Remaining (out of our hands / future)
- **Install-catalog verdict**: PIF spoofs DroidGuard (Integrity) + the Play Store
  fingerprint (catalog device-config upload). Whether a cert-gated app (e.g. Sonar)
  flips to installable then depends on Google's backend re-evaluating the uploaded
  device config — propagation latency (minutes–hours), not observable on-device.
- **Shamiko** (root-hiding) is optional, behind its own `shamiko` build feature.
  Remora's builds use v1.2.5, but it is closed-source with no licence, so none of its
  files are in the public tree — supply it yourself; provenance and hash in
  `shamiko/REMORA.md`. `install.sh` installs it only if it was staged.
- **GUI checkbox** for the play_spoof runtime option (the profile key works today).

`pif.json` holds the Pixel 8 Pro (husky) Canary profile; edit to rotate. Advanced PIF flags
must be JSON strings ("1"/"0"), not booleans.

**Refreshing it.** The profile goes stale on a clock nobody watches — it sat 15 months old and on
Google's blocklist before anyone noticed (bd remora-4ei.47), so `remora build --source` now warns
before baking one older than 6 months. `pifork/autopif4.sh` is the module's own refresher but does
not run here: it needs busybox (`date -D`, `nc`), it hangs in-container, and it picks a *random*
Pixel. Do it by hand instead — the sources are Google's own, and the whole derivation is four
fetches:

1. `developer.android.com/about/versions` → latest version page → its `download` page carries the
   `<tr id="…">` beta device table (product → model).
2. `flash.android.com` → the API key sits in `<body data-client-config=…>`, second `;`-field.
3. `content-flashstation-pa.googleapis.com/v1/builds?product=<device>_beta&key=<key>` → the
   `flashstationBuild` array. Take the **last entry whose `previewMetadata.canary` is true**;
   `releaseCandidateName` is the ID and `buildId` is the incremental. (The API no longer has a
   top-level `canary` flag the way `autopif4.sh` still greps for — it moved into `previewMetadata`.)
4. `source.android.com/docs/security/bulletin/pixel` → the row for the canary's `id` month
   (`canary-YYYYMM`) gives `SECURITY_PATCH`; falling back to `YYYY-MM-05` matches what that page
   says anyway.

Then `FINGERPRINT=google/<product>/<device>:CANARY/<ID>/<incremental>:user/release-keys` — the
version field is the literal string `CANARY` for canary builds, not a number.

Keep `DEVICE` aligned with what `devicespoof` claims system-wide (bd remora-4ei.52). PIF only
spoofs inside GMS, so a mismatch means the device tells Play one story and every other app a
different one — an inconsistency worth avoiding for free. `DEVICE_INITIAL_SDK_INT` stays `32`
regardless of the device's real launch API level; that is deliberate in `autopif4.sh` too, since
devices launched at API ≤32 face relaxed key-attestation requirements.
