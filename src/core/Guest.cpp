#include "core/Guest.h"

#include "core/ManagedHost.h"

namespace remora {

// QEMU splits option values on ',' and reads ",," as a literal comma, so a path containing one
// would otherwise end the option early.
static QString qemuPath(const QString &p) {
    QString s = p;
    s.replace(QLatin1Char(','), QLatin1String(",,"));
    return s;
}

QStringList buildGuestQemuArgv(const GuestLaunch &g) {
    QStringList a{g.qemu, QStringLiteral("-name"), QStringLiteral("remora-guest")};
    // WHPX with QEMU's own interrupt controller: its in-kernel irqchip is what Linux guests hang
    // on. TCG with one thread per vCPU, since it is only ever a slow fallback.
    if (g.accel == QLatin1String("whpx"))
        a << QStringLiteral("-accel") << QStringLiteral("whpx,kernel-irqchip=off");
    else if (g.accel == QLatin1String("tcg"))
        a << QStringLiteral("-accel") << QStringLiteral("tcg,thread=multi");
    else
        a << QStringLiteral("-accel") << g.accel;
    // `host` only exists where the accelerator can pass the CPU through (KVM); `max` is the
    // fullest model the others offer. The Android image needs x86-64-v2 (SSE4.2, POPCNT) at least.
    a << QStringLiteral("-machine") << QStringLiteral("q35")
      << QStringLiteral("-cpu") << (g.accel == QLatin1String("kvm") ? QStringLiteral("host")
                                                                      : QStringLiteral("max"))
      << QStringLiteral("-smp") << QString::number(g.cpus)
      << QStringLiteral("-m") << QString::number(g.memoryMb)
      // Nothing QEMU would add on its own: no VGA (its BIOS is not shipped), no default serial
      // or monitor, no floppy. Everything the guest has is listed below.
      << QStringLiteral("-nodefaults") << QStringLiteral("-no-user-config")
      << QStringLiteral("-display") << QStringLiteral("none");
    if (!g.firmwareDir.isEmpty()) a << QStringLiteral("-L") << g.firmwareDir;
    a << QStringLiteral("-kernel") << g.kernel;
    // Remora's guest kernel boots without one; an image that ships an initramfs gets it passed.
    if (!g.initrd.isEmpty()) a << QStringLiteral("-initrd") << g.initrd;
    a << QStringLiteral("-append") << g.cmdline;
    a << QStringLiteral("-drive")
      << QStringLiteral("file=%1,if=virtio,format=qcow2").arg(qemuPath(g.systemDisk))
      << QStringLiteral("-drive")
      << QStringLiteral("file=%1,if=virtio,format=qcow2,discard=unmap,detect-zeroes=unmap")
             .arg(qemuPath(g.dataDisk));
    // Loopback only: these ports are a root-equivalent door into the guest (docker) and into
    // every Android in it (adb), and nothing beyond this machine has any business there.
    QString net = QStringLiteral("user,id=n0,hostfwd=tcp:127.0.0.1:%1-:22").arg(g.sshPort);
    for (int i = 0; i < g.adbCount; ++i)
        net += QStringLiteral(",hostfwd=tcp:127.0.0.1:%1-:%2")
                   .arg(g.adbLocalFirst + i)
                   .arg(kManagedAdbFirst + i);
    a << QStringLiteral("-netdev") << net
      // romfile=: no iPXE option ROM to load (or ship) for a guest that never netboots.
      << QStringLiteral("-device") << QStringLiteral("virtio-net-pci,netdev=n0,romfile=")
      << QStringLiteral("-device") << QStringLiteral("virtio-rng-pci")
      << QStringLiteral("-fw_cfg")
      << QStringLiteral("name=opt/remora/authorized_key,file=%1").arg(qemuPath(g.authorizedKey))
      << QStringLiteral("-fw_cfg")
      << QStringLiteral("name=opt/remora/host_key,file=%1").arg(qemuPath(g.hostKey))
      << QStringLiteral("-serial") << QStringLiteral("file:") + g.consoleLog
      << QStringLiteral("-monitor") << QStringLiteral("none")
      << QStringLiteral("-qmp")
      << QStringLiteral("tcp:127.0.0.1:%1,server=on,wait=off").arg(g.qmpPort);
    return a;
}

}  // namespace remora
