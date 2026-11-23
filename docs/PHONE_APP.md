# The phone app

From an Android phone, wherever it is, open any app on the computer: the computer's own Linux
apps, and the Android apps of a running Remora instance, each on a screen of its own sized to the
phone. This page is the design. What exists is a preview: the phone app in `phone/` (built as
[phone/README.md](../phone/README.md) says), `remora link`, which lets a paired phone in, and
`remora-host-agent`, which serves the computer's own apps. The reverse direction, phone apps on
the computer, comes later (§5).

Naming: "companion apps" already means the system apps bind-mounted into a container, so the new
thing is **the phone app**.

## 1. One protocol for every source

The phone app speaks protocol v3 ([MIRROR_AGENT.md](MIRROR_AGENT.md),
[MIRROR_PROTOCOL.md](MIRROR_PROTOCOL.md)) to everything it shows. A Remora instance is served by
the agent it already has. The computer's Linux apps are served by the **host agent**, which
implements the same session kinds: `list-apps` from the desktop entries, `new-display` with
`start_app`, a video and an audio connection, and the same control messages. So the phone has one
decoder, one input mapper and one launcher with a section per source.

Two additions to the protocol: `app-icon`, the app's launcher icon as a PNG (on the computer, from
the desktop's icon theme), because a phone has no shell on the device to ask; and a source
description in the listing, so the phone can default to a pointer mode for desktop apps. For a
Linux app, the package field of `list-apps` carries the desktop-file id.

Positional input is a fraction of the frame, so the two ends never need the same screen size.

## 2. The phone always dials the computer

A phone roams across networks and sits behind NAT, so the computer runs a small **link service**
(`remora link serve`). Pairing is by a QR code that `remora link pair` or the workspace's Phone
page shows: the computer's address, its certificate fingerprint and a one-time token. The phone
app scans it with its own camera and decoder, and the code is also a link the app opens, so the
phone's camera app or a pasted link pair it as well. After that,
every connection is TLS with both certificates pinned, carrying protocol v3 unchanged. The link
bridges a phone to each running instance's agent over the forward that exists already, and to the
host agent locally. The wire format is [LINK_PROTOCOL.md](LINK_PROTOCOL.md). Away from home, the
phone reaches the computer through the user's own VPN; Remora runs no relay.

## 3. Linux apps: a virtual screen in the live Plasma session

A Linux app needs a screen of the phone's size that it can be shown on, captured from and driven
through. Two ways were weighed:

| | A — a virtual output in the live session | B — a nested headless compositor per app |
|---|---|---|
| Single-instance apps (browsers, file managers, most KDE apps) | **Work** — a new window of the running instance is placed on the phone's output | **Fail** — the request goes to the running instance and the window opens on the desk; browsers refuse a second process on one profile |
| Desk locked | The lock screen covers the virtual output too; unlocking from the phone unlocks the session | Unaffected |
| Desk in use at the same time | One seat: keyboard focus is shared | Independent |
| Needs | A logged-in Plasma session | Nothing — works on a host with no desktop |

**A is the first version**, because the apps people want on a phone are overwhelmingly the
single-instance ones, and B cannot show them. It needs KDE Plasma 6. The interfaces it uses,
`zkde_screencast_unstable_v1` (`stream_virtual_output`) and fake input, come with no consent
dialog, which matters because nobody is at the desk to answer one. KWin restricts them to programs
whose desktop entry lists them in `X-KDE-Wayland-Interfaces`, and the agent ships that entry.

B remains the answer for hosts with no desktop session (a `remote` docker host) and comes later,
behind the same internal interface: *a screen of this size, this app on it, its frames, this
input*. Another desktop would go through the xdg-desktop-portal ScreenCast/RemoteDesktop pair, with
a restore token granted once at the desk.

### The virtual output and the desk

- **Size in pixels, scale from the layout.** KWin makes the width and height it is sent the
  output's mode in pixels, although the protocol calls them logical, and ignores the scale. The
  agent asks for the phone's pixels and sets the scale with the layout below, in KWin's own steps
  of 1/120. A change of scale alone (the phone app's *Scale*) is that layout applied again: the
  output, its capture and the encoder stay as they are.
- **The app must fit.** KWin keeps a window that cannot be as small as an output off it, so Elisa,
  at least 612x459, opened on the desk from a phone whose screen is 997x448 at its own scale 3.
  The session's script reports each window's minimum size, and the agent lowers the output's scale
  to the largest that holds it (in KWin's steps, never below 0.5) and places the window again.
- **The physical monitors stay where they are.** KWin lays out a set of outputs it has never seen
  from scratch, which moves the physical ones. The agent records their positions and priorities
  first, puts them back, and parks the virtual output outside their bounding box with the lowest
  priority, so no desk panel moves onto the phone's screen. A monitor that mirrors another is never
  named in that repair: it is shown at its source's geometry, and setting its own position would
  stop the mirroring.
- **Each session has its own stream.** A second consumer on an existing stream receives no
  frames, so the agent requests a stream per session and is its only consumer.
- **The lock.** The lock screen covers a virtual output like any other, so using an app from a
  locked desk means unlocking it from the phone. The agent locks the desk again when the last
  phone session ends, if it was locked when the first began. Switching the desk's monitors off
  meanwhile was tried and dropped: KWin turns a blanked monitor back on at the phone's next input,
  and a mirrored monitor cannot be switched off on its own.
- **GPU memory.** A session's frames live on the GPU that drives the desk, so a session that
  would leave that GPU too little memory for the desk is refused (`remora-host-agent serve
  --vram-reserve`, 1 GiB by default). The phone app's *Session size* setting asks for a smaller
  screen than the phone's, which the phone scales up, to spare the GPU and the network.

### The app on it

The agent launches the app the way the desktop launches it and places its window with a KWin
script per session, the one place that can move a window onto an output. Which window is the
app's is decided by desktop-file id, never by process id: a single-instance app answers a launch
with a new window of the process already running, and an app that was already open only asks for
its existing window to be activated, which KWin's focus-stealing prevention usually turns into a
request for attention. The script watches for both. The phone's windows are borderless,
maximized and on every virtual desktop, because desktops are global across outputs and switching
one at the desk must not take the app off the phone. When the session ends, a window that was
already on the desk goes back exactly as it was — where it was, and maximized or minimized again
if it had been, as recorded before the launch, since a launch that activates a window unminimizes
it — and one opened for the phone is minimized, not closed.

### Frames

Frames arrive from PipeWire as DMA-BUFs and feed the encoder `remora-frame-encoder` (DMA-BUF →
Vulkan → hardware encoder → protocol v3 video records) on the GPU that rendered them, so they
never leave it. The agent accepts every layout KWin offers except the two no importer can take:
the compressed layouts NVIDIA's Vulkan refuses, and the implicit modifier. It holds the newest
frame rather than handing it straight back, so KWin cannot draw into a buffer the encoder may
still be reading, and so the encoder can be served that frame again at once if it asks.

A session's video connection may go and come back. The phone lets its picture go while it is
away, with the screen off or another app in front, keeps the session and its sound, and attaches
a new video connection on its return; the encoder opens every new one with a keyframe. The phone
ends a session after five minutes away, and the agent's encoder gives up a little later on its
own. A client that never attaches its video is ended after 30 seconds.

### Reflow

A rotated phone, a resized window or the phone's on-screen keyboard reflows the app to the new
shape rather than stretching it. KWin cannot resize a virtual output, so a new one comes up beside
the old, the app moves over, a capture of the new one replaces the old capture on the same
encoder, and only then does the old output go. Density follows the long side, as the in-image
agent's reflow does, unless the RESIZE names one: the phone app always does, since its scale is
the client's to set, and the named density is the anchor for later ones.

### Input

Fake input drives pointer, touch, keys and keysyms. A pointer or a touch lands on the virtual
output by position, and it moves the desk's one real pointer there: the shared seat, in practice.
Keys go to whichever window has keyboard focus, so the agent tracks it: the session's KWin script
reports whether focus is on one of the phone's windows, and while it is not, the phone's keys
wait, the phone's window is given focus again, and the keys follow once it has it. If it does not
get focus, they are dropped rather than typed into a window on the desk. Desktop apps are
pointer-first, so the phone offers a pointer mode: tap to click, long-press for the secondary
button, two fingers to scroll.

Typed text has no simple path on Wayland: it goes as keysyms, and a paste goes through the
clipboard (the agent sets the desk's clipboard and presses Ctrl+V in the app). KWin types a keysym
by setting the modifier state around it, but an input method holding the keyboard (Plasma's own)
learns that state only from real keys, after the fact, so the agent reads the desk's keymap and
holds the real Shift or AltGr key around any character that needs it. The clipboard
follows in both directions through Klipper, the session's own clipboard reader and writer, over
D-Bus.

### Sound

The sound is that app's own. It goes to a sink of the session's own, whose monitor is encoded as
Opus, so it plays on the phone instead of the desk, and nothing else the desk plays goes with it.
The sink exists before the app is launched, and the app is launched pointed at it (`PULSE_SINK`),
so its sound and its children's starts on the phone. An app that was already running (a
single-instance app's process) or that is started by D-Bus activation did not get that
environment: its playback streams, matched by the processes behind the session's windows and
their children, are moved to the sink as they appear, so the first moment of each can still play
on the desk. When the session ends the sink goes, and an app still pointed at it falls back to the
desk's own output.

## 4. A phone is a linked device, not a backend

A phone has no image, no container and no deployment. It is something the mirror attaches to and
the link service knows about, and the backend policy (local or remote, nothing else) is
untouched.

## 5. Phone apps on the computer: later, and only with the shell uid

Launching another app's activity onto a virtual display an app created needs a *trusted* display,
and `ADD_TRUSTED_DISPLAY` is `signature|role`. Every role that grants it (app streaming,
nearby-device streaming, virtual device, system shell) is `systemOnly`, and `INJECT_EVENTS` is
signature-only. The shell package holds all of them. So an ordinary app on a stock phone cannot do
what the agent does; screen capture plus an accessibility service can only mirror the phone's own
screen, behind a consent prompt each session.

The phone side of this direction is therefore the agent itself, run as uid 2000: started from the
computer over adb (USB or wireless debugging), by the phone app through the phone's own wireless
debugging connection, or as root. Unlike the image's agent it has to run on frameworks Remora did
not build, so it carries a compatibility layer and a separate build; the image's agent stays
reflection-free. Google has been reported to be weighing restrictions on a phone's adb connection
to itself; if that lands, only the computer-started and root routes remain.

## Distribution

Android developer verification is enforced in Brazil, Indonesia, Singapore and Thailand since
September 2026 and worldwide from 2027. An APK from an unregistered developer needs the "advanced
flow" (developer options and a 24-hour wait), except when it is installed over adb, which skips
it, as it does for updates. Registering as a developer is the route for a phone app people install
themselves.
