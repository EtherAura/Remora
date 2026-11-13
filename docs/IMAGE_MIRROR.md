# Public Android image distribution policy

**Current policy:** publish only artifacts whose complete contents permit
redistribution, with required notices and corresponding source. The owner's requirement that
Remora respect licences and service terms **supersedes the accepted-exposure decision**
to mirror proprietary GApps, Widevine and ARM-translation variants. Tolerance by an upstream or
distribution by another project is not permission.

The policy is implemented by the `public_release` build profile, the release audit and the signed
catalog described below. Whether a given image has been published is recorded by the catalog
itself. Evidence and the original hosting/add-on design are in
[Image distribution feasibility](WINDOWS_AND_IMAGE_FEASIBILITY.md), research `remora-wqmn`.

## Current tag register

Checked against `imageTags()` in `src/core/Features.cpp`:

| A17 tag | components without an established redistribution grant | approved for public distribution |
|---|---|---|
| `remora24:x86_64-public` | none: built by the `public_release` profile | **Yes**, per build, once its release audit passes |
| `remora24:x86_64` | proprietary `arm_translate` | **No** |
| `remora24:x86_64-gapps` | ARM translator + GApps | **No** |
| `remora24:x86_64-gapps-wv` | ARM translator + GApps + Widevine | **No** |
| `remora24:x86_64-gapps-wv-hwc2` | ARM translator + GApps + Widevine | **No** |

The private tags may remain local variants; their names and feature availability are not release
approval. The public tag is the proven A17 profile minus every excluded payload: x86_64 only, with
the agent, source-built Mesa, VAAPI decode, sensors, camera and microphone. It has no ARM
translator, so apps that ship only ARM native code refuse to install. The download page says so.

## Public artifact requirements

- Exclude GApps, Widevine, the proprietary ARM translator and unlicensed payloads such as Shamiko
  unless a documented grant authorizes the exact distribution. Exclude identity/attestation
  spoofing from the compliant offering; a module's code licence does not grant Google licensing
  or service eligibility.
- Inventory every shipped component. Most AOSP/LineageOS modules are permissively licensed, but
  exceptions have their own obligations. Preserve licence texts, copyright and NOTICE entries.
- For each GPL/LGPL component, provide the required exact corresponding source, modifications
  and build/install material under its actual licence. Magisk, if retained, needs the exact
  version's source. A homepage or tag alone is not the whole release process. Link source access
  beside the binary and keep it available as required.
- Build from controlled inputs and inspect the full image and every layer. A later deletion
  retains earlier bytes. Check stale outputs and extensionless files, not just APK/library paths.
- Bind immutable artifacts to digests, capabilities/ABIs, source records, notices, SBOM and
  authenticated release metadata. Verify before import/promotion; validate fresh-host download,
  boot, agent connection, app launch, persistent data, upgrade and rollback.

### How a public image is made (bd remora-phvx)

The `public_release` build feature is a refusal contract, enforced at four points:

1. **Profile:** it conflicts with `gapps`, `microg`, `arm_translate`, `webview_pairip_fix`,
   `widevine_l3`, `magisk_root`, `play_spoof` and `shamiko`. `remora build --source` refuses the
   combination before anything is staged.
2. **Staging:** `stage-features.sh` refuses any of those payloads under `WITH_PUBLIC_RELEASE`, and
   writes `device/remora/remora-features.mk`. With it, the board and product config advertise the
   arm64 ABI and the bridge properties only when ARM translation is actually built in. They used to
   advertise them unconditionally.
3. **Build:** `do-build.sh` runs `m installclean` first. The image is packaged from the installed
   staging dirs, so a file a private build installed there would otherwise be packaged too. AOSP's
   own install-clean fires only when the product or variant changes, and both builds lunch
   `remora_x86_64-userdebug`. The build also runs `m sbom`.
4. **Assembly:** `assemble-image.sh` refuses the ARM, Widevine and spoof overlays outright. Earlier
   it overlaid the bridge whenever its directory existed. It then runs
   `vendor/source-build/public-audit.py` on the built image and removes the tag if the audit fails.

The audit walks the exported rootfs, so it sees a file that arrived by any route. It checks names,
and also content: every ELF's machine type, and every APK's package name and native ABIs. It reads
every ABI and native-bridge property in every prop file, and maps each installed file to its
licence the way the tree's SBOM generator does. A `proprietary` licence fails outright. AOSP's
`by_exception_only` and `not_allowed` conditions mean "needs review", not "cannot be
redistributed", so they pass only when the licence module is listed, with the reason taken from its
text, in `vendor/source-build/public-licence-review.json`. Copyleft (`restricted`, `reciprocal`)
licences become source obligations. The report, `<image id>.audit.json`, also carries the full file
inventory and its path-list digest, which is how a fresh and a reused-output build are compared.

LineageOS's extra projects (`bash`, `nano`, `vim`, `htop`, `rsync`, `ntfs-3g`, `libncurses`)
declare no licence in their build files, so neither the image NOTICE nor the SBOM knows them. For
any file without metadata, the audit falls back to the owning project's own `COPYING`/`LICENSE`,
so GPL code becomes a source obligation rather than an unexplained file. The packager ships those
licence texts in the notices bundle.

**Validated on the first public build:** the audit passes with no failures and 64 copyleft
packages. Thirteen files remain listed for review: eleven AOSP Apache-2.0 modules whose projects
declare no licence, the generated repo manifest, and Remora's two GPS feature declarations. The
image boots with only `x86_64` advertised, the bridge set to `0`, and no Google, microG or Magisk
package. The agent serves the mirror. An x86_64 APK installs and runs, while its arm64-only build
is refused (`INSTALL_FAILED_NO_MATCHING_ABIS`). Data survives the container being recreated. A
`play-install` overlay of it starts Google Play services as x86_64 and opens the Store, and
removing it keeps the base. Served from a local HTTPS mirror, the release passes `images list`,
`fetch` and `rollback`; a one-byte change to the catalog is refused.

**Published** as `remora24:x86_64-public-20261113.035540`, catalog sequence 1. An
anonymous `fetch` from GitHub imports it on both Docker image stores: the classic one, which names
the image by its config digest, and the containerd store a fresh Docker 29 install defaults to,
which names it by its manifest digest. The client checks that the manifest is in the verified
archive and carries the signed config, then accepts either ID. The first attempt on a clean
containerd daemon refused the image, because the client expected only the config digest.

Run against the private deployed image, the audit refuses it with 275 findings: the ARM guest
runtime and aarch64 ELFs, the bridge and ABI properties, GApps by package and by permission file,
Widevine, Magisk and the spoof stack. It refuses none of stock AOSP.

The current Android container ships no kernel or kernel modules, so it creates no kernel source
obligation itself. A distributed Linux VM appliance or kernel/module artifact has additional
obligations and needs its own audit.

## Hosting and the catalog (bd remora-qksj)

**Where:** GitHub Releases on the public repository, as one fixed release, `android-images`, at
`https://github.com/EtherAura/Remora/releases/download/android-images/`. The `Official downloads`
button adds that location as an image source, and `remora images list|fetch|rollback` reads it.
Each file must be under 2 GiB, and GitHub sets no limit on total release size or bandwidth
([About releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)).
Hosting therefore costs nothing, and the catalog is the only file that ever changes.

**What a release is:** `vendor/release/package-release.py build` turns one audited image into:

| asset | contents |
|---|---|
| `<name>.tar.xz` | `docker save` of the immutable version tag `remora24:x86_64-public-<build number>` |
| `<name>.spdx.json` | the product SBOM from the same build, checked by build fingerprint |
| `<name>-notices.tar` | every partition's `NOTICE.xml.gz`, as shipped inside the image |
| `<name>-audit.json.gz` | the release audit, with the full file inventory |
| `<name>-provenance.json` | fingerprint, build number, labels, pinned-manifest digest, patched projects |
| `<name>-src-*.tar.xz` | corresponding source: Remora's public tree, and every third-party project the audit found under a copyleft licence, as `git archive HEAD`. HEAD is the patched source that was compiled. |
| `index.json`, `index.json.sig` | the catalog, Ed25519-signed with the key whose public half is `vendor/release/image-signing-key.pem` |

The source is published beside the binary in the same release rather than offered on request, and
the catalog binds its hashes exactly as it binds the image's. Never delete a source asset while its
image is still listed.

**Verification order:** the client checks the catalog signature, its validity window (at most 93
days, so it is re-signed quarterly with `package-release.py resign`), and its sequence against the
last one seen. It then checks the image's tag and approval fields. It downloads into a cache named
by the signed digest, checking size and SHA-256 as it streams, and reads the archive's manifest,
OCI index and config: the config digest must equal the signed image ID, the only tag must be the
signed version tag, and the `remora.public_release` label must be present. Only then does it run
`docker load`. A truncated, corrupt, oversized, re-tagged, stale or foreign-signed artifact fails
before Docker is touched; `tests/test_public_images.py` pins each case. The previous image keeps a
`remora-rollback:` tag, and `images rollback` restores it. No Android data is touched.

**Budget, measured on the first packaged release:** 44 assets, 1.0 GiB in total. The image
archive is 716 MB (2.4 GB loaded). The SBOM is 28 MB. Corresponding source for the 64 packages
under a copyleft licence is about 300 MB, the largest being ICU and Mesa at about 80 MB each.
Packaging takes about 4 minutes. Each further release adds about 1 GiB of storage and nothing else;
prune old images from the catalog before deleting their assets.

**Third-party mirrors** keep the older `index.txt` contract (`<docker-tag> [<archive-file>
[<bytes>]]`, `#` comments). They cannot sign with Remora's key, and fetching from them is
unauthenticated, exactly as before. Only the official location is verified.

**Publishing** is `package-release.py publish`. It uploads every asset, and the catalog last, so a
client never sees an index naming assets that are not up yet. It is a separate, deliberate step.

## Optional local add-ons

**Chosen design:** use the Genymotion-style workflow: our audited prebuilt base, with
a separate opt-in action that downloads identified third-party packages directly to the user's
execution host and installs a local derivative. Keep those payloads, caches and derivatives out
of Remora's hosting/publication paths. This choice establishes the workflow, not Google approval
or a conclusion that all remaining licence/service questions are resolved. The detailed contract
and risks are in the feasibility report; `remora-wzaa` tracks delivery.

The Image library and [`remora play-install`](user/CLI-Reference.md#build) implement the optional
flow. They require explicit versioned terms acceptance, download the pinned LiteGapps package
directly on the local/SSH Docker host, verify the audited bytes, extract only 20 selected payload
files and add one local permission denial. A COPY-only layer uses an immutable base image ID;
`remora-local:` names and `org.remora.play.local_only=true` mark private derivatives. No build
archive, registry push or profile change is performed. Receipts remain in a private host cache;
unused-overlay removal retains the base and all Android data.

A fresh private bridge-free fixture booted and opened the unauthenticated Store. The feature
model no longer makes GApps depend on ARM translation. This establishes startup, not Google
permission or certification. The separate full-layer public-base audit is still required before
hosting. Legacy payload origins remain unrecorded in [PAYLOADS.md](../vendor/PAYLOADS.md).

Keeping payloads local can avoid Remora redistributing them; it does not itself establish
permitted acquisition, installation or service use. The owner nevertheless selected the
Genymotion-style integration without Google outreach: proceed with explicit user opt-in, the
named provider's terms and limitations, verified direct downloads and local-only derivatives
(`remora-wzaa`). A Google reply is not a project prerequisite. Do not claim Google approval,
licensing or certification from this choice. Android ID registration and attestation spoofing
do not grant those statuses. See the source/terms record for primary evidence.
