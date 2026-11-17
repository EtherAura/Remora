#include "core/ManagedHost.h"

#include <QHash>
#include <QReadWriteLock>

namespace remora {

namespace {
QReadWriteLock gLock;
QHash<QString, ManagedHost> gHosts;
#ifdef Q_OS_WIN
bool gLocalIsGuest = true;
#else
bool gLocalIsGuest = false;
#endif
}  // namespace

QString guestAlias() { return QStringLiteral("remora-guest"); }

void registerManagedHost(const ManagedHost &h) {
    QWriteLocker l(&gLock);
    gHosts.insert(h.alias, h);
}

void forgetManagedHost(const QString &alias) {
    QWriteLocker l(&gLock);
    gHosts.remove(alias);
}

std::optional<ManagedHost> managedHost(const QString &sshHost) {
    const QString name = sshHost.trimmed().section(QLatin1Char('@'), -1);
    QReadLocker l(&gLock);
    const auto it = gHosts.constFind(name);
    if (it == gHosts.constEnd()) return std::nullopt;
    return *it;
}

QStringList managedSshOptions(const ManagedHost &h, bool forScp) {
    // BatchMode: nothing on the other end may ever prompt — there is no terminal behind a GUI
    // launch, and a prompt would hang the step. StrictHostKeyChecking=yes against a known_hosts
    // holding only the key Remora handed the guest: anything else answering on that port is
    // refused, never trusted on first use.
    return {forScp ? QStringLiteral("-P") : QStringLiteral("-p"), QString::number(h.sshPort),
            QStringLiteral("-i"), h.identityFile,
            QStringLiteral("-o"), QStringLiteral("IdentitiesOnly=yes"),
            QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
            QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=yes"),
            QStringLiteral("-o"), QStringLiteral("UserKnownHostsFile=") + h.knownHostsFile};
}

QString managedSshDestination(const ManagedHost &h) {
    return h.user.isEmpty() ? h.address : h.user + QLatin1Char('@') + h.address;
}

std::optional<int> managedAdbPort(const ManagedHost &h, int guestPort) {
    const int i = guestPort - kManagedAdbFirst;
    if (h.adbLocalFirst <= 0 || i < 0 || i >= h.adbCount) return std::nullopt;
    return h.adbLocalFirst + i;
}

QString dockerHostOf(const QString &sshHost) {
    const QString h = sshHost.trimmed();
    return (h.isEmpty() && localDockerIsGuest()) ? guestAlias() : h;
}

bool localDockerIsGuest() {
    QReadLocker l(&gLock);
    return gLocalIsGuest;
}

void setLocalDockerIsGuest(bool on) {
    QWriteLocker l(&gLock);
    gLocalIsGuest = on;
}

Backend effectiveBackend(Backend b) {
    return (b == Backend::Bare && localDockerIsGuest()) ? Backend::Remote : b;
}

}  // namespace remora
