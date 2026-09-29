#include <QtTest>

#include "core/MirrorInput.h"
#include "core/MirrorProto.h"
#include "core/SplashFormats.h"

// Byte-exact goldens for the mirror wire protocol (docs/MIRROR_PROTOCOL.md). Every vector here
// was cross-checked against the reference implementation's serialization code (scrcpy v4.0) — a
// failing golden means the wire contract changed, not the test.

using namespace remora::mirror;

static QByteArray hex(const char *s) { return QByteArray::fromHex(s); }

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
        if (e == StreamDemuxer::Event::StreamDisabled || e == StreamDemuxer::Event::ConfigError ||
            e == StreamDemuxer::Event::Error)
            return out;
    }
}

class TestMirrorProto : public QObject {
    Q_OBJECT
private slots:
    void fixedPoint() {
        QCOMPARE(pressureToU16(1.0f), quint16(0xFFFF));
        QCOMPARE(pressureToU16(0.5f), quint16(0x8000));
        QCOMPARE(pressureToU16(0.0f), quint16(0));
        QCOMPARE(pressureToU16(-1.0f), quint16(0));
        // The wire value is value/16 in [-1,1] fixed-point; ±16 saturates, beyond clamps.
        QCOMPARE(scrollToI16(16.0f), qint16(0x7FFF));
        QCOMPARE(scrollToI16(-16.0f), qint16(-0x8000));
        QCOMPARE(scrollToI16(32.0f), qint16(0x7FFF));
        QCOMPARE(scrollToI16(-32.0f), qint16(-0x8000));
        QCOMPARE(scrollToI16(0.0f), qint16(0));
    }

    void keycodeGolden() {
        QCOMPARE(injectKeycode(0, 3 /* AKEYCODE_HOME */, 0, 0),
                 hex("00" "00" "00000003" "00000000" "00000000"));
    }

    void textGolden() {
        QCOMPARE(injectText(QStringLiteral("hé")), hex("01" "00000003" "68c3a9"));
        // Cap is 300 UTF-8 bytes...
        QCOMPARE(injectText(QString(301, QLatin1Char('a'))).size(), 5 + 300);
        // ...and never splits a codepoint: 299 ASCII + 'é' (2 bytes) cuts before the 'é'.
        const QByteArray b = injectText(QString(299, QLatin1Char('a')) + QChar(0xe9));
        QCOMPARE(b.size(), 5 + 299);
        QCOMPARE(b.mid(1, 4), hex("0000012b"));  // 299
    }

    void touchGolden() {
        QCOMPARE(injectTouch(2 /* MOVE */, kPointerIdMouse, {100, -200, 1920, 1080}, 1.0f, 1, 1),
                 hex("02" "02" "ffffffffffffffff" "00000064" "ffffff38" "0780" "0438" "ffff"
                     "00000001" "00000001"));
    }

    void scrollGolden() {
        QCOMPARE(injectScroll({1, 2, 3, 4}, 16.0f, -16.0f, 0),
                 hex("03" "00000001" "00000002" "0003" "0004" "7fff" "8000" "00000000"));
    }

    void simpleMessagesGolden() {
        QCOMPARE(backOrScreenOn(0), hex("0400"));
        QCOMPARE(expandNotificationPanel(), hex("05"));
        QCOMPARE(expandSettingsPanel(), hex("06"));
        QCOMPARE(collapsePanels(), hex("07"));
        QCOMPARE(getClipboard(1 /* COPY */), hex("0801"));
        QCOMPARE(setDisplayPower(false), hex("0a00"));
        QCOMPARE(rotateDevice(), hex("0b"));
        QCOMPARE(openHardKeyboardSettings(), hex("0f"));
        QCOMPARE(resetVideo(), hex("11"));
        QCOMPARE(resizeDisplay(1080, 2340), hex("15" "0438" "0924"));
    }

    void clipboardGolden() {
        QCOMPARE(setClipboard(Q_UINT64_C(0x0102030405060708), true, QStringLiteral("hi")),
                 hex("09" "0102030405060708" "01" "00000002" "6869"));
    }

    void uhidGolden() {
        QCOMPARE(uhidCreate(1, 0x046d, 0xc52b, QStringLiteral("kbd"), hex("05010906")),
                 hex("0c" "0001" "046d" "c52b" "03" "6b6264" "0004" "05010906"));
        QCOMPARE(uhidInput(1, hex("0102")), hex("0d" "0001" "0002" "0102"));
        QCOMPARE(uhidDestroy(1), hex("0e" "0001"));
    }

    void videoStreamHappyPath() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(hex("68323635"                                       // h265
                   "80000001" "00000780" "00000438"                 // session, client-resized
                   "4000000000000000" "00000002" "0102"             // config packet, no PTS
                   "2000000000000005" "00000003" "aabbcc"));        // keyframe, pts=5
        auto r = drain(d);
        QCOMPARE(r.events, (QList<StreamDemuxer::Event>{
                               StreamDemuxer::Event::CodecId, StreamDemuxer::Event::Session,
                               StreamDemuxer::Event::Packet, StreamDemuxer::Event::Packet}));
        QCOMPARE(d.codecId(), kCodecH265);
        QCOMPARE(d.session().width, quint32(1920));
        QCOMPARE(d.session().height, quint32(1080));
        QVERIFY(d.session().clientResized);
        QVERIFY(r.packets[0].config);
        QVERIFY(!r.packets[0].ptsUs.has_value());
        QCOMPARE(r.packets[0].payload, hex("0102"));
        QVERIFY(r.packets[1].keyFrame);
        QVERIFY(!r.packets[1].config);
        QCOMPARE(r.packets[1].ptsUs.value(), Q_UINT64_C(5));
        QCOMPARE(r.packets[1].payload, hex("aabbcc"));
    }

    void videoRequiresSessionFirst() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(hex("68323634" "2000000000000005" "00000001" "aa"));
        auto r = drain(d);
        QCOMPARE(r.events.last(), StreamDemuxer::Event::Error);
        QVERIFY(!d.errorString().isEmpty());
        QCOMPARE(d.next(), StreamDemuxer::Event::Error);  // latches
    }

    void sentinelCodecIds() {
        StreamDemuxer off(StreamDemuxer::Kind::Audio);
        off.feed(hex("00000000"));
        QCOMPARE(off.next(), StreamDemuxer::Event::StreamDisabled);
        QCOMPARE(off.next(), StreamDemuxer::Event::NeedMoreData);  // terminal, not an error

        StreamDemuxer bad(StreamDemuxer::Kind::Video);
        bad.feed(hex("00000001"));
        QCOMPARE(bad.next(), StreamDemuxer::Event::ConfigError);
    }

    void zeroLengthPacketIsFatal() {
        StreamDemuxer d(StreamDemuxer::Kind::Audio);
        d.feed(hex("6f707573" "0000000000000001" "00000000"));
        auto r = drain(d);
        QCOMPARE(r.events.last(), StreamDemuxer::Event::Error);
    }

    void ptsIs61Bits() {
        StreamDemuxer d(StreamDemuxer::Kind::Audio);
        d.feed(hex("6f707573" "3fffffffffffffff" "00000001" "aa"));
        auto r = drain(d);
        QVERIFY(r.packets[0].keyFrame);
        QCOMPARE(r.packets[0].ptsUs.value(), Q_UINT64_C(0x1FFFFFFFFFFFFFFF));
    }

    void sessionCanReappearMidStream() {
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        d.feed(hex("68323634"
                   "80000000" "00000780" "00000438"
                   "2000000000000001" "00000001" "aa"
                   "80000000" "00000438" "00000780"));  // rotation
        auto r = drain(d);
        QCOMPARE(r.events, (QList<StreamDemuxer::Event>{
                               StreamDemuxer::Event::CodecId, StreamDemuxer::Event::Session,
                               StreamDemuxer::Event::Packet, StreamDemuxer::Event::Session}));
        QCOMPARE(d.session().width, quint32(1080));
        QVERIFY(!d.session().clientResized);
    }

    void audioHasNoSessionPacket() {
        StreamDemuxer d(StreamDemuxer::Kind::Audio);
        d.feed(hex("6f707573" "0000000000000005" "00000001" "aa"));
        auto r = drain(d);
        QCOMPARE(r.events, (QList<StreamDemuxer::Event>{StreamDemuxer::Event::CodecId,
                                                        StreamDemuxer::Event::Packet}));
    }

    // Fragmentation must not change the event stream: same bytes, fed one at a time.
    void byteAtATimeFeeding() {
        const QByteArray stream = hex("68323635"
                                      "80000001" "00000780" "00000438"
                                      "4000000000000000" "00000002" "0102"
                                      "2000000000000005" "00000003" "aabbcc");
        StreamDemuxer d(StreamDemuxer::Kind::Video);
        QList<StreamDemuxer::Event> events;
        QList<MediaPacket> packets;
        for (char c : stream) {
            d.feed(QByteArray(1, c));
            for (;;) {
                auto e = d.next();
                if (e == StreamDemuxer::Event::NeedMoreData) break;
                events << e;
                if (e == StreamDemuxer::Event::Packet) packets << d.takePacket();
            }
        }
        QCOMPARE(events, (QList<StreamDemuxer::Event>{
                             StreamDemuxer::Event::CodecId, StreamDemuxer::Event::Session,
                             StreamDemuxer::Event::Packet, StreamDemuxer::Event::Packet}));
        QCOMPARE(packets[1].payload, hex("aabbcc"));
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

    void deviceMessages() {
        DeviceMessageParser p;
        p.feed(hex("00" "00000002" "6869"
                   "01" "0000000000000042"
                   "02" "0001" "0002" "aabb"));
        auto clip = p.next();
        QCOMPARE(clip->type, DeviceMessage::Type::Clipboard);
        QCOMPARE(clip->clipboardText, QStringLiteral("hi"));
        auto ack = p.next();
        QCOMPARE(ack->type, DeviceMessage::Type::AckClipboard);
        QCOMPARE(ack->sequence, Q_UINT64_C(0x42));
        auto uhid = p.next();
        QCOMPARE(uhid->type, DeviceMessage::Type::UhidOutput);
        QCOMPARE(uhid->uhidId, quint16(1));
        QCOMPARE(uhid->uhidData, hex("aabb"));
        QVERIFY(!p.next().has_value());
        QVERIFY(!p.error());
    }

    void deviceMessagesIncremental() {
        DeviceMessageParser p;
        p.feed(hex("00"));
        QVERIFY(!p.next().has_value());
        p.feed(hex("00000002" "68"));
        QVERIFY(!p.next().has_value());  // length known, payload short
        p.feed(hex("69"));
        QCOMPARE(p.next()->clipboardText, QStringLiteral("hi"));
    }

    void mirrorForwardCandidates() {
        QCOMPARE(adbForwardListArgv("adb"), (QStringList{"adb", "forward", "--list"}));
        // The observed leak (bd remora-6f92): agent forwards from crashed sessions. Rows for other
        // serials and non-mirror targets in one listing — only this serial's agent entries are
        // candidates. The scrcpy_<hex> row is KEPT IN THE FIXTURE ON PURPOSE and is now expected to
        // be IGNORED (bd remora-28ix.4 step 4): nothing can create such a tunnel any
        // more, since an image without the agent bake is refused rather than falling back, so a row
        // like this can only be a fossil. Asserting it is skipped is a stronger claim than deleting
        // the row would be — deleting it would stop testing the discrimination entirely.
        const QString list = QStringLiteral(
            "192.168.0.78:5556 tcp:27184 localabstract:remora_agent\n"
            "192.168.0.78:5556 tcp:27185 localabstract:remora_agent\n"
            "192.168.0.78:5556 tcp:27183 localabstract:scrcpy_49271e25\n"
            "192.168.0.78:5556 tcp:9222 localabstract:chrome_devtools_remote\n"
            "emulator-5554 tcp:27186 localabstract:remora_agent\n");
        QCOMPARE(mirrorForwardPorts(list, QStringLiteral("192.168.0.78:5556")),
                 (QList<quint16>{27184, 27185}));
        QCOMPARE(mirrorForwardPorts(list, QStringLiteral("emulator-5554")),
                 (QList<quint16>{27186}));
        QCOMPARE(mirrorForwardPorts(QString(), QStringLiteral("x")), (QList<quint16>{}));
    }

    void agentHelloGolden() {
        QCOMPARE(agentHello(), hex("524d5241" "0002" "0000"));  // "RMRA", v2, flags 0

        const auto ok = parseAgentHello(hex("524d5241" "0002" "0003"));
        QVERIFY(ok.has_value());
        QCOMPARE(ok->version, quint16(2));
        QCOMPARE(ok->capabilities, quint16(3));
        // Short reply: keep waiting, do not guess.
        QVERIFY(!parseAgentHello(hex("524d5241" "0002")).has_value());
        // Wrong magic is how the probe recognises a device with no agent — the v1 server's first
        // bytes are its 64-byte device-name field, which cannot begin with "RMRA" by accident.
        QVERIFY(!parseAgentHello(QByteArray(kAgentHelloSize, '\0')).has_value());
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
        // Audio is role 2 — same attach shape, so a session with no video does not renumber it
        // the way v1's positional ordering did (bd remora-28ix.3.6).
        QCOMPARE(agentAudioAttach(42), hex("02" "0000002a"));
        QVERIFY(agentAudioAttach(1).at(0) != agentVideoAttach(1).at(0));
        QVERIFY(agentAudioAttach(1).at(0) != agentSessionRequest(SessionKind::Mirror, {}).at(0));

        ServerOptions o;
        o.videoCodec = QStringLiteral("h265");
        o.maxSize = 1920;
        o.startApp = QStringLiteral("?settings");
        o.extraParams << QStringLiteral("x=y");
        const QStringList p = agentSessionParams(o);
        // v1's param vocabulary verbatim — minus scid/log_level/tunnel_forward, which describe
        // the v1 launch and have no meaning for a persistent agent — plus start_app, which the
        // agent takes at session creation where v1 sent a TYPE_START_APP control message.
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
        // Unset = the fork's lalt,lsuper, which is what the built-in Alt+D/Alt+F already used.
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
        QVERIFY(!parseShortcutMod(QStringLiteral("lmeta")).has_value());  // not scrcpy's spelling
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

    void unknownDeviceMessageIsUnrecoverable() {
        DeviceMessageParser p;
        p.feed(hex("63"));
        QVERIFY(!p.next().has_value());
        QVERIFY(p.error());
        p.feed(hex("00" "00000001" "61"));  // a valid message after the poison byte
        QVERIFY(!p.next().has_value());     // stays latched: no resync point exists
    }
};

QTEST_APPLESS_MAIN(TestMirrorProto)
#include "test_mirrorproto.moc"
