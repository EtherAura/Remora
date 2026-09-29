#pragma once
#include <QMetaType>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

// The mirror wire protocol. Stream framing, control and device messages are v1's — byte-identical
// to scrcpy 4.0's stream/control protocol, which the in-image agent keeps unchanged; only the
// handshake is Remora's own (v2, below). docs/MIRROR_PROTOCOL.md is the normative contract;
// tests/test_mirrorproto.cpp pins every layout byte-for-byte. Pure: QByteArray in, QByteArray
// out — sockets, adb and decode live above this layer (bd remora-28ix.2).

namespace remora::mirror {

// u32 codec ids: the ASCII codec name, left-zero-padded. (The v1 version gate and the 64-byte
// device-name preamble went with the v1 path — bd remora-28ix.5; the agent hello carries both
// jobs.)
inline constexpr quint32 kCodecH264 = 0x68323634;
inline constexpr quint32 kCodecH265 = 0x68323635;
inline constexpr quint32 kCodecAv1 = 0x00617631;
inline constexpr quint32 kCodecOpus = 0x6f707573;
inline constexpr quint32 kCodecAac = 0x00616163;
inline constexpr quint32 kCodecFlac = 0x666c6163;
inline constexpr quint32 kCodecRawAudio = 0x00726177;
// Sentinels replacing the codec id.
inline constexpr quint32 kStreamDisabled = 0x00000000;   // continue without this stream
inline constexpr quint32 kStreamConfigError = 0x00000001;  // fatal, stop

// Stream demuxer ---------------------------------------------------------------------------------

inline constexpr int kPacketHeaderSize = 12;
inline constexpr quint64 kFlagConfig = Q_UINT64_C(1) << 62;
inline constexpr quint64 kFlagKeyFrame = Q_UINT64_C(1) << 61;
inline constexpr quint64 kPtsMask = kFlagKeyFrame - 1;  // PTS is a 61-bit field, microseconds

struct SessionInfo {
    quint32 width = 0, height = 0;
    bool clientResized = false;  // bit0 of the session flags
};

struct MediaPacket {
    std::optional<quint64> ptsUs;  // unset for config packets (their header discards the PTS)
    bool config = false, keyFrame = false;
    QByteArray payload;
};

// Incremental demuxer for one video or audio socket (or the external video socket — same format,
// the connection layer just doesn't read a device-name preamble there). Feed arbitrary chunks,
// then drain events until NeedMoreData.
class StreamDemuxer {
public:
    enum class Kind { Video, Audio };
    enum class Event {
        NeedMoreData,
        CodecId,         // codecId() is now valid
        Session,         // session() updated — can also arrive mid-stream (resize/rotation)
        Packet,          // takePacket()
        StreamDisabled,  // terminal: the server declined this stream
        ConfigError,     // terminal: server-side configuration failure
        Error,           // terminal: framing violation, errorString()
    };

    explicit StreamDemuxer(Kind kind) : kind_(kind) {}

    void feed(const QByteArray &data) { buf_.append(data); }
    Event next();

    quint32 codecId() const { return codecId_; }
    SessionInfo session() const { return session_; }
    MediaPacket takePacket() { return std::move(packet_); }
    QString errorString() const { return error_; }

private:
    enum class State { AwaitCodecId, AwaitFirstHeader, AwaitHeader, AwaitPayload, Done };
    Event fail(const QString &why);

    Kind kind_;
    State state_ = State::AwaitCodecId;
    QByteArray buf_;
    quint32 codecId_ = 0;
    SessionInfo session_;
    MediaPacket packet_;
    quint64 pendingHeader_ = 0;
    quint32 pendingSize_ = 0;
    QString error_;
};

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

// Control messages, client → server --------------------------------------------------------------

inline constexpr int kInjectTextMaxLength = 300;         // truncated at a UTF-8 boundary
inline constexpr int kSetClipboardMaxLength = 262130;    // (1 << 18) - 14
inline constexpr quint64 kPointerIdMouse = ~Q_UINT64_C(0);  // -1

// The shared 12-byte position: a point in the displayed video's coordinate space plus that
// space's size, so the server can rescale.
struct Position {
    qint32 x = 0, y = 0;
    quint16 screenWidth = 0, screenHeight = 0;
};

// Fixed-point conversions, exactly the reference client's (clamped, 1.0f → 0xFFFF).
quint16 pressureToU16(float pressure);   // [0,1]
qint16 scrollToI16(float value);         // wire i16 is [-1,1] of value/16, clamped

QByteArray injectKeycode(quint8 action, qint32 keycode, quint32 repeat, qint32 metaState);
QByteArray injectText(const QString &text);
QByteArray injectTouch(quint8 action, quint64 pointerId, Position pos, float pressure,
                       qint32 actionButton, qint32 buttons);
QByteArray injectScroll(Position pos, float hScroll, float vScroll, qint32 buttons);
QByteArray backOrScreenOn(quint8 action);
QByteArray expandNotificationPanel();
QByteArray expandSettingsPanel();
QByteArray collapsePanels();
QByteArray getClipboard(quint8 copyKey);  // 0 none, 1 copy, 2 cut
QByteArray setClipboard(quint64 sequence, bool paste, const QString &text);
QByteArray setDisplayPower(bool on);
QByteArray rotateDevice();
QByteArray uhidCreate(quint16 id, quint16 vendorId, quint16 productId, const QString &name,
                      const QByteArray &reportDesc);
QByteArray uhidInput(quint16 id, const QByteArray &data);
QByteArray uhidDestroy(quint16 id);
QByteArray openHardKeyboardSettings();
QByteArray resetVideo();
QByteArray resizeDisplay(quint16 width, quint16 height);

// Session launch ---------------------------------------------------------------------------------

// The reference client's tunnel port range; husk detection (Chain.cpp) knows a healthy client by
// its ESTAB pair on these ports, so the replacement keeps them. (kServerRemotePath and the
// scrcpy server class name went with the v1 path — bd remora-28ix.5.)
inline constexpr quint16 kTunnelPortFirst = 27183, kTunnelPortLast = 27199;

// The agent session's parameters (agentSessionParams below). The name predates the purge: there
// is no server process any more, but the fields still describe what a session asks the device
// half for, so the shape stayed when the v1 argv builders around it were deleted.
struct ServerOptions {
    bool audio = false;  // opt-in (--audio); independent of videoCompose — see bd remora-e5x.18.10
    // Compose mode: the device composes but does not capture-encode (video=false
    // video_compose=true, no video socket at all) — the picture comes from the host encoder's
    // unix socket instead. Fork param; a conforming stock server would warn-and-ignore it and
    // then send no video, so only pair this with an external video source.
    bool videoCompose = false;
    QString videoCodec;    // empty = server default (h264)
    QString videoEncoder;  // empty = server auto-select
    std::optional<int> videoBitRate, maxSize;
    std::optional<double> maxFps;
    // Desktop mode: mirror a NEW virtual display instead of display 0. "WxH/dpi", "WxH", or
    // empty-but-set (server-chosen size) — the sentinel "auto" maps to the empty form.
    std::optional<QString> newDisplay;
    // Launch this app on the session's display as part of session CREATION ("+pkg" force-stops
    // first, "?name" searches by label — interpreted device-side).
    QString startApp;
    QStringList extraParams;  // raw key=value passthrough (additive params are ignored by
                              // conforming servers, so this is forward-compatible)
};

// The forward-tunnel sweep (bd remora-6f92): forwards leak on the killed-client paths, and they
// are WORSE — the adb server keeps listening on a leaked port, so tryAgentPort's bind test
// skips it forever and every leak permanently shrinks the tunnel-port pool. Because that same
// listener makes port-liveness useless for telling a live session from a leak, ownership is
// recorded out of band: each session holds a QLockFile for its forwarded port, and the sweep
// removes a candidate only when its lock is acquirable (the owning process is gone).
QStringList adbForwardListArgv(const QString &adb);
// Pure: ports of `serial`'s forward entries that a mirror session created (remora_agent targets
// only — see MirrorProto.cpp for why scrcpy_* is no longer swept). Candidates only — liveness is
// the caller's lock-file check.
QList<quint16> mirrorForwardPorts(const QString &forwardList, const QString &serial);

// Protocol v2: the Remora agent ------------------------------------------------------------------
//
// The in-image agent (docs/MIRROR_AGENT.md, bd remora-28ix.3) replaces the pushed jar: no push,
// no app_process spawn, no exact-match version gate. What it does NOT change is everything
// below this line — control messages, device messages and the stream framing are v1's, byte for
// byte, because they were never the problem. Only the handshake is new.

inline constexpr char kAgentSocketName[] = "remora_agent";  // fixed: a persistent agent needs no scid
inline constexpr char kAgentMagic[] = "RMRA";
inline constexpr quint16 kAgentProtoVersion = 2;
inline constexpr int kAgentHelloSize = 8;  // magic + u16 version + u16 flags/capabilities

enum class SessionKind : quint8 { Mirror = 1, Compose = 2, NewDisplay = 3, ListApps = 4 };

// The connection ROLE, the byte right after the hello. One agent socket serves both kinds of
// connection, so the role has to be readable before anything role-specific — and without it
// "kind=mirror" and "attach video" are both a leading 0x01.
// Audio is role 2, not a positional third socket: v1 made audio an ORDERING convention (video,
// then audio, then control), so a session without video silently renumbered everything after it.
// A role byte is what removes that (bd remora-28ix.3.6).
enum class ConnRole : quint8 { Control = 0, Video = 1, Audio = 2 };

// "RMRA" u16 version u16 flags(reserved=0) — sent first on every connection, both kinds.
QByteArray agentHello();

struct AgentHello {
    quint16 version = 0, capabilities = 0;
};
// Parses the agent's 8-byte reply; nullopt when the magic is wrong (not an agent, or a v1 server
// whose first bytes are the device-name preamble — which is exactly how the probe tells them apart).
std::optional<AgentHello> parseAgentHello(const QByteArray &reply);

// role, u8 kind, u16-prefixed UTF-8 "key=value\n" block. Length-prefixed, so the shell-escaping
// limits of the v1 argv path do not apply — but the option NAMES are v1's, unchanged.
QByteArray agentSessionRequest(SessionKind kind, const QStringList &params);
// role, u32 sessionId — binds a second connection to that session's video. The v1 stream framing
// follows on it, unchanged, so StreamDemuxer reads it as-is.
QByteArray agentVideoAttach(quint32 sessionId);
// The same, for the session's audio: codec id then packets, no session packet, which is exactly
// what StreamDemuxer::Kind::Audio already reads.
QByteArray agentAudioAttach(quint32 sessionId);
// The session's key=value parameter lines (v1's vocabulary, kept byte-compatible on purpose —
// the agent's Session.java parses the same spellings the jar did).
QStringList agentSessionParams(const ServerOptions &opts);

struct SessionReply {
    quint8 status = 0;  // 0 = ok
    quint32 sessionId = 0;
    QString reason;  // status != 0
};
// Incremental: nullopt until the whole reply (including a failure's reason string) has arrived.
std::optional<SessionReply> parseSessionReply(const QByteArray &buf, int *consumed);

// The adb tunnel for the agent's fixed socket. FORWARD only, where v1 preferred reverse: the
// agent is a persistent listener, so the client always dials in. That also removes v1's dummy
// byte — with nothing spawning behind adbd, a successful connect means the agent is really there,
// and the hello reply proves it.
QStringList adbAgentTunnelArgv(const QString &adb, const QString &serial, quint16 port,
                               bool remove);

// Device messages, server → client ---------------------------------------------------------------

struct DeviceMessage {
    enum class Type : quint8 { Clipboard = 0, AckClipboard = 1, UhidOutput = 2 };
    Type type = Type::Clipboard;
    QString clipboardText;  // Clipboard
    quint64 sequence = 0;   // AckClipboard
    quint16 uhidId = 0;     // UhidOutput
    QByteArray uhidData;    // UhidOutput
};

// Incremental parser for the control socket's device→client direction. An unknown type is
// unrecoverable by contract — error() latches and next() yields nothing further.
class DeviceMessageParser {
public:
    void feed(const QByteArray &data) { buf_.append(data); }
    std::optional<DeviceMessage> next();
    bool error() const { return error_; }

private:
    QByteArray buf_;
    bool error_ = false;
};

}  // namespace remora::mirror

// Queued across the mirror's reader thread boundary (bd remora-10bt), which resolves parameter
// types by name at connect time — so both must be registered metatypes or the connection warns
// once and then silently delivers nothing. Outside the namespace: Q_DECLARE_METATYPE specialises
// a template in the global scope and does not compile inside one.
Q_DECLARE_METATYPE(remora::mirror::MediaPacket)
Q_DECLARE_METATYPE(remora::mirror::SessionInfo)
