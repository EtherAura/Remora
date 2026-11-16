# Getting Started

This page walks through the first run end to end: checking the host, fetching the ready-made
Android image, setting up a profile in the workspace, deploying it, and finding your Android apps
in the desktop's application menu. It assumes Remora is installed and you are in the `docker`
group: see [Installation](Installation.md).

Remora is configure-first. You decide what the image contains and where it runs, check that the
target is ready, and only then deploy. The quickest way to an image is the ready-made one the
project publishes. Building your own image from source is optional, and comes
[after the walkthrough](#build-your-own-image).

## 1. Check the host

```sh
remora check
```

The check is read-only. It prints one row per prerequisite, each marked ok or MISSING with the
remedy beside it, and ends with a verdict: `READY to deploy.` or a statement that the target is not
ready. Fix what it names (the `docker` group, the docker daemon, `adb`, and so on) and run it
again.

With no arguments it reports on the active profile. It names the target and the profile it read,
so you can see which one the report is about. To check another profile, name it:
`remora check <profile>`.

Fetching an image does not change any of this: it installs neither Remora nor what `remora check`
asks for. [Requirements](Requirements.md#to-run-android) lists what the host needs.

## 2. Fetch the ready-made image

```sh
remora images fetch --tag remora24:x86_64-public
```

This downloads the audited Android 17 image the project publishes and loads it into Docker under
the tag `remora24:x86_64-public`. The signature, validity window, archive hash, Docker identity and
tag are checked before Docker loads it, and an image it replaces is kept for rollback. To deploy to
a docker host over ssh, fetch onto that host by adding `--host user@host`. In the workspace,
**Official downloads** adds the same source to the Image library.
[Official images](CLI-Reference.md#official-images) covers `list`, `check` and `rollback`.

The image is x86_64 only, and carries no Google apps, ARM translation, Widevine or root, so apps
that ship only ARM native code refuse to install on it. Google Play is optional:
`remora play-install` installs it locally, on your own Docker host, as a separate image over this
one (see [Optional Google Play](CLI-Reference.md#optional-google-play)). Root, ARM translation and
Widevine can be added the same way, as a layer built on your own machine without a source build
(see [Add-ons](Image-Features.md#add-ons-root-arm-translation-and-widevine)). ARM translation and
Widevine need payloads you supply yourself.

## 3. Open the workspace and describe what you want

```sh
remora gui
```

In the workspace:

1. Pick the Android version: 16 or 17. The ready-made image is Android 17.
2. Tick the image features you want on the **Image** page. Each is shown with its status. Your
   selection resolves live to an image tag. [Image Features](Image-Features.md) describes each
   one.
3. Choose the target: this machine, or a docker host over ssh.
   [Deploy Targets](Deploy-Targets.md) explains the difference.
4. Tune the runtime and mirror settings if you want to. Every setting is optional and defaulted,
   so an empty profile still resolves to a bootable instance.

The default features include ones the ready-made image leaves out, so they do not resolve to it.
To deploy the image you fetched, name it in the profile: `image_tag=remora24:x86_64-public`. An
explicit `image_tag` wins over the features (see
[How a selection becomes an image](Building-the-Image.md#how-a-selection-becomes-an-image)).

The workspace stores each profile in `~/.config/remorarc`. A profile created there is given its
own container name (`remora-<profile>`), its own data directory (`~/.remora-<profile>`) and the
lowest free host adb port from 5555 upward, so several profiles can run side by side. The file is
plain INI and hand edits are fine: see [Configuration](Configuration.md).

To see what a profile resolves to without deploying anything:

```sh
remora plan
```

This prints the resolved `docker run` command and the mirror command, with notes about anything
the deploy would change or refuse. Check that it names the image you expect.

## 4. Deploy and connect

Press **Deploy & Connect** in the workspace, or run:

```sh
remora up
```

This runs the bring-up chain in stages: `preflight`, `ensure-image`, `host-prereqs`, `provision`,
`boot-wait`, `netfix` and `connect`. Each step reports its progress, and a step that fails shows
the error output it captured, so you can see which stage stopped and why.

At the end you get a mirror window. It is an external process that keeps running when the
workspace or the command exits.

`remora up` is safe to repeat. If the instance is already running it attaches to it. If the device
is asleep it wakes it. Only when nothing is running does it deploy and boot.

## 5. Find your apps in the menu

After each connect, Remora refreshes a launcher folder for the profile in your desktop's
application menu, with one entry per installed Android app, each under its real icon. Choosing an
entry opens that app in a window of its own. The folder also carries entries for the whole device:
*Android (mirror)*, *Android (desktop)* and *Android (PIP)*.

[Android Apps on the Desktop](Android-Apps-on-the-Desktop.md) covers per-app windows, sleep and
wake, and starting the device at login.

## Build your own image

Instead of fetching the ready-made image, you can build an Android image from source, for example
with features it leaves out. This builds the Android image, not Remora: building the application
itself is covered in [Installation](Installation.md#from-source).

A source build is the long step. The first one needs about 450 GB of free disk and 32 GB of RAM
(64 GB recommended) and runs for hours; these figures apply to the image build only, and
[Requirements](Requirements.md#to-build-the-android-image) gives the details. Start it when you can
leave the machine alone for some hours.

### Prepare the build inputs

A source build needs things that Remora cannot download for you. Do these before you start the
build, because a build that is missing one stops before it compiles anything:

- **Build the Mesa payload** once on the host with
  `vendor/host-prereqs/mesa-android/build-mesa.sh`, and enable the `mesa_source` feature. Every
  source build requires it.
- **Supply the third-party payloads** for the features you ticked (Google apps, ARM translation,
  Widevine and others). `vendor/fetch-payloads.sh` reports what is present and what is missing.
- **Set the two host settings** that otherwise kill the build in misleading ways:
  `vm.max_map_count` and the systemd-oomd limits.
- **Point the profile at a source tree** (`source_tree`), where the Android source is checked out
  and compiled.

[Building the Image](Building-the-Image.md) covers each of these in detail, along with the disk,
memory and time the build needs.

### Build it

Use *Build from source…* on the Image page, or from a terminal:

```sh
remora build --source
```

Both run the same chain. The command prints the profile, the source tree, the tag it will build
and the number of patches, then streams the build to the terminal and to `/tmp/remora-build.log`.
It is a multi-hour build the first time. Ctrl-C stops it.

When it finishes, the image is tagged with a `-src` suffix. *Promote to active* on the Image page
drops the suffix, which gives the image the name that the profile resolves to. Then deploy it as
in [Deploy and connect](#4-deploy-and-connect).

## Day to day

| you want to | run |
|---|---|
| bring the device up, or reattach to it | `remora up` |
| attach to a running instance without any bring-up | `remora reconnect` |
| freeze the device and keep its session | `remora sleep` |
| see the live state of the target | `remora status` |
| open a shell on the device | `remora shell` |
| install an APK | `remora install <apk>` |
| collect diagnostics for a bug report | `remora doctor` |

**Stop** in the workspace tears the target down. The full command list is in the
[CLI Reference](CLI-Reference.md), and `remora` with no arguments prints it.
