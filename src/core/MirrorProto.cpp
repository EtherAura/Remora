#include "MirrorProto.h"

#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <cstring>

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
static void putF32(QByteArray &b, float v) {
    quint32 bits;
    static_assert(sizeof bits == sizeof v);
    std::memcpy(&bits, &v, sizeof bits);
    putU32(b, bits);
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

// Records ----------------------------------------------------------------------------------------

QByteArray record(quint8 type, const QByteArray &body) {
    QByteArray b;
    b.reserve(5 + body.size());
    putU32(b, quint32(1 + body.size()));
    putU8(b, type);
    b.append(body);
    return b;
}

std::optional<Record> RecordReader::next() {
    if (!error_.isEmpty() || buf_.size() < 4) return std::nullopt;
    const quint32 size = qFromBigEndian<quint32>(buf_.constData());
    if (size == 0) {
        error_ = QStringLiteral("record of size 0");
        return std::nullopt;
    }
    if (size > limit_) {
        error_ = QStringLiteral("record of %1 bytes is over the %2-byte limit").arg(size).arg(limit_);
        return std::nullopt;
    }
    if (quint64(buf_.size()) < 4 + quint64(size)) return std::nullopt;
    Record r;
    r.type = quint8(buf_.at(4));
    r.body = buf_.mid(5, int(size) - 1);
    buf_.remove(0, 4 + int(size));
    return r;
}

// Media streams ----------------------------------------------------------------------------------

StreamDemuxer::Event StreamDemuxer::fail(const QString &why) {
    error_ = why;
    done_ = true;
    return Event::Error;
}

StreamDemuxer::Event StreamDemuxer::next() {
    if (done_) return error_.isEmpty() ? Event::NeedMoreData : Event::Error;
    for (;;) {
        const std::optional<Record> r = records_.next();
        if (!r) return records_.error() ? fail(records_.errorString()) : Event::NeedMoreData;
        const uchar *d = reinterpret_cast<const uchar *>(r->body.constData());
        const int n = r->body.size();

        if (r->type == quint8(StreamRecord::End)) {
            done_ = true;
            if (n < 1) return fail(QStringLiteral("END without a reason"));
            detail_ = QString::fromUtf8(r->body.constData() + 1, n - 1);
            return d[0] == kEndUnavailable ? Event::Unavailable : Event::Failed;
        }
        if (!started_) {
            if (r->type != quint8(StreamRecord::Start))
                return fail(QStringLiteral("record type %1 before START").arg(r->type));
            if (n < 1) return fail(QStringLiteral("START without a codec"));
            codec_ = d[0];
            started_ = true;
            return Event::Started;
        }
        switch (StreamRecord(r->type)) {
            case StreamRecord::Start:
                return fail(QStringLiteral("a second START"));
            case StreamRecord::Format:
                if (n < 9) return fail(QStringLiteral("FORMAT body of %1 bytes").arg(n));
                format_.width = qFromBigEndian<quint32>(d);
                format_.height = qFromBigEndian<quint32>(d + 4);
                format_.clientResized = d[8] & 1;
                formatSeen_ = true;
                return Event::Format;
            case StreamRecord::Config:
            case StreamRecord::Frame: {
                // A video frame means nothing without its size, so FORMAT must have come first;
                // audio's format is fixed and it never sends one.
                if (kind_ == Kind::Video && !formatSeen_)
                    return fail(QStringLiteral("media before the first FORMAT"));
                MediaPacket p;
                if (r->type == quint8(StreamRecord::Config)) {
                    p.config = true;
                    p.payload = r->body;
                } else {
                    if (n < 10) return fail(QStringLiteral("FRAME body of %1 bytes").arg(n));
                    p.ptsUs = qFromBigEndian<quint64>(d);
                    p.keyFrame = d[8] & 1;
                    p.payload = r->body.mid(9);
                }
                packet_ = std::move(p);
                return Event::Packet;
            }
            case StreamRecord::End:
                break;  // handled above
        }
        // Unknown type: skipped — the size already said where the next record starts.
    }
}

QString videoEndMessage(const QString &detail) {
    return QStringLiteral("the agent ended the video stream: %1")
        .arg(detail.isEmpty() ? QStringLiteral("no reason given") : detail);
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

// Control messages, client → agent ---------------------------------------------------------------

FramePoint framePoint(QPoint videoPoint, QSize videoSize) {
    if (videoSize.width() <= 0 || videoSize.height() <= 0) return {};
    return {float(videoPoint.x()) / float(videoSize.width()),
            float(videoPoint.y()) / float(videoSize.height())};
}

static QByteArray control(ControlRecord type, const QByteArray &body = {}) {
    return record(quint8(type), body);
}

QByteArray keyMessage(quint8 action, qint32 keycode, quint32 repeat, qint32 metaState) {
    QByteArray b;
    putU8(b, action);
    putU32(b, quint32(keycode));
    putU32(b, repeat);
    putU32(b, quint32(metaState));
    return control(ControlRecord::Key, b);
}

QByteArray textMessage(const QString &text) {
    return control(ControlRecord::Text, utf8Truncated(text, int(kControlRecordMax) - 1));
}

QByteArray pointerMessage(quint8 action, PointerTool tool, quint32 pointerId, FramePoint at,
                          float pressure, qint32 actionButton, qint32 buttons) {
    QByteArray b;
    putU8(b, action);
    putU8(b, quint8(tool));
    putU32(b, pointerId);
    putF32(b, at.x);
    putF32(b, at.y);
    putF32(b, pressure);
    putU32(b, quint32(actionButton));
    putU32(b, quint32(buttons));
    return control(ControlRecord::Pointer, b);
}

QByteArray scrollMessage(FramePoint at, float hScroll, float vScroll, qint32 buttons) {
    QByteArray b;
    putF32(b, at.x);
    putF32(b, at.y);
    putF32(b, hScroll);
    putF32(b, vScroll);
    putU32(b, quint32(buttons));
    return control(ControlRecord::Scroll, b);
}

QByteArray backMessage(quint8 action) { return control(ControlRecord::Back, QByteArray(1, char(action))); }

QByteArray panelMessage(Panel panel) {
    return control(ControlRecord::Panel, QByteArray(1, char(quint8(panel))));
}

QByteArray clipboardMessage(quint64 sequence, bool paste, const QString &text) {
    QByteArray b;
    putU64(b, sequence);
    putU8(b, paste ? 1 : 0);
    // The record's type byte and these nine bytes come out of the same 1 MiB.
    b.append(utf8Truncated(text, int(kControlRecordMax) - 1 - 9));
    return control(ControlRecord::Clipboard, b);
}

QByteArray resizeMessage(quint16 width, quint16 height, quint16 dpi) {
    QByteArray b;
    putU16(b, width);
    putU16(b, height);
    if (dpi) putU16(b, dpi);  // the four-byte form is what an agent before the density had
    return control(ControlRecord::Resize, b);
}

QByteArray windowMessage(quint8 width, quint8 height, bool desktop) {
    QByteArray b;
    putU8(b, width);
    putU8(b, height);
    putU8(b, desktop ? 1 : 0);
    return control(ControlRecord::Window, b);
}

QByteArray deskMessage(bool extend) {
    return control(ControlRecord::Desk, QByteArray(1, char(extend ? 1 : 0)));
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
    // stale is the caller's question (the lock file answers it). Any other socket name is not a
    // mirror session's and is left alone.
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

// Session setup ----------------------------------------------------------------------------------

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

QString agentVersionRefusal(const AgentHello &hello) {
    if (hello.version == kAgentProtoVersion) return {};
    const QString which = hello.version < kAgentProtoVersion
                              ? QStringLiteral("rebuild the image to update its agent")
                              : QStringLiteral("update Remora on this host");
    return QStringLiteral("%1 v%2, and this client speaks v%3 — %4")
        .arg(QString::fromLatin1(kAgentVersionRefusalPrefix))
        .arg(hello.version)
        .arg(kAgentProtoVersion)
        .arg(which);
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

// Device messages, agent → client ----------------------------------------------------------------

std::optional<DeviceMessage> DeviceMessageParser::next() {
    while (!error_) {
        const std::optional<Record> r = records_.next();
        if (!r) return std::nullopt;
        if (r->type == quint8(DeviceRecord::Clipboard)) {
            DeviceMessage m;
            m.type = DeviceRecord::Clipboard;
            m.clipboardText = QString::fromUtf8(r->body);
            return m;
        }
        if (r->type == quint8(DeviceRecord::ClipboardAck)) {
            if (r->body.size() < 8) {
                error_ = true;
                return std::nullopt;
            }
            DeviceMessage m;
            m.type = DeviceRecord::ClipboardAck;
            m.sequence = qFromBigEndian<quint64>(r->body.constData());
            return m;
        }
        if (r->type == quint8(DeviceRecord::Notice)) {
            DeviceMessage m;
            m.type = DeviceRecord::Notice;
            m.notice = QString::fromUtf8(r->body);
            return m;
        }
        // Unknown type: skipped.
    }
    return std::nullopt;
}

// The agent's side --------------------------------------------------------------------------------

std::optional<AgentHello> parseClientHello(const QByteArray &buf) {
    // The same eight bytes in the other direction: magic, version, flags.
    return parseAgentHello(buf);
}

QByteArray agentHelloReply(quint16 capabilities) {
    QByteArray b(kAgentMagic, 4);
    putU16(b, kAgentProtoVersion);
    putU16(b, capabilities);
    return b;
}

std::optional<ConnectionRequest> parseConnectionRequest(const QByteArray &buf, int *consumed,
                                                        QString *error) {
    if (buf.isEmpty()) return std::nullopt;
    const uchar *d = reinterpret_cast<const uchar *>(buf.constData());
    ConnectionRequest r;
    const quint8 role = d[0];
    if (role == quint8(ConnRole::Video) || role == quint8(ConnRole::Audio)) {
        if (buf.size() < 5) return std::nullopt;
        r.role = ConnRole(role);
        r.sessionId = qFromBigEndian<quint32>(d + 1);
        *consumed = 5;
        return r;
    }
    if (role != quint8(ConnRole::Control)) {
        *error = QStringLiteral("unknown connection role %1").arg(role);
        return std::nullopt;
    }
    if (buf.size() < 4) return std::nullopt;
    const quint8 kind = d[1];
    if (kind < quint8(SessionKind::Mirror) || kind > quint8(SessionKind::AppIcon)) {
        *error = QStringLiteral("unknown session kind %1").arg(kind);
        return std::nullopt;
    }
    const quint16 len = qFromBigEndian<quint16>(d + 2);
    if (buf.size() < 4 + int(len)) return std::nullopt;
    r.kind = SessionKind(kind);
    const QString block = QString::fromUtf8(buf.constData() + 4, len);
    for (const QString &line : block.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        r.options.insert(line.left(eq), line.mid(eq + 1));
    }
    *consumed = 4 + int(len);
    return r;
}

QByteArray sessionAccepted(quint32 sessionId) {
    QByteArray b;
    putU8(b, 0);
    putU32(b, sessionId);
    return b;
}

QByteArray sessionRefused(const QString &reason, quint8 status) {
    QByteArray b;
    putU8(b, status ? status : 1);
    putU32(b, 0);
    const QByteArray utf8 = utf8Truncated(reason, 0xFFFF);
    putU16(b, quint16(utf8.size()));
    b.append(utf8);
    return b;
}

QByteArray listAppsReply(const QByteArray &listing) {
    QByteArray b = sessionAccepted(0);
    putU32(b, quint32(listing.size()));
    b.append(listing);
    return b;
}

QByteArray streamStartRecord(quint8 codec) {
    QByteArray body;
    putU8(body, codec);
    return record(quint8(StreamRecord::Start), body);
}

QByteArray streamConfigRecord(const QByteArray &config) {
    return record(quint8(StreamRecord::Config), config);
}

QByteArray streamFrameRecord(quint64 ptsUs, bool keyFrame, const QByteArray &data) {
    QByteArray body;
    body.reserve(9 + data.size());
    putU64(body, ptsUs);
    putU8(body, keyFrame ? 1 : 0);
    body.append(data);
    return record(quint8(StreamRecord::Frame), body);
}

QByteArray streamEndRecord(quint8 reason, const QString &detail) {
    QByteArray body;
    putU8(body, reason);
    body.append(detail.toUtf8());
    return record(quint8(StreamRecord::End), body);
}

QByteArray deviceNoticeRecord(const QString &text) {
    return record(quint8(DeviceRecord::Notice), utf8Truncated(text, int(kControlRecordMax) - 1));
}

QByteArray deviceClipboardRecord(const QString &text) {
    // The type byte and the size prefix live inside the limit too.
    return record(quint8(DeviceRecord::Clipboard), utf8Truncated(text, int(kControlRecordMax) - 1));
}

QByteArray clipboardAckRecord(quint64 sequence) {
    QByteArray body;
    putU64(body, sequence);
    return record(quint8(DeviceRecord::ClipboardAck), body);
}

namespace {
// Big-endian readers over a record body whose length the caller has checked.
quint32 getU32(const QByteArray &b, int at) {
    return qFromBigEndian<quint32>(b.constData() + at);
}
float getF32(const QByteArray &b, int at) {
    const quint32 bits = getU32(b, at);
    float v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}
}  // namespace

std::optional<ControlMessage> ControlMessageParser::next() {
    // The fixed part of each type's body; Text has none and Clipboard's text follows its nine.
    static const QHash<quint8, int> minSize = {
        {quint8(ControlRecord::Key), 13},     {quint8(ControlRecord::Text), 0},
        {quint8(ControlRecord::Pointer), 26}, {quint8(ControlRecord::Scroll), 20},
        {quint8(ControlRecord::Back), 1},     {quint8(ControlRecord::Panel), 1},
        {quint8(ControlRecord::Clipboard), 9}, {quint8(ControlRecord::Resize), 4},
        {quint8(ControlRecord::Window), 3},   {quint8(ControlRecord::Desk), 1},
    };
    while (!error_) {
        const std::optional<Record> r = records_.next();
        if (!r) return std::nullopt;
        if (!minSize.contains(r->type)) continue;  // unknown: skipped
        const QByteArray &b = r->body;
        if (b.size() < minSize.value(r->type)) {
            error_ = true;
            return std::nullopt;
        }
        ControlMessage m;
        m.type = ControlRecord(r->type);
        switch (m.type) {
        case ControlRecord::Key:
            m.action = quint8(b[0]);
            m.keycode = qint32(getU32(b, 1));
            m.repeat = getU32(b, 5);
            m.metaState = qint32(getU32(b, 9));
            break;
        case ControlRecord::Text:
            m.text = QString::fromUtf8(b);
            break;
        case ControlRecord::Pointer:
            m.action = quint8(b[0]);
            m.tool = quint8(b[1]) == quint8(PointerTool::Mouse) ? PointerTool::Mouse
                                                                : PointerTool::Finger;
            m.pointerId = getU32(b, 2);
            m.at = {getF32(b, 6), getF32(b, 10)};
            m.pressure = getF32(b, 14);
            m.actionButton = qint32(getU32(b, 18));
            m.buttons = qint32(getU32(b, 22));
            break;
        case ControlRecord::Scroll:
            m.at = {getF32(b, 0), getF32(b, 4)};
            m.hScroll = getF32(b, 8);
            m.vScroll = getF32(b, 12);
            m.buttons = qint32(getU32(b, 16));
            break;
        case ControlRecord::Back:
            m.action = quint8(b[0]);
            break;
        case ControlRecord::Panel:
            m.panel = Panel(quint8(b[0]));
            break;
        case ControlRecord::Clipboard:
            m.sequence = qFromBigEndian<quint64>(b.constData());
            m.paste = b[8] != 0;
            m.text = QString::fromUtf8(b.constData() + 9, b.size() - 9);
            break;
        case ControlRecord::Resize:
            m.width = qFromBigEndian<quint16>(b.constData());
            m.height = qFromBigEndian<quint16>(b.constData() + 2);
            if (b.size() >= 6) m.dpi = qFromBigEndian<quint16>(b.constData() + 4);
            break;
        case ControlRecord::Window:
            m.windowWidth = quint8(b[0]);
            m.windowHeight = quint8(b[1]);
            m.windowDesktop = b[2] != 0;
            break;
        case ControlRecord::Desk:
            m.extendDesk = b[0] != 0;
            break;
        }
        return m;
    }
    return std::nullopt;
}

}  // namespace remora::mirror
