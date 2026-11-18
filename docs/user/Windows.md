# Windows

This page explains how Remora runs on Windows: where Android actually runs, what the PC needs,
the guest Remora manages for you, and what differs from Linux.

> **Status:** Remora for Windows is in development. It builds and runs, but there is no Windows
> release package yet, and it has not been tested on a range of PCs. Everything below describes
> the design as built.

## Where Android runs

Android needs a Linux kernel (binder, memfd, cgroups), and Windows does not have one. So Remora
for Windows runs its own small Linux system, the **guest**, and puts Android in it. You never set
it up or log in to it: Remora starts it the first time a profile needs it, deploys into it, and
leaves it running for the next connect, the way docker keeps running on Linux.

No Linux machine, WSL or virtual machine software of your own is needed. The guest is an Alpine
Linux system with a kernel built by Remora, run by the copy of QEMU that ships with Remora, on the
Windows Hypervisor Platform.

To the rest of Remora the guest is just a docker host: a profile whose target is **this PC** (an
empty `ssh_host`) deploys into the guest exactly as a [remote](Deploy-Targets.md) profile deploys
to any docker host over ssh. A profile with `ssh_host=user@host` still deploys to that host.

## What the PC needs

| | |
|---|---|
| Windows | 64-bit Windows 10 or 11 |
| Windows Hypervisor Platform | turned on (below) |
| CPU virtualization | Intel VT-x or AMD-V, enabled in the PC's firmware settings |
| Memory | the guest takes 6 GB while it runs (`guest_memory_mb`) |
| Disk | the guest's data disk grows with what you store, up to 256 GB |

To turn on the Windows Hypervisor Platform: **Settings → System → Optional features → More Windows
features → Windows Hypervisor Platform**, then restart Windows. As an administrator you can
instead run:

```bat
dism /online /enable-feature /featurename:HypervisorPlatform /all
```

If it is missing, the first connect stops with a message saying so, before anything else happens.

## Installing

Run `remora-setup.exe`. It installs Remora under Program Files, adds **Remora** to the Start menu,
and turns on the Windows Hypervisor Platform if it is off, in which case it asks you to restart
Windows. Uninstalling, from **Settings → Apps**, stops the guest, removes the program and the
profiles' Start-menu folders, and keeps `%LOCALAPPDATA%\remora`: the guest's data disk there holds
your Android images and data, and is yours to delete.

## The two programs

`remora-gui.exe` opens the workspace; it is what a shortcut should start, and it never shows a
console window. `remora.exe` is the same program for a terminal: every command on
[CLI Reference](CLI-Reference.md) works there.

## The guest

```sh
remora guest status           # running or stopped, its image, where its files are
remora guest start            # start it now (connect and up do this themselves)
remora guest stop             # power it down; Android inside stops with it
remora guest reset-system     # boot a fresh system disk next time; your data is kept
```

The guest keeps two disks in `%LOCALAPPDATA%\remora\guest`. The **system disk** is a thin layer
over the image that ships with Remora; an update replaces it. The **data disk** holds everything
that is yours: Android images, containers and each profile's `/data`. An update never touches it.

It is reachable only from this PC. Its ssh and adb ports are forwarded on `127.0.0.1` alone, with
a key Remora generated and a host key Remora pinned before the first connection.

### Settings

Set these in `[Defaults]`: there is one guest, shared by every profile.

| key | default | |
|---|---|---|
| `guest_memory_mb` | `6144` | memory the guest holds while it runs |
| `guest_cpus` | half this PC's threads, 2 to 8 | |
| `guest_accel` | `auto` (the Windows Hypervisor Platform) | `tcg` runs without it, for testing only: Android boots and the Start-menu folder is written, but the mirror cannot show the screen |

## What differs from Linux

- **Rendering and video are software.** The guest has no GPU, so Android renders with
  SwiftShader and encodes the mirror's video on the CPU. Expect it to be slower than on a Linux
  PC with a GPU. [GPU Acceleration](GPU-Acceleration.md) does not apply.
- **The app-menu folder is in the Start menu.** Each profile gets a Start-menu folder of its
  apps, as it gets an application-menu folder on Linux
  ([Android Apps on the Desktop](Android-Apps-on-the-Desktop.md)).
- **Linux-desktop integration is not available**: `remora service`, auto-sleep and the KWin
  window placement.
- **Building images from source** needs a Linux build host (`source_host`). Fetching the
  ready-made image, `remora play-install` and add-on layers work, and run inside the guest.
