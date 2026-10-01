#pragma once
#include <QMetaType>
#include <QPoint>
#include <QSize>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

// The mirror wire protocol, v3 — Remora's own (bd remora-c79i). docs/MIRROR_PROTOCOL.md is the
// normative contract; tests/test_mirrorproto.cpp pins every layout byte-for-byte. Pure: QByteArray
// in, QByteArray out — sockets, adb and decode live above this layer (bd remora-28ix.2).

namespace remora::mirror {

// Records ----------------------------------------------------------------------------------------
//
// Everything after a connection's setup is a record, in both directions and on every kind of
// connection: u32 size (the type byte plus the body), u8 type, body. The size is what lets a
// receiver skip a type it does not know, so a peer's additions never desynchronise the stream.

inline constexpr quint32 kControlRecordMax = 1u << 20;  // control connections, both directions
inline constexpr quint32 kStreamRecordMax = 32u << 20;  // media streams: a 4K key frame fits

struct Record {
    quint8 type = 0;
    QByteArray body;
};

QByteArray record(quint8 type, const QByteArray &body = {});

// Incremental: feed arbitrary chunks, take records until nullopt. A size of 0 or over the limit is
// a framing violation — error() latches and nothing further is returned, because a bad size leaves
// no way to find the next record.
class RecordReader {
public:
    explicit RecordReader(quint32 limit) : limit_(limit) {}
    void feed(const QByteArray &data) { buf_.append(data); }
    std::optional<Record> next();
    bool error() const { return !error_.isEmpty(); }
    QString errorString() const { return error_; }

private:
    QByteArray buf_;
    quint32 limit_;
    QString error_;
};

// Media streams ----------------------------------------------------------------------------------

enum class StreamRecord : quint8 { Start = 0x01, Format = 0x02, Config = 0x03, Frame = 0x04, End = 0x05 };

// Codec ids, the START body. Audio sets the top bit.
inline constexpr quint8 kCodecH264 = 0x01;
inline constexpr quint8 kCodecH265 = 0x02;
inline constexpr quint8 kCodecAv1 = 0x03;
inline constexpr quint8 kCodecOpus = 0x81;
inline constexpr quint8 kCodecAac = 0x82;
inline constexpr quint8 kCodecFlac = 0x83;
inline constexpr quint8 kCodecRawAudio = 0x84;  // PCM s16le, 48 kHz stereo

// END reasons.
inline constexpr quint8 kEndUnavailable = 0;  // carry on without this stream
inline constexpr quint8 kEndFailed = 1;       // stop the session

struct FrameFormat {
    quint32 width = 0, height = 0;
    bool clientResized = false;  // FORMAT flags bit 0
};

struct MediaPacket {
    std::optional<quint64> ptsUs;  // unset for CONFIG, which carries no timestamp
    bool config = false, keyFrame = false;
    QByteArray payload;
};

// Incremental demuxer for one video or audio connection, or the external video socket (the same
// stream, with no hello in front). Feed arbitrary chunks, then drain events until NeedMoreData.
class StreamDemuxer {
public:
    enum class Kind { Video, Audio };
    enum class Event {
        NeedMoreData,
        Started,      // codec() is now valid
        Format,       // format() updated — also mid-stream, on a resize or rotation
        Packet,       // takePacket()
        Unavailable,  // terminal: the agent has no such stream; carry on without it (detail())
        Failed,       // terminal: the stream failed and the session should stop (detail())
        Error,        // terminal: a framing violation, errorString()
    };

    explicit StreamDemuxer(Kind kind) : kind_(kind), records_(kStreamRecordMax) {}

    void feed(const QByteArray &data) { records_.feed(data); }
    Event next();

    quint8 codec() const { return codec_; }
    FrameFormat format() const { return format_; }
    MediaPacket takePacket() { return std::move(packet_); }
    QString detail() const { return detail_; }  // END's own words, for Unavailable and Failed
    QString errorString() const { return error_; }

private:
    Event fail(const QString &why);

    Kind kind_;
    RecordReader records_;
    bool started_ = false, formatSeen_ = false, done_ = false;
    quint8 codec_ = 0;
    FrameFormat format_;
    MediaPacket packet_;
    QString detail_, error_;
};

// The session-failure text for a video stream the agent ENDed (either reason: without its video a
// mirror has nothing to show), worded once for both transports.
QString videoEndMessage(const QString &detail);

// H264/H265 config packets (SPS/PPS) must be prepended to the next media packet before decoding,
// not submitted alone. AV1 (and audio) bypass the merger.
class PacketMerger {
public:
    // Returns the packet to hand to the decoder, or nullopt when the packet was a config packet
    // that is now held for merging.
    std::optional<MediaPacket> merge(MediaPacket packet);

private:
    QByteArray pendingConfig_;
};

// Control messages, client → agent ---------------------------------------------------------------

enum class ControlRecord : quint8 {
    Key = 0x01, Text = 0x02, Pointer = 0x03, Scroll = 0x04,
    Back = 0x05, Panel = 0x06, Clipboard = 0x07, Resize = 0x08,
};

enum class PointerTool : quint8 { Finger = 0, Mouse = 1 };
enum class Panel : quint8 { Collapse = 0, Notifications = 1, QuickSettings = 2 };

// A point as a FRACTION of the frame the client is showing — 0 the left/top edge, 1 the
// right/bottom. The agent multiplies by its display's live size, so neither side needs the other's
// resolution, and a max_size-capped video maps exactly like a full-size one.
struct FramePoint {
    float x = 0, y = 0;
};
// `videoPoint` in a `videoSize` frame, as a fraction of it. A degenerate frame maps to the origin.
FramePoint framePoint(QPoint videoPoint, QSize videoSize);

QByteArray keyMessage(quint8 action, qint32 keycode, quint32 repeat, qint32 metaState);
QByteArray textMessage(const QString &text);
QByteArray pointerMessage(quint8 action, PointerTool tool, quint32 pointerId, FramePoint at,
                          float pressure, qint32 actionButton, qint32 buttons);
QByteArray scrollMessage(FramePoint at, float hScroll, float vScroll, qint32 buttons);
QByteArray backMessage(quint8 action);
QByteArray panelMessage(Panel panel);
QByteArray clipboardMessage(quint64 sequence, bool paste, const QString &text);
QByteArray resizeMessage(quint16 width, quint16 height);

// Session launch ---------------------------------------------------------------------------------

// The tunnel port range; husk detection (Chain.cpp) knows a healthy client by its ESTAB pair on
// these ports.
inline constexpr quint16 kTunnelPortFirst = 27183, kTunnelPortLast = 27199;

// The agent session's parameters (agentSessionParams below): what a session asks the device half
// for. The name predates the in-image agent; the fields are the session options it reads.
struct ServerOptions {
    bool audio = false;  // opt-in (--audio); independent of videoCompose — see bd remora-e5x.18.10
    // Compose mode: the device composes but does not capture-encode (video=false
    // video_compose=true, no video socket at all) — the picture comes from the host encoder's
    // unix socket instead. A device half that does not honour it still takes video=false and
    // sends no video, so only pair this with an external video source.
    bool videoCompose = false;
    QString videoCodec;    // empty = the agent's default (h264)
    QString videoEncoder;  // empty = the agent picks
    std::optional<int> videoBitRate, maxSize;
    std::optional<double> maxFps;
    // Desktop mode: mirror a NEW virtual display instead of display 0. "WxH/dpi", "WxH", or
    // empty-but-set (agent-chosen size) — the sentinel "auto" maps to the empty form.
    std::optional<QString> newDisplay;
    // Launch this app on the session's display as part of session CREATION ("+pkg" force-stops
    // first, "?name" searches by label — interpreted device-side).
    QString startApp;
    QStringList extraParams;  // raw key=value passthrough (the agent ignores keys it does not
                              // know, so this is forward-compatible)
};

// The forward-tunnel sweep (bd remora-6f92): forwards leak on the killed-client paths, and they
// are WORSE — the adb server keeps listening on a leaked port, so tryAgentPort's bind test
// skips it forever and every leak permanently shrinks the tunnel-port pool. Because that same
// listener makes port-liveness useless for telling a live session from a leak, ownership is
// recorded out of band: each session holds a QLockFile for its forwarded port, and the sweep
// removes a candidate only when its lock is acquirable (the owning process is gone).
QStringList adbForwardListArgv(const QString &adb);
// Pure: ports of `serial`'s forward entries that a mirror session created (remora_agent targets
// only). Candidates only — liveness is the caller's lock-file check.
QList<quint16> mirrorForwardPorts(const QString &forwardList, const QString &serial);

// Session setup ----------------------------------------------------------------------------------
//
// The in-image agent (docs/MIRROR_AGENT.md, bd remora-28ix.3): a persistent listener on a fixed
// socket, so there is nothing to push or spawn. Every connection opens with a versioned hello and a
// role; after the session request (control) or the attach (video, audio), the connection carries
// records.

inline constexpr char kAgentSocketName[] = "remora_agent";  // fixed: a persistent agent needs no scid
inline constexpr char kAgentMagic[] = "RMRA";
// v3 is the record format above (bd remora-c79i). There is no negotiating down: an agent that
// answers with another version speaks another format, and the session is refused, naming what to
// update — agentVersionRefusal().
inline constexpr quint16 kAgentProtoVersion = 3;
inline constexpr int kAgentHelloSize = 8;  // magic + u16 version + u16 flags/capabilities

enum class SessionKind : quint8 { Mirror = 1, Compose = 2, NewDisplay = 3, ListApps = 4 };

// The connection ROLE, the byte right after the hello. One agent socket serves both kinds of
// connection, so the role has to be readable before anything role-specific — and without it
// "kind=mirror" and "attach video" are both a leading 0x01.
// Audio is a role, not a positional third socket, so a session without video never renumbers the
// connections after it (bd remora-28ix.3.6).
enum class ConnRole : quint8 { Control = 0, Video = 1, Audio = 2 };

// "RMRA" u16 version u16 flags(reserved=0) — sent first on every connection, both kinds.
QByteArray agentHello();

struct AgentHello {
    quint16 version = 0, capabilities = 0;
};
// Parses the agent's 8-byte reply; nullopt when the magic is wrong — something answered that is
// not an agent, which is how the probe tells "no agent" from "an agent that says no".
std::optional<AgentHello> parseAgentHello(const QByteArray &reply);
// Empty when this client speaks the agent's version; otherwise the refusal, which names the side
// to update. Starts with kAgentVersionRefusalPrefix so callers can pass it through whole instead of
// wrapping it in "no in-image agent", which it is not.
inline constexpr char kAgentVersionRefusalPrefix[] = "the image's mirror agent speaks protocol";
QString agentVersionRefusal(const AgentHello &hello);

// role, u8 kind, u16-prefixed UTF-8 "key=value\n" block. Length-prefixed, so values carry none of
// a command line's escaping limits.
QByteArray agentSessionRequest(SessionKind kind, const QStringList &params);
// role, u32 sessionId — binds a second connection to that session's video stream.
QByteArray agentVideoAttach(quint32 sessionId);
// The same, for the session's audio stream (START, then CONFIG/FRAME records — no FORMAT).
QByteArray agentAudioAttach(quint32 sessionId);
// The session's key=value option lines — the spellings the agent's Session.java parses.
QStringList agentSessionParams(const ServerOptions &opts);

struct SessionReply {
    quint8 status = 0;  // 0 = ok
    quint32 sessionId = 0;
    QString reason;  // status != 0
};
// Incremental: nullopt until the whole reply (including a failure's reason string) has arrived.
std::optional<SessionReply> parseSessionReply(const QByteArray &buf, int *consumed);

// The adb tunnel for the agent's fixed socket. FORWARD only: the agent is a persistent listener,
// so the client always dials in — and with nothing spawning behind adbd, a successful connect
// means the agent is really there, which the hello reply then proves.
QStringList adbAgentTunnelArgv(const QString &adb, const QString &serial, quint16 port,
                               bool remove);

// Device messages, agent → client ----------------------------------------------------------------

enum class DeviceRecord : quint8 { Clipboard = 0x01, ClipboardAck = 0x02 };

struct DeviceMessage {
    DeviceRecord type = DeviceRecord::Clipboard;
    QString clipboardText;  // Clipboard
    quint64 sequence = 0;   // ClipboardAck
};

// Incremental parser for the control connection's agent→client direction. Unknown record types
// are skipped; a framing violation latches error() and next() yields nothing further.
class DeviceMessageParser {
public:
    DeviceMessageParser() : records_(kControlRecordMax) {}
    void feed(const QByteArray &data) { records_.feed(data); }
    std::optional<DeviceMessage> next();
    bool error() const { return error_ || records_.error(); }

private:
    RecordReader records_;
    bool error_ = false;
};

}  // namespace remora::mirror

// Queued across the mirror's reader thread boundary (bd remora-10bt), which resolves parameter
// types by name at connect time — so both must be registered metatypes or the connection warns
// once and then silently delivers nothing. Outside the namespace: Q_DECLARE_METATYPE specialises
// a template in the global scope and does not compile inside one.
Q_DECLARE_METATYPE(remora::mirror::MediaPacket)
Q_DECLARE_METATYPE(remora::mirror::FrameFormat)
