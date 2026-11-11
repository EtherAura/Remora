# Getting Started

This page walks through the first run end to end: checking the host, setting up a profile in the
workspace, building the image, deploying it, and finding your Android apps in the desktop's
application menu. It assumes Remora is installed and you are in the `docker` group: see
[Installation](Installation.md).

Remora is configure-first. You decide what the image contains and where it runs, check that the
target is ready, and only then deploy. The long step is the image build, so start it when you can
leave the machine alone for some hours.

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

## 2. Open the workspace and describe what you want

```sh
remora gui
```

In the workspace:

1. Pick the Android version: 16 or 17.
2. Tick the image features you want on the **Image** page. Each is shown with its status. Your
   selection resolves live to an image tag. [Image Features](Image-Features.md) describes each
   one.
3. Choose the target: this machine, or a docker host over ssh.
   [Deploy Targets](Deploy-Targets.md) explains the difference.
4. Tune the runtime and mirror settings if you want to. Every setting is optional and defaulted,
   so an empty profile still resolves to a bootable instance.

The workspace stores each profile in `~/.config/remorarc`. A profile created there is given its
own container name (`remora-<profile>`), its own data directory (`~/.remora-<profile>`) and the
lowest free host adb port from 5555 upward, so several profiles can run side by side. The file is
plain INI and hand edits are fine: see [Configuration](Configuration.md).

To see what a profile resolves to without deploying anything:

```sh
remora plan
```

This prints the resolved `docker run` command and the mirror command, with notes about anything
the deploy would change or refuse.

## 3. Prepare the build inputs

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

## 4. Build the image

Use *Build from source…* on the Image page, or from a terminal:

```sh
remora build --source
```

Both run the same chain. The command prints the profile, the source tree, the tag it will build
and the number of patches, then streams the build to the terminal and to `/tmp/remora-build.log`.
It is a multi-hour build the first time. Ctrl-C stops it.

When it finishes, the image is tagged with a `-src` suffix. *Promote to active* on the Image page
drops the suffix, which gives the image the name that the profile resolves to.

## 5. Deploy and connect

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

## 6. Find your apps in the menu

After each connect, Remora refreshes a launcher folder for the profile in your desktop's
application menu, with one entry per installed Android app, each under its real icon. Choosing an
entry opens that app in a window of its own. The folder also carries entries for the whole device:
*Android (mirror)*, *Android (desktop)* and *Android (PIP)*.

[Android Apps on the Desktop](Android-Apps-on-the-Desktop.md) covers per-app windows, sleep and
wake, and starting the device at login.

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
