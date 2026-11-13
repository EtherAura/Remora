# CLI Reference

This page lists every `remora` command with its arguments and what it does. It follows the usage
text that `remora` prints with no arguments. Everything the workspace does is available here, so
the command line is also the way to drive Remora over ssh.

## Conventions

- **`<profile>` is optional almost everywhere.** A command that takes `[<profile>]` uses the
  active profile when you leave it out. The active profile is `active_instance` in
  [the configuration file](Configuration.md).
- **A named profile must exist.** Naming one that does not exits with status 2 and lists the
  profiles that are configured.
- **`--backend bare|remote`**, on the commands that take it, overrides the target the profile
  would resolve to.
- **`--help` or `-h` after a command prints the usage** and does not run the command. The
  passthrough commands `shell`, `logcat`, `install` and `prop` are exempt, because their arguments
  belong to the tool they hand them to.
- **An unknown command is an error**: it prints the usage and exits with status 2. `remora`,
  `remora help`, `remora --help` and `remora -h` print the usage and exit 0.

## Inspect and diagnose

| command | what it does |
|---|---|
| `remora plan [<profile>] [--backend bare\|remote]` | Dry run. Prints the resolved `docker run` command and the mirror command, with notes on a codec fallback, macvlan networking, the `/data` layout and the image's CPU tuning |
| `remora status [<profile>] [--backend …]` | Read-only live state of the target and host capabilities, as JSON, followed by plain-text warnings when something is wrong |
| `remora check [<profile>] [--backend …] [--ssh-host user@host] [--gpu-host]` | Readiness of the target: what is missing and how to fix it. `--ssh-host` overrides the profile's ssh host, `--gpu-host` adds the GPU rows. Exits 0 when ready, 1 when not |
| `remora doctor [<profile>] [--stdout]` | Diagnostics bundle: probes, versions, device properties and recent logcat in one file. Writes a timestamped file in the current directory and prints its path. `--stdout` prints the report instead |
| `remora certify [<profile>] [--recheck] [--restore]` | Report Play Protect state and the `android_id`. `--recheck` asks for a fresh checkin first. `--restore` writes back the saved checkin identity after a `/data` rebuild. This command does not grant Google licensing or device certification |

## Run the device

| command | what it does |
|---|---|
| `remora up [<profile>]` | Connect. Deploys and boots first when the instance is not running, wakes it when it is asleep, attaches when it is already up |
| `remora reconnect [<profile>] [--backend …]` | Attach to an already-running instance: adb and mirror only, no bring-up |
| `remora sleep [<profile>]` | Freeze the device with `docker pause`. `remora up` wakes it in about a second |
| `remora auto-sleep [<profile>]` | Idle watcher: freezes the device after `auto_sleep_minutes` with no mirror window. Runs until stopped |
| `remora service [<profile>] [--remove]` | Install and enable systemd user units: up at login, sleep at logout, plus the update and auto-sleep watchers. `--remove` disables and deletes them |
| `remora watch-boot [<profile>]` | Restart the device and attach the mirror during boot, to see the boot animation |
| `remora update-watch [<profile>]` | Serve the in-Android updater: wait for *Apply* on the device, then redeploy onto the new image. Runs until stopped |
| `remora reset-data [<profile>] --yes` | Factory reset: delete the profile's `/data`. `bare` only. Without `--yes` it explains what would be deleted and does nothing |

## Windows

| command | what it does |
|---|---|
| `remora app <profile> <pkg> [--title <name>]` | Launch one Android app in its own window, bringing the device up first if needed |
| `remora apps [<profile>] [--remove]` | Refresh the profile's application-menu folder (the device must be running), or remove it |
| `remora desktop [<profile>]` | Open a separate desktop-mode display, with no specific app |
| `remora pip [<profile>]` | Toggle a small always-on-top second mirror. The device must be up |
| `remora record [<profile>] [<out.mp4>]` | Relaunch the mirror with recording on. Close the mirror window to finish the file |
| `remora shot [<profile>] [<out.png>]` | Save a device screenshot. No mirror needed |
| `remora rotate [<profile>] [landscape\|portrait\|landscape-flip\|portrait-flip\|0-3\|auto]` | Rotate the device display. With no value it prints the current rotation |

## On the device

| command | what it does |
|---|---|
| `remora shell [<profile>] [cmd…]` | `adb shell` on the device. Interactive without a command |
| `remora logcat [<profile>] [args…]` | Stream the device log. Arguments pass through to `adb logcat` |
| `remora prop [<profile>] [get <key> \| set <key> <value>]` | Read or write Android properties. With no verb it prints them all |
| `remora install [<profile>] [-r\|-g\|…] <apk>` | Push and install an APK. Flags pass through to `adb install` |
| `remora push-app [<profile>] <dir>` | Copy a companion-app directory (one containing `<Name>.apk`) to the docker host, and print the `companion_apps=` line to add |
| `remora reddit-login [<profile>]` | Log the Reddit app in through an embedded reddit.com login window |
| `remora reddit-login [<profile>] --user <name> --cookie -` | The same, headless: the captured session cookie is read from stdin |
| `remora gboard-reset [<profile>] [--wipe] [--yes]` | Un-pin Gboard's on-screen keyboard without losing its data. `--wipe --yes` falls back to clearing all Gboard data |

For `shell`, `logcat`, `prop` and `install`, the first argument is taken as a profile only when it
names one that exists. Anything else passes to adb untouched, so `remora shell ls -l` runs on the
active profile.

## Build

### Official images

```bash
remora images list
remora images fetch --tag remora24:x86_64-public
remora images rollback --tag remora24:x86_64-public
```

`images list` prints the signed catalog of Remora's public, redistributable Android 17 images.
`images fetch` downloads one and imports it, after checking the catalog's signature and validity
and the archive's SHA-256, Docker identity and tag. Anything that fails a check is refused before
Docker sees it. The image is imported under an immutable version tag and then tagged
`remora24:x86_64-public`. A previous image with that name is kept as `remora-rollback:public-…`.
`images rollback` restores it; Android data is never touched. Add `--host user@host` to run on that
Docker host. `--source` names another signed catalog location. The workspace's **Official
downloads** button adds the same source to the Image library.

The public image is x86_64 only. It has no Google apps and no ARM translation, so apps that ship
only ARM native code refuse to install on it. Add Google Play locally with `play-install` below.

### Optional Google Play

`remora play-install --show-terms` displays the provider, terms and acceptance version for the
optional Google Play package. Installation requires an already loaded, Google-free **Android 17
(SDK 37), x86_64** base. It creates a separate local image without compiling AOSP:

```bash
remora play-install --base YOUR_BASE_TAG --accept-terms litegapps-a17-20261001-v1
```

Add `--host user@host` to execute on that Docker host. Only Remora's helper source and manifest
are copied over SSH; that host downloads the payload directly. A remote Docker context is refused;
use `--host` instead. Python 3 and Docker must be available there. `--archive /host/path/file.zip`
uses an already downloaded copy of the exact pinned archive. `--tag remora-local:NAME` names the
derived image; otherwise its name includes the base and installer recipe hashes. Other tag
names and overwriting a different existing image are refused.

Select the resulting tag for a **fresh instance**. Existing profiles, running containers and
Android data are unchanged. Google/microG conflicts, wrong SDK/ABI, unsafe base links/ONBUILD
instructions and mismatching payloads are rejected before building. The cache and receipts live
on the Docker host at `${XDG_CACHE_HOME:-~/.cache}/remora/play/`, accessible only to that user.
The receipt binds the source, archive/APK hashes and audited certificate identities, local
permission adjustment, accepted terms, immutable base, recipe and result image.

```bash
remora play-install --rollback remora-local:NAME
```

Rollback removes that overlay tag only after checking its original base still exists and no
running **or stopped** container uses the overlay image. Remove the disposable container first,
retaining its data if needed. Rollback does not erase apps/accounts from Android data; use the
original base with its original data, or a fresh instance. It does not revert a Google login.

The package is proprietary, the installer licence grants no Google APK licence, and Remora is
not Google licensed or certified. Service access, Play Integrity, DRM and app compatibility are
not guaranteed. Keep derived images private. See [source and terms](../GOOGLE_PLAY_SOURCE_TERMS.md)
and [public image policy](../IMAGE_MIRROR.md). The public base, `remora24:x86_64-public`, is
the image this installer is meant for. The overlay it creates is private and is never published.

### Build commands

| command | what it does |
|---|---|
| `remora recipe [<profile>] [--base <tag>]` | Print the custom-image Dockerfile for the profile's features. `--base` layers onto that image instead of the best-matching registered tag |
| `remora build [--tag <tag>] [--base <tag>]` | Build that recipe for the active profile, as a Docker overlay. `--tag` names the result |
| `remora build --source [<profile>]` | Full source build of the profile's image: the same chain the workspace runs. A profile with the `public_release` feature builds the redistributable `remora24:x86_64-public` image, and the build fails if the release audit does |
| `remora images list\|fetch\|rollback [--tag <tag>] [--source <url>] [--host <user@host>]` | List, verify and import, or roll back an official signed image. See above |
| `remora venus-build [--dir <dir>] [--tag <version>\|latest]` | Build and install the NVIDIA Venus host renderer. `--dir` sets the install root, `--tag` the guest-library release |

## Move apps between instances

| command | what it does |
|---|---|
| `remora transfer-apps <from> <to> [--dry-run] [--packages a,b] [--with-data]` | Move installed APKs between devices. App data is not moved unless you add the experimental `--with-data` |
| `remora export-apks <from> <dir> [--packages a,b]` | Save installed APKs to a local directory |
| `remora import-apks <to> <dir> [--dry-run]` | Install APKs from a local directory |

`<from>` and `<to>` are a profile name or a raw adb target written as `host:port`.

## The workspace and the version

| command | what it does |
|---|---|
| `remora gui` | Launch the config workspace |
| `remora --version` | Print Remora's version. `remora version` does the same |

## The mirror client

`remora mirror` is the mirror client itself. It is not in the usage text because you do not
normally start it by hand: `up`, `reconnect`, `app`, `desktop`, `pip` and `record` launch it with
the right arguments. `remora plan` prints the client command a profile resolves to.

## Exit status

| status | meaning |
|---|---|
| 0 | success. For `check`, the target is ready |
| 1 | the command ran and failed. For `check`, the target is not ready |
| 2 | the request was refused before anything ran: an unknown command, an unknown profile, missing arguments, a refused `--backend` value, or a refused build configuration. A few commands also use it when they cannot proceed, such as `pip` when the device is unreachable |
