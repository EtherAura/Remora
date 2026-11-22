#include "engine/MirrorRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace remora {

QString mirrorTunnelLockPath(quint16 port) {
#ifdef Q_OS_WIN
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                        QStringLiteral("/remora/mirrors");
    QDir().mkpath(dir);
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#endif
    return dir + QStringLiteral("/remora-mirror-tunnel-%1.lock").arg(port);
}

QString mirrorRegistryDir() {
#ifdef Q_OS_WIN
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/remora/mirrors");
#else
    // The tunnel locks' rule (MirrorSession.cpp), so the two kinds of per-session state sit side
    // by side and go away together.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return dir + QStringLiteral("/remora-mirrors");
#endif
}

namespace {

enum class LockState { Held, Free, Absent, Unknown };

QString lockPathOf(const QString &dir, const QString &id) {
    return dir + QLatin1Char('/') + id + QStringLiteral(".lock");
}
QString entryPathOf(const QString &dir, const QString &id) {
    return dir + QLatin1Char('/') + id + QStringLiteral(".json");
}

#ifdef Q_OS_WIN
using NativeLock = HANDLE;
const NativeLock kNoLock = INVALID_HANDLE_VALUE;

std::wstring nativePath(const QString &p) { return QDir::toNativeSeparators(p).toStdWString(); }

// A handle that shares READ (and DELETE) but never WRITE: while it is open, nobody can open the
// file for writing, which is the probe. DELETE_ON_CLOSE makes the kernel remove the file when the
// handle goes — on any exit, TerminateProcess included — so a dead mirror leaves no lock behind.
NativeLock takeLock(const QString &path) {
    return ::CreateFileW(nativePath(path).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                         FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
}

void dropLock(NativeLock h) {
    if (h != kNoLock) ::CloseHandle(h);  // DELETE_ON_CLOSE removes the file
}

LockState probeLock(const QString &path) {
    const HANDLE h = ::CreateFileW(nativePath(path).c_str(), GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SHARING_VIOLATION) return LockState::Held;
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return LockState::Absent;
        // ERROR_ACCESS_DENIED among them: a lock whose owner is mid-exit is delete-pending and
        // refuses every open until it is gone — not alive enough to trust, nor dead enough to
        // clean up after.
        return LockState::Unknown;
    }
    ::CloseHandle(h);
    return LockState::Free;
}
#else
using NativeLock = int;
const NativeLock kNoLock = -1;

// flock(2) rather than fcntl locks: an flock belongs to the open file description, so it is not
// silently dropped when some unrelated code in this process closes another fd on the same file,
// and it is released by the kernel when the process dies. Taken under a temporary name and only
// then renamed into place, so no reader can ever see the lock file unlocked while its owner is
// alive (a probe in the gap between open and flock would have read it as stale and deleted it).
NativeLock takeLock(const QString &path) {
    const QByteArray tmp = QFile::encodeName(path + QStringLiteral(".tmp"));
    const int fd = ::open(tmp.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return kNoLock;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0
        || ::rename(tmp.constData(), QFile::encodeName(path).constData()) != 0) {
        ::unlink(tmp.constData());
        ::close(fd);
        return kNoLock;
    }
    return fd;
}

void dropLock(NativeLock fd) {
    if (fd >= 0) ::close(fd);
}

LockState probeLock(const QString &path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? LockState::Absent : LockState::Unknown;
    const int r = ::flock(fd, LOCK_SH | LOCK_NB);
    const int err = errno;
    ::close(fd);
    if (r == 0) return LockState::Free;
    return err == EWOULDBLOCK ? LockState::Held : LockState::Unknown;
}
#endif

// Everything one id left behind: the entry, the lock, and a QSaveFile temporary a crash
// interrupted.
void removeRegistration(const QDir &d, const QString &id) {
    for (const QString &f :
         d.entryList({id + QStringLiteral(".*")}, QDir::Files | QDir::Hidden | QDir::System))
        QFile::remove(d.filePath(f));
}

}  // namespace

struct MirrorRegistration::Impl {
    QString dir, id;
    MirrorEntry entry;
    QString error;
    NativeLock lock = kNoLock;

    bool writeEntry() {
        // Atomic replace, so a reader sees the old entry or the new one and never half of either.
        // Retried because Windows refuses to replace a file another process has open for reading
        // — a lookup that happened to be reading it at that instant.
        for (int attempt = 0; attempt < 5; ++attempt) {
            QSaveFile f(entryPathOf(dir, id));
            if (f.open(QIODevice::WriteOnly) && f.write(encodeMirrorEntry(entry)) >= 0
                && f.commit())
                return true;
            error = f.errorString();
            QThread::msleep(20);
        }
        return false;
    }
};

MirrorRegistration::MirrorRegistration(const MirrorEntry &self, const QString &dir)
    : d_(std::make_unique<Impl>()) {
    d_->dir = dir;
    d_->entry = self;
    d_->entry.registered = true;
    d_->id = QStringLiteral("%1-%2").arg(self.pid).arg(QRandomGenerator::global()->generate64(),
                                                       16, 16, QLatin1Char('0'));
    if (!QDir().mkpath(dir)) {
        d_->error = QStringLiteral("cannot create %1").arg(dir);
        return;
    }
    // The lock FIRST, the entry second: an entry only ever exists beside a held lock, so a reader
    // never takes a starting mirror for a dead one.
    d_->lock = takeLock(lockPathOf(dir, d_->id));
    if (d_->lock == kNoLock) {
        d_->error = QStringLiteral("cannot lock %1").arg(lockPathOf(dir, d_->id));
        return;
    }
    if (!d_->writeEntry()) {
        dropLock(d_->lock);
        d_->lock = kNoLock;
        removeRegistration(QDir(dir), d_->id);
    }
}

MirrorRegistration::~MirrorRegistration() {
    if (d_->lock == kNoLock) return;
    // Entry first, then the lock: an entry without a held lock is what a crash looks like.
    QFile::remove(entryPathOf(d_->dir, d_->id));
#ifndef Q_OS_WIN
    QFile::remove(lockPathOf(d_->dir, d_->id));  // Windows: DELETE_ON_CLOSE does it
#endif
    dropLock(d_->lock);
}

bool MirrorRegistration::ok() const { return d_->lock != kNoLock; }
QString MirrorRegistration::error() const { return d_->error; }

void MirrorRegistration::setSession(MirrorSessionState s) {
    if (!ok() || d_->entry.session == s) return;
    d_->entry.session = s;
    d_->writeEntry();
}

QVector<MirrorEntry> readMirrorRegistry(const QString &dir) {
    const QDir d(dir);
    if (!d.exists()) return {};
    QVector<MirrorEntry> out;
    QStringList withEntry;
    for (const QString &name : d.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        const QString id = name.chopped(5);
        withEntry << id;
        const LockState st = probeLock(lockPathOf(dir, id));
        if (st == LockState::Free || st == LockState::Absent) {
            removeRegistration(d, id);  // its owner is gone: a crash, a KILL, a power cut
            continue;
        }
        if (st != LockState::Held) continue;
        QFile f(d.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;  // removed between the listing and now
        const std::optional<MirrorEntry> e = decodeMirrorEntry(f.readAll());
        // The id leads with the pid its owner wrote; a disagreement is a damaged entry, and a
        // damaged entry's pid is not one to stop.
        if (!e || QString::number(e->pid) != id.section(QLatin1Char('-'), 0, 0)) continue;
        out << *e;
    }
    // A lock with no entry is a mirror that removed its entry on the way out and died before it
    // let go — or one still registering, which holds its lock and is left alone.
    for (const QString &name : d.entryList({QStringLiteral("*.lock")}, QDir::Files)) {
        const QString id = name.chopped(5);
        if (!withEntry.contains(id) && probeLock(d.filePath(name)) == LockState::Free)
            removeRegistration(d, id);
    }
    std::sort(out.begin(), out.end(), [](const MirrorEntry &a, const MirrorEntry &b) {
        return a.startedMs != b.startedMs ? a.startedMs < b.startedMs : a.pid < b.pid;
    });
    return out;
}

QVector<MirrorEntry> scanMirrorProcesses(const QString &procRoot) {
    QVector<MirrorEntry> out;
#ifdef Q_OS_LINUX
    // /proc itself rather than pgrep: what pgrep -f reads, minus the joined-string ambiguity.
    // A title carries spaces ("Remora PIP — A 17"), and only the NUL-separated cmdline says where
    // one argument ends — which is also why no [b]racket self-exclusion trick is needed here.
    const qint64 self = QCoreApplication::applicationPid();
    const QDir proc(procRoot);
    for (const QString &name : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const qint64 pid = name.toLongLong(&ok);
        if (!ok || pid <= 1 || pid == self) continue;
        QFile f(proc.filePath(name + QStringLiteral("/cmdline")));
        if (!f.open(QIODevice::ReadOnly)) continue;  // gone, or not ours to read
        const QByteArray raw = f.readAll();
        if (!raw.contains("mirror")) continue;  // nearly everything; skip the decode
        QStringList argv;
        for (const QByteArray &a : raw.split('\0')) argv << QString::fromLocal8Bit(a);
        if (!argv.isEmpty() && argv.last().isEmpty()) argv.removeLast();  // the trailing NUL
        if (std::optional<MirrorEntry> e = mirrorEntryFromArgv(argv)) {
            e->pid = pid;
            out << *e;
        }
    }
#else
    (void)procRoot;
#endif
    return out;
}

}  // namespace remora
