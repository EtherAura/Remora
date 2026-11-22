#include "engine/SelfImage.h"

#include <QFile>
#include <cerrno>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace remora {

SelfImageOps realSelfImageOps() {
    SelfImageOps ops;
#ifdef Q_OS_UNIX
    ops.stat = [](const QString &path) -> std::optional<SelfFileId> {
        struct stat st {};
        if (::stat(QFile::encodeName(path).constData(), &st) != 0) return std::nullopt;
        SelfFileId id;
        id.dev = quint64(st.st_dev);
        id.ino = quint64(st.st_ino);
        id.size = qint64(st.st_size);
#ifdef Q_OS_LINUX
        id.mtimeNs = qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#else
        id.mtimeNs = qint64(st.st_mtime) * 1000000000;
#endif
        return id;
    };
    ops.executable = [](const QString &path) {
        const QByteArray p = QFile::encodeName(path);
        struct stat st {};
        return ::stat(p.constData(), &st) == 0 && S_ISREG(st.st_mode) &&
               ::access(p.constData(), X_OK) == 0;
    };
    ops.exec = [](const QString &path, const QStringList &argv) {
        std::vector<QByteArray> store;
        store.reserve(size_t(argv.size()));
        for (const QString &a : argv) store.push_back(QFile::encodeName(a));
        std::vector<char *> cargv;
        for (QByteArray &a : store) cargv.push_back(a.data());
        cargv.push_back(nullptr);
        ::execv(QFile::encodeName(path).constData(), cargv.data());
        return errno ? errno : EINVAL;
    };
#else
    ops.stat = [](const QString &) { return std::optional<SelfFileId>(); };
    ops.executable = [](const QString &) { return false; };
    ops.exec = [](const QString &, const QStringList &) { return EINVAL; };
#endif
    return ops;
}

QString launchPathFromArgv0(const QString &argv0, const QString &startDir) {
    if (argv0.isEmpty() || !argv0.contains(QLatin1Char('/'))) return {};
    if (argv0.startsWith(QLatin1Char('/'))) return argv0;
    // Joined, not cleaned or canonicalised: ".." after a symlinked directory means something else
    // lexically, and the final component may be a symlink the next install repoints.
    if (startDir.isEmpty() || !startDir.startsWith(QLatin1Char('/'))) return {};
    return startDir.endsWith(QLatin1Char('/')) ? startDir + argv0
                                               : startDir + QLatin1Char('/') + argv0;
}

SelfImageWatch SelfImageWatch::seeded(const QString &launchPath, const QStringList &argv,
                                      SelfImageOps ops, const QString &runningLink) {
    SelfImageWatch w;
    w.launchPath_ = launchPath;
    w.argv_ = argv;
    w.ops_ = std::move(ops);
    // The running image, from the executing-image link — never from the launch path.
    if (!launchPath.isEmpty() && w.ops_.stat) w.running_ = w.ops_.stat(runningLink);
    return w;
}

SelfImageWatch::Outcome SelfImageWatch::check() {
    lastErrno_ = 0;
    if (launchPath_.isEmpty() || !running_ || argv_.isEmpty()) return Outcome::Disabled;
    const std::optional<SelfFileId> installed = ops_.stat(launchPath_);
    if (!installed) return Outcome::Missing;
    if (*installed == *running_) return Outcome::Unchanged;
    if (!ops_.executable(launchPath_)) return Outcome::NotExecutable;
    const int err = ops_.exec(launchPath_, argv_);
    if (err == 0) return Outcome::Replaced;
    // The baseline stays the image actually running: adopting the new file's identity here would
    // keep a failed exec from ever being retried once the file is repaired.
    lastErrno_ = err;
    return Outcome::ExecFailed;
}

}  // namespace remora
