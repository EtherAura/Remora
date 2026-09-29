#include "MirrorProto.h"

#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

namespace remora::mirror {

// Big-endian appenders — every integer on the wire is BE, no alignment, no padding.
static void putU8(QByteArray &b, quint8 v) { b.append(char(v)); }
static void putU16(QByteArray &b, quint16 v) {
    char raw[2];
    qToBigEndian(v, raw);
    b.append(raw, 2);
}
static void putU32(QByteArray &b, quint32 v) {
    char raw[4];
    qToBigEndian(v, raw);
    b.append(raw, 4);
}
static void putU64(QByteArray &b, quint64 v) {
    char raw[8];
    qToBigEndian(v, raw);
    b.append(raw, 8);
}
static void putPosition(QByteArray &b, Position p) {
    putU32(b, quint32(p.x));
    putU32(b, quint32(p.y));
    putU16(b, p.screenWidth);
    putU16(b, p.screenHeight);
}

// UTF-8 bytes cut to at most `limit`, never splitting a codepoint: back off over continuation
// bytes so a partial sequence is dropped whole.
static QByteArray utf8Truncated(const QString &text, int limit) {
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= limit) return utf8;
    int cut = limit;
    while (cut > 0 && (uchar(utf8.at(cut)) & 0xC0) == 0x80) --cut;
    utf8.truncate(cut);
    return utf8;
}

// Stream demuxer ---------------------------------------------------------------------------------

StreamDemuxer::Event StreamDemuxer::fail(const QString &why) {
    error_ = why;
    state_ = State::Done;
    return Event::Error;
}

StreamDemuxer::Event StreamDemuxer::next() {
    if (state_ == State::Done) return error_.isEmpty() ? Event::NeedMoreData : Event::Error;

    if (state_ == State::AwaitCodecId) {
        if (buf_.size() < 4) return Event::NeedMoreData;
        codecId_ = qFromBigEndian<quint32>(buf_.constData());
        buf_.remove(0, 4);
        if (codecId_ == kStreamDisabled) {
            state_ = State::Done;
            return Event::StreamDisabled;
        }
        if (codecId_ == kStreamConfigError) {
            state_ = State::Done;
            return Event::ConfigError;
        }
        // A video stream's first packet must be the session header; audio has none (the format
        // is fixed 48 kHz stereo) and goes straight to media packets.
        state_ = kind_ == Kind::Video ? State::AwaitFirstHeader : State::AwaitHeader;
        return Event::CodecId;
    }

    if (state_ == State::AwaitFirstHeader || state_ == State::AwaitHeader) {
        if (buf_.size() < kPacketHeaderSize) return Event::NeedMoreData;
        const uchar *h = reinterpret_cast<const uchar *>(buf_.constData());
        const bool isSession = h[0] & 0x80;
        if (state_ == State::AwaitFirstHeader && !isSession)
            return fail(QStringLiteral("expected a session header as the first video packet"));
        if (isSession) {
            session_.clientResized = h[3] & 1;
            session_.width = qFromBigEndian<quint32>(h + 4);
            session_.height = qFromBigEndian<quint32>(h + 8);
            buf_.remove(0, kPacketHeaderSize);
            state_ = State::AwaitHeader;
            return Event::Session;
        }
        pendingHeader_ = qFromBigEndian<quint64>(h);
        pendingSize_ = qFromBigEndian<quint32>(h + 8);
        if (pendingSize_ == 0) return fail(QStringLiteral("invalid packet length: 0"));
        buf_.remove(0, kPacketHeaderSize);
        state_ = State::AwaitPayload;
        // fall through to the payload check below
    }

    if (state_ == State::AwaitPayload) {
        if (quint32(buf_.size()) < pendingSize_) return Event::NeedMoreData;
        packet_.config = pendingHeader_ & kFlagConfig;
        packet_.keyFrame = pendingHeader_ & kFlagKeyFrame;
        // A config packet's header discards the PTS entirely (it is exactly kFlagConfig).
        packet_.ptsUs = packet_.config ? std::nullopt
                                       : std::optional<quint64>(pendingHeader_ & kPtsMask);
        packet_.payload = buf_.left(pendingSize_);
        buf_.remove(0, pendingSize_);
        state_ = State::AwaitHeader;
        return Event::Packet;
    }

    return Event::NeedMoreData;
}

std::optional<MediaPacket> PacketMerger::merge(MediaPacket packet) {
    if (packet.config) {
        pendingConfig_ = std::move(packet.payload);
        return std::nullopt;
    }
    if (!pendingConfig_.isEmpty()) {
        packet.payload.prepend(pendingConfig_);
        pendingConfig_.clear();
    }
    return packet;
}

// Control messages, client → server --------------------------------------------------------------

quint16 pressureToU16(float pressure) {
    if (pressure <= 0.0f) return 0;
    const quint32 u = quint32(pressure * 65536.0f);  // 2^16; 1.0f lands on 65536, clamped below
    return u >= 0xFFFF ? 0xFFFF : quint16(u);
}

qint16 scrollToI16(float value) {
    // The wire carries value/16 as [-1,1] fixed-point; the server multiplies by 16 back.
    float f = value / 16.0f;
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    const qint32 i = qint32(f * 32768.0f);  // 2^15
    if (i >= 0x7FFF) return 0x7FFF;
    if (i < -0x8000) return -0x8000;
    return qint16(i);
}

QByteArray injectKeycode(quint8 action, qint32 keycode, quint32 repeat, qint32 metaState) {
    QByteArray b;
    putU8(b, 0);
    putU8(b, action);
    putU32(b, quint32(keycode));
    putU32(b, repeat);
    putU32(b, quint32(metaState));
    return b;
}

QByteArray injectText(const QString &text) {
    const QByteArray utf8 = utf8Truncated(text, kInjectTextMaxLength);
    QByteArray b;
    putU8(b, 1);
    putU32(b, quint32(utf8.size()));
    b.append(utf8);
    return b;
}

QByteArray injectTouch(quint8 action, quint64 pointerId, Position pos, float pressure,
                       qint32 actionButton, qint32 buttons) {
    QByteArray b;
    putU8(b, 2);
    putU8(b, action);
    putU64(b, pointerId);
    putPosition(b, pos);
    putU16(b, pressureToU16(pressure));
    putU32(b, quint32(actionButton));
    putU32(b, quint32(buttons));
    return b;
}

QByteArray injectScroll(Position pos, float hScroll, float vScroll, qint32 buttons) {
    QByteArray b;
    putU8(b, 3);
    putPosition(b, pos);
    putU16(b, quint16(scrollToI16(hScroll)));
    putU16(b, quint16(scrollToI16(vScroll)));
    putU32(b, quint32(buttons));
    return b;
}

QByteArray backOrScreenOn(quint8 action) {
    QByteArray b;
    putU8(b, 4);
    putU8(b, action);
    return b;
}

QByteArray expandNotificationPanel() { return QByteArray(1, char(5)); }
QByteArray expandSettingsPanel() { return QByteArray(1, char(6)); }
QByteArray collapsePanels() { return QByteArray(1, char(7)); }

QByteArray getClipboard(quint8 copyKey) {
    QByteArray b;
    putU8(b, 8);
    putU8(b, copyKey);
    return b;
}

QByteArray setClipboard(quint64 sequence, bool paste, const QString &text) {
    const QByteArray utf8 = utf8Truncated(text, kSetClipboardMaxLength);
    QByteArray b;
    putU8(b, 9);
    putU64(b, sequence);
    putU8(b, paste ? 1 : 0);
    putU32(b, quint32(utf8.size()));
    b.append(utf8);
    return b;
}

QByteArray setDisplayPower(bool on) {
    QByteArray b;
    putU8(b, 10);
    putU8(b, on ? 1 : 0);
    return b;
}

QByteArray rotateDevice() { return QByteArray(1, char(11)); }

QByteArray uhidCreate(quint16 id, quint16 vendorId, quint16 productId, const QString &name,
                      const QByteArray &reportDesc) {
    const QByteArray nameUtf8 = utf8Truncated(name, 127);  // 1-byte-length string
    QByteArray b;
    putU8(b, 12);
    putU16(b, id);
    putU16(b, vendorId);
    putU16(b, productId);
    putU8(b, quint8(nameUtf8.size()));
    b.append(nameUtf8);
    putU16(b, quint16(reportDesc.size()));
    b.append(reportDesc);
    return b;
}

QByteArray uhidInput(quint16 id, const QByteArray &data) {
    QByteArray b;
    putU8(b, 13);
    putU16(b, id);
    putU16(b, quint16(data.size()));
    b.append(data);
    return b;
}

QByteArray uhidDestroy(quint16 id) {
    QByteArray b;
    putU8(b, 14);
    putU16(b, id);
    return b;
}

QByteArray openHardKeyboardSettings() { return QByteArray(1, char(15)); }

QByteArray resetVideo() { return QByteArray(1, char(17)); }

QByteArray resizeDisplay(quint16 width, quint16 height) {
    QByteArray b;
    putU8(b, 21);
    putU16(b, width);
    putU16(b, height);
    return b;
}

// Tunnels ----------------------------------------------------------------------------------------

QStringList adbForwardListArgv(const QString &adb) {
    // Global on purpose: `forward --list` reports every device's rows with a serial column, and
    // the parser filters — a -s here would not (adb prints them all regardless).
    return {adb, QStringLiteral("forward"), QStringLiteral("--list")};
}

QList<quint16> mirrorForwardPorts(const QString &forwardList, const QString &serial) {
    // Rows look like '<serial> tcp:<port> localabstract:<name>'. Only this serial's rows, and only
    // the socket a mirror session creates — the agent's — is a candidate; whether one is actually
    // stale is the caller's question (the lock file answers it).
    // THE scrcpy_<hex> TUNNEL ALTERNATIVE IS GONE (bd remora-28ix.4 step 4). It matched
    // the jar tunnel a pre-cutover session opened, so such a forward could still be counted and
    // cleaned. Nothing can create one now: the device half is the in-image agent, and an image old
    // enough to lack the bake is REFUSED outright rather than falling back (see ADR-0001 / the
    // mirror note in CLAUDE.md), so there is no path that opens a scrcpy tunnel. A stale forward
    // left by such a session is now ignored rather than reaped — `adb forward --remove-all` clears
    // it, and it costs a port entry until then.
    QList<quint16> out;
    static const QRegularExpression rx(QStringLiteral(
        "^(\\S+)\\s+tcp:(\\d+)\\s+localabstract:(remora_agent)\\s*$"));
    for (const QString &line : forwardList.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto m = rx.match(line.trimmed());
        if (!m.hasMatch() || m.captured(1) != serial) continue;
        const uint port = m.captured(2).toUInt();
        if (port > 0 && port <= 65535 && !out.contains(quint16(port))) out << quint16(port);
    }
    return out;
}

// Protocol v2: the Remora agent ------------------------------------------------------------------

QByteArray agentHello() {
    QByteArray b(kAgentMagic, 4);
    putU16(b, kAgentProtoVersion);
    putU16(b, 0);  // client flags — reserved
    return b;
}

std::optional<AgentHello> parseAgentHello(const QByteArray &reply) {
    if (reply.size() < kAgentHelloSize) return std::nullopt;
    if (reply.left(4) != QByteArray(kAgentMagic, 4)) return std::nullopt;
    const uchar *d = reinterpret_cast<const uchar *>(reply.constData());
    AgentHello h;
    h.version = qFromBigEndian<quint16>(d + 4);
    h.capabilities = qFromBigEndian<quint16>(d + 6);
    return h;
}

QByteArray agentSessionRequest(SessionKind kind, const QStringList &params) {
    QByteArray opts;
    for (const QString &p : params) opts.append(p.toUtf8()).append('\n');
    QByteArray b;
    putU8(b, quint8(ConnRole::Control));
    putU8(b, quint8(kind));
    putU16(b, quint16(opts.size()));
    b.append(opts);
    return b;
}

QByteArray agentVideoAttach(quint32 sessionId) {
    QByteArray b;
    putU8(b, quint8(ConnRole::Video));
    putU32(b, sessionId);
    return b;
}

QByteArray agentAudioAttach(quint32 sessionId) {
    QByteArray b;
    putU8(b, quint8(ConnRole::Audio));
    putU32(b, sessionId);
    return b;
}

QStringList agentSessionParams(const ServerOptions &opts) {
    QStringList a;
    if (opts.videoCompose) a << QStringLiteral("video=false") << QStringLiteral("video_compose=true");
    if (!opts.audio) a << QStringLiteral("audio=false");
    if (!opts.videoCodec.isEmpty()) a << QStringLiteral("video_codec=") + opts.videoCodec;
    if (!opts.videoEncoder.isEmpty()) a << QStringLiteral("video_encoder=") + opts.videoEncoder;
    if (opts.videoBitRate) a << QStringLiteral("video_bit_rate=%1").arg(*opts.videoBitRate);
    if (opts.maxSize) a << QStringLiteral("max_size=%1").arg(*opts.maxSize);
    if (opts.maxFps) a << QStringLiteral("max_fps=%1").arg(*opts.maxFps);
    if (opts.newDisplay) {
        const bool autoSize = opts.newDisplay->isEmpty() || *opts.newDisplay == QLatin1String("auto");
        a << (autoSize ? QStringLiteral("new_display=")
                       : QStringLiteral("new_display=") + *opts.newDisplay);
    }
    if (!opts.startApp.isEmpty()) a << QStringLiteral("start_app=") + opts.startApp;
    a << opts.extraParams;
    return a;
}

std::optional<SessionReply> parseSessionReply(const QByteArray &buf, int *consumed) {
    if (buf.size() < 5) return std::nullopt;
    const uchar *d = reinterpret_cast<const uchar *>(buf.constData());
    SessionReply r;
    r.status = d[0];
    r.sessionId = qFromBigEndian<quint32>(d + 1);
    if (r.status == 0) {
        if (consumed) *consumed = 5;
        return r;
    }
    if (buf.size() < 7) return std::nullopt;
    const quint16 len = qFromBigEndian<quint16>(d + 5);
    if (buf.size() < 7 + int(len)) return std::nullopt;
    r.reason = QString::fromUtf8(buf.constData() + 7, len);
    if (consumed) *consumed = 7 + int(len);
    return r;
}

QStringList adbAgentTunnelArgv(const QString &adb, const QString &serial, quint16 port,
                               bool remove) {
    const QString local = QStringLiteral("tcp:%1").arg(port);
    QStringList a{adb, QStringLiteral("-s"), serial, QStringLiteral("forward")};
    if (remove) return a << QStringLiteral("--remove") << local;
    return a << local
             << QStringLiteral("localabstract:") + QString::fromLatin1(kAgentSocketName);
}

// Device messages, server → client ---------------------------------------------------------------

std::optional<DeviceMessage> DeviceMessageParser::next() {
    if (error_ || buf_.isEmpty()) return std::nullopt;
    const uchar *d = reinterpret_cast<const uchar *>(buf_.constData());
    const quint8 type = d[0];

    if (type == quint8(DeviceMessage::Type::Clipboard)) {
        if (buf_.size() < 5) return std::nullopt;
        const quint32 len = qFromBigEndian<quint32>(d + 1);
        if (quint32(buf_.size()) < 5 + len) return std::nullopt;
        DeviceMessage m;
        m.type = DeviceMessage::Type::Clipboard;
        m.clipboardText = QString::fromUtf8(buf_.constData() + 5, len);
        buf_.remove(0, 5 + int(len));
        return m;
    }
    if (type == quint8(DeviceMessage::Type::AckClipboard)) {
        if (buf_.size() < 9) return std::nullopt;
        DeviceMessage m;
        m.type = DeviceMessage::Type::AckClipboard;
        m.sequence = qFromBigEndian<quint64>(d + 1);
        buf_.remove(0, 9);
        return m;
    }
    if (type == quint8(DeviceMessage::Type::UhidOutput)) {
        if (buf_.size() < 5) return std::nullopt;
        const quint16 size = qFromBigEndian<quint16>(d + 3);
        if (buf_.size() < 5 + int(size)) return std::nullopt;
        DeviceMessage m;
        m.type = DeviceMessage::Type::UhidOutput;
        m.uhidId = qFromBigEndian<quint16>(d + 1);
        m.uhidData = buf_.mid(5, size);
        buf_.remove(0, 5 + int(size));
        return m;
    }

    // Unknown type: unrecoverable by contract — no length prefix means no resync point.
    error_ = true;
    return std::nullopt;
}

}  // namespace remora::mirror
