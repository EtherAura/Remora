# Changelog

All notable changes to Remora are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Remora uses
calendar versions, `YYYY.MM.PATCH`: the year and month of the release, then a patch number.

## [Unreleased]

### Changed

- **The Windows installer no longer carries a second copy of FFmpeg.** Qt's FFmpeg media backend
  brought FFmpeg 7 along beside the FFmpeg 8 Remora decodes with, though Remora uses Qt
  Multimedia only to play sound, which Qt's native Windows backend does as well. That backend
  alone now ships: the download is 7 MB smaller, and the installed program 21 MB.

## [2026.11.5] - 2026-11-18

### Added

- **Remora for Windows, as a preview: every release now carries a Windows installer**,
  `Remora-<version>-windows-x64-setup.exe`, for 64-bit Windows 10 and 11. Android runs in a small
  Linux guest Remora manages itself, on the Windows Hypervisor Platform, which the installer turns
  on. No WSL and no virtual machine software of your own. Rendering is software (SwiftShader).
  The installer is not code-signed, so Windows SmartScreen asks before the first run; its build
  provenance is attested like the Linux packages
  (`gh attestation verify <file> --repo EtherAura/Remora`). It has been tested in virtual machines,
  not yet on a range of PCs. See the [Windows](https://github.com/EtherAura/Remora/wiki/Windows)
  page.

- **A Start-menu folder per profile on Windows.** Windows now gets in the Start menu the folder
  Linux gets in the application menu: one entry per Android app under the app's own icon, plus
  the mirror, desktop and PIP entries. It is refreshed after each connect and by `remora apps`.
  `desktop_menu` is now on by default on Windows too. An app's window shows the app's name and
  icon on the taskbar, and the Apps page lists the apps on Windows as well. Remora changes only
  the shortcuts it wrote, so anything else in a Start-menu folder that shares a profile's name is
  left alone.

- **`remora apps --remove-all`** removes every profile's menu folder, including those of profiles
  that no longer exist. The Windows uninstaller runs it.

### Changed

- **`guest_accel=tcg` is described as what it is: for testing only.** Under emulation Android
  boots and the Start-menu folder is written, but the device cannot encode its screen fast enough
  for the mirror to show it. The message shown when the Windows Hypervisor Platform is missing,
  and the Windows page, say so instead of calling it slow.

- **The Windows installer stops Remora's guest and adb's server before it installs or
  uninstalls.** A running guest held QEMU's files open, and the adb server Remora starts, which
  keeps running after Remora exits, held adb's. An update could not replace them and an uninstall
  left them behind.

### Fixed

- **Remora's Windows guest shuts down properly, Android included.** Its docker is started by
  init, so nothing in the guest's shutdown stopped Android or docker. Android was killed mid-write,
  the data disk stayed in use, and the power-down hung until Remora told QEMU to quit after 60 s
  (240 s under emulation): every `remora guest stop`, and every installer update, cut the power.
  The guest now asks Android to power off (about a second), then stops docker and unmounts its
  disks: a stop with Android running takes 6 s instead of a minute. Install the new version for
  its guest image.

- **Restarting or shutting down Windows no longer cuts the power to Remora's guest.** QEMU has no
  window, so Windows ended it within seconds of a restart: the last seconds of what Android wrote
  were lost, and both guest disks needed journal recovery at the next boot. Remora now keeps a
  small hidden helper beside the guest that powers it down first. If that takes more than a few
  seconds, Windows says "Stopping Android, so that nothing it is saving is lost" while it waits,
  with the usual choice to restart anyway. Windows holds only a shutdown started in your own
  session; one started from elsewhere (`shutdown /r` over ssh) still cuts the guest off.

- **A mirror that fails while the boot animation is showing exits.** It used to hang on its way
  out, so it stayed running after the error, and a request to stop it was ignored as well.

- **The boot animation stops for good once the mirror shows the device.** On a first connect the
  animation can arrive after live video has started, and it then started playing again behind
  the mirror with nothing to show, ticking 60 times a second for as long as the mirror ran.

- **An empty CPU flags line no longer refuses a tuned image.** When the probe read a `flags`
  line with nothing after its colon, preflight reported every flag the image's arch variant needs
  as missing, telling you the CPU lacked `avx2` when nothing about it had been read. Such a line
  now counts as not knowing, which never refuses; a CPU that really lacks the flags is still
  refused.

- **On Windows a mirror's log holds its own warnings.** Without a console, Qt sends its messages
  to the debugger rather than to the log a detached mirror writes, so the reasons a mirror gave
  for falling back or failing, such as which hardware decoder it used, never reached its log.
  Remora now sends them to the log on Windows.

- **D3D11VA decoding tries every graphics adapter, not only Windows' default.** The adapter that
  can decode video is not always the default one: a display-only or remote-desktop adapter can be
  the default with a working GPU beside it, and the mirror then decoded in software. It now tries
  the default adapter and then the others in turn.

- **A first menu waits for a device that has only just booted.** Right after a slow boot the
  device can list no apps for a few seconds, and a menu that did not exist yet was then reported
  as failed instead of written. It is now retried for a while first, as a refresh of an existing
  menu already was, on Linux and Windows alike.

- **The menu folder is written for a profile that has no section of its own.** `remora up`
  with no profile runs the active one even when `remorarc` has no section for it, as on a fresh
  install, but the menu refresh it starts afterwards asked for that profile by name and was
  refused, so the folder never appeared. The refresh now picks the active profile the same way.

- **A profile pinned to a local image built on one of Remora's keeps that image's hardware
  encoder.** Remora decides what an image can do, such as encode HEVC in hardware, carry the
  mirror agent or carry the updater, from the list of images it builds. A local image built on
  one of them was on no list: a layer from `remora build`, an add-on layer, or one made by hand.
  Pinned with `image_tag`, it counted as carrying nothing. The Viewer page greyed out h265 and
  switched the page to h264, and Connect offered h264 instead. Remora now asks the docker host
  which of its images such an image was built on, from the labels its own layers carry or else
  from the image's layers, and answers as that image does. `remora plan` names it. Labels that
  name the image itself are ignored, under any of its tags: a local image tagged like one of
  Remora's no longer takes that image's capabilities from its name alone, and is judged by its
  layers instead.

- **An Android shutdown can no longer hit the docker host.** When a shutdown goes wrong, Android's
  init writes to the kernel's sysrq trigger: it tells every process to stop or kills it, remounts
  every filesystem read-only and finally crashes the kernel. That trigger is the host's, not the
  container's, so on a bare Linux host this would end the whole desktop session. It was seen
  happening in Remora's own Windows guest. Every container now masks the trigger at boot, through
  the init file each deploy mounts, so the next deploy protects existing images without a
  rebuild.

- **A container that stops while Android boots is reported at once.** The boot wait kept polling
  a container that had already exited until its time ran out, which is half an hour on Remora's
  emulated Windows guest. It now stops as soon as the container is gone and says why: when the
  exit code shows that Android asked to reboot or power off, it says that.

- **Remora for Windows deploys whatever folder it is started from.** It looked for adb on `PATH`
  and in the working directory only, so it refused to deploy ("adb not found") unless it had been
  started from its own folder, though the package carries adb beside it. Remora's own folder now
  comes first on `PATH` for everything it starts, so its adb is always the one used, rather than
  another adb that happens to be on `PATH`.

- **Remora for Windows has its icon.** `remora.exe`, `remora-gui.exe`, the installer, its
  Start-menu entry and every window carry Remora's icon. Before this, Explorer and the taskbar
  showed a blank program icon. The windows used the white icon made for a dark Linux theme, which
  is invisible on Windows' light default. The Windows icon is the dark silhouette with a light
  outline, so it shows on a light Start menu and on a dark taskbar. `tools/windows-icon.sh`
  makes it from the SVG.

- **The Apps and Settings pages have their sidebar icons on Windows.** Qt draws Windows icons from
  the system icon font, which has no icon under any name those two pages asked for. They are now
  drawn from the same font.

- **A hand-written comma-separated `shortcut_mod` is kept.** A remorarc line such as
  `shortcut_mod=lctrl,lalt`, written without quotes, used to be read as empty, so the mirror fell
  back to its default shortcut modifiers. It now reaches the mirror as written, the way `key_bind`
  already did.

- **The Gentoo ebuild asks for glibc's static library at build time.** Two helpers that run
  inside the container are linked statically, and Gentoo's glibc installs `libc.a` only with
  `USE=static-libs`, so with that flag off the build stopped at configure. The ebuild now
  depends on `sys-libs/glibc[static-libs]` for the build only.

## [2026.11.4] - 2026-11-17

### Added

- **Hardware video decode in the Windows client.** The mirror decodes with D3D11VA, which every
  Windows GPU driver provides, so AMD and Intel machines no longer fall back to software.
  `--hwdec=auto` tries NVIDIA's CUDA first and D3D11VA second, and `--hwdec=d3d11va` asks for it
  directly. When the GPU cannot decode the stream, the mirror says so once and decodes in software.
  It has not yet been tested on a Windows machine with a GPU.

- **The Windows client works against a Linux docker host too.** On Windows, the image library,
  prune, push, fetch, the official catalog, `images`, `play-install` and the add-on layer use the
  profile's docker host when no other host is given. Source builds need an explicit `source_host`
  and say so when it is missing.

- **Explicit support for private images managed by Magisk's built-in Zygisk.** Reconnect
  verifies their root and native companion instead of replacing their modules with ReZygisk.
  An opt-in source patch documents the Android 17 container build validated with Shamiko 0.7.5.
  Shamiko 1.2.5 remains unsupported on this container.

- **Root on the official image without a source build.** `magisk_root` can now be layered onto an
  existing image, the official `remora24:x86_64-public` included, the way `gapps` and
  `widevine_l3` already could: `remora recipe --base <image>` prints the layer and `remora build
  --base <image>` builds it from the Magisk v30.7 payload that `vendor/fetch-payloads.sh magisk`
  downloads. Before this, Remora said root needed a full source rebuild. Every layer Remora builds
  is now labelled as not a public release, so a layered image can no longer carry the official
  image's public label.

- **ARM apps on the official image without a source build.** `arm_translate` can be layered onto an
  existing image too. `vendor/source-build/stage-arm-layer.sh` stages the translator payload, which
  you supply, patches it exactly as a source build does, and adds the properties that turn the
  bridge on and advertise `arm64-v8a`. `remora build --base remora24:x86_64-public` then builds the
  layer, alone or with root. Apps that ship only arm64 code, which the official image refuses to
  install, can then install and run translated.

- **Add-ons as a profile option.** The Image page's **Active image** card has an **Add-ons** row:
  **Root (Magisk)**, **ARM app translation** and **Widevine L3**, saved as `addons=` in the
  profile. Each Connect builds or reuses a private `remora-local:addons-` layer over the image the
  profile deploys, or over its Google Play layer, on the profile's Docker host, so the add-ons
  follow an image update without a manual build. Magisk is downloaded the first time, hash-checked;
  the ARM translation and Widevine payloads are yours to supply, in
  `~/.local/share/remora/payloads/`. `remora plan` names the add-ons, the in-Android updater
  compares the image under the layer, and the Built images card nests each layer under the image
  it was built over. `vendor/fetch-payloads.sh` takes `REMORA_PAYLOAD_DIR` to stage a payload
  outside the vendor tree.

- **Play spoof and Shamiko as add-ons.** The Add-ons row also offers **Play spoof**, the stack that
  presents Pixel device properties inside Play services, and **Shamiko**, which hides
  Magisk's changes from apps on the DenyList. Both are layered over the image like the other
  add-ons, so the official image gets them without a source build, and ticking Play spoof turns on
  the `play_spoof` setting too. Play spoof needs Root and Shamiko needs both; the page offers each
  only once what it needs is ticked. With ARM translation, the Play spoof layer also carries the
  module that keeps TikTok and Amazon running under translation. The binaries are yours to supply
  in `~/.local/share/remora/payloads/zygisk_pif`. `vendor/source-build/stage-spoof-layer.sh`
  stages the same layer for `remora build --base`.

- **Remora for Windows, without a second machine (early).** Windows has no Linux kernel to run
  Android on, so Remora for Windows runs its own small Linux guest: Alpine with a kernel Remora
  builds, under the QEMU it ships, on the Windows Hypervisor Platform. A profile with no `ssh_host`
  deploys into it over the same path a remote docker host takes, and `remora guest
  status|start|stop|reset-system` shows and controls it. Its data disk keeps every image and Android
  `/data` across updates. Settings: `guest_memory_mb`, `guest_cpus`, `guest_accel`. The client
  cross-compiles with llvm-mingw (`tools/windows-sdk.sh`, `cmake/windows-llvm-mingw.cmake`) and
  `tools/windows-stage.sh` stages the folder a PC runs: `remora-gui.exe` opens the workspace with no
  console window, and `remora.exe` is the command line. `tools/windows-installer.sh` builds
  `remora-setup.exe`, which also turns on the Windows Hypervisor Platform. Nothing is published yet,
  and the hypervisor path has not been run on a real Windows PC. [Windows](docs/user/Windows.md) has
  the details.

### Changed

- **NVIDIA Venus, `host_encode` and `host_decode` are `bare` only.** Their helpers run on the
  machine running Remora, and a `remote` deploy used to start them there, beside a container that
  was somewhere else. On `remote`, automatic Venus now stays off. A remote profile with
  `gpu_mode=host` that sets `venus=true`, `host_encode=true` or `host_decode=true` is refused before
  anything is created: `up` names the setting, `check` reports it as a required row, and `plan`
  prints a `DEPLOY WILL REFUSE` line.

- **A remote profile's data directory stays a path on the docker host.** A `~` typed into the
  data-directory field is no longer expanded with this machine's home unless the profile is `bare`.
  The folder picker is greyed out for remote profiles.

- **The Reddit sign-in no longer needs `sqlite3` on the host.** Android's account databases are
  edited with Qt's own SQLite driver, and the session cookie appears in no command line. Debian
  and Ubuntu builds need `libqt6sql6-sqlite`.

### Fixed

- **Remora finds its mirror windows by a registry, not `pgrep`.** Each mirror window registers
  itself, and Remora finds, replaces, closes and counts mirrors from that list. On Windows a
  reconnect now replaces the previous mirror instead of opening a second one beside it, **Stop**
  closes the windows, the PIP toggle closes as well as opens, and `auto_sleep_minutes` no longer
  freezes a device that is being watched.

- **The PIP toggle closes only its own profile's PIP.** Toggling a profile whose name is the start
  of another's ("A" and "A 2") used to close the other profile's PIP. Window lookups now match
  profile names and device addresses exactly.

- **A stale mirror that ignores the request to close is killed** before a deploy's boot splash
  opens, before a reconnect and before `remora sleep`, instead of staying open beside the new
  window.

- **`remora check` and the workspace readiness no longer report adb missing on Windows.** A bare
  profile there resolves to Remora's own guest, and the local docker rows are not asked of a
  machine that has no local docker.

- **Screenshots, the boot animation, Alt+D and the lit-screen wait need no local shell.** The mirror
  unpacks `bootanimation.zip` itself, so `unzip` is no longer needed for it, and a deploy on Windows
  no longer spends 60 s on the lit-screen wait.

- **`remora shell`, `logcat`, `prop` and `install` on Windows wait for adb and return its exit
  code.** Arguments with spaces reach adb intact, and the prompt no longer comes back while adb is
  still running.

- **Profiles can be deleted on Windows, and on Linux without systemd.** With no `systemctl` there
  are no services to disable, so Delete profile removes the profile instead of keeping it.

- **Linux desktop features say they are unavailable on Windows instead of failing.** The
  application-menu refresh is skipped with a reason and `desktop_menu` defaults to off there;
  `remora service` refuses and writes nothing; the input-device pickers and *Open device storage*
  explain what to do instead. The window icon shows on Windows.

- **The image library no longer treats a command that never started as a success.** A missing
  `ssh` used to read as success, and on the GUI thread it hung for the whole timeout first.

- **A push or fetch whose sending side fails is reported as failed.** Before, the result was taken
  from `docker load` alone.

- **An explicit VA-API render node works.** `--hwdec=/dev/dri/renderD128` looked for
  `/dev/dri/renderd128` instead and always fell back to software decode.

- **Remote deploys set up the container they deployed.** Several deploy steps ran `docker exec`
  on this machine instead of on the remote docker host: the boot-id ashmem link for Android 17
  WebViews, handing camera and USB audio nodes to their Android services, and the whole Play spoof
  stack. A remote container got none of them. A deploy with `play_spoof` on reported that the
  image was built without it, and a local container with the same name could be changed instead.

- **App windows check their own video path.** Sonar and other separate displays no longer wait
  for a packet from the main mirror's host encoder or report a false `serve unconfirmed` warning.
  An unrelated host-encoder failure also cannot reject the app window or restart Android during
  its launch.

- **Magisk startup builds the environment upstream expects.** BusyBox now has its applet
  directory, the daemon receives the certificate that identifies its official manager, and
  module mounting gets its scratch filesystem. This fixes boot errors and the official
  manager being rejected for a signature mismatch.

- **Rooted containers can install Magisk modules normally.** Startup now supplies the missing
  installer helpers in Magisk's data directory. Existing manager-installed helpers are preserved.

- **ARM downloads recover from an empty tablet-profile response.** When Play supplies no files for
  an automatically chosen density profile, the installer tries its ARM phone profile before
  changing the installed app. This fixes Sonar's empty download. Explicit profile choices are kept.

- **Sonar's ARM build opens from Remora's app launcher.** Its Flutter GPU renderer could crash or
  leave a black window on the host GPU. Remora restarts Sonar with its software-rendering launch flags;
  other apps keep their renderer.

- **App windows use a working encoder on NVIDIA.** A main mirror encoded on the host GPU could
  leave app windows trying the device's unusable HEVC encoder. Those windows now use software AVC
  when device VAAPI encoding is unavailable or a Venus profile uses host encoding, including
  hybrid Intel/NVIDIA systems. The main mirror keeps its host encoder.

- **CPU-rendered app surfaces display correctly with Venus.** CPU-write buffers now require
  linear memory, so a scanout preference cannot give a software renderer tiled memory and corrupt
  its pixels. GPU-only scanout buffers retain their preferred layout.

- **Denylisted apps lose access to Magisk's mounted files.** The container now uses the mount name
  ReZygisk's cleanup expects. The old generic mount name left Magisk and `su` visible in app
  namespaces. Shamiko's unsupported status and the failed basic-integrity verdict remain separate
  limitations.

- **The mirror stays up with the Play spoof stack on.** On a device's first boot with the stack,
  the mirror failed to connect with `DeadSystemException`: the framework restart that brings the
  stack up left the in-image agent talking to the old system server. The deploy now restarts the
  agent after it. Restarting it exposed a second fault: ReZygisk took the agent for a zygote,
  killed it four times, then stopped injecting for good, so a later framework restart came back
  without the stack. Remora's ReZygisk build now injects only processes started as a zygote.

- **The Magisk app survives the first boot with the Play spoof stack.** The same framework restart
  could catch Magisk installing its app at boot completion, and Android then deleted the
  half-installed package. Root kept working from a shell, but no app could ask for `su`. The deploy
  now puts the Magisk app back when it is missing after that restart.

- **LAN detection skips link-local addresses.** When the interface of the default route listed
  a link-local address (such as `169.254.x.x`) before its real one, Remora took the link-local
  address as the LAN, so the macvlan network handed to docker got the wrong subnet and host
  address. Only a global-scope address is used now, and an interface with no global address
  gives no LAN rather than a wrong one.

## [2026.11.3] - 2026-11-16

### Fixed

- **A fetched image is one row in the image library.** An official image showed up twice once
  it was fetched: as the local Docker image, and again as the source's listing marked
  `local ✓`. The Google Play layer over that image was a third row. The source is now named on
  the image's own row, and Fetch on that row still loads the source's current copy. The Play
  layer is collapsed under the image it was built over, and the Active image list no longer
  offers it, since the Google Play option is how a profile runs it. A tag that is offered but not
  yet fetched still gets its own `needs fetch ↓` row. A source that offers one of an image's
  other names now marks it `local ✓` too, where it used to say it needed a fetch.

- **Remora builds without Python again.** Since 2026.11.1 configuring the build required a
  Python 3 interpreter, although only the test suites run it, so a machine or a clean chroot
  without Python could not build Remora. CMake now looks for Python instead of requiring it.
  Without it, Remora still configures and builds: the three Python suites are left out and
  configure says so, and the prereqs suite skips its two image-helper cases. The `PKGBUILD` lists
  `python` and `openssl` in `checkdepends`, and the ebuild lists them as `USE=test` build
  dependencies through `python-any-r1`. Their test runs and CI's configure with
  `-DCMAKE_REQUIRE_FIND_PACKAGE_Python3=ON`, so a missing interpreter fails there instead of
  quietly dropping suites.
- **The packages say what Python is for.** The `PKGBUILD`'s optional `python`, the ebuild's
  optfeature and the Requirements page named only input sharing. Fetching official images,
  checking for their updates and adding Google Play run it too, on the machine that runs the
  containers, and the image fetch and update check also run `openssl`. The Requirements page no
  longer says that Remora ships no image.

## [2026.11.2] - 2026-11-16

### Added

- **New public releases reach the device through its own updater.** For a profile that deploys
  Remora's official image, `remora update-watch` checks the signed release catalog every six
  hours and shows a newer release in Android's *System updates*, with its size. *Download and
  apply* downloads and verifies it, keeps the previous image for rollback, builds the Google
  Play layer again when the profile has one, and restarts the device on it with its data kept. A
  failed download or restart is reported on that screen. `remora images check` asks the same
  question from a terminal.

- **The release fetch helper can report its progress.** `vendor/release/catalog.py fetch
  --progress` prints a `REMORA_FETCH_PROGRESS=` line as each part of the archive arrives
  (`download <done> <total>`, in bytes of the signed size), or `cached <total>` when a cached copy
  is reused, then `check` and `import` as those steps begin. Without the flag its output is
  unchanged, and nothing passes the flag yet: *System updates* still shows no download progress.

- **Google Play as a profile option.** The Image page's **Active image** card has an **Add
  Google Play services** toggle, saved in the profile as `google_play` and the accepted
  `play_terms` version. Turning it on asks for the provider's terms. Each connect then builds, or
  reuses, the private local Play overlay over whatever image the profile resolves to, on the
  profile's Docker host, and runs the device from it. An image update therefore brings Play
  along, which a profile pinned to an overlay never did. `plan` names the image the overlay goes
  on. A connect refuses when the accepted terms are missing or older than the installer's, and
  does nothing for an image that already includes Google apps.

- **Releases attach a Gentoo ebuild.** Each release now carries `remora-<version>.ebuild` beside
  the `PKGBUILD`, for a local overlay. The release workflow runs `tools/test-gentoo-ebuild.sh`
  on the template for that version before attaching it. The ebuild has not yet been built by
  Portage.

### Changed

- **The host frame encoder accepts only the encoders Remora uses.** `remora-frame-encoder
  --codec` takes `hevc`, `h265`, `h264` or `avc`, or names one backend: `hevc_vulkan`,
  `hevc_nvenc`, `h264_vulkan`, `h264_nvenc`, `av1_vulkan` or `av1_nvenc`. It used to hand any
  other name to FFmpeg and open whatever encoder that FFmpeg had. Now it refuses the name before
  it opens a GPU, `--bench` included. Remora's own deploys pass `hevc` or `h264`, so they are
  unaffected.

### Removed

- **The Image library's Install Google Play… and Remove Play overlay… buttons.** They made a
  Play image a profile had to be pinned to by hand, beside the profile option that does the whole
  job. The option's images appear in the list as a **Google Play layer**, which Delete removes.
  `remora play-install` still makes a standalone Play image from the command line.

### Fixed

- **`remora service <profile> --remove` cleans up after a deleted profile.** It used to refuse
  with "no such instance" once the profile was gone, leaving no command that removed its login
  unit and its two watchers. It now works from the unit names alone, and also deletes each unit's
  `.service.d` drop-in directory. It no longer reports success when a unit could not be stopped
  or disabled: that unit's files are kept, the reason is printed, and the command exits non-zero.
  A name whose units would include another existing profile's units is refused.
- **Deleting a profile in the workspace also removes its services.** Its login unit and its two
  watchers, with their `.service.d` drop-ins, used to stay behind, so every login started
  `remora up` for a profile that no longer existed. Delete now disables and removes them first,
  the same way `remora service <profile> --remove` does, and says so in its confirmation. If any
  of them cannot be removed, the profile is kept and the reason is shown, so the delete can be
  retried. Images, containers and device data are still untouched.

- **A release update no longer leaves its download behind.** Every fetch of an official image —
  `remora images fetch`, the workspace's Official downloads and Android's *Download and apply* —
  kept the downloaded archive in `~/.cache/remora/images/` for good, several hundred MB per
  release. The archive is now removed once its image has been verified, imported and promoted;
  an import that fails keeps it, so the retry does not download it again. Fetching a release that
  is already installed now downloads and verifies it again. Older images and rollback tags are
  still kept, as before.
- **The in-Android updater works again on bare and remote devices.** Its directory was mounted
  only by the retired `vm` backend, so the *System updates* screen could neither see an update
  nor ask for one. Each container now gets its own directory, mounted where the app looks, and a
  container created without it is recreated on the next connect. Profiles on one host no longer
  share one Apply signal, which could have updated the wrong device.
- **A redeploy that fails no longer leaves the device down.** When a connect had to replace the
  profile's own running container, it removed the container before any deploy step ran, so a
  failure in preflight, the image check or the host prerequisites left nothing running. The old
  container is now replaced only when the deploy reaches the step that creates the new one.
- **Apply in Android now redeploys with the host's GPU probed.** `remora update-watch` redeployed
  without the live GPU probe that `remora up` takes, so on an NVIDIA (Venus) host the update
  failed its host prerequisites every time.
- **The image library lists each image once.** An image with several names, such as a fetched
  release under its tag and its version name, was a row per name, each showing the full size.
  It is now one row, its other names in a tooltip. Delete removes all of them, and the Active
  image list still offers each name.
- **h265 works with the public image again in the workspace.** The Image page and Connect both
  decided whether an image ships the hardware HEVC encoder from `hwc2` in its tag, which the
  public image lacks. The Image page reset the codec to h264, and Connect stopped with "No h265
  encoder in this image". Both now ask the image registry, as the mirror does.
- **The in-Android updater follows profile changes.** `remora update-watch` read the profile once
  at start, so **Apply** in Android redeployed the settings it started with, undoing any change
  made in the workspace since. It now reads the profile on every check. With Google Play on, it
  compares the image the Play overlay was built on, rather than reporting an update forever.
- **`remora reset-data` also clears the profile's application menu.** The menu refresh keeps a
  folder whose device suddenly reports under half its apps, which protects it from a list read
  mid-boot. After a reset, that guard kept a launcher for every deleted app. The next connect now
  builds the folder again from the fresh device.
- **`remora images check` and `fetch` without `--tag` now say that the tag is missing.** They
  downloaded the catalog first and then reported "Requested tag is absent from the authenticated
  catalog", as though the catalog were at fault. They now stop before any download and name the
  flag, as `rollback` already did.

## [2026.11.1] - 2026-11-14

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

[Unreleased]: https://github.com/EtherAura/Remora/compare/v2026.11.5...HEAD
[2026.11.5]: https://github.com/EtherAura/Remora/compare/v2026.11.4...v2026.11.5
[2026.11.4]: https://github.com/EtherAura/Remora/compare/v2026.11.3...v2026.11.4
[2026.11.3]: https://github.com/EtherAura/Remora/compare/v2026.11.2...v2026.11.3
[2026.11.2]: https://github.com/EtherAura/Remora/compare/v2026.11.1...v2026.11.2
[2026.11.1]: https://github.com/EtherAura/Remora/compare/v2026.11.0...v2026.11.1
[2026.11.0]: https://github.com/EtherAura/Remora/compare/v2026.10.0...v2026.11.0
[2026.10.0]: https://github.com/EtherAura/Remora/compare/edf8471e0a38447667502e70c19115ee6c014083...v2026.10.0
[2026.09]: https://github.com/EtherAura/Remora/commit/edf8471e0a38447667502e70c19115ee6c014083
