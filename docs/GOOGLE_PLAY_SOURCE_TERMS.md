# Google Play source and terms decision record

**; research `remora-wzaa.1`.** This records inspected bytes and published terms, not a
Google licensing opinion. The original machine-readable source evidence is
[audit.json](investigations/google-play-source-terms/audit.json). During that research no payload
was installed; the later implementation validation is recorded below. No third-party installer
script was executed, no legacy payload was changed, and no external inquiry was sent.

**Owner decision:** do not contact Google. Proceed with the Genymotion-style optional
direct-download/local-install approach using an audited Google-free public base, explicit user
opt-in and third-party terms notice, and local-only proprietary derivatives. A Google reply is
not a project prerequisite. The published-terms findings below remain documented limitations;
this decision does not assert a Google licence or certification. `remora-wzaa.2` was cancelled.

## Findings

Implementation follow-up: `vendor/play/package.json` is the executable pin; `vendor/play/install.py`
implements explicit acceptance, direct host download, hash verification, bounded package parsing,
allowlisted extraction, immutable COPY-only image layering, receipts and unused-overlay removal.
The research audit's `download_enabled=false` is a historical research snapshot, not its runtime
configuration. No Google binaries are included in these installer sources. The installer adds an
explicit **denial** of GMS's `ACCESS_COMPANION_INFO` permission because this package omitted its
API 37 privileged-permission entry; enforcement stays enabled. Release-base auditing remains
`remora-phvx`, separately from testing the installer on a private stripped fixture. That fixture
booted with only x86_64 advertised, bridge binaries absent and privileged-permission enforcement
enabled. All five packages installed; GMS/Store selected x86_64 and Store rendered its sign-in
screen. The same system_server remained running through a stability observation and the crash
buffer was empty. Local rollback and real SSH-host download/reuse/cleanup also passed. No Google
account was used, so login, purchases, app downloads and certification are not established.
See [validation.json](investigations/google-play-install/validation.json).

There is now a concrete forward source: **LiteGapps Core x86_64, Android 17, v5.0**.
The archive was downloaded directly from the publisher's SourceForge project, hashed and inspected.
All five base APKs pass Android's APK signature verifier; their verified signing lineages begin
with the same certificate fingerprint used by the legacy payload. GmsCore and Phonesky contain
x86_64 native code. The other three base APKs contain no native libraries.

The earlier claim that the current GApps payload was ARM64 was **wrong**. Inspection of the local
APKs proves that the current GmsCore and Phonesky also contain x86_64 code. The incorrect
`arm_translate` dependency in `Features.cpp` was removed after the isolated bridge-free A17 boot.
ARM-only third-party apps remain a separate compatibility question.

The package source is identified; **permission for the complete Remora offering is not cleared**.
LiteGapps licences its installer code under MIT but explicitly leaves Google APK rights to Google.
Google documents custom-ROM registration for service access and separately requires a licence
for distribution. Those statements do not expressly answer whether the proposed third-party
download/local-copy flow on an x86_64 container is covered. The owner chose to proceed using the
established optional-installer model without outreach. A drafted inquiry was cancelled unsent;
it stays in the development repository as historical research.

## Pinned candidate

- Publisher listing: [LiteGapps x86_64 / API 37 / Core / ](https://sourceforge.net/projects/litegapps/files/litegapps/x86_64/37/core//).
- File: `LiteGapps-core-x86_64-17.0-20261001-official.zip`.
- Download: [exact SourceForge artifact](https://downloads.sourceforge.net/project/litegapps/litegapps/x86_64/37/core//LiteGapps-core-x86_64-17.0-20261001-official.zip).
- Observed size: `149104693` bytes.
- Observed SHA-256: `975501feb5f0a68cf023847291155eaa33e76fe6db8dae3c588e12af61d4bba0`.
- The hash was computed from the HTTPS download. It is **not** a publisher-signed checksum manifest.
  ZIP CRC validation also passed; neither result grants distribution rights.
- `module.prop` declares x86_64, API 37, Android 17, v5.0. The five base APKs are in
  `files/files.tar.xz`, under `x86_64/37/system/`. Extra setup-wizard modules, scripts and tools
  are also present; do not execute or copy the entire archive as if it were a minimal overlay.
- At research time, runtime compatibility was untested and the audit recorded
  `download_enabled=false`. The later installer/boot results above supersede that technical
  status; the audit remains an unchanged historical record and does not assert permission clearance.

| Base APK | Package version | Native libraries |
|---|---|---|
| GmsCore | `26.26.34 (260800-945364269)` | x86_64; archive also contains x86 entries |
| Phonesky | `52.4.41-31 [0] [PR] 953053140` | x86_64 |
| GoogleServicesFramework | `17` | none |
| GoogleCalendarSyncAdapter | `2024.14.0-622676295-release` | none |
| GoogleContactsSyncAdapter | `12` | none |

Every APK passed `apksigner verify`. The signing lineages were additionally read through
`ApkVerifier` after verification, rather than inferred from a certificate subject string or
`META-INF/CERT.RSA` alone. The lineage root SHA-256 is
`f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83`.
GSF/Calendar include the rotation to `7ce83c1b…`; the candidate GmsCore adds `5f239127…`.
Complete per-APK hashes, certificate lineages and verifier output are in the JSON evidence.
Valid signatures establish signed content and key continuity, not Google permission for this use.

## Legacy payload provenance

The original package source remains unrecorded. All 14 current GApps APK/XML files were hashed.
The five APKs have valid signatures; GmsCore and Phonesky support x86_64. LiteGapps copyright
headers in two XML files remain a lead, not proof of the whole package's origin.

The existing `gapps.zip` downloader cache has SHA-256
`be2b44bb877bd8a409b9c4f6e60603c40a73baf79e590d53267b9774b29959ef`; none of the 14 current
files hash-matches it. Of the five APKs in the new candidate, only ContactsSync is byte-identical
to the legacy payload. That identifies one shared APK, not the legacy distribution. Use the new
candidate as a separately reviewed source; do not relabel old bytes with its provenance.

## Terms that apply, and what each establishes

**LiteGapps code and packaging.** The inspected archive contains an MIT licence with an explicit
Google APK carve-out. The [pinned upstream installer licence](https://github.com/litegapps/litegapps/blob/e4837bd35784b64c081b396f2a4c4cdac01e5c89/installer/LICENSE)
states the same distinction. Its own code may be reused with the required notice; it does not
relicense Google's APKs. The archive also identifies GNU tar as GPLv3+ and XZdec as public domain.
Using our own data-only extractor avoids distributing or executing those installer tools.

**Open GApps.** Its [published terms](https://opengapps.org/) grant no Google APK licence and
restrict public mirroring of its prebuilts. Genymotion's one-click design is useful precedent,
but its [disclaimer](https://docs.genymotion.com/features/opengapps/) is not a grant to Remora.
The candidate above is LiteGapps, not an Open GApps archive or a Genymotion-owned package.

**Google end-user software terms.** The US [Google terms](https://policies.google.com/terms?hl=en),
effective July 30, 2026 when checked, provide a personal, non-assignable software-use licence
within Google's services and restrict copying, modification and distribution. Service-specific
terms can control. A local installer is not automatically exempt from all those conditions.
The [Play terms](https://play.google.com/intl/en-US_us/about/play-terms/index.html) also require
supported devices/software and reserve restrictions on redistribution and bypassing access
protections. The [announced November 17 version](https://play.google.com/intl/ALL_us/about/play-terms/index-update.html)
retains the relevant personal-use, redistribution and access-protection restrictions reviewed
here; recheck the effective terms before release. These are the US public pages, not a universal
jurisdiction-specific opinion.

**Custom-ROM service registration.** Google's own
[uncertified-device notice](https://www.google.com/android/uncertified/warningauto) explicitly
directs custom-ROM users to register their device to use Google apps, while directing unlicensed
manufacturers to obtain a distribution licence. This corrects any impression that Google provides
no documented custom-ROM access route. The registration endpoint requires the user's Google
login; it was not entered or submitted. Registration is distinct from certified status and does
not itself supply a grant for a third party's APK distribution. The public notice does not
expressly establish the scope for Remora's virtual/container devices or automated local installer.

**GMS distribution.** The [GMS page](https://www.android.com/gms/) requires a Google licence;
the notice links to the [official contact form](https://www.android.com/gms/contact/). We have not
obtained a Remora agreement. Hosting only the audited Google-free base remains the release policy.
The optional installer needs the exact remaining scope questions answered before being advertised
as complying with every applicable rule.

## ARM translation is a separate decision

The existing `libnb.so` is `5403704` bytes, SHA-256
`fbadc774c989534a567e6af8fd16d2c00727b1f1d9cc778bf538d6b59ed9776d`, ELF build ID
`2810e5b44895c4ea0b1ad6882ea73b73`, Android note API 36. It is byte-identical to the local
`libndk_translation.so`; its original distribution remains unrecorded.

The cached translator archive (SHA-256
`a142d1586c9eafb5edf62277110f2128bc03066179a6783bcaa33ee322e1cbd0`) is pinned to commit
`68734c52556d3d7a6db34c603dd9276915c29f2f`. None of 144 local `.so`/APK/XML files matched it.
Its README identifies a ChromeOS Android 13 source. Its
[ARM-to-AMD licence](https://github.com/supremegamers/vendor_google_proprietary_ndk_translation-prebuilt/blob/68734c52556d3d7a6db34c603dd9276915c29f2f/ARM_TO_AMD_DBT_LICENSE.txt)
is narrowly scoped to ARM ISA copyrights, ties redistribution to specified AMD semiconductor
sales/use, and excludes other IP rights. It is not a general software-project grant. It also
cannot be assigned to our different binary merely because both are called `libndk_translation`.

Another [prebuilt repository](https://github.com/zhouziyang/libndk_translation/blob/c2093bd678eb493ea0f918e01ab76c0695a54c3c/README.md)
documents harvesting from emulator images. That establishes a possible origin, not permission to
repackage. The [SDK terms](https://developer.android.com/studio/terms), particularly section 3,
restrict proprietary components; extraction is not a blanket personal-use exception. Public
Berberis source includes ARM plumbing but is not evidence that the complete shipped ARM translator
is open source.

Keep proprietary ARM translation out of the public base. It need not be a compulsory dependency
of an x86_64 Google-app payload. Correct the feature model only with a bridge-free boot test;
decide ARM-only app support and its permissions independently.

## Implementation decision

The source candidate, signatures, architecture and publisher's terms are documented. Proceed
under `remora-wzaa` with the selected optional installer, without contacting Google. Users receive
a third-party terms/limitations notice before the download. Keep Google binaries out of Remora's
hosted base and public caches, and keep the modified image local. Technical release conditions
remain package verification, compatibility checks, a bridge-free boot test and safe rollback.
The unanswered scope question is a documented limitation, not an outreach prerequisite or a
reason to describe the integration as Google-authorized. No message was sent.
