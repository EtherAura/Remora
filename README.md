# Remora

**Run Android apps on your Linux desktop.** Android 16 and 17 (LineageOS 23 / 24) in a Docker
container, GPU-accelerated on NVIDIA, Intel and AMD, with every app in a window of its own.

[![CI](https://github.com/EtherAura/Remora/actions/workflows/ci.yml/badge.svg)](https://github.com/EtherAura/Remora/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/EtherAura/Remora)](https://github.com/EtherAura/Remora/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue)](LICENSE)
![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-informational)

Remora is a desktop control plane for Android in Docker: one place to compose the image, deploy it
and mirror it — *with every knob actually exposed.* It is **not an emulator**: Android runs
natively in a container on your own kernel, with no virtual machine and no CPU emulation, and
arm64-only apps run through ARM translation.

Remora is **configure-first**: compose your Android image à-la-carte, pick **where** to run it
(this machine, or any docker host over ssh), check the target is ready, then **deploy + connect**
— with a real staged-progress checklist and captured errors instead of a 200-line shell alias.

![The Remora config workspace](docs/workspace.png)

> **You build your own Android image.** Remora does not download one: it compiles LineageOS from
> source with exactly the features you pick. Plan on **about 450 GB of free disk**, **32 GB of RAM
> (64 GB recommended)** and an afternoon for the first build — see
> [What you need](#what-you-need).

Written in **C++20 / Qt 6** — a headless CLI and a QtWidgets workspace over one shared, unit-tested core.

---

## Highlights

- **Real NVIDIA acceleration.** Android renders on the proprietary NVIDIA driver: ANGLE → Vulkan →
  Venus → a vtest render server on the host → your GPU. One command (`remora venus-build`) builds
  and installs the host half, and deploys turn it on automatically. Measured at 3760×1992, frame
  times stay at 5 ms from p50 to p99 with 1 ms of GPU time — the same as at 720p — and zero jank.
  Intel and AMD render nodes work directly, and a GPU-less host falls back to SwiftShader.
- **Its own mirror, end to end.** A Remora agent is baked into the image as an init service, and
  a Qt/FFmpeg client talks to it over Remora's own wire protocol: video, audio, input, clipboard,
  desktop-mode displays and app listing. Frames can go straight from Remora's own hwcomposer to a
  hardware encoder on the host GPU, and the client decodes on NVDEC or VA-API. The mirror runs in
  its own systemd scope, so it outlives the workspace.
- **Hardware video inside Android.** A VA-API Codec2 component in the image decodes AVC, HEVC, VP9
  and AV1 and encodes AVC and HEVC on the GPU; on NVIDIA hosts decoding can be handed to the
  host. Widevine L3 is available for DRM apps.
- **Android apps on your desktop.** Every installed app gets a launcher entry with its real icon,
  grouped per profile, and opens in a window of its own (`remora app`), alongside a desktop-mode
  display, a picture-in-picture mirror, and sleep/wake that freezes the whole device and resumes
  it in about a second — automatically when no window is open, if you like.
- **Build exactly the Android you want.** Android 16 or 17 (LineageOS 23 / 24), built from source
  with the features you pick: Google apps or microG, arm64 app translation, Magisk root, Play
  integrity spoofing, a host camera, a host microphone, sensors, CPU-tuned builds. No kernel
  module is needed for Android 17: shared memory runs on memfd.
- **Run it anywhere.** The same deployment runs on this machine or on any docker host over ssh,
  and several instances can run side by side, each with its own data.

---

## What it does

- **Compose the image** — pick **Android 16 or 17** (LineageOS 23 / 24), then toggle each build
  feature à-la-carte, shown at its *honest status* (GApps `OK`, ARM translation `CAVEAT`,
  container-compat `MANDATORY`). Your selection resolves **live** to an image tag, and Remora builds
  that image from source when you don't have it yet. Full catalog:
  **[docs/FEATURES.md](docs/FEATURES.md)**.
- **Pick a deploy target** — `bare` (local docker) or `remote` (any docker host over ssh). Both
  reach a real GPU: the host's render node directly, or NVIDIA via a Venus/vtest render server.
- **Check readiness** — a read-only, per-target probe tells you exactly what's missing (docker
  group, render node, ssh reachability, …) with remedies.
- **Deploy + connect** — runs the proven bring-up chain (`preflight → ensure-image → host-prereqs →
  provision → boot-wait → netfix → connect`), streams per-step progress + captured stderr, and
  launches an external **mirror** window that survives Remora exiting. **Reconnect** attaches to an
  already-running instance; **Stop** tears the target down.
- **Mirror it** — `remora mirror` is an in-house Qt/FFmpeg client, and the device half is a Remora
  **agent baked into the image** (an init service, no per-session push): video, audio, input,
  clipboard, desktop-mode displays and app listing over Remora's own wire protocol. See
  **[docs/MIRROR_AGENT.md](docs/MIRROR_AGENT.md)**.

---

## What you need

**To run Android:** Linux on x86_64 with Docker, a kernel that provides binder (binderfs — most
desktop kernels do), `adb`, and membership of the `docker` group. A GPU is optional: Intel and AMD
render nodes are used directly, NVIDIA's proprietary driver through the Venus render server, and
with no GPU Android renders in software. Android 17 needs no kernel module; an Android 16 image
also needs the IBT-fixed `ashmem_linux.ko`. For NVIDIA, `remora venus-build` builds and installs the
render server (see `vendor/host-prereqs/venus-nvidia/`). A `remote` target needs the same on the
docker host. Each image takes about 3 GB in Docker, and each instance's Android data grows with the
apps you install. `remora check` probes all of this read-only and names what is missing; the full
table is in [docs/FEATURES.md](docs/FEATURES.md).

**To build the Android image** — needed once per Android release and feature set, because Remora
ships no prebuilt images. Two inputs come from you rather than from the download: the Mesa graphics
stack, built on the host once by `vendor/host-prereqs/mesa-android/build-mesa.sh`, and the
third-party payloads behind features like Google apps, ARM translation and Widevine, which Remora
does not redistribute — [vendor/PAYLOADS.md](vendor/PAYLOADS.md) says what each one is and where it
comes from. A build that is missing either stops and says so before it starts compiling.

| | |
|---|---|
| **Disk** | About **450 GB** free, on an SSD if you can. One release's source tree and build output measured 470 GB here after many rebuilds (99 GB source mirror, ~100 GB checkout, 269 GB of build output); add ~20 GB for the Mesa toolchain built on the host and ~3 GB per finished image. |
| **Memory** | **32 GB** of RAM at minimum, **64 GB** recommended, plus swap. The build's executor alone peaks around 21 GB, and every Java/metalava step running beside it takes about 4 GB more. |
| **Time** | The first build downloads about **100 GB** of source, then compiles for **3½ to 13 hours** on a 24-thread i9-12900K, depending on how many jobs you give it. Later rebuilds take under an hour (47 minutes here, 29 of them compiling). |

The image build also needs two host settings, and both fail in ways that
point at the wrong thing:

```sh
# soong opens a lot of files; too low and it dies with "newosproc", which is NOT an OOM
sudo sysctl -w vm.max_map_count=1048576

# systemd-oomd kills the build container at exit 137 and logs to the JOURNAL, not dmesg —
# so `dmesg | grep oom-kill` shows nothing while your builds keep dying
systemctl is-active systemd-oomd && journalctl -u systemd-oomd --since -1h
```

Raise `SwapUsedLimit` in `/etc/systemd/oomd.conf`, or set `ManagedOOMPreference=avoid` on the
slice the build runs in. Remora's build preflight warns about both before it starts.

**To build Remora itself:** a C++20 compiler, **CMake ≥ 3.20**, **Qt 6** (`Core`, `Widgets`, `Test`, `Network`,
`DBus`, `OpenGLWidgets`, `Multimedia`, `WebEngineWidgets`), FFmpeg's development libraries
(`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`) and FFmpeg's NVIDIA
codec headers (`ffnvcodec` — headers only; the mirror loads the driver at runtime, so no NVIDIA
GPU is needed to build). The build also compiles the small native helpers in `vendor/native`; two
of them are static, so they need your C library's static archive (`libc.a`, part of glibc's
development files on most distributions). Two more are optional and built when their libraries are
found: the host frame encoder (`host_encode`) needs FFmpeg 7 or newer (`libavfilter`) and the Vulkan
headers, and the host frame decoder (`host_decode`) needs `libva`. CMake says which it built.

Gentoo:

```sh
sudo emerge -av dev-qt/qtbase:6 dev-qt/qtmultimedia:6 dev-qt/qtwebengine:6 \
    dev-build/cmake media-video/ffmpeg media-libs/nv-codec-headers dev-util/android-tools
```

Arch:

```sh
sudo pacman -S qt6-base qt6-multimedia qt6-webengine cmake ffmpeg ffnvcodec-headers android-tools
```

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake qt6-base-dev qt6-multimedia-dev qt6-webengine-dev \
    libgl1-mesa-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libswresample-dev libffmpeg-nvenc-dev adb
```

---

## Install

Every [release](https://github.com/EtherAura/Remora/releases/latest) carries a source tarball, a
`.deb` for Ubuntu 24.04+ / Debian and a `PKGBUILD` for Arch, with SHA-256 sums, an SBOM and a
Sigstore build-provenance attestation.

**Ubuntu 24.04+ / Debian**

```sh
ver=$(curl -fsSL https://api.github.com/repos/EtherAura/Remora/releases/latest \
      | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')
curl -fLO "https://github.com/EtherAura/Remora/releases/download/v$ver/remora_${ver}_amd64.deb"
gh attestation verify "remora_${ver}_amd64.deb" --repo EtherAura/Remora   # optional, needs gh
sudo apt install "./remora_${ver}_amd64.deb"
```

The `.deb` is built against Ubuntu 24.04's FFmpeg 6.1, which is too old for the host frame encoder,
so `host_encode` is not available from it; mirroring encodes on the device instead.

**Arch**

```sh
mkdir remora && cd remora
curl -fLO https://github.com/EtherAura/Remora/releases/latest/download/PKGBUILD
makepkg -si
```

**Any other distribution (Gentoo, Fedora, …), from source** — install the build dependencies listed
in [What you need](#what-you-need), then:

```sh
git clone https://github.com/EtherAura/Remora.git
cd Remora
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DREMORA_INSTALLED_VENDOR=ON
cmake --build build
sudo cmake --install build
```

**Then, whichever way you installed it:**

```sh
sudo usermod -aG docker "$USER"   # then log out and back in
remora check                      # what this machine still needs, with the fix for each
remora gui
```

---

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces a single `build/remora` binary (CLI + `gui` subcommand), the native helpers it
deploys, and the test executables. `remora --version` prints the version.

---

## Launch

**GUI** — the config workspace:

```sh
./build/remora gui
```

**CLI** — everything the GUI does, headless (great over ssh):

```sh
./build/remora <command>
```

| command | what it does |
|---|---|
| `remora check [--backend bare\|remote] [--ssh-host user@host]` | per-target deploy readiness — what's missing + how to fix |
| `remora plan [--backend …]` | dry-run: print the resolved `docker run` + mirror command vectors |
| `remora status [--backend …]` | read-only live state (JSON) of the backend + host capabilities |
| `remora up [<profile>]` | deploy + connect for real (staged chain, launches the mirror) |
| `remora reconnect [--backend …]` | attach to an already-running instance (adb + mirror only, no bring-up) |
| `remora build --source [<profile>]` | full AOSP source build of the profile's image — the same chain the GUI runs |
| `remora app <profile> <pkg>` | launch one Android app in its own window |
| `remora shell` / `logcat` / `install` / `shot` | adb conveniences against the profile's device |
| `remora gui` | the QtWidgets config workspace |

`remora` with no arguments lists every command (sleep/wake, systemd service, app-menu integration,
diagnostics bundle, APK transfer between profiles, …).

Config is stored per-instance in `~/.config/remorarc` (standard INI; hand-editable, layered
`[Defaults]` < `[Instance-<name>]`).

### Getting started

1. `remora check` → confirm the target is READY, and fix what it names.
2. `remora gui` → pick the Android version, tick the image features you want, choose the target, tune
   runtime/mirror.
3. **Build the image** (the Image page, or `remora build --source`) — the long step. Build the Mesa
   payload and supply any third-party payloads first; see [What you need](#what-you-need).
4. **Deploy & Connect** (or `remora up`) → watch the checklist, get a mirror window. Installed apps
   appear in your desktop's application menu, grouped per profile.

---

## Layout

| path | role |
|---|---|
| `src/core/` | **pure** domain (Qt Core only, no Widgets, no subprocess): config model, resolver, golden-tested command-builders, status parsers, the à-la-carte **feature + gating** engine, per-target **readiness** prereqs, custom-image **recipe** generator |
| `src/engine/` | execution: an injectable `Spawner` (QProcess/ssh), the staged chain, the `bare` / `remote` deployers, per-target mutual exclusion, and the `connect` orchestrator (spawner is injectable → fully testable offline) |
| `src/store/` | `remorarc` config persistence (QSettings INI) |
| `src/ui/` | the **QtWidgets** config workspace + worker `QThread`s (`MainWindow`, `Workers`, `App`) |
| `src/mirror/` | the mirror client (`remora mirror`): FFmpeg decode, audio, input, Remora's wire protocol |
| `src/main.cpp` | the CLI dispatcher + `gui` subcommand |
| `agent/` | the in-image mirror agent (Java, baked into the image by the `mirror_agent` feature) |
| `vendor/` | container/host scripts, the native `.c` helpers the build compiles, and the Android source-build tree (`device/remora`, `vendor/remora`, the c2-va codec, patch series, feature wiring). Third-party payloads are fetched, not committed — see `vendor/PAYLOADS.md` |
| `tests/` | offline Qt Test suites: golden argv vectors, parsers (against real captured fixtures), gating, readiness, and the backend chains (with a `FakeSpawner`) |

## Testing

```sh
ctest --test-dir build
```

Eight suites, green with **no hardware**: the subprocess/ssh layer is injectable, so nothing touches
Docker or a device.

## More

- **[docs/FEATURES.md](docs/FEATURES.md)** — the à-la-carte catalog (build-time vs runtime, defaults,
  deps, status), with each entry's honest status and what it actually delivers.
- **[docs/MIRROR_AGENT.md](docs/MIRROR_AGENT.md)** — the device half: what the in-image agent does,
  why it needs no reflection, and the wire protocol it speaks.

### Licence

Remora is **GPL v3** (see [LICENSE](LICENSE)). It builds on AOSP, LineageOS, Qt and FFmpeg —
[NOTICE](NOTICE) records that work and, importantly, explains when an image you build is *not*
redistributable: enabling Google Apps, Widevine or the ARM native-bridge means the result is
yours to run, not yours to share.

### Contributing and security

Bug reports and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Report
vulnerabilities privately, as [SECURITY.md](SECURITY.md) describes. Release notes are in
[CHANGELOG.md](CHANGELOG.md).

### Not built (deferred)

- **Prebuilt images** — there are none to download: building from source works end to end, and a
  published image mirror does not exist yet. [docs/IMAGE_MIRROR.md](docs/IMAGE_MIRROR.md) records
  what each tag would carry and what that means for redistribution.
- The **Android companion app** (a phone client) — future.
