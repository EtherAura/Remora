#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>

#include "core/Backend.h"

namespace remora {

// Wrap a POSIX-sh command line to run INSIDE the target container: a local `docker exec` on the bare
// backend, or that same `docker exec` wrapped over ssh to the docker host for remote. Pure +
// golden-tested; the engine runs the argv verbatim. `innerSh` must not contain a double quote — it
// is embedded in "…sh -c \"<inner>\"" for the ssh form (matching the rest of the engine).
QStringList containerShellArgv(Backend backend, const QString &containerName, const QString &guest,
                               const QString &innerSh);

// Reconstructs the official Reddit app's logged-in state from a single web `reddit_session` cookie.
// The app keeps login state in Jetpack DataStore protobuf files (files/datastore/*.preferences_pb),
// NOT AccountManager and NOT the R8-obfuscated EncryptedSharedPreferences. Those files are plaintext
// and account-bound (not device-bound), so they can be written from a cookie captured by a plain web
// login — no rooted device (bd remora-6zj, proven live: the app minted a fresh access token from an
// injected empty/expired token blob using only the cookie).
//
// This file is PURE: it only produces bytes and descriptors. The engine performs the impure writes.

// --- DataStore Preferences protobuf ---------------------------------------------------------------
// androidx.datastore.preferences wire format:
//   PreferenceMap { map<string, Value> preferences = 1; }
//   Value { oneof { ... int64 long = 4; string string = 5; ... } }
// Only the two value types Reddit's auth state uses (string, long) are modelled; that is all the
// feature needs and keeps the encoder trivially verifiable against the protobuf spec.
struct DsPref {
    enum Type { String, Long };
    QString key;
    Type type = String;
    QString str;      // when type == String
    qint64 num = 0;   // when type == Long
    static DsPref string(const QString &key, const QString &value);
    static DsPref longv(const QString &key, qint64 value);
    bool operator==(const DsPref &o) const;
};

// Encode a Preferences map to its on-disk .preferences_pb bytes. Byte-for-byte compatible with the
// app's own DataStore writer for string/long entries (proven: the app read files this produced).
QByteArray encodeDataStorePrefs(const QList<DsPref> &entries);

// Parse .preferences_pb bytes back to entries. Unknown value types are skipped. Used to read an
// existing store (e.g. resolve a username, or verify the app refreshed a token) and by the tests.
QList<DsPref> decodeDataStorePrefs(const QByteArray &data);

// --- Reddit session artifacts ---------------------------------------------------------------------
struct RedditFile {
    QString name;      // basename under files/datastore/
    QByteArray bytes;
};

struct RedditArtifacts {
    QList<RedditFile> datastoreFiles;  // session.cookie, auth_active.<user>, active-session pointer
    QString accountName;               // <username>            -> AccountManager row (accounts_ce/de)
    QString accountType;               // "com.reddit.account"
    QString cookieExtraKey;            // "com.reddit.cookie"   -> accounts_ce extras row
    QString cookieExtraValue;          // "reddit_session=<cookie>"
};

// Build every artifact needed to log the app in as `username` from `cookie` (the raw reddit_session
// JWT, without the "reddit_session=" prefix). The access token is deliberately empty and expired:
// the app mints a fresh one from the cookie on first launch, so the feature never has to obtain one.
RedditArtifacts buildRedditArtifacts(const QString &username, const QString &cookie);

// The Reddit account id (JWT `sub`, e.g. "t2_g4cxd") decoded from a reddit_session cookie, or empty
// if it can't be parsed. The cookie carries the account id, not the username — the caller resolves
// the username separately (an authenticated /api/v1/me call in the login WebView's context).
QString redditAccountIdFromCookie(const QString &cookie);

}  // namespace remora
