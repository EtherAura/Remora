#include "engine/Engine.h"

#include "core/ManagedHost.h"

#include <algorithm>
#include <csignal>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include "engine/MirrorRegistry.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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
    // Remora's own guest has a reserved name, not an address: it is reached on the loopback
    // port QEMU forwards, with the key and pinned host key Remora made for it (bd remora-7oih.5).
    QString dest = host;
    if (const auto m = managedHost(host)) {
        a << managedSshOptions(*m, false);
        dest = managedSshDestination(*m);
    }
    // ssh joins the remaining arguments and hands them to the remote account's LOGIN shell, which
    // is not necessarily POSIX. These payloads are sh snippets, so a fish or csh account cannot
    // parse them — and the failure is SILENT: the probe reads the empty output as "capability
    // absent", so a host with an NVIDIA node and a full Venus install reports neither, venus
    // auto-detection declines, and gpu_mode=host half-applies (bd remora-4ei.68). Name the
    // interpreter instead of inheriting whatever the account happens to use.
    a << dest << QStringLiteral("/bin/sh") << QStringLiteral("-c") << shSingleQuote(remote);
    return a;
}

QStringList Spawner::scpArgv(const QStringList &operands, const QStringList &flags) {
    QStringList a{QStringLiteral("scp")};
    a << flags;
    QStringList ops;
    std::optional<ManagedHost> managed;
    for (const QString &op : operands) {
        // host:path, where host is everything before the first ':' and holds no '/' or '\'.
        // C:\x has a one-letter "host", and no managed host is ever called that.
        const int colon = op.indexOf(QLatin1Char(':'));
        const QString host = colon > 0 ? op.left(colon) : QString();
        const auto m = (!host.isEmpty() && !host.contains(QLatin1Char('/')) &&
                        !host.contains(QLatin1Char('\\')))
                           ? managedHost(host)
                           : std::nullopt;
        if (m) {
            managed = m;
            ops << managedSshDestination(*m) + op.mid(colon);
        } else {
            ops << op;
        }
    }
    if (managed) a << managedSshOptions(*managed, true);
    a << QStringLiteral("--") << ops;
    return a;
}

static QProcessEnvironment mergedEnv(const QMap<QString, QString> &env) {
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    for (auto it = env.constBegin(); it != env.constEnd(); ++it)
        e.insert(it.key(), it.value());
    return e;
}

// One implementation behind run(), runTimed() and runToFile(): a deadline (0 = none) and a file
// for raw stdout (empty = collect it) are the only things that differ between them.
static ProcResult runProcess(const QStringList &argv, const QMap<QString, QString> &env,
                             const LineSink &onLine, int timeoutMs, const QString &stdoutFile) {
    if (argv.isEmpty()) return {127, {}, QStringLiteral("empty argv")};
    QProcess p;
    if (!env.isEmpty()) p.setProcessEnvironment(mergedEnv(env));
    if (!stdoutFile.isEmpty()) p.setStandardOutputFile(stdoutFile, QIODevice::Truncate);
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
    QElapsedTimer clock;
    clock.start();
    bool timedOut = false;
    while (!p.waitForFinished(50)) {
        drain(false);
        if (timeoutMs > 0 && clock.elapsed() >= timeoutMs) {
            // TERM first, as `timeout` sends; a child that ignores it (and every console program
            // on Windows, where terminate() is a WM_CLOSE) is killed outright.
            timedOut = true;
            p.terminate();
            if (!p.waitForFinished(1000)) {
                p.kill();
                p.waitForFinished(1000);
            }
            break;
        }
    }
    drain(true);

    const QString err = QString::fromUtf8(errAll);
    QStringList errLines = err.split(QLatin1Char('\n'));
    if (errLines.size() > 64) errLines = errLines.mid(errLines.size() - 64);
    return {timedOut ? 124 : p.exitCode(), QString::fromUtf8(outAll),
            errLines.join(QLatin1Char('\n')).trimmed()};
}

ProcResult RealSpawner::run(const QStringList &argv, const QMap<QString, QString> &env,
                            const LineSink &onLine) {
    return runProcess(argv, env, onLine, 0, QString());
}

ProcResult RealSpawner::runTimed(const QStringList &argv, int timeoutMs, const LineSink &onLine) {
    return runProcess(argv, {}, onLine, timeoutMs, QString());
}

ProcResult RealSpawner::runToFile(const QStringList &argv, const QString &outPath) {
    return runProcess(argv, {}, {}, 0, outPath);
}

ProcResult Spawner::runTimed(const QStringList &argv, int, const LineSink &onLine) {
    return run(argv, {}, onLine);
}

ProcResult Spawner::runToFile(const QStringList &argv, const QString &outPath) {
    const ProcResult r = run(argv, {}, {});
    QFile f(outPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(r.out.toUtf8());
    return r;
}

QString Spawner::readTail(const QString &path, int lines) {
    QFile f(path);
    if (lines <= 0 || !f.open(QIODevice::ReadOnly)) return QString();
    // From the end, widening until the window holds enough lines: these logs grow for as long as
    // the process lives (the render server writes a line per composed frame), so reading the
    // whole file to keep its last few hundred lines would cost more the longer a session ran.
    const qint64 size = f.size();
    QByteArray buf;
    for (qint64 window = 16 * 1024;; window *= 4) {
        const qint64 from = qMax<qint64>(0, size - window);
        if (!f.seek(from)) return QString();
        buf = f.read(size - from);
        // A trailing newline ends the last line rather than starting another, as for `tail`.
        const int nl = int(buf.count('\n')) - (buf.endsWith('\n') ? 1 : 0);
        if (from == 0 || nl >= lines) break;
    }
    qsizetype start = buf.size() - (buf.endsWith('\n') ? 2 : 1);
    for (int seen = 0; start >= 0; --start)
        if (buf.at(start) == '\n' && ++seen == lines) break;
    return QString::fromUtf8(buf.mid(start + 1));
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
// Windows has neither, and needs no scope: a detached child outlives its parent there unless it
// was put in a job that says otherwise.
static bool systemdUserAvailable() {
#ifdef Q_OS_WIN
    return false;
#endif
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
    //
    // /tmp by name on Linux, not QDir::tempPath(): parseDetachedLogPath rebuilds this path from the
    // scope name alone, so honouring $TMPDIR here would put the log where nothing looks for it.
    // Windows has no /tmp, and no scope to rebuild a path from.
#ifdef Q_OS_WIN
    const QString ownLog = QDir(QDir::tempPath()).filePath(unit + QStringLiteral(".log"));
    // No console window: the output goes to the log, and startDetached would otherwise open a
    // fresh console for every console-subsystem child — remora.exe itself is one.
    //
    // And the child inherits ONLY the handles Qt set up for it. Windows otherwise hands a child
    // every inheritable handle this process holds — remora.exe's own stdout among them — and a
    // detached child outlives us holding it: `remora guest start > log` left QEMU with the log
    // open, so the next `>> log` in the same script failed and cmd skipped the command (seen
    //), and a pipe into one would never see EOF. PROC_THREAD_ATTRIBUTE_HANDLE_LIST is
    // the documented way to say exactly which handles go.
    struct Inherit {
        STARTUPINFOEXW si{};
        QByteArray attrs;
        HANDLE handles[3] = {};
        bool ready = false;
        ~Inherit() {
            if (ready)
                DeleteProcThreadAttributeList(
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data()));
        }
    } inherit;
    p.setCreateProcessArgumentsModifier([&inherit](QProcess::CreateProcessArguments *a) {
        a->flags = (a->flags & ~DWORD(CREATE_NEW_CONSOLE)) | CREATE_NO_WINDOW;
        STARTUPINFOW *base = a->startupInfo;
        int n = 0;
        for (HANDLE h : {base->hStdInput, base->hStdOutput, base->hStdError}) {
            DWORD info = 0;
            if (h && h != INVALID_HANDLE_VALUE && GetHandleInformation(h, &info) &&
                (info & HANDLE_FLAG_INHERIT) &&
                std::find(inherit.handles, inherit.handles + n, h) == inherit.handles + n)
                inherit.handles[n++] = h;
        }
        if (n == 0) {
            a->inheritHandles = false;
            return;
        }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        inherit.attrs.resize(qsizetype(size));
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(inherit.attrs.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return;
        inherit.ready = true;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.handles,
                                       SIZE_T(n) * sizeof(HANDLE), nullptr, nullptr))
            return;
        inherit.si.StartupInfo = *base;
        inherit.si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        inherit.si.lpAttributeList = list;
        a->startupInfo = &inherit.si.StartupInfo;
        a->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });
#else
    const QString ownLog = QStringLiteral("/tmp/%1.log").arg(unit);
#endif
    p.setStandardOutputFile(ownLog, QIODevice::Append);
    p.setStandardErrorFile(ownLog, QIODevice::Append);
    qint64 pid = 0;
    p.startDetached(&pid);
    return {pid, ownLog};
}

bool RealSpawner::detachedAlive(const Detached &d) {
    if (d.pid <= 0) return false;
#ifdef Q_OS_WIN
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(d.pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
#else
    return ::kill(static_cast<pid_t>(d.pid), 0) == 0;
#endif
}

QVector<MirrorEntry> RealSpawner::mirrors() {
    QVector<MirrorEntry> all = readMirrorRegistry();
    // The upgrade fallback (Linux only; empty elsewhere): a mirror an older Remora started never
    // registered, and without this an upgrade would leave every running mirror invisible — the
    // next reconnect opening a second client beside it, auto-sleep freezing a device being
    // watched. A registered mirror shows up here too and keeps its entry, which knows more.
    for (const MirrorEntry &e : scanMirrorProcesses()) {
        bool known = false;
        for (const MirrorEntry &r : all)
            if (r.pid == e.pid) { known = true; break; }
        if (!known) all << e;
    }
    return all;
}

#ifdef Q_OS_WIN
// What `taskkill` without /F does: WM_CLOSE to every top-level window the process owns. It only
// reaches windows on this desktop, which is where the GUI and its mirrors live.
static BOOL CALLBACK postCloseToWindowsOf(HWND hwnd, LPARAM pid) {
    DWORD owner = 0;
    ::GetWindowThreadProcessId(hwnd, &owner);
    if (owner == DWORD(pid)) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}
#endif

QList<qint64> RealSpawner::terminate(const QList<qint64> &pids, int graceMs, bool force) {
    const qint64 self = QCoreApplication::applicationPid();
    QList<qint64> wanted;
    // pid 0 and -1 are not processes to kill(2) but "my process group" and "everything I may
    // signal"; pid 1 is init. None of them is ever a mirror, so none of them is ever asked.
    for (qint64 p : pids)
        if (p > 1 && p != self && !wanted.contains(p)) wanted << p;
#ifdef Q_OS_WIN
    struct Proc {
        qint64 pid;
        HANDLE h;
    };
    QVector<Proc> procs;
    for (qint64 p : wanted) {
        const HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, DWORD(p));
        if (!h) continue;  // already gone (or never ours)
        ::EnumWindows(postCloseToWindowsOf, LPARAM(p));
        const std::wstring ev = mirrorStopEventName(p).toStdWString();
        if (const HANDLE e = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, ev.c_str())) {
            ::SetEvent(e);
            ::CloseHandle(e);
        }
        procs.push_back({p, h});
    }
    const QDeadlineTimer grace(qMax(0, graceMs));
    for (const Proc &pr : procs)
        ::WaitForSingleObject(pr.h, DWORD(qMax<qint64>(0, grace.remainingTime())));
    QList<qint64> left;
    for (const Proc &pr : procs) {
        if (::WaitForSingleObject(pr.h, 0) == WAIT_TIMEOUT && force) {
            // No teardown runs: the adb forward leaks, and the next session's start-up sweep
            // reclaims it by its tunnel lock (MirrorSession::sweepStaleTunnels).
            ::TerminateProcess(pr.h, 1);
            ::WaitForSingleObject(pr.h, 1000);
        }
        if (::WaitForSingleObject(pr.h, 0) == WAIT_TIMEOUT) left << pr.pid;
        ::CloseHandle(pr.h);
    }
    return left;
#else
    QList<qint64> live;
    for (qint64 p : wanted)
        if (::kill(pid_t(p), SIGTERM) == 0) live << p;
    const auto settle = [&live](int ms) {
        const QDeadlineTimer dl(qMax(0, ms));
        for (;;) {
            live.erase(std::remove_if(live.begin(), live.end(),
                                      [](qint64 p) { return ::kill(pid_t(p), 0) != 0; }),
                       live.end());
            if (live.isEmpty() || dl.hasExpired()) return;
            QThread::msleep(50);
        }
    };
    settle(graceMs);
    if (force && !live.isEmpty()) {
        for (qint64 p : live) ::kill(pid_t(p), SIGKILL);
        settle(1000);
    }
    return live;
#endif
}

}  // namespace remora
