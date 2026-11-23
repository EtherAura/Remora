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
  decoded in the phone's hardware; touch, the keyboard, Back, rotation, sound and the clipboard
  go both ways over protocol v3 ([MIRROR_AGENT.md](../docs/MIRROR_AGENT.md)).
- **Session controls** — a small handle on the edge of the screen, which can be dragged anywhere
  along either side and stays where it was left. It opens a panel: *Keyboard*; *Paste from phone*,
  which types what the phone copied into the app's focused field; *Size* and *Scale*, both changed
  in place; and *End session*, which closes the app's screen on the computer and goes back to the
  list. Back goes to the remote app, and closes the panel first when it is open.
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

## Building

Java and the framework only — no Gradle, no AndroidX — against an Android source tree's prebuilt
SDK, the way `agent/build.sh` builds the agent:

```
phone/build.sh /path/to/android/tree      # → phone/out/remora-phone.apk
adb install -r phone/out/remora-phone.apk
```

It needs Android 10 or later.
