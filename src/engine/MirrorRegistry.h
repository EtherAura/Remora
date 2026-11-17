#pragma once
#include <QString>
#include <QVector>
#include <memory>

#include "core/MirrorRegistry.h"

// The mirror registry's filesystem half (bd remora-7oih.1). Every `remora mirror` process
// registers itself for its whole life; the engine finds, stops and counts mirrors through what is
// registered instead of through pgrep patterns over a Linux command line.
//
// One registration is two files named by a per-process id (<pid>-<random>, so a reused pid can
// never collide with a dead owner's leftovers):
//   <id>.lock  held under an OS lock for the process's life — flock(2) on POSIX, a no-write-share
//              handle on Windows. The kernel drops it when the process dies however it dies,
//              SIGKILL and TerminateProcess included, so liveness is the lock and never the pid.
//              QLockFile was the obvious tool and is not quite this: it judges a dead owner by
//              its PID first, so a crashed mirror whose pid now belongs to another Remora process
//              reads as alive — and a lookup would then hand that other process to a stop call.
//   <id>.json  the entry (core/MirrorRegistry.h), replaced atomically when the session changes.
// A reader that finds an entry whose lock nobody holds removes both: that is the crash cleanup.

namespace remora {

// Where every mirror registers. The GUI, the CLI and the mirrors must all agree on it, so it
// depends on nothing a process sets for itself (not the application name):
//  Linux    $XDG_RUNTIME_DIR/remora-mirrors, beside the tunnel locks. A tmpfs the session's end
//           and a reboot both empty, so no entry outlives the boot it was made in.
//  Windows  %LOCALAPPDATA%\remora\mirrors. RuntimeLocation there is the profile root itself.
QString mirrorRegistryDir();

// One mirror's registration, held for the life of this object. Best-effort like the tunnel lock:
// a registration that cannot be made must not cost the user their mirror, so the mirror runs and
// says so — the price is that nothing can find it to reap it (on Linux, the command-line
// fallback still can).
class MirrorRegistration {
public:
    explicit MirrorRegistration(const MirrorEntry &self, const QString &dir = mirrorRegistryDir());
    ~MirrorRegistration();
    MirrorRegistration(const MirrorRegistration &) = delete;
    MirrorRegistration &operator=(const MirrorRegistration &) = delete;

    bool ok() const;
    QString error() const;
    // The self-report the husk test reads where there is no `ss`. Rewrites the entry only when
    // the state actually changes.
    void setSession(MirrorSessionState s);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The live registered mirrors, oldest first. Entries whose owner is gone are removed on the way,
// and an entry whose liveness cannot be established is skipped rather than trusted: what this
// returns ends up in stop calls, and a pid nobody vouches for may by now be anyone's.
QVector<MirrorEntry> readMirrorRegistry(const QString &dir = mirrorRegistryDir());

// Linux only (empty elsewhere): every `remora mirror` process, read from <procRoot>/<pid>/cmdline
// — the fallback for mirrors started by a Remora that predates the registry, which an upgrade
// leaves running and nothing else would ever find (marked registered=false). procRoot is a
// parameter for the offline suite alone.
QVector<MirrorEntry> scanMirrorProcesses(const QString &procRoot = QStringLiteral("/proc"));

}  // namespace remora
