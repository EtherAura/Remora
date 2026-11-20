#pragma once
#include <QString>
#include <QStringList>
#include <optional>

#include "core/Backend.h"

namespace remora {

// A docker host Remora runs itself (bd remora-7oih.5): on Windows, the Linux guest Android lives
// in. To the rest of Remora it is an ordinary remote docker host, named by a reserved ssh_host,
// and this is where that name is turned into how the host is actually reached: loopback ports
// QEMU forwards, a key Remora generated, and a host key pinned from the first connection.
//
// Pure data: the engine's GuestVm fills it in (from its state file at startup, or after it picks
// ports), and ssh/scp/adb argv builders and the Resolver read it.
struct ManagedHost {
    QString alias;           // the ssh_host profiles use for it
    QString user;
    QString address = QStringLiteral("127.0.0.1");
    int sshPort = 0;
    QString identityFile;    // private key
    QString knownHostsFile;  // holds exactly the guest's host key
    int adbLocalFirst = 0;   // local port QEMU forwards to kManagedAdbFirst in the guest
    int adbCount = 0;        // …and the next adbCount-1 ports, one per profile
    QString accelerator;     // the accelerator recorded by the running guest
};

// The guest ports profiles publish adb on: the lowest free from 5555 upward, as anywhere else.
inline constexpr int kManagedAdbFirst = 5555;

// The reserved ssh_host of Remora's own guest.
QString guestAlias();

void registerManagedHost(const ManagedHost &h);
void forgetManagedHost(const QString &alias);
// By alias, or user@alias: profiles may name the account too.
std::optional<ManagedHost> managedHost(const QString &sshHost);

// ssh and scp options that reach it, followed by nothing: the caller appends the destination
// (managedSshDestination) for ssh, or rewrites `alias:path` operands for scp. They differ only in
// the port flag, -p against -P.
QStringList managedSshOptions(const ManagedHost &h, bool forScp);
QString managedSshDestination(const ManagedHost &h);
// The local port that reaches a guest adb port, or nullopt outside the forwarded range.
std::optional<int> managedAdbPort(const ManagedHost &h, int guestPort);

// The docker host a profile with this ssh_host reaches: the host itself, or — where this machine's
// docker is Remora's guest and the profile names none — the guest's reserved name, exactly as the
// Resolver resolves it. For the code that reads ssh_host straight off a config.
QString dockerHostOf(const QString &sshHost);

// Whether "this machine's docker" is Remora's own guest rather than a local daemon: true on
// Windows, which has no Linux kernel to run docker on. Settable so the suites can exercise it.
bool localDockerIsGuest();
void setLocalDockerIsGuest(bool on);
// Bare where the local docker is the guest is the guest over ssh: remote, as far as every
// deploy path is concerned. Everything that picks a backend goes through this.
Backend effectiveBackend(Backend b);

}  // namespace remora
