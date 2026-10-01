# Mirror wire protocol

The byte-level contract between the Remora mirror client (`remora mirror`) and the in-image Remora
agent: the record framing, the media streams, the control and device messages, and the host-side
external video socket. How a connection is opened and a session set up — the `RMRA` hello,
connection roles, session kinds — is in [MIRROR_AGENT.md](MIRROR_AGENT.md).

`src/core/MirrorProto.{h,cpp}` implements the client side of this document and
`tests/test_mirrorproto.cpp` pins it byte for byte; the agent's side is `Records.java`,
`ControlReader.java` and `VideoStreamer.java` under `agent/src/com/remora/agent/`, and the host
encoder's is `vendor/native/remora-frame-encoder.c`. A change to any of them is a change to this
contract, and to the protocol version in the hello.

This is **protocol v3**. All integers are **big-endian**; `f32` is an IEEE-754 binary32, also
big-endian. There is no alignment or padding.

## 1. Records

Once a connection's setup is done (hello, role, session request or attach), everything on it — in
both directions, on every kind of connection — is a sequence of records:

```
u32 size    bytes that follow: the type byte plus the body (≥ 1)
u8  type
    body    size − 1 bytes
```

- A receiver **skips a record whose type it does not know**. The size says where the next one
  starts, so a newer peer's additions never desynchronise an older one.
- A record whose size is 0 or over the connection's limit is a framing error; the receiver drops
  the connection. The limits are 1 MiB on control connections and 32 MiB on media streams.
- A type's body may grow at the end in a later version. A receiver reads the fields it knows and
  ignores trailing bytes; a body *shorter* than its known fields is a framing error.
- The sender flushes after every record, so a receiver may parse incrementally.

## 2. Media streams (video and audio connections)

| type | record | body |
|---|---|---|
| 0x01 | START | `u8 codec` |
| 0x02 | FORMAT | `u32 width, u32 height, u8 flags` — bit 0: the size follows a client resize |
| 0x03 | CONFIG | codec configuration: H.264/H.265 parameter sets, the Opus ID header |
| 0x04 | FRAME | `u64 ptsUs, u8 flags, data` — bit 0: key frame; `data` is at least one byte |
| 0x05 | END | `u8 reason, UTF-8 detail` — reason 0: unavailable, carry on without this stream; 1: failed, stop |

**Codecs** (`u8`): `0x01` H.264, `0x02` H.265, `0x03` AV1; `0x81` Opus, `0x82` AAC, `0x83` FLAC,
`0x84` PCM (s16le). Audio is always 48 kHz stereo. The agent produces H.264, H.265 or AV1 video
and Opus audio.

Order:

- **START** is the first record of every stream, or **END** comes in its place. Nothing but those
  two is valid before START.
- A **video** stream sends FORMAT after START and before any CONFIG or FRAME, and again whenever
  the encoder restarts — a size change (rotation, a resized new display) or an error recovery — so
  a FORMAT may repeat the current size. A new CONFIG follows it before the next FRAME. Audio
  streams never carry FORMAT.
- **END** is the last record: the sender closes after it. It may come at any point, so an agent
  that loses its encoder says why instead of just closing.
- The PTS is in microseconds. For H.264 and H.265 only, the client prepends a CONFIG body to the
  next FRAME before decoding rather than decoding it alone; AV1 and audio CONFIG bodies go to the
  decoder as they are.

### 2.1 The external video socket (compose and host encode)

When the mirror is encoded on the host (`host_encode`), the agent runs a **compose** session — the
display is composed but not encoded, and there is no video connection — and the picture comes from
`remora-frame-encoder` over a host unix socket instead. That socket carries exactly the video
stream above, with no hello: START and FORMAT on connect, CONFIG, then FRAMEs.

- The encoder listens; the client connects, retrying up to 40 times 50 ms apart within a 12-second
  deadline.
- PTS is wall-clock microseconds since the first frame.
- The encoder re-sends FORMAT and CONFIG on every size change, and re-encodes the newest frame
  every 300 ms as a keepalive on a still screen.
- The client arms a first-frame deadline (45 s by default) and fails the session if no frame
  arrives within it.

## 3. Control messages, client → agent

On the session's control connection. Every type here is one the agent acts on.

| type | message | body |
|---|---|---|
| 0x01 | KEY | `u8 action, i32 keycode, u32 repeat, i32 metaState` — Android `KeyEvent` values |
| 0x02 | TEXT | UTF-8 text, typed through the virtual keyboard's character map |
| 0x03 | POINTER | `u8 action, u8 tool, u32 pointerId, f32 x, f32 y, f32 pressure, i32 actionButton, i32 buttons` |
| 0x04 | SCROLL | `f32 x, f32 y, f32 hScroll, f32 vScroll, i32 buttons` |
| 0x05 | BACK | `u8 action` — Back, or power when the display is off (down half only) |
| 0x06 | PANEL | `u8 panel` — 0 collapse, 1 notifications, 2 quick settings |
| 0x07 | CLIPBOARD | `u64 sequence, u8 paste, UTF-8 text` — sequence 0 asks for no acknowledgement |
| 0x08 | RESIZE | `u16 width, u16 height` — reflows a new-display session; a mirror ignores it |

- **x, y** are **fractions of the frame the client is showing**: 0 is the left or top edge, 1 the
  right or bottom. The agent multiplies by the session display's current size, so the client never
  needs to know it — a `max_size`-capped video, a mid-session resize and a session with no video
  connection all map the same way. A drag past the edge may carry values outside [0, 1].
- **tool**: 0 finger, 1 mouse; the agent treats an unknown tool as a finger. `action` is an Android `MotionEvent` action; `pressure` is in
  [0, 1]. A mouse POINTER with no buttons held moves the hover cursor.
- **Scroll** amounts are in Android's `AXIS_HSCROLL`/`AXIS_VSCROLL` units — one notch is 1.0.

## 4. Device messages, agent → client

On the same control connection, in the other direction:

| type | message | body |
|---|---|---|
| 0x01 | CLIPBOARD | UTF-8 text — the device clipboard changed |
| 0x02 | CLIPBOARD_ACK | `u64 sequence` — a CLIPBOARD control message with that sequence is applied |

The agent cuts clipboard text to fit the 1 MiB record, never splitting a UTF-8 sequence.
