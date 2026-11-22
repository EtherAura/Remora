#pragma once

#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace remora {

// A long-running watcher that notices its own program file was replaced and starts the new one in
// its place (`remora update-watch`). An upgrade renames the old binary away (or unlinks it) and
// installs a new file at the path the unit runs; the running process keeps executing the old inode
// until something restarts it, and nothing did.
//
// Two different things are needed, and they come from two different places:
//   - WHAT IS RUNNING: the identity of the executing image, from a stat of the executing-image
//     link (/proc/self/exe). That link follows the inode, so it stays right across a rename or an
//     unlink.
//   - WHERE IT WAS INSTALLED: the pathname the process was launched as, captured at startup from
//     argv[0] (the unit's absolute ExecStart path). Never the link's target: after a rename that
//     names the backup, whose identity is the running one, so the replacement is never seen.
// The baseline is never taken from a stat of the installation path: a replacement already
// installed before the watcher started would then look current.

struct SelfFileId {
    quint64 dev = 0, ino = 0;
    qint64 size = 0, mtimeNs = 0;
    bool operator==(const SelfFileId &o) const {
        return dev == o.dev && ino == o.ino && size == o.size && mtimeNs == o.mtimeNs;
    }
    bool operator!=(const SelfFileId &o) const { return !(*this == o); }
};

struct SelfImageOps {
    // stat(2), following symlinks; nullopt when the path cannot be statted.
    std::function<std::optional<SelfFileId>(const QString &)> stat;
    // access(X_OK) on a regular file.
    std::function<bool(const QString &)> executable;
    // Replaces the process image. Returns only on failure, with the errno; the production one
    // never returns 0. A test double returns 0 to mean "the exec happened".
    std::function<int(const QString &path, const QStringList &argv)> exec;
};

// The real operations (POSIX); on a platform without them stat always fails, so nothing is seeded.
SelfImageOps realSelfImageOps();

// The installation path from argv[0] and the working directory, both as they were at process
// start: absolute as given (a final symlink is kept, not resolved — the unit runs that name);
// relative-with-a-slash joined to the start directory; a bare name (found on PATH) gives an empty
// string, because which file PATH named then cannot be known reliably now.
QString launchPathFromArgv0(const QString &argv0, const QString &startDir);

class SelfImageWatch {
public:
    enum class Outcome {
        Disabled,       // no launch path, or the running image could not be identified
        Unchanged,      // the launch path holds the running image
        Missing,        // nothing (statable) at the launch path yet: mid-install, wait
        NotExecutable,  // a new file that cannot be run yet: wait, it may be finished or repaired
        ExecFailed,     // exec refused it; the running image stays the baseline, retried next check
        Replaced,       // exec succeeded (only observable with a test double)
    };

    // `launchPath` is the startup capture (launchPathFromArgv0); `argv` the original arguments,
    // re-used as they are. `runningLink` is the executing-image link, injectable for tests.
    static SelfImageWatch seeded(const QString &launchPath, const QStringList &argv,
                                 SelfImageOps ops,
                                 const QString &runningLink = QStringLiteral("/proc/self/exe"));

    // One check. Call only between passes of the watch loop, never during an Apply.
    Outcome check();

    int lastErrno() const { return lastErrno_; }
    const QString &launchPath() const { return launchPath_; }

private:
    QString launchPath_;
    QStringList argv_;
    SelfImageOps ops_;
    std::optional<SelfFileId> running_;
    int lastErrno_ = 0;
};

}  // namespace remora
