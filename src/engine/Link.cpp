#include "Link.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QNetworkInterface>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "core/MirrorProto.h"
#include "engine/MirrorRegistry.h"

namespace remora::link {

namespace {

constexpr qint64 kPairingLifetimeMs = 10 * 60 * 1000;
constexpr int kRequestTimeoutMs = 10'000;
// The splice's bounds: neither side reads past kSpliceHigh from the OS nor is written past it.
// Small, because what crosses the link is mostly live video, where every buffered byte is delay:
// half a megabyte is half a second of an 8 Mbit/s stream. The agent behind the splice skips
// ahead when its client falls behind, and it only sees that once these buffers are full.
constexpr qint64 kSpliceHigh = 512 << 10, kSpliceLow = 128 << 10, kSpliceChunk = 64 << 10;
constexpr int kPhoneSendBuffer = 256 << 10;

QString keyPath() { return linkDir() + QStringLiteral("/host.key"); }
QString certPath() { return linkDir() + QStringLiteral("/host.crt"); }
QString devicesPath() { return linkDir() + QStringLiteral("/devices.json"); }
QString pairingPath() { return linkDir() + QStringLiteral("/pairing.json"); }

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &data, bool privateFile) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (privateFile) f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(data);
    return f.commit();
}

QString sha256Hex(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// Every address a phone on the same network or the same VPN could reach this computer by. Not
// the container plumbing: docker bridges, veth pairs, libvirt and Remora's own macvlan shim
// carry nothing a phone can route to.
QStringList reachableAddresses(quint16 port) {
    static const QStringList skip{QStringLiteral("docker"), QStringLiteral("br-"),
                                  QStringLiteral("veth"),   QStringLiteral("virbr"),
                                  QStringLiteral("remora-shim")};
    QStringList out;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        if (std::any_of(skip.begin(), skip.end(),
                        [&](const QString &p) { return iface.name().startsWith(p); }))
            continue;
        for (const QNetworkAddressEntry &e : iface.addressEntries()) {
            const QHostAddress a = e.ip();
            if (a.isLinkLocal() || a.isLoopback()) continue;
            if (a.protocol() == QAbstractSocket::IPv4Protocol)
                out << QStringLiteral("%1:%2").arg(a.toString()).arg(port);
            else if (a.protocol() == QAbstractSocket::IPv6Protocol)
                out << QStringLiteral("[%1]:%2").arg(a.toString()).arg(port);
        }
    }
    return out;
}

void closeDevice(QIODevice *d) {
    if (auto *s = qobject_cast<QAbstractSocket *>(d)) s->disconnectFromHost();
    else if (auto *l = qobject_cast<QLocalSocket *>(d)) l->disconnectFromServer();
    else d->close();
}

void limitReadBuffer(QIODevice *d, qint64 size) {
    if (auto *s = qobject_cast<QAbstractSocket *>(d)) s->setReadBufferSize(size);
    else if (auto *l = qobject_cast<QLocalSocket *>(d)) l->setReadBufferSize(size);
}

template <typename F>
void onDisconnected(QIODevice *d, QObject *owner, F f) {
    if (auto *s = qobject_cast<QAbstractSocket *>(d))
        QObject::connect(s, &QAbstractSocket::disconnected, owner, f);
    else if (auto *l = qobject_cast<QLocalSocket *>(d))
        QObject::connect(l, &QLocalSocket::disconnected, owner, f);
}

}  // namespace

QString linkDir() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                        QStringLiteral("/remora/link");
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner);
    return dir;
}

bool ensureHostIdentity(QString *error) {
    if (QFile::exists(keyPath()) && QFile::exists(certPath())) return true;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("openssl"),
            {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
             QStringLiteral("ec"), QStringLiteral("-pkeyopt"),
             QStringLiteral("ec_paramgen_curve:prime256v1"), QStringLiteral("-nodes"),
             QStringLiteral("-keyout"), keyPath(), QStringLiteral("-out"), certPath(),
             QStringLiteral("-days"), QStringLiteral("3650"), QStringLiteral("-subj"),
             QStringLiteral("/CN=Remora link on %1").arg(QSysInfo::machineHostName())});
    if (!p.waitForFinished(30'000) || p.exitCode() != 0) {
        *error = p.error() == QProcess::FailedToStart
                     ? QStringLiteral("the link's identity is made with the openssl command, "
                                      "which is not installed")
                     : QStringLiteral("openssl could not make the link's identity: %1")
                           .arg(QString::fromLocal8Bit(p.readAll()).trimmed());
        QFile::remove(keyPath());
        QFile::remove(certPath());
        return false;
    }
    QFile::setPermissions(keyPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString hostFingerprint(QString *error) {
    const QList<QSslCertificate> certs = QSslCertificate::fromData(readFile(certPath()));
    if (certs.isEmpty()) {
        *error = QStringLiteral("no link certificate at %1").arg(certPath());
        return {};
    }
    return fingerprintOf(certs.first().toDer());
}

std::optional<QString> beginPairing(quint16 port, QString *error, const QStringList &addresses) {
    if (!ensureHostIdentity(error)) return std::nullopt;
    const QString fp = hostFingerprint(error);
    if (fp.isEmpty()) return std::nullopt;
    quint32 raw[4];
    QRandomGenerator::system()->fillRange(raw);
    const QString token = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(raw), sizeof raw).toHex());
    const QJsonObject pending{
        {QStringLiteral("tokenSha256"), sha256Hex(token.toLatin1())},
        {QStringLiteral("expiresMs"),
         double(QDateTime::currentMSecsSinceEpoch() + kPairingLifetimeMs)}};
    if (!writeFile(pairingPath(), QJsonDocument(pending).toJson(QJsonDocument::Compact), true)) {
        *error = QStringLiteral("could not write %1").arg(pairingPath());
        return std::nullopt;
    }
    PairingInfo info;
    info.addresses = addresses.isEmpty() ? reachableAddresses(port) : addresses;
    info.fingerprint = fp;
    info.token = token;
    info.hostName = QSysInfo::machineHostName();
    if (info.addresses.isEmpty()) {
        *error = QStringLiteral("this computer has no network address a phone could reach");
        return std::nullopt;
    }
    return pairingUri(info);
}

QList<Device> pairedDevices() { return parseDevicesJson(readFile(devicesPath())); }

bool forgetDevice(const QString &name) {
    QList<Device> devices = pairedDevices();
    const qsizetype before = devices.size();
    devices.removeIf([&](const Device &d) { return d.name == name; });
    return devices.size() != before && writeFile(devicesPath(), devicesJson(devices), true);
}

// --- splice ------------------------------------------------------------------------------------

void splice(QIODevice *a, QIODevice *b, QObject *owner) {
    // Bounded on both sides, so a stalled reader stalls its writer instead of filling this
    // process's memory.
    limitReadBuffer(a, kSpliceHigh);
    limitReadBuffer(b, kSpliceHigh);
    auto pump = [](QIODevice *from, QIODevice *to) {
        while (to->bytesToWrite() < kSpliceHigh && from->bytesAvailable() > 0)
            to->write(from->read(std::min(from->bytesAvailable(), kSpliceChunk)));
    };
    QObject::connect(a, &QIODevice::readyRead, owner, [=] { pump(a, b); });
    QObject::connect(b, &QIODevice::readyRead, owner, [=] { pump(b, a); });
    QObject::connect(b, &QIODevice::bytesWritten, owner, [=] {
        if (b->bytesToWrite() < kSpliceLow) pump(a, b);
    });
    QObject::connect(a, &QIODevice::bytesWritten, owner, [=] {
        if (a->bytesToWrite() < kSpliceLow) pump(b, a);
    });
    // Whichever side goes, the other follows once what is in flight has been written.
    onDisconnected(a, owner, [=] { closeDevice(b); });
    onDisconnected(b, owner, [=] { closeDevice(a); });
    pump(a, b);
    pump(b, a);
}

// --- server ------------------------------------------------------------------------------------

LinkServer::LinkServer(SourceProvider sources, QObject *parent)
    : QObject(parent), sources_(std::move(sources)) {}

LinkServer::~LinkServer() {
    for (auto it = forwards_.cbegin(); it != forwards_.cend(); ++it) {
        QProcess p;
        const QStringList argv = mirror::adbAgentTunnelArgv(QStringLiteral("adb"), it.key(),
                                                            it->port, true);
        p.start(argv.first(), argv.mid(1));
        p.waitForFinished(5000);
    }
}

bool LinkServer::listen(quint16 port, QString *error) {
    if (!QSslSocket::supportsSsl()) {
        *error = QStringLiteral("this Qt has no TLS backend");
        return false;
    }
    if (!ensureHostIdentity(error)) return false;
    const QList<QSslCertificate> certs = QSslCertificate::fromData(readFile(certPath()));
    const QSslKey key(readFile(keyPath()), QSsl::Ec, QSsl::Pem);
    if (certs.isEmpty() || key.isNull()) {
        *error = QStringLiteral("the link's identity in %1 is unreadable").arg(linkDir());
        return false;
    }
    tls_ = QSslConfiguration::defaultConfiguration();
    tls_.setLocalCertificate(certs.first());
    tls_.setPrivateKey(key);
    tls_.setProtocol(QSsl::TlsV1_3OrLater);
    // Ask for the phone's certificate without judging it by a CA: there is none. Trust is its
    // fingerprint being among the paired devices, checked per request below.
    tls_.setPeerVerifyMode(QSslSocket::QueryPeer);

    server_ = new QSslServer(this);
    server_->setSslConfiguration(tls_);
    connect(server_, &QSslServer::pendingConnectionAvailable, this, [this] {
        while (QTcpSocket *s = server_->nextPendingConnection())
            if (auto *ssl = qobject_cast<QSslSocket *>(s)) onEncrypted(ssl);
    });
    if (!server_->listen(QHostAddress::Any, port)) {
        *error = QStringLiteral("could not listen on port %1: %2").arg(port).arg(server_->errorString());
        return false;
    }
    return true;
}

quint16 LinkServer::port() const { return server_ ? server_->serverPort() : 0; }

void LinkServer::onEncrypted(QSslSocket *sock) {
    sock->setParent(this);
    pending_.insert(sock, {});
    connect(sock, &QSslSocket::readyRead, this, [this, sock] { onRequestData(sock); });
    connect(sock, &QSslSocket::disconnected, this, [this, sock] {
        if (pending_.remove(sock)) sock->deleteLater();
    });
    QTimer::singleShot(kRequestTimeoutMs, sock, [this, sock] {
        if (pending_.contains(sock)) sock->disconnectFromHost();
    });
    if (sock->bytesAvailable()) onRequestData(sock);
}

void LinkServer::onRequestData(QSslSocket *sock) {
    auto it = pending_.find(sock);
    if (it == pending_.end()) return;
    it->append(sock->readAll());
    int consumed = 0;
    QString error;
    const auto req = parseRequest(*it, &consumed, &error);
    if (!req) {
        if (!error.isEmpty()) refuse(sock, error);
        return;
    }
    const QByteArray rest = it->mid(consumed);
    pending_.erase(it);
    disconnect(sock, nullptr, this, nullptr);
    handle(sock, *req, rest);
}

void LinkServer::refuse(QSslSocket *sock, const QString &why) {
    pending_.remove(sock);
    disconnect(sock, nullptr, this, nullptr);
    sock->write(refusedReply(why));
    connect(sock, &QSslSocket::disconnected, sock, &QObject::deleteLater);
    sock->disconnectFromHost();
}

void LinkServer::handle(QSslSocket *sock, const Request &req, const QByteArray &rest) {
    const QSslCertificate peer = sock->peerCertificate();
    const QString fp = peer.isNull() ? QString() : fingerprintOf(peer.toDer());
    if (fp.isEmpty()) return refuse(sock, QStringLiteral("present a certificate: the link pins it"));

    if (req.kind == RequestKind::Pair) {
        const QJsonObject pending = QJsonDocument::fromJson(readFile(pairingPath())).object();
        const bool live = qint64(pending.value(QStringLiteral("expiresMs")).toDouble()) >
                          QDateTime::currentMSecsSinceEpoch();
        if (!live || pending.value(QStringLiteral("tokenSha256")).toString() !=
                         sha256Hex(req.token.toLatin1()))
            return refuse(sock, QStringLiteral("that pairing code is not valid any more — run "
                                               "`remora link pair` again"));
        QFile::remove(pairingPath());  // one use
        QList<Device> devices = pairedDevices();
        devices.removeIf([&](const Device &d) { return d.fingerprint == fp; });
        QString name = req.deviceName.trimmed().isEmpty() ? QStringLiteral("phone")
                                                          : req.deviceName.trimmed();
        const QString base = name;
        for (int n = 2; std::any_of(devices.begin(), devices.end(),
                                    [&](const Device &d) { return d.name == name; });
             ++n)
            name = QStringLiteral("%1 (%2)").arg(base).arg(n);
        devices << Device{name, fp, QDateTime::currentMSecsSinceEpoch()};
        if (!writeFile(devicesPath(), devicesJson(devices), true))
            return refuse(sock, QStringLiteral("could not save the pairing"));
        qInfo("link: paired %s", qUtf8Printable(name));
        sock->write(okReply(name.toUtf8()));
        connect(sock, &QSslSocket::disconnected, sock, &QObject::deleteLater);
        sock->disconnectFromHost();
        return;
    }

    const QList<Device> devices = pairedDevices();
    if (std::none_of(devices.begin(), devices.end(), [&](const Device &d) { return d.fingerprint == fp; }))
        return refuse(sock, QStringLiteral("this phone is not paired with %1 — run `remora link "
                                           "pair` there and scan the code")
                                .arg(QSysInfo::machineHostName()));

    if (req.kind == RequestKind::Sources) {
        sock->write(okReply(sourcesJson(sources_.list())));
        connect(sock, &QSslSocket::disconnected, sock, &QObject::deleteLater);
        sock->disconnectFromHost();
        return;
    }
    connectSource(sock, req.sourceId, rest);
}

void LinkServer::connectSource(QSslSocket *sock, const QString &sourceId, const QByteArray &rest) {
    QIODevice *target = nullptr;
    QString error;
    if (sourceId == kComputerSourceId) {
        auto *local = new QLocalSocket(sock);
        local->connectToServer(sources_.hostAgentSocket);
        if (!local->waitForConnected(2000))
            return refuse(sock, QStringLiteral("this computer's apps are not being served — start "
                                               "remora-host-agent"));
        target = local;
    } else if (const auto profile = profileFromSourceId(sourceId)) {
        const QString adb = sources_.adbTarget(*profile, &error);
        if (adb.isEmpty()) return refuse(sock, error);
        const auto port = forwardFor(adb, &error);
        if (!port) return refuse(sock, error);
        auto *tcp = new QTcpSocket(sock);
        tcp->connectToHost(QHostAddress::LocalHost, *port);
        if (!tcp->waitForConnected(3000)) {
            forwards_.remove(adb);  // a forward that does not answer is remade next time
            return refuse(sock, QStringLiteral("%1's agent is not answering").arg(*profile));
        }
        tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        target = tcp;
    } else {
        return refuse(sock, QStringLiteral("no source %1").arg(sourceId));
    }
    sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    // A kernel send buffer left to grow on its own holds seconds of video on a slow phone link.
    sock->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, kPhoneSendBuffer);
    sock->write(okReply());
    if (!rest.isEmpty()) target->write(rest);  // a client may send its hello behind the request
    connect(sock, &QSslSocket::disconnected, sock, &QObject::deleteLater);
    splice(sock, target, sock);
}

std::optional<quint16> LinkServer::forwardFor(const QString &target, QString *error) {
    if (const auto it = forwards_.constFind(target); it != forwards_.constEnd()) return it->port;
    for (quint16 p = mirror::kTunnelPortFirst; p <= mirror::kTunnelPortLast; ++p) {
        auto lock = std::make_shared<QLockFile>(mirrorTunnelLockPath(p));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) continue;  // a live mirror's tunnel
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, p)) continue;
        probe.close();
        QProcess adb;
        const QStringList argv = mirror::adbAgentTunnelArgv(QStringLiteral("adb"), target, p, false);
        adb.start(argv.first(), argv.mid(1));
        if (!adb.waitForFinished(10'000) || adb.exitCode() != 0) continue;
        forwards_.insert(target, Forward{p, std::move(lock)});
        return p;
    }
    *error = QStringLiteral("no free tunnel port in %1..%2 for %3")
                 .arg(mirror::kTunnelPortFirst)
                 .arg(mirror::kTunnelPortLast)
                 .arg(target);
    return std::nullopt;
}

}  // namespace remora::link
