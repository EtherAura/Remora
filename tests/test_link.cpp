// The link's wire format and records (core/LinkProto, docs/LINK_PROTOCOL.md): what a phone sends
// and receives before a connection becomes a source's.
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSslKey>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "core/LinkProto.h"
#include "engine/Link.h"

using namespace remora;

class TestLink : public QObject {
    Q_OBJECT
private slots:
    void requestsRoundTrip() {
        int consumed = 0;
        QString error;
        const QByteArray pair = link::pairRequest(QStringLiteral("a1b2c3"), QStringLiteral("Pixel 9"));
        // Incremental: nothing until the last field is in.
        QVERIFY(!link::parseRequest(pair.left(pair.size() - 1), &consumed, &error));
        QVERIFY(error.isEmpty());
        auto r = link::parseRequest(pair + "rest", &consumed, &error);
        QVERIFY(r);
        QCOMPARE(consumed, pair.size());
        QCOMPARE(r->kind, link::RequestKind::Pair);
        QCOMPARE(r->token, QStringLiteral("a1b2c3"));
        QCOMPARE(r->deviceName, QStringLiteral("Pixel 9"));

        r = link::parseRequest(link::sourcesRequest(), &consumed, &error);
        QCOMPARE(r->kind, link::RequestKind::Sources);
        r = link::parseRequest(link::connectRequest(QStringLiteral("android:Android 17")), &consumed,
                               &error);
        QCOMPARE(r->kind, link::RequestKind::Connect);
        QCOMPARE(r->sourceId, QStringLiteral("android:Android 17"));
    }

    void refusesWhatIsNotALinkRequest() {
        int consumed = 0;
        QString error;
        // Protocol v3's hello sent straight at the link: refused on its first bytes.
        QVERIFY(!link::parseRequest("RMRA", &consumed, &error));
        QVERIFY(!error.isEmpty());
        error.clear();
        QByteArray future = link::sourcesRequest();
        future[5] = 9;  // version 9
        QVERIFY(!link::parseRequest(future, &consumed, &error));
        QVERIFY(error.contains(QStringLiteral("v9")));
        error.clear();
        QByteArray unknown = link::sourcesRequest();
        unknown[6] = 42;
        QVERIFY(!link::parseRequest(unknown, &consumed, &error));
        QVERIFY(error.contains(QStringLiteral("42")));
    }

    void replies() {
        int consumed = 0;
        const QByteArray ok = link::okReply("payload");
        QVERIFY(!link::parseReply(ok.left(ok.size() - 1), &consumed));
        auto r = link::parseReply(ok + "x", &consumed);
        QVERIFY(r && r->ok);
        QCOMPARE(r->payload, QByteArray("payload"));
        QCOMPARE(consumed, ok.size());
        r = link::parseReply(link::refusedReply(QStringLiteral("not paired")), &consumed);
        QVERIFY(r && !r->ok);
        QCOMPARE(QString::fromUtf8(r->payload), QStringLiteral("not paired"));
        QCOMPARE(link::parseReply(link::okReply(), &consumed)->payload, QByteArray());
    }

    void sources() {
        const QList<link::Source> s{
            {link::kComputerSourceId, QStringLiteral("This computer"), QStringLiteral("computer")},
            {link::androidSourceId(QStringLiteral("Android 17")), QStringLiteral("Android 17"),
             QStringLiteral("android")}};
        QCOMPARE(link::parseSourcesJson(link::sourcesJson(s)), s);
        QCOMPARE(link::profileFromSourceId(QStringLiteral("android:Android 17")),
                 std::optional<QString>(QStringLiteral("Android 17")));
        QVERIFY(!link::profileFromSourceId(QStringLiteral("android:")));
        QVERIFY(!link::profileFromSourceId(link::kComputerSourceId));
    }

    void pairingUriRoundTrips() {
        link::PairingInfo info;
        info.addresses = {QStringLiteral("192.168.0.10:27200"), QStringLiteral("[fd00::1]:27200")};
        info.fingerprint = QString(64, QLatin1Char('a'));
        info.token = QStringLiteral("0f1e2d3c");
        info.hostName = QStringLiteral("Desk & Den");
        const QString uri = link::pairingUri(info);
        QVERIFY(uri.startsWith(QStringLiteral("remora-link://pair?")));
        const auto back = link::parsePairingUri(uri);
        QVERIFY(back);
        QCOMPARE(back->addresses, info.addresses);
        QCOMPARE(back->fingerprint, info.fingerprint);
        QCOMPARE(back->token, info.token);
        QCOMPARE(back->hostName, info.hostName);
        QVERIFY(!link::parsePairingUri(QStringLiteral("https://pair?h=x&fp=aa&t=1")));
        QVERIFY(!link::parsePairingUri(QStringLiteral("remora-link://pair?h=x:1&fp=short&t=1")));
    }

    void fingerprints() {
        QCOMPARE(link::fingerprintOf("abc"),
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }

    // The whole link in one process: real TLS, a phone certificate, and a stand-in agent behind
    // the "computer" source that echoes what reaches it. Pins what a phone sees before and after
    // pairing, that a pairing code works once, and that a connected connection is the source's.
    void serverEndToEnd() {
        if (!QSslSocket::supportsSsl()) QSKIP("no TLS backend");
        QTemporaryDir dir;
        QProcess openssl;
        openssl.setWorkingDirectory(dir.path());
        openssl.start(QStringLiteral("openssl"),
                      {"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                       "-nodes", "-keyout", "phone.key", "-out", "phone.crt", "-days", "1", "-subj",
                       "/CN=test phone"});
        if (!openssl.waitForFinished(30000) || openssl.exitCode() != 0) QSKIP("no openssl command");

        QLocalServer agent;
        const QString agentPath = dir.filePath(QStringLiteral("agent.sock"));
        QVERIFY(agent.listen(agentPath));
        connect(&agent, &QLocalServer::newConnection, this, [&agent] {
            QLocalSocket *s = agent.nextPendingConnection();
            connect(s, &QLocalSocket::readyRead, s, [s] { s->write("ECHO:" + s->readAll()); });
        });

        link::SourceProvider sources;
        sources.hostAgentSocket = agentPath;
        sources.list = [] {
            return QList<link::Source>{{link::kComputerSourceId, QStringLiteral("desk"),
                                        QStringLiteral("computer")}};
        };
        sources.adbTarget = [](const QString &, QString *e) {
            *e = QStringLiteral("no devices in a test");
            return QString();
        };
        link::LinkServer server(sources);
        QString why;
        QVERIFY2(server.listen(0, &why), qPrintable(why));
        // The suites run offline (no address but loopback), and the phone below dials 127.0.0.1.
        const auto uri = link::beginPairing(
            server.port(), &why, {QStringLiteral("127.0.0.1:%1").arg(server.port())});
        QVERIFY2(uri, qPrintable(why));
        const auto info = link::parsePairingUri(*uri);
        QVERIFY(info);

        QFile crt(dir.filePath(QStringLiteral("phone.crt"))), key(dir.filePath(QStringLiteral("phone.key")));
        QVERIFY(crt.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly));
        const QSslCertificate phoneCert(crt.readAll());
        const QSslKey phoneKey(key.readAll(), QSsl::Ec);
        // One request on its own connection, as a phone makes them; the reply, and whatever the
        // connection carries after it.
        auto ask = [&](const QByteArray &request, const QByteArray &after = {}) {
            QSslSocket phone;
            phone.setLocalCertificate(phoneCert);
            phone.setPrivateKey(phoneKey);
            phone.setPeerVerifyMode(QSslSocket::VerifyNone);  // pinned below instead
            phone.connectToHostEncrypted(QStringLiteral("127.0.0.1"), server.port());
            [&] { QTRY_VERIFY(phone.isEncrypted()); }();
            [&] { QCOMPARE(link::fingerprintOf(phone.peerCertificate().toDer()), info->fingerprint); }();
            phone.write(request + after);
            QByteArray buf;
            std::optional<link::Reply> reply;
            int consumed = 0;
            [&] {
                QTRY_VERIFY((buf += phone.readAll(), reply = link::parseReply(buf, &consumed)).has_value());
            }();
            if (!after.isEmpty())
                [&] { QTRY_VERIFY((buf += phone.readAll()).size() >= consumed + 5 + after.size()); }();
            return std::make_pair(*reply, buf.mid(consumed));
        };

        auto [unpaired, _1] = ask(link::sourcesRequest());
        QVERIFY(!unpaired.ok);
        QVERIFY(unpaired.payload.contains("not paired"));
        auto [paired, _2] = ask(link::pairRequest(info->token, QStringLiteral("Test phone")));
        QVERIFY2(paired.ok, paired.payload.constData());
        QCOMPARE(paired.payload, QByteArray("Test phone"));
        auto [again, _3] = ask(link::pairRequest(info->token, QStringLiteral("Test phone")));
        QVERIFY(!again.ok);
        auto [list, _4] = ask(link::sourcesRequest());
        QVERIFY(list.ok);
        QCOMPARE(link::parseSourcesJson(list.payload).value(0).id, link::kComputerSourceId);
        // Connected, and the hello sent behind the request reaches the agent untouched.
        auto [connected, carried] = ask(link::connectRequest(link::kComputerSourceId), "RMRA-hello");
        QVERIFY(connected.ok);
        QCOMPARE(carried, QByteArray("ECHO:RMRA-hello"));
        auto [unknown, _5] = ask(link::connectRequest(QStringLiteral("android:nope")));
        QVERIFY(!unknown.ok);
        QCOMPARE(link::pairedDevices().size(), 1);
        QVERIFY(link::forgetDevice(QStringLiteral("Test phone")));
    }

    void devices() {
        const QList<link::Device> d{{QStringLiteral("Pixel"), QString(64, QLatin1Char('b')), 1700000000000}};
        QCOMPARE(link::parseDevicesJson(link::devicesJson(d)), d);
        QVERIFY(link::parseDevicesJson(R"([{"name":"x","fingerprint":"short"}])").isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLink)
#include "test_link.moc"
