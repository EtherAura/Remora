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
| `remora certify [<profile>] [--recheck] [--restore]` | Play Protect state and the `android_id` to register. `--recheck` asks for a fresh checkin first. `--restore` writes back the saved checkin identity after a `/data` rebuild |

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

| command | what it does |
|---|---|
| `remora recipe [<profile>] [--base <tag>]` | Print the custom-image Dockerfile for the profile's features. `--base` layers onto that image instead of the best-matching registered tag |
| `remora build [--tag <tag>] [--base <tag>]` | Build that recipe for the active profile, as a Docker overlay. `--tag` names the result |
| `remora build --source [<profile>]` | Full source build of the profile's image: the same chain the workspace runs |
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
