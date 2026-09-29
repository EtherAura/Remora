# Mirror wire protocol — byte layouts

The byte-level contract between the Remora mirror client (`remora mirror`) and the in-image Remora
agent: the media stream framing, the control messages, the device messages, and the host-side
external video socket. How a connection is opened and a session set up — the `RMRA` hello,
connection roles, session kinds — is in [MIRROR_AGENT.md](MIRROR_AGENT.md).

**Attribution.** These layouts are those of the scrcpy 4.0 stream and control protocol (Genymobile,
Apache-2.0), reused byte for byte because they were sound. Remora's client and agent are separate
implementations, and the session setup around them is Remora's own; nothing here talks to a scrcpy
server any more.

`src/core/MirrorProto.{h,cpp}` implements the client side of this document and
`tests/test_mirrorproto.cpp` pins it byte for byte; the agent's side is
`agent/src/com/remora/agent/ControlReader.java` and `VideoStreamer.java`. A change to any of them is
a change to this contract.

All integers are **big-endian**, with no alignment or padding. No message carries its own length
prefix; framing is positional.

## 1. Stream framing (video and audio connections)

After the connection's hello, role and session id (MIRROR_AGENT.md), the agent sends:

```
[4B codec id]  [12B session packet — video only]  [12B header][payload] ...
```

- **Codec ids** (u32, ASCII left-zero-padded): `h264`=0x68323634, `h265`=0x68323635,
  `av1`=0x00617631, `opus`=0x6f707573, `aac`=0x00616163, `flac`=0x666c6163, `raw`=0x00726177. The
  agent produces h264, h265 or av1 video and opus audio.
- **Sentinels** in place of the codec id: `0x00000000` = stream disabled (carry on without it),
  `0x00000001` = configuration error (stop).
- **12-byte headers**, told apart by the most significant bit of byte 0:
  - **Session packet** (MSB set): `u32 flags` (bit 31 = session marker, bit 0 = this size came from
    a client resize), `u32 width`, `u32 height`. No payload. Mandatory as the video stream's first
    packet, and repeated **mid-stream** on resize or rotation. Audio streams never carry one; their
    format is fixed at 48 kHz stereo.
  - **Media packet** (MSB clear): `u64 ptsAndFlags` — bit 62 CONFIG, bit 61 KEY_FRAME, bits 60..0
    the PTS in microseconds — then `u32 payloadSize` (0 is invalid), then the encoder's output. A
    CONFIG packet's header is exactly `0x4000000000000000`: its PTS is discarded, so treat it as
    unset.
- For **H.264 and H.265 only**, a CONFIG payload (SPS/PPS) is *prepended to the next media packet*
  before decoding rather than submitted alone. AV1 and audio config packets pass through as they
  are.

### 1.1 The external video socket (compose and host encode)

When the mirror is encoded on the host (`host_encode`), the agent runs a **compose** session — the
display is composed but not encoded, and there is no video connection — and the picture comes from
`remora-frame-encoder` over a host unix socket instead. The format is the same as above:
`[4B codec id][12B session packet][12B header][payload]...`.

- The encoder listens; the client connects, retrying up to 40 times 50 ms apart within a 12-second
  deadline.
- PTS is wall-clock microseconds since the first frame.
- The encoder re-sends the session packet on every size change, and re-encodes the newest frame
  every 300 ms as a keepalive on a still screen.
- The client arms a first-frame deadline (45 s by default) and fails the session if no frame
  arrives within it.

## 2. Control messages, client → agent

Sent on the session's control connection: `u8 type`, then a fixed layout. Maximum message size is
256 KiB. The last column says what the current agent does with each; every type is decoded in full
so the stream stays in sync.

| id | message | layout after the type byte | agent |
|---|---|---|---|
| 0 | INJECT_KEYCODE | `u8 action, i32 keycode, u32 repeat, i32 metastate` (14 bytes total) | acts |
| 1 | INJECT_TEXT | `u32 len, UTF-8 bytes` (len ≤ 300, truncated at a UTF-8 boundary) | acts |
| 2 | INJECT_TOUCH_EVENT | `u8 action, u64 pointerId, 12B position, u16 pressure, i32 actionButton, i32 buttons` (32 total) | acts |
| 3 | INJECT_SCROLL_EVENT | `12B position, i16 hscroll, i16 vscroll, i32 buttons` (21 total) | acts |
| 4 | BACK_OR_SCREEN_ON | `u8 action` | acts |
| 5 / 6 / 7 | EXPAND_NOTIFICATION_PANEL / EXPAND_SETTINGS_PANEL / COLLAPSE_PANELS | — | acts |
| 8 | GET_CLIPBOARD | `u8 copyKey` (0 none, 1 copy, 2 cut) | acts |
| 9 | SET_CLIPBOARD | `u64 sequence, u8 paste, u32 len, UTF-8 bytes` (len ≤ 262130; sequence 0 = no ack) | acts |
| 10 | SET_DISPLAY_POWER | `u8 on` | decoded only |
| 11 | ROTATE_DEVICE | — | decoded only |
| 12 | UHID_CREATE | `u16 id, u16 vendorId, u16 productId, u8 nameLen, name, u16 descLen, reportDesc` (nameLen ≤ 127) | decoded only |
| 13 | UHID_INPUT | `u16 id, u16 size, data` | decoded only |
| 14 | UHID_DESTROY | `u16 id` | decoded only |
| 15 | OPEN_HARD_KEYBOARD_SETTINGS | — | decoded only |
| 16 | START_APP | `u8 nameLen, name` (`+` prefix = force-stop first, `?` = search by label) | decoded only — use the `start_app` session option |
| 17 | RESET_VIDEO | — | decoded only |
| 18 | CAMERA_SET_TORCH | `u8 on` | decoded only |
| 19 / 20 | CAMERA_ZOOM_IN / CAMERA_ZOOM_OUT | — | decoded only |
| 21 | RESIZE_DISPLAY | `u16 width, u16 height` | acts (reflows a new-display session; a mirror ignores it) |

- **Position** (12 bytes): `i32 x, i32 y, u16 screenWidth, u16 screenHeight` — the point in the
  coordinate space of the video the client displays, plus that space's size. The agent scales it to
  the display (MIRROR_AGENT.md, *Coordinates*).
- **Pressure**: u16 fixed-point over [0,1] (`f × 2^16`, clamped so 1.0 → 0xFFFF).
- **Scroll**: the wire i16 is fixed-point over [-1,1] of `value / 16`, clamped; the receiver
  multiplies by 16 after decoding.
- Well-known pointer ids: mouse = −1, finger = −2, virtual finger = −3 (as u64).
- UHID_CREATE and UHID_DESTROY must never be dropped when a send queue is full.

## 3. Device messages, agent → client

On the same control connection, in the other direction:

| id | message | layout after the type byte |
|---|---|---|
| 0 | CLIPBOARD | `u32 len, UTF-8 bytes` |
| 1 | ACK_CLIPBOARD | `u64 sequence` |
| 2 | UHID_OUTPUT | `u16 id, u16 size, data` |

The current agent sends only 0 and 1; the client parses all three. An unknown type is
unrecoverable — with no length prefix there is no point to resynchronise at, so stop reading the
connection. The agent flushes after every message, so incremental reads are safe.
