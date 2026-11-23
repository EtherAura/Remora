# The Remora phone app

This computer's Linux apps and its Remora profiles' Android apps, each opened on an Android phone
at the phone's own size and density ([docs/PHONE_APP.md](../docs/PHONE_APP.md)). A preview.

- **Pairing** — run `remora link pair` (or *New code* on the workspace's Phone page) and scan the
  code with the phone's camera; the link opens this app. The phone's key is made in the Android
  Keystore and never leaves it; each side pins the other's certificate
  ([LINK_PROTOCOL.md](../docs/LINK_PROTOCOL.md)).
- **Sources** — this computer's apps while `remora-host-agent serve` runs, and every profile whose
  Android is up, while `remora link serve` (or `remora-link.service`) lets phones in.
- **A session** — a new display of the phone's size on the source, the app launched on it,
  decoded in the phone's hardware; touch, the keyboard (the ⌨ button), Back, rotation, sound and
  the clipboard go both ways over protocol v3 ([MIRROR_AGENT.md](../docs/MIRROR_AGENT.md)).
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
  the computer's GPU, the network and the phone's decoder.

## Building

Java and the framework only — no Gradle, no AndroidX — against an Android source tree's prebuilt
SDK, the way `agent/build.sh` builds the agent:

```
phone/build.sh /path/to/android/tree      # → phone/out/remora-phone.apk
adb install -r phone/out/remora-phone.apk
```

It needs Android 10 or later.
