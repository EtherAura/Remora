// The host agent's pure parts (docs/PHONE_APP.md): the desk layout it keeps while it adds
// a phone's virtual output, the app list it serves, and its side of protocol v3 — pinned against
// the client's own encoders, so the two halves cannot drift apart.
#include <QtTest>

#include <cmath>

#include "core/DesktopEntries.h"
#include "core/GpuMemory.h"
#include "core/HostInput.h"
#include "core/MirrorProto.h"
#include "core/OutputLayout.h"
#ifdef REMORA_TEST_XKB
#include "hostagent/DeskKeymap.h"
#endif

using namespace remora;

namespace {

// kscreen-doctor -j, trimmed to the fields the planner reads, in the shape Plasma 6.8 prints:
// a 4K monitor at scale 2 above-left of an unscaled one that is primary, plus (optionally) a
// virtual output as KWin placed it — overlapping, and with a priority above the second monitor.
// The virtual output's entry has no "enabled" key, which is how kscreen reports one.
QByteArray kscreen(bool withVirtual, int virtX = 1920, int virtPriority = 2, int secondPriority = 3,
                   double virtScale = 1.0) {
    QByteArray json = R"({"outputs":[
      {"name":"DP-1","enabled":true,"connected":true,"pos":{"x":0,"y":0},"scale":2,
       "priority":)" + QByteArray::number(withVirtual ? secondPriority : 2) + R"(,"rotation":1,
       "currentModeId":"1","modes":[{"id":"1","size":{"width":3840,"height":2160}},
                                     {"id":"2","size":{"width":1920,"height":1080}}]},
      {"name":"HDMI-A-1","enabled":true,"connected":true,"pos":{"x":474,"y":1011},"scale":1,
       "priority":1,"rotation":1,"currentModeId":"32",
       "modes":[{"id":"32","size":{"width":3840,"height":2160}}]},
      {"name":"HDMI-A-2","enabled":false,"connected":false,"pos":{"x":0,"y":0},"scale":1,
       "priority":0,"rotation":1,"currentModeId":"","modes":[]})";
    if (withVirtual) {
        json += R"(,
      {"name":"Virtual-remora-1","connected":true,"pos":{"x":)" + QByteArray::number(virtX) +
                R"(,"y":0},"scale":)" + QByteArray::number(virtScale) + R"(,"priority":)" +
                QByteArray::number(virtPriority) + R"(,"rotation":1,"currentModeId":"0",
       "modes":[{"id":"0","size":{"width":1080,"height":2400}}]})";
    }
    return json + "]}";
}

}  // namespace

class TestHostAgent : public QObject {
    Q_OBJECT
private slots:
    void parsesKScreen() {
        const auto outs = outputs::parseKScreenJson(kscreen(true));
        QCOMPARE(outs.size(), 4);
        QCOMPARE(outs[0].name, QStringLiteral("DP-1"));
        QCOMPARE(outs[0].pixelSize, QSize(3840, 2160));
        QCOMPARE(outs[0].logicalSize(), QSize(1920, 1080));
        QVERIFY(!outs[2].enabled);
        // No "enabled" key: connected stands in.
        QVERIFY(outs[3].enabled);
        QCOMPARE(outs[3].priority, 2);
        QVERIFY(outputs::parseKScreenJson("not json").isEmpty());
    }

    void logicalSizeRotatesAndRoundsUp() {
        outputs::OutputState s;
        s.pixelSize = {1080, 2400};
        s.scale = 2.625;
        QCOMPARE(s.logicalSize(), QSize(412, 915));  // 411.43 × 914.29, rounded up
        s.rotation = 8;
        QCOMPARE(s.logicalSize(), QSize(915, 412));
    }

    // The measured case: KWin moved nothing this time but handed the virtual output priority 2,
    // pushing the second monitor to 3 — a panel would follow it onto the phone's screen.
    void repairsPrioritiesAndParksTheVirtualOutput() {
        const auto before = outputs::parseKScreenJson(kscreen(false));
        const auto now = outputs::parseKScreenJson(kscreen(true));
        const QStringList args =
            outputs::repairArgs(before, now, {{QStringLiteral("Virtual-remora-1"), 2.625}});
        // Right of the bounding box (HDMI-A-1 ends at 474 + 3840), top-aligned with the desk.
        QCOMPARE(args, (QStringList{
            QStringLiteral("output.DP-1.priority.2"),
            QStringLiteral("output.Virtual-remora-1.position.4314,0"),
            QStringLiteral("output.Virtual-remora-1.priority.3"),
            QStringLiteral("output.Virtual-remora-1.scale.2.625"),
        }));
    }

    void movedMonitorsGoBack() {
        const auto before = outputs::parseKScreenJson(kscreen(false));
        QByteArray moved = kscreen(true);
        moved.replace(R"("pos":{"x":474,"y":1011})", R"("pos":{"x":0,"y":0})");
        const QStringList args = outputs::repairArgs(before, outputs::parseKScreenJson(moved),
                                                     {{QStringLiteral("Virtual-remora-1"), 1.0}});
        QVERIFY(args.contains(QStringLiteral("output.HDMI-A-1.position.474,1011")));
    }

    // KWin saved the corrected layout last time and applied it again: nothing to do.
    void settledLayoutIsANoOp() {
        const auto before = outputs::parseKScreenJson(kscreen(false));
        const auto now = outputs::parseKScreenJson(kscreen(true, 4314, 3, 2, 2.625));
        QVERIFY(outputs::repairArgs(before, now, {{QStringLiteral("Virtual-remora-1"), 2.625}})
                    .isEmpty());
    }

    // A scale off KWin's 120ths grid (a reduced session size: 320 dpi scaled to 109) is asked for
    // as the step KWin will store, and the stored step counts as settled.
    void fractionalScaleUsesKWinSteps() {
        const auto before = outputs::parseKScreenJson(kscreen(false));
        const auto unscaled = outputs::parseKScreenJson(kscreen(true, 4314, 3, 2, 1.0));
        QCOMPARE(outputs::repairArgs(before, unscaled, {{QStringLiteral("Virtual-remora-1"), 0.68125}}),
                 QStringList{QStringLiteral("output.Virtual-remora-1.scale.0.683333")});
        const auto stored = outputs::parseKScreenJson(kscreen(true, 4314, 3, 2, 82 / 120.0));
        QVERIFY(outputs::repairArgs(before, stored, {{QStringLiteral("Virtual-remora-1"), 0.68125}})
                    .isEmpty());
    }

    // Two phones: the second parks after the first, one priority lower.
    void scaleFitsTheAppsMinimum() {
        // Elisa (minimum 612x459) on a Pixel 10 Pro XL at its own 480 dpi, scale 3: 448x997
        // logical upright, 997x448 sideways, and KWin kept her on the desk.
        const QSize elisa(612, 459);
        const double upright = outputs::fitScale({1344, 2992}, elisa);
        const double sideways = outputs::fitScale({2992, 1344}, elisa);
        QCOMPARE(upright, 263 / 120.0);  // 1344 / 612 = 2.196, down to a step
        QCOMPARE(sideways, 351 / 120.0);  // 1344 / 459 = 2.928
        QVERIFY(1344 / upright >= 612 && 1344 / sideways >= 459);
        // Nothing to fit, or no output: no limit.
        QVERIFY(std::isinf(outputs::fitScale({1344, 2992}, {})));
        QVERIFY(std::isinf(outputs::fitScale({}, elisa)));
        // A desktop-sized minimum is not followed below half scale.
        QCOMPARE(outputs::fitScale({1344, 2992}, {3000, 2000}), 0.5);
    }

    void secondVirtualOutputStacks() {
        const auto before = outputs::parseKScreenJson(kscreen(false));
        QByteArray two = kscreen(true, 4314, 3, 2, 2.625);
        two.chop(2);  // "]}"
        two += R"(,{"name":"Virtual-remora-2","connected":true,"pos":{"x":0,"y":0},"scale":1,
                  "priority":4,"rotation":1,"currentModeId":"0",
                  "modes":[{"id":"0","size":{"width":1200,"height":2000}}]}]})";
        const QStringList args = outputs::repairArgs(
            before, outputs::parseKScreenJson(two),
            {{QStringLiteral("Virtual-remora-1"), 2.625}, {QStringLiteral("Virtual-remora-2"), 2}});
        QCOMPARE(args, (QStringList{QStringLiteral("output.Virtual-remora-2.position.4726,0"),
                                    QStringLiteral("output.Virtual-remora-2.scale.2")}));
    }

    // A mirrored desk: the second monitor shows the first's picture. Its stored position and
    // priority are not what is on screen, and setting them would end the mirroring — so the repair
    // never names it, and it does not widen the desk the virtual output is parked beside.
    void mirroredOutputIsLeftAlone() {
        auto mirrored = [](QByteArray json) {
            json.replace(R"("name":"HDMI-A-1","enabled":true,"connected":true,)",
                         R"("name":"HDMI-A-1","enabled":true,"connected":true,"replicationSource":7,)");
            return json;
        };
        const auto before = outputs::parseKScreenJson(mirrored(kscreen(false)));
        QCOMPARE(before[1].replicationSource, 7);
        // The virtual output where the old repair put it: right of the mirror's stored geometry.
        QByteArray moved = mirrored(kscreen(true, 4314, 3, 2, 1.0));
        moved.replace(R"("pos":{"x":474,"y":1011})", R"("pos":{"x":0,"y":0})");
        const QStringList args = outputs::repairArgs(before, outputs::parseKScreenJson(moved),
                                                     {{QStringLiteral("Virtual-remora-1"), 1.0}});
        for (const QString &a : args) QVERIFY2(!a.contains(QStringLiteral("HDMI-A-1")), qPrintable(a));
        // Parked right of DP-1 alone (1920 logical wide), not of the mirror's stored geometry.
        QVERIFY(args.contains(QStringLiteral("output.Virtual-remora-1.position.1920,0")));
    }

    // Not listed yet (KWin announces the output a moment after the stream): skipped, not guessed.
    void unlistedVirtualOutputIsSkipped() {
        const auto before = outputs::parseKScreenJson(kscreen(false));
        QVERIFY(outputs::repairArgs(before, before, {{QStringLiteral("Virtual-remora-1"), 2}})
                    .isEmpty());
    }

    // --- video memory --------------------------------------------------------------------------

    void readsVram() {
        const auto nv = gpumem::parseNvidiaSmi(QStringLiteral("5817, 8192\n"));
        QVERIFY(nv);
        QCOMPARE(nv->freeMiB(), qint64(2375));
        QVERIFY(!gpumem::parseNvidiaSmi(QStringLiteral("[N/A], 8192")));
        QVERIFY(!gpumem::parseNvidiaSmi(QString()));
        const auto amd = gpumem::fromAmdgpuBytes(QStringLiteral("1073741824\n"),
                                                 QStringLiteral("17163091968\n"));
        QVERIFY(amd);
        QCOMPARE(amd->usedMiB, qint64(1024));
        QCOMPARE(amd->totalMiB, qint64(16368));
        QCOMPARE(gpumem::nvidiaBusId(QStringLiteral("0000:01:00.0")),
                 QStringLiteral("00000000:01:00.0"));
    }

    // The measured case: an 8 GB card at 6.9 GB used, where a monitor was left without scanout
    // memory, refuses a phone session; the same card at its everyday 5.8 GB does not.
    void refusesWhenTheDeskWouldStarve() {
        const qint64 need = gpumem::sessionNeedMiB({1080, 2400}, 1024);
        QCOMPARE(need, qint64(640 + 40 + 1024));
        const QString no = gpumem::vramRefusal({6901, 8192}, need);
        QVERIFY(no.contains(QStringLiteral("1291 MiB")));
        QVERIFY(gpumem::vramRefusal({5817, 8192}, need).isEmpty());
    }

    // --- the app list --------------------------------------------------------------------------

    void desktopFileIds() {
        QCOMPARE(desktop::desktopFileId(QStringLiteral("org.kde.dolphin.desktop")),
                 QStringLiteral("org.kde.dolphin"));
        QCOMPARE(desktop::desktopFileId(QStringLiteral("kde4/konsole.desktop")),
                 QStringLiteral("kde4-konsole"));
    }

    void parsesAnEntry() {
        const QString text = QStringLiteral(
            "# comment\n[Desktop Entry]\nType=Application\nName=Files\nName[de]=Dateien\n"
            "Name[sr@latin]=Datoteke\nIcon=system-file-manager\nExec=dolphin %u\n"
            "[Desktop Action new]\nName=New Window\nExec=dolphin --new-window\n");
        const auto e = desktop::parseDesktopEntry(QStringLiteral("org.kde.dolphin"), text,
                                                  {QStringLiteral("KDE")}, QStringLiteral("en_US"));
        QVERIFY(e);
        QCOMPARE(e->name, QStringLiteral("Files"));  // the action's Name does not leak in
        QCOMPARE(e->icon, QStringLiteral("system-file-manager"));
        QCOMPARE(desktop::parseDesktopEntry(QStringLiteral("x"), text, {}, QStringLiteral("de_DE.UTF-8"))
                     ->name,
                 QStringLiteral("Dateien"));
        QCOMPARE(desktop::parseDesktopEntry(QStringLiteral("x"), text, {}, QStringLiteral("sr_RS@latin"))
                     ->name,
                 QStringLiteral("Datoteke"));
    }

    void dropsWhatALauncherHides() {
        const QString base = QStringLiteral("[Desktop Entry]\nType=Application\nName=X\nExec=x\n");
        const QStringList kde{QStringLiteral("KDE")};
        const QString en = QStringLiteral("en");
        QVERIFY(desktop::parseDesktopEntry(QStringLiteral("x"), base, kde, en));
        QVERIFY(!desktop::parseDesktopEntry(QStringLiteral("x"), base + "NoDisplay=true\n", kde, en));
        QVERIFY(!desktop::parseDesktopEntry(QStringLiteral("x"), base + "Hidden=true\n", kde, en));
        QVERIFY(!desktop::parseDesktopEntry(QStringLiteral("x"), base + "OnlyShowIn=GNOME;\n", kde, en));
        QVERIFY(desktop::parseDesktopEntry(QStringLiteral("x"), base + "OnlyShowIn=GNOME;KDE;\n", kde, en));
        QVERIFY(!desktop::parseDesktopEntry(QStringLiteral("x"), base + "NotShowIn=KDE;\n", kde, en));
        QString link = base;
        link.replace(QStringLiteral("Type=Application"), QStringLiteral("Type=Link"));
        QVERIFY(!desktop::parseDesktopEntry(QStringLiteral("x"), link, kde, en));
        QString noExec = base;
        noExec.remove(QStringLiteral("Exec=x\n"));
        QVERIFY(!desktop::parseDesktopEntry(QStringLiteral("x"), noExec, kde, en));
        QVERIFY(desktop::parseDesktopEntry(QStringLiteral("x"), noExec + "DBusActivatable=true\n",
                                           kde, en));
    }

    // The first file for an id decides — a user's Hidden copy masks the system entry — and
    // Remora's own Android launchers never appear.
    void collectsInPrecedenceOrder() {
        const QString app = QStringLiteral("[Desktop Entry]\nType=Application\nExec=a\nName=%1\n");
        const QList<QPair<QString, QString>> files{
            {QStringLiteral("org.kde.kate"), app.arg(QStringLiteral("Kate (user copy)"))},
            {QStringLiteral("htop"), app.arg(QStringLiteral("htop")) + "Hidden=true\n"},
            {QStringLiteral("remora-android-17-org.lineageos.jelly"), app.arg(QStringLiteral("Browser"))},
            {QStringLiteral("org.kde.kate"), app.arg(QStringLiteral("Kate"))},
            {QStringLiteral("htop"), app.arg(QStringLiteral("htop"))},
            {QStringLiteral("firefox"), app.arg(QStringLiteral("firefox"))},
        };
        const auto apps = desktop::collectApps(files, {QStringLiteral("KDE")}, QStringLiteral("en"));
        QCOMPARE(apps.size(), 2);
        QCOMPARE(apps[0].id, QStringLiteral("firefox"));  // case-insensitive name order
        QCOMPARE(apps[1].name, QStringLiteral("Kate (user copy)"));
        QCOMPARE(desktop::listAppsBlob(apps),
                 QByteArray(" - firefox  firefox\n - Kate (user copy)  org.kde.kate\n"));
    }

    // --- protocol v3, the agent's side ---------------------------------------------------------

    void hellos() {
        const auto client = mirror::parseClientHello(mirror::agentHello());
        QVERIFY(client);
        QCOMPARE(client->version, mirror::kAgentProtoVersion);
        const auto reply = mirror::parseAgentHello(mirror::agentHelloReply(0x0001));
        QVERIFY(reply);
        QCOMPARE(reply->capabilities, quint16(1));
        QVERIFY(mirror::agentVersionRefusal(*reply).isEmpty());
        QVERIFY(!mirror::parseClientHello("GET / HTTP/1.1\r\n"));
    }

    void sessionRequests() {
        const QByteArray req = mirror::agentSessionRequest(
            mirror::SessionKind::NewDisplay,
            {QStringLiteral("new_display=1080x2400/420"), QStringLiteral("start_app=org.kde.kate"),
             QStringLiteral("close_on_app_exit=true")});
        int consumed = 0;
        QString error;
        // Incremental: nothing until the whole option block is in.
        QVERIFY(!mirror::parseConnectionRequest(req.left(req.size() - 1), &consumed, &error));
        QVERIFY(error.isEmpty());
        const auto r = mirror::parseConnectionRequest(req + "trailing", &consumed, &error);
        QVERIFY(r);
        QCOMPARE(consumed, req.size());
        QCOMPARE(r->role, mirror::ConnRole::Control);
        QCOMPARE(r->kind, mirror::SessionKind::NewDisplay);
        QCOMPARE(r->options.value(QStringLiteral("new_display")), QStringLiteral("1080x2400/420"));
        QCOMPARE(r->options.value(QStringLiteral("start_app")), QStringLiteral("org.kde.kate"));

        const auto v = mirror::parseConnectionRequest(mirror::agentVideoAttach(7), &consumed, &error);
        QVERIFY(v);
        QCOMPARE(v->role, mirror::ConnRole::Video);
        QCOMPARE(v->sessionId, quint32(7));
        QCOMPARE(mirror::parseConnectionRequest(mirror::agentAudioAttach(9), &consumed, &error)->role,
                 mirror::ConnRole::Audio);

        QVERIFY(!mirror::parseConnectionRequest(QByteArray("\x05", 1), &consumed, &error));
        QVERIFY(error.contains(QStringLiteral("role")));
        error.clear();
        const auto icon = mirror::parseConnectionRequest(
            mirror::agentSessionRequest(mirror::SessionKind::AppIcon,
                                        {QStringLiteral("app=org.kde.kcalc"), QStringLiteral("size=96")}),
            &consumed, &error);
        QVERIFY(icon);
        QCOMPARE(icon->kind, mirror::SessionKind::AppIcon);
        QCOMPARE(icon->options.value(QStringLiteral("size")), QStringLiteral("96"));

        QVERIFY(!mirror::parseConnectionRequest(QByteArray("\x00\x09\x00\x00", 4), &consumed, &error));
        QVERIFY(error.contains(QStringLiteral("kind")));
    }

    void sessionReplies() {
        int consumed = 0;
        const auto ok = mirror::parseSessionReply(mirror::sessionAccepted(42), &consumed);
        QVERIFY(ok);
        QCOMPARE(ok->status, quint8(0));
        QCOMPARE(ok->sessionId, quint32(42));
        const auto no = mirror::parseSessionReply(
            mirror::sessionRefused(QStringLiteral("no app org.kde.nothing")), &consumed);
        QVERIFY(no);
        QVERIFY(no->status != 0);
        QCOMPARE(no->reason, QStringLiteral("no app org.kde.nothing"));

        const QByteArray apps = mirror::listAppsReply(" - Kate  org.kde.kate\n");
        QCOMPARE(apps.left(5), mirror::sessionAccepted(0));
        QCOMPARE(qFromBigEndian<quint32>(apps.constData() + 5), quint32(22));
        QCOMPARE(apps.mid(9), QByteArray(" - Kate  org.kde.kate\n"));
    }

    // The device messages, read back by the client's own parser.
    void deviceMessages() {
        mirror::DeviceMessageParser p;
        p.feed(mirror::deviceClipboardRecord(QStringLiteral("from the desk")));
        p.feed(mirror::clipboardAckRecord(7));
        auto m = p.next();
        QCOMPARE(m->type, mirror::DeviceRecord::Clipboard);
        QCOMPARE(m->clipboardText, QStringLiteral("from the desk"));
        m = p.next();
        QCOMPARE(m->type, mirror::DeviceRecord::ClipboardAck);
        QCOMPARE(m->sequence, quint64(7));
        QVERIFY(!p.error());
    }

    // An audio stream as the host agent writes one, read by the client's demuxer.
    void audioStreamRoundTrips() {
        mirror::StreamDemuxer demux(mirror::StreamDemuxer::Kind::Audio);
        demux.feed(mirror::streamStartRecord(mirror::kCodecOpus));
        demux.feed(mirror::streamConfigRecord("OpusHead-ish"));
        demux.feed(mirror::streamFrameRecord(20000, false, "packet"));
        QCOMPARE(demux.next(), mirror::StreamDemuxer::Event::Started);
        QCOMPARE(demux.codec(), mirror::kCodecOpus);
        QCOMPARE(demux.next(), mirror::StreamDemuxer::Event::Packet);
        auto p = demux.takePacket();
        QVERIFY(p.config);
        QCOMPARE(p.payload, QByteArray("OpusHead-ish"));
        QCOMPARE(demux.next(), mirror::StreamDemuxer::Event::Packet);
        p = demux.takePacket();
        QVERIFY(!p.config);
        QCOMPARE(p.ptsUs, std::optional<quint64>(20000));
        QCOMPARE(p.payload, QByteArray("packet"));
        QCOMPARE(demux.next(), mirror::StreamDemuxer::Event::NeedMoreData);
    }

    // An END in place of START is the client's "Failed" with the agent's own words.
    void streamEndReachesTheClient() {
        mirror::StreamDemuxer demux(mirror::StreamDemuxer::Kind::Video);
        demux.feed(mirror::streamEndRecord(mirror::kEndFailed, QStringLiteral("no encoder")));
        QCOMPARE(demux.next(), mirror::StreamDemuxer::Event::Failed);
        QCOMPARE(demux.detail(), QStringLiteral("no encoder"));
    }

    void controlMessagesRoundTrip() {
        mirror::ControlMessageParser p;
        p.feed(mirror::pointerMessage(0, mirror::PointerTool::Mouse, 3, {0.25f, 0.75f}, 1.0f, 1, 1));
        p.feed(mirror::keyMessage(1, 66, 2, 0x1000));
        p.feed(mirror::scrollMessage({0.5f, 0.5f}, 0.0f, -1.0f, 0));
        p.feed(mirror::record(0x7F, "a type from the future"));  // skipped
        p.feed(mirror::textMessage(QStringLiteral("héllo")));
        p.feed(mirror::clipboardMessage(5, true, QStringLiteral("copied")));
        p.feed(mirror::resizeMessage(1080, 2400));
        p.feed(mirror::resizeMessage(1280, 678, 109));
        p.feed(mirror::backMessage(0));

        auto m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Pointer);
        QCOMPARE(m->tool, mirror::PointerTool::Mouse);
        QCOMPARE(m->pointerId, quint32(3));
        QCOMPARE(m->at.x, 0.25f);
        QCOMPARE(m->at.y, 0.75f);
        QCOMPARE(m->buttons, 1);
        m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Key);
        QCOMPARE(m->keycode, 66);
        QCOMPARE(m->repeat, quint32(2));
        QCOMPARE(m->metaState, 0x1000);
        m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Scroll);
        QCOMPARE(m->vScroll, -1.0f);
        m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Text);
        QCOMPARE(m->text, QStringLiteral("héllo"));
        m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Clipboard);
        QCOMPARE(m->sequence, quint64(5));
        QVERIFY(m->paste);
        QCOMPARE(m->text, QStringLiteral("copied"));
        m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Resize);
        QCOMPARE(m->width, quint16(1080));
        QCOMPARE(m->height, quint16(2400));
        QCOMPARE(m->dpi, quint16(0));  // the density follows the long side
        m = p.next();
        QCOMPARE(m->type, mirror::ControlRecord::Resize);
        QCOMPARE(m->width, quint16(1280));
        QCOMPARE(m->dpi, quint16(109));
        QCOMPARE(p.next()->type, mirror::ControlRecord::Back);
        QVERIFY(!p.next());
        QVERIFY(!p.error());
    }

    void shortControlBodyIsAFramingError() {
        mirror::ControlMessageParser p;
        p.feed(mirror::record(quint8(mirror::ControlRecord::Pointer), QByteArray(10, '\0')));
        QVERIFY(!p.next());
        QVERIFY(p.error());
    }

    // --- input ---------------------------------------------------------------------------------

    void keysMapToEvdev() {
        QCOMPARE(hostinput::evdevKey(29), std::optional<quint32>(30));   // A → KEY_A
        QCOMPARE(hostinput::evdevKey(54), std::optional<quint32>(44));   // Z → KEY_Z
        QCOMPARE(hostinput::evdevKey(7), std::optional<quint32>(11));    // 0 → KEY_0
        QCOMPARE(hostinput::evdevKey(8), std::optional<quint32>(2));     // 1 → KEY_1
        QCOMPARE(hostinput::evdevKey(66), std::optional<quint32>(28));   // ENTER
        QCOMPARE(hostinput::evdevKey(113), std::optional<quint32>(29));  // CTRL_LEFT
        QCOMPARE(hostinput::evdevKey(142), std::optional<quint32>(88));  // F12
        QVERIFY(!hostinput::evdevKey(3));  // HOME: the phone's, not the app's
    }

    void textBecomesKeysyms() {
        QCOMPARE(hostinput::keysymFor(U'a'), std::optional<quint32>(0x61));
        QCOMPARE(hostinput::keysymFor(U'é'), std::optional<quint32>(0xe9));
        QCOMPARE(hostinput::keysymFor(U'€'), std::optional<quint32>(0x010020ac));
        QCOMPARE(hostinput::keysymFor(U'\n'), std::optional<quint32>(0xff0d));
        QVERIFY(!hostinput::keysymFor(U'\x01'));
        hostinput::Translator t;
        mirror::ControlMessage m;
        m.type = mirror::ControlRecord::Text;
        m.text = QStringLiteral("hi");
        const auto ops = t.translate(m);
        QCOMPARE(ops.size(), 4);
        QCOMPARE(ops[0], (hostinput::InputOp{hostinput::InputOp::Keysym, {}, 0x68, true}));
        QCOMPARE(ops[1], (hostinput::InputOp{hostinput::InputOp::Keysym, {}, 0x68, false}));
    }

    // Keys reach only the session's window: while another has focus they wait, in order, and a
    // touch passes (it focuses the app by itself).
    void keysWaitForTheSessionsWindow() {
        using hostinput::InputOp;
        using Ops = QList<InputOp>;
        hostinput::KeyGate g;
        const InputOp touch{InputOp::TouchDown, {10, 10}, 0};
        const InputOp aDown{InputOp::Key, {}, 30, true}, aUp{InputOp::Key, {}, 30, false};
        const InputOp sym{InputOp::Keysym, {}, 0x35, true};
        QCOMPARE(g.pass({touch, aDown, aUp, sym}), Ops{touch});
        QVERIFY(g.holding());
        QCOMPARE(g.focusChanged(true), (Ops{aDown, aUp, sym}));
        QVERIFY(!g.holding());
        QCOMPARE(g.pass({aDown, aUp}), (Ops{aDown, aUp}));
        // Focus lost: nothing typed, and nothing typed later when it never comes back.
        QCOMPARE(g.focusChanged(false), Ops{});
        QCOMPARE(g.pass({aDown, aUp}), Ops{});
        QCOMPARE(g.expire(), Ops{});
        QVERIFY(!g.holding());
        QCOMPARE(g.focusChanged(true), Ops{});
    }

    // A key pressed into the session's window is released wherever focus went, so it cannot stay
    // down in the seat — unless a held key is ahead of it: a modifier's release must not overtake
    // the key it modifies. If focus never returns, only that release comes back.
    void keyGateReleasesWhatItPressed() {
        using hostinput::InputOp;
        using Ops = QList<InputOp>;
        const InputOp ctrlDown{InputOp::Key, {}, 29, true}, ctrlUp{InputOp::Key, {}, 29, false};
        const InputOp vDown{InputOp::Key, {}, 47, true}, vUp{InputOp::Key, {}, 47, false};
        hostinput::KeyGate g;
        g.focusChanged(true);
        QCOMPARE(g.pass({ctrlDown}), Ops{ctrlDown});
        g.focusChanged(false);
        QCOMPARE(g.pass({ctrlUp}), Ops{ctrlUp});
        QCOMPARE(g.focusChanged(true), Ops{});
        QCOMPARE(g.pass({ctrlDown}), Ops{ctrlDown});
        g.focusChanged(false);
        QCOMPARE(g.pass({vDown, vUp, ctrlUp}), Ops{});
        QCOMPARE(g.focusChanged(true), (Ops{vDown, vUp, ctrlUp}));
        QCOMPARE(g.pass({ctrlDown}), Ops{ctrlDown});
        g.focusChanged(false);
        QCOMPARE(g.pass({vDown, vUp, ctrlUp}), Ops{});
        QCOMPARE(g.expire(), Ops{ctrlUp});
    }

#ifdef REMORA_TEST_XKB
    // Typed text: the real modifier keys held around a keysym, for the key and level KWin picks.
    // Shift is evdev 42 and AltGr 100 on these layouts; a keysym the layout lacks needs nothing
    // (KWin types it on a spare key).
    void keysymsNeedTheirModifierKeys() {
        using Keys = QList<quint32>;
        hostagent::DeskKeymap us;
        if (!us.loadNames("us")) QSKIP("no xkeyboard-config data to compile a keymap from");
        QCOMPARE(us.modifierKeysFor('p'), Keys{});
        QCOMPARE(us.modifierKeysFor('P'), Keys{42});
        QCOMPARE(us.modifierKeysFor('*'), Keys{42});
        QCOMPARE(us.modifierKeysFor('_'), Keys{42});
        QCOMPARE(us.modifierKeysFor('7'), Keys{});
        QCOMPARE(us.modifierKeysFor(0x20ac), Keys{});  // EuroSign: not on us
        hostagent::DeskKeymap de;
        QVERIFY(de.loadNames("de"));
        QCOMPARE(de.modifierKeysFor('@'), Keys{100});      // AltGr+Q
        QCOMPARE(de.modifierKeysFor(0x20ac), Keys{100});   // AltGr+E
        QCOMPARE(de.modifierKeysFor(0xd6), Keys{42});      // Odiaeresis
        QCOMPARE(de.modifierKeysFor('y'), Keys{});
        hostagent::DeskKeymap eu;  // EurKEY: the accented letters on AltGr, their capitals on both
        QVERIFY(eu.loadNames("eu"));
        QCOMPARE(eu.modifierKeysFor(0xe4), Keys{100});      // adiaeresis
        QCOMPARE(eu.modifierKeysFor(0xc4), (Keys{42, 100}));  // Adiaeresis
        QCOMPARE(eu.modifierKeysFor('A'), Keys{42});
        // Text spells the Euro sign as Unicode; a layout that has it lists the named EuroSign.
        constexpr quint32 euroUnicode = 0x010020ac, euroNamed = 0x20ac;
        QCOMPARE(eu.spelledOn(euroUnicode), euroNamed);
        QCOMPARE(eu.modifierKeysFor(euroNamed), Keys{100});  // AltGr+5
        QCOMPARE(de.spelledOn(euroUnicode), euroNamed);
        constexpr quint32 zheUnicode = 0x01000436;  // Cyrillic zhe: not on us, left for KWin's spare key
        QCOMPARE(us.spelledOn(zheUnicode), zheUnicode);
        QCOMPARE(us.spelledOn('a'), quint32('a'));
    }
#endif

    // The desktop mirror's mouse: left as DOWN/UP, the rest as BUTTON_PRESS/RELEASE, hover as
    // HOVER_MOVE with no buttons — placed on the output at its position on the desk.
    void mouseDrivesThePointer() {
        hostinput::Translator t;
        t.setOutput(QRectF(4314, 0, 412, 915));
        mirror::ControlMessage m;
        m.type = mirror::ControlRecord::Pointer;
        m.tool = mirror::PointerTool::Mouse;
        m.at = {0.5f, 0.0f};
        m.action = 0;  // DOWN
        m.actionButton = 1;
        m.buttons = 1;
        auto ops = t.translate(m);
        QCOMPARE(ops.size(), 2);
        QCOMPARE(ops[0].kind, hostinput::InputOp::PointerMotion);
        QCOMPARE(ops[0].at, QPointF(4314 + 206, 0));
        QCOMPARE(ops[1], (hostinput::InputOp{hostinput::InputOp::PointerButton, {}, 0x110, true}));
        m.action = 2;  // MOVE, still held: no button change
        QCOMPARE(t.translate(m).size(), 1);
        m.action = 11;  // BUTTON_PRESS secondary
        m.actionButton = 2;
        m.buttons = 3;
        ops = t.translate(m);
        QCOMPARE(ops.last(), (hostinput::InputOp{hostinput::InputOp::PointerButton, {}, 0x111, true}));
        m.action = 7;  // HOVER_MOVE with nothing held: both go up, in order
        m.actionButton = 0;
        m.buttons = 0;
        ops = t.translate(m);
        QCOMPARE(ops.size(), 3);
        QCOMPARE(ops[1], (hostinput::InputOp{hostinput::InputOp::PointerButton, {}, 0x110, false}));
        QCOMPARE(ops[2], (hostinput::InputOp{hostinput::InputOp::PointerButton, {}, 0x111, false}));
    }

    void fingersAreTouches() {
        hostinput::Translator t;
        t.setOutput(QRectF(0, 0, 100, 200));
        mirror::ControlMessage m;
        m.type = mirror::ControlRecord::Pointer;
        m.tool = mirror::PointerTool::Finger;
        m.pointerId = 4;
        m.at = {0.1f, 0.5f};
        m.action = 0;
        auto ops = t.translate(m);
        QCOMPARE(ops.size(), 2);
        QCOMPARE(ops[0].kind, hostinput::InputOp::TouchDown);
        QCOMPARE(ops[0].code, quint32(4));
        QCOMPARE(ops[1].kind, hostinput::InputOp::TouchFrame);
        // A session ending mid-touch lifts the finger and any key still down.
        mirror::ControlMessage key;
        key.type = mirror::ControlRecord::Key;
        key.keycode = 59;  // SHIFT_LEFT
        t.translate(key);
        ops = t.releaseAll();
        QCOMPARE(ops.size(), 3);
        QCOMPARE(ops[0].kind, hostinput::InputOp::TouchUp);
        QCOMPARE(ops[2], (hostinput::InputOp{hostinput::InputOp::Key, {}, 42, false}));
        QVERIFY(t.releaseAll().isEmpty());
        m.action = 2;  // a move for a finger that is no longer down
        QVERIFY(t.translate(m).isEmpty());
    }

    void wheelScrollsUnderThePointer() {
        hostinput::Translator t;
        t.setOutput(QRectF(0, 0, 100, 100));
        mirror::ControlMessage m;
        m.type = mirror::ControlRecord::Scroll;
        m.at = {0.5f, 0.5f};
        m.vScroll = 1.0f;  // one notch up
        const auto ops = t.translate(m);
        QCOMPARE(ops.size(), 2);
        QCOMPARE(ops[1].value, -15.0);
        QVERIFY(!ops[1].horizontal);

        // Sideways, Android and Wayland agree: a notch right is positive in both.
        m.vScroll = 0;
        m.hScroll = 1.0f;
        const auto right = t.translate(m);
        QCOMPARE(right.size(), 2);
        QVERIFY(right[1].horizontal);
        QCOMPARE(right[1].value, 15.0);
    }
};

QTEST_GUILESS_MAIN(TestHostAgent)
#include "test_hostagent.moc"
