# The Remora phone app

This computer's Linux apps and its Remora profiles' Android apps, each opened on an Android phone
at the phone's own size and density ([docs/PHONE_APP.md](../docs/PHONE_APP.md)). A preview.

- **Pairing** — run `remora link pair` (or *New code* on the workspace's Phone page) and scan the
  code from the app: *Scan the code*, or *Pair another computer* once one is paired. The phone's
  own camera app works too, since the code is a link this app opens, and the scan screen takes
  the link pasted. The QR decoder is the app's own (`QrDecoder.java`, no libraries), and
  `phone/test/run.sh` tests it on the computer against qrencode. The phone's key is made in the
  Android Keystore and never leaves it; each side pins the other's certificate
  ([LINK_PROTOCOL.md](../docs/LINK_PROTOCOL.md)).
- **Sources** — this computer's apps while `remora-host-agent serve` runs, and every profile whose
  Android is up, while `remora link serve` (or `remora-link.service`) lets phones in.
- **A session** — a new display of the phone's size on the source, the app launched on it,
  decoded in the phone's hardware; touch or a pointer, the keyboard, Back, rotation, sound and the
  clipboard go both ways over protocol v3 ([MIRROR_AGENT.md](../docs/MIRROR_AGENT.md)).
- **Session controls** — a small handle on the edge of the screen, which can be dragged anywhere
  along either side and stays where it was left. It opens a panel: *Keyboard*; *Paste from phone*,
  which types what the phone copied into the app's focused field; *Input*; *Size* and *Scale*, both
  changed in place; for a computer's app, *Window* and *Desk*; and *End session*, which closes the
  app's screen on the computer and goes back to the list. Back goes to the remote app, and closes the panel first when it is open.
- **Input** — *Pointer* or *Touch*, per source. This computer's apps start with the pointer, since
  desktop apps are made for a mouse: the pointer goes where the finger is, a tap clicks, a long
  press is the secondary click, one finger dragged holds the button (selecting text, moving a
  slider), and two fingers scroll. Android's apps start with touch, every finger its own.
- **Away from a session** — the screen going off or another app in front leaves the session
  open, and its sound playing; the notification leads back to it, picture and all. After five
  minutes away it ends, since it holds a screen and an encoder on the computer. A connection lost
  meanwhile (the network changed, the computer restarted its service) opens the app again in a new
  session on the way back. Android cuts a backgrounded app's network and mutes its sound unless it
  runs a foreground service, so the app runs one for each session, and asks once to show its
  notification.
- **Session size** — on the main screen: *Full* asks for the phone's own pixels; *Balanced* and
  *Light* cap the remote display's long side at 1920 or 1280 px, at proportionally lower density,
  so the app is laid out the same and the phone scales the picture up. A fraction of the work for
  the computer's GPU, the network and the phone's decoder. It changes from inside a session too.
- **Scale** — how large the app is laid out, per source, from 50 to 150% of the phone's own
  density: smaller fits more of a desktop app on the phone. A desktop app that cannot be as small
  as the phone's screen at that scale (Elisa's window is at least 612x459) is shown at the largest
  scale it fits instead.
- **Window** — a computer's app fills the phone's screen, or, stepped down, takes 90, 75, 60 or
  50% of it, centred. Per app. Never smaller than the app's own minimum.
- **Fit** — which sides *Window* takes from: *Stretch*, both; *Width*, the app keeps the screen's
  full width and *Window* sets its height; *Height*, the app keeps the full height and *Window*
  sets its width. Per app.
- **Bypass desktop** — *On*, the default: the app fills a screen of its own on the computer,
  maximized, so no desktop shows around it and the computer's tiling scripts (Krohnkite and the
  like) leave it alone; a smaller *Window* is a smaller screen, shown at the same size on black.
  *Off*: the phone's screen is a desktop of the computer's, and the app a window on it like any
  other — its title bar, which the pointer can move it by and resize it from, the desktop behind
  it, and the tiling scripts arranging it. Per computer.
- **Desk** — *Extended*: the phone's screen extends the computer's desk, so the desk's pointer and
  windows can cross onto it. *Separate*: it stands apart, where the desk's pointer cannot reach;
  when the phone's pointer has taken the desk's pointer there, it goes back to where it was once
  the phone stops pointing. Per computer.
- **Keys** — go only to a window on the phone's screen. When a window on the computer's own
  screens has the keyboard (someone at the desk clicked into it), the phone's keys are not typed,
  and the phone says so: tap the app, then type. The computer has one keyboard focus and one
  pointer, so using the desk and the phone at the same time takes turns.

## Building

Java and the framework only — no Gradle, no AndroidX — against an Android source tree's prebuilt
SDK, the way `agent/build.sh` builds the agent, or against an Android SDK that has
`platforms;android-36` and `build-tools;36.0.0`:

```
phone/build.sh /path/to/android/tree      # → phone/out/remora-phone.apk
phone/build.sh "$ANDROID_HOME"            # the same, from an SDK
adb install -r phone/out/remora-phone.apk
```

It needs Android 10 or later. A build is signed with your debug key. Each release carries the app
signed with the release key instead (`.github/workflows/release.yml`, the key's certificate pinned
by `release-cert.sha256`), and Android updates neither with the other: uninstall one before
installing the other.
