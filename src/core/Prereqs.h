#pragma once
#include <QString>
#include <QVector>

#include "core/Backend.h"
#include "core/Caps.h"

namespace remora {

struct PrereqResult {
    QString key, label;
    bool ok = false;
    QString remedy;
    QString severity;  // required | gpu | optional
};

// Per-target prereqs evaluated against a capability probe. gpuHost adds the host-GPU rows;
// macvlan adds the host-LAN row the macvlan network is built from (bd remora-400); sharedInputs
// adds the python3 row remora-input-share.py runs on (bd remora-4ei.36) — each asked of the
// RESOLVED config rather than the backend, because the mode is a per-profile choice everywhere.
QVector<PrereqResult> checkReadiness(Backend target, const HostCapabilities &caps,
                                     bool gpuHost = false, bool macvlan = false,
                                     bool sharedInputs = false);

bool isReady(const QVector<PrereqResult> &results);   // every REQUIRED prereq satisfied
QVector<PrereqResult> blockers(const QVector<PrereqResult> &results);

// --- CPU arch-variant vs the host that has to execute it (bd remora-bm7.11) --------------------
//
// A TARGET_ARCH_VARIANT build emits instructions the baseline does not, and an image carrying them
// does not "run slower" on a CPU that lacks them — it SIGILLs, with no message naming why. That is
// a live hazard rather than a theoretical one: this project's two AMD hosts are Zen 1 (EPYC 7551P,
// Ryzen 7 2700X) and measurably lack every Alder Lake addition, while an image built here is
// tuned for one. Delivering an image between machines is a `docker save | ssh docker load`, which
// has no step where anything would notice.
//
// THE VARIANT MUST COME FROM THE BUILD, NOT FROM THE IMAGE TAG. The tag is not trustworthy for
// this: before the fix the arch-variant sed rewrote a BoardConfig the A17 product does
// not build, so every image tagged -alderlake in that window was compiled at BASELINE. A
// tag-derived check would refuse those on a Zen host and be WRONG — the "three false refusals,
// zero catches" outcome bd remora-4ei.19 already retired one preflight for.
//
// Returns the /proc/cpuinfo flag names a variant's codegen can actually emit. UNKNOWN VARIANTS
// RETURN EMPTY, deliberately: a name this table has not been taught is not a reason to refuse a
// deploy, exactly as an unmapped DRM driver emits no Vulkan ICD rather than a plausible wrong one.
QStringList archVariantRequiredFlags(const QString &variant);

// The subset of archVariantRequiredFlags(variant) that `flagsLine` — one raw "flags : ..." line
// from /proc/cpuinfo — does NOT contain. Empty means the host can execute the build. An empty or
// unreadable flagsLine also returns empty: failing to READ the CPU is not evidence the CPU is
// wrong, and refusing on a failed probe is the same false-refusal trap as above.
QStringList missingArchVariantFlags(const QString &variant, const QString &flagsLine);

// The deploy refusal for that mismatch, or empty when there is nothing to refuse — the same
// empty-means-proceed contract as dataShapeRefusal(), and for the same reason: one definition, so
// what the CLI prints and what the chain refuses with cannot drift.
//
// EVERY FORM OF NOT-KNOWING PASSES, which is a rule and not an oversight. An image with no
// remora.arch_variant label predates the bake (or was assembled where the BoardConfig could not be
// read); a variant this table has never been taught asks for no flags; an unreadable /proc/cpuinfo
// says nothing about the CPU. missingArchVariantFlags() already answers empty to all three, and this
// adds no judgement of its own — refusing on ignorance is what earned bd remora-4ei.19's preflight
// its deletion after three false refusals and zero catches.
//
// `where` names the machine that would have executed it, for the message only ("this host", or the
// ssh target).
QString archVariantRefusal(const QString &variant, const QString &flagsLine,
                           const QString &imageTag, const QString &where);

}  // namespace remora
