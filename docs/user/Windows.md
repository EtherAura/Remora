# Windows

This page explains how Remora runs on Windows: where Android actually runs, what the PC needs,
the guest Remora manages for you, and what differs from Linux.

> **Status: preview.** Every release carries a Windows installer. It has been tested in virtual
> machines, not yet on a range of PCs, so expect rough edges, and please
> [report](https://github.com/EtherAura/Remora/issues/new/choose) what you find.

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

Download `Remora-<version>-windows-x64-setup.exe` from the
[latest release](https://github.com/EtherAura/Remora/releases/latest) and run it. The installer is
not code-signed, so Windows SmartScreen stops it the first time with "Windows protected your PC":
choose **More info**, then **Run anyway**. To check the download first, compare its SHA-256 with
the `.sha256` file beside it in the release (`certutil -hashfile <file> SHA256`), or verify where
it was built with `gh attestation verify <file> --repo EtherAura/Remora`.

The installer puts Remora under Program Files, adds **Remora** to the Start menu,
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

The guest keeps running when Remora exits, as docker does. When Windows shuts down, restarts or
signs you out, a small hidden helper Remora leaves beside the guest powers it down first, Android
included. If that takes more than a few seconds, Windows shows "Stopping Android, so that nothing
it is saving is lost" while it waits. Choosing to restart anyway cuts the guest off as a power cut
would: Android loses whatever it had not yet saved. So does a shutdown started from outside your
session, such as `shutdown /r` over ssh, which Windows does not hold for anyone; run
`remora guest stop` first.

It is reachable only from this PC. Its ssh and adb ports are forwarded on `127.0.0.1` alone, with
a key Remora generated and a host key Remora pinned before the first connection.

### Settings

Set these in `[Defaults]`: there is one guest, shared by every profile.

| key | default | |
|---|---|---|
| `guest_memory_mb` | `6144` | memory the guest holds while it runs |
| `guest_cpus` | half this PC's threads, 2 to 8 | |
| `guest_accel` | `auto` (the Windows Hypervisor Platform) | `tcg` runs without it, for testing only: Android boots and the Start-menu folder is written, but the mirror cannot show the screen |

### Cold boot in a nested Windows VM

On WHPX, Remora gives Android up to 20 minutes to report boot completion after container
startup. The wait ends as soon as Android is ready or the container exits.

For the Amd-host-a WINQ smoke test, expose nested AMD-V to Windows and enable WHPX. Give the
Windows VM eight vCPUs and 12 GiB of RAM. Set `guest_cpus=4` in Remora's `[Defaults]` and use its
6 GiB guest memory setting. The disposable Docker runner allows up to 10 CPUs for the VM and
QEMU's other threads, with a 28 GiB memory limit for the VM, RAM-backed disk and image staging.

Launch the smoke test from the Windows desktop session. For a scheduled test, use
`New-ScheduledTaskSettingsSet -Priority 4 -ExecutionTimeLimit (New-TimeSpan -Minutes 28)`.
Priority 4 starts Remora and QEMU at Normal priority. The time limit covers guest startup,
Android's first boot, mirror setup, verification and shutdown.
[Windows Task Scheduler priority settings](https://learn.microsoft.com/en-us/windows/win32/taskschd/tasksettings-priority)

Time image transfer and import separately from Android startup. With fresh Android data, ART
does first-boot verification and compilation before Android becomes ready. Confirm boot with
two `getprop sys.boot_completed` reads through the packaged ADB endpoint, 15 seconds apart.
[Android ART Service configuration](https://source.android.com/docs/core/runtime/configure/art-service)

### Warm restart with RX 580 passthrough on Amd-host-a

The host's generic PCI bus reset let Windows restart, but the Radeon driver reported
Code 43 on the new desktop. Amd-host-a's passthrough profile loads a signed `vendor-reset`
module and selects `device_specific` for the RX 580 before VFIO opens it. The host journal
records the Polaris 10 reset at the Windows guest's reset event. [Upstream supported devices](https://github.com/gnif/vendor-reset/tree/cbb4dd50926337e0f9fe1dd1e8a74aa5cb3448a7#supported-devices)

The nested Windows profile uses `HV=N` with `+svm` to keep WHPX available. The November 20
smoke test used the published Remora 2026.11.6 preview. Windows kept a healthy RX 580
through an in-session restart, and Android 17 booted again under WHPX and read back
data written before the restart. After Windows shut down, both devices reattached to their
Linux drivers. The kernel rejected the old `bus` setting, so recovery restarted the login
screen and refreshed the kernel's available reset methods after unloading the module.
The installed profile uses that same reset-method cleanup. [Linux PCI reset-method documentation](https://www.kernel.org/doc/html/latest/admin-guide/abi-testing.html)

A repeat using the installed launchers kept the RX 580 and HDMI audio healthy across
the Windows restart. After shutdown, the profile returned both devices to Linux and
unloaded the module automatically. The test started the packaged shutdown watcher in
the Windows session. After restart, Remora reported `the guest stopped while booting`
and the test ended before checking Android data recovery.

A diagnostic repeat with the same 10 GiB Windows configuration completed Android boot
and recovered the data marker after restart. The earlier error only showed that the
QEMU monitor missed a startup probe, which could also happen with QEMU still running.

Amd-host-a's reset module is installed under `/usr/local/lib/remora/rx580-reset/`, keyed by
kernel release. When changing the kernel, build and sign a matching module before using
the RX 580 passthrough profile.

During guest startup, Remora waits for SSH and Docker while the QEMU process is alive,
even if its monitor is temporarily unavailable. If startup fails, the error includes
the available QEMU output and Linux console log.

A WINQ preview installer rebuilt from the published preview source with this startup
fix completed the Amd-host-a warm-restart smoke test. The installed launcher used the
host CPU with XSAVEC masked before and after the restart. Android 17 booted under WHPX
and read back the marker saved before Windows restarted.

### Installer smoke test on Amd-host-b

The same WINQ preview installer completed a fresh installation in a disposable
12 GiB Windows VM on Amd-host-b. The installed shutdown watcher started automatically
and shut down the Linux guest during a normal Windows restart. Android 17 booted
under WHPX before and after the restart and read back its saved data marker.

This test ran without PCI passthrough. The RX 550 and HDMI audio stayed attached
to Linux throughout the run. An earlier attempt lost host connectivity during GPU
handoff, before the installer ran. That handoff is tracked in `remora-1978.6.1`.

### Reserving Amd-host-b's RX 550 for passthrough at boot

The November 20 outage began during the second native-to-VFIO transition, eight
seconds after the prior VM's handback. The last kernel message was the entry to
amdgpu hardware teardown. VFIO had yet to finish binding the GPU and no new VM
was running. The logs establish the trigger and leave the subsequent host or
network lockup unresolved.

The handoff supervisor now holds its shared lock through seven minutes of
healthy peer observations after returning the RX 550 to Linux. If the health
check fails or the supervisor is interrupted, the next requested handoff is
inhibited pending investigation.

`tools/amd-host-b-vfio/build-reserved-boot.sh` builds a separate, Secure
Boot-signed systemd-boot entry for this host. The installed entry,
**Gentoo Linux (Amd-host-b RX 550 reserved for VFIO)**, reserves `1002:699f` and
`1002:aae0` in the initramfs and loads `radeon` early for the HD 8490.
Connect the host monitor to the HD 8490's DisplayPort or DVI before selecting
it. Its early driver must bring up the encrypted disk unlock screen.
The regular Gentoo entry remains the default and also provides the rollback
path. The alternate entry must be rebuilt against the current kernel when
updating it.

Use the reserved profile for the physical test so Windows can start and stop
while both RX 550 functions stay with `vfio-pci`. Before opening the VFIO group,
run `tools/amd-host-b-vfio/check-reserved-boot.sh` and configure the RX 550's signed
`vendor-reset` module for `device_specific` reset. A successful alternate boot tests
early binding. A Windows restart and shutdown still need a physical check.

For a test with the monitor still on the RX 550's HDMI, use the separate
**Amd-host-b RX 550 test after disk unlock** entry. It was built with
`--after-unlock` and installed as a non-default option.
It keeps the firmware framebuffer for the existing LUKS passphrase prompt
and starts Linux in `multi-user.target`. After SSH is available, run
`check-reserved-boot.sh --bind-after-unlock` as root. The command checks that
both RX 550 functions are unbound and then loads VFIO, which takes over the
card and clears the Linux console on HDMI. Linux can be managed over SSH
through the test. Select the regular Gentoo entry to return to the desktop.
The signed image displayed the prompt and unlocked a disposable LUKS disk in
an isolated UEFI VM. The physical test waits for host maintenance to finish.

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
