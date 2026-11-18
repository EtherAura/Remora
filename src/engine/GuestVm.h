#pragma once
#include <QString>

#include "engine/Engine.h"

namespace remora {

// Remora's own Linux guest (bd remora-7oih.5). Windows has no Linux kernel, and Android needs one,
// so Remora for Windows runs a small Alpine guest (guest/build.sh) under QEMU on the Windows
// Hypervisor Platform, out of sight, and deploys into it exactly as it deploys to any docker host
// over ssh: the guest is registered as a ManagedHost under guestAlias(), and everything else
// reaches it through that. This is not the removed vm backend — there is no guest of the user's
// to manage, only Remora's own, and no deploy path knows it is a VM.
//
// Files: the shipped image (vmlinuz, system.qcow2, cmdline, manifest.txt) beside the
// executable in guest/, QEMU in qemu/; per-user state under the generic data dir in
// remora/guest: the two disks, the keys, known_hosts, the console log and state.json (ports, the
// running QEMU's pid, the image it was provisioned from).
//
// REMORA_GUEST_IMAGE_DIR and REMORA_GUEST_QEMU point a development build elsewhere: the image
// guest/build.sh produced, and a qemu-system-x86_64 (or a wrapper around one).
class GuestVm {
public:
    explicit GuestVm(Spawner &sp) : sp_(sp) {}

    static QString stateDir();
    static QString imageDir();
    static QString qemuProgram();

    // Register the guest's endpoint from state.json, or from the default ports when there is none
    // yet — at startup, before anything resolves a profile, so the guest's name already resolves
    // to its forwarded adb ports. Where this machine's docker is the guest it always registers;
    // elsewhere only once a guest has been provisioned.
    static void registerFromState();

    struct Status {
        bool shipped = false;      // an image is installed to start from
        bool provisioned = false;  // disks and keys exist
        bool running = false;      // QEMU answers on its control port
        qint64 pid = 0;
        int sshPort = 0;
        QString accel;
        QString image;             // "Alpine …, kernel …" from the manifest
    };
    Status status() const;

    // Provision if needed, start if stopped, and wait until ssh and docker answer. Idempotent:
    // a running guest costs one control-port probe and one ssh round trip.
    StepResult ensureRunning(const ResolvedConfig &rc, const LineSink &log);
    // Graceful power-down over QMP (the guest's acpid shuts down docker and its disks), QEMU's
    // own quit after graceMs, and nothing worse.
    StepResult stop(const LineSink &log, int graceMs = 60000);
    // `remora guest watch`, Windows only: stay beside the running guest until its QEMU exits, and
    // power the guest down when Windows ends the session. ensureRunning starts it; a second one
    // for the same guest leaves at once.
    int watchSessionEnd(const LineSink &log);
    // Throw away the system overlay so the next start boots the shipped image fresh. The data
    // disk — every image, container and Android /data — is untouched.
    StepResult resetSystem(const LineSink &log);

private:
    Spawner &sp_;
};

}  // namespace remora
