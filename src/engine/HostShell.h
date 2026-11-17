#pragma once
#include <QString>
#include <QStringList>

class QProcess;

namespace remora {

// Blocking shell helpers for the workspace's image side: the library scan, prune, push and fetch.
// Each runs on a named host over ssh, or on this machine when the host is blank. Kept out of the
// GUI so their failure modes can be pinned offline (bd remora-7oih.3).

struct ShellResult {
    int rc;
    QString out;
};

// Why a blank host cannot run anything on Windows: every caller's payload is a POSIX sh snippet,
// usually around docker, and Windows has neither — its docker host is always reached over ssh.
QString noLocalShellMessage();

// Wait for a started QProcess WITHOUT wedging the GUI when called on the main thread: off it,
// block plainly; on it, run a nested event loop that excludes user input (see the .cpp).
bool waitForProcess(QProcess &p, int timeoutMs);

// `cmd` under sh on `host` (ssh, the command fed over stdin so the login shell never parses it)
// or here. runShell returns stdout, empty on ANY failure; runShellRc returns the exit code and
// the merged output, 127 when nothing could be started and 124 on a timeout.
QString runShell(const QString &host, const QString &cmd, int timeoutMs = 120000);
ShellResult runShellRc(const QString &host, const QString &cmd, int timeoutMs = 120000);

// One program run on `host` over ssh, or here when the host is blank — an ARGV, not a shell line,
// so the local side needs no sh at all. The far side runs it under /bin/sh -c (Spawner::sshArgv),
// which is what lets a pipeline stage keep its stdin for data.
struct HostCommand {
    QString host;
    QStringList argv;
};
// The argv actually started for `c`.
QStringList hostCommandArgv(const HostCommand &c);

// producer | consumer, piped by Qt (QProcess::setStandardOutputProcess) — what a local
// `sh -c "a | b"` did, without a local sh. The rc follows pipefail: the consumer's when it failed,
// else the producer's, so a producer that died half way never reads as a load of what it sent.
// `out` is the producer's stderr, then the consumer's merged output.
ShellResult runHostPipe(const HostCommand &producer, const HostCommand &consumer, int timeoutMs);

// GET `url` and stream the body into `consumer`'s stdin, never holding more than a few MiB of it:
// the http(s) half of a fetch, done with Qt rather than a local curl. Fails on any HTTP error
// status, so a 404 page is never handed to `docker load`.
ShellResult runHttpToHost(const QString &url, const HostCommand &consumer, int timeoutMs);

// GET `url` into memory, for small documents (a mirror's index.txt). Empty with *error set on any
// failure, including an HTTP error status.
QByteArray httpGetSmall(const QString &url, int timeoutMs, QString *error = nullptr);

}  // namespace remora
