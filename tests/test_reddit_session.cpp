#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "core/RedditSession.h"

using namespace remora;

// Find a decoded entry by key (empty DsPref if absent).
static DsPref at(const QList<DsPref> &l, const QString &key) {
    for (const DsPref &p : l)
        if (p.key == key) return p;
    return DsPref();
}

class TestRedditSession : public QObject {
    Q_OBJECT
private slots:
    // Golden wire bytes, hand-computed from the protobuf spec. Locks the format the app parses.
    void encodesStringEntry() {
        // {"a":"b"} = map-entry(1){ key(1)="a", value(2){ string(5)="b" } }
        //   0A 08  0A 01 61  12 03  2A 01 62
        QCOMPARE(encodeDataStorePrefs({DsPref::string(QStringLiteral("a"), QStringLiteral("b"))}),
                 QByteArray::fromHex("0a080a016112032a0162"));
    }
    void encodesLongEntry() {
        // {"e":1} = map-entry(1){ key(1)="e", value(2){ long(4)=1 } }
        //   0A 07  0A 01 65  12 02  20 01
        QCOMPARE(encodeDataStorePrefs({DsPref::longv(QStringLiteral("e"), 1)}),
                 QByteArray::fromHex("0a070a016512022001"));
    }

    // Encode → decode reproduces the entries (order preserved, both value types).
    void roundTrips() {
        const QList<DsPref> in{
            DsPref::string(QStringLiteral("username"), QStringLiteral("c3d4")),
            DsPref::string(QStringLiteral("account_type"), QStringLiteral("com.reddit.account")),
            DsPref::string(QStringLiteral("token"), QString()),
            DsPref::longv(QStringLiteral("token_expiration"), 1785377841605LL),
        };
        QCOMPARE(decodeDataStorePrefs(encodeDataStorePrefs(in)), in);
    }

    void emptyStringSurvivesRoundTrip() {
        const QList<DsPref> in{DsPref::string(QStringLiteral("token"), QString())};
        const QList<DsPref> out = decodeDataStorePrefs(encodeDataStorePrefs(in));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).type, DsPref::String);
        QVERIFY(out.at(0).str.isEmpty());
    }

    // The three DataStore files, named and shaped as the app expects.
    void buildsArtifacts() {
        const RedditArtifacts a =
            buildRedditArtifacts(QStringLiteral("testuser"), QStringLiteral("HDR.PAY.SIG"));

        QCOMPARE(a.datastoreFiles.size(), 3);
        QCOMPARE(a.datastoreFiles.at(0).name,
                 QStringLiteral("com.reddit.session.cookie.preferences_pb"));
        QCOMPARE(a.datastoreFiles.at(1).name,
                 QStringLiteral("com.reddit.auth_active.testuser.preferences_pb"));
        QCOMPARE(a.datastoreFiles.at(2).name,
                 QStringLiteral("com.reddit.auth_active.preferences_pb"));

        const QList<DsPref> cookie = decodeDataStorePrefs(a.datastoreFiles.at(0).bytes);
        QCOMPARE(at(cookie, QStringLiteral("cookie_testuser")).str,
                 QStringLiteral("reddit_session=HDR.PAY.SIG"));

        const QList<DsPref> blob = decodeDataStorePrefs(a.datastoreFiles.at(1).bytes);
        QCOMPARE(at(blob, QStringLiteral("username")).str, QStringLiteral("testuser"));
        QCOMPARE(at(blob, QStringLiteral("account_type")).str, QStringLiteral("com.reddit.account"));
        QVERIFY(at(blob, QStringLiteral("token")).str.isEmpty());  // app mints the real one
        QCOMPARE(at(blob, QStringLiteral("token_expiration")).num, 1LL);  // expired -> forces refresh

        const QList<DsPref> ptr = decodeDataStorePrefs(a.datastoreFiles.at(2).bytes);
        QCOMPARE(at(ptr, QStringLiteral("active_session_mode")).str, QStringLiteral("LOGGED_IN"));
        QCOMPARE(at(ptr, QStringLiteral("active_session_name")).str,
                 QStringLiteral("com.reddit.auth_active.testuser"));

        QCOMPARE(a.accountName, QStringLiteral("testuser"));
        QCOMPARE(a.accountType, QStringLiteral("com.reddit.account"));
        QCOMPARE(a.cookieExtraKey, QStringLiteral("com.reddit.cookie"));
        QCOMPARE(a.cookieExtraValue, QStringLiteral("reddit_session=HDR.PAY.SIG"));
    }

    // Backend-appropriate container exec: direct argv on bare, ssh-wrapped elsewhere.
    void wrapsContainerExec() {
        QCOMPARE(containerShellArgv(Backend::Bare, QStringLiteral("remora-bm"), QString(),
                                    QStringLiteral("am force-stop com.reddit.frontpage")),
                 (QStringList{QStringLiteral("docker"), QStringLiteral("exec"),
                              QStringLiteral("remora-bm"), QStringLiteral("sh"),
                              QStringLiteral("-c"),
                              QStringLiteral("am force-stop com.reddit.frontpage")}));
        QCOMPARE(containerShellArgv(Backend::Remote, QStringLiteral("remora-lineage24"),
                                    QStringLiteral("user@host"), QStringLiteral("stop")),
                 (QStringList{QStringLiteral("ssh"), QStringLiteral("user@host"),
                              QStringLiteral("docker exec remora-lineage24 sh -c \"stop\"")}));
    }

    // Account id comes from the JWT payload's `sub`, tolerating base64url without padding.
    void decodesAccountIdFromCookie() {
        const QByteArray payload =
            QJsonDocument(QJsonObject{{QStringLiteral("sub"), QStringLiteral("t2_g4cxd")}})
                .toJson(QJsonDocument::Compact);
        const QString jwt = QStringLiteral("header.")
                            + QString::fromUtf8(payload.toBase64(QByteArray::Base64UrlEncoding
                                                                 | QByteArray::OmitTrailingEquals))
                            + QStringLiteral(".sig");
        QCOMPARE(redditAccountIdFromCookie(jwt), QStringLiteral("t2_g4cxd"));
        QVERIFY(redditAccountIdFromCookie(QStringLiteral("not-a-jwt")).isEmpty());
    }
};

QTEST_MAIN(TestRedditSession)
#include "test_reddit_session.moc"
