#include "engine/RedditLogin.h"

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <optional>

#include "core/RedditSession.h"

namespace remora {

static const QString kPkg = QStringLiteral("com.reddit.frontpage");

// Escape a value for embedding in a single-quoted SQL literal.
// The account row Android's AccountManager reads, written into the two pulled databases with Qt's
// own SQLite (bd remora-7oih.3). This was the host's sqlite3 CLI, which a Windows client does not
// have, fed the cookie through a .read file so it stayed out of argv; bound parameters keep it out
// of every command line with no file at all, and nothing is quoted by hand. Returns the account
// id; nullopt with *error set when the credential database could not be updated. The DE row is
// best-effort, as it always was: AccountManager rebuilds it from CE.
static std::optional<qint64> writeAccountRows(const QString &ce, const QString &de,
                                              const RedditArtifacts &art, QString *error) {
    const QString conn =
        QStringLiteral("remora-accounts-") + QUuid::createUuid().toString(QUuid::Id128);
    std::optional<qint64> id;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        const auto run = [&db](const QString &sql, const QVariantList &values) {
            QSqlQuery q(db);
            if (!q.prepare(sql)) return q;
            for (const QVariant &v : values) q.addBindValue(v);
            q.exec();
            return q;
        };
        const auto failed = [&](const QString &what, const QSqlError &e) {
            *error = QStringLiteral("%1: %2").arg(what, e.text());
        };
        db.setDatabaseName(ce);
        if (!db.isValid()) {
            *error = QStringLiteral("Qt's SQLite driver (QSQLITE) is not installed");
        } else if (!db.open()) {
            failed(QStringLiteral("could not open accounts_ce.db"), db.lastError());
        } else {
            // Reuse the account's existing id if present, else the next free one.
            QSqlQuery sel = run(QStringLiteral(
                                    "SELECT COALESCE((SELECT _id FROM accounts WHERE name=? AND "
                                    "type=?),(SELECT COALESCE(MAX(_id),0)+1 FROM accounts))"),
                                {art.accountName, art.accountType});
            bool numeric = false;
            const qint64 nid = sel.next() ? sel.value(0).toLongLong(&numeric) : 0;
            if (!numeric) {
                failed(QStringLiteral("could not read the account table"), sel.lastError());
            } else if (!db.transaction()) {
                failed(QStringLiteral("could not start a transaction"), db.lastError());
            } else {
                QSqlQuery a = run(QStringLiteral("INSERT OR IGNORE INTO accounts(_id,name,type,"
                                                 "password) VALUES(?,?,?,'')"),
                                  {nid, art.accountName, art.accountType});
                QSqlQuery x = run(QStringLiteral("INSERT OR REPLACE INTO extras(accounts_id,key,"
                                                 "value) VALUES(?,?,?)"),
                                  {nid, art.cookieExtraKey, art.cookieExtraValue});
                QSqlQuery s = run(QStringLiteral("UPDATE sqlite_sequence SET seq=MAX(seq,?) WHERE "
                                                 "name='accounts'"),
                                  {nid});
                const bool ok = a.isActive() && x.isActive() && s.isActive();
                if (ok && db.commit()) {
                    id = nid;
                } else {
                    db.rollback();
                    failed(QStringLiteral("could not add the account"),
                           !a.isActive() ? a.lastError() : !x.isActive() ? x.lastError()
                                                                          : s.lastError());
                }
            }
            // Fold any WAL back into the file: only the main database is pushed back.
            run(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), {});
            db.close();
        }
        if (id) {
            db.setDatabaseName(de);
            if (db.open()) {
                run(QStringLiteral("INSERT OR IGNORE INTO accounts(_id,name,type,previous_name,"
                                   "last_password_entry_time_millis_epoch) VALUES(?,?,?,NULL,0)"),
                    {*id, art.accountName, art.accountType});
                run(QStringLiteral("UPDATE sqlite_sequence SET seq=MAX(seq,?) WHERE name='accounts'"),
                    {*id});
                run(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), {});
                db.close();
            }
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return id;
}

// Run a POSIX-sh command inside the target container (docker exec, backend-aware).
static ProcResult csh(Spawner &sp, const RunContext &ctx, const QString &inner,
                      const LineSink &log = {}) {
    return sp.run(containerShellArgv(ctx.backend, ctx.rc.containerName, ctx.guest, inner), {}, log);
}

// Copy a file OUT of the container to a local path. `docker cp` can't be used: the container's
// rootfs overlay is read-only and docker cp needs a writable pivot dir, so it errors. Instead
// stream the bytes through `docker exec … cat`, redirecting into a file on the side where the
// daemon runs (locally for bare; on the docker host, then scp back, for remote) — the redirect
// keeps binary intact.
//
// Everything this file moves carries the session cookie, so every copy of it is owner-only: umask
// 077 on both sides, and on a remote docker host a fresh mktemp name rather than a fixed path in a
// shared /tmp, which another user could read while it existed — or pre-create as a symlink.
static bool cpOut(Spawner &sp, const RunContext &ctx, const QString &containerPath,
                  const QString &local) {
    if (ctx.backend == Backend::Bare) {
        const QString inner = QStringLiteral("umask 077; docker exec %1 cat '%2' > '%3'")
                                  .arg(ctx.rc.containerName, containerPath, local);
        return sp.run({QStringLiteral("sh"), QStringLiteral("-c"), inner}).rc == 0
               && QFileInfo(local).size() > 0;
    }
    const ProcResult made = sp.run(Spawner::sshArgv(
        ctx.guest, QStringLiteral("umask 077; f=$(mktemp /tmp/remora-rl.XXXXXX) || exit 1; "
                                  "docker exec %1 cat '%2' > \"$f\" || { rm -f \"$f\"; exit 1; }; "
                                  "echo \"$f\"")
                       .arg(ctx.rc.containerName, containerPath)));
    const QString rtmp = made.out.trimmed().section(QLatin1Char('\n'), -1);
    if (made.rc != 0 || !rtmp.startsWith(QLatin1String("/tmp/remora-rl."))) return false;
    const bool ok = sp.run(Spawner::scpArgv({ctx.guest + QLatin1Char(':') + rtmp, local},
                                            {QStringLiteral("-q")}))
                            .rc == 0
                    && QFileInfo(local).size() > 0;
    sp.run(Spawner::sshArgv(ctx.guest, QStringLiteral("rm -f %1").arg(rtmp)));
    return ok;
}

// Copy a local file INTO the container by piping it to `docker exec -i … 'cat > <path>'` (stdin
// streaming; no pivot dir, unlike docker cp). For remote the file rides to the docker host via
// scp first, then that host's shell redirects it into docker exec's stdin.
static bool cpIn(Spawner &sp, const RunContext &ctx, const QString &local,
                 const QString &containerPath) {
    if (ctx.backend == Backend::Bare) {
        const QString inner = QStringLiteral("docker exec -i %1 sh -c 'cat > %2' < '%3'")
                                  .arg(ctx.rc.containerName, containerPath, local);
        return sp.run({QStringLiteral("sh"), QStringLiteral("-c"), inner}).rc == 0;
    }
    // scp gives the copy the SOURCE's mode, so the source is made owner-only first.
    QFile::setPermissions(local, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const ProcResult made = sp.run(
        Spawner::sshArgv(ctx.guest, QStringLiteral("umask 077; mktemp /tmp/remora-rl.XXXXXX")));
    const QString rtmp = made.out.trimmed().section(QLatin1Char('\n'), -1);
    if (made.rc != 0 || !rtmp.startsWith(QLatin1String("/tmp/remora-rl."))) return false;
    const bool ok =
        sp.run(Spawner::scpArgv({local, ctx.guest + QLatin1Char(':') + rtmp}, {QStringLiteral("-q")}))
                .rc == 0
        && sp.run(Spawner::sshArgv(ctx.guest,
                                   QStringLiteral("docker exec -i %1 sh -c 'cat > %2' < '%3'")
                                       .arg(ctx.rc.containerName, containerPath, rtmp)))
                   .rc == 0;
    sp.run(Spawner::sshArgv(ctx.guest, QStringLiteral("rm -f %1").arg(rtmp)));
    return ok;
}

// A new owner-only file, created that way rather than tightened after the write.
static bool writePrivate(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    return f.write(bytes) == bytes.size();
}

// One DataStore file into the app's private dir, owned by the app uid. Staged as a local file and
// streamed in rather than embedded in the command: these bytes are the session cookie, and a
// command line — even base64'd, as this once was — is readable by every user on the host through
// ps and /proc for as long as the process runs.
static bool pushSessionFile(Spawner &sp, const RunContext &ctx, const QTemporaryDir &tmp,
                            const RedditFile &f, const QString &dsdir, const QString &uid,
                            const LineSink &log) {
    const QString local = tmp.filePath(f.name);
    const QString dest = dsdir + QLatin1Char('/') + f.name;
    return writePrivate(local, f.bytes) && cpIn(sp, ctx, local, dest)
           && csh(sp, ctx, QStringLiteral("chown %1:%1 '%2'; chmod 600 '%2'").arg(uid, dest), log)
                      .rc == 0;
}

bool injectRedditSession(Spawner &sp, const RunContext &ctx, const QString &username,
                         const QString &cookie, const LineSink &log) {
    const auto note = [&](const QString &m) {
        if (log) log(QStringLiteral("stdout"), m);
    };
    const RedditArtifacts art = buildRedditArtifacts(username, cookie);
    const QString dsdir = QStringLiteral("/data/data/%1/files/datastore").arg(kPkg);

    const QString uid =
        csh(sp, ctx, QStringLiteral("stat -c %u /data/data/") + kPkg).out.trimmed();
    if (uid.isEmpty() || !uid.at(0).isDigit()) {
        note(QStringLiteral("Reddit is not installed in %1 (or the container is unreachable) — "
                            "install it from the Play Store first.")
                 .arg(ctx.rc.containerName));
        return false;
    }
    note(QStringLiteral("Reddit uid=%1 in container %2").arg(uid, ctx.rc.containerName));
    csh(sp, ctx, QStringLiteral("am force-stop ") + kPkg);

    // 1) DataStore session files — small, so inline base64.
    //
    // Create the directory first. DataStore makes files/datastore lazily, the first time the app
    // itself writes a preference — so on a freshly installed Reddit that has never been opened,
    // neither files/ nor datastore/ exists and every write below fails with "No such file or
    // directory", which is exactly the state a user injecting a session is in.
    //
    // mkdir -p runs as root, so hand the whole chain to the app uid afterwards: a root-owned
    // files/ would survive this login and break the app's own writes later, which is a worse
    // failure than the one being fixed (it would look like Reddit corrupting itself days later).
    const QString filesDir = QStringLiteral("/data/data/%1/files").arg(kPkg);
    if (csh(sp, ctx,
            QStringLiteral("mkdir -p '%1' && chown %2:%2 '%3' '%1' && chmod 700 '%3' '%1'")
                .arg(dsdir, uid, filesDir))
            .rc != 0) {
        note(QStringLiteral("Could not create %1 — cannot write the session.").arg(dsdir));
        return false;
    }

    // Mode 0700, from QTemporaryDir: everything staged in it is, or carries, the cookie.
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        note(QStringLiteral("Could not create a private temporary directory."));
        return false;
    }

    note(QStringLiteral("Writing %1 DataStore session file(s)…").arg(art.datastoreFiles.size()));
    for (const RedditFile &f : art.datastoreFiles)
        if (!pushSessionFile(sp, ctx, tmp, f, dsdir, uid, log)) {
            note(QStringLiteral("Failed writing %1").arg(f.name));
            return false;
        }

    // 2) AccountManager row. The container's own sqlite3 crashes (aborts even on :memory:), so the
    // account db is edited offline: stop the framework to release it, pull, edit here with Qt's
    // SQLite, drop the stale WAL/SHM, push back, restart.
    note(QStringLiteral("Registering AccountManager account '%1'…").arg(art.accountName));
    csh(sp, ctx, QStringLiteral("stop"));
    sp.waitMs(4000);

    const QString ce = tmp.filePath(QStringLiteral("accounts_ce.db"));
    const QString de = tmp.filePath(QStringLiteral("accounts_de.db"));
    const auto resume = [&]() { csh(sp, ctx, QStringLiteral("start")); };
    if (!cpOut(sp, ctx, QStringLiteral("/data/system_ce/0/accounts_ce.db"), ce)
        || !cpOut(sp, ctx, QStringLiteral("/data/system_de/0/accounts_de.db"), de)) {
        note(QStringLiteral("Could not pull the account databases."));
        resume();
        return false;
    }
    csh(sp, ctx,
        QStringLiteral("rm -f /data/system_ce/0/accounts_ce.db-wal /data/system_ce/0/accounts_ce.db-shm "
                       "/data/system_de/0/accounts_de.db-wal /data/system_de/0/accounts_de.db-shm"));

    QString dbError;
    if (!writeAccountRows(ce, de, art, &dbError)) {
        note(QStringLiteral("Could not add the account to the database (%1).").arg(dbError));
        resume();
        return false;
    }

    if (!cpIn(sp, ctx, ce, QStringLiteral("/data/system_ce/0/accounts_ce.db"))
        || !cpIn(sp, ctx, de, QStringLiteral("/data/system_de/0/accounts_de.db"))) {
        note(QStringLiteral("Could not push the account databases."));
        resume();
        return false;
    }
    csh(sp, ctx,
        QStringLiteral("chown system:system /data/system_ce/0/accounts_ce.db /data/system_de/0/accounts_de.db;"
                       "chmod 660 /data/system_ce/0/accounts_ce.db /data/system_de/0/accounts_de.db;"
                       "restorecon /data/system_ce/0/accounts_ce.db /data/system_de/0/accounts_de.db 2>/dev/null; true"));

    // 3) Restart the framework and wait for it.
    note(QStringLiteral("Restarting the Android framework…"));
    resume();
    for (int i = 0; i < 30; ++i) {
        if (csh(sp, ctx, QStringLiteral("pidof system_server >/dev/null")).rc == 0) break;
        sp.waitMs(2000);
    }
    sp.waitMs(4000);

    // 4) Re-arm the LOGGED_IN pointer (the framework restart reset it) and launch.
    note(QStringLiteral("Arming the session and launching Reddit…"));
    csh(sp, ctx, QStringLiteral("am force-stop ") + kPkg);
    for (const RedditFile &f : art.datastoreFiles)
        if (f.name == QStringLiteral("com.reddit.auth_active.preferences_pb"))
            pushSessionFile(sp, ctx, tmp, f, dsdir, uid, log);
    csh(sp, ctx, QStringLiteral("am start -n %1/com.reddit.launch.main.MainActivity").arg(kPkg));

    note(QStringLiteral("Done — Reddit should now be logged in as %1.").arg(username));
    return true;
}

}  // namespace remora
