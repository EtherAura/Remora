#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <unistd.h>

#include <memory>
#include <vector>

#include "core/Builders.h"
#include "core/Config.h"
#include "core/Features.h"
#include "core/Gating.h"
#include "core/ImageBuild.h"
#include "core/Parsers.h"
#include "core/Prereqs.h"
#include "core/Resolver.h"
#include "core/SourcePatches.h"
#include "engine/AppTransfer.h"
#include "engine/Engine.h"
#include <csignal>
#include <unistd.h>
#include <QProcess>
#include "engine/RedditLogin.h"
#include "engine/SourceBuild.h"
#include "store/Store.h"

using namespace remora;

namespace remora {
int runGui(int argc, char **argv);  // ui/App.cpp — constructs its own QApplication
int runMirror(int argc, char **argv);  // mirror/MirrorApp.cpp — its own QApplication
int runRedditLoginGui(int argc, char **argv);  // ui/RedditLoginWindow.cpp — its own QApplication
}

static Backend backendArg(const QStringList &a, Backend def = Backend::Bare) {
    const int i = a.indexOf(QStringLiteral("--backend"));
    if (!(i > 0 && i + 1 < a.size())) return def;
    // The same refusal a backend=vm PROFILE gets at preflight. backendFromString maps every
    // unknown spelling to Bare, which is right for an unset key and wrong for this one: `--backend
    // vm` was accepted without a word and planned a local deploy while the usage line still
    // advertised the flag.
    if (isRetiredVmBackend(a[i + 1])) {
        QTextStream(stderr) << "remora: --backend vm: " << retiredVmBackendMessage() << '\n';
        ::exit(2);
    }
    return backendFromString(a[i + 1]);
}
static QString strArg(const QStringList &a, const QString &flag, const QString &def = {}) {
    const int i = a.indexOf(flag);
    return (i > 0 && i + 1 < a.size()) ? a[i + 1] : def;
}
static RemoraConfig loadActive(QString &instOut) {
    const QString path = defaultRemorarcPath();
    instOut = activeInstance(path);
    return loadInstance(path, instOut);
}

// A named instance must EXIST. loadInstance() layers [Defaults] < [Instance-<name>], so an unknown
// name silently yields the defaults alone — every command then reported confidently on a DIFFERENT
// profile than the one asked for, and for `up` that means deploying against the wrong instance
// (bd remora-a4i). Name the mistake and list what is available, the way the remote preflight
// names a missing ssh_host.
static QString requireInstance(const QString &name) {
    const QString path = defaultRemorarcPath();
    const QStringList known = listInstances(path);
    if (known.contains(name)) return name;
    QTextStream(stderr) << "remora: no such instance: '" << name << "'\n"
                        << "known instances: "
                        << (known.isEmpty() ? QStringLiteral("(none configured)")
                                            : known.join(QStringLiteral(", ")))
                        << "\n";
    ::exit(2);
}

static RemoraConfig configForArg(const QStringList &a, QString &instOut);  // defined below

// Load the profile the args name (or the active one) AND pick its backend, in that order: an
// explicit --backend always wins, else inference (inferBackend: the profile's own backend= key
// if present, else blank host = bare / host = remote). The order is the whole point — every
// command used to call backendArg(a) before reading the config, so a hard default won
// unconditionally and a profile saying backend=bare was silently ignored. Reaching a bare
// profile then meant repeating --backend bare on every single invocation (bd remora-4ei.60).
static RemoraConfig configAndBackend(const QStringList &a, QString &instOut, Backend &backendOut) {
    RemoraConfig cfg = configForArg(a, instOut);
    backendOut = backendArg(a, inferBackend(cfg));
    return cfg;
}

// The /data delivery and what is actually on disk — the visibility half of bd remora-4ei.89. The
// silent factory-reset there was a SHAPE change nothing printed: the deploy now refuses the
// mismatch, and this is what lets `plan` and `check` say which delivery the profile resolves to,
// what the dirs hold, and — under overlayfs — whether the lower layer is EMPTY, which is that
// whole failure mode and was invisible. Multi-line when the deploy would refuse; the refusal text
// is the deployer's own, so the two can never describe the situation differently.
static QString dataShapeReport(Spawner &sp, Backend b, const ResolvedConfig &rc) {
    const QString host = rc.sshHost.value_or(QString());
    const auto word = [](DataDirShape s) -> QString {
        switch (s) {
            case DataDirShape::Missing: return QStringLiteral("missing — created on deploy");
            case DataDirShape::Empty: return QStringLiteral("empty");
            case DataDirShape::Layered: return QStringLiteral("layered, upper/ present");
            case DataDirShape::Flat: return QStringLiteral("a flat /data tree");
        }
        return QString();
    };
    const DataDirShape shape = probeDataDirShape(sp, b, rc.dataDir, host);
    QString line;
    if (rc.useOverlayfs) {
        // The lower layer's shape is probed with the same scan: Flat means it holds a real /data
        // tree to inherit; missing or empty means this instance layers over NOTHING, so a fresh
        // deploy starts blank — worth a sentence, not an inference from a path.
        const DataDirShape lower = probeDataDirShape(sp, b, rc.dataBaseDir, host);
        const bool lowerEmpty = lower == DataDirShape::Missing || lower == DataDirShape::Empty;
        line = QStringLiteral("/data: overlayfs — diff %1 (%2); lower %3 (%4)")
                   .arg(rc.dataDir, word(shape), rc.dataBaseDir,
                        lowerEmpty ? QStringLiteral("EMPTY — layers over nothing, a fresh "
                                                    "deploy starts blank")
                                   : word(lower));
    } else {
        line = QStringLiteral("/data: plain bind — %1 (%2)").arg(rc.dataDir, word(shape));
    }
    const QString refusal = dataShapeRefusal(rc.useOverlayfs, shape, rc.dataDir, rc.dataBaseDir);
    if (!refusal.isEmpty()) line += QStringLiteral("\nDEPLOY WILL REFUSE: ") + refusal;
    return line;
}

// The image's baked CPU tuning against the CPU that would run it (bd remora-bm7.13), reported the
// same way and for the same reason as the shape above: a guard that only ever speaks by refusing a
// deploy cannot be checked on a host you are not about to deploy to — and this one's whole subject
// is a machine OTHER than the one you are sitting at. The refusal text is the deployer's own, so
// `plan`, `check` and the chain cannot describe the situation differently.
static QString archVariantReport(Spawner &sp, Backend b, const ResolvedConfig &rc) {
    const QString host = rc.sshHost.value_or(QString());
    const QString where = b == Backend::Bare ? QStringLiteral("this host") : host;
    const ArchVariantProbe av = probeArchVariant(sp, b, rc.imageTag, host);
    if (av.variant.isEmpty())
        return QStringLiteral(
                   "CPU tuning: %1 carries no arch-variant stamp — built before assemble started "
                   "writing one, so nothing can be checked against %2")
            .arg(rc.imageTag, where);
    QString line = QStringLiteral("CPU tuning: %1 was built for %2").arg(rc.imageTag, av.variant);
    if (av.flagsLine.isEmpty()) {
        // Said out loud rather than reported as a pass: "could not read the CPU" and "the CPU is
        // fine" are the same silence otherwise, and only one of them means the guard is working.
        return line + QStringLiteral("; %1's /proc/cpuinfo could not be read, so the deploy will "
                                     "not judge it either way")
                          .arg(where);
    }
    const QString refusal = archVariantRefusal(av.variant, av.flagsLine, rc.imageTag, where);
    if (refusal.isEmpty())
        return line + QStringLiteral(" — %1 can execute it").arg(where);
    return line + QStringLiteral("\nDEPLOY WILL REFUSE: ") + refusal;
}

static int cmdPlan(const QStringList &a, QTextStream &out) {
    QString inst;
    Backend backend;
    // Plan the REQUESTED profile, not an empty config. Printing resolver defaults regardless of
    // input is what made a throwaway profile look broken while verifying remora-fgj.2 — the
    // profile was fine, plan simply never read one.
    const RemoraConfig cfg = configAndBackend(a, inst, backend);
    // The SAME probe the deploy takes: host mode on bare/remote resolves its render node from the
    // live probe, so a pure plan would print host mode with no node and no --device —
    // indistinguishable from the broken half-applied state this fixed. plan is the human-facing
    // contract, so it must not resolve from a narrower probe than the thing it claims to describe.
    RealSpawner planSp;
    ResolvedConfig rc = resolve(cfg, backend, deployCapabilities(backend, cfg, planSp));
    applyVendorPaths(rc, backend);  // same fill makeContext does, so plan matches the real argv
    const auto [env, mirror] = buildMirrorArgv(rc, rc.target);
    out << "# instance=" << (inst.isEmpty() ? QStringLiteral("(none)") : inst)
        << "  backend=" << backendToString(backend) << "  target="
        << (rc.target.isEmpty() ? QStringLiteral("(macvlan IP assigned at connect)") : rc.target)
        << "\n";
    // A silent downgrade would read as "the user asked for h264". plan is the human-facing
    // contract, so the trade gets named here (bd remora-e5x.4).
    if (rc.videoCodecFallback.has_value())
        out << "# NOTE: " << *rc.videoCodecFallback << "\n";
    // The macvlan network is created BY the deploy, so the docker-run line below is only half the
    // story — show the create the chain would run (or say why it can't build one), the same way
    // the run argv is shown rather than described (bd remora-400).
    if (rc.networkMode == QLatin1String("macvlan")) {
        const QStringList net = buildMacvlanCreateArgv(rc);
        out << (net.isEmpty()
                    ? QStringLiteral("# macvlan network '%1': shape unresolved (no host LAN probed "
                                     "— set macvlan_parent/subnet/gateway); the deploy will fail "
                                     "unless it already exists\n")
                          .arg(rc.dockerNetwork.value_or(QString()))
                    : QStringLiteral("# created if missing: %1\n").arg(net.join(QLatin1Char(' '))));
        // The host route is a change to THIS machine's networking, so plan — the human-facing
        // contract — states it in full rather than letting it appear as a surprise interface.
        const QStringList shim = buildMacvlanShimArgv(rc);
        if (!shim.isEmpty())
            out << "# host route (no sudo — docker group): " << shim.join(QLatin1Char(' ')) << "\n";
        else if (rc.macvlanHostRoute && backend == Backend::Bare)
            out << "# host route: WANTED but unresolvable — this host will not be able to reach "
                   "the container unless it has another interface on the same LAN\n";
    }
    // Which /data delivery, and what is on disk (bd remora-4ei.89). Probed like the caps above:
    // plan is the human-facing contract and must describe the deploy that would actually happen,
    // including the one the shape guard would refuse.
    for (const QString &l : dataShapeReport(planSp, backend, rc).split(QLatin1Char('\n')))
        out << "# " << l << "\n";
    for (const QString &l : archVariantReport(planSp, backend, rc).split(QLatin1Char('\n')))
        out << "# " << l << "\n";
    out << buildDockerRunArgv(rc).join(QLatin1Char(' ')) << "\n\n";
    // The in-house client takes flags, not environment — the argv IS the whole contract.
    Q_UNUSED(env);
    out << mirror.join(QLatin1Char(' ')) << "\n";
    return 0;
}

static int cmdStatus(const QStringList &a, QTextStream &out) {
    RealSpawner sp;
    // Feed the probe the ACTIVE instance's ssh host / domain / container instead of the constants
    // it used to carry. Unconfigured fields stay empty and the remote rows simply read as not-ready
    // (bd remora-4ei.12).
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    const ResolvedConfig rc = resolve(cfg, b);
    const StatusReport rep = probeStatus(b, sp, rc.sshHost.value_or(QString()),
                                         rc.containerName);
    out << statusToJson(rep) << "\n";
    // The SwiftShader-fallback detector (bd remora-4ei.79): a host-mode deploy composited by a
    // software renderer is the silent failure that has cost real diagnosis time twice — say it
    // in plain text under the JSON, where a human eye actually lands.
    const QString gles = rep.bareGles.value_or(rep.remoteGles.value_or(QString()));
    if (rc.gpuMode == GpuMode::Host &&
        (gles.contains(QLatin1String("SwiftShader"), Qt::CaseInsensitive) ||
         gles.contains(QLatin1String("llvmpipe"), Qt::CaseInsensitive)))
        out << "WARNING: gpu_mode=host but the device is rendering in SOFTWARE (" << gles
            << ") — the GPU was not picked up; see `remora check`\n";
    // The dead-front-door detector (bd remora-yn4), same placement for the same reason: with fwd
    // down, adb and the mirror report a dead device — reconnect fails, the mirror is gone — while
    // Android is perfectly healthy on :5555, and nothing anywhere names which of the two it is.
    // The repair is one idempotent command, so say it. Only on disagreement: both-down is a dead
    // device and the boot rows already carry that story.
    if (rep.forwarder.adbdListening.value_or(false) &&
        !rep.forwarder.fwdListening.value_or(true))
        out << "WARNING: the adb front door (:5556) is not listening but adbd itself is healthy "
               "on :5555 — the device will look dead to adb and the mirror while Android is fine. "
               "`docker exec " << rc.containerName
            << " sh /remora/netfix.sh` restores the forwarder.\n";
    // The mirror-starvation detector (bd remora-j4z), for the same reason and in the same place:
    // frames composed on the device can stop reaching the host encoder while every other reading
    // stays plausible, and the only symptom is judder the user cannot attribute to anything.
    // Measured by hand once, it took an entire session to find; it is two numbers the chain
    // already prints.
    if (const auto flow = probeMirrorFlow(sp, rc)) {
        if (flow->starved)
            out << "WARNING: the mirror is losing frames between the device and the encoder — "
                << QString::number(flow->composedFps, 'f', 1) << " fps composed but only "
                << QString::number(flow->encodedFps, 'f', 1) << " fps encoded ("
                << QString::number(flow->ratio * 100, 'f', 0)
                << "%). This is judder, not a slow device: it is composing fine and the frames are "
                   "not reaching the stream. Seen live as the render server's publisher going on "
                   "counting submits while `resolved` stops moving.\n"
                   "         Kill this profile's remora-frame-encoder and reconnect. A plain "
                   "reconnect will NOT do it — the host-encoder step adopts a running encoder, and "
                   "the fault lives in that encoder's link to the publisher.\n";
    }
    return 0;
}

// `remora certify` — the Play Protect certification state of the active instance, plus the exact
// step that fixes it. Registration happens on Google's site under the user's own login, so this
// command cannot complete the job; what it CAN do is remove every other obstacle, which is most of
// the cost. Finding the id by hand means knowing that sqlite3 core-dumps in the container, that the
// gservices provider refuses adb, and that `settings get secure android_id` is the wrong number
// entirely — three dead ends that cost a full session before this existed (bd remora-4ei.83).
static int cmdCertify(const QStringList &a, QTextStream &out) {
    RealSpawner sp;
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    const ResolvedConfig rc = resolve(cfg, b);
    const QString host = rc.sshHost.value_or(QString());
    const bool recheck = a.contains(QStringLiteral("--recheck"));
    const bool restore = a.contains(QStringLiteral("--restore"));

    // --restore runs BEFORE the read, so the state reported afterwards is the state it produced.
    if (restore) {
        const auto snap = loadCheckinSnapshot(inst);
        const PlayCertification before =
            probePlayCertification(sp, b, rc.containerName, host, false);
        if (!snap) {
            out << "no checkin-identity snapshot for '" << inst << "'.\n"
                << "Remora saves one whenever it reads a healthy identity off the device, so there "
                   "is\nnothing to restore until `remora certify` has seen this instance at least "
                   "once.\n";
            return 1;
        }
        if (before.identityPair() == *snap) {
            out << "the device already holds the snapshotted identity — nothing to restore.\n";
        } else {
            out << "restoring the snapshotted checkin identity onto " << rc.containerName << "…\n"
                << "  device holds: "
                << (before.identityPair().isEmpty()
                        ? QStringLiteral("(none — GMS has not checked in)")
                        : before.identityPair())
                << "\n  snapshot:     " << *snap << "\n"
                << "This force-stops GMS, writes the pair back, and drops the derived caches "
                   "(gservices.db\nincluded) so they re-seed from it.\n";
            const CheckinRestore r =
                restoreCheckinIdentity(sp, b, rc.containerName, host, inst);
            out << (r.ok ? "  ok: " : "  FAILED: ") << r.detail << "\n";
            if (!r.ok) return 1;
        }
    }

    if (recheck) out << "asking GMS to check in again, then re-reading the verdict…\n";
    // The read is also the snapshot: this is the only moment Remora ever sees the identity, so it
    // is the only moment it can keep a copy (bd remora-4ei.84).
    const PlayCertification c =
        probePlayCertification(sp, b, rc.containerName, host, recheck || restore, inst);

    out << "instance:   " << inst << "  (" << backendToString(b) << " / " << rc.containerName
        << ")\n";
    if (!c.certified.has_value() && !c.androidId.has_value()) {
        out << "state:      unknown — the container is not running, or is unreachable\n";
        return 1;
    }
    out << "android_id: "
        << c.androidId.value_or(QStringLiteral("(none yet — GMS has not checked in)")) << "\n"
        << "state:      "
        << (c.pendingEvaluation
                ? pendingEvaluationNote(c.identityAgeSec.value_or(0))
                : c.certified.value_or(false) ? QStringLiteral("certified")
                                              : QStringLiteral("NOT Play Protect certified"))
        << "\n";

    // Say whether a rebuild would be recoverable. A snapshot that silently is not there is the same
    // invisible failure this whole feature exists to end, so its absence has to be as visible as
    // its presence.
    const auto snap = loadCheckinSnapshot(inst);
    out << "snapshot:   " << checkinSnapshotNote(inst, c) << "\n";
    if (snap && *snap != c.identityPair())
        out << "            `remora certify --restore` puts it back\n";

    if (c.certified.value_or(false)) return 0;

    // Pending is its own exit: the uncertified spiel below asserts a verdict this state exactly
    // does not have (bd remora-4ei.90). The note above already says what to do; add the one thing
    // it cannot carry, the registration URL.
    if (c.pendingEvaluation) {
        out << "\nIf this id still needs registering: " << playCertRegistrationUrl() << "\n"
               "then `remora certify --recheck`. If it is already registered there is nothing to "
               "do —\nthe verdict lands on the next checkin cycle or container restart.\n";
        return 1;
    }

    out << "\nAn uncertified device blocks GOOGLE SIGN-IN, so this also presents as the Play Store\n"
           "refusing to log in — one cause, not two. To fix it:\n";
    if (snap && *snap != c.identityPair())
        out << "  0. this instance has a saved identity that is NOT the one on the device — if the\n"
               "     saved one was registered, `remora certify --restore` fixes this without "
               "Google.\n";
    out << "  1. register the android_id above at "
        << playCertRegistrationUrl()
        << "\n"
           "     That page needs your Google login, so Remora cannot do this step for you.\n"
           "  2. run `remora certify --recheck`, which forces the checkin that re-evaluates it.\n"
           "Registration can take a few minutes to propagate; a container restart also picks it up.\n";
    return 1;
}

static int cmdCheck(const QStringList &a, QTextStream &out) {
    RealSpawner sp;
    // Take the ssh host from the ACTIVE instance unless --ssh-host overrides. It has no baked
    // default any more (bd remora-4ei.12): before this, check() probed a hardcoded host and
    // happened to be right only on the machine it was written for.
    QString inst;
    Backend b;
    // Honour the profile positional, like plan/status/up/reconnect. This used to be loadActive()
    // "deliberately", but the silent part was never defensible: `remora check <a-remote-profile>`
    // printed the preflight for whatever profile happened to be ACTIVE, with that profile's
    // remediation advice, and nothing in the output naming the profile it had actually read. The
    // report looks authoritative and is about a different machine
    // (bd remora-4ei.70). configAndBackend keeps --backend working on top, and with no positional
    // it still falls back to the active profile, so the old invocation is unchanged.
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    const ResolvedConfig rcc = resolve(cfg, b);
    const QString sshOverride = strArg(a, "--ssh-host");
    const HostCapabilities caps =
        probeCapabilities(b, sshOverride.isEmpty() ? rcc.sshHost.value_or(QString()) : sshOverride,
                          sp);
    // Ask the RESOLVED config, not the backend. This was once `b == Vm`, a proxy for "vm defaults to
    // host mode", which silently made the GPU checks unreachable on bare/remote however the profile
    // was configured — so the NVIDIA Venus lines could never appear there. The explicit flag still
    // forces them on for a profile that has not opted into host mode yet.
    const bool gpu = a.contains(QStringLiteral("--gpu-host")) || rcc.gpuMode == GpuMode::Host;
    const auto results =
        checkReadiness(b, caps, gpu, rcc.networkMode == QLatin1String("macvlan"),
                       !rcc.sharedInputs.isEmpty(), releaseNeedsAshmem(rcc.androidVersion));
    // Say WHICH profile this is about. A preflight that names no subject reads as a statement
    // about the machine, so when it did silently report on a different profile there was nothing
    // in the output to contradict that reading (bd remora-4ei.70).
    QTextStream(stderr) << "remora check: " << backendToString(b) << " / instance '" << inst
                        << "'\n";
    for (const PrereqResult &r : results) {
        out << (r.ok ? "[  ok   ] " : "[MISSING] ") << r.label;
        if (!r.ok) out << "  → " << r.remedy;
        out << "\n";
    }
    // The /data delivery and shape, informational (bd remora-4ei.89): the readiness verdict stays
    // the prereqs' — a refusing shape is the deploy's decision to explain, and it does, below.
    {
        const QStringList shapeLines =
            dataShapeReport(sp, b, rcc).split(QLatin1Char('\n'));
        out << "[ info  ] " << shapeLines.value(0) << "\n";
        for (int i = 1; i < shapeLines.size(); ++i) out << "          " << shapeLines.at(i) << "\n";
        // Same treatment for the arch stamp, and informational for the same reason: readiness is
        // the prereqs' verdict about THIS machine, and this one is about an image on a target.
        const QStringList archLines = archVariantReport(sp, b, rcc).split(QLatin1Char('\n'));
        out << "[ info  ] " << archLines.value(0) << "\n";
        for (int i = 1; i < archLines.size(); ++i) out << "          " << archLines.at(i) << "\n";
    }
    // The running encoder's published shape (bd remora-e5x.18.3): it degrades AUTONOMOUSLY under
    // VRAM pressure, and that used to live only in its stderr, which nothing watches. Local read
    // wherever the target is — the encoder only exists where the venus stack does. No file means
    // no encoder (or one from before status existed): nothing to report, not a failure.
    if (rcc.hostEncode) {
        const ProcResult st =
            sp.run({QStringLiteral("sh"), QStringLiteral("-c"),
                    QStringLiteral("cat %1 2>/dev/null")
                        .arg(hostEncodeStatusFileFor(rcc.containerName))},
                   {}, {});
        if (const auto es = parseEncoderStatus(st.out)) {
            const QString note = encoderDegradationNote(*es);
            out << "[ info  ] "
                << (note.isEmpty() ? QStringLiteral("host encoder: serving %1x%2 at full size")
                                         .arg(es->encodeW)
                                         .arg(es->encodeH)
                                   : note)
                << "\n";
        }
    }
    // Per-version source-build assets (bd remora-82c.2): a version Remora offers to build must have
    // its manifest and patch set on disk, or the build dies 40 minutes in. Informational, not part
    // of the host-readiness verdict — a missing A17 asset does not stop an A16 deploy.
    for (const VersionAssetRow &v : auditVersionAssets()) {
        out << (v.ok ? "[  ok   ] " : "[MISSING] ") << "Android " << v.version
            << " build assets";
        if (!v.ok) out << "  → missing " << v.detail;
        out << "\n";
    }
    // The patch tree against the registry. Same reason and same informational status: a stale
    // INSTALLED tree does not stop a deploy, it stops a BUILD — and it should say so here rather
    // than in the apply loop after a repo sync (bd remora-22n's residue, and the general case).
    {
        const QVector<PatchAssetRow> gaps = auditPatchAssetsOnDisk();
        if (gaps.isEmpty()) {
            out << "[  ok   ] source patch tree matches this binary's registry\n";
        } else {
            // The two directions of the same skew, each with its own remedy: missing files mean
            // the TREE is older (reinstall it), unknown files mean the BINARY is older and would
            // silently build without them (rebuild/reinstall remora, bd remora-y9y9).
            bool anyMissing = false, anyUnknown = false;
            for (const PatchAssetRow &g : gaps) {
                if (!g.detail.isEmpty()) {
                    anyMissing = true;
                    out << "[MISSING] A" << g.version << " patch " << g.key << "  → " << g.dir
                        << " lacks " << g.detail << "\n";
                }
                if (!g.unknownDetail.isEmpty()) {
                    anyUnknown = true;
                    out << "[ STALE ] A" << g.version << " patch " << g.key << "  → " << g.dir
                        << " carries " << g.unknownDetail
                        << ", which this binary's registry does not name\n";
                }
            }
            if (anyMissing)
                out << "           the installed patch tree is older than this binary — reinstall "
                       "it (cmake --install), or a source build will fail once it reaches the "
                       "patch step\n";
            if (anyUnknown)
                out << "           this binary is older than its patch tree — a build would "
                       "silently SKIP the unnamed files; rebuild remora (or reinstall the pair) "
                       "so the registry knows them\n";
        }
    }
    // The content half (bd remora-y9y9 option b): same names on both sides says nothing about
    // same BYTES, and stale bytes ship under a green build. Compared against the counterpart tree
    // when one exists; no counterpart is silence, not an ok-line, because absence of a repo
    // checkout is not evidence of agreement.
    {
        const VendorDriftReport d = auditVendorTreeDrift();
        if (!d.otherRoot.isEmpty()) {
            if (d.rows.isEmpty()) {
                out << "[  ok   ] source-patches content matches " << d.otherRoot << "\n";
            } else {
                for (const VendorDriftRow &r : d.rows) {
                    QStringList parts;
                    if (!r.differs.isEmpty())
                        parts << QStringLiteral("%1 differing").arg(r.differs.size());
                    if (!r.onlyInMine.isEmpty())
                        parts << QStringLiteral("%1 only here").arg(r.onlyInMine.size());
                    if (!r.onlyInOther.isEmpty())
                        parts << QStringLiteral("%1 only there").arg(r.onlyInOther.size());
                    out << (d.binaryStagesFromDrifted ? "[ STALE ] " : "[ info  ] ")
                        << "source-patches/" << r.dir << " drifts from " << d.otherRoot << " ("
                        << parts.join(QStringLiteral(", ")) << ")\n";
                }
                out << (d.binaryStagesFromDrifted
                            ? "           builds STAGE from this binary's tree, so the counterpart"
                              "'s edits are not what ships — reinstall (cmake --install) to "
                              "reconcile\n"
                            : "           the installed copy is inert to this binary (it stages "
                              "from the source tree) — reinstall (cmake --install) when "
                              "convenient\n");
            }
        }
    }
    const bool ready = isReady(results);
    // The rows went to stdout and the verdict goes to stderr; without this flush a redirected or
    // captured run printed "see MISSING above" ABOVE the rows it pointed at.
    out.flush();
    QTextStream(stderr) << (ready ? "\nREADY to deploy.\n" : "\nNOT ready — see MISSING above.\n");
    return ready ? 0 : 1;
}

// Positional (non-flag) args after the subcommand. Skips each flag's value for flags that take
// one (--backend x, --title x, --ssh-host x, --tag x).
static QStringList positional(const QStringList &a) {
    // Every flag that TAKES a value must be listed, or its value is read as a positional — and
    // for the commands that take an optional profile name, a stray positional is looked up as an
    // instance and the command exits 2 on "no such instance: remora24:x86_64" (bd remora-sgkg).
    static const QStringList valueFlags{"--backend", "--title",  "--ssh-host", "--tag",
                                        "--cookie",  "--user",   "--packages", "--base"};
    QStringList p;
    for (int i = 2; i < a.size(); ++i) {
        if (a[i].startsWith(QLatin1Char('-'))) {
            if (valueFlags.contains(a[i])) ++i;
            continue;
        }
        p << a[i];
    }
    return p;
}

// Config for a command that optionally names an instance: the named one (validated) or the
// active one. Used by plan/status/recipe, which previously ACCEPTED a name and threw it away —
// so `plan "Android 17"` and `plan "typo"` printed byte-identical output and neither reflected
// the profile asked for (bd remora-a4i).
static RemoraConfig configForArg(const QStringList &a, QString &instOut) {
    const QStringList pos = positional(a);
    if (pos.isEmpty()) return loadActive(instOut);
    instOut = requireInstance(pos[0]);
    return loadInstance(defaultRemorarcPath(), instOut);
}


// Refresh the desktop app-menu folder WITHOUT holding the connect open.
//
// It enumerates the device's apps, which means starting a second agent session and waiting for it —
// ~2.8s measured, on a deploy whose whole point is already achieved: the mirror is up and on screen
// before this runs. All it delays is the process exiting and the GUI's "done", so it is spent
// entirely on making the deploy LOOK slower than it was.
//
// Re-invoked as `remora apps <profile>` rather than threaded, because the CLI is about to exit and
// a background thread would die with it. Detached, so it outlives us; failures are its own to
// report, and a stale menu is not worth a failed connect.
static void syncAppMenuDetached(Spawner &sp, const QString &instance) {
    const QString self = QCoreApplication::applicationFilePath();
    if (self.isEmpty()) return;
    sp.spawnDetached({self, QStringLiteral("apps"), instance}, {});
}

static int cmdUp(const QStringList &a) {
    RealSpawner sp;
    ConsoleSink sink;
    QString inst;
    Backend b;
    // configForArg also handles the menu folder's "Android (mirror)" entry: `remora up "<profile>"`
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    QTextStream(stderr) << "remora up: " << backendToString(b) << " / instance '" << inst << "'\n";
    // gpu_mode=host on bare/remote can only work with the live render node, and the resolver
    // refuses to bake one — so without a probe the mode half-applies and the guest falls back to
    // SwiftShader (bd remora-4ei.57). Probe only in exactly that case, so a guest-mode profile
    // pays nothing for it.
    const std::optional<HostCapabilities> caps = deployCapabilities(b, cfg, sp);
    // Smart bring-up (the GUI Connect): attach to a running instance, full deploy otherwise —
    // so `up` is idempotent (the menu folder's "Android (mirror)" entry relies on this).
    const bool ok = smartRun(b, cfg, sink, sp, caps, inst,
                             [&](const Conflict &c) {
                                 QTextStream(stderr)
                                     << "! " << c.holder << " is already using " << c.reason
                                     << " — stop it first, or give this profile its own, then "
                                        "retry.\n";
                                 return false;
                             });
    if (ok && resolve(cfg, b).desktopMenu) syncAppMenuDetached(sp, inst);
    QTextStream(stderr) << (ok ? "\nconnected.\n" : "\nnot connected — see the messages above.\n");
    return ok ? 0 : 1;
}

// remora sleep [<profile>] — freeze the device in place (docker pause): zero CPU, session fully
// preserved. The next Connect/up wakes it in ~1s instead of a ~50s cold boot.
// remora update-watch [<profile>] — the host half of the in-Android updater: the device
// shows/requests updates, the HOST applies them (a container has no recovery). Loops: every 15s,
// checks the guest-side /rezmods/updater/apply signal (written by the RemoraUpdater app's Apply
// button); when present, consumes it and runs the smart deploy, which recreates the container onto
// the new image. Also keeps state.json honest each tick.
// Where the host half of the updater keeps apply/state.json. A pinned rezmods tree is the
// operator's own layout and the one the guest's /rezmods mount is fed from, so it wins when set.
// Unset, this was a hardcoded ~/rezmods: every tick of every update-watch created a VISIBLE
// directory in the root of $HOME, on profiles that never configured the feature and had nothing
// reading it back. $HOME (not ~) because the path is used inside double quotes, and it is expanded
// by whichever shell runs the probe — the REMOTE one on an ssh profile.
static QString updaterStateDir(const ResolvedConfig &rc) {
    return rc.rezmodsDir.value_or(QStringLiteral("$HOME/.local/share/remora/rezmods"));
}

static int cmdUpdateWatch(const QStringList &a) {
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    RealSpawner sp;
    ConsoleSink sink;
    const ResolvedConfig rc = resolve(cfg, b);
    const QString guest = rc.sshHost.value_or(QString());
    // Same two transports as every other host-side probe (Probe.cpp): bare runs the shell locally,
    // remote gets there over ssh. This loop only ever knew the ssh half, so on a BARE profile — the
    // preferred target, and what an unset backend= resolves to — it exited 2 the moment it started.
    // Its unit is Restart=on-failure with RestartSec=30, and 30s is wider than systemd's default
    // start-limit window, so nothing ever rate-limited the retry: the watcher restarted every 30s
    // forever (2244 times on the machine this was found on) while the in-device updater silently
    // did nothing at all on the one backend most profiles use (bd remora-veo).
    const bool remote = b != Backend::Bare;
    // Only reachable via an explicit backend=remote with no ssh_host — inferBackend reads a blank
    // ssh host as bare. A watcher with nothing to reach is a config error, not a transport choice.
    if (remote && guest.isEmpty()) {
        QTextStream(stderr) << "remora update-watch: no ssh host configured for instance '"
                            << inst << "' — set it on the Device page (or ssh_host= in "
                               "remorarc)\n";
        return 2;
    }
    QTextStream(stderr) << "remora update-watch: instance '" << inst << "' "
                        << (remote ? QStringLiteral("via ") + guest
                                   : QStringLiteral("on this machine"))
                        << " — waiting for in-device Apply (Ctrl-C to stop)\n";
    for (;;) {
        // one round trip: consume the apply signal if present AND refresh state
        // (pending = the running container's image differs from what the tag points to now).
        // The state carries what the RemoraUpdater card renders: the tag, the tag image's
        // creation time (the update's "built"), the running image's creation time ("current"),
        // and a one-line summary from the ~/rezmods/updater/summary.txt sidecar (written by
        // whoever ships an image; quotes/newlines stripped to keep the JSON valid).
        const QString probe = QStringLiteral(
            "A=0; [ -e \"%3/updater/apply\" ] && { rm -f \"%3/updater/apply\"; A=1; }; "
            "C=$(docker inspect -f '{{.Image}}' %1 2>/dev/null); "
            "T=$(docker image inspect -f '{{.Id}}' %2 2>/dev/null); "
            "TB=$(docker image inspect -f '{{.Created}}' %2 2>/dev/null | cut -c1-19); "
            "CB=''; [ -n \"$C\" ] && CB=$(docker image inspect -f '{{.Created}}' \"$C\" "
            "2>/dev/null | cut -c1-19); "
            "S=''; [ -f \"%3/updater/summary.txt\" ] && "
            "S=$(head -c 300 \"%3/updater/summary.txt\" | tr -d '\"\\\\' | tr '\\n' ' '); "
            "mkdir -p \"%3/updater\"; "
            "if [ -n \"$T\" ] && [ \"$C\" != \"$T\" ]; then P=true; else P=false; fi; "
            "printf '{\"pending\": %s, \"tag\": \"%s\", \"built\": \"%s\", "
            "\"current\": \"%s\", \"summary\": \"%s\"}' "
            "\"$P\" '%2' \"$TB\" \"$CB\" \"$S\" > \"%3/updater/state.json\"; "
            "echo APPLY=$A")
                                   .arg(rc.containerName, rc.imageTag, updaterStateDir(rc));
        const ProcResult r = remote ? sp.run(Spawner::sshArgv(guest, probe), {}, {})
                                    : sp.run({"sh", "-c", probe}, {}, {});
        if (r.out.contains(QLatin1String("APPLY=1"))) {
            QTextStream(stderr) << "apply requested from the device — updating…\n";
            smartRun(b, cfg, sink, sp, std::nullopt, inst,
                     [](const Conflict &) { return true; });
        }
        QThread::sleep(15);
    }
}

// remora auto-sleep [<profile>] — the idle watcher: when no local mirror client has been attached
// to the profile's target for auto_sleep_minutes, freeze the device (docker pause — any Connect
// wakes it in ~1s). Installed as a systemd unit by `remora service`; the config is re-read every
// tick, so the GUI toggle applies without a restart.
static int cmdAutoSleep(const QStringList &a) {
    const QStringList pos = positional(a);
    const QString path = defaultRemorarcPath();
    const QString inst = pos.isEmpty() ? activeInstance(path) : requireInstance(pos[0]);
    RealSpawner sp;
    ConsoleSink sink;
    QTextStream(stderr) << "remora auto-sleep: watching '" << inst << "' (Ctrl-C to stop)\n";
    constexpr int kPollSec = 30;
    int idleSec = 0;
    bool slept = false;   // one freeze per idle episode — a returning client re-arms
    QString target;       // cached; macvlan auto-IP re-resolves after a miss
    for (;;) {
        QThread::sleep(kPollSec);
        const RemoraConfig cfg = loadInstance(path, inst);
        const std::optional<int> mins = cfg.advanced.autoSleepMinutes;
        if (!mins || *mins <= 0) {  // disabled — stay resident so enabling applies live
            idleSec = 0;
            slept = false;
            continue;
        }
        const Backend b = inferBackend(cfg);
        if (target.isEmpty()) target = resolveAdbTarget(sp, makeContext(cfg, b));
        if (target.isEmpty()) continue;  // container gone — nothing to freeze; retry next tick
        if (countMirrorClients(sp, target) > 0) {
            idleSec = 0;
            slept = false;
            continue;
        }
        idleSec += kPollSec;
        if (slept || idleSec < *mins * 60) continue;
        QTextStream(stderr) << "no client for " << idleSec << "s — freezing '" << inst << "'\n";
        sleepRun(b, cfg, sink, sp, std::nullopt, inst);
        slept = true;  // even on failure: don't hammer a broken freeze; a client reset re-arms
    }
}

static int cmdSleep(const QStringList &a) {
    RealSpawner sp;
    ConsoleSink sink;
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    const bool ok = sleepRun(b, cfg, sink, sp, std::nullopt, inst);
    QTextStream(stderr) << (ok ? "\nasleep — `remora up` (or Connect) wakes it in ~1s.\n"
                               : "\ncould not freeze — see the messages above.\n");
    return ok ? 0 : 1;
}

// remora reset-data [<profile>] --yes — delete the profile's /data, factory-resetting the
// instance. Exists because an unprivileged `rm -rf` cannot finish the job: Android writes the tree
// as root and as its own system uids, so the user owns the profile but not its contents, and the
// removal dies partway through on EACCES (bd remora-4u4.7). Delegating it to a privileged
// container is what makes this work with no sudo at all.
static int cmdResetData(const QStringList &a, QTextStream &out) {
    RealSpawner sp;
    ConsoleSink sink;
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    const ResolvedConfig rc = resolve(cfg, b);
    if (b != Backend::Bare) {
        QTextStream(stderr) << "! reset-data only handles the bare backend — a remote profile "
                               "keeps its data dir on the ssh host.\n";
        return 1;
    }
    // --yes is required rather than prompted for: this is every app, account and setting on the
    // instance, and the command is the kind of thing that ends up in a script.
    if (!a.contains(QStringLiteral("--yes"))) {
        out << "This deletes EVERYTHING in " << rc.dataDir << " — every app, account and setting\n"
            << "on profile '" << inst << "'. The directory itself is kept.\n"
            << (rc.useOverlayfs ? "Only this instance's diff is removed; the shared /data-base "
                                  "stays.\n"
                                : "")
            << "\nRe-run with --yes to do it.\n";
        return 1;
    }
    const StepResult r = wipeDataDir(sp, rc, {});
    QTextStream(stderr) << (r.ok ? QStringLiteral("\n%1 — the next `up` boots a fresh instance.\n")
                                       .arg(r.detail)
                                 : QStringLiteral("\nnot wiped: %1\n").arg(r.detail));
    return r.ok ? 0 : 1;
}

// remora service [<profile>] [--remove] — a systemd --user unit: the device comes up at login
// (boot splash + mirror appear automatically) and is frozen in place (sleep) at logout, so the
// next login resumes the session in ~1s.
static int cmdService(const QStringList &a, QTextStream &out) {
    const QStringList pos = positional(a);
    const QString path = defaultRemorarcPath();
    const QString inst = pos.isEmpty() ? activeInstance(path) : requireInstance(pos[0]);
    QString unitInst = inst;
    unitInst.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("-"));
    const QString unitName = QStringLiteral("remora-%1.service").arg(unitInst);
    const QString unitDir = QDir::homePath() + QStringLiteral("/.config/systemd/user");
    const QString unitPath = unitDir + QLatin1Char('/') + unitName;
    RealSpawner sp;
    if (a.contains(QLatin1String("--remove"))) {
        sp.run({"systemctl", "--user", "disable", "--now", unitName}, {}, {});
        const QString watchName = QStringLiteral("remora-updater-%1.service").arg(unitInst);
        sp.run({"systemctl", "--user", "disable", "--now", watchName}, {}, {});
        const QString idleName = QStringLiteral("remora-autosleep-%1.service").arg(unitInst);
        sp.run({"systemctl", "--user", "disable", "--now", idleName}, {}, {});
        QFile::remove(unitPath);
        QFile::remove(unitDir + QLatin1Char('/') + watchName);
        QFile::remove(unitDir + QLatin1Char('/') + idleName);
        sp.run({"systemctl", "--user", "daemon-reload"}, {}, {});
        out << "removed " << unitPath << " (+ update + auto-sleep watchers)\n";
        return 0;
    }
    QDir().mkpath(unitDir);
    QFile f(unitPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(stderr) << "could not write " << unitPath << "\n";
        return 1;
    }
    // PATH: systemd user units don't include ~/.local/bin by default. The unit's own commands run
    // by absolute path (and so does the mirror, which is this binary), but the host tools a
    // deploy shells out to are found through PATH, and a user-local install of them lives there.
    f.write(QStringLiteral(
                "# Generated by `remora service` — bring the '%1' device up at login.\n"
                "[Unit]\n"
                "Description=Remora Android device — %1\n"
                "After=graphical-session.target\n"
                "PartOf=graphical-session.target\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "Environment=PATH=%h/.local/bin:/usr/local/bin:/usr/bin\n"
                "# up = attach-or-deploy with the one-window boot splash; a sleeping device\n"
                "# wakes in ~1s. The mirror detaches and outlives this oneshot.\n"
                "ExecStart=%2 up \"%1\"\n"
                "# logout/stop: freeze the device in place — zero CPU, session preserved\n"
                "ExecStop=%2 sleep \"%1\"\n"
                "TimeoutStartSec=600\n"
                "\n"
                "[Install]\n"
                "WantedBy=graphical-session.target\n")
                .arg(inst, QCoreApplication::applicationFilePath())
                .toUtf8());
    f.close();
    // Companion long-running watcher: serves the in-Android updater (Settings -> System ->
    // Remora Updater) — when the user taps Apply on the device, this recreates the container
    // onto the new image. Its own unit: the main unit is a oneshot and must not block on a loop.
    const QString watchName = QStringLiteral("remora-updater-%1.service").arg(unitInst);
    QFile wf(unitDir + QLatin1Char('/') + watchName);
    if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        wf.write(QStringLiteral(
                     "# Generated by `remora service` — host half of the in-Android updater.\n"
                     "[Unit]\n"
                     "Description=Remora update watcher — %1\n"
                     "After=graphical-session.target\n"
                     "PartOf=graphical-session.target\n"
                     "# Give up after 3 failures in 10 minutes. RestartSec=30 is wider than the\n"
                     "# DEFAULT start limit's 10s window, so without this the rate limiter never\n"
                     "# trips and a profile the watcher cannot serve restarts every 30s forever\n"
                     "# rather than failing where someone would see it (bd remora-veo).\n"
                     "StartLimitIntervalSec=600\n"
                     "StartLimitBurst=3\n"
                     "\n"
                     "[Service]\n"
                     "Type=simple\n"
                     "Environment=PATH=%h/.local/bin:/usr/local/bin:/usr/bin\n"
                     "ExecStart=%2 update-watch \"%1\"\n"
                     "Restart=on-failure\n"
                     "RestartSec=30\n"
                     "\n"
                     "[Install]\n"
                     "WantedBy=graphical-session.target\n")
                     .arg(inst, QCoreApplication::applicationFilePath())
                     .toUtf8());
        wf.close();
    }
    // Idle watcher: freezes the device after auto_sleep_minutes with no mirror client attached.
    // Installed unconditionally and cheap when the knob is unset — it re-reads the config every
    // tick, so the GUI toggle takes effect without touching systemd again.
    const QString idleName = QStringLiteral("remora-autosleep-%1.service").arg(unitInst);
    QFile idf(unitDir + QLatin1Char('/') + idleName);
    if (idf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        idf.write(QStringLiteral(
                      "# Generated by `remora service` — auto-sleep idle watcher.\n"
                      "[Unit]\n"
                      "Description=Remora auto-sleep watcher — %1\n"
                      "After=graphical-session.target\n"
                      "PartOf=graphical-session.target\n"
                      "\n"
                      "[Service]\n"
                      "Type=simple\n"
                      "Environment=PATH=%h/.local/bin:/usr/local/bin:/usr/bin\n"
                      "ExecStart=%2 auto-sleep \"%1\"\n"
                      "Restart=on-failure\n"
                      "RestartSec=30\n"
                      "\n"
                      "[Install]\n"
                      "WantedBy=graphical-session.target\n")
                      .arg(inst, QCoreApplication::applicationFilePath())
                      .toUtf8());
        idf.close();
    }
    sp.run({"systemctl", "--user", "daemon-reload"}, {}, {});
    const ProcResult en = sp.run({"systemctl", "--user", "enable", unitName}, {}, {});
    sp.run({"systemctl", "--user", "enable", watchName}, {}, {});
    sp.run({"systemctl", "--user", "enable", idleName}, {}, {});
    out << "installed + enabled " << unitPath << "\n"
        << "  + update watcher " << watchName << " (serves the in-Android updater)\n"
        << "  + auto-sleep watcher " << idleName << " (freezes an unwatched device; enable via "
        << "auto_sleep_minutes)\n"
        << "the device comes up at login and sleeps at logout.\n"
        << "start it now:   systemctl --user start " << unitName << "\n"
        << "remove it:      remora service \"" << inst << "\" --remove\n";
    return en.rc == 0 ? 0 : 1;
}

// PIP: a second, small, always-on-top mirror beside the running one, for multitasking. It does
// NOT bring the target up and does NOT touch the main mirror — the device is expected to be
// running already, exactly like `reconnect` assumes. The window gets its own class (--app-id),
// which is what a KWin/Krohnkite rule keys on (bd remora-4ei.38).
static int cmdPip(const QStringList &a, QTextStream &out) {
    const QStringList pos = positional(a);
    QString inst;
    RemoraConfig cfg;
    if (pos.isEmpty()) {
        cfg = loadActive(inst);
    } else {
        inst = requireInstance(pos[0]);
        cfg = loadInstance(defaultRemorarcPath(), inst);
    }
    const Backend b = backendArg(a, inferBackend(cfg));
    RealSpawner sp;

    // TOGGLE: bound to a hotkey, the useful behaviour is "show it / put it away", not "open a
    // seventh copy". Matched on the window title, which buildPipMirrorArgv always sets. The
    // bracket in "[R]emora PIP" keeps pgrep from matching its OWN command line — without it the
    // pattern is always found and the toggle would think a PIP is running when none is.
    //
    // The title is PER-PROFILE, so each profile's menu entry toggles its OWN PIP and leaves the
    // others up. pgrep -f takes an ERE and a profile name is arbitrary user text, so the instance
    // half is escaped — an unescaped "C++" profile would be a malformed pattern, not a miss.
    //
    // Runs BEFORE the device is resolved: putting a window away must not require the device to
    // still be reachable, or a PIP orphaned by a stopped instance cannot be dismissed.
    const QString title = pipWindowTitle(inst);
    QString pat = QStringLiteral("[") + title.left(1) + QStringLiteral("]");
    for (const QChar c : title.mid(1)) {
        if (QStringLiteral(".[]{}()*+?^$|\\").contains(c)) pat += QLatin1Char('\\');
        pat += c;
    }
    const auto pipRunning = [&sp, &pat] {
        const ProcResult r = sp.run({"pgrep", "-f", "--", pat}, {}, {});
        return r.rc == 0 && !r.out.trimmed().isEmpty();
    };
    if (pipRunning()) {
        sp.run({"pkill", "-f", "--", pat}, {}, {});
        // pkill returns once the signal is SENT, so a second press inside the mirror's teardown window
        // still saw the old process and reported "closed" again instead of reopening. Wait for it
        // to actually go — bounded, so a wedged client cannot hang the hotkey.
        for (int i = 0; i < 30 && pipRunning(); ++i) QThread::msleep(100);
        out << (pipRunning() ? "pip: close signalled, but it is still running\n" : "pip: closed\n");
        return 0;
    }

    const RunContext ctx = makeContext(cfg, b, std::nullopt, title);
    QString err;
    const QString target = resolveAdbTarget(sp, ctx, &err);
    if (target.isEmpty()) {
        out << "pip: cannot reach the device — " << (err.isEmpty() ? QStringLiteral("no target") : err)
            << "\nIs the instance running? Try `remora up`.\n";
        return 2;
    }

    auto [env, argv] = buildPipMirrorArgv(ctx.rc, target, inst);
    argv[0] = QCoreApplication::applicationFilePath();
    // Merged over the mirror's env (empty today; the client is flag-configured).
    QMap<QString, QString> spawnEnv = ctx.mirrorEnv;
    for (auto it = env.constBegin(); it != env.constEnd(); ++it) spawnEnv.insert(it.key(), it.value());
    const Detached d = sp.spawnDetached(argv, spawnEnv);
    if (d.pid <= 0) {
        out << "pip: failed to launch the mirror client\n";
        return 1;
    }
    out << "pip: " << ctx.rc.pipWidth << "x" << ctx.rc.pipHeight << " on " << target
        << " (pid " << d.pid << ", window class 'remora-pip')\n";
    return 0;
}

// Capabilities for a CONNECT-ONLY path (reconnect / watch-boot / record). Was a private helper
// here, which is exactly why the GUI never got the same treatment — see deployCapabilities().
static std::optional<HostCapabilities> connectCaps(const RemoraConfig &cfg, Backend b,
                                                   Spawner &sp) {
    return deployCapabilities(b, cfg, sp);
}

static int cmdReconnect(const QStringList &a) {
    RealSpawner sp;
    ConsoleSink sink;
    // Honour an explicit profile like up/status/check/sleep do. This used to call loadActive()
    // unconditionally and silently ignore the argument, so `reconnect "Android 16"` reconnected
    // whatever the ACTIVE profile was — reporting a failure against the wrong container
    // (naming a container that no longer exists) with nothing to suggest the name had been
    // dropped.
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    const bool ok = reconnectRun(b, cfg, sink, sp, connectCaps(cfg, b, sp), inst);
    if (ok && resolve(cfg, b).desktopMenu) syncAppMenuDetached(sp, inst);
    QTextStream(stderr) << (ok ? "\nconnected.\n"
                               : "\nnot connected — is the instance running? (try `up`)\n");
    return ok ? 0 : 1;
}

// remora watch-boot [<profile>]  — restart the device and attach the mirror during boot, so the boot
// animation is visible (the normal connect waits past it).
static int cmdWatchBoot(const QStringList &a) {
    const QStringList pos = positional(a);
    const QString path = defaultRemorarcPath();
    const QString inst = pos.isEmpty() ? activeInstance(path) : requireInstance(pos[0]);
    const RemoraConfig cfg = loadInstance(path, inst);
    const Backend b = inferBackend(cfg);
    RealSpawner sp;
    ConsoleSink sink;
    QTextStream(stderr) << "remora watch-boot: restarting '" << inst
                        << "' and attaching during boot…\n";
    const bool ok = watchBootRun(b, cfg, sink, sp, connectCaps(cfg, b, sp), inst);
    QTextStream(stderr) << (ok ? "\nattached — watch the device boot.\n"
                               : "\nnot attached — see the messages above.\n");
    return ok ? 0 : 1;
}

// remora desktop [<profile>]  — a separate desktop-mode display of the plain device (no specific
// app): what the menu folder's "Android (desktop)" entry runs.
static int cmdDesktop(const QStringList &a) {
    const QStringList pos = positional(a);
    const QString path = defaultRemorarcPath();
    const QString inst = pos.isEmpty() ? activeInstance(path) : requireInstance(pos[0]);
    RemoraConfig cfg = loadInstance(path, inst);
    cfg.mirror.fullscreenSeparateDisplay = true;  // --new-display, its own window
    const Backend b = inferBackend(cfg);
    RealSpawner sp;
    ConsoleSink sink;
    const bool ok = smartRun(b, cfg, sink, sp, std::nullopt, kDesktopTitlePrefix + inst,
                             [](const Conflict &) { return false; }, /*fullscreenDisplay=*/true);
    return ok ? 0 : 1;
}

// remora app <profile> <package> [--title "<Name>"]  — launch one Android app in its own window
// (a separate display), bringing the device up first if needed. This is what each generated
// .desktop menu entry runs. The window mode (desktop res vs freeform popup) comes from the
// profile's app_modes; an unlisted app gets the default — a freeform popup —
// RECORDED, so the GUI's Apps list shows the same truth (no first-launch prompt).
static int cmdApp(const QStringList &a) {
    const QStringList pos = positional(a);
    const QString path = defaultRemorarcPath();
    QString inst, pkg;
    if (pos.size() >= 2) { inst = pos[0]; pkg = pos[1]; }
    else if (pos.size() == 1) { inst = activeInstance(path); pkg = pos[0]; }
    if (pkg.isEmpty()) {
        QTextStream(stderr) << "usage: remora app <profile> <package> [--title <name>]\n";
        return 2;
    }
    const QString title = strArg(a, QStringLiteral("--title"), pkg);
    RemoraConfig cfg = loadInstance(path, inst);

    QString mode = appModeFor(cfg, pkg);
    if (mode.isEmpty()) {
        mode = QStringLiteral("freeform");
        setAppMode(cfg, pkg, mode);
        saveInstance(path, inst, cfg);
    }
    cfg.mirror.fullscreenSeparateDisplay = true;  // --new-display, its own window
    cfg.mirror.fullscreenApp = pkg;
    const Backend b = inferBackend(cfg);
    // "freeform-stretch" is the legacy spelling from when letterboxed popups existed —
    // every freeform popup reflows now, so both tokens take the same path.
    if (mode.startsWith(QLatin1String("freeform"))) {
        // A phone-shaped popup: its own compact display geometry instead of the inherited mirror
        // window frame, opened small (fixed height, width from the display's aspect). Backing out
        // of the app closes the window (close_on_app_exit) — a popup IS its app. NB: the
        // display keeps its system decorations — without a home task, apps that background on
        // back (instead of finishing) have nowhere to go and BACK stops working entirely.
        cfg.mirror.extra << QStringLiteral("--server-param=close_on_app_exit=true");
        const QString ff = resolve(cfg, b).freeformDisplay;
        const QStringList wh = ff.section(QLatin1Char('/'), 0, 0).split(QLatin1Char('x'));
        const int dw = wh.value(0).toInt(), dh = wh.value(1).toInt();
        const int dpi = ff.section(QLatin1Char('/'), 1, 1).toInt();
        const int h = 800;
        const int w = (dw > 0 && dh > 0) ? qMax(240, dw * h / dh) : 360;
        // Reflowing popup: the virtual display continuously follows the window
        // (flex_display), so finishing a resize relayouts the app to the new aspect; the
        // stretched render only bridges the moment in between. Flex maps window px 1:1 to
        // display px — scale the whole geometry (dpi included) down to the popup size, and
        // leave the window size to it (--window-* is rejected with flex).
        const int fdpi = (dpi > 0 && dh > 0) ? qMax(72, dpi * h / dh) : 0;
        cfg.mirror.fullscreenDisplay =
            fdpi > 0 ? QStringLiteral("%1x%2/%3").arg(w).arg(h).arg(fdpi)
                     : QStringLiteral("%1x%2").arg(w).arg(h);
        cfg.mirror.windowWidth = std::nullopt;
        cfg.mirror.windowHeight = std::nullopt;
    }
    // Pair the window with its menu entry (StartupWMClass) so the desktop shows the app's own
    // name + icon instead of the mirror's. --app-id sets Wayland app_id and X11 WM_CLASS both.
    // (flex_display is already emitted for every new-display session by buildMirrorArgv.)
    // remora- since the identity rename (bd remora-28ix.4) — must match the StartupWMClass the
    // app-menu script writes. A pre-rename .desktop pairs by title fallback until its refresh.
    cfg.mirror.extra << QStringLiteral("--app-id=remora-%1-%2").arg(appMenuSlug(inst), pkg);
    RealSpawner sp;
    ConsoleSink sink;
    const bool ok = smartRun(b, cfg, sink, sp, std::nullopt, title,
                             [](const Conflict &) { return false; }, /*fullscreenDisplay=*/true);
    return ok ? 0 : 1;
}

// remora apps [<profile>] [--remove]  — refresh (or remove) the profile's desktop-menu folder.
// Refresh needs the device running (Connect / `remora up` first); remove is purely local.
static int cmdApps(const QStringList &a) {
    const QStringList pos = positional(a);
    const QString path = defaultRemorarcPath();
    const QString inst = pos.isEmpty() ? activeInstance(path) : requireInstance(pos[0]);
    RealSpawner sp;
    ConsoleSink sink;
    if (a.contains(QStringLiteral("--remove"))) return appMenuRemove(sp, inst, sink) ? 0 : 1;
    const RemoraConfig cfg = loadInstance(path, inst);
    return appMenuSync(sp, cfg, inferBackend(cfg), inst, sink) ? 0 : 1;
}

// remora shell/logcat/prop — thin adb passthroughs. The first arg is a profile only when it names a
// stored instance; anything else passes to adb untouched, so `remora shell ls -l` runs on the
// active instance and flags like `logcat -d` are never eaten by remora's own arg parsing (which is
// why these do NOT use positional()).
static QString takeProfile(QStringList &tail) {
    const QString path = defaultRemorarcPath();
    if (!tail.isEmpty() && listInstances(path).contains(tail.first())) return tail.takeFirst();
    return activeInstance(path);
}

// Resolve the profile's adb target the way connect does (pinned target, else the macvlan IP
// assigned at provision) and assert the link once. Empty return = unreachable (message printed).
static QString adbLink(const QString &inst, Spawner &sp) {
    const RemoraConfig cfg = loadInstance(defaultRemorarcPath(), inst);
    const RunContext ctx = makeContext(cfg, inferBackend(cfg));
    QString err;
    const QString target = resolveAdbTarget(sp, ctx, &err);
    if (target.isEmpty()) {
        QTextStream(stderr) << "remora: " << err << "\n";
        return {};
    }
    sp.run({"adb", "connect", target}, {}, {});
    if (sp.run({"adb", "-s", target, "get-state"}, {}, {}).out.trimmed()
        != QLatin1String("device")) {
        QTextStream(stderr) << "remora: " << target << " is not answering adb — is '" << inst
                            << "' running? (try `remora up`)\n";
        return {};
    }
    return target;
}

// exec adb against a linked target — remora is REPLACED, fds 0/1/2 stay on the terminal, so
// `shell` is fully interactive, `logcat` streams until Ctrl-C and `install` shows push progress.
static int execAdb(const QString &target, const QStringList &tail) {
    QStringList argv{QStringLiteral("adb"), QStringLiteral("-s"), target};
    argv << tail;
    std::vector<QByteArray> bytes;
    std::vector<char *> cargv;
    for (const QString &s : argv) bytes.push_back(s.toLocal8Bit());
    for (QByteArray &b : bytes) cargv.push_back(b.data());
    cargv.push_back(nullptr);
    ::execvp(cargv[0], cargv.data());
    QTextStream(stderr) << "remora: could not exec adb — is it installed?\n";
    return 127;
}

static int cmdShell(const QStringList &a) {
    QStringList tail = a.mid(2);
    RealSpawner sp;
    const QString target = adbLink(takeProfile(tail), sp);
    return target.isEmpty() ? 1 : execAdb(target, QStringList{QStringLiteral("shell")} + tail);
}

static int cmdLogcat(const QStringList &a) {
    QStringList tail = a.mid(2);
    RealSpawner sp;
    const QString target = adbLink(takeProfile(tail), sp);
    return target.isEmpty() ? 1 : execAdb(target, QStringList{QStringLiteral("logcat")} + tail);
}

static int cmdProp(const QStringList &a) {
    QStringList tail = a.mid(2);
    const QString inst = takeProfile(tail);
    const QString verb = tail.value(0);
    QStringList adbTail;
    if (verb.isEmpty())  // bare `remora prop`: dump everything, like plain getprop
        adbTail = {QStringLiteral("shell"), QStringLiteral("getprop")};
    else if (verb == QLatin1String("get") && tail.size() == 2)
        adbTail = {QStringLiteral("shell"), QStringLiteral("getprop"), tail[1]};
    else if (verb == QLatin1String("set") && tail.size() == 3)
        adbTail = {QStringLiteral("shell"), QStringLiteral("setprop"), tail[1], tail[2]};
    else {
        QTextStream(stderr) << "usage: remora prop [<profile>] [get <key> | set <key> <value>]\n";
        return 2;
    }
    RealSpawner sp;
    const QString target = adbLink(inst, sp);
    return target.isEmpty() ? 1 : execAdb(target, adbTail);
}

// remora install [<profile>] [flags…] <apk> — push+install an APK. The device tune disables the
// Play Protect adb-install verifier on every connect, but a device linked before that fix (or
// brought up outside remora) still has it armed — re-assert here so an unknown APK can never
// sit in the multi-minute upload-consent hang, then hand the tail to `adb install`.
// remora push-app [<profile>] <localDir> — copy a companion-app directory to the profile's DOCKER
// HOST once, without deploying. Deploys already re-stage every "local:" entry (bd remora-fgj.3), so
// this is for the cases that are not a deploy: trying an app before committing it to remorarc, or
// refreshing the guest copy while the container keeps running. It prints the config line to add.
static int cmdPushApp(const QStringList &a, QTextStream &out) {
    // positional() rather than a.mid(2): the directory must not be confused with a flag's value,
    // and `push-app <profile> --backend bare <dir>` is a reasonable thing to type.
    QStringList tail = positional(a);
    const QString inst = takeProfile(tail);
    if (tail.isEmpty()) {
        QTextStream(stderr) << "usage: remora push-app [<profile>] <dir containing <Name>.apk>\n";
        return 2;
    }
    const QString src = QDir(tail.first()).absolutePath();
    if (!QFileInfo(src).isDir()) {
        QTextStream(stderr) << "remora push-app: not a directory: " << src << "\n";
        return 1;
    }
    const QString entry = QStringLiteral("local:") + src;
    const RemoraConfig cfg = loadInstance(defaultRemorarcPath(), inst);
    // --backend overrides the profile's, like every other command that takes it.
    const ResolvedConfig rc =
        resolve(cfg, backendArg(a, inferBackend(cfg)));
    const QString rez = rc.rezmodsDir.value_or(QString());
    const QString dest = companionAppHostDir(entry, rez);
    // No ssh host or no rezmods dir means the docker host IS this machine — the path is already
    // reachable, so there is nothing to copy and the entry can name it directly.
    if (!rc.sshHost || rc.sshHost->isEmpty() || rez.isEmpty()) {
        out << "docker host is local — nothing to copy\n"
            << "add to remorarc [Instance-" << inst << "]:  companion_apps=" << src << "\n";
        return 0;
    }
    RealSpawner sp;
    const QString guest = *rc.sshHost;
    // Replace rather than merge, for the same reason the deploy step does: a stale APK left from an
    // earlier build is still scanned by PackageManager and the newest file does not win.
    if (sp.run(Spawner::sshArgv(guest, QStringLiteral("rm -rf %1 && mkdir -p %1").arg(dest)), {}, {})
            .rc != 0) {
        QTextStream(stderr) << "remora push-app: could not prepare " << dest << " on " << guest
                            << "\n";
        return 1;
    }
    if (sp.run({QStringLiteral("scp"), QStringLiteral("-r"), src + QStringLiteral("/."),
                guest + QLatin1Char(':') + dest},
               {}, {})
            .rc != 0) {
        QTextStream(stderr) << "remora push-app: scp to " << guest << " failed\n";
        return 1;
    }
    out << "pushed " << src << " -> " << guest << ':' << dest << "\n"
        << "add to remorarc [Instance-" << inst << "]:  companion_apps=" << entry << "\n"
        << "(the \"local:\" form re-stages on every deploy; a bare host path does not)\n";
    return 0;
}

// Move the user-installed apps from one profile's device to another's. The motivating case is
// retiring a profile: the apps are the part worth keeping, and reinstalling thirty of them by hand
// is the reason a retired instance lingers.
//
// APKS ONLY — NOT APP DATA, and this is a deliberate limit rather than an unfinished one. Apps
// arrive as fresh installs: logged out, unconfigured. `adb backup` has been deprecated and
// increasingly ignored by apps since Android 12, and copying /data/data across instances means
// carrying SELinux contexts and per-install uids that the target assigns differently — a good way
// to produce an app that installs and then cannot start. Say so plainly rather than half-doing it.
// Either side may be a profile name OR a raw adb target (host:port). A device outliving its
// profile is the normal case for a migration — you retire the profile and still want its apps —
// and it also lets apps come from a phone or an instance this machine has no config for.
static QString transferEndpoint(const QString &nameOrTarget, Spawner &sp) {
    // A profile name must be a KNOWN profile: loadInstance() on a typo silently layers [Defaults]
    // alone and resolves somebody else's adb target — for a command that INSTALLS onto the result,
    // that is the bd remora-a4i bug with consequences. Raw host:port stays the escape hatch for
    // devices this machine has no profile for.
    if (!nameOrTarget.contains(QLatin1Char(':')))
        return adbLink(requireInstance(nameOrTarget), sp);
    return assertAdbEndpoint(sp, nameOrTarget);
}

// The data carry's root route for one side of a transfer (bd remora-4ei.73). A raw host:port has
// no profile to resolve a container from, so it gets an empty RootExec and falls back to the adb
// `su` path inside transferAppData — the only route left for a device this machine has no config
// for. Profile names reaching here are already validated by transferEndpoint's requireInstance.
static RootExec transferRoot(const QString &nameOrTarget) {
    if (nameOrTarget.contains(QLatin1Char(':'))) return {};
    const RemoraConfig cfg = loadInstance(defaultRemorarcPath(), nameOrTarget);
    return rootExecFor(cfg, inferBackend(cfg));
}

// The per-app work lives in engine/AppTransfer.cpp so the GUI can run the same operations
// through a worker (bd remora-4ei.75); these commands keep only argv parsing, endpoint
// resolution, and the header/summary lines around the streamed per-app output.
static LineSink outSink(QTextStream &out) {
    return [&out](const QString &, const QString &text) {
        out << text << "\n";
        out.flush();
    };
}

// Save installed apps to a local directory, one subdirectory per package, splits intact. The
// inverse of import-apks: together they are a backup/restore that survives the instance itself.
static int cmdExportApks(const QStringList &a, QTextStream &out) {
    QStringList tail = positional(a);
    if (tail.size() < 2) {
        QTextStream(stderr)
            << "usage: remora export-apks <profile|host:port> <dir> [--packages a,b]\n\n"
               "Saves each app to <dir>/<package>/, splits included. App DATA is not saved.\n";
        return 2;
    }
    const QString from = tail.at(0);
    const QString dir = QDir(tail.at(1)).absolutePath();
    RealSpawner sp;
    const QString src = transferEndpoint(from, sp);
    if (src.isEmpty()) {
        QTextStream(stderr) << "remora export-apks: '" << from << "' not reachable\n";
        return 1;
    }
    if (!QDir().mkpath(dir)) {
        QTextStream(stderr) << "remora export-apks: cannot create " << dir << "\n";
        return 1;
    }
    QStringList pkgs;
    const QString only = strArg(a, QStringLiteral("--packages"));
    if (!only.isEmpty()) {
        pkgs = only.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString &p : pkgs) p = p.trimmed();
    } else {
        pkgs = listThirdPartyPackages(sp, src);
    }
    if (pkgs.isEmpty()) {
        out << "no third-party apps on " << from << " — nothing to export\n";
        return 0;
    }
    out << "export-apks: " << from << " (" << src << ")  ->  " << dir << "\n"
        << pkgs.size() << " app(s)\n";
    const AppOpCounts c = exportApks(sp, src, pkgs, dir, outSink(out));
    out << "\n" << c.done << " exported, " << c.failed << " failed  ->  " << dir << "\n";
    return c.failed == 0 ? 0 : 1;
}

// Install APKs from a local directory. Accepts both shapes: the per-package layout export-apks
// writes (splits kept together), and a plain folder of standalone .apk files, which is what a
// user who just downloaded something has.
static int cmdImportApks(const QStringList &a, QTextStream &out) {
    QStringList tail = positional(a);
    if (tail.size() < 2) {
        QTextStream(stderr)
            << "usage: remora import-apks <profile|host:port> <dir> [--dry-run]\n\n"
               "Installs <dir>/<package>/*.apk (splits together), or any loose *.apk in <dir>.\n";
        return 2;
    }
    const QString to = tail.at(0);
    const QString dir = QDir(tail.at(1)).absolutePath();
    if (!QFileInfo(dir).isDir()) {
        QTextStream(stderr) << "remora import-apks: not a directory: " << dir << "\n";
        return 1;
    }
    const bool dryRun = a.contains(QStringLiteral("--dry-run"));
    RealSpawner sp;
    const QString dst = dryRun ? QStringLiteral("(dry-run)") : transferEndpoint(to, sp);
    if (dst.isEmpty()) {
        QTextStream(stderr) << "remora import-apks: '" << to << "' not reachable\n";
        return 1;
    }
    const QList<ApkJob> jobs = importJobs(dir);
    if (jobs.isEmpty()) {
        out << "no APKs found under " << dir << "\n";
        return 0;
    }
    out << "import-apks: " << dir << "  ->  " << to << " (" << dst << ")\n"
        << jobs.size() << " app(s)\n";
    const AppOpCounts c = importApks(sp, dst, jobs, dryRun, outSink(out));
    // "installed" would be a lie after a dry run, and a summary line is what gets pasted into a
    // report — it has to stand on its own without the lines above it.
    out << "\n" << c.done << (dryRun ? " would be installed, " : " installed, ") << c.failed
        << " failed\n";
    return c.failed == 0 ? 0 : 1;
}

static int cmdTransferApps(const QStringList &a, QTextStream &out) {
    QStringList tail = positional(a);
    if (tail.size() < 2) {
        QTextStream(stderr)
            << "usage: remora transfer-apps <from> <to> [--dry-run] [--with-data]\n"
               "       <from>/<to> is a profile name or a raw adb target (host:port)\n"
               "       [--packages a,b,c]   only these, instead of every third-party app\n"
               "       [--with-data]        also carry each app's /data/data (EXPERIMENTAL —\n"
               "                            needs root on both sides; a data failure leaves the\n"
               "                            app installed but logged out)\n\n"
               "Transfers installed APKs (including split APKs). Without --with-data, apps\n"
               "arrive logged out. Window-mode preferences (app_modes) DO ride along for\n"
               "the transferred apps when both sides are profiles; the target's own answers win.\n";
        return 2;
    }
    const QString from = tail.at(0), to = tail.at(1);
    if (from == to) {
        QTextStream(stderr) << "remora transfer-apps: source and target are the same profile\n";
        return 2;
    }
    const bool dryRun = a.contains(QStringLiteral("--dry-run"));
    const bool withData = a.contains(QStringLiteral("--with-data"));
    RealSpawner sp;

    // Resolve BOTH devices before touching either: a transfer that dies half way because the
    // target was never reachable is worse than one that refuses to start.
    const QString src = transferEndpoint(from, sp);
    if (src.isEmpty()) {
        QTextStream(stderr) << "remora transfer-apps: source '" << from << "' not reachable\n";
        return 1;
    }
    const QString dst = dryRun ? QStringLiteral("(dry-run)") : transferEndpoint(to, sp);
    if (dst.isEmpty()) {
        QTextStream(stderr) << "remora transfer-apps: target '" << to << "' not reachable\n";
        return 1;
    }
    out << "transfer-apps: " << from << " (" << src << ")  ->  " << to << " (" << dst << ")\n";

    QStringList pkgs;
    const QString only = strArg(a, QStringLiteral("--packages"));
    if (!only.isEmpty()) {
        pkgs = only.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString &p : pkgs) p = p.trimmed();
    } else {
        pkgs = listThirdPartyPackages(sp, src);
    }
    if (pkgs.isEmpty()) {
        out << "no third-party apps on " << from << " — nothing to transfer\n";
        return 0;
    }
    out << pkgs.size() << " app(s) to transfer\n";

    const AppOpCounts c = transferApps(sp, src, dst, pkgs, dryRun, withData,
                                       transferRoot(from), transferRoot(to), outSink(out));
    // Window-mode preferences ride along for the packages that actually landed — but only when
    // BOTH sides are profiles: a raw host:port has no remorarc section to read or write (bd
    // remora-4ei.73). Target-side answers win inside carryAppModes.
    if (!c.transferred.isEmpty() && !from.contains(QLatin1Char(':')) &&
        !to.contains(QLatin1Char(':'))) {
        const QString path = defaultRemorarcPath();
        RemoraConfig dstCfg = loadInstance(path, to);
        const int carried = carryAppModes(loadInstance(path, from), dstCfg, c.transferred);
        if (carried > 0) {
            saveInstance(path, to, dstCfg);
            out << carried << " window-mode preference(s) carried over to '" << to << "'\n";
        }
    }
    out << "\n" << c.done << (dryRun ? " would be transferred, " : " transferred, ") << c.failed
        << " failed, " << c.skipped << " skipped";
    if (withData)
        out << "; app data: " << c.dataDone << (dryRun ? " would be carried, " : " carried, ")
            << c.dataFailed << " failed";
    out << "\n";
    if (!dryRun && c.done > 0 && !withData)
        out << "NB app data was not transferred — the apps are installed but logged out.\n";
    return (c.failed == 0 && c.dataFailed == 0) ? 0 : 1;
}

static int cmdInstall(const QStringList &a) {
    QStringList tail = a.mid(2);
    const QString inst = takeProfile(tail);
    if (tail.isEmpty()) {
        QTextStream(stderr) << "usage: remora install [<profile>] [-r|-g|…] <apk>\n";
        return 2;
    }
    RealSpawner sp;
    const QString target = adbLink(inst, sp);
    if (target.isEmpty()) return 1;
    sp.run({QStringLiteral("adb"), QStringLiteral("-s"), target, QStringLiteral("shell"),
            QStringLiteral("settings put global verifier_verify_adb_installs 0; "
                           "settings put global package_verifier_enable 0")},
           {}, {});
    return execAdb(target, QStringList{QStringLiteral("install")} + tail);
}

// remora reddit-login [<profile>] --user <username> --cookie <reddit_session> — log the official
// Reddit app in inside the container from a cookie captured by a plain reddit.com web login (no
// rooted device; bd remora-6zj). The pure artifact building lives in core; the container-side
// transplant lives in engine/RedditLogin. Increment 1 takes the cookie on the command line; a later
// increment captures it through an embedded login WebView.
static int cmdRedditLogin(const QStringList &a) {
    QStringList tail = positional(a);
    const QString inst = takeProfile(tail);
    const QString user = strArg(a, QStringLiteral("--user"));
    const QString cookie = strArg(a, QStringLiteral("--cookie"));
    if (user.isEmpty() || cookie.isEmpty()) {
        QTextStream(stderr)
            << "usage: remora reddit-login [<profile>] --user <username> --cookie <reddit_session>\n"
            << "  <reddit_session> is the cookie value from a reddit.com web login (treat it like a "
               "password).\n";
        return 2;
    }
    RemoraConfig cfg = loadInstance(defaultRemorarcPath(), inst);
    const Backend b = inferBackend(cfg);
    RealSpawner sp;
    const RunContext ctx = makeContext(cfg, b);
    QTextStream out(stdout);
    const bool ok = injectRedditSession(sp, ctx, user, cookie,
                                        [&](const QString &, const QString &t) {
                                            out << "  " << t << "\n";
                                            out.flush();
                                        });
    return ok ? 0 : 1;
}

// remora rotate [<profile>] [landscape|portrait|landscape-flip|portrait-flip|0-3|auto] — manual
// rotation control (works with zero sensors: accelerometer_rotation 0 + user_rotation N), plus
// the matching gravity vector for the injectable sensors HAL (debug.remora.sensor.accel) so a
// sensors-feature image auto-rotates consistently. No argument prints the current state.
static int cmdRotate(const QStringList &a, QTextStream &out) {
    QStringList tail = a.mid(2);
    const QString inst = takeProfile(tail);
    RealSpawner sp;
    const QString target = adbLink(inst, sp);
    if (target.isEmpty()) return 1;
    const QString want = tail.value(0);
    if (want.isEmpty()) {
        const QString rot =
            sp.run({"adb", "-s", target, "shell", "settings get system user_rotation"}, {}, {})
                .out.trimmed();
        const QString acc = sp.run({"adb", "-s", target, "shell",
                                    "settings get system accelerometer_rotation"}, {}, {})
                                .out.trimmed();
        out << "rotation " << rot << " (0=landscape 1=portrait 2/3=flipped)  auto-rotate "
            << (acc == QLatin1String("1") ? "on" : "off") << "\n";
        return 0;
    }
    if (want == QLatin1String("auto")) {
        sp.run({"adb", "-s", target, "shell", "settings put system accelerometer_rotation 1"},
               {}, {});
        out << "auto-rotate on (drives rotation once the sensors feature is baked in)\n";
        return 0;
    }
    static const QMap<QString, int> names{{"landscape", 0},      {"portrait", 1},
                                          {"landscape-flip", 2}, {"portrait-flip", 3},
                                          {"0", 0},              {"1", 1},
                                          {"2", 2},              {"3", 3}};
    if (!names.contains(want)) {
        QTextStream(stderr) << "usage: remora rotate [<profile>] "
                               "[landscape|portrait|landscape-flip|portrait-flip|0-3|auto]\n";
        return 2;
    }
    const int r = names.value(want);
    // The gravity vector the injected accelerometer should report for this orientation — a
    // harmless no-op on images without the sensors HAL.
    static const char *const grav[] = {"0,9.8,0", "9.8,0,0", "0,-9.8,0", "-9.8,0,0"};
    sp.run({"adb", "-s", target, "shell",
            QStringLiteral("settings put system accelerometer_rotation 0; "
                           "settings put system user_rotation %1; "
                           "setprop debug.remora.sensor.accel %2")
                .arg(r)
                .arg(QLatin1String(grav[r]))},
           {}, {});
    out << want << " (rotation " << r << ")\n";
    return 0;
}

// remora gboard-reset [<profile>] [--wipe] [--yes] — un-pin Gboard's on-screen keyboard.
//
// Tapping the keyboard icon on Gboard's physical-keyboard pill expands the on-screen keyboard, and
// Gboard REMEMBERS that with no visible way back: force-stop, ESC/BACK, toggling
// show_ime_with_hard_keyboard, and recreating the container were all confirmed not to reverse it.
// That matters more than it sounds, because the pill IS the horizontal-toolbar feature
// (bd remora-a41) — one mis-tap disables it permanently.
//
// THE RECORD IS FOUND (bd remora-ms3w, verified in both directions on a scratch instance):
// pinning adds the physical keyboard's NAME to the string-set pref "show_vk_devices_names" —
// show the virtual keyboard despite these devices — and the set lives in Gboard's
// DEVICE-ENCRYPTED prefs, /data/user_de/0/<pkg>/shared_prefs/<pkg>_preferences.xml. Gboard is
// direct-boot aware, so its live prefs are under /data/user_de, not /data/data — which is why
// the original hunt through shared_prefs came up empty: it enumerated the credential-encrypted
// side. Deleting that one element with Gboard force-stopped restores the pill on the next
// focused field and touches nothing else.
//
// So the DEFAULT is now that surgical delete, and it runs over docker exec (adb shell is uid
// 2000; the DE path is app-private) on the profile's container — the same two transports as
// every host-side probe. pm clear stays behind --wipe for the day Gboard moves the record: it
// takes the learned dictionary, clipboard history and every setting including "Show horizontal
// toolbar", so it remains loud and consent-gated.
static int cmdGboardReset(const QStringList &a, QTextStream &out) {
    QStringList tail = a.mid(2);
    takeProfile(tail);  // strip the positional; configAndBackend re-reads it from `a`
    const bool yes = tail.removeAll(QStringLiteral("--yes")) > 0;
    const bool wipe = tail.removeAll(QStringLiteral("--wipe")) > 0;
    QString inst;
    Backend b;
    const RemoraConfig cfg = configAndBackend(a, inst, b);
    RealSpawner sp;

    if (!wipe) {
        const ResolvedConfig rc = resolve(cfg, b);
        const QString guest = rc.sshHost.value_or(QString());
        const bool remote = b != Backend::Bare;
        if (remote && guest.isEmpty()) {
            QTextStream(stderr) << "remora gboard-reset: no ssh host configured for instance '"
                                << inst << "' — set it on the Device page (or ssh_host= in "
                                   "remorarc)\n";
            return 2;
        }
        // Force-stop FIRST: a live Gboard rewrites the file from memory on its own schedule, so
        // an edit under it can be silently undone. Ownership is restored because sed -i renames
        // a root-owned temp into place; Gboard would survive that (its own writes rename into
        // the app-owned dir), but there is no reason to leave the window open. The second sed
        // expression is the real one; the first catches a hypothetical self-closing empty set so
        // the range can never run past the element to the next </set>.
        const QString script = QStringLiteral(
            "P=/data/user_de/0/com.google.android.inputmethod.latin/shared_prefs/"
            "com.google.android.inputmethod.latin_preferences.xml\n"
            "pm path com.google.android.inputmethod.latin >/dev/null 2>&1 || "
            "{ echo NOPKG; exit 0; }\n"
            "grep -q \"show_vk_devices_names\" \"$P\" 2>/dev/null || { echo NOKEY; exit 0; }\n"
            "own=$(stat -c \"%u:%g\" \"$P\")\n"
            "am force-stop com.google.android.inputmethod.latin\n"
            "sed -i -e \"/show_vk_devices_names.*\\/>/d\" "
            "-e \"/show_vk_devices_names/,/<\\/set>/d\" \"$P\"\n"
            "chown \"$own\" \"$P\"; chmod 660 \"$P\"\n"
            "grep -q \"show_vk_devices_names\" \"$P\" && echo EDITFAIL || echo OK");
        // base64 through both transports: sshArgv single-quotes its payload and the host shell
        // parses the double quotes below, so the script itself must not depend on which layer
        // strips what — encoded, it never takes part in quoting at all.
        const QString cmd =
            QStringLiteral("docker exec %1 sh -c \"echo %2 | base64 -d | sh\"")
                .arg(rc.containerName,
                     QString::fromLatin1(script.toUtf8().toBase64()));
        const ProcResult r = remote ? sp.run(Spawner::sshArgv(guest, cmd), {}, {})
                                    : sp.run({"sh", "-c", cmd}, {}, {});
        const QString said = r.out.trimmed();
        if (said.contains(QLatin1String("NOPKG"))) {
            out << "Gboard (com.google.android.inputmethod.latin) is not installed here — "
                   "nothing to reset.\n"
                << "Gboard is not part of any Remora image; it comes from Play. Install it and "
                   "this\ncommand becomes relevant again — the bug is Gboard's, not the "
                   "image's.\n";
            return 0;
        }
        if (said.contains(QLatin1String("NOKEY"))) {
            out << "No pinned-keyboard record (show_vk_devices_names) in Gboard's prefs — the "
                   "pill\nshould already be active. If the on-screen keyboard is still pinned, "
                   "Gboard has\nmoved the record; fall back to `remora gboard-reset --wipe`.\n";
            return 0;
        }
        if (said.contains(QLatin1String("OK"))) {
            out << "Un-pinned: removed show_vk_devices_names from Gboard's device-encrypted "
                   "prefs.\nThe physical-keyboard pill returns on the next focused text field. "
                   "Dictionary,\nclipboard and settings are untouched.\n";
            return 0;
        }
        QTextStream(stderr) << "gboard-reset: the surgical edit did not take ("
                            << (said.isEmpty() ? r.stderrTail : said)
                            << ") — fall back to `remora gboard-reset --wipe`.\n";
        return 1;
    }

    const QString target = adbLink(inst, sp);
    if (target.isEmpty()) return 1;
    // Gboard is NOT baked into any Remora image — the gapps payload is GmsCore, Phonesky and the
    // sync adapters, no keyboard — so it is whatever the user installed from Play. That makes its
    // absence normal rather than exceptional, and a pm clear against a missing package would print
    // a confusing failure instead of saying so.
    const QString pkg = QStringLiteral("com.google.android.inputmethod.latin");
    const QString have =
        sp.run({"adb", "-s", target, "shell", QStringLiteral("pm list packages %1").arg(pkg)}, {}, {})
            .out.trimmed();
    if (!have.contains(pkg)) {
        const QString ime =
            sp.run({"adb", "-s", target, "shell", "settings get secure default_input_method"}, {}, {})
                .out.trimmed();
        out << "Gboard (" << pkg << ") is not installed here — nothing to reset.\n"
            << "The current IME is " << (ime.isEmpty() ? QStringLiteral("<unset>") : ime) << ".\n"
            << "Gboard is not part of any Remora image; it comes from Play. Install it and this\n"
            << "command becomes relevant again — the bug is Gboard's, not the image's.\n";
        return 0;
    }
    if (!yes) {
        out << "This CLEARS ALL Gboard DATA on " << target << " (pm clear " << pkg << ").\n"
            << "You lose: the learned dictionary, clipboard history, and every Gboard setting —\n"
            << "including \"Show horizontal toolbar\", which you will have to switch back on.\n"
            << "The default (no --wipe) un-pins surgically and loses nothing — use this only if\n"
            << "that reported the record missing (bd remora-ms3w).\n\n"
            << "Re-run with --wipe --yes to do it.\n";
        return 2;
    }
    const ProcResult r =
        sp.run({"adb", "-s", target, "shell", QStringLiteral("pm clear %1").arg(pkg)}, {}, {});
    const QString said = r.out.trimmed();
    if (r.rc != 0 || !said.contains(QLatin1String("Success"))) {
        QTextStream(stderr) << "pm clear failed: " << (said.isEmpty() ? r.stderrTail : said) << "\n";
        return 1;
    }
    out << "Gboard data cleared. The physical-keyboard pill returns on the next focused field.\n"
        << "Re-enable Gboard settings > Preferences > \"Show horizontal toolbar\" to get the "
           "toolbar back.\n";
    return 0;
}

// remora shot [<profile>] [<out.png>] — one-click device screenshot (no mirror needed).
static int cmdShot(const QStringList &a, QTextStream &out) {
    QStringList tail = a.mid(2);
    const QString inst = takeProfile(tail);
    RealSpawner sp;
    const QString target = adbLink(inst, sp);
    if (target.isEmpty()) return 1;
    const QString path =
        tail.isEmpty()
            ? QDir::current().filePath(
                  QStringLiteral("remora-shot-%1-%2.png")
                      .arg(appMenuSlug(inst), QDateTime::currentDateTime().toString(
                                                  QStringLiteral("yyyyMMdd-HHmmss"))))
            : tail.first();
    QString err;
    if (!captureScreenshot(sp, target, path, &err)) {
        QTextStream(stderr) << "remora: " << err << "\n";
        return 1;
    }
    out << path << "\n";
    return 0;
}

// remora record [<profile>] [<out.mp4>] — relaunch the mirror with --record. The recorded
// session REPLACES the running mirror (a second client shares the VF encoder and costs it
// bandwidth); closing the mirror window ends the recording and finalizes the mp4.
static int cmdRecord(const QStringList &a, QTextStream &out) {
    QStringList tail = a.mid(2);
    const QString inst = takeProfile(tail);
    const QString path =
        tail.isEmpty()
            ? QDir::current().filePath(
                  QStringLiteral("remora-rec-%1-%2.mp4")
                      .arg(appMenuSlug(inst), QDateTime::currentDateTime().toString(
                                                  QStringLiteral("yyyyMMdd-HHmmss"))))
            : tail.first();
    RemoraConfig cfg = loadInstance(defaultRemorarcPath(), inst);
    cfg.mirror.extra << QStringLiteral("--record=%1").arg(path);
    RealSpawner sp;
    ConsoleSink sink;
    const Backend rb = inferBackend(cfg);
    const bool ok = reconnectRun(rb, cfg, sink, sp, connectCaps(cfg, rb, sp), inst);
    if (ok) out << path << "\n";
    QTextStream(stderr) << (ok ? "recording — close the mirror window to finish the file.\n"
                               : "not recording — is the instance running? (try `up`)\n");
    return ok ? 0 : 1;
}

// remora doctor [<profile>] [--stdout] — bundle the probes, versions, device props and recent
// logcat into one shareable file for triage. Default: a timestamped file in the current dir;
// --stdout prints instead.
static int cmdDoctor(const QStringList &a, QTextStream &out) {
    QStringList tail = a.mid(2);
    const bool toStdout = tail.removeAll(QStringLiteral("--stdout")) > 0;
    const QString inst = takeProfile(tail);
    const RemoraConfig cfg = loadInstance(defaultRemorarcPath(), inst);
    RealSpawner sp;
    QTextStream(stderr) << "remora doctor: probing '" << inst << "' — this takes ~15s…\n";
    const QString report = doctorReport(sp, cfg, inferBackend(cfg), inst);
    if (toStdout) {
        out << report;
        return 0;
    }
    const QString path = QDir::current().filePath(
        QStringLiteral("remora-doctor-%1-%2.txt")
            .arg(appMenuSlug(inst),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(stderr) << "remora: could not write " << path << "\n";
        return 1;
    }
    f.write(report.toUtf8());
    f.close();
    out << path << "\n";
    return 0;
}

// Name any default-on feature this profile's pinned features= never picked up. Once that key is in
// remorarc it is an explicit list and a feature registered later never joins it, so the image is
// built without it and nothing anywhere says so — the same silent pinned-list hole the source
// patches had (bd remora-4ei.44). stderr, so it survives `remora recipe > Dockerfile`.
static void warnUnappliedDefaultFeatures(const RemoraConfig &cfg) {
    const QSet<QString> feats(cfg.image.features.begin(), cfg.image.features.end());
    const QStringList missed = unappliedDefaultFeatures(feats);
    if (missed.isEmpty()) return;
    QTextStream(stderr) << "# NOT BUILT IN (not enabled in this profile): " << missed.join(", ")
                        << "\n#   these ship enabled by default; this profile pinned its feature "
                           "list before they were added.\n#   Enable them on the Image page, or "
                           "add them to features= in remorarc.\n";
}

// Name any enabled feature whose required source patches this build will not apply. Unlike the two
// warnings around it this is not about a pinned list going stale — the patches are opt-in and were
// simply never ticked — so the build is green and the feature is absent at runtime, which is how
// sensors reached a live image with no HAL at all (bd remora-4ei.53). stderr, same as the rest.
static void warnFeaturePatchGaps(const RemoraConfig &cfg, const QStringList &enabledPatches,
                                 int androidVersion) {
    const QSet<QString> feats(cfg.image.features.begin(), cfg.image.features.end());
    QTextStream err(stderr);
    // First the harder gap: a feature that simply CANNOT be built on this version, because a patch
    // it requires has no assets here (bd remora-82c.3). Enabling the patch is not the fix — there
    // is nothing to enable — so this is a distinct, louder message than the inert-feature one, and
    // it runs first because it subsumes it (an inapplicable patch is also a not-enabled one).
    QSet<QString> unbuildable;
    for (const FeatureVersionGap &g : featureVersionGaps(feats, androidVersion, readVendorPath)) {
        unbuildable.insert(g.feature);
        err << "# FEATURE NOT BUILDABLE ON ANDROID " << androidVersion << ": '" << g.feature
            << "' requires source patch(es) that do not apply to this version: "
            << g.inapplicablePatches.join(QStringLiteral(", "))
            << "\n#   there is nothing to enable — the series has no assets (or is absorbed "
               "upstream) for A"
            << androidVersion << ". Drop the feature, or build a version it applies to.\n";
    }
    // Then the inert-feature gap, skipping any feature already reported as un-buildable above so the
    // two messages do not stack on the same feature.
    for (const FeaturePatchGap &g : featurePatchGaps(feats, enabledPatches, androidVersion)) {
        if (unbuildable.contains(g.feature)) continue;
        err << "# FEATURE WILL DO NOTHING: '" << g.feature
            << "' needs source patches that are not enabled: "
            << g.missingPatches.join(QStringLiteral(", "))
            << "\n#   the build succeeds and ships the feature inert. Tick them on the Source "
               "page, or add them to source_patches= in remorarc.\n";
    }
}

// The vendored PlayIntegrityFix profile is baked into the image, so a stale one is re-shipped by
// every build until someone happens to look inside a running container — which is how the pinned
// oriole_beta profile reached 15 months old and Google's blocklist (bd remora-4ei.47).
// Only when play_spoof is on: the file is vendored unconditionally but nothing reads it otherwise.
static void warnStalePifProfile(const RemoraConfig &cfg) {
    if (!cfg.image.features.contains(QStringLiteral("play_spoof"))) return;
    const auto files = readVendorPath(QStringLiteral("source-build/features/zygisk_pif/pif.json"));
    if (files.isEmpty()) return;
    const auto age = stalePifProfile(QString::fromUtf8(files.first().second), QDate::currentDate());
    if (!age) return;
    QTextStream err(stderr);
    err << "# STALE PLAY SPOOF PROFILE: vendor/source-build/features/zygisk_pif/pif.json is ";
    if (age->monthsOld < 0)
        err << "undatable (SECURITY_PATCH '" << age->securityPatch << "')\n";
    else
        err << age->monthsOld << " months old (SECURITY_PATCH " << age->securityPatch << ")\n";
    err << "#   this build bakes it in. Google blocklists old fingerprints, and a banned profile "
           "is a\n#   POSITIVE tamper signal — worse than not spoofing at all. Refresh the file "
           "on the host;\n#   the module's own autopif4.sh hangs in-container (bd remora-4ei.47).\n";
}

static int cmdRecipe(const QStringList &a, QTextStream &out) {
    QString inst;
    const RemoraConfig cfg = configForArg(a, inst);
    QSet<QString> feats(cfg.image.features.begin(), cfg.image.features.end());
    if (feats.isEmpty()) feats = defaultBuildSet(cfg.image.androidVersion.value_or(16));
    // --base pins what to layer onto, instead of letting the registry pick (bd remora-sgkg). The
    // clean-mirror case needs it: with both remora24:x86_64 and remora24:x86_64-gapps registered,
    // the ranking always prefers the tag that already bakes gapps, so "I downloaded the clean
    // image, add GApps to IT" is not otherwise expressible.
    const QString baseOverride = strArg(a, QStringLiteral("--base"));
    const ImageRecipe r =
        buildRecipe(feats, cfg.image.androidVersion.value_or(16), QString(),
                    cfg.image.sourceTree.value_or(QString()),
                    cfg.image.sourceKind.value_or(QString()), baseOverride);
    if (!r.needsBuild && !r.needsSourceBuild)
        QTextStream(stderr) << "# base image " << r.baseTag
                            << " already satisfies these features — no build needed\n";
    if (r.needsSourceBuild) QTextStream(stderr) << "# " << r.sourceBuildHint << "\n";
    {
        QStringList pats = cfg.image.sourcePatches;
        if (pats.isEmpty())
            pats = defaultSourcePatchSet(cfg.image.androidVersion.value_or(16));
        // ONLY on the source-build path. A recipe that resolves to a pre-built tag layers over an
        // image whose patches were applied when it was built — sensors is baked into the -hwc2
        // golden, verified live (bd remora-4ei.8) — so warning there would fire on the one
        // configuration that is provably fine, which is how a warning class gets ignored.
        if (r.needsSourceBuild) {
            warnFeaturePatchGaps(cfg, pats, cfg.image.androidVersion.value_or(16));
            warnStalePifProfile(cfg);
        }
        QTextStream(stderr) << "# input fingerprint: "
                            << imageBuildFingerprint(pats,
                                                     QStringList(feats.begin(), feats.end()),
                                                     cfg.image.androidVersion.value_or(16),
                                                     cfg.image.sourceRef.value_or(QString()))
                            << " (built_recipe="
                            << cfg.image.builtRecipe.value_or(QStringLiteral("<unset>")) << ")\n";
    }
    warnUnappliedDefaultFeatures(cfg);
    out << r.dockerfile;
    return 0;
}

// `remora build --source` — the full AOSP source rebuild, the same chain the GUI's "Build from
// source" runs (engine/SourceBuild), so a verification build no longer needs a GUI button click.
// That mattered: bd remora-25u, 4ei.39, 4ei.27 and 4ei.30 all close on "one real build", and the
// only way to get one was to be sitting at the workspace.
// Streams to the terminal and to /tmp/remora-build.log, exactly like the GUI path, and judges
// success by the EXIT CODE — never by grepping output, because a command that failed silently must
// not read as a passing build.
static int cmdSourceBuild(const QStringList &a, QTextStream &out) {
    QString inst;
    const RemoraConfig cfg = configForArg(a, inst);
    const QString host = cfg.image.sourceHost.value_or(QString());
    const QString tree = cfg.image.sourceTree.value_or(QString());
    if (tree.isEmpty()) {
        out << "build --source: no source_tree configured for instance '" << inst
            << "'. Set Source directory in the Image page's Source build card, or add "
               "source_tree= to remorarc.\n";
        return 2;
    }
    const bool lineage = cfg.image.sourceKind.value_or(QStringLiteral("lineage"))
                         != QLatin1String("aosp");
    const int androidVer = cfg.image.androidVersion.value_or(16);
    QSet<QString> feats(cfg.image.features.begin(), cfg.image.features.end());
    if (feats.isEmpty()) feats = defaultBuildSet(cfg.image.androidVersion.value_or(16));
    warnUnappliedDefaultFeatures(cfg);
    QStringList enabled = cfg.image.sourcePatches;
    if (enabled.isEmpty()) enabled = defaultSourcePatchSet(androidVer);
    for (const QString &k : unappliedDefaultPatches(enabled, androidVer))
        QTextStream(stderr) << "# NOT APPLIED (not enabled in this profile): " << k << "\n";
    warnFeaturePatchGaps(cfg, enabled, androidVer);
    warnStalePifProfile(cfg);
    // MESA IS THE ONLY GPU STACK THE REMORA PRODUCT HAS (bd remora-28ix.4). Dropping the
    // prebuilt GPU-driver project from the manifests removed the second source of every GL driver
    // and Vulkan ICD, so a source build of remora_x86_64 without mesa_source does not degrade to
    // the old Mesa 24.0.8 prebuilts — it produces an image with no libgallium_dri, no libEGL_mesa,
    // no GLES pair and no ICD but lavapipe and pastel. It builds green and cannot composite.
    //
    // KEYED ON THE PRODUCT, NOT ON THE VERSION NUMBER. lunchConfigFor() is the authoritative
    // version→product mapping (do-build.sh's own case is documented as mirroring it, not the other
    // way round), and it is the Remora device tree that has no fallback — not "17" as such. A
    // future release that lunches remora_x86_64 inherits the requirement without an edit here, and
    // Since the A16 carry-over (bd remora-31sq) BOTH releases lunch remora_x86_64, so the gate
    // fires for both — verified by driving it: an A16 profile without mesa_source is refused
    // naming Android 16. That arrived with zero edits here, which is the point of the key.
    //
    // This is the FAST copy of a check that also exists in device-remora/remora.mk as a $(error).
    // Neither is redundant: this one answers in about a second and is what anyone using the engine
    // or the GUI hits, while the makefile one is about two minutes into product config and is the
    // one a manual do-build.sh run — which never passes through here at all — runs into. The
    // requirement is deliberately NOT expressed as a feature default; see the long note on
    // mesa_source in Features.cpp for why both obvious ways of doing that are wrong.
    if (lunchConfigFor(androidVer).startsWith(QStringLiteral("remora_x86_64"))
        && !feats.contains(QStringLiteral("mesa_source"))) {
        QTextStream(stderr)
            << "build --source: the Android " << androidVer << " product (remora_x86_64) requires "
            << "the 'mesa_source' build feature, and this profile does not enable it.\n"
            << "  The Remora device tree carries no prebuilt GL or Vulkan stack to fall back on, "
               "so this\n"
               "  build would finish cleanly and produce an image with no GL driver and no Vulkan "
               "ICD\n"
               "  but lavapipe and pastel.\n"
            << "Enable \"Mesa from source\" in the build features, or add mesa_source to the "
               "profile's features= line.\n"
            << "The payload itself is built host-side first: "
               "vendor/host-prereqs/mesa-android/build-mesa.sh\n";
        return 2;
    }
    // THE USB AUDIO PATCH WITHOUT ITS FEATURE BREAKS ALL AUDIO, so it is refused rather than
    // warned about (bd remora-4ei.37). The asymmetry is the point: the feature WITHOUT the patch
    // ships a HAL that is never opened — inert, and covered by the "FEATURE WILL DO NOTHING"
    // warning above, which is proportionate because nothing regresses. The patch WITHOUT the
    // feature adds an <xi:include> for usb_audio_policy_configuration.xml, which only the feature
    // installs; an xi:include of a missing file fails the WHOLE audio policy parse, so capture,
    // playback and the mirror's own submix all die together. That is a strictly worse image than
    // the one the profile started with, which is the line this project draws for a hard failure.
    if (enabled.contains(QStringLiteral("usb_audio_policy"))
        && !feats.contains(QStringLiteral("usb_audio"))) {
        QTextStream(stderr)
            << "build --source: source patch 'usb_audio_policy' is enabled but the 'usb_audio' "
               "build feature is not.\n"
            << "  The patch declares a USB module in the top-level audio policy; the FEATURE is "
               "what installs\n"
               "  the usb_audio_policy_configuration.xml fragment that declaration points at. "
               "Without it the\n"
               "  policy xi:includes a file that is not on the image, which fails the whole parse "
               "— that breaks\n"
               "  ALL audio, not just recording.\n"
            << "Enable \"Microphone (USB audio HAL)\" in the build features, or remove "
               "usb_audio_policy from the\n"
               "profile's source_patches= line.\n";
        return 2;
    }
    // REFUSE HERE, not in the apply loop. A patch file the registry names but the tree does not
    // carry is already fatal — the apply loop hard-fails on it by design (bd remora-fgj.11) — but
    // it does so after repo bring-up, which on a cold tree is the expensive part of the build. The
    // condition is knowable now, so ask now.
    //
    // Scoped to what THIS build would apply: this version, and only the enabled entries. `check`
    // reports every gap because a stale tree is worth knowing about in general; a build must not
    // be blocked by an opt-in entry it was never going to touch.
    {
        // Two refusals with opposite remedies, never conflated: LACKING files fail the apply loop
        // loudly (the tree is older — reinstall it); UNNAMED files never reach the apply loop at
        // all (the binary is older — it would build green and ship WITHOUT them, bd remora-y9y9's
        // five-of-six trap), which is the worse failure precisely because nothing downstream
        // catches it. Both are knowable now, so both refuse now.
        QStringList lacking, unnamed;
        for (const PatchAssetRow &g : auditPatchAssetsOnDisk()) {
            if (g.version != androidVer || !enabled.contains(g.key)) continue;
            if (!g.detail.isEmpty())
                lacking << QStringLiteral("  %1: %2 lacks %3").arg(g.key, g.dir, g.detail);
            if (!g.unknownDetail.isEmpty())
                unnamed << QStringLiteral("  %1: %2 carries %3")
                               .arg(g.key, g.dir, g.unknownDetail);
        }
        if (!lacking.isEmpty() || !unnamed.isEmpty()) {
            QTextStream err(stderr);
            if (!lacking.isEmpty())
                err << "build --source: the patch tree does not carry what this binary's registry "
                       "names, so the build would fail at the patch step:\n"
                    << lacking.join(QLatin1Char('\n')) << "\n"
                    << "The installed vendor tree is older than this binary — reinstall it "
                       "(cmake --install <builddir>) and build again.\n";
            if (!unnamed.isEmpty())
                err << "build --source: the patch tree carries files this binary's registry does "
                       "not name, and a build would silently SKIP them:\n"
                    << unnamed.join(QLatin1Char('\n')) << "\n"
                    << "This binary is older than its patch tree — rebuild remora (or run the "
                       "./build/remora that matches the tree) and build again.\n";
            return 2;
        }
    }

    // Kind=aosp with lineage evidence in the profile is refused before anything is staged: the
    // aosp arm skips the manifest sync and every container-compat patch, so building anyway
    // produces the wrong image without saying so. Same gate the GUI applies (aospKindConflict).
    if (!lineage) {
        const QString clash = aospKindConflict(cfg.image.sourceRef.value_or(QString()), enabled);
        if (!clash.isEmpty()) {
            out << clash << "\n";
            return 2;
        }
    }

    RealSpawner sp;
    SourceBuildPlan plan;
    plan.tree = tree;
    plan.patchRoot = stageSourceAssets(sp, host);
    plan.sourceBuildDir = plan.patchRoot.left(plan.patchRoot.lastIndexOf(QLatin1Char('/')))
                          + QStringLiteral("/source-build");
    plan.enabledPatches = enabled;
    plan.lineage = lineage;
    plan.pinned = lineage && cfg.image.sourcePinned.value_or(true);
    plan.repoUrl = cfg.image.sourceRepo.value_or(defaultSourceRepo(lineage));
    plan.ref = cfg.image.sourceRef.value_or(QString());
    plan.androidVersion = androidVer;
    plan.buildJobs = cfg.image.buildJobs.value_or(0);
    plan.buildMemGiB = cfg.image.buildMemGiB.value_or(0);
    plan.soongMemGiB = cfg.image.soongMemGiB.value_or(0);
    plan.withGapps = feats.contains(QStringLiteral("gapps"));
    plan.withMicrog = feats.contains(QStringLiteral("microg"));
    plan.withNdk = feats.contains(QStringLiteral("arm_translate"));
    plan.withMagisk = feats.contains(QStringLiteral("magisk_root"));
    plan.withUpdater = feats.contains(QStringLiteral("system_updater"));
    plan.withPlaySpoof = feats.contains(QStringLiteral("play_spoof"));
    plan.withShamiko = feats.contains(QStringLiteral("shamiko"));
    plan.withCamera = feats.contains(QStringLiteral("camera_v4l2"));
    plan.withUsbAudio = feats.contains(QStringLiteral("usb_audio"));
    plan.withMesaSource = feats.contains(QStringLiteral("mesa_source"));
    plan.withAgent = feats.contains(QStringLiteral("mirror_agent"));
    plan.archVariant = cfg.image.archVariant.value_or(QString());
    plan.requiredPatchOwners = featureRequiredPatchOwners(feats);
    plan.builtTag = sourceBuiltTag(androidVer, lineage, plan.withGapps, plan.withMicrog,
                                   feats.contains(QStringLiteral("widevine_l3")),
                                   feats.contains(QStringLiteral("hw_video_decode")),
                                   plan.archVariant);

    const QString cmd = sourceBuildCommand(sourceBuildSteps(plan, readVendorPath));
    out << "instance=" << inst << "  tree=" << tree
        << "  on=" << (host.isEmpty() ? QStringLiteral("local") : host)
        << "\ntag=" << plan.builtTag << "  patches=" << enabled.size()
        << "  jobs=" << plan.buildJobs << "  mem_cap="
        << (plan.buildMemGiB > 0 ? QString::number(plan.buildMemGiB) + QStringLiteral(" GiB")
                                 : QStringLiteral("auto"))
        << "\nlog=/tmp/remora-build.log — this is a multi-hour build; Ctrl-C stops it.\n\n";
    out.flush();

    // pty so soong/ninja emit their progress lines, same as the GUI path; the wrapper appends the
    // sentinel status line that judgePtyExit reads below.
    const QStringList argv = ptyRunArgv(cmd, host);
    QFile log(QStringLiteral("/tmp/remora-build.log"));
    // The banner above promised this file; if it cannot be written (someone else's stale copy in
    // /tmp, a read-only /tmp) say so now rather than let the build run with its log going nowhere.
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate))
        QTextStream(stderr) << "warning: cannot write " << log.fileName() << " (" << log.errorString()
                            << ") — the build runs, but only the terminal will show its output\n";
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    posixShellEnv(p);  // `script -c` uses $SHELL; a fish login shell cannot run this chain
#ifdef Q_OS_UNIX
    // OWN PROCESS GROUP, so an interrupt reaps the WHOLE chain. `script` is what we start, but the
    // build is its descendants — killing only the parent leaves repo/ninja/docker running detached.
    // MEASURED the first time this command was exercised: a `timeout` on remora left the chain
    // running and it had to be hunted down by pgid. Ctrl-C promising to stop a build has to
    // actually stop it, and the GUI's closeEvent already does exactly this.
    p.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    p.start(argv.first(), argv.mid(1));
    if (!p.waitForStarted(10000)) {
        out << "build --source: could not start " << argv.first() << "\n";
        return 1;
    }
#ifdef Q_OS_UNIX
    // SIGINT/SIGTERM -> tear down the group, not just us. killpg is async-signal-safe; nothing else
    // here is, so the handler does only that and lets the read loop below notice the exit.
    static qint64 gBuildPid = 0;
    gBuildPid = p.processId();
    struct Sig {
        static void stop(int) {
            if (gBuildPid > 0) ::killpg(static_cast<pid_t>(gBuildPid), SIGTERM);
        }
    };
    ::signal(SIGINT, &Sig::stop);
    ::signal(SIGTERM, &Sig::stop);
#endif
    QString vTail;  // the last stretch of output, for the sentinel — the log may have failed to open
    while (p.state() != QProcess::NotRunning) {
        if (p.waitForReadyRead(1000)) {
            const QByteArray chunk = p.readAll();
            log.write(chunk);
            log.flush();
            out << QString::fromUtf8(chunk);
            out.flush();
            vTail = (vTail + QString::fromUtf8(chunk)).right(2048);
        }
    }
    const QByteArray tail = p.readAll();
    log.write(tail);
    log.close();
    out << QString::fromUtf8(tail);
    vTail = (vTail + QString::fromUtf8(tail)).right(2048);
    // Never the wrapper's code alone: a `script` killed at session teardown kills the shell and
    // exits 0, and this line read 'source build OK' while ninja was still mid-compile in the
    // detached builder (bd remora-c7y). The sentinel is the chain's own report; no sentinel means
    // no verdict exists — which is "interrupted", not "failed", and never "OK".
    const int wrapperRc = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 128;
    const PtyExit ex = judgePtyExit(wrapperRc, vTail);
    int rc = ex.rc;
    // The postcondition the exit code stands in for: OK must mean the tag exists. The chain ends
    // in assemble-image.sh, so a truthful 0 always leaves the image behind.
    QString noImage;
    if (!ex.interrupted && rc == 0) {
        const ProcResult img = sp.run(
            host.isEmpty()
                ? QStringList{QStringLiteral("docker"), QStringLiteral("image"),
                              QStringLiteral("inspect"), plan.builtTag}
                : QStringList{QStringLiteral("ssh"), host,
                              QStringLiteral("docker image inspect ") + plan.builtTag},
            {}, {});
        if (img.rc != 0) {
            rc = 1;
            noImage = QStringLiteral(" — the chain exited 0 but the expected image %1 does not "
                                     "exist").arg(plan.builtTag);
        }
    }
    if (ex.interrupted)
        out << QStringLiteral(
                   "\nsource build INTERRUPTED (exit %1) — the pty wrapper died before the build "
                   "reported a status, so there is no verdict. The builder container is detached "
                   "and may still be compiling: check `docker ps` and tail the log before "
                   "deploying anything. log: /tmp/remora-build.log\n")
                   .arg(rc);
    else
        out << (rc == 0
                    ? QStringLiteral("\nsource build OK — log: /tmp/remora-build.log\n")
                    : QStringLiteral(
                          "\nsource build FAILED (exit %1)%2 — log: /tmp/remora-build.log\n")
                          .arg(rc)
                          .arg(noImage));
    // An action killed by the container's cgroup cap leaves the container alive, so ninja fails and
    // the outer code is 1 rather than 137 — say what it actually was (bd remora-4ei.30).
    if (rc != 0) {
        QFile lf(QStringLiteral("/tmp/remora-build.log"));
        if (lf.open(QIODevice::ReadOnly)) {
            const qint64 tailBytes = 512 * 1024;
            if (lf.size() > tailBytes) lf.seek(lf.size() - tailBytes);
            const QString tailText = QString::fromUtf8(lf.readAll());
            // A latched read-only tree reports a generic exit 1 with the cause buried in
            // out/verbose.log.gz — say it plainly (bd remora-4ei.20).
            if (parseReadOnlyTree(tailText))
                out << "  THE SOURCE TREE WENT READ-ONLY mid-build. This is not a build error and "
                       "retrying will not help. If it is btrfs it latched after a failed write "
                       "barrier — check `btrfs device stats` (a lone flush timeout with wr/rd/"
                       "corrupt at 0 is NOT a failing drive), then unmount fully and mount again; "
                       "`mount -o remount,rw` will not clear it (bd remora-4ei.20).\n";
            if (parseInnerOomKill(tailText))
                out << "  OOM-KILLED INSIDE THE CONTAINER — this is the --memory cap, not "
                       "systemd-oomd. Check `journalctl -k` for \"Memory cgroup out of memory\"; "
                       "note the build log stamps UTC and the journal local, so searching it with a "
                       "build-log timestamp finds nothing. Raise build_mem_gib, or lower "
                       "NINJA_HIGHMEM_NUM_JOBS (bd remora-4ei.30).\n";
        }
    }
    return rc == 0 ? 0 : 1;
}

// Build + install the NVIDIA Venus host renderer (bd remora-4ei.56). The recipe itself is the
// vendored script — it clones, patches, compiles and verifies — so this only locates it, passes
// the install root, and streams its output. Deliberately NOT part of `up`: it clones two repos and
// compiles virglrenderer, which is not something a connect should ever do behind the user's back.
static int cmdVenusBuild(const QStringList &a, QTextStream &out) {
    const QString script = vendorScript(QStringLiteral("host-prereqs/venus-nvidia"),
                                        QStringLiteral("build-venus.sh"));
    if (!QFileInfo::exists(script)) {
        QTextStream(stderr) << "remora venus-build: recipe not found: " << script << "\n";
        return 1;
    }
    QMap<QString, QString> env;
    const QString dir = strArg(a, QStringLiteral("--dir"));
    if (!dir.isEmpty()) env["REMORA_VENUS_DIR"] = dir;
    const QString tag = strArg(a, QStringLiteral("--tag"));
    if (!tag.isEmpty()) env["REMORA_VENUS_TAG"] = tag;

    out << "building the NVIDIA Venus host renderer (this clones and compiles — minutes, not "
           "seconds)\n";
    out.flush();
    RealSpawner sp;
    const ProcResult r = sp.run({"bash", script}, env, [&out](const QString &, const QString &t) {
        out << t << "\n";
        out.flush();
    });
    if (r.rc != 0) {
        QTextStream(stderr) << "remora venus-build: failed\n"
                            << (r.stderrTail.isEmpty() ? QString() : r.stderrTail + "\n");
        return 1;
    }
    out << "\nvenus stack installed. `remora check --backend bare` should now report it, and a\n"
           "bare profile with gpu_mode=host will pick it up automatically.\n";
    return 0;
}

static int cmdBuild(const QStringList &a, QTextStream &out) {
    QString inst;
    const RemoraConfig cfg = loadActive(inst);
    const Backend backend = backendArg(a, inferBackend(cfg));
    const ResolvedConfig rc = resolve(cfg, backend);

    QSet<QString> feats(cfg.image.features.begin(), cfg.image.features.end());
    if (feats.isEmpty()) feats = defaultBuildSet(cfg.image.androidVersion.value_or(16));
    warnUnappliedDefaultFeatures(cfg);  // before the build, not after it
    const QString tag = strArg(a, QStringLiteral("--tag"));
    // Same --base as `recipe`, and it has to be here too: printing an overlay you cannot then
    // build is the half that does not deliver anything (bd remora-sgkg).
    const ImageRecipe recipe =
        buildRecipe(feats, cfg.image.androidVersion.value_or(16), tag,
                    cfg.image.sourceTree.value_or(QString()),
                    cfg.image.sourceKind.value_or(QString()),
                    strArg(a, QStringLiteral("--base")));

    if (!recipe.needsBuild && recipe.unbuildable.isEmpty()) {
        out << "base image " << recipe.baseTag
            << " already satisfies the selected features — nothing to build\n";
        return 0;
    }
    RealSpawner sp;
    ConsoleSink sink;
    const bool ok = runChain(imageBuildSteps(sp, rc, recipe), sink);
    if (ok)
        out << "\nbuilt " << recipe.tag << " on "
            << rc.buildHost.value_or(QStringLiteral("local docker")) << "\narchive: "
            << imageArchivePath(rc.outputDir, recipe.tag)
            << "\nset image_tag=" << recipe.tag << " in remorarc to use it\n";
    return ok ? 0 : 1;
}

static void printUsage(QTextStream &out) {
    out << "remora (C++/Qt6) — commands:\n"
        << "  plan      [--backend bare|remote]\n"
        << "  status    [--backend …]\n"
        << "  check     [--backend …] [--ssh-host user@host] [--gpu-host]\n"
        << "  certify   [<profile>] [--recheck] [--restore]  Play Protect state + the GSF\n"
           "                                     android_id to register; --restore writes back the\n"
           "                                     saved checkin identity after a /data rebuild\n"
        << "  up        [<profile>]     connect — deploys + boots first when not running\n"
        << "  sleep     [<profile>]     freeze the device (docker pause) — up wakes it in ~1s\n"
        << "  auto-sleep [<profile>]    idle watcher: freeze after auto_sleep_minutes with no window\n"
        << "  update-watch [<profile>]  serve the in-Android updater: apply-on-request loop\n"
        << "  service   [<profile>] [--remove]  systemd --user unit: up at login, sleep at logout\n"
        << "  reset-data [<profile>] --yes   factory-reset: delete the profile's /data (bare)\n"
        << "  reconnect [--backend …]   attach to a running instance\n"
        << "  app       <profile> <pkg> launch one Android app in its own window\n"
        << "  apps      [<profile>] [--remove]  refresh/remove the desktop app-menu folder\n"
        << "  desktop   [<profile>]     open a separate desktop-mode display (no specific app)\n"
        << "  pip       [<profile>]     toggle a small always-on-top second mirror (device up)\n"
        << "  shell     [<profile>] [cmd…]   adb shell on the device (interactive without cmd)\n"
        << "  logcat    [<profile>] [args…]  stream the device log (adb logcat passthrough)\n"
        << "  prop      [<profile>] [get <k> | set <k> <v>]  read/write Android properties\n"
        << "  install   [<profile>] [-r|-g|…] <apk>  push+install an APK (no Play-Protect hang)\n"
        << "  reddit-login [<profile>]  log the Reddit app in via an embedded reddit.com login window\n"
        << "               [--user <name> --cookie <reddit_session>]  …or headless from a captured cookie\n"
        << "  doctor    [<profile>] [--stdout]  diagnostics bundle: probes+versions+logcat, one file\n"
        << "  rotate    [<profile>] [portrait|landscape|…|auto]  rotate the device display\n"
        << "  gboard-reset [<p>] [--wipe] un-pin Gboard's on-screen keyboard (surgical; --wipe\n"
        << "                              --yes falls back to clearing ALL Gboard data)\n"
        << "  shot      [<profile>] [<out.png>]  save a device screenshot (no mirror needed)\n"
        << "  record    [<profile>] [<out.mp4>]  relaunch the mirror recording; close it to finish\n"
        << "  watch-boot [<profile>]    restart the device + attach during boot (see boot anim)\n"
        << "  push-app [<p>] <dir>      copy a companion-app dir to the docker host\n"
        << "  recipe    [--base tag]    print the custom-image Dockerfile\n"
           "                            --base layers onto THAT image instead of the best-matching\n"
           "                            registry tag — how GApps goes onto a clean mirrored image\n"
        << "  build     [--tag t] [--base tag]  build the recipe (docker/source hosts + dirs in remorarc)\n"
        "  build --source [<p>]    FULL AOSP source rebuild — the same chain the GUI runs\n"
        << "  venus-build [--dir d] [--tag v|latest]  build+install the NVIDIA Venus host renderer\n"
        << "  transfer-apps <from> <to> [--dry-run] [--packages a,b] [--with-data]\n"
           "                            move installed APKs between profiles (not app data)\n"
        << "  export-apks <from> <dir> [--packages a,b]   save installed APKs to a local dir\n"
        << "  import-apks <to> <dir> [--dry-run]          install APKs from a local dir\n"
           "                            <from>/<to> is a profile name or a raw adb target\n"
        << "  gui                       launch the config workspace\n";
}

int main(int argc, char **argv) {
    // Branch before constructing an app object — GUI needs QApplication, CLI needs QCoreApplication,
    // and only one may exist. Peek argv directly (QCoreApplication::arguments() isn't up yet).
    if (argc > 1 && QLatin1String(argv[1]) == QLatin1String("gui")) return runGui(argc, argv);

    // The in-house mirror client (bd remora-28ix.2) — a QApplication of its own, same pattern.
    if (argc > 1 && QLatin1String(argv[1]) == QLatin1String("mirror")) return runMirror(argc, argv);

    // `reddit-login` with no --cookie opens an embedded reddit.com login window (WebEngine needs a
    // QApplication); with --cookie it stays headless (handled below under QCoreApplication).
    if (argc > 1 && QLatin1String(argv[1]) == QLatin1String("reddit-login")) {
        bool hasCookie = false;
        for (int i = 2; i < argc; ++i)
            if (QLatin1String(argv[i]) == QLatin1String("--cookie")) hasCookie = true;
        if (!hasCookie) return runRedditLoginGui(argc, argv);
    }

    // `app` may show the first-launch desktop/freeform prompt — give it a QApplication when a
    // display is reachable (menu launches always have one); everything else stays headless.
    std::unique_ptr<QCoreApplication> app;
    if (argc > 1 && QLatin1String(argv[1]) == QLatin1String("app")
        && (qEnvironmentVariableIsSet("DISPLAY") || qEnvironmentVariableIsSet("WAYLAND_DISPLAY")))
        app = std::make_unique<QApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("remora"));
    QTextStream out(stdout);
    const QStringList args = QCoreApplication::arguments();
    const QString cmd = args.size() > 1 ? args[1] : QString();

    // `--help` or `-h` after a command asks for help; it never runs the command. Each command
    // reads only the flags it knows, so these used to be ignored, and `remora up --help` deployed
    // the active profile for real. The passthrough commands are exempt: their
    // arguments belong to the tool they hand them to (`remora shell ls -h`).
    static const QStringList passthrough{QStringLiteral("shell"), QStringLiteral("logcat"),
                                         QStringLiteral("install"), QStringLiteral("prop")};
    const QStringList rest = args.mid(2);
    if (!passthrough.contains(cmd) &&
        (rest.contains(QStringLiteral("--help")) || rest.contains(QStringLiteral("-h")))) {
        printUsage(out);
        return 0;
    }

    if (cmd == QLatin1String("plan")) return cmdPlan(args, out);
    if (cmd == QLatin1String("status")) return cmdStatus(args, out);
    if (cmd == QLatin1String("check")) return cmdCheck(args, out);
    if (cmd == QLatin1String("certify")) return cmdCertify(args, out);
    if (cmd == QLatin1String("up")) return cmdUp(args);
    if (cmd == QLatin1String("sleep")) return cmdSleep(args);
    if (cmd == QLatin1String("auto-sleep")) return cmdAutoSleep(args);
    if (cmd == QLatin1String("update-watch")) return cmdUpdateWatch(args);
    if (cmd == QLatin1String("service")) return cmdService(args, out);
    if (cmd == QLatin1String("reset-data")) return cmdResetData(args, out);
    if (cmd == QLatin1String("pip")) return cmdPip(args, out);
    if (cmd == QLatin1String("reconnect")) return cmdReconnect(args);
    if (cmd == QLatin1String("app")) return cmdApp(args);
    if (cmd == QLatin1String("apps")) return cmdApps(args);
    if (cmd == QLatin1String("desktop")) return cmdDesktop(args);
    if (cmd == QLatin1String("shell")) return cmdShell(args);
    if (cmd == QLatin1String("logcat")) return cmdLogcat(args);
    if (cmd == QLatin1String("prop")) return cmdProp(args);
    if (cmd == QLatin1String("install")) return cmdInstall(args);
    if (cmd == QLatin1String("reddit-login")) return cmdRedditLogin(args);
    if (cmd == QLatin1String("doctor")) return cmdDoctor(args, out);
    if (cmd == QLatin1String("rotate")) return cmdRotate(args, out);
    if (cmd == QLatin1String("gboard-reset")) return cmdGboardReset(args, out);
    if (cmd == QLatin1String("shot")) return cmdShot(args, out);
    if (cmd == QLatin1String("record")) return cmdRecord(args, out);
    if (cmd == QLatin1String("watch-boot")) return cmdWatchBoot(args);
    if (cmd == QLatin1String("push-app")) return cmdPushApp(args, out);
    if (cmd == QLatin1String("recipe")) return cmdRecipe(args, out);
    if (cmd == QLatin1String("build"))
        return args.contains(QStringLiteral("--source")) ? cmdSourceBuild(args, out)
                                                         : cmdBuild(args, out);
    if (cmd == QLatin1String("venus-build")) return cmdVenusBuild(args, out);
    if (cmd == QLatin1String("transfer-apps")) return cmdTransferApps(args, out);
    if (cmd == QLatin1String("export-apks")) return cmdExportApks(args, out);
    if (cmd == QLatin1String("import-apks")) return cmdImportApks(args, out);

    // A word that is not a command is an error, and scripts need to see it as one: this used to
    // print the usage and exit 0 for `remora bogus`, exactly as for a bare `remora`.
    const bool unknown = !cmd.isEmpty() && cmd != QLatin1String("help") &&
                         cmd != QLatin1String("--help") && cmd != QLatin1String("-h");
    if (unknown) QTextStream(stderr) << "remora: unknown command '" << cmd << "'\n\n";
    printUsage(out);
    return unknown ? 2 : 0;
}
