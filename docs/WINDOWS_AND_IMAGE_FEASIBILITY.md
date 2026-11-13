# Prebuilt images and optional Google Play

Evaluated **** against the current working tree and primary sources linked below.
Research: `remora-wqmn`. No Windows execution, Android rebuild, payload installation, publication
or infrastructure change was performed for this evaluation.

## Recommendation

Project-hosted prebuilt images are feasible after building and auditing a variant whose complete
contents permit redistribution. **None of the four registered A17 tags currently qualifies:**
even the no-GApps tag includes a proprietary ARM translator. The older accepted-exposure policy
is superseded by the owner's requirement that distribution and supported use respect licences and
service terms. [IMAGE_MIRROR.md](IMAGE_MIRROR.md) is the current policy.

**Scope update:** the owner shelved Windows consideration for now. No Windows port,
WSL route or local VM appliance investigation is active. The existing research below is retained
for reference only. Current work concerns Linux image distribution and permitted local add-ons.

Google Play is technically layerable without compiling Android; Remora already implements the
GApps part. **That does not establish a permitted Google Play offering.** A later APK audit
corrected this evaluation's original ARM64 claim: the current GmsCore and Phonesky support x86_64.
A pinned Android 17 x86_64 LiteGapps candidate is now identified and signature-verified. The
feature model no longer forces ARM translation after an isolated bridge-free A17 boot opened
the unauthenticated Store. Google also documents custom-ROM registration; its scope for this
installer/container and package source was not established. See the
[source and terms record](GOOGLE_PLAY_SOURCE_TERMS.md).

**Chosen workflow:** the owner selected the Genymotion-style model: a project-hosted
base without Google payloads, plus a separate opt-in installer that downloads third-party packages
directly to the user's execution host and applies them locally. This selects the product design;
it does not establish a Google licence or accept every unresolved distribution/use risk. Windows
remains deferred. The Image library and `remora play-install` now implement this workflow, with
real local and SSH-host validation on a private fixture. The later
[source/implementation record](GOOGLE_PLAY_SOURCE_TERMS.md) identifies the exact pin and technical
limits. A fully audited public base and Google permission/certification are not established by
those tests.

**Subsequent owner decision:** do not contact Google; proceed with the Genymotion-style
optional installer without awaiting a Google reply. Retain the published-terms limitations,
explicit user opt-in and direct-provider/local-only boundaries. `remora-wzaa.2` is cancelled;
implementation remains in `remora-wzaa`. This is a product decision, not a claim of Google approval.

## Repository evidence

| Evidence | Consequence |
|---|---|
| [Requirements](user/Requirements.md), [deploy targets](user/Deploy-Targets.md) | Execution requires x86_64 Linux, Docker and binder. `bare` and `remote` are the supported targets. A17 uses memfd; A16 additionally requires ashmem. |
| [CMakeLists.txt](../CMakeLists.txt) | Unconditional Qt DBus, static Linux container helpers built with the host C compiler, and Linux packaging need separation for a native Windows build. Linux helper artifacts must remain Linux binaries. |
| [Spawner.cpp](../src/engine/Spawner.cpp), [MirrorApp.cpp](../src/mirror/MirrorApp.cpp), [MainWindow.cpp](../src/ui/MainWindow.cpp), [main.cpp](../src/main.cpp) | POSIX signals, pipes, process groups, `/proc`, shell execution and Linux desktop integration make this more than a compiler change. |
| [Features.cpp](../src/core/Features.cpp) | Every A17 tag includes `arm_translate`; `gapps` and `webview_pairip_fix` require it. Defaults include GApps and translation. A public variant needs accurate capability/default handling. |
| [assemble-image.sh](../vendor/source-build/assemble-image.sh) | Native-bridge payload copying depends on local `ndk_translation/guest/system` directory presence, independently of `WITH_NDK`. Disabling the source-build toggle does not prevent contamination. |
| [device.mk](../vendor/source-build/device-remora/remora_x86_64/device.mk), [BoardConfig.mk](../vendor/source-build/device-remora/remora_x86_64/BoardConfig.mk) | Bridge properties and the ARM guest ABI are unconditional. A bridge-free release must stop advertising unsupported ARM execution. |
| [ImageBuild.cpp](../src/core/ImageBuild.cpp), [ImageBuilder.cpp](../src/engine/ImageBuilder.cpp) | GApps and Widevine support COPY overlays and CLI `--base`. Payloads are operator-staged. `arm_translate` is not overlayable; unknown bases advertise no known capabilities. |
| `bd show remora-sgkg` | Historical GApps validation booted a base made by removing GApps from an existing image. It retained ARM translation. This proves layering, not the future public-base flow or permission. |
| [PAYLOADS.md](../vendor/PAYLOADS.md), [fetch-payloads.sh](../vendor/fetch-payloads.sh) | Legacy GApps and translator origins remain unrecorded. A new, separately identified x86_64 GApps candidate is documented in the source/terms audit. Fetching remains disabled pending review. |
| [MainWindow.cpp](../src/ui/MainWindow.cpp), `scanSourceLocation` / `onFetchImage` | HTTPS mirrors already use `index.txt` rows of `tag [archive [bytes]]`. Import streams `curl` into `docker load` without expected digest/signature verification. |
| [release.yml](../.github/workflows/release.yml) | The existing release pipeline publishes the application. Its SBOM/provenance does not audit an Android image. |

## Deferred Windows research

> Shelved at the owner's request. The following is historical research, not an active
> recommendation or implementation plan. `remora-7oih` is deferred indefinitely.

**WSL is out of scope at the owner's request.** There are two separable pieces: the Windows
application and the Linux execution environment. A native GUI is plausible because much of the
client already uses Qt, FFmpeg and the network protocol, but current code does not build or run
natively on Windows as demonstrated by the dependencies above.

The smallest native milestone is the Windows client with `backend=remote`, connecting to an
existing Linux Docker host. Split CMake targets so client builds do not compile Linux container
helpers with a Windows compiler. Isolate DBus/desktop menus, replace local POSIX signals and
process-group lifetime handling with Windows equivalents, and remove assumptions that a local
POSIX shell and `/proc` exist. Remote shell commands can still execute on the Linux host. Package
the exact Qt/FFmpeg dependencies, adb and required transport tools with their licences and source
obligations, then add a native Windows build/CI job. This is a port, not a packaging-only task.

For Android running on the same Windows machine without WSL, the most compatible proposal is a
small, prebuilt **Linux appliance** containing Docker Engine and a binder-capable kernel. The
native client connects to it as `backend=remote`. Its provisioner/lifecycle is a separate packaging
concern; do not restore the removed `vm` backend or its old passthrough assumptions. Users should
download the appliance and Android image, without compiling either one. Shipping the kernel and
other appliance software adds their own source/notices obligations to the image audit.

[QEMU's WHPX backend](https://www.qemu.org/docs/master/system/whpx.html) supplies hardware-assisted
virtualization on Windows and requires the Windows Hypervisor Platform optional feature. It is
the first local-appliance candidate to evaluate; select and test a pinned stable QEMU build rather
than assuming the development manual describes every binary. This requires neither WSL nor
nested CPU virtualization: Android remains a container inside the guest. WHPX CPU acceleration
does not establish an accelerated Android GPU path. QEMU's
[GPLv2 and component licences](https://www.qemu.org/docs/master/about/license.html), bundled
firmware and libraries must also be included in the distribution audit.

[Hyper-V](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/get-started/install-hyper-v)
is another appliance host with edition and hardware requirements; the full Hyper-V role is not
available on Windows Home. That restriction must not be assumed to describe the separate WHPX
API's availability. Test the intended Windows editions and optional features explicitly before
promising Home support. A direct bootable Android VM is a larger alternative requiring a new
boot/device integration; the current container archive is not a bootable disk image.

Graphics is the largest local uncertainty. Remora currently expects Linux DRM nodes, Mesa/Venus,
buffer sharing and host codecs. Neither WHPX nor a generic virtual display proves those paths
work in the appliance. Start with guest software rendering/encoding and software client decode.
The [current decoder](../src/mirror/Decoder.cpp) implements CUDA and VAAPI selection, without a
D3D11VA path; Windows hardware decode needs separate implementation/validation. Assess a Windows
graphics bridge only after boot/connect works. Do not promise Linux-equivalent gaming performance
or banking/DRM compatibility. Windows on ARM is outside this x86_64 assessment.

Native-client acceptance requires observed GUI startup, video/audio/input/clipboard/recording,
remote reconnect and correct child-process cleanup. Appliance acceptance additionally includes
provisioning, binderfs, Android boot, app installation, persistent data, versioned updates/rollback,
sleep/resume, Windows reboot recovery and clean shutdown. No Windows run or performance estimate
was produced here. The first deliverable should be a bounded native-client build and remote
connection experiment; local graphics is a separate go/no-go result.

## A redistributable image

[AOSP uses several licences](https://source.android.com/docs/setup/contribute/licenses), mostly
Apache-2.0, with exceptions. Inventory actual installed components and their individual
obligations; an AOSP/LineageOS attribution alone does not cover the complete image.

| Component | Public treatment |
|---|---|
| Remora, Android/LineageOS modules, Mesa and other open-source dependencies | Include after exact source/version, licence, notices and applicable source obligations are accounted for. |
| Magisk, if retained | GPLv3 redistribution is possible with complete corresponding source for the exact binary, changes and required build/install scripts. A root-free variant is an optional product choice requiring boot validation. |
| GApps / Play Store / Play services | Exclude without a Google agreement permitting the exact offering. Hosting a separate proprietary OCI layer is still redistribution. |
| ARM translator (`libnb` / `libndk_translation`) | Exclude without a redistribution grant. The current record establishes none. ARM-only native apps then cannot run on this x86_64 image. |
| Widevine | Exclude without applicable permission. Installing Play would not grant a CDM redistribution licence or secure video path. |
| Shamiko or another unlicensed payload | Exclude. Public availability and checksums do not grant redistribution rights. |
| Identity / attestation spoofing | Exclude from the compliant offering. The module's code licence does not confer Google licensing, certification or service access rights. |

Account for **every** shipped GPL/LGPL component, not just Magisk. Under
[GPLv3 sections 1 and 6(d)](https://raw.githubusercontent.com/topjohnwu/Magisk/v30.7/LICENSE),
corresponding source includes the required build/install material, and equivalent source access
must be offered beside the binary with clear directions if hosted elsewhere. A version label or
upstream homepage alone is insufficient evidence. Archive exact source and patches under project
control. Users need not download the source to use the image.

Build from controlled inputs and inspect the filesystem and **all shipped layers**: a later
deletion retains earlier bytes. Check stale outputs, APKs, native-bridge libraries, init/vendor
services and extensionless files. Deliberately place disallowed private payloads on the builder
to test exclusion. The public-source-tree filter and Shamiko-only gating test are insufficient.

## Hosting

The smallest integration change is a project-owned HTTPS archive endpoint retaining the current
index format, accompanied by authenticated digest, capability and source metadata. A web server
or object store can hold immutable versioned `docker save` archives and source bundles. Retain
rollback versions. The project's roughly 3 GB Docker image size is not a measured compressed
download size; measure real artifacts before budgeting storage/egress. No host or price is selected.

[GHCR](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry)
is an optional OCI/Docker registry with public anonymous pulls, a current 10 GB layer limit and
10-minute upload timeout. For operating our own server,
[CNCF Distribution](https://distribution.github.io/distribution/about/deploying/) supports a
registry deployment; production needs TLS and controlled writes. Neither changes image rights.
The existing archive index is not an OCI registry client: fetching requires explicit integration
or a documented pull/retag path.

[GitHub release assets](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)
must each be under 2 GiB. Measure compression before depending on a single archive fitting.
HTTPS storage or OCI avoids that per-file constraint.

Bind each image digest to exact ABI/features, source manifest, patches, licences/notices, SBOM
and signed provenance. Authenticate official metadata and verify archives before `docker load`,
or verify OCI digest/signature before promotion. The current importer needs work for this
contract. Keep local proprietary derivatives/caches outside publication jobs. Validate anonymous
download, verified import, boot, upgrade and rollback from a fresh host.

## Google Play overlays and permission

### Selected installer model and remaining risk

[Genymotion documents](https://docs.genymotion.com/features/opengapps/) a base without GApps and a
built-in Open GApps download/install action after the user accepts a disclaimer. Its documentation
explicitly disclaims liability for that download, installation and use. This is evidence of an
established workflow, not a transferable permission for Remora.
Genymotion's [support guidance](https://support.genymotion.com/hc/en-us/articles/4414586104977-I-can-t-find-Google-Play-Store-Where-is-it)
also warns that the public Open GApps site has no packages for Android 12 and above and directs
users to its own integration. Copying the workflow does not identify an Android 17 payload source.
[Open GApps states](https://opengapps.org/) that its packages grant no licence to Google's APKs and
that its prebuilt packages may not be publicly mirrored. A disclaimer communicates the limitation;
it does not grant rights. This evaluation cannot estimate the probability of a complaint, service
restriction or enforcement action from these sources.

The selected Remora design is:

1. Publish the separately audited base and its source/notices. Keep proprietary add-ons out of
   the hosted artifact and every shipped layer.
2. Offer an explicit optional Google Play installation action with the named package provider,
   its applicable terms and the known licensing/certification limitations. No Google credentials
   pass through Remora's hosting service.
3. Download directly from the identified provider to the user's Docker/build host. No Remora
   payload mirror, proxy or shared public cache is part of this design. Identify a compatible
   source before enabling the download; do not reuse the current unrecorded blob provenance.
4. Validate the Android release, ABI, expected hashes, relevant APK signing identities and the
   complete permission/dependency payload before applying COPY-only layers to a local derivative.
   The ARM translator requires its own source/rights and technical assessment; GApps permission
   would not cover it automatically.
5. Preserve the base, validate on fresh app data, and provide backup/rollback for supported
   migrations. Keep the derivative out of release jobs and do not add identity/attestation
   spoofing to make certification appear successful.

There are three remaining practical risks. **Licensing/service use:** permission remains
unresolved despite separating hosting from installation. **Compatibility:** Store sign-in,
updates, app catalogues and integrity-dependent apps can fail on uncertified systems. **Package
trust and recovery:** a privileged third-party payload needs verification and a tested rollback.
Direct downloads reduce Remora's role in distributing the binaries; they do not establish a
guarantee that the complete flow satisfies every applicable rule. The source/terms review is
recorded in [the audit](GOOGLE_PLAY_SOURCE_TERMS.md). The owner selected implementation without
Google outreach; technical verification and honest terms/certification notices remain required.

### Permission and technical evidence

[Google requires a GMS licence](https://www.android.com/gms/); GMS is separate from AOSP.
[Android compatibility](https://source.android.com/docs/compatibility/overview) provides potential
licensing eligibility, not an automatic grant. Google's
[certification help](https://support.google.com/googleplay/answer/7165974?hl=en) ties Google-app
eligibility to certified devices. The applicable
[Play terms](https://play.google.com/intl/en-US_us/about/play-terms/index.html) require a supported
device and compatible software. On the evaluation date, that page announces a further terms
change for November 17, 2026; recheck effective terms before release.

A local overlay avoids Remora distributing Google binaries only if those bytes are never hosted,
publicly cached, proxied or republished by Remora. It does not establish a user's right to acquire,
copy or use them on this system. An unofficial GApps download, Google account, acknowledgement
checkbox, ownership of a licensed phone or Android ID registration is not evidence of a
transferable GMS licence. No applicable Google grant for this exact installer was established
here. Proceed with the selected optional integration without representing it as Google-authorized
or certified. This records the published-terms limitation, not a finding about every conceivable
personal install in every jurisdiction.

**Correction from the byte-level audit:** the current payload is not ARM64-only; GmsCore and
Phonesky contain x86_64 code. The new LiteGapps Android 17 candidate also supports x86_64. The
incorrect `gapps` → `arm_translate` model dependency was removed after a new private bridge-free
A17 instance booted and opened the unauthenticated Store. That establishes startup independently
of the historical bridge-bearing test; it does not establish account/service access or a complete
public-base release audit. A proprietary translator is a separate ARM-app compatibility feature.

A future permitted add-on should validate origin, Android release, ABI, signatures, hashes,
dependencies and privileged-permission XML; refuse microG conflicts; use COPY-only layers and
leave the downloaded base immutable. Android's shell cannot run during these Docker builds:
its APEX runtime is mounted only at boot. Test fresh data first. Established-instance migration
needs backup and rollback rather than implicitly replacing `/data`. Validate the actual public
base plus permitted add-ons, including package services and Store/account behaviour allowed by
the agreement, without compiling AOSP.

Report licensing, Store access, certification and
[Play Integrity](https://developer.android.com/google/play/integrity/verdicts) separately.
Registration and a spoofed Pixel identity grant no certification. Banking, DRM and games can
impose further checks. Google Play Games' virtual integrity verdict documents no general Remora
entitlement. Nor are SDK Play images an extraction loophole: the
[SDK terms](https://developer.android.com/studio/terms) restrict proprietary-component use and
redistribution, separately from open-source licences.

An audited microG variant is an alternative for some app compatibility, with corresponding source
for packages that require it. It does not supply the Google Play Store. Its backend interactions
and compatibility need their own review; do not promise a certified equivalent.

## Follow-up work and scope

Implementation and validation are tracked in Beads:

- `remora-phvx`: redistributable variant, assembly gates, truthful ABIs and complete content audit.
- `remora-qksj`: verified hosting/import metadata; depends on the image audit.
- `remora-wzaa`: permitted GApps/ARM acquisition and use, then conditional add-on design.

`remora-7oih` (Windows feasibility) is deferred indefinitely at the owner's request.

This evaluation changes documentation only. It does not approve existing private tags for public
release, change runtime defaults, establish Google permission or claim a Windows test.
