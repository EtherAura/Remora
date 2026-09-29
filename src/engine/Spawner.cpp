#include "engine/Engine.h"

#include <csignal>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

namespace remora {

void Spawner::waitMs(int ms) {
    if (ms > 0) QThread::msleep(static_cast<unsigned long>(ms));
}

// POSIX single-quoting: wrap in '…' and rewrite every embedded ' as '\'' (close, escaped quote,
// reopen). Seven of the ssh payloads carry single quotes already — `docker ps --format
// '{{.Names}}'` and friends — so naive wrapping would corrupt exactly the commands that work
// today. fish parses this identically to sh: it accepts \' outside quotes and concatenates
// adjacent tokens, so the same escaping survives both.
static QString shSingleQuote(const QString &s) {
    QString q = s;
    q.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QStringList Spawner::sshArgv(const QString &host, const QString &remote, int connectTimeout) {
    QStringList a{"ssh"};
    if (connectTimeout > 0)
        a << "-o" << QStringLiteral("ConnectTimeout=%1").arg(connectTimeout);
    // ssh joins the remaining arguments and hands them to the remote account's LOGIN shell, which
    // is not necessarily POSIX. These payloads are sh snippets, so a fish or csh account cannot
    // parse them — and the failure is SILENT: the probe reads the empty output as "capability
    // absent", so a host with an NVIDIA node and a full Venus install reports neither, venus
    // auto-detection declines, and gpu_mode=host half-applies (bd remora-4ei.68). Name the
    // interpreter instead of inheriting whatever the account happens to use.
    a << host << QStringLiteral("/bin/sh") << QStringLiteral("-c") << shSingleQuote(remote);
    return a;
}

static QProcessEnvironment mergedEnv(const QMap<QString, QString> &env) {
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    for (auto it = env.constBegin(); it != env.constEnd(); ++it)
        e.insert(it.key(), it.value());
    return e;
}

ProcResult RealSpawner::run(const QStringList &argv, const QMap<QString, QString> &env,
                            const LineSink &onLine) {
    if (argv.isEmpty()) return {127, {}, QStringLiteral("empty argv")};
    QProcess p;
    if (!env.isEmpty()) p.setProcessEnvironment(mergedEnv(env));
    p.setProgram(argv[0]);
    p.setArguments(argv.mid(1));
    p.start();
    if (!p.waitForStarted(5000))
        return {127, {}, QStringLiteral("failed to start %1").arg(argv[0])};

    // Stream output line-by-line WHILE the process runs (50ms ticks on this worker thread —
    // never the GUI thread). Long chain steps report progress mid-run (image pulls, source
    // builds), and delivering lines only at exit left the log silent until each one finished.
    // Buffers stay QByteArray so a UTF-8 sequence split across read chunks can't be mangled —
    // only complete lines are decoded.
    QByteArray outAll, errAll, outBuf, errBuf;
    const auto drain = [&](bool final) {
        const QByteArray o = p.readAllStandardOutput();
        const QByteArray e = p.readAllStandardError();
        outAll += o;
        errAll += e;
        if (!onLine) return;
        outBuf += o;
        errBuf += e;
        const auto emitLines = [&](QByteArray &buf, const QString &stream) {
            int nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
                const QByteArray l = buf.left(nl);
                buf.remove(0, nl + 1);
                if (!l.isEmpty()) onLine(stream, QString::fromUtf8(l));
            }
            if (final && !buf.isEmpty()) {
                onLine(stream, QString::fromUtf8(buf));
                buf.clear();
            }
        };
        emitLines(outBuf, QStringLiteral("stdout"));
        emitLines(errBuf, QStringLiteral("stderr"));
    };
    while (!p.waitForFinished(50)) drain(false);
    drain(true);

    const QString err = QString::fromUtf8(errAll);
    QStringList errLines = err.split(QLatin1Char('\n'));
    if (errLines.size() > 64) errLines = errLines.mid(errLines.size() - 64);
    return {p.exitCode(), QString::fromUtf8(outAll),
            errLines.join(QLatin1Char('\n')).trimmed()};
}

QStringList Spawner::scopedArgv(const QStringList &argv, bool haveSystemdUser,
                                const QString &unit) {
    if (argv.isEmpty() || !haveSystemdUser || unit.isEmpty()) return argv;
    // --scope rather than a transient --service on purpose. A scope runs the command in THIS
    // process's context, so it keeps the environment and the stdout/stderr redirection the caller
    // set up; a service would be started by the manager with a clean environment and its output
    // would go to the journal, which would silently empty /tmp/remora-detached.log — the file
    // Chain.cpp tails to explain why the mirror died. systemd-run also execs the command in place, so
    // the pid startDetached() reports is still the real process and detachedAlive() keeps working.
    //
    // --quiet is load-bearing, not cosmetic: without it systemd-run announces the unit on stderr,
    // and that line lands in the very log tailed for the mirror's failure output.
    // --collect reaps the unit if the command fails, so failures don't accumulate as dead units.
    QStringList out{QStringLiteral("systemd-run"), QStringLiteral("--user"),
                    QStringLiteral("--scope"), QStringLiteral("--quiet"),
                    QStringLiteral("--collect"), QStringLiteral("--unit=") + unit};
    out += argv;
    return out;
}

// A systemd user manager is only reachable if systemd-run exists AND the per-user socket is there.
// Checking $XDG_RUNTIME_DIR alone is not enough — it is set in plenty of sessions with no manager.
static bool systemdUserAvailable() {
    if (QStandardPaths::findExecutable(QStringLiteral("systemd-run")).isEmpty()) return false;
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (rt.isEmpty()) return false;
    return QFileInfo::exists(QString::fromLocal8Bit(rt) + QStringLiteral("/systemd/private"));
}

Detached RealSpawner::spawnDetached(const QStringList &argv, const QMap<QString, QString> &env) {
    // QProcess::startDetached is the C++ equivalent of a detached Popen: the child is NOT owned by
    // any event loop and survives this process — the fix for the mirror-window-killed bug. That
    // detaches the process but NOT the cgroup, so scopedArgv() additionally lifts it out of the
    // launching app unit (bd remora-4ei.29).
    if (argv.isEmpty()) return {0};
    static QAtomicInt seq = 0;
    const QString unit = QStringLiteral("remora-detached-%1-%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(seq.fetchAndAddOrdered(1));
    const QStringList cmd = scopedArgv(argv, systemdUserAvailable(), unit);
    QProcess p;
    if (!env.isEmpty()) p.setProcessEnvironment(mergedEnv(env));
    p.setProgram(cmd[0]);
    p.setArguments(cmd.mid(1));
    // Keep the child's output: when the mirror dies immediately, its last lines are the only
    // evidence of why — doConnect tails this file into the failure message.
    // Per-spawn log, plus the shared one. The shared truncate-on-spawn design ate the evidence
    // THREE times this project (venus startup lines, SurfaceFlinger's context announce, and the
    // boot splash's death reason — bd remora-e5x.16): whichever child spawned LAST owned the file,
    // and every earlier child's last words vanished exactly when they were needed. Each child now
    // also gets its own file keyed by the transient unit name, so a death can always be read.
    // BOTH fds append: the file is unique per spawn, so Truncate buys nothing — and a truncated
    // stdout is not O_APPEND, so its offset starts at 0 and every stdout write OVERWRITES the
    // stderr lines already at those offsets. That clobbering ate the mirror's whole initializeGL
    // block while later lines survived, which read as "these lines never printed" during the
    // bd remora-j6yf autopsy (bd remora-cpzc).
    const QString ownLog = QStringLiteral("/tmp/%1.log").arg(unit);
    p.setStandardOutputFile(ownLog, QIODevice::Append);
    p.setStandardErrorFile(ownLog, QIODevice::Append);
    qint64 pid = 0;
    p.startDetached(&pid);
    return {pid};
}

bool RealSpawner::detachedAlive(const Detached &d) {
    if (d.pid <= 0) return false;
    return ::kill(static_cast<pid_t>(d.pid), 0) == 0;
}

}  // namespace remora
