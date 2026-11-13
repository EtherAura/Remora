# Troubleshooting

This page starts with the three commands that tell you what is wrong (`remora check`,
`remora status` and `remora doctor`), then lists the failure modes the project documents, each
with its cause and fix.

## Ask Remora first

| command | what it tells you |
|---|---|
| `remora check [<profile>]` | what the target is missing before a deploy, with a remedy per row. Add `--gpu-host` for the GPU rows |
| `remora plan [<profile>]` | what the profile resolves to, including notes about anything the deploy would refuse |
| `remora status [<profile>]` | the live state as JSON, followed by plain-text warnings for known silent failures |
| `remora doctor [<profile>]` | one file with everything needed for a bug report |

`check` marks each row ok or MISSING. Rows marked `[ info  ]` or `[ STALE ]` do not change the
verdict. A line beginning `DEPLOY WILL REFUSE` in `check` or `plan` is the deploy's own refusal
text, shown in advance.

### The diagnostics bundle

```sh
remora doctor <profile>            # writes a timestamped file in the current directory
remora doctor <profile> --stdout   # prints it instead
```

The bundle collects the resolved configuration, the host probes and readiness, versions, and the
device's state and recent log. It takes about 15 seconds.

**Read the bundle before you share it.** It contains your profile's settings and the device's
logcat, which can include things you would rather not publish. Redact freely.

## A deploy is refused

| what you see | cause | fix |
|---|---|---|
| `no such instance`, with a list of known profiles | the profile name does not exist | use one of the listed names |
| a profile with `backend=vm` is refused at preflight, or `--backend vm` is refused on the command line | that backend was removed | set `ssh_host=user@host` for the docker host you want and use `backend=remote`. Then re-check `gpu_mode`, `network_mode` and `host_adb_port`: on `remote` they default to `guest`, `bridge` and 5555 |
| `gpu_mode=<value> is not a GPU mode` | only `host` and `guest` are valid | set one of them, or remove the key for the default (`guest`) |
| another container "is already using" something | a running instance holds a resource this profile needs, for example the host adb port | stop that instance, or give this profile its own, then retry |
| `DEPLOY WILL REFUSE` about `/data` | the data directory on disk does not have the layout the profile resolves to (layered, or a plain bind) | follow the refusal text. `use_overlayfs` selects the layout |
| `DEPLOY WILL REFUSE` about CPU tuning | the image was built for an `arch_variant` whose instructions this CPU lacks | deploy a portable image, build with a variant this host satisfies, or run the image on a host that has the missing flags |
| the image has no `gralloc.minigbm_gbm_mesa.so` | Venus is on but the image was built without its gralloc | rebuild with the `minigbm_gbm_mesa_vtest` source patch, or set `venus=false` |

## The mirror does not start or looks wrong

| what you see | cause | fix |
|---|---|---|
| `no in-image agent`, with advice to rebuild | the image does not bake the mirror agent | rebuild the image with the `mirror_agent` feature |
| client and agent refuse each other over the protocol version | the image and Remora are from different releases | an older agent means rebuilding the image, a newer one means updating Remora. Images built before release 2026.10.0 need a rebuild |
| `could not open an adb tunnel to the device` | a host-side problem with the adb tunnel, which uses a local port between 27183 and 27199. It is not a missing agent | look at the host, and do not rebuild the image for this |
| `the agent declined the session:` and a reason | the agent answered and refused | act on the reason it gives |
| the mirror streams H.264 although you expected H.265 | `gpu_mode=guest`, or a host GPU with no VA-API encode | `remora plan` names the reason. On NVIDIA, consider `host_encode` |
| `reconnect` reports that it is not connected | the instance is not running | use `remora up` |
| adb and the mirror say the device is dead, but Android is fine | the in-container adb forwarder is down. `remora status` warns about it | run the command the warning prints: `docker exec <container> sh /remora/netfix.sh` |
| judder with `host_encode`, and `remora status` warns that the mirror is losing frames | frames composed on the device are not reaching the host encoder | kill that profile's `remora-frame-encoder` process and reconnect. A plain reconnect does not fix it |

On an NVIDIA host, the host frame encoder can crash inside FFmpeg's Vulkan code. The project
carries an optional host FFmpeg patch for that:
[host-prereqs](../../vendor/host-prereqs/README.md) describes it. Nothing depends on it.

## GPU and video

| what you see | cause | fix |
|---|---|---|
| `remora status` warns that a `gpu_mode=host` device is rendering in software | the GPU was not picked up | run `remora check --gpu-host` and fix the rows it names |
| SurfaceFlinger crash-loops on an NVIDIA host, looking like a broken image | the NVIDIA driver was updated without a reboot, so the loaded module and the userspace libraries differ | `check` shows the row *NVIDIA kernel module matches userspace* failing. Reboot before deploying |
| a black screen under Venus | a guest library is missing | run `remora venus-build` again |
| rendering works under Venus but the mirror never receives a frame | the Venus driver in use is the image's own, not the prebuilt one | reinstall with `remora venus-build`. If you set `venus_lib_dir` yourself, make sure it holds the prebuilt libraries |
| video plays in software | the GPU has no VA-API driver that Remora maps | set `va_driver=` if a driver exists |

Background is in [GPU Acceleration](GPU-Acceleration.md).

## Networking

| what you see | cause | fix |
|---|---|---|
| `check` reports the host LAN as unreadable | no default IPv4 route with an addressed interface on the docker host | set `macvlan_parent`, `macvlan_subnet` and `macvlan_gateway`, or use `network_mode=bridge` |
| a `bare` host cannot reach its own macvlan container | the kernel isolates a macvlan interface from its parent | leave `macvlan_host_route` on, so Remora maintains the host shim |
| the deploy stops and asks for `macvlan_shim_ip` | something already answers on the shim's derived address | set `macvlan_shim_ip` to a free address |
| Remora reports a `remora-macvlan-shim.service` on the host | that unit and the automatic path both try to own the shim interface | remove the unit, or set `macvlan_host_route=false` |
| the container gets an address inside your router's DHCP pool | the default pool is the top /28 of the subnet | set `macvlan_range` to a slice outside the DHCP pool |
| apps on the host cannot discover the macvlan container | the shim carries unicast only | add the device by its address where the app allows it |

## The image build

| what you see | cause | fix |
|---|---|---|
| the build dies with `newosproc` | `vm.max_map_count` is too low. This is not an out-of-memory kill | `sudo sysctl -w vm.max_map_count=1048576` |
| the build container exits with 137 and `dmesg` shows nothing | systemd-oomd killed it, and logs to the journal | `journalctl -u systemd-oomd`. Raise `SwapUsedLimit` in `/etc/systemd/oomd.conf`, or set `ManagedOOMPreference=avoid` on the slice the build runs in |
| `OOM-KILLED INSIDE THE CONTAINER` | the build container's own memory cap, not systemd-oomd | raise `build_mem_gib`. `journalctl -k` shows "Memory cgroup out of memory". The build log stamps UTC and the journal local time |
| `THE SOURCE TREE WENT READ-ONLY` | the filesystem latched read-only mid-build. It is not a build error and retrying does not help | on btrfs, check `btrfs device stats`, then unmount fully and mount again. `mount -o remount,rw` does not clear it |
| `source build INTERRUPTED` | the wrapper died before the build reported a status, so there is no verdict | the builder container may still be compiling. Check `docker ps` and the log before deploying anything |
| the build refuses to start over `mesa_source` | the feature is required for every source build | enable it and build the Mesa payload first |
| a feature refuses at staging | its payload directory is absent | run the fetch command the message names, or supply the payload |
| `check` shows `[MISSING]` or `[ STALE ]` patch rows | the installed patch tree and the binary are from different versions | reinstall with `cmake --install`, or rebuild Remora, as the row says |
| `FEATURE WILL DO NOTHING` | a feature's required source patch is not enabled | tick the patch, or add it to `source_patches=` |
| `NOT BUILT IN` | a default feature is missing from a pinned `features=` list | add it, or accept the image without it |
| an Android 17 build fails at `repo sync` on an old release | fixed in release 2026.10.0 | update Remora |

The log of the last source build is `/tmp/remora-build.log`. More in
[Building the Image](Building-the-Image.md).

## Apps and input

| what you see | cause | fix |
|---|---|---|
| the Play Store refuses to sign in | the device may not be certified, or have another account/service issue | `remora certify` reports state. Registration does not grant certification or a Google licence; see [Google sign-in](Android-Apps-on-the-Desktop.md#google-sign-in-and-play-certification) |
| an APK install hangs for minutes | Play Protect's verifier for adb installs | install with `remora install`, which switches it off first |
| the microphone records silence | the image has no USB audio HAL | build with the `usb_audio` feature and its patch |
| Android sees no camera | no provider in the image, or no camera node in the container | build with `camera_v4l2`. A camera plugged in later needs the container recreated |
| Gboard's on-screen keyboard stays pinned open | Gboard remembers a tap on its keyboard icon | `remora gboard-reset`. If that reports no record, `remora gboard-reset --wipe --yes` clears all Gboard data |
| shared input devices are not shared | `python3` is missing on the docker host | install it. `check` lists this row when `shared_inputs` is set |

## Reporting a problem

Use the bug report form in the repository and attach the `remora doctor` bundle, after reading it.
Say what you run on: GPU vendor and driver, kernel and distribution.
[CONTRIBUTING.md](../../CONTRIBUTING.md) has the details. Report a suspected vulnerability
privately, as [SECURITY.md](../../SECURITY.md) describes, not as a regular issue.
