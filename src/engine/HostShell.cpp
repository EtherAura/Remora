#include "engine/HostShell.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <optional>

#include "engine/Engine.h"       // Spawner::sshArgv
#include "engine/SourceBuild.h"  // shellQuote

namespace remora {

QString noLocalShellMessage() {
    return QStringLiteral(
        "this runs on a Linux docker host, and on Windows there is no local one to run it on — "
        "set ssh_host on the profile (or build_host/source_host in remorarc)");
}

static bool onMainThread() {
    return QCoreApplication::instance() &&
           QThread::currentThread() == QCoreApplication::instance()->thread();
}

// Event-loop flags for a nested wait: on the GUI thread input is QUEUED rather than delivered, so
// the handler that called us cannot be re-entered by a second click. This is strictly MORE
// restrictive than what those handlers already do: every QMessageBox in them nests an event loop
// that does not exclude input.
static QEventLoop::ProcessEventsFlags nestedFlags() {
    return onMainThread() ? QEventLoop::ExcludeUserInputEvents : QEventLoop::AllEvents;
}

// The GUI used to call these from its own thread at fourteen sites. onBuildFromSource is the worst:
// it runs `df`, then `du -sk <tree>/out`, BEFORE it prompts — and that `du` measures 7.0s on the
// A16 tree's 110 GiB out/ with a warm cache and an idle disk, against a 60s timeout.
// QProcess::waitForFinished polls its OWN fd set, not the application's, so for that entire window
// the main loop dispatched nothing: no repaints, no D-Bus, and the compositor marked the window
// "not responding" (bd remora-51z). Off the main thread nothing is at stake, so block plainly.
bool waitForProcess(QProcess &p, int timeoutMs) {
    if (!onMainThread()) return p.waitForFinished(timeoutMs);
    if (p.state() == QProcess::NotRunning) return true;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&p, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (timeoutMs > 0) timer.start(timeoutMs);
    loop.exec(nestedFlags());
    return p.state() == QProcess::NotRunning;
}

// Start `cmd` on `host` over ssh, or locally under sh when `host` is blank. Remote commands are
// POSIX sh — but "ssh host cmd" hands cmd to the login shell, which may be fish and reject
// $(...)/[..]/{..} syntax. Run a remote `sh` and feed the command over stdin instead: no
// login-shell parsing, no quoting games.
//
// False when nothing started, with the reason in *why. Checked HERE rather than read off the exit
// code: a process that never started leaves exitCode() at Qt's default 0 with NormalExit, and the
// GUI-thread wait returns at once for a process that is not running, so a missing `ssh` or `sh`
// used to read as a command that succeeded and printed nothing.
static bool startShell(QProcess &p, const QString &host, const QString &cmd, QString *why) {
    if (host.isEmpty()) {
#ifdef Q_OS_WIN
        *why = noLocalShellMessage();
        return false;
#else
        p.start(QStringLiteral("sh"), {QStringLiteral("-c"), cmd});
#endif
    } else {
        p.start(QStringLiteral("ssh"),
                {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"), QStringLiteral("-o"),
                 QStringLiteral("ConnectTimeout=6"), host, QStringLiteral("sh")});
    }
    if (!p.waitForStarted(15000)) {
        *why = QStringLiteral("could not start %1: %2").arg(p.program(), p.errorString());
        return false;
    }
    if (!host.isEmpty()) {
        p.write(cmd.toUtf8() + '\n');
        p.closeWriteChannel();
    }
    return true;
}

QString runShell(const QString &host, const QString &cmd, int timeoutMs) {
    QProcess p;
    QString why;
    if (!startShell(p, host, cmd, &why)) return QString();
    if (!waitForProcess(p, timeoutMs) || p.exitStatus() != QProcess::NormalExit ||
        p.exitCode() != 0)
        return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
}

ShellResult runShellRc(const QString &host, const QString &cmd, int timeoutMs) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QString why;
    if (!startShell(p, host, cmd, &why)) return {127, why};
    if (!waitForProcess(p, timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        return {124, QString::fromUtf8(p.readAll())};
    }
    const int rc = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 128;
    return {rc, QString::fromUtf8(p.readAll())};
}

QStringList hostCommandArgv(const HostCommand &c) {
    if (c.argv.isEmpty()) return {};
    if (c.host.isEmpty()) return c.argv;
    QStringList words;
    for (const QString &a : c.argv) words << shellQuote(a);
    QStringList a = Spawner::sshArgv(c.host, words.join(QLatin1Char(' ')), 6);
    a.insert(1, QStringLiteral("BatchMode=yes"));
    a.insert(1, QStringLiteral("-o"));
    return a;
}

// A blank host is THIS machine, which on Windows runs nothing of a docker pipeline.
static std::optional<QString> unrunnable(const HostCommand &c) {
    if (c.argv.isEmpty()) return QStringLiteral("empty command");
#ifdef Q_OS_WIN
    if (c.host.isEmpty()) return noLocalShellMessage();
#endif
    return std::nullopt;
}

static bool startArgv(QProcess &p, const QStringList &argv, QString *why) {
    p.start(argv.first(), argv.mid(1));
    if (p.waitForStarted(15000)) return true;
    *why = QStringLiteral("could not start %1: %2").arg(argv.first(), p.errorString());
    return false;
}

static int exitOf(const QProcess &p) {
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 128;
}

ShellResult runHostPipe(const HostCommand &producer, const HostCommand &consumer, int timeoutMs) {
    if (auto bad = unrunnable(producer)) return {127, *bad};
    if (auto bad = unrunnable(consumer)) return {127, *bad};
    // The producer's stderr goes to a FILE, not a pipe: nothing reads it until the end, and
    // waitForFinished on the consumer services only the consumer's own pipes — a chatty producer
    // would fill 64 KiB of pipe and stall the whole transfer.
    QTemporaryFile errFile;
    if (!errFile.open()) return {127, QStringLiteral("could not create a temporary file")};
    errFile.close();
    QProcess prod, cons;
    prod.setStandardOutputProcess(&cons);
    prod.setStandardErrorFile(errFile.fileName());
    cons.setProcessChannelMode(QProcess::MergedChannels);
    QString why;
    if (!startArgv(prod, hostCommandArgv(producer), &why)) return {127, why};
    if (!startArgv(cons, hostCommandArgv(consumer), &why)) {
        prod.kill();
        prod.waitForFinished(2000);
        return {127, why};
    }
    const QDeadlineTimer deadline = timeoutMs < 0 ? QDeadlineTimer(QDeadlineTimer::Forever)
                                                  : QDeadlineTimer(qint64(timeoutMs));
    const auto left = [&] { return deadline.isForever() ? -1 : int(deadline.remainingTime()); };
    const bool consDone = waitForProcess(cons, left());
    const bool prodDone = consDone && waitForProcess(prod, qMax(left(), 2000));
    const auto stderrOf = [&] {
        QFile f(errFile.fileName());
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    if (!consDone || !prodDone) {
        prod.kill();
        cons.kill();
        prod.waitForFinished(2000);
        cons.waitForFinished(2000);
        return {124, stderrOf() + QString::fromUtf8(cons.readAll())};
    }
    const QString out = stderrOf() + QString::fromUtf8(cons.readAll());
    // pipefail's rule — the rightmost failure — because a consumer that refused the stream makes
    // the producer die of a closed pipe, and that EPIPE is the symptom, not the reason.
    const int crc = exitOf(cons);
    return {crc != 0 ? crc : exitOf(prod), out};
}

ShellResult runHttpToHost(const QString &url, const HostCommand &consumer, int timeoutMs) {
    if (auto bad = unrunnable(consumer)) return {127, *bad};
    QProcess cons;
    cons.setProcessChannelMode(QProcess::MergedChannels);
    QString why;
    if (!startArgv(cons, hostCommandArgv(consumer), &why)) return {127, why};

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    // curl -L's behaviour, minus the downgrade: follow a mirror's redirect to its CDN, never from
    // https to http.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam.get(req);
    // Backpressure, both ways: the reply buffers at most this much before Qt stops reading the
    // socket, and the pump stops reading the reply while the consumer is this far behind. A
    // multi-GB archive therefore never sits in memory.
    constexpr qint64 kChunk = 1 << 20, kHighWater = 8 << 20;
    reply->setReadBufferSize(4 << 20);

    QEventLoop loop;
    QString failure;
    bool replyDone = false, inputClosed = false, timedOut = false, consumerEnded = false;
    const auto fail = [&](const QString &f) {
        if (failure.isEmpty()) failure = f;
        reply->abort();
        if (cons.state() != QProcess::NotRunning) cons.kill();
    };
    const auto statusError = [&]() -> QString {
        const QVariant st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (st.isValid() && st.toInt() >= 400)
            return QStringLiteral("%1 answered HTTP %2").arg(url).arg(st.toInt());
        return QString();
    };
    const auto pump = [&] {
        if (!failure.isEmpty()) return;
        // Never a byte of an error page into the consumer: `docker load` would report it as a
        // corrupt archive, and the real reason would be lost.
        if (const QString e = statusError(); !e.isEmpty()) return fail(e);
        while (reply->bytesAvailable() > 0 && cons.bytesToWrite() < kHighWater) {
            const QByteArray chunk = reply->read(kChunk);
            if (chunk.isEmpty()) break;
            cons.write(chunk);
        }
        if (replyDone && reply->bytesAvailable() == 0 && !inputClosed) {
            inputClosed = true;
            cons.closeWriteChannel();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, pump);
    QObject::connect(&cons, &QProcess::bytesWritten, &loop, pump);
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&] {
        replyDone = true;
        // An abort because the consumer already ended is not a network failure: its own exit
        // code and output say why, and they are what gets returned.
        if (reply->error() != QNetworkReply::NoError && failure.isEmpty() && !consumerEnded) {
            const QString e = statusError();
            return fail(e.isEmpty() ? QStringLiteral("%1: %2").arg(url, reply->errorString()) : e);
        }
        pump();
    });
    // The consumer ending is the end, whichever way it ends: early means it refused the stream.
    QObject::connect(&cons, &QProcess::finished, &loop, [&] {
        consumerEnded = true;
        if (!replyDone) reply->abort();
        loop.quit();
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        fail(QStringLiteral("timed out"));
    });
    if (timeoutMs > 0) timer.start(timeoutMs);
    if (cons.state() != QProcess::NotRunning) loop.exec(nestedFlags());
    cons.waitForFinished(2000);
    const QString out = QString::fromUtf8(cons.readAll());
    if (timedOut) return {124, out};
    if (!failure.isEmpty()) return {22, failure + (out.isEmpty() ? QString() : '\n' + out)};
    return {exitOf(cons), out};
}

QByteArray httpGetSmall(const QString &url, int timeoutMs, QString *error) {
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec(nestedFlags());
    QByteArray body;
    const QVariant st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (reply->error() != QNetworkReply::NoError || (st.isValid() && st.toInt() >= 400)) {
        if (error)
            *error = st.isValid() && st.toInt() >= 400
                         ? QStringLiteral("%1 answered HTTP %2").arg(url).arg(st.toInt())
                         : QStringLiteral("%1: %2").arg(url, reply->errorString());
    } else {
        body = reply->readAll();
    }
    return body;
}

}  // namespace remora
