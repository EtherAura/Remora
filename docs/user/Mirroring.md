# Mirroring

This page explains how you see and control the Android device: the mirror client on your desktop
and the agent inside the image, what travels between them, the settings that shape the stream,
and the extra windows you can open (a desktop-mode display, picture-in-picture, a recording).

## The two halves

| half | what it is | where it lives |
|---|---|---|
| the client | `remora mirror`, a Qt and FFmpeg program inside the `remora` binary | your desktop |
| the agent | a Remora service baked into the image by the `mirror_agent` feature | the Android instance |

Android's init starts the agent at every boot and restarts it if it crashes. Nothing is pushed to
the device per session. Because both halves are Remora's own, they speak Remora's own wire
protocol, currently version 3.

You rarely start the client yourself. `remora up` and `remora reconnect` launch it, and
`remora plan` prints the exact client command a profile resolves to.

Two refusals are worth knowing in advance:

- **An image with no agent cannot be mirrored.** The client refuses to start and tells you to
  rebuild the image with the `mirror_agent` feature. There is no fallback path.
- **A client and an agent on different protocol versions refuse each other**, with a message
  naming the side to update. An older agent means rebuilding the image. A newer agent means
  updating Remora.

## What travels over it

| stream | detail |
|---|---|
| video | H.264, H.265 or AV1 |
| audio | Opus, 48 kHz stereo, captured inside Android. Off by default |
| input | touch with full multi-pointer state, mouse buttons and hover, scroll, keys, typed text |
| navigation | Back, Home, Recents, the notification and quick-settings panels |
| clipboard | both ways |
| displays | extra desktop-mode displays, created and resized on request |
| app list | the installed apps, used for the application menu |

The agent listens on a unix socket inside the container, and the client reaches it through an adb
forward on a local port between 27183 and 27199. That works the same way for a `remote` target.
Pointer positions travel as fractions of the frame, so input stays correct when the video is
scaled or the display is resized.

Sessions are independent and concurrent: the main mirror, a picture-in-picture window and any
number of app windows can run at once.

## The mirror outlives the workspace

The mirror window is a separate process in its own systemd scope. Closing the workspace, or the
terminal that ran `remora up`, does not close it. `remora reconnect` attaches a new mirror to an
instance that is already running, without any bring-up.

## Video and audio settings

| key | default | meaning |
|---|---|---|
| `video_codec` | `h265` | `h264`, `h265` or `av1`. Falls back to h264, with the reason shown by `remora plan`, in `gpu_mode=guest` or on a host GPU with no VA-API encode |
| `video_bit_rate` | `30M` | mirror bit rate |
| `max_size` | 0 | longest side of the mirrored video. 0 means the display's full size |
| `max_fps` | unset, or 60 when `fps` is above 60 | cap on the encoder's frame rate |
| `audio` | off | mirror audio |
| `hw_decode` | on | decode on this machine's GPU: NVDEC, then VA-API, then software. `false` forces software |
| `host_encode` | off | encode on the host GPU instead of inside Android. `gpu_mode=host` and the Venus stack only |
| `host_encode_max_size` | 0 | cap on the host encoder's output size, longest side |

With `host_encode`, frames go from Remora's own hwcomposer to a hardware encoder on the host GPU,
and the client reads the stream from that encoder. It is the route to hardware H.265 on NVIDIA,
whose VA-API driver cannot encode. See [GPU Acceleration](GPU-Acceleration.md#what-the-gpu-means-for-the-mirror).

## The mirror window

| key | default | meaning |
|---|---|---|
| `window_width`, `window_height`, `window_x`, `window_y` | unset | window geometry |
| `window_fullscreen` | off | open the mirror fullscreen |
| `mirror_extra` | none | extra flags passed to `remora mirror` |

The window's frame belongs to your desktop: there is no border setting. For a frameless mirror,
add a *No titlebar and frame* window rule for the `remora` window class.

### Navigation bindings

The left mouse button is always touch. The other buttons and a set of keyboard chords trigger
Android's navigation actions. The Viewer page's *Navigation* card edits them, and it writes three
keys:

| key | default | meaning |
|---|---|---|
| `mouse_bind` | `bhsn:++++` | right = Back, middle = Home, 4th = Recents, 5th = Notifications. Shifted clicks pass through to Android |
| `shortcut_mod` | `lalt,lsuper` | the modifier that arms the chords: MOD+b Back, MOD+h Home, MOD+s Recents, MOD+n Notifications |
| `key_bind` | none | single keys bound to an action, for example `F1:b,F2:h,F3:s`. A bound key never reaches Android |

The exact value formats are in [Configuration](Configuration.md#mirror-window-and-input).

## A desktop-mode display

Besides mirroring the main display, the agent can create a separate Android display and show it in
its own window. The display follows the window: resize the window and Android reflows to fit.

```sh
remora desktop            # the active profile
remora desktop <profile>
```

This opens a desktop-mode display of the plain device, with no particular app. It is what the
*Android (desktop)* menu entry runs. Per-app windows use the same mechanism: see
[Android Apps on the Desktop](Android-Apps-on-the-Desktop.md).

| key | default | meaning |
|---|---|---|
| `fullscreen_separate_display` | off | *Fullscreen* opens a separate desktop-mode display instead of mirroring |
| `fullscreen_display` | the main display's size | that display's `WxH[/dpi]` |
| `fullscreen_app` | none | the app to start on it |
| `desktop_new_window` | off | the desktop shortcut opens a new window each press instead of replacing the last |

## Picture-in-picture

```sh
remora pip            # toggle for the active profile
remora pip <profile>
```

`remora pip` toggles a second, small, always-on-top mirror beside the running one. It opens one if
none is up and closes it otherwise, so it binds cleanly to a hotkey. The device must already be
running. The window is `pip_width` by `pip_height` (480 by 270 by default) and uses the profile's
codec at a lower bit rate (6M) with a 960-pixel cap, which keeps a second session cheap.

The toggle is per profile, and every profile's menu folder has an *Android (PIP)* entry. Every PIP
window has the class `remora-pip`, distinct from the main mirror's `remora`, so one window rule
covers all of them. [FEATURES.md](../FEATURES.md) has a ready-made KWin rule that keeps it on
every desktop and above other windows.

## Recording and screenshots

```sh
remora record                    # the active profile, a timestamped .mp4 in the current directory
remora record <profile> out.mp4
remora shot <profile> out.png
```

`remora record` relaunches the mirror with recording on. The recorded session replaces the running
mirror. Close the mirror window to finish the file. The instance must be running.

`remora shot` saves a device screenshot and needs no mirror. Without a file name it writes a
timestamped `.png` in the current directory and prints the path.

## Watching the boot

A normal connect waits until Android has booted before it opens the mirror.
`remora watch-boot [<profile>]` restarts the device and attaches during boot, so you can see the
boot animation.

## Rotating the display

`remora rotate [<profile>]` prints the current rotation. Give it `landscape`, `portrait`,
`landscape-flip`, `portrait-flip` or a number from 0 to 3 to set one, or `auto` to turn auto-rotate
on. Auto-rotate drives the rotation on an image built with the `sensors` feature.

For the protocol itself, read [MIRROR_AGENT.md](../MIRROR_AGENT.md) and
[MIRROR_PROTOCOL.md](../MIRROR_PROTOCOL.md).
