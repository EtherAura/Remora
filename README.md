<div align="center">

# Remora

**Android apps, at home on your Linux desktop.**

[![CI](https://github.com/EtherAura/Remora/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/EtherAura/Remora/actions/workflows/ci.yml)
[![CodeQL](https://github.com/EtherAura/Remora/actions/workflows/codeql.yml/badge.svg)](https://github.com/EtherAura/Remora/actions/workflows/codeql.yml)
[![Release](https://img.shields.io/github/v/release/EtherAura/Remora)](https://github.com/EtherAura/Remora/releases/latest)
[![GPLv3](https://img.shields.io/badge/license-GPLv3-blue)](LICENSE)
![Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-5a8868)

Fetch the ready-made Android 17 image or build Android 16 or 17 from source, deploy it in Docker, and open each app in its own window.

The ready-made image is a 716 MB download and takes 2.4 GB in Docker; a freshly started device uses about 1 GB of RAM. No source build is needed: see [Quick start](#quick-start).

[**Get started**](docs/user/Getting-Started.md) · [**Requirements**](docs/user/Requirements.md) · [**User guide**](https://github.com/EtherAura/Remora/wiki) · [**Downloads**](https://github.com/EtherAura/Remora/releases)

<img src="docs/workspace.png" alt="Remora workspace for composing an Android image, choosing a deployment target and connecting" width="1000" />
</div>

| Compose | Deploy | Connect |
|---|---|---|
| [Choose image features](docs/user/Image-Features.md) | [Local or remote Docker](docs/user/Deploy-Targets.md) | [Mirroring and desktop apps](docs/user/Android-Apps-on-the-Desktop.md) |

NVIDIA, Intel and AMD [GPU acceleration](docs/user/GPU-Acceleration.md), with a shared Qt workspace and CLI.

Support Remora on Patreon: https://www.patreon.com/EtherAura

## Quick start

The quickest way to Android is the ready-made image the project publishes. No source build needed.
It is a 716 MB download and takes 2.4 GB in Docker (allow about 3.1 GB of free disk while it is
fetched). A freshly started device used about 1 GB of RAM at its home screen, measured with
software rendering; the apps you install add to both
([Requirements](docs/user/Requirements.md#to-run-android)).

1. **Install Remora** and join the `docker` group: follow [Installation](docs/user/Installation.md)
   (the `.deb`, the Arch `PKGBUILD`, the Gentoo ebuild, or a build of the application from
   source).
2. **Fetch the ready-made Android 17 image** into Docker:

   ```sh
   remora images fetch --tag remora24:x86_64-public
   ```

3. **Check the host, select the image and deploy**: continue with
   [Getting Started](docs/user/Getting-Started.md). It covers fetching onto a remote Docker host
   over ssh, naming the fetched image in your profile (the default feature selection does not
   resolve to it), `remora check` and `remora up`.

Fetching the image does not install Remora, satisfy what `remora check` asks for, or start Android
([Requirements](docs/user/Requirements.md#to-run-android) lists what the host needs). The signed
catalog, hashes and rollback are described in [Official images](docs/user/CLI-Reference.md#official-images).

The published image is x86_64 only and carries no Google apps, ARM translation, Widevine or root,
so apps that ship only ARM native code refuse to install on it. Google Play is optional: it is
installed locally, on your own Docker host, as a separate image
([Optional Google Play](docs/user/CLI-Reference.md#optional-google-play)). Root, ARM translation and
Widevine can be added the same way, as a layer built on your own machine without a source build
([Add-ons](docs/user/Image-Features.md#add-ons-root-arm-translation-and-widevine)); ARM
translation and Widevine need payloads you supply yourself.

[Full feature overview](docs/Overview.md) · [Configuration](docs/user/Configuration.md) · [Troubleshooting](docs/user/Troubleshooting.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Discussions](https://github.com/EtherAura/Remora/discussions)

## Build your own image

Optional: instead of fetching the ready-made image, you can build an Android image from source, for example with features it leaves out. This is not the same as building the Remora application.
Building an Android image from source needs about **450 GB free disk** and **32 GB RAM** (64 GB recommended) for the first build.
These figures are for [the image build](docs/user/Requirements.md#to-build-the-android-image), not for installing Remora. Follow [building the image](docs/user/Building-the-Image.md).
