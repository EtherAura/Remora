#include <QtTest>

#include "core/MirrorInput.h"
#include "core/MirrorProto.h"
#include "core/SplashFormats.h"

// Byte-exact goldens for the mirror wire protocol (docs/MIRROR_PROTOCOL.md) — a failing golden
// means the wire contract changed, not the test.

using namespace remora::mirror;

static QByteArray hex(const char *s) { return QByteArray::fromHex(s); }

using E = StreamDemuxer::Event;

// The happy-path video stream, every record spelled out in hex so the goldens pin the wire, not
// record(): START h265, FORMAT 1920x1080, CONFIG, a key FRAME at 1000 us, a delta FRAME at 2000.
static QByteArray happyVideo() {
    return hex("00000002" "01" "02"
               "0000000a" "02" "00000780" "00000438" "00"
               "00000003" "03" "abcd"
               "0000000c" "04" "00000000000003e8" "01" "eeff"
               "0000000b" "04" "00000000000007d0" "00" "11");
}

// Drain every ready event, pairing packets with their payloads for compact assertions.
struct Drained {
    QList<StreamDemuxer::Event> events;
    QList<MediaPacket> packets;
};
static Drained drain(StreamDemuxer &d) {
    Drained out;
    for (;;) {
        auto e = d.next();
        if (e == StreamDemuxer::Event::NeedMoreData) return out;
        out.events << e;
        if (e == StreamDemuxer::Event::Packet) out.packets << d.takePacket();
        if (e == StreamDemuxer::Event::Unavailable || e == StreamDemuxer::Event::Failed ||
            e == StreamDemuxer::Event::Error)
            return out;
    }
}

class TestMirrorProto : public QObject {
    Q_OBJECT
private slots:
    // Records ---------------------------------------------------------------------------------

    void recordFramingGolden() {
        // u32 size counts the type byte and the body, never itself.
        QCOMPARE(record(0x07, "ab"), hex("00000003" "07" "6162"));
        QCOMPARE(record(0x05), hex("00000001" "05"));
    }

    void recordReaderLimits() {
        RecordReader r(16);
        r.feed(hex("00000003" "07"));
        QVERIFY(!r.next().has_value());  // size known, body short: wait
        r.feed(hex("6162" "00000001" "05"));
        auto a = r.next();
        QCOMPARE(a->type, quint8(0x07));
        QCOMPARE(a->body, QByteArray("ab"));
        auto b = r.next();
        QCOMPARE(b->type, quint8(0x05));
        QVERIFY(b->body.isEmpty());
        QVERIFY(!r.error());

        // A size of 0 has no type byte, and one over the limit is not worth buffering: both leave
        // no way to find the next record, so the reader latches rather than guess.
        RecordReader zero(16);
        zero.feed(hex("00000000" "00000001" "05"));
        QVERIFY(!zero.next().has_value());
        QVERIFY(zero.error());
        QVERIFY(!zero.next().has_value());
        RecordReader big(16);
        big.feed(hex("00000011"));
        QVERIFY(!big.next().has_value());
        QVERIFY(big.error());
        QVERIFY(big.errorString().contains(QStringLiteral("limit")));
    }

    // Control messages ----------------------------------------------------------------------

    void keyGolden() {
        QCOMPARE(keyMessage(0, 66, 1, 0x3000),
                 hex("0000000e" "01" "00" "00000042" "00000001" "00003000"));
    }

    void textGolden() {
        QCOMPARE(textMessage(QStringLiteral("hé")), hex("00000004" "02" "68c3a9"));
    }

    void pointerGolden() {
        // Fractions and pressure are plain big-endian binary32: 0.5, 0.25, 1.0.
        QCOMPARE(pointerMessage(0, PointerTool::Mouse, 0, {0.5f, 0.25f}, 1.0f, 1, 1),
                 hex("0000001b" "03" "00" "01" "00000000" "3f000000" "3e800000" "3f800000"
                     "00000001" "00000001"));
        QCOMPARE(pointerMessage(1, PointerTool::Finger, 7, {0, 0}, 0, 0, 0).mid(6, 6),
                 hex("00" "00000007" "00"));  // tool 0, pointer id 7, then x
    }

    void scrollGolden() {
        // Notches go over as they are — no fixed point, no scale factor.
        QCOMPARE(scrollMessage({1.0f, 0.0f}, -1.0f, 2.5f, 0),
                 hex("00000015" "04" "3f800000" "00000000" "bf800000" "40200000" "00000000"));
    }

    void simpleMessagesGolden() {
        QCOMPARE(backMessage(1), hex("00000002" "05" "01"));
        QCOMPARE(panelMessage(Panel::Collapse), hex("00000002" "06" "00"));
        QCOMPARE(panelMessage(Panel::Notifications), hex("00000002" "06" "01"));
        QCOMPARE(panelMessage(Panel::QuickSettings), hex("00000002" "06" "02"));
        QCOMPARE(resizeMessage(1920, 1080), hex("00000005" "08" "0780" "0438"));
    }

    void clipboardGolden() {
        QCOMPARE(clipboardMessage(7, true, QStringLiteral("ab")),
                 hex("0000000c" "07" "0000000000000007" "01" "6162"));
        // Cut to fit the 1 MiB record, never mid-codepoint: one ASCII byte then two-byte
        // characters puts the raw cut inside one, and it must back off to a boundary.
        const QString big = QStringLiteral("a") + QString(600000, QChar(0xe9));
        const QByteArray m = clipboardMessage(0, false, big);
        QVERIFY(quint32(m.size() - 4) <= kControlRecordMax);
        const QByteArray text = m.mid(4 + 1 + 9);
        QCOMPARE(QString::fromUtf8(text).toUtf8(), text);  // round-trips: valid UTF-8
        QVERIFY(text.size() > int(kControlRecordMax) - 16);
    }

    void framePointMapping() {
        const FramePoint mid = framePoint({960, 540}, {1920, 1080});
        QCOMPARE(mid.x, 0.5f);
        QCOMPARE(mid.y, 0.5f);
        const FramePoint none = framePoint({10, 10}, {0, 0});  // degenerate frame
        QCOMPARE(none.x, 0.0f);
        QCOMPARE(none.y, 0.0f);
    }

    // Media streams ---------------------------------------------------------------------------

    void videoStreamHappyPath() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(happyVideo());
        const auto [events, packets] = drain(d);
        QCOMPARE(events, (QList<E>{E::Started, E::Format, E::Packet, E::Packet, E::Packet}));
        QCOMPARE(d.codec(), kCodecH265);
        QCOMPARE(d.format().width, quint32(1920));
        QCOMPARE(d.format().height, quint32(1080));
        QVERIFY(!d.format().clientResized);
        QVERIFY(packets[0].config);
        QVERIFY(!packets[0].ptsUs.has_value());  // CONFIG carries no timestamp
        QCOMPARE(packets[0].payload, hex("abcd"));
        QCOMPARE(packets[1].ptsUs, std::optional<quint64>(1000));
        QVERIFY(packets[1].keyFrame);
        QCOMPARE(packets[1].payload, hex("eeff"));
        QVERIFY(!packets[2].keyFrame);
        QCOMPARE(packets[2].payload, hex("11"));
    }

    void byteAtATimeFeeding() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        QList<E> events;
        for (char c : happyVideo()) {
            d.feed(QByteArray(1, c));
            events << drain(d).events;
        }
        QCOMPARE(events, (QList<E>{E::Started, E::Format, E::Packet, E::Packet, E::Packet}));
    }

    void videoNeedsFormatFirst() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(hex("00000002" "01" "01" "0000000b" "04" "0000000000000001" "01" "aa"));
        QCOMPARE(drain(d).events, (QList<E>{E::Started, E::Error}));
    }

    void formatCanReappearMidStream() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(happyVideo());
        drain(d);
        d.feed(hex("0000000a" "02" "00000438" "00000780" "01"));  // rotated, client-resized
        QCOMPARE(drain(d).events, (QList<E>{E::Format}));
        QCOMPARE(d.format().width, quint32(1080));
        QVERIFY(d.format().clientResized);
    }

    void audioHasNoFormat() {
        StreamDemuxer d(StreamDemuxer::Kind::Audio);
        d.feed(hex("00000002" "01" "81"
                   "00000002" "03" "4f"
                   "0000000b" "04" "0000000000000014" "00" "aa"));
        const auto [events, packets] = drain(d);
        QCOMPARE(events, (QList<E>{E::Started, E::Packet, E::Packet}));
        QCOMPARE(d.codec(), kCodecOpus);
        QVERIFY(packets[0].config);
        QCOMPARE(packets[1].ptsUs, std::optional<quint64>(20));
    }

    void endCarriesItsReason() {
        // In place of START: the agent has no such stream, and says why.
        StreamDemuxer none(StreamDemuxer::Kind::Audio);
        none.feed(hex("0000000b" "05" "00" "6e6f20646576"));  // reason 0, "no dev"
        none.feed(hex("6963"));                                // …split mid-record: "ice"
        QCOMPARE(drain(none).events, (QList<E>{}));
        none.feed(hex("65"));
        QCOMPARE(drain(none).events, (QList<E>{E::Unavailable}));
        QCOMPARE(none.detail(), QStringLiteral("no device"));
        // Mid-stream: a failure ends a stream that was working, with its own words.
        StreamDemuxer late(StreamDemuxer::Kind::Video);
        late.feed(happyVideo() + hex("00000005" "05" "01" "6f6f6d"));
        const auto drained = drain(late);
        QCOMPARE(drained.events.last(), E::Failed);
        QCOMPARE(late.detail(), QStringLiteral("oom"));
        QCOMPARE(late.next(), E::NeedMoreData);  // terminal
        QCOMPARE(videoEndMessage(QString()),
                 QStringLiteral("the agent ended the video stream: no reason given"));
    }

    void unknownStreamRecordsAreSkipped() {
        // A newer agent's additions must not break an older client: the size says where the next
        // record starts, so an unknown type costs nothing but its bytes.
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(hex("00000002" "01" "01"
                   "00000003" "7f" "7a7a"
                   "0000000a" "02" "00000010" "00000010" "00"));
        QCOMPARE(drain(d).events, (QList<E>{E::Started, E::Format}));
    }

    void streamFramingViolations() {
        const auto events = [](const QByteArray &bytes) {
            StreamDemuxer d(StreamDemuxer::Kind::Video);
            d.feed(bytes);
            return drain(d).events;
        };
        QCOMPARE(events(hex("00000000")), (QList<E>{E::Error}));             // size 0
        QCOMPARE(events(hex("02000001")), (QList<E>{E::Error}));             // over 32 MiB
        QCOMPARE(events(hex("00000003" "03" "abcd")), (QList<E>{E::Error})); // media before START
        QCOMPARE(events(hex("00000002" "01" "01" "00000002" "01" "01")),
                 (QList<E>{E::Started, E::Error}));                         // a second START
        QCOMPARE(events(hex("00000002" "01" "01" "0000000a" "02" "00000010" "00000010" "00"
                            "00000009" "04" "0000000000000001")),
                 (QList<E>{E::Started, E::Format, E::Error}));              // FRAME with no data
    }

    void configMerging() {
        PacketMerger m;
        MediaPacket config{std::nullopt, true, false, hex("0102")};
        QVERIFY(!m.merge(std::move(config)).has_value());  // held, not delivered
        MediaPacket frame{Q_UINT64_C(5), false, true, hex("aabb")};
        auto merged = m.merge(std::move(frame));
        QCOMPARE(merged->payload, hex("0102aabb"));  // SPS/PPS prepended
        MediaPacket next{Q_UINT64_C(6), false, false, hex("cc")};
        QCOMPARE(m.merge(std::move(next))->payload, hex("cc"));  // merger cleared
    }

    // Device messages -------------------------------------------------------------------------

    void deviceMessages() {
        DeviceMessageParser p;
        p.feed(hex("00000003" "01" "6869"
                   "00000003" "7e" "0000"  // unknown: skipped
                   "00000009" "02" "0000000000000042"));
        auto clip = p.next();
        QCOMPARE(clip->type, DeviceRecord::Clipboard);
        QCOMPARE(clip->clipboardText, QStringLiteral("hi"));
        auto ack = p.next();
        QCOMPARE(ack->type, DeviceRecord::ClipboardAck);
        QCOMPARE(ack->sequence, Q_UINT64_C(0x42));
        QVERIFY(!p.next().has_value());
        QVERIFY(!p.error());
    }

    void deviceMessagesIncremental() {
        DeviceMessageParser p;
        p.feed(hex("000000"));
        QVERIFY(!p.next().has_value());
        p.feed(hex("03" "01" "68"));
        QVERIFY(!p.next().has_value());  // size known, body short
        p.feed(hex("69"));
        QCOMPARE(p.next()->clipboardText, QStringLiteral("hi"));
    }

    void mirrorForwardCandidates() {
        QCOMPARE(adbForwardListArgv("adb"), (QStringList{"adb", "forward", "--list"}));
        // The observed leak (bd remora-6f92): agent forwards from crashed sessions. Rows for other
        // serials and non-mirror targets in one listing — only this serial's agent entries are
        // candidates. The 27183 row sits in the tunnel port range under another socket name: it
        // is not a session's, so it must be IGNORED — port range alone is not ownership.
        const QString list = QStringLiteral(
            "192.168.0.78:5556 tcp:27184 localabstract:remora_agent\n"
            "192.168.0.78:5556 tcp:27185 localabstract:remora_agent\n"
            "192.168.0.78:5556 tcp:27183 localabstract:mirror_49271e25\n"
            "192.168.0.78:5556 tcp:9222 localabstract:chrome_devtools_remote\n"
            "emulator-5554 tcp:27186 localabstract:remora_agent\n");
        QCOMPARE(mirrorForwardPorts(list, QStringLiteral("192.168.0.78:5556")),
                 (QList<quint16>{27184, 27185}));
        QCOMPARE(mirrorForwardPorts(list, QStringLiteral("emulator-5554")),
                 (QList<quint16>{27186}));
        QCOMPARE(mirrorForwardPorts(QString(), QStringLiteral("x")), (QList<quint16>{}));
    }

    void agentHelloGolden() {
        QCOMPARE(agentHello(), hex("524d5241" "0003" "0000"));  // "RMRA", v3, flags 0

        const auto ok = parseAgentHello(hex("524d5241" "0003" "0003"));
        QVERIFY(ok.has_value());
        QCOMPARE(ok->version, quint16(3));
        QCOMPARE(ok->capabilities, quint16(3));
        QVERIFY(agentVersionRefusal(*ok).isEmpty());
        // Short reply: keep waiting, do not guess.
        QVERIFY(!parseAgentHello(hex("524d5241" "0003")).has_value());
        // Wrong magic is how the probe recognises something that is not an agent at all.
        QVERIFY(!parseAgentHello(QByteArray(kAgentHelloSize, '\0')).has_value());
    }

    // bd remora-c79i. Another version is an agent that speaks another format: refused, naming the
    // side to update — and never in the "no in-image agent" words, because there is one.
    void agentVersionRefusals() {
        const QString older = agentVersionRefusal({2, 0});
        QVERIFY(older.startsWith(QLatin1String(kAgentVersionRefusalPrefix)));
        QVERIFY(older.contains(QStringLiteral("v2")));
        QVERIFY(older.contains(QStringLiteral("rebuild the image")));
        const QString newer = agentVersionRefusal({4, 0});
        QVERIFY(newer.startsWith(QLatin1String(kAgentVersionRefusalPrefix)));
        QVERIFY(newer.contains(QStringLiteral("update Remora")));
    }

    void agentSessionRequestGolden() {
        // role 0 (control), u8 kind, u16 length, then the "key=value\n" block — each line
        // NEWLINE-TERMINATED, so a one-param block ends with 0x0a too.
        QCOMPARE(agentSessionRequest(SessionKind::Mirror, {}), hex("00" "01" "0000"));
        QCOMPARE(agentSessionRequest(SessionKind::ListApps, {QStringLiteral("a=b")}),
                 hex("00" "04" "0004" "613d620a"));
        // role 1 (video), u32 session id. The role byte is what makes these two distinguishable:
        // without it, "attach video" and "kind=mirror" are both a leading 0x01 and one listener
        // cannot serve both.
        QCOMPARE(agentVideoAttach(42), hex("01" "0000002a"));
        QVERIFY(agentSessionRequest(SessionKind::Mirror, {}).at(0)
                != agentVideoAttach(1).at(0));
        // Audio is role 2 — same attach shape, so a session with no video never renumbers it
        // (bd remora-28ix.3.6).
        QCOMPARE(agentAudioAttach(42), hex("02" "0000002a"));
        QVERIFY(agentAudioAttach(1).at(0) != agentVideoAttach(1).at(0));
        QVERIFY(agentAudioAttach(1).at(0) != agentSessionRequest(SessionKind::Mirror, {}).at(0));

        ServerOptions o;
        o.videoCodec = QStringLiteral("h265");
        o.maxSize = 1920;
        o.startApp = QStringLiteral("?settings");
        o.extraParams << QStringLiteral("x=y");
        const QStringList p = agentSessionParams(o);
        // The option spellings Session.java parses, start_app among them: an app launch is part
        // of creating the session, not a control message. Nothing about launching a process —
        // a persistent agent has no scid and no tunnel direction to be told.
        QCOMPARE(p, (QStringList{"audio=false", "video_codec=h265", "max_size=1920",
                                 "start_app=?settings", "x=y"}));
        QVERIFY(!p.filter(QStringLiteral("scid")).size());
        QVERIFY(!p.filter(QStringLiteral("tunnel_forward")).size());
    }

    void agentSessionReplyParsing() {
        int consumed = 0;
        const auto ok = parseSessionReply(hex("00" "0000002a"), &consumed);
        QVERIFY(ok.has_value());
        QCOMPARE(ok->status, quint8(0));
        QCOMPARE(ok->sessionId, quint32(42));
        QCOMPARE(consumed, 5);  // success carries no reason: the control stream starts at byte 5

        // A failure's u16-prefixed reason must arrive whole before the reply is complete.
        QVERIFY(!parseSessionReply(hex("01" "00000000" "0002" "6e"), &consumed).has_value());
        const auto bad = parseSessionReply(hex("01" "00000000" "0002" "6e6f"), &consumed);
        QVERIFY(bad.has_value());
        QCOMPARE(bad->status, quint8(1));
        QCOMPARE(bad->reason, QStringLiteral("no"));
        QCOMPARE(consumed, 9);
        QVERIFY(!parseSessionReply(hex("00" "000000"), nullptr).has_value());
    }

    void agentTunnelArgvGolden() {
        // Forward only — the agent listens, so the client always dials in.
        QCOMPARE(adbAgentTunnelArgv("adb", "127.0.0.1:5555", 27183, false),
                 (QStringList{"adb", "-s", "127.0.0.1:5555", "forward", "tcp:27183",
                              "localabstract:remora_agent"}));
        QCOMPARE(adbAgentTunnelArgv("adb", "127.0.0.1:5555", 27183, true),
                 (QStringList{"adb", "-s", "127.0.0.1:5555", "forward", "--remove", "tcp:27183"}));
    }

    void inputKeymap() {
        QCOMPARE(androidKeycode(Qt::Key_A).value(), 29);
        QCOMPARE(androidKeycode(Qt::Key_Z).value(), 54);
        QCOMPARE(androidKeycode(Qt::Key_0).value(), 7);
        QCOMPARE(androidKeycode(Qt::Key_9).value(), 16);
        QCOMPARE(androidKeycode(Qt::Key_F1).value(), 131);
        QCOMPARE(androidKeycode(Qt::Key_F12).value(), 142);
        QCOMPARE(androidKeycode(Qt::Key_Return).value(), 66);
        QCOMPARE(androidKeycode(Qt::Key_Backspace).value(), 67);
        QCOMPARE(androidKeycode(Qt::Key_Escape).value(), 111);
        QVERIFY(!androidKeycode(Qt::Key_CapsLock).has_value());
    }

    // mouse_bind is a POSITIONAL string: a wrong length or an unknown character silently rebinds
    // or disarms a button, so parsing refuses rather than guessing (bd remora-28ix.3.3).
    void mouseBindParsing() {
        const auto def = defaultMouseBindings();
        QCOMPARE(def.at(kSlotRight, false), BindAction::Back);
        QCOMPARE(def.at(kSlotMiddle, false), BindAction::Home);
        QCOMPARE(def.at(kSlotSide4, false), BindAction::AppSwitch);
        QCOMPARE(def.at(kSlotSide5, false), BindAction::Notifications);
        // The shifted half of the default passes every click through untouched.
        for (int s = kSlotRight; s < kSlotCount; ++s)
            QCOMPARE(def.at(s, true), BindAction::PassThrough);

        // The real profile this landed for: right = Recents and the 4th button = Back, i.e. the
        // default's two swapped.
        const auto swapped = parseMouseBind(QStringLiteral("shbn:++++"));
        QVERIFY(swapped.has_value());
        QCOMPARE(swapped->at(kSlotRight, false), BindAction::AppSwitch);
        QCOMPARE(swapped->at(kSlotSide4, false), BindAction::Back);

        const auto ignoring = parseMouseBind(QStringLiteral("-b-h:n-s-"));
        QVERIFY(ignoring.has_value());
        QCOMPARE(ignoring->at(kSlotRight, false), BindAction::Ignore);
        QCOMPARE(ignoring->at(kSlotMiddle, false), BindAction::Back);
        QCOMPARE(ignoring->at(kSlotRight, true), BindAction::Notifications);

        QVERIFY(!parseMouseBind(QStringLiteral("bhsn")).has_value());        // no shifted half
        QVERIFY(!parseMouseBind(QStringLiteral("bhs:++++")).has_value());    // short vector
        QVERIFY(!parseMouseBind(QStringLiteral("bhsnn:++++")).has_value());  // long vector
        QVERIFY(!parseMouseBind(QStringLiteral("bhsx:++++")).has_value());   // unknown action
        QVERIFY(!parseMouseBind(QStringLiteral("BHSN:++++")).has_value());   // case is not folded
        QVERIFY(!parseMouseBind(QString()).has_value());
    }

    // Each bindable button forwards as its own MotionEvent.BUTTON_*, which is also what tells the
    // device a real mouse from a finger.
    void mouseSlotButtons() {
        QCOMPARE(slotButton(kSlotRight), kButtonSecondary);
        QCOMPARE(slotButton(kSlotMiddle), kButtonTertiary);
        QCOMPARE(slotButton(kSlotSide4), kButtonBack);
        QCOMPARE(slotButton(kSlotSide5), kButtonForward);
    }

    // key_bind spells keys the SDL way. The card and the client share this table precisely so a
    // capturable key is an honoured key (bd remora-28ix.2.6).
    void sdlKeyNames() {
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("Escape")).value(), int(Qt::Key_Escape));
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("escape")).value(), int(Qt::Key_Escape));
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("Esc")).value(), int(Qt::Key_Escape));
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("F12")).value(), int(Qt::Key_F12));
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("a")).value(), int(Qt::Key_A));
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("7")).value(), int(Qt::Key_7));
        QCOMPARE(qtKeyFromSdlName(QStringLiteral("PageDown")).value(), int(Qt::Key_PageDown));
        QVERIFY(!qtKeyFromSdlName(QStringLiteral("NoSuchKey")).has_value());
        QVERIFY(!qtKeyFromSdlName(QStringLiteral("F99")).has_value());
        QVERIFY(!qtKeyFromSdlName(QString()).has_value());

        // Round-trip: the canonical spelling is what gets written back.
        QCOMPARE(sdlKeyName(Qt::Key_Escape), QStringLiteral("Escape"));
        QCOMPARE(sdlKeyName(Qt::Key_Print), QStringLiteral("PrintScreen"));
        QCOMPARE(sdlKeyName(Qt::Key_Menu), QStringLiteral("Application"));
        QCOMPARE(sdlKeyName(Qt::Key_F5), QStringLiteral("F5"));
        QCOMPARE(sdlKeyName(Qt::Key_A), QStringLiteral("A"));
        QVERIFY(sdlKeyName(Qt::Key_Shift).isEmpty());
        for (int k : {int(Qt::Key_Escape), int(Qt::Key_F1), int(Qt::Key_A), int(Qt::Key_0),
                      int(Qt::Key_PageUp), int(Qt::Key_Menu)})
            QCOMPARE(qtKeyFromSdlName(sdlKeyName(k)).value(), k);
    }

    void keyBindParsing() {
        // The live profile's binding: Escape goes Back.
        const auto esc = parseKeyBind(QStringLiteral("Escape:b"));
        QVERIFY(esc.has_value());
        QCOMPARE(esc->value(Qt::Key_Escape), BindAction::Back);

        const auto many = parseKeyBind(QStringLiteral("F1:b,F2:h,F3:s,F4:n"));
        QVERIFY(many.has_value());
        QCOMPARE(many->size(), 4);
        QCOMPARE(many->value(Qt::Key_F3), BindAction::AppSwitch);
        QCOMPARE(many->value(Qt::Key_F4), BindAction::Notifications);

        QVERIFY(parseKeyBind(QString())->isEmpty());  // no bindings is valid, not an error
        QVERIFY(!parseKeyBind(QStringLiteral("Escape")).has_value());     // no action
        QVERIFY(!parseKeyBind(QStringLiteral("Escape:")).has_value());    // empty action
        QVERIFY(!parseKeyBind(QStringLiteral("Nope:b")).has_value());     // unknown key
        QVERIFY(!parseKeyBind(QStringLiteral("Escape:x")).has_value());   // unknown action
        // '+'/'-' describe what to do with a CLICK; they cannot mean anything for a key.
        QVERIFY(!parseKeyBind(QStringLiteral("Escape:+")).has_value());
        QVERIFY(!parseKeyBind(QStringLiteral("Escape:-")).has_value());
    }

    void shortcutModParsing() {
        // Unset = lalt,lsuper (Alt or Super), which is what the built-in Alt+D/Alt+F already used.
        const auto def = parseShortcutMod(QString());
        QVERIFY(def.has_value());
        QVERIFY(def->contains(Qt::AltModifier));
        QVERIFY(def->contains(Qt::MetaModifier));

        const auto combo = parseShortcutMod(QStringLiteral("lctrl+lalt"));
        QVERIFY(combo.has_value());
        QCOMPARE(combo->size(), 1);
        QCOMPARE(combo->first(), Qt::ControlModifier | Qt::AltModifier);

        const auto alts = parseShortcutMod(QStringLiteral("lctrl,lsuper"));
        QCOMPARE(alts->size(), 2);

        // Qt cannot tell left from right, so the two sides collapse to one alternative rather
        // than arming a duplicate.
        QCOMPARE(parseShortcutMod(QStringLiteral("lalt,ralt"))->size(), 1);

        QVERIFY(!parseShortcutMod(QStringLiteral("shift")).has_value());  // never a shortcut mod
        QVERIFY(!parseShortcutMod(QStringLiteral("lmeta")).has_value());  // super is lsuper
    }

    void chordActions() {
        QCOMPARE(chordAction(Qt::Key_B).value(), BindAction::Back);
        QCOMPARE(chordAction(Qt::Key_H).value(), BindAction::Home);
        QCOMPARE(chordAction(Qt::Key_S).value(), BindAction::AppSwitch);
        QCOMPARE(chordAction(Qt::Key_N).value(), BindAction::Notifications);
        QVERIFY(!chordAction(Qt::Key_D).has_value());  // Alt+D is the client's own chord
        QVERIFY(!chordAction(Qt::Key_F).has_value());
    }

    void inputMetaState() {
        QCOMPARE(androidMetaState(Qt::NoModifier), 0);
        QCOMPARE(androidMetaState(Qt::ShiftModifier), 0x41);
        QCOMPARE(androidMetaState(Qt::ControlModifier), 0x3000);
        QCOMPARE(androidMetaState(Qt::ShiftModifier | Qt::AltModifier), 0x41 | 0x12);
    }

    void coordinateMapping() {
        // Stretched fit: both axes scale independently; results clamp inside the frame.
        QCOMPARE(mapToVideo({0, 0}, {800, 600}, {1920, 1080}), QPoint(0, 0));
        QCOMPARE(mapToVideo({400, 300}, {800, 600}, {1920, 1080}), QPoint(960, 540));
        QCOMPARE(mapToVideo({800, 600}, {800, 600}, {1920, 1080}), QPoint(1919, 1079));
        QCOMPARE(mapToVideo({-5, -5}, {800, 600}, {1920, 1080}), QPoint(0, 0));
        QCOMPARE(mapToVideo({10, 10}, {0, 0}, {1920, 1080}), QPoint(0, 0));  // degenerate widget
    }

    void bootAnimDescParsing() {
        const auto d = parseBootAnimDesc(QStringLiteral(
            "3760 1992 30\n"
            "c 1 0 part0\n"
            "p 0 15 part1\n"
            "# vendor extension line, ignored\n"
            "f 2 0 part2 extra tokens\n"));
        QVERIFY(d.valid());
        QCOMPARE(d.width, 3760);
        QCOMPARE(d.fps, 30.0);
        QCOMPARE(d.parts.size(), 3);
        QCOMPARE(d.parts[0].type, QChar('c'));
        QCOMPARE(d.parts[1].count, 0);   // 0 = loop forever
        QCOMPARE(d.parts[1].pause, 15);
        QCOMPARE(d.parts[1].dir, QStringLiteral("part1"));
        QVERIFY(!parseBootAnimDesc(QStringLiteral("garbage")).valid());
    }

    // The bug this pins: the A17 animation is "c 1 0 part0 / c 0 0 part1 / c 1 0 part2" and its
    // ENDING (part2) never played, because a count=0 part loops forever and nothing ever told
    // playback that boot had finished — so part1 was terminal in practice.
    void bootAnimPartAdvance() {
        const auto once = parseBootAnimDesc(QStringLiteral(
            "1080 360 60\n" "c 1 0 part0\n" "c 0 0 part1\n" "c 1 0 part2\n"));
        QCOMPARE(once.parts.size(), 3);

        // Still booting: the intro yields after its single playthrough...
        QCOMPARE(bootAnimPartStep(once.parts[0], 1, false, false), PartStep::Advance);
        // ...and the loop part does not, however many times it runs.
        QCOMPARE(bootAnimPartStep(once.parts[1], 1, false, false), PartStep::Repeat);
        QCOMPARE(bootAnimPartStep(once.parts[1], 99, false, false), PartStep::Repeat);

        // Boot finished: the loop yields at the end of its CURRENT playthrough, so the ending
        // is reachable at last.
        QCOMPARE(bootAnimPartStep(once.parts[1], 1, false, true), PartStep::Advance);
        // The ending plays once and then holds — repeating it would read as an endless loop.
        QCOMPARE(bootAnimPartStep(once.parts[2], 1, true, true), PartStep::Finished);
        // A last part reached while still booting holds instead: a tail part is usually the loop.
        QCOMPARE(bootAnimPartStep(once.parts[2], 1, true, false), PartStep::Repeat);
        // A multi-play part is not cut short before its count, even once finishing.
        BootAnimPart thrice;
        thrice.count = 3;
        QCOMPARE(bootAnimPartStep(thrice, 2, false, true), PartStep::Repeat);
        QCOMPARE(bootAnimPartStep(thrice, 3, false, true), PartStep::Advance);
    }

    void splashStatusMarkers() {
        // Order carries meaning: BOOTING clears earlier ATTACH/ABORT; later markers stand.
        auto s = parseSplashStatus(QStringLiteral(
            "Deploying container...\nREMORA_ABORT\nREMORA_BOOTING\nWaiting for boot...\n"));
        QVERIFY(s.booting);
        QVERIFY(!s.abort);
        QVERIFY(!s.attach);
        QCOMPARE(s.text, QStringLiteral("Waiting for boot..."));
        s = parseSplashStatus(QStringLiteral("REMORA_BOOTING\nREMORA_ATTACH\n"));
        QVERIFY(s.attach);
        s = parseSplashStatus(QString());
        QVERIFY(!s.booting && !s.attach && !s.abort && s.text.isEmpty());
    }

    void deviceFramingErrorLatches() {
        DeviceMessageParser p;
        p.feed(hex("00000000"));  // size 0: no type, no way to find the next record
        QVERIFY(!p.next().has_value());
        QVERIFY(p.error());
        p.feed(hex("00000003" "01" "6869"));  // a valid message after the poison
        QVERIFY(!p.next().has_value());       // stays latched
        // A known type whose body is too short for its fields is a violation too.
        DeviceMessageParser shortAck;
        shortAck.feed(hex("00000003" "02" "0000"));
        QVERIFY(!shortAck.next().has_value());
        QVERIFY(shortAck.error());
    }
};

QTEST_APPLESS_MAIN(TestMirrorProto)
#include "test_mirrorproto.moc"
