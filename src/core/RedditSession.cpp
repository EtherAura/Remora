#include "core/RedditSession.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace remora {

// --- protobuf primitives --------------------------------------------------------------------------
static void putVarint(QByteArray &b, quint64 n) {
    do {
        quint8 x = quint8(n & 0x7f);
        n >>= 7;
        if (n) x |= 0x80;
        b.append(char(x));
    } while (n);
}

static quint64 getVarint(const QByteArray &b, int &i) {
    quint64 r = 0;
    int s = 0;
    while (i < b.size()) {
        const quint8 x = quint8(b.at(i++));
        r |= quint64(x & 0x7f) << s;
        if (!(x & 0x80)) break;
        s += 7;
    }
    return r;
}

static QByteArray lenDelim(int field, const QByteArray &data) {
    QByteArray b;
    putVarint(b, (quint64(field) << 3) | 2);  // wire type 2 = length-delimited
    putVarint(b, quint64(data.size()));
    b.append(data);
    return b;
}

static QByteArray varintField(int field, quint64 v) {
    QByteArray b;
    putVarint(b, (quint64(field) << 3) | 0);  // wire type 0 = varint
    putVarint(b, v);
    return b;
}

// --- DsPref ---------------------------------------------------------------------------------------
DsPref DsPref::string(const QString &key, const QString &value) {
    DsPref p;
    p.key = key;
    p.type = String;
    p.str = value;
    return p;
}

DsPref DsPref::longv(const QString &key, qint64 value) {
    DsPref p;
    p.key = key;
    p.type = Long;
    p.num = value;
    return p;
}

bool DsPref::operator==(const DsPref &o) const {
    return key == o.key && type == o.type
           && (type == String ? str == o.str : num == o.num);
}

// --- encode/decode --------------------------------------------------------------------------------
QByteArray encodeDataStorePrefs(const QList<DsPref> &entries) {
    QByteArray out;
    for (const DsPref &e : entries) {
        const QByteArray valueMsg = (e.type == DsPref::String)
                                        ? lenDelim(5, e.str.toUtf8())       // Value.string = field 5
                                        : varintField(4, quint64(e.num));   // Value.long   = field 4
        const QByteArray entryMsg = lenDelim(1, e.key.toUtf8()) + lenDelim(2, valueMsg);
        out += lenDelim(1, entryMsg);  // preferences map entry = field 1
    }
    return out;
}

QList<DsPref> decodeDataStorePrefs(const QByteArray &data) {
    QList<DsPref> out;
    int i = 0;
    while (i < data.size()) {
        const quint64 tag = getVarint(data, i);
        if ((tag & 7) != 2) break;  // every top-level entry is length-delimited
        const int end = i + int(getVarint(data, i));
        QString key;
        DsPref pref;
        bool haveKey = false, haveVal = false;
        while (i < end) {
            const quint64 t = getVarint(data, i);
            const int f = int(t >> 3), wt = int(t & 7);
            if (wt != 2) {  // the entry only holds length-delimited fields; be defensive otherwise
                if (wt == 0) getVarint(data, i);
                else if (wt == 5) i += 4;
                else if (wt == 1) i += 8;
                continue;
            }
            QByteArray seg = data.mid(i, int(getVarint(data, i)));
            i += seg.size();
            if (f == 1) {
                key = QString::fromUtf8(seg);
                haveKey = true;
            } else if (f == 2) {  // the Value message
                int j = 0;
                while (j < seg.size()) {
                    const quint64 vt = getVarint(seg, j);
                    const int vf = int(vt >> 3), vw = int(vt & 7);
                    if (vw == 2) {
                        const QByteArray v = seg.mid(j, int(getVarint(seg, j)));
                        j += v.size();
                        if (vf == 5) {
                            pref = DsPref::string(QString(), QString::fromUtf8(v));
                            haveVal = true;
                        }
                    } else if (vw == 0) {
                        const quint64 n = getVarint(seg, j);
                        if (vf == 4) {
                            pref = DsPref::longv(QString(), qint64(n));
                            haveVal = true;
                        }
                    } else if (vw == 5) {
                        j += 4;
                    } else if (vw == 1) {
                        j += 8;
                    }
                }
            }
        }
        i = end;
        if (haveKey && haveVal) {
            pref.key = key;
            out.append(pref);
        }
    }
    return out;
}

// --- artifacts ------------------------------------------------------------------------------------
static const QString kAccountType = QStringLiteral("com.reddit.account");

RedditArtifacts buildRedditArtifacts(const QString &username, const QString &cookie) {
    const QString cookieVal = QStringLiteral("reddit_session=") + cookie;
    const QString activeName = QStringLiteral("com.reddit.auth_active.") + username;

    RedditArtifacts a;
    a.datastoreFiles.append(
        {QStringLiteral("com.reddit.session.cookie.preferences_pb"),
         encodeDataStorePrefs({DsPref::string(QStringLiteral("cookie_") + username, cookieVal)})});
    a.datastoreFiles.append(
        {QStringLiteral("com.reddit.auth_active.%1.preferences_pb").arg(username),
         encodeDataStorePrefs({
             DsPref::string(QStringLiteral("username"), username),
             DsPref::string(QStringLiteral("account_type"), kAccountType),
             DsPref::string(QStringLiteral("token"), QString()),      // empty -> app refreshes it
             DsPref::longv(QStringLiteral("token_expiration"), 1),    // expired -> forces the refresh
         })});
    a.datastoreFiles.append(
        {QStringLiteral("com.reddit.auth_active.preferences_pb"),
         encodeDataStorePrefs({
             DsPref::string(QStringLiteral("active_session_mode"), QStringLiteral("LOGGED_IN")),
             DsPref::string(QStringLiteral("active_session_name"), activeName),
         })});

    a.accountName = username;
    a.accountType = kAccountType;
    a.cookieExtraKey = QStringLiteral("com.reddit.cookie");
    a.cookieExtraValue = cookieVal;
    return a;
}

QStringList containerShellArgv(Backend backend, const QString &containerName, const QString &guest,
                               const QString &innerSh) {
    if (backend == Backend::Bare)
        return {QStringLiteral("docker"),   QStringLiteral("exec"), containerName,
                QStringLiteral("sh"),       QStringLiteral("-c"),   innerSh};
    // remote: the docker daemon lives on `guest`; hand ssh one command string. innerSh carries no
    // '"' (see header), so the inner double-quotes are unambiguous.
    return {QStringLiteral("ssh"), guest,
            QStringLiteral("docker exec %1 sh -c \"%2\"").arg(containerName, innerSh)};
}

QString redditAccountIdFromCookie(const QString &cookie) {
    const QStringList parts = cookie.split(QLatin1Char('.'));
    if (parts.size() < 2) return {};
    // JWT payload is base64url (no padding); QByteArray::Base64UrlEncoding handles the alphabet and
    // OmitTrailingEquals tolerates the missing padding.
    const QByteArray payload = QByteArray::fromBase64(
        parts.at(1).toUtf8(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    return o.value(QStringLiteral("sub")).toString();
}

}  // namespace remora
