# The Remora agent (protocol v2)

The device-side half of the mirror. It is a **persistent agent living in the image**: started by
Android init at boot, supervised by init, and speaking a versioned protocol to the desktop client,
`remora mirror` (`src/mirror/`). Its sources are in `agent/`; the `mirror_agent` build feature bakes
it into the image. It replaced a server jar that was pushed over adb and bootstrapped through
`app_process` for every session.

## Why owning the image changes the design

A mirror server built to run on *someone else's* Android has to use reflection over hidden APIs
that shift per release, gate on an exact client/server version because both ship as one artifact,
push and spawn itself per session because nothing persists, and live with shell-uid capabilities
because adbd is the only way in. None of that applies to an image Remora builds:

- **No reflection.** The agent compiles in-tree against the platform `framework.jar` (the full jar,
  hidden APIs included). Internal API drift becomes a compile error at image-build time, not a
  runtime crash.
- **No sepolicy work.** The image runs with SELinux stubbed (the `container_compat_patches`
  feature), so an init service declaration suffices.
- **No push, no per-session bootstrap.** Init starts the agent at every boot — including container
  restarts Remora never sees — and restarts it if it crashes, with no host-side babysitting.
- **No exact-match version gate.** A versioned hello with capability flags replaces it; client and
  agent evolve independently.

## Lifecycle

The agent is an init service:

```
service remora_agent /system/bin/app_process -Djava.class.path=<agent jar or dex> /system/bin com.remora.agent.Main
    class late_start
    user shell
    group shell log
    disabled            # started by the sys.boot_completed trigger

on property:sys.boot_completed=1
    start remora_agent
```

`user shell` gives it the capability set a mirror needs (input injection, SurfaceControl,
MediaCodec) and, because uid 2000 owns `com.android.shell`, which holds
`READ_CLIPBOARD_IN_BACKGROUND`, the clipboard from the background too. It starts at
`sys.boot_completed` rather than with its class because it binds system services that do not exist
in early boot.

There are two ways it gets into a container, and the resolver picks exactly one:

- **Baked** (how images ship). The `mirror_agent` build feature adds the soong module and its rc
  (`vendor/source-build/features/remora_agent/`), installing the agent as
  `/system_ext/framework/remora-agent.jar`. A profile whose image bakes the agent — `mirror_agent`
  in its features, or an image tag registered as providing it — gets **no** agent mounts, because a
  second definition of `service remora_agent` is a duplicate init discards, and which of the two it
  read first would be a parse-order accident.
- **Mounted** (the development loop). `agent/build.sh <tree>` compiles the dex against a built
  source tree and writes `vendor/agent/agent.dex`. For an image that does not bake the agent, a
  deploy bind-mounts that dex at `/remora/agent.dex` and `vendor/container-scripts/remora-agent.rc`
  at `/vendor/etc/init/remora-agent.rc`. Iterate by rebuilding the dex and running
  `setprop ctl.restart remora_agent` — no image rebuild. Both mounts are skipped when the dex has
  not been built, since an rc whose class path is missing is a service init retries forever.

An image with neither has no agent, and the mirror **refuses** to start with a message naming the
fix: `no in-image agent (…) — rebuild the image with the mirror_agent feature`. There is no
fallback to another device-side path.

### The framework, without reflection

The agent bootstraps like a system service, not like an app: `Looper.prepareMainLooper()`, then
`ActivityThread.systemMain()` for the system context, and `main()` parks in `Looper.loop()` because
clipboard change callbacks dispatch through a `Handler` on that looper — the accept loop gets its
own thread. Hidden APIs are plain calls: `InputManagerGlobal.injectInputEvent`,
`InputEvent.setDisplayId`, `MotionEvent.setActionButton`, `IStatusBarService`,
`DisplayManagerGlobal.getDisplayInfo`. One identity fix-up remains: the system context's package is
`android`, which uid 2000 does not own, so `ShellContext` re-attributes it to `com.android.shell`
and managers that need that identity (the clipboard) are constructed directly against it.

## Transport

The agent listens on **`localabstract:remora_agent`** — a fixed name, because a persistent agent
needs no per-session id — and the client reaches it through an adb **forward** tunnel on a local
port in 27183–27199. adb's key-based auth stays the trust boundary, and it works identically for
`backend=remote` over ssh.

Because the agent is always listening, the client always dials in, and the connect itself is the
probe: an image with no agent refuses the connection, and the hello reply proves what answered.
Running out of tunnel ports is reported as a host-side problem (`could not open an adb tunnel to the
device`), not as a missing agent.

## Wire protocol v2

Every connection opens with a hello, then a role byte saying what the connection is for. Control
messages, device messages and the media stream framing keep the byte layouts specified in
[MIRROR_PROTOCOL.md](MIRROR_PROTOCOL.md); only the handshake and session setup are new. All
integers are big-endian.

### Connection hello (every connection)

```
client → agent:  "RMRA"  u16 protoVersion  u16 flags(reserved=0)
agent  → client: "RMRA"  u16 protoVersion  u16 capabilities
```

The current version is 2. A version mismatch is a negotiation (lowest common), not an error.
Capability bits announce optional features so the client never guesses; none are defined yet.

### Connection role

One socket name serves every kind of connection, so the byte right after the hello says which:

```
u8 role        0=control  1=video  2=audio
```

Without it, a control connection opening a mirror session (kind 1) and a video connection
attaching would both lead with `0x01`, and the agent could not tell them apart before reading
role-specific fields. Making audio a role rather than a positional third socket means a session
without video no longer renumbers everything after it.

### Control connection: session request

After the role byte, the client sends one session request:

```
u8  kind       1=mirror  2=compose  3=new-display  4=list-apps
u16 optLen     then optLen bytes of UTF-8 "key=value\n" lines
```

The options the agent reads: `video_codec` (`h264`, `h265`, `av1`; default h264),
`video_encoder`, `video_bit_rate`, `max_size`, `max_fps`, `display_id`, `new_display`
(`WxH[/dpi]`, or empty to follow the main display), `start_app`, `close_on_app_exit`,
`video_compose` and `audio_bit_rate`. Unknown keys are ignored, so new options are
forward-compatible. Because the block is length-prefixed, values carry none of the shell-escaping
limits of a command line.

The agent replies `u8 status  u32 sessionId`; a non-zero status is followed by a u16-prefixed UTF-8
reason. On success the connection then *is* the session's control socket: control messages flow
client → agent and device messages agent → client, exactly as specified in MIRROR_PROTOCOL.md.

`list-apps` is a request/response on its own control connection: the ok reply, then one
u32-prefixed UTF-8 blob of `" * Name  pkg"` lines. The connection is done once it is written.

### Video connection

Hello, role `1`, then `u32 sessionId` to attach to that session's video. The stream that follows is
the framing in MIRROR_PROTOCOL.md §3: a `u32` codec id, a 12-byte session packet, then 12-byte
headers and payloads. There is no device-name preamble and no dummy byte — the hello supersedes
both.

A video connection closing takes the video down and leaves the session alive, so a client can
reattach without renegotiating; the *control* connection closing ends the session and stops the
encoder with it. That asymmetry preserves the engine's supervision model — kill the client and
everything it owned goes with it, with no host-side reaping.

### Audio connection

Hello, role `2`, then `u32 sessionId`. The agent captures Android's remote submix and encodes opus
(48 kHz stereo); the stream is a codec id followed by media packets, with no session packet. An
unknown session id gets the configuration-error sentinel in place of a codec id. The client treats
a failed audio connection as "no audio", never as a failed mirror.

### Coordinates

Positional control messages carry a point in the client's video coordinate space plus that
space's size. The agent **scales** rather than rejecting a mismatch: client point × display size ÷
client's declared size, read live from `DisplayInfo`. That is correct for the common case (a
`max_size`-capped video is permanently a different size from the display), it tracks a mid-session
resize with no capture-side handshake, and it lets input work on a session that has no video
connection at all.

### Session kinds

- **mirror** — the display (`display_id`, default 0) captured into a MediaCodec encoder. Input
  covers touch with full multi-pointer state and the mouse button press/release sequence browsers
  need, scroll, key codes, text with dead-key decomposition, back/screen-on, the status-bar panels,
  and the clipboard both ways. The encoder is chosen by name or type, sized within the codec's
  capability constraints, downsized on an error before the first frame, and given a bounded wait
  for first output after an error recovery.

  Capture is an `AUTO_MIRROR` virtual display created at the constrained size and at the source
  display's refresh rate. Requesting that rate matters — left unset, the mirror latches at 60 Hz
  whatever the display is doing. Asking for the constrained size directly is what applies
  `max_size`, so there is no filter and no GL pass between the display and the encoder's input
  surface.

  Measured against the jar-based server it replaced, on Android 17 at 3760×1992 with h265 on the
  VA-API HEVC encoder: 60.06 fps vs 60.12 while scrolling (both at the display cap), 10.20 vs
  10.07 fps on a static screen (both the repeat-frame timer), and 383 ms vs 511 ms to first frame
  — the push and `app_process` spawn that no longer happen.
- **compose** — the display is composed into an RGBA `ImageReader` with `GPU_COLOR_OUTPUT` usage,
  drained and dropped, with a nudge schedule that keeps composition alive. No video connection
  exists: the host encoder serves the pixels (`host_encode`).
- **new-display** — creates a virtual display (`new_display` size/dpi, or the main display's), optionally
  starts `start_app` on it, reflows it when the client sends `RESIZE_DISPLAY`, ends the session when
  the watched app exits if `close_on_app_exit=true`, and destroys the display's content on close.
  The agent drives composition itself, because a fresh display composes nothing until something
  does. `video_compose=true` sends its frames to the host encoder, as compose does.

Sessions are independent and concurrent (main mirror, PIP, any number of app windows). The agent
owns encoder lifecycles and tears a session down when its control connection closes.

## Client integration

`MirrorSession` connects by opening the forward tunnel, sending the hello and role, and making the
session request — no jar push, no `app_process` spawn, no `CLASSPATH`. It distinguishes *absent*
from *unhappy*: no connection or no `RMRA` magic means the image has no agent, and the mirror
refuses with the rebuild message above; an agent that answers and then declines a session is
reported with the agent's own reason (`the agent declined the session: …`).

Host-side supervision is unchanged in shape: the same tunnel port range, the same detached and
scoped client process. Stale device-side sessions are not a category — the agent reaps a session
when its control connection closes. Forwards left behind by a killed client are swept: each session
holds a lock file for its port, and a forward whose lock is free is removed.
