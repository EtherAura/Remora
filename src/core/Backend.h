#pragma once
#include <QString>
#include <optional>

namespace remora {

// Where the container runs: this machine's docker, or someone else's over ssh. That is the whole
// distinction — a KVM guest, a NAS or a rented box are all BARE-with-an-ssh-hop, which is what
// REMOTE is. A third "vm" backend used to drive libvirt as well (bd remora-d5v); Remora does not
// start VMs any more, because a guest is just another docker host once it is running.
enum class Backend { Bare, Remote };
enum class GpuMode { Host, Guest };

QString backendToString(Backend b);
Backend backendFromString(const QString &s);  // defaults to Bare on unknown/unset

// True for the one retired spelling. A profile that still says backend=vm must not be reinterpreted
// as remote behind the user's back — vm resolved gpu_mode=host, no overlayfs, macvlan and container
// adb 5555, and remote defaults to the opposite of all four, so "close enough" would silently change
// the deployment. Preflight refuses and says what to edit (bd remora-d5v).
bool isRetiredVmBackend(const QString &s);
// The refusal itself — what to edit instead — worded once for preflight and the CLI's --backend.
QString retiredVmBackendMessage();

QString gpuModeToString(GpuMode m);
// host | guest, and nothing else: an unrecognised spelling is nullopt, NOT a default. "auto" used
// to parse into a third mode that nothing handled, so gpu_mode=auto ran as guest (SwiftShader)
// without a word. The loader keeps the raw value so preflight can refuse it by name.
std::optional<GpuMode> gpuModeFromString(const QString &s);
// The refusal — the valid values and what each means — worded once for preflight.
QString unrecognisedGpuModeMessage(const QString &value);

}  // namespace remora
