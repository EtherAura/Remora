# Changelog

All notable changes to Remora are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Remora uses
calendar versions, `YYYY.MM.PATCH`: the year and month of the release, then a patch number.

## [Unreleased]

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

[Unreleased]: https://github.com/EtherAura/Remora/compare/v2026.11.0...HEAD
[2026.11.0]: https://github.com/EtherAura/Remora/compare/v2026.10.0...v2026.11.0
[2026.10.0]: https://github.com/EtherAura/Remora/compare/edf8471e0a38447667502e70c19115ee6c014083...v2026.10.0
[2026.09]: https://github.com/EtherAura/Remora/commit/edf8471e0a38447667502e70c19115ee6c014083
