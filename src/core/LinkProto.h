#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

// The link: how a paired phone reaches this computer (docs/PHONE_APP.md §2,
// docs/LINK_PROTOCOL.md). TLS with both certificates pinned by fingerprint; inside it, one short
// request per connection — pair, list the sources, or connect to one — after which a connected
// connection carries protocol v3 unchanged, byte for byte, to the source's agent. Pure: bytes and
// strings in and out; sockets and certificates live in the engine.

namespace remora::link {

inline constexpr char kLinkMagic[] = "RMLK";
inline constexpr quint16 kLinkVersion = 1;
inline constexpr quint16 kDefaultPort = 27200;
inline constexpr char kUriScheme[] = "remora-link";

enum class RequestKind : quint8 { Pair = 1, Sources = 2, Connect = 3 };

// Client → host: "RMLK" u16 version u8 kind, then the kind's fields, each a u16-prefixed UTF-8
// string — Pair: token, device name; Sources: nothing; Connect: source id.
QByteArray pairRequest(const QString &token, const QString &deviceName);
QByteArray sourcesRequest();
QByteArray connectRequest(const QString &sourceId);

struct Request {
    RequestKind kind = RequestKind::Sources;
    QString token, deviceName;  // Pair
    QString sourceId;           // Connect
};
// nullopt until the whole request is in (then *consumed is its length), or with *error set for a
// request no host serves (wrong magic or version, unknown kind) — the caller closes.
std::optional<Request> parseRequest(const QByteArray &buf, int *consumed, QString *error);

// Host → client: u8 status (0 ok), u32 length, then the payload — the source list for Sources, a
// refusal's UTF-8 reason when the status is not 0, nothing for Pair and Connect. After an ok
// Connect, the connection is the source's.
QByteArray okReply(const QByteArray &payload = {});
QByteArray refusedReply(const QString &why);
struct Reply {
    bool ok = false;
    QByteArray payload;
};
std::optional<Reply> parseReply(const QByteArray &buf, int *consumed);

// A place a phone can open apps from: this computer's own ("computer"), or a Remora instance's
// Android ("android"). The id is what Connect takes.
struct Source {
    QString id, name, kind;
    bool operator==(const Source &) const = default;
};
QByteArray sourcesJson(const QList<Source> &sources);
QList<Source> parseSourcesJson(const QByteArray &json);
inline const QString kComputerSourceId = QStringLiteral("computer");
// "android:<profile>": the profile is the source's identity, so it survives the instance's
// address changing.
QString androidSourceId(const QString &profile);
std::optional<QString> profileFromSourceId(const QString &id);

// What `remora link pair` shows, as a QR code and as text: every address the host listens on,
// its certificate's SHA-256 fingerprint, a one-time token and the host's name.
//   remora-link://pair?h=192.168.0.10:27200&h=[fd00::1]:27200&fp=<hex>&t=<token>&n=<name>
struct PairingInfo {
    QStringList addresses;  // host:port, IPv6 hosts bracketed
    QString fingerprint;    // lowercase hex, 64 characters
    QString token;
    QString hostName;
};
QString pairingUri(const PairingInfo &info);
std::optional<PairingInfo> parsePairingUri(const QString &uri);

// SHA-256 of a DER certificate, as the fingerprint is written everywhere: lowercase hex.
QString fingerprintOf(const QByteArray &der);

// The paired phones: a name the person gave it and its certificate's fingerprint.
struct Device {
    QString name, fingerprint;
    qint64 pairedAtMs = 0;
    bool operator==(const Device &) const = default;
};
QByteArray devicesJson(const QList<Device> &devices);
QList<Device> parseDevicesJson(const QByteArray &json);

}  // namespace remora::link
