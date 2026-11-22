#include "LinkProto.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

namespace remora::link {

namespace {

void putU16(QByteArray &b, quint16 v) {
    char raw[2];
    qToBigEndian(v, raw);
    b.append(raw, 2);
}
void putU32(QByteArray &b, quint32 v) {
    char raw[4];
    qToBigEndian(v, raw);
    b.append(raw, 4);
}
void putString(QByteArray &b, const QString &s) {
    const QByteArray utf8 = s.toUtf8().left(0xFFFF);
    putU16(b, quint16(utf8.size()));
    b.append(utf8);
}

QByteArray header(RequestKind kind) {
    QByteArray b(kLinkMagic, 4);
    putU16(b, kLinkVersion);
    b.append(char(kind));
    return b;
}

// A u16-prefixed string at *at; false until it is all there.
bool takeString(const QByteArray &buf, int *at, QString *out) {
    if (buf.size() < *at + 2) return false;
    const quint16 len = qFromBigEndian<quint16>(buf.constData() + *at);
    if (buf.size() < *at + 2 + len) return false;
    *out = QString::fromUtf8(buf.constData() + *at + 2, len);
    *at += 2 + len;
    return true;
}

constexpr quint32 kMaxReply = 8u << 20;

}  // namespace

QByteArray pairRequest(const QString &token, const QString &deviceName) {
    QByteArray b = header(RequestKind::Pair);
    putString(b, token);
    putString(b, deviceName);
    return b;
}

QByteArray sourcesRequest() { return header(RequestKind::Sources); }

QByteArray connectRequest(const QString &sourceId) {
    QByteArray b = header(RequestKind::Connect);
    putString(b, sourceId);
    return b;
}

std::optional<Request> parseRequest(const QByteArray &buf, int *consumed, QString *error) {
    if (buf.size() < 7) {
        // Refuse a wrong magic as soon as it shows, rather than wait for seven bytes of it.
        const int n = std::min<int>(int(buf.size()), 4);
        if (buf.left(n) != QByteArray(kLinkMagic, n))
            *error = QStringLiteral("not a link request");
        return std::nullopt;
    }
    if (buf.left(4) != QByteArray(kLinkMagic, 4)) {
        *error = QStringLiteral("not a link request");
        return std::nullopt;
    }
    const quint16 version = qFromBigEndian<quint16>(buf.constData() + 4);
    if (version != kLinkVersion) {
        *error = QStringLiteral("link protocol v%1, and this host speaks v%2").arg(version).arg(kLinkVersion);
        return std::nullopt;
    }
    Request r;
    int at = 7;
    switch (quint8(buf[6])) {
        case quint8(RequestKind::Pair):
            r.kind = RequestKind::Pair;
            if (!takeString(buf, &at, &r.token) || !takeString(buf, &at, &r.deviceName))
                return std::nullopt;
            break;
        case quint8(RequestKind::Sources):
            r.kind = RequestKind::Sources;
            break;
        case quint8(RequestKind::Connect):
            r.kind = RequestKind::Connect;
            if (!takeString(buf, &at, &r.sourceId)) return std::nullopt;
            break;
        default:
            *error = QStringLiteral("unknown link request %1").arg(quint8(buf[6]));
            return std::nullopt;
    }
    *consumed = at;
    return r;
}

QByteArray okReply(const QByteArray &payload) {
    QByteArray b(1, '\0');
    putU32(b, quint32(payload.size()));
    return b + payload;
}

QByteArray refusedReply(const QString &why) {
    const QByteArray utf8 = why.toUtf8();
    QByteArray b(1, '\1');
    putU32(b, quint32(utf8.size()));
    return b + utf8;
}

std::optional<Reply> parseReply(const QByteArray &buf, int *consumed) {
    if (buf.size() < 5) return std::nullopt;
    const quint32 len = qFromBigEndian<quint32>(buf.constData() + 1);
    if (len > kMaxReply || quint32(buf.size()) < 5 + len) return std::nullopt;
    *consumed = 5 + int(len);
    return Reply{buf[0] == '\0', buf.mid(5, int(len))};
}

QByteArray sourcesJson(const QList<Source> &sources) {
    QJsonArray arr;
    for (const Source &s : sources)
        arr << QJsonObject{{QStringLiteral("id"), s.id},
                           {QStringLiteral("name"), s.name},
                           {QStringLiteral("kind"), s.kind}};
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QList<Source> parseSourcesJson(const QByteArray &json) {
    QList<Source> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        out << Source{o.value(QStringLiteral("id")).toString(),
                      o.value(QStringLiteral("name")).toString(),
                      o.value(QStringLiteral("kind")).toString()};
    }
    return out;
}

QString androidSourceId(const QString &profile) { return QStringLiteral("android:") + profile; }

std::optional<QString> profileFromSourceId(const QString &id) {
    if (!id.startsWith(QLatin1String("android:")) || id.size() <= 8) return std::nullopt;
    return id.mid(8);
}

QString pairingUri(const PairingInfo &info) {
    QUrlQuery q;
    for (const QString &a : info.addresses) q.addQueryItem(QStringLiteral("h"), a);
    q.addQueryItem(QStringLiteral("fp"), info.fingerprint);
    q.addQueryItem(QStringLiteral("t"), info.token);
    if (!info.hostName.isEmpty()) q.addQueryItem(QStringLiteral("n"), info.hostName);
    return QStringLiteral("%1://pair?%2")
        .arg(QString::fromLatin1(kUriScheme), q.toString(QUrl::FullyEncoded));
}

std::optional<PairingInfo> parsePairingUri(const QString &uri) {
    const QUrl url(uri);
    if (url.scheme() != QLatin1String(kUriScheme) || url.host() != QLatin1String("pair"))
        return std::nullopt;
    const QUrlQuery q(url);
    PairingInfo info;
    info.addresses = q.allQueryItemValues(QStringLiteral("h"), QUrl::FullyDecoded);
    info.fingerprint = q.queryItemValue(QStringLiteral("fp"), QUrl::FullyDecoded);
    info.token = q.queryItemValue(QStringLiteral("t"), QUrl::FullyDecoded);
    info.hostName = q.queryItemValue(QStringLiteral("n"), QUrl::FullyDecoded);
    if (info.addresses.isEmpty() || info.fingerprint.size() != 64 || info.token.isEmpty())
        return std::nullopt;
    return info;
}

QString fingerprintOf(const QByteArray &der) {
    return QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex());
}

QByteArray devicesJson(const QList<Device> &devices) {
    QJsonArray arr;
    for (const Device &d : devices)
        arr << QJsonObject{{QStringLiteral("name"), d.name},
                           {QStringLiteral("fingerprint"), d.fingerprint},
                           {QStringLiteral("pairedAtMs"), d.pairedAtMs}};
    return QJsonDocument(arr).toJson(QJsonDocument::Indented);
}

QList<Device> parseDevicesJson(const QByteArray &json) {
    QList<Device> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        Device d{o.value(QStringLiteral("name")).toString(),
                 o.value(QStringLiteral("fingerprint")).toString(),
                 qint64(o.value(QStringLiteral("pairedAtMs")).toDouble())};
        if (d.fingerprint.size() == 64) out << d;
    }
    return out;
}

}  // namespace remora::link
