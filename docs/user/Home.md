# Remora user guide

This guide explains how to install Remora, get an Android image (the ready-made one, or one you
build), run that image on your own machine or on another docker host, and use Android apps from
a Linux or FreeBSD desktop, or from the Windows preview. This page says what Remora is and lists every
other page.

## What Remora is

Remora is a desktop control plane for Android in Docker. It does three jobs from one place:

- **Fetch or build the image.** `remora images fetch` downloads the ready-made Android 17 image
  the project publishes. Or you pick Android 16 or 17 (LineageOS 23 or 24) and the features you
  want, and Remora builds that image from source.
- **Deploy it.** The same container runs on this machine (`bare`) or on any docker host you reach
  over ssh (`remote`). A read-only check tells you what the target still needs before you deploy.
- **Mirror it.** Remora's own client talks to an agent baked into the image: video, audio, input,
  clipboard, desktop-mode displays and the app list. Each Android app can open in a window of its
  own.

There are two front ends over one shared core: a headless command line (`remora <command>`) and a
QtWidgets workspace (`remora gui`). Everything the workspace does is also available from the
command line.

## What Remora is not

- **It is not an emulator.** Android runs natively in a container on your own kernel. There is no
  virtual machine and no CPU emulation. Apps that exist only for arm64 run through ARM translation,
  which the ready-made image leaves out. Windows has no Linux kernel, so there the container runs
  in a small Linux guest Remora manages itself, on the Windows Hypervisor Platform
  ([Windows](Windows.md)).
- **The ready-made image is not every feature.** It is x86_64 only and carries no Google apps, ARM
  translation, Widevine or root. Google Play, root, ARM translation and Widevine are added locally,
  on your own machine, or come with an image you build yourself. Building one takes about 450 GB of
  free disk, 32 GB of RAM (64 GB recommended) and an afternoon for the first build. See
  [Requirements](Requirements.md).
- **The phone app is a preview.** It opens one app at a time, from this computer or from a
  device, on an Android 10 or later phone ([Installation](Installation.md#the-phone-app)).

Remora runs on Linux x86_64, FreeBSD 15 amd64 and the 64-bit Windows 10 and 11 preview.
FreeBSD uses a Linux Docker host over SSH. Remora is licensed under GPL v3. An image you build is not
automatically yours to share: [NOTICE](../../NOTICE) explains which features make an image
non-redistributable.

## Start here

| page | what it covers |
|---|---|
| [Requirements](Requirements.md) | What you need to run Android, to build the image, and to build Remora itself |
| [Installation](Installation.md) | Linux packages, the FreeBSD package, the Windows installer, source builds and download verification |
| [Getting Started](Getting-Started.md) | The first run from `remora check` to apps in your menu |
| [Windows](Windows.md) | Remora on Windows 10 and 11: the guest Android runs in, what the PC needs, what differs |

## The Android image

| page | what it covers |
|---|---|
| [Building the Image](Building-the-Image.md) | Android versions, the Mesa payload, third-party payloads, `remora build --source` |
| [Image Features](Image-Features.md) | Every build feature, with its status and what it needs |

## Running Android

| page | what it covers |
|---|---|
| [Deploy Targets](Deploy-Targets.md) | `bare` and `remote`, readiness checks, networking, adb exposure, several instances |
| [GPU Acceleration](GPU-Acceleration.md) | NVIDIA through the Venus render server, Intel and AMD, software rendering, hardware video |
| [Mirroring](Mirroring.md) | The mirror client and the in-image agent, recording, picture-in-picture, desktop-mode displays |
| [Android Apps on the Desktop](Android-Apps-on-the-Desktop.md) | Per-app windows, the application menu, sleep and wake, the login service, APKs |

## Reference

| page | what it covers |
|---|---|
| [CLI Reference](CLI-Reference.md) | Every `remora` command with its arguments |
| [Configuration](Configuration.md) | `~/.config/remorarc`: layering, profiles and every key |

## When something goes wrong

| page | what it covers |
|---|---|
| [Troubleshooting](Troubleshooting.md) | `remora check`, `remora doctor`, and the documented failure modes with their fixes |
| [Security Model](Security-Model.md) | What Remora trusts, and how to report a vulnerability |

## Deeper documentation in the repository

- [FEATURES.md](../FEATURES.md): the full catalogue of build features, source patches and runtime
  settings.
- [MIRROR_AGENT.md](../MIRROR_AGENT.md) and [MIRROR_PROTOCOL.md](../MIRROR_PROTOCOL.md): the device
  half of the mirror and the wire format it speaks.
- [HW-VIDEO-DECODE.md](../HW-VIDEO-DECODE.md): hardware video inside Android.
- [IMAGE_MIRROR.md](../IMAGE_MIRROR.md): what each image tag carries and what that means for
  redistribution.
- [CHANGELOG.md](../../CHANGELOG.md): release notes.
