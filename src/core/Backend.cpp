#include "core/Backend.h"

namespace remora {

QString backendToString(Backend b) {
    switch (b) {
        case Backend::Bare: return QStringLiteral("bare");
        case Backend::Remote: return QStringLiteral("remote");
    }
    return QStringLiteral("bare");
}

// Unrecognised — and, importantly, UNSET — means the LOCAL host.
//
// This used to fall through to Vm, in both directions, which made "no backend= key" silently mean
// "the KVM guest over ssh". A profile that had never mentioned a backend therefore deployed nowhere
// and failed at preflight with "no ssh host set", which reads as a missing setting rather than as
// the wrong target being chosen for you (bd remora-d5v). Local is the right default on every count:
// it is the preferred target, it needs no configuration to be reachable, and it is what someone who
// did not specify a backend meant.
//
// "vm" now lands here too, and that is deliberately NOT a silent migration to remote: see
// isRetiredVmBackend(), which the loader uses to flag the profile so preflight can refuse it with
// instructions instead of quietly deploying it somewhere else.
Backend backendFromString(const QString &s) {
    if (s == QLatin1String("remote")) return Backend::Remote;
    return Backend::Bare;
}

bool isRetiredVmBackend(const QString &s) { return s.trimmed() == QLatin1String("vm"); }

QString retiredVmBackendMessage() {
    return QStringLiteral(
        "backend=vm has been removed — Remora does not start VMs any more. Start the guest "
        "yourself (virsh start <domain>), then set backend=remote with ssh_host=<user>@<host> and "
        "drop the vm_name/libvirt_uri keys. Check gpu_mode and network_mode after the switch: vm "
        "defaulted to gpu_mode=host and network_mode=macvlan (adb straight to the container on "
        "5555); remote defaults to guest and bridge, with adb published on host port 5555.");
}

QString gpuModeToString(GpuMode m) {
    switch (m) {
        case GpuMode::Host: return QStringLiteral("host");
        case GpuMode::Guest: return QStringLiteral("guest");
    }
    return QStringLiteral("guest");
}

std::optional<GpuMode> gpuModeFromString(const QString &s) {
    const QString v = s.trimmed();
    if (v == QLatin1String("host")) return GpuMode::Host;
    if (v == QLatin1String("guest")) return GpuMode::Guest;
    return std::nullopt;
}

QString unrecognisedGpuModeMessage(const QString &value) {
    return QStringLiteral(
               "gpu_mode=%1 is not a GPU mode. Use gpu_mode=host (render on the docker host's GPU) "
               "or gpu_mode=guest (SwiftShader, software rendering), or remove the key for the "
               "default (guest).")
        .arg(value);
}

}  // namespace remora
