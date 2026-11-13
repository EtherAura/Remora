# Android Apps on the Desktop

This page covers using Android apps as if they were desktop applications: the application-menu
folder, per-app windows, freezing and waking the device, starting it at login, and getting APKs
onto it and between instances.

## The application-menu folder

After each successful connect, Remora refreshes a launcher folder named after the profile in your
desktop's application menu. It holds one entry per installed Android app, under the app's real
icon, plus three entries for the device itself:

| entry | what it opens |
|---|---|
| *Android (mirror)* | the main mirror, bringing the device up first if needed |
| *Android (desktop)* | a separate desktop-mode display |
| *Android (PIP)* | the picture-in-picture toggle |

```sh
remora apps <profile>            # refresh the folder now (the device must be running)
remora apps <profile> --remove   # remove the folder (purely local)
```

`desktop_menu=false` turns the automatic refresh off. Existing entries then stay as they are until
you refresh or remove them yourself.

## One app, one window

```sh
remora app <profile> <package>
remora app <profile> <package> --title "My App"
```

This launches one Android app in a window of its own, on its own Android display, and brings the
device up first if it is not running. It is what each generated menu entry runs. The window is
paired with its menu entry, so your desktop shows the app's own name and icon.

Each app opens in one of two window modes, recorded per app in the profile's `app_modes`:

| mode | the window |
|---|---|
| `freeform` (default) | a phone-shaped window. Resizing it reflows the app. Backing out of the app closes the window |
| `desktop` | a window at the configured desktop resolution |

An app with no entry gets `freeform`, and that choice is recorded the first time it is listed or
launched. To change one, edit `app_modes` (entries look like `<package>=desktop`) or use the
workspace. `freeform_display` sets the geometry of the phone-shaped window (default
`1080x2340/420`).

## Sleep and wake

```sh
remora sleep <profile>   # freeze the device
remora up <profile>      # wake it
```

`remora sleep` freezes the whole device in place with `docker pause`: it uses no CPU and the
session is fully preserved. The next `remora up`, or a connect from the workspace, wakes it in
about a second instead of a cold boot.

### Auto-sleep

Set `auto_sleep_minutes` in the profile to freeze the device automatically after that many minutes
with no mirror window attached. It is off by default. The setting is enforced by a watcher,
`remora auto-sleep [<profile>]`, which the login service below installs for you. The watcher
re-reads the configuration as it runs, so changing the setting needs no restart.

## Start the device at login

```sh
remora service <profile>            # install and enable
remora service <profile> --remove   # disable and remove
```

`remora service` writes systemd user units into `~/.config/systemd/user` and enables them:

| unit | what it does |
|---|---|
| `remora-<profile>.service` | runs `remora up` at login and `remora sleep` at logout, so the next login resumes the session |
| `remora-updater-<profile>.service` | runs `remora update-watch`, the host half of the in-Android updater |
| `remora-autosleep-<profile>.service` | runs `remora auto-sleep`, which does nothing until `auto_sleep_minutes` is set |

The units belong to your graphical session. To start the device without logging out and in:

```sh
systemctl --user start remora-<profile>.service
```

Characters in the profile name other than letters, digits, `_`, `.` and `-` become `-` in the unit
name. The command prints the exact names it installed.

## Installing an APK

```sh
remora install app.apk
remora install <profile> -r app.apk
```

This pushes and installs an APK through adb. Flags such as `-r` and `-g` pass straight to
`adb install`. Before installing, Remora switches off the Play Protect verifier for adb installs,
so an unknown APK cannot sit for minutes in the upload-consent prompt.

With the `gapps` or `microg` feature in the image you can also install from the store inside
Android.

## Moving apps between instances

Three commands move installed APKs, split APKs included. Each endpoint is a profile name or a raw
adb target written as `host:port`.

| command | what it does |
|---|---|
| `remora export-apks <from> <dir> [--packages a,b]` | saves each app to `<dir>/<package>/` |
| `remora import-apks <to> <dir> [--dry-run]` | installs `<dir>/<package>/*.apk`, or any loose `.apk` files in `<dir>` |
| `remora transfer-apps <from> <to> [--dry-run] [--packages a,b] [--with-data]` | moves apps directly from one device to another |

For `export-apks` and `transfer-apps`, leaving out `--packages` includes every third-party app.

**App data is not transferred.** Apps arrive as fresh installs, logged out. `--with-data` on
`transfer-apps` also carries each app's data, but it is experimental and needs root on both sides,
and a data failure leaves the app installed but logged out. When both sides are profiles,
window-mode preferences ride along for the transferred apps.

## Keeping apps working

| setting | default | what it does |
|---|---|---|
| `arm_apps` | none | packages pinned to their arm64 build, for apps whose x86_64 build is absent or broken |
| `arm_guard` | on | after each connect, re-fetches a pinned package that Play updated back to x86_64 |
| `auto_update_apps` / `auto_update_hours` | off / 24 | runs the Play update sweep after a connect when this many hours have passed |
| `shared_folders` | none | host directories visible in Android under `/sdcard/<name>` |

Changing `shared_folders` recreates the container on the next connect.

## Google sign-in and Play certification

An uncertified device may be refused Google sign-in. `remora certify` reports state; it does not
certify a device. Android ID registration and identity spoofing do not grant a Google licence or
establish permission to run GApps on Remora. The selected optional add-on design uses a separate
third-party download and local installation with explicit user acceptance of its terms and
limitations; see the [feasibility evaluation](../WINDOWS_AND_IMAGE_FEASIBILITY.md).

For an existing locally configured instance, `remora certify [<profile>]` prints its Play Protect
state and the `android_id` to register with Google, and tells you the next step. Registration happens on Google's site under
your own login, so Remora cannot do it for you. After registering, run
`remora certify --recheck`.

Remora saves a copy of the checkin identity whenever `remora certify` reads a healthy one off the
device, so run it once on a working instance. After you rebuild that instance's data,
`remora certify --restore` writes the saved identity back.

## The in-Android updater

An image built with the `system_updater` feature has *Remora Updater* under Android's system
settings. Pressing *Apply* there signals the host, and `remora update-watch` on the host recreates
the container onto the new image. The login service installs that watcher.
