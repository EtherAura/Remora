# Changelog

All notable changes to Remora are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Remora uses
calendar versions, `YYYY.MM.PATCH`: the year and month of the release, then a patch number.

## [Unreleased]

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

[Unreleased]: https://github.com/EtherAura/Remora/compare/v2026.10.0...HEAD
[2026.10.0]: https://github.com/EtherAura/Remora/compare/edf8471e0a38447667502e70c19115ee6c014083...v2026.10.0
[2026.09]: https://github.com/EtherAura/Remora/commit/edf8471e0a38447667502e70c19115ee6c014083
