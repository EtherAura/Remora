#pragma once
#include <QHash>
#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "core/LinkProto.h"

class QIODevice;
class QLockFile;
class QSslServer;
class QSslSocket;

// The link service (docs/PHONE_APP.md §2, docs/LINK_PROTOCOL.md): a paired phone's one
// way into this computer. It listens for TLS on every interface, pins both certificates by
// fingerprint, answers pair / sources / connect, and splices a connected connection to the source
// it asked for — the host agent's unix socket, or a Remora instance's in-image agent through an
// adb forward — so protocol v3 runs end to end with nothing in between that reads it.

namespace remora::link {

// $XDG_DATA_HOME/remora/link: the host's key and certificate, the paired devices, and a pending
// pairing. The directory is the owner's alone (0700); the key is 0600.
QString linkDir();

// The host's identity, made on first use with the openssl CLI (Qt cannot generate a key): an
// ECDSA P-256 key and a self-signed certificate. Nothing about it is secret but the key, and
// nothing about trust rests on a CA — each side pins the other's fingerprint.
bool ensureHostIdentity(QString *error);
QString hostFingerprint(QString *error);

// A one-time pairing token, valid for ten minutes and stored only as its hash, and the URI a phone
// scans to use it: every address this computer can be reached on, the port, the fingerprint and
// the token. A second pairing replaces a pending one. `addresses` replaces the ones found (the
// suites run with no network but loopback, so they name 127.0.0.1 themselves).
std::optional<QString> beginPairing(quint16 port, QString *error,
                                    const QStringList &addresses = {});

QList<Device> pairedDevices();
bool forgetDevice(const QString &name);

// What the server needs from the rest of Remora, injected so it stays testable without a
// remorarc or a device: the sources a paired phone may open right now, and a profile's adb target
// with the device answering (empty with *error otherwise).
struct SourceProvider {
    std::function<QList<Source>()> list;
    std::function<QString(const QString &profile, QString *error)> adbTarget;
    QString hostAgentSocket;
};

class LinkServer : public QObject {
    Q_OBJECT
public:
    explicit LinkServer(SourceProvider sources, QObject *parent = nullptr);
    ~LinkServer() override;

    // Port 0 picks a free one; port() says which.
    bool listen(quint16 port, QString *error);
    quint16 port() const;

private:
    void onEncrypted(QSslSocket *sock);
    void onRequestData(QSslSocket *sock);
    void handle(QSslSocket *sock, const Request &req, const QByteArray &rest);
    void connectSource(QSslSocket *sock, const QString &sourceId, const QByteArray &rest);
    std::optional<quint16> forwardFor(const QString &target, QString *error);
    void refuse(QSslSocket *sock, const QString &why);

    SourceProvider sources_;
    QSslServer *server_ = nullptr;
    QSslConfiguration tls_;
    QHash<QSslSocket *, QByteArray> pending_;  // connections still sending their request
    // One adb forward per instance target, held — with the mirror's tunnel lock, so its sweep of
    // leaked forwards leaves these alone — until the service stops.
    struct Forward {
        quint16 port = 0;
        std::shared_ptr<QLockFile> lock;
    };
    QHash<QString, Forward> forwards_;
};

// Bytes both ways between two connected devices until either closes, with backpressure: a side
// that is not draining stops the other being read, rather than growing a buffer without bound.
void splice(QIODevice *a, QIODevice *b, QObject *owner);

}  // namespace remora::link
