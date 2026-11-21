#pragma once
#include <QString>
#include <QStringList>

namespace remora {

// How Remora starts its own Linux guest under QEMU (bd remora-7oih.5). Pure, so the argv is
// golden-tested like the docker argv; engine/GuestVm fills it in and runs it. The guest itself
// is built by guest/build.sh, which is where cmdline and the fw_cfg contract come from.
struct GuestLaunch {
    QString qemu;           // qemu-system-x86_64
    QString firmwareDir;    // -L: bios-256k.bin, linuxboot_dma.bin (empty: QEMU's own default)
    QString accel;          // whpx | kvm | tcg
    QString videoDisplay;   // QEMU's GL display backend for the video prototype
    int memoryMb = 0;
    int cpus = 0;
    QString kernel, initrd, cmdline;  // initrd may be empty: Remora's kernel needs none
    QString systemDisk;     // the qcow2 overlay over the shipped system.qcow2
    QString dataDisk;       // the guest's /home and docker data-root
    int sshPort = 0;        // loopback ports, forwarded into the guest
    int adbLocalFirst = 0;  //   → guest kManagedAdbFirst…
    int adbCount = 0;
    int qmpPort = 0;        // loopback control channel: powerdown, quit
    QString authorizedKey;  // fw_cfg opt/remora/authorized_key
    QString hostKey;        // fw_cfg opt/remora/host_key
    QString consoleLog;     // the guest's serial console
};

QStringList buildGuestQemuArgv(const GuestLaunch &g);

}  // namespace remora
