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
    const bool video = !g.videoDisplay.isEmpty();
    const bool winqWhpx = video && g.accel == QLatin1String("whpx");
    QStringList a{g.qemu, QStringLiteral("-name"),
                  video ? QStringLiteral("remora-guest-video") : QStringLiteral("remora-guest")};
    // WINQ uses WHPX's default interrupt controller. The stock Windows QEMU guest uses a
    // userspace irqchip. TCG uses one thread per vCPU.
    if (g.accel == QLatin1String("whpx") && !winqWhpx)
        a << QStringLiteral("-accel") << QStringLiteral("whpx,kernel-irqchip=off");
    else if (g.accel == QLatin1String("tcg"))
        a << QStringLiteral("-accel") << QStringLiteral("tcg,thread=multi");
    else
        a << QStringLiteral("-accel") << g.accel;
    // KVM and WINQ provide CPU passthrough. Android needs at least x86-64-v2 (SSE4.2, POPCNT).
    a << QStringLiteral("-machine") << QStringLiteral("q35")
      << QStringLiteral("-cpu") << (winqWhpx || g.accel == QLatin1String("kvm")
                                         ? QStringLiteral("host") : QStringLiteral("max"))
      << QStringLiteral("-smp") << QString::number(g.cpus)
      << QStringLiteral("-m") << QString::number(g.memoryMb)
      // Nothing QEMU would add on its own: no VGA (its BIOS is not shipped), no default serial
      // or monitor, no floppy. Everything the guest has is listed below.
      << QStringLiteral("-nodefaults") << QStringLiteral("-no-user-config")
      << QStringLiteral("-display") << (video ? g.videoDisplay : QStringLiteral("none"));
    if (video) {
        a << QStringLiteral("-device")
          << QStringLiteral("virtio-gpu-gl-pci,blob=on,hostmem=4G,venus=on,romfile=");
    }
    if (!g.firmwareDir.isEmpty()) a << QStringLiteral("-L") << g.firmwareDir;
    a << QStringLiteral("-kernel") << g.kernel;
    // Remora's guest kernel boots without one; an image that ships an initramfs gets it passed.
    if (!g.initrd.isEmpty()) a << QStringLiteral("-initrd") << g.initrd;
    QString cmdline = g.cmdline;
    // Under nested WHPX on AMD, WINQ advertises XSAVEC with a zero-sized compacted state area.
    // Mask XSAVEC in Linux; WINQ's WHPX CPUID passthrough leaves -cpu feature masks ineffective.
    if (winqWhpx) cmdline += QStringLiteral(" clearcpuid=xsavec");
    a << QStringLiteral("-append") << cmdline;
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
