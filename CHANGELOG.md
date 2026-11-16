# Changelog

All notable changes to Remora are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Remora uses
calendar versions, `YYYY.MM.PATCH`: the year and month of the release, then a patch number.

## [Unreleased]

### Fixed

- **Remora builds without Python again.** Since 2026.11.1 configuring the build required a
  Python 3 interpreter, although only the test suites run it, so a machine or a clean chroot
  without Python could not build Remora. CMake now looks for Python instead of requiring it.
  Without it, Remora still configures and builds: the three Python suites are left out and
  configure says so, and the prereqs suite skips its two image-helper cases. The `PKGBUILD` lists
  `python` and `openssl` in `checkdepends`, and the ebuild lists them as `USE=test` build
  dependencies through `python-any-r1`. Their test runs and CI's configure with
  `-DCMAKE_REQUIRE_FIND_PACKAGE_Python3=ON`, so a missing interpreter fails there instead of
  quietly dropping suites.
- **The packages say what Python is for.** The `PKGBUILD`'s optional `python`, the ebuild's
  optfeature and the Requirements page named only input sharing. Fetching official images,
  checking for their updates and adding Google Play run it too, on the machine that runs the
  containers, and the image fetch and update check also run `openssl`. The Requirements page no
  longer says that Remora ships no image.

## [2026.11.2] - 2026-11-16

### Added

- **New public releases reach the device through its own updater.** For a profile that deploys
  Remora's official image, `remora update-watch` checks the signed release catalog every six
  hours and shows a newer release in Android's *System updates*, with its size. *Download and
  apply* downloads and verifies it, keeps the previous image for rollback, builds the Google
  Play layer again when the profile has one, and restarts the device on it with its data kept. A
  failed download or restart is reported on that screen. `remora images check` asks the same
  question from a terminal.

- **The release fetch helper can report its progress.** `vendor/release/catalog.py fetch
  --progress` prints a `REMORA_FETCH_PROGRESS=` line as each part of the archive arrives
  (`download <done> <total>`, in bytes of the signed size), or `cached <total>` when a cached copy
  is reused, then `check` and `import` as those steps begin. Without the flag its output is
  unchanged, and nothing passes the flag yet: *System updates* still shows no download progress.

- **Google Play as a profile option.** The Image page's **Active image** card has an **Add
  Google Play services** toggle, saved in the profile as `google_play` and the accepted
  `play_terms` version. Turning it on asks for the provider's terms. Each connect then builds, or
  reuses, the private local Play overlay over whatever image the profile resolves to, on the
  profile's Docker host, and runs the device from it. An image update therefore brings Play
  along, which a profile pinned to an overlay never did. `plan` names the image the overlay goes
  on. A connect refuses when the accepted terms are missing or older than the installer's, and
  does nothing for an image that already includes Google apps.

- **Releases attach a Gentoo ebuild.** Each release now carries `remora-<version>.ebuild` beside
  the `PKGBUILD`, for a local overlay. The release workflow runs `tools/test-gentoo-ebuild.sh`
  on the template for that version before attaching it. The ebuild has not yet been built by
  Portage.

### Changed

- **The host frame encoder accepts only the encoders Remora uses.** `remora-frame-encoder
  --codec` takes `hevc`, `h265`, `h264` or `avc`, or names one backend: `hevc_vulkan`,
  `hevc_nvenc`, `h264_vulkan`, `h264_nvenc`, `av1_vulkan` or `av1_nvenc`. It used to hand any
  other name to FFmpeg and open whatever encoder that FFmpeg had. Now it refuses the name before
  it opens a GPU, `--bench` included. Remora's own deploys pass `hevc` or `h264`, so they are
  unaffected.

### Removed

- **The Image library's Install Google Play… and Remove Play overlay… buttons.** They made a
  Play image a profile had to be pinned to by hand, beside the profile option that does the whole
  job. The option's images appear in the list as a **Google Play layer**, which Delete removes.
  `remora play-install` still makes a standalone Play image from the command line.

### Fixed

- **`remora service <profile> --remove` cleans up after a deleted profile.** It used to refuse
  with "no such instance" once the profile was gone, leaving no command that removed its login
  unit and its two watchers. It now works from the unit names alone, and also deletes each unit's
  `.service.d` drop-in directory. It no longer reports success when a unit could not be stopped
  or disabled: that unit's files are kept, the reason is printed, and the command exits non-zero.
  A name whose units would include another existing profile's units is refused.
- **Deleting a profile in the workspace also removes its services.** Its login unit and its two
  watchers, with their `.service.d` drop-ins, used to stay behind, so every login started
  `remora up` for a profile that no longer existed. Delete now disables and removes them first,
  the same way `remora service <profile> --remove` does, and says so in its confirmation. If any
  of them cannot be removed, the profile is kept and the reason is shown, so the delete can be
  retried. Images, containers and device data are still untouched.

- **A release update no longer leaves its download behind.** Every fetch of an official image —
  `remora images fetch`, the workspace's Official downloads and Android's *Download and apply* —
  kept the downloaded archive in `~/.cache/remora/images/` for good, several hundred MB per
  release. The archive is now removed once its image has been verified, imported and promoted;
  an import that fails keeps it, so the retry does not download it again. Fetching a release that
  is already installed now downloads and verifies it again. Older images and rollback tags are
  still kept, as before.
- **The in-Android updater works again on bare and remote devices.** Its directory was mounted
  only by the retired `vm` backend, so the *System updates* screen could neither see an update
  nor ask for one. Each container now gets its own directory, mounted where the app looks, and a
  container created without it is recreated on the next connect. Profiles on one host no longer
  share one Apply signal, which could have updated the wrong device.
- **A redeploy that fails no longer leaves the device down.** When a connect had to replace the
  profile's own running container, it removed the container before any deploy step ran, so a
  failure in preflight, the image check or the host prerequisites left nothing running. The old
  container is now replaced only when the deploy reaches the step that creates the new one.
- **Apply in Android now redeploys with the host's GPU probed.** `remora update-watch` redeployed
  without the live GPU probe that `remora up` takes, so on an NVIDIA (Venus) host the update
  failed its host prerequisites every time.
- **The image library lists each image once.** An image with several names, such as a fetched
  release under its tag and its version name, was a row per name, each showing the full size.
  It is now one row, its other names in a tooltip. Delete removes all of them, and the Active
  image list still offers each name.
- **h265 works with the public image again in the workspace.** The Image page and Connect both
  decided whether an image ships the hardware HEVC encoder from `hwc2` in its tag, which the
  public image lacks. The Image page reset the codec to h264, and Connect stopped with "No h265
  encoder in this image". Both now ask the image registry, as the mirror does.
- **The in-Android updater follows profile changes.** `remora update-watch` read the profile once
  at start, so **Apply** in Android redeployed the settings it started with, undoing any change
  made in the workspace since. It now reads the profile on every check. With Google Play on, it
  compares the image the Play overlay was built on, rather than reporting an update forever.
- **`remora reset-data` also clears the profile's application menu.** The menu refresh keeps a
  folder whose device suddenly reports under half its apps, which protects it from a list read
  mid-boot. After a reset, that guard kept a launcher for every deleted app. The next connect now
  builds the folder again from the fresh device.
- **`remora images check` and `fetch` without `--tag` now say that the tag is missing.** They
  downloaded the catalog first and then reported "Requested tag is absent from the authenticated
  catalog", as though the catalog were at fault. They now stop before any download and name the
  flag, as `rollback` already did.

## [2026.11.1] - 2026-11-14

### Fixed

- **A source build no longer deadlocks on a tree that has never been built.** Before, with
  `mesa_source` enabled and no Mesa payload, staging refused, and `build-mesa.sh` needed a libdrm
  that only a build could make. Now `remora build --source` builds just libdrm, its backends and
  the libva headers that script reads. It reuses them when they are already built. It then stops
  with a nonzero status and prints the `build-mesa.sh` command to run before building again. It
  does not run that script, build the image or record the build as done. A failure during this
  preparation stops everything after it. A `public_release` profile refuses this preparation
  unless those libraries are already built. With the payload present, the build runs as before.
  This route is tested offline with fake commands only. It has not yet been run on a fresh tree.

## [2026.11.0] - 2026-11-13

### Added

- **Optional Google Play, installed locally.** From the Image library or `remora play-install`:
  downloads a pinned LiteGapps Android 17 x86_64 package directly on the Docker host after
  explicit terms acceptance, verifies archive/APK hashes against the signature audit, and creates
  a separate COPY-only `remora-local:` image. Rejects incompatible or Google/microG-containing
  bases, retains receipts and the original image/data, and can remove unused overlays. Public
  base-image redistribution remains subject to the separate image audit.
- **A redistributable Android 17 image.** The `public_release` build profile produces
  `remora24:x86_64-public`: x86_64 only, with no Google apps, ARM translation, Widevine, root or
  Play spoofing. The profile refuses those features. The build clears the previous build's installed
  partitions and generates an SBOM, and assembly ends with a release audit
  (`vendor/source-build/public-audit.py`). The audit walks the built image's files, ELF
  architectures, APK packages, ABI properties and licences, and fails the build on any excluded
  payload.
- **Official prebuilt images.** `remora images list|fetch|rollback` and the Image library's
  **Official downloads** source fetch Remora's audited image from the project's GitHub release.
  Official images come from a signed catalog: the signature, validity window, archive SHA-256,
  Docker identity and tags are all checked before `docker load`, and the previously active image
  is kept for rollback. Both Docker image stores are supported, classic and containerd.
  `vendor/release/package-release.py` packages an audited image with its SBOM, notices, audit,
  provenance and corresponding source, and signs the catalog.
- **A Gentoo ebuild template.** `packaging/remora.ebuild.in` is an EAPI 8 ebuild (`~amd64`) that
  builds Remora from a release's source tarball with the Gentoo equivalents of the Arch package's
  dependencies, for use as `remora-<version>.ebuild` in a local overlay.
  `tools/test-gentoo-ebuild.sh [<version>]` checks it offline. Releases do not attach the ebuild
  yet, and it has not yet been built by Portage.

### Changed

- **The source build container now runs in `containers-remorabuild.slice`, and gets 4 GiB of
  swap instead of as much swap as its memory cap.** The slice name nests the build under
  `containers.slice`, so a host that limits memory for all its containers counts the build in
  the same budget. With a memory cap set in the Source build card, `--memory-swap` is now the cap
  plus 4 GiB, not twice the cap: a build that spilled far into swap could make the whole host
  unresponsive instead of failing. A build that needs more memory needs a higher cap, not more
  swap. Neither slice needs to exist beforehand; systemd creates it, with no limits, if the host
  does not define one.

### Fixed

- The host frame encoder now runs on the GPU that rendered the frames. It used to open the first
  render node, so on a boot that enumerated the Intel GPU before the NVIDIA one, NVENC failed with
  "opening the encoder: Bad address" and the mirror reported "exited immediately after 5 attempts".
  A mirror or encoder that dies is now reported with its own last log line, not another process's.
- Removed the incorrect mandatory ARM-translation dependency from `gapps`, after verifying the
  x86_64 APKs and a fresh bridge-free Android 17 boot with the Play Store's unauthenticated screen.
- Images built without `arm_translate` no longer carry the ARM bridge, and no longer advertise
  `arm64-v8a`. Assembly used to overlay the bridge whenever its payload was present, and the device
  config advertised the ABI regardless, so ARM-only apps installed and then crashed.
- Re-staging `device/remora` over a copy it could not fully remove (for example, root-owned files
  left by a staging run inside the builder) now refuses and names the fix. Previously it nested the
  new copy inside the old one and marked the broken result as current.
- The source-built Mesa now carries licence metadata, so the image's NOTICE and SBOM include Mesa,
  LLVM, expat and the Rust runtime that are linked into it.
- The mirror on a Google Play overlay (or any image Remora does not have registered) no longer
  dies on the VAAPI H.264 encoder where none can run, in guest rendering or on a GPU without VA-API
  encode: it now uses the software encoder there, as the registered `-hwc2` images already did.

## [2026.10.0] - 2026-10-02

### Added

- **Release packages.** Each release now publishes a source tarball, a `.deb` for Ubuntu 24.04
  (amd64) and an Arch Linux `PKGBUILD`, with SHA-256 checksums, a CycloneDX SBOM and a
  build-provenance attestation you can check with
  `gh attestation verify <file> --repo EtherAura/Remora`. The `.deb` carries no host frame
  encoder: Ubuntu 24.04's FFmpeg is too old to build it.
- **The native helpers are built with Remora.** `cmake --build` now also compiles `fwd` and
  `uinput-kbd`, which every container needs, and — when their libraries are installed — the host
  frame encoder (`host_encode`, FFmpeg 7 or newer) and decoder (`host_decode`). Before, only a
  separate `make -C vendor/native` built them, so a fresh build and install could be missing
  `fwd`. Configure reports which optional helpers it builds, and why it skipped any.
- `remora --version`.
- `adb_bind=` — the address a bridge-mode instance's adb port is published on (see Security).
- `remora reddit-login --cookie -` reads the session cookie from stdin.

### Changed

- **Mirror protocol v3, Remora's own wire format.** Every connection carries size-prefixed,
  typed records, so either side skips a message it does not know instead of dropping the
  connection, and when a video stream cannot start or fails, the agent says why. Pointer
  positions travel as fractions of the frame, so neither side needs the other's resolution. A
  client and an agent that speak different protocol versions refuse each other with a message
  naming what to update. **Images built before this release need a rebuild** for the new agent.
- **Android 17 no longer needs the host ashmem module.** Android 17 runs on memfd: it boots,
  renders WebView, decodes video in hardware and mirrors with no `/dev/ashmem` device at all, on
  guest, Intel and NVIDIA rendering alike. `remora check`, the workspace and the deploy preflight
  now ask for the module only for Android 16 images.
- **Guest-rendered profiles stream H.264.** Guest GPU mode has no hardware HEVC encoder, so a
  `gpu_mode=guest` profile now resolves to H.264 rather than asking for a codec that is not there.

### Fixed

- Stopping a mirror session in the middle of a frame no longer takes the device agent down with
  it.
- A video encoder that accepts frames but never produces output now gets one fallback to the
  default encoder, and then a stated error, instead of leaving the mirror waiting.
- `--help` or `-h` after any command prints the usage. Commands used to ignore it and run, so
  `remora up --help` deployed the active profile, and `remora gui --help` opened the workspace.
- Android 17 source builds failed at `repo sync`: four projects were pinned to commits that were
  never published upstream. They are pinned to their upstream parents now, and the one change
  those pins carried is applied as a patch instead.

### Security

- **adb is published on loopback for `bare` instances.** adb is a root shell into the device, and
  a bridge-mode instance used to publish it on every interface of the host, reachable from the
  local network. A `bare` instance now publishes it on `127.0.0.1` only; `remote` instances keep
  every interface, because the client connects from another machine, and `adb_bind=` narrows or
  widens either. Existing containers change when they are next recreated.
- **The Google checkin identity Remora keeps per instance is owner-only from the moment it is
  written**, and so is its directory. It used to be created with the default mode and tightened
  only afterwards, in a directory other users could list.
- **The Reddit session cookie never appears on a command line.** Logging the Reddit app in used to
  pass the cookie to `docker exec` (base64-encoded) and to `sqlite3`, where any user on the host
  could read it through `ps` while the login ran, and on a remote target it was staged in the
  docker host's `/tmp` under a predictable name with the default mode. It now travels through
  private files and stdin only, remote staging uses a private temporary file that is removed on
  every path, and the CLI takes the cookie on stdin (`--cookie -`).

## [2026.09] - 2026-09-28

First public release.

- **Compose the image**: pick Android 16 or 17 (LineageOS 23 / 24) and toggle each build feature,
  each shown at its honest status; Remora builds the image from source when you don't have it.
- **Deploy it where you want**: `bare` (docker on this machine) or `remote` (any docker host over
  ssh). Both reach a real GPU — the host's render node directly, or NVIDIA through a Venus render
  server.
- **Check readiness first**: a read-only probe of the target that names what is missing and how
  to fix it.
- **Deploy and connect** through a staged bring-up with per-step progress and captured errors,
  then mirror with Remora's own Qt/FFmpeg client and an agent baked into the image — video, audio,
  input, clipboard, desktop-mode displays and app listing.

[Unreleased]: https://github.com/EtherAura/Remora/compare/v2026.11.2...HEAD
[2026.11.2]: https://github.com/EtherAura/Remora/compare/v2026.11.1...v2026.11.2
[2026.11.1]: https://github.com/EtherAura/Remora/compare/v2026.11.0...v2026.11.1
[2026.11.0]: https://github.com/EtherAura/Remora/compare/v2026.10.0...v2026.11.0
[2026.10.0]: https://github.com/EtherAura/Remora/compare/edf8471e0a38447667502e70c19115ee6c014083...v2026.10.0
[2026.09]: https://github.com/EtherAura/Remora/commit/edf8471e0a38447667502e70c19115ee6c014083
