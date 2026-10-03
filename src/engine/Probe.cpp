#include "engine/Engine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTextStream>

#include "core/Gating.h"
#include "core/Parsers.h"
#include "core/Prereqs.h"
#include "core/Resolver.h"

namespace remora {

// EVERY /dev/dri/renderD* with its DRM driver, one "node driver" line each (bd remora-4ei.79 —
// this replaced two single-driver scans that only ever knew xe and nvidia, which left
// hostRenderDriver a boolean in disguise and every other GPU anonymous in check/plan). Which node
// is which is NOT stable across boots or machines — on this host renderD128 is nvidia and
// renderD129 is xe, the opposite of the numbering one would guess — so the driver is always read
// from sysfs rather than assumed.
// Third field: the card sibling, paired by PCI device (bd remora-n4qx). The card* glob skips
// connector entries (card0-DP-1) via the dash check. Absent when unpairable — the parser accepts
// the two-field form, so a host whose sysfs disagrees degrades to "no sibling known".
static const QString kAllNodesScan = QStringLiteral(
    "for r in /dev/dri/renderD*; do [ -e \"$r\" ] || continue; "
    "rn=$(basename \"$r\"); "
    "dev=$(readlink -f \"/sys/class/drm/$rn/device\" 2>/dev/null); "
    "drv=$(basename \"$(readlink -f \"/sys/class/drm/$rn/device/driver\" 2>/dev/null)\"); "
    "card=; for c in /sys/class/drm/card*; do cn=$(basename \"$c\"); "
    "case \"$cn\" in *-*) continue;; esac; "
    "[ \"$(readlink -f \"$c/device\" 2>/dev/null)\" = \"$dev\" ] && card=\"/dev/dri/$cn\" && break; "
    "done; echo \"$r $drv $card\"; done");

// The docker host's primary LAN attachment, for the macvlan parameters (bd remora-400). Both
// halves in one shell round trip, separated by a marker line, because they are one answer: the
// default route names the NIC and the gateway, and only that NIC's own address carries the prefix.
// `ip -o` keeps every record on one line, which is what makes the parse a split rather than a
// state machine.
static const QString kHostLanScan = QStringLiteral(
    "ip -o -4 route show default 2>/dev/null; echo '--'; ip -o -4 addr show scope global 2>/dev/null");

static std::optional<LanInterface> parseHostLanScan(const QString &out) {
    QStringList routes, addrs;
    bool afterMarker = false;
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        if (line.trimmed() == QLatin1String("--")) { afterMarker = true; continue; }
        (afterMarker ? addrs : routes) << line;
    }
    if (!afterMarker) return std::nullopt;  // the scan did not complete — do not half-parse it
    return parseHostLan(routes.join(QLatin1Char('\n')), addrs.join(QLatin1Char('\n')));
}

// Sort the scan's nodes into the capability slots: the FIRST node the guest could actually open
// becomes the render node (any driver — naming it is a statement of intent, and the resolver's
// per-driver rules key off the driver name), while nvidia is captured separately for Venus —
// the guest never opens the NVIDIA node directly, and gbm gralloc cannot drive it. vgem is virtual
// GEM: a render node in name only, never a renderer.
static void fillRenderNodes(HostCapabilities &c, const QList<RenderNode> &nodes) {
    c.renderNodes = nodes;  // the full list — the multi-GPU picker's data (bd remora-e5x.1)
    for (const RenderNode &n : nodes) {
        if (n.driver == QLatin1String("nvidia")) {
            if (!c.hostNvidiaRenderNode.has_value()) c.hostNvidiaRenderNode = n.node;
            continue;
        }
        if (n.driver == QLatin1String("vgem")) continue;
        if (!c.hostRenderNode.has_value()) {
            c.hostRenderNode = n.node;
            c.hostRenderDriver = n.driver;
        }
    }
}

QList<RenderNode> probeRenderNodes(Spawner &sp) {
    return parseRenderNodes(sp.run({"sh", "-c", kAllNodesScan}).out);
}

std::optional<HostCapabilities> deployCapabilities(Backend backend, const RemoraConfig &cfg,
                                                   Spawner &sp) {
    const ResolvedConfig pre = resolve(cfg, backend);
    // venus is checked as well as gpu_mode because an explicit venus=true is honoured by the
    // resolver regardless of the probe — so without this its preflight is the thing that fails.
    if (pre.gpuMode != GpuMode::Host && !pre.venus) return std::nullopt;
    return probeCapabilities(backend, pre.sshHost.value_or(QString()), sp);
}

// Venus host prerequisites, checked on the DOCKER HOST in one round-trip. Two lines out: the
// render-server binary (empty if absent) and the guest-lib dir (empty unless EVERY library is
// there — a half-populated dir would produce a container that boots to a black screen, so
// "partially present" is deliberately reported as "not available").
static const QString kVenusScan = QStringLiteral(
    "root=${REMORA_VENUS_DIR:-$HOME/.local/share/remora/venus}; "
    "if [ -x \"$root/bin/virgl_test_server\" ]; then echo \"$root/bin/virgl_test_server\"; "
    "else echo; fi; ok=1; "
    "for l in vulkan.virtio.so libgbm_mesa_wrapper.so libEGL_angle.so libGLESv2_angle.so "
    "libGLESv1_CM_angle.so; do "
    "[ -f \"$root/guest-libs/$l\" ] || ok=0; done; "
    "if [ \"$ok\" = 1 ]; then echo \"$root/guest-libs\"; else echo; fi");

// Host-encode readiness, in the same one-scan style as kVenusScan (bd remora-e5x.10).
//   line 0: the frame encoder binary, or blank
//   line 1: "1" when this ffmpeg lists an hevc_vulkan ENCODER
//   line 2: "1" when the installed render server carries the frame publisher, i.e. it is a
//           patched build — an unpatched one publishes nothing and the encoder waits forever
static QString hostEncodeScan(const QString &encoderPath) {
    return QStringLiteral(
               "if [ -x '%1' ]; then echo '%1'; else echo; fi; "
               "if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' hevc_vulkan '; "
               "then echo 1; else echo 0; fi; "
               "root=${REMORA_VENUS_DIR:-$HOME/.local/share/remora/venus}; "
               "if strings \"$root/bin/virgl_render_server\" 2>/dev/null "
               "| grep -q 'connected to encoder'; then echo 1; else echo 0; fi")
        .arg(encoderPath);
}

// Play Protect certification, in one round trip (bd remora-4ei.83).
//   line 0: the GSF android_id, or blank when GMS has never checked in
//   line 1: 1 = certified, 0 = GMS is currently posting its uncertified-device notification
//
// THE ID IS READ OUT OF Checkin.xml, not the authoritative gservices.db, because BOTH obvious
// routes are dead ends on this image: the bundled /system/bin/sqlite3 core-dumps under
// `docker exec`, and the gservices CONTENT PROVIDER refuses adb without READ_GSERVICES. Checkin.xml
// is plain XML and carries the identical value (verified against gservices.db on the live A17).
//
// THE VERDICT GREP IS ANCHORED ON NotificationRecord ON PURPOSE. "uncertified_device" also names a
// registered notification CHANNEL, which is present on a perfectly healthy device — so the obvious
// `grep -q uncertified_device` reports EVERY instance as uncertified. Only a posted record is the
// verdict, and it carries flags=ONGOING_EVENT, so it cannot be dismissed out from under the probe.
//
// Deliberately contains NO single quotes, so the remote path can wrap the whole script in them.
//
// THE PAIR COMES FROM /data/misc_ce/0/checkin/checkin_id_token, WITH Checkin.xml AS THE FALLBACK.
// That file is the identity's one authoritative home — plain text, "<android_id>:<securityToken>",
// and every other copy on the device (gservices.db, Checkin.xml, GMS files/*, GSF prefs) is a cache
// that re-seeds from it. Proven by deleting all seven caches offline and watching the same pair come
// back on the next boot (bd remora-4ei.84). Reading it here gets the id AND the token in the same
// round trip, which is what makes a snapshot restorable — Checkin.xml carries the id alone, and an
// id without its token cannot authenticate a checkin.
static const QString kPlayCertScan = QStringLiteral(
    "pair=$(cat /data/misc_ce/0/checkin/checkin_id_token 2>/dev/null); "
    "id=${pair%%:*}; tok=${pair#*:}; "
    "if [ -z \"$pair\" ]; then "
    "id=$(sed -n \"s/.*android_id\\\">\\([0-9]*\\)<.*/\\1/p\" "
    "/data/data/com.google.android.gms/shared_prefs/Checkin.xml 2>/dev/null | head -1); tok=; fi; "
    "echo \"$id\"; "
    "if dumpsys notification 2>/dev/null | grep -q "
    "\"NotificationRecord.*channel=uncertified_device\"; then echo 0; else echo 1; fi; "
    "echo \"$tok\"; "
    // The identity file's age, for the pending-verdict downgrade (bd remora-4ei.90): absence of
    // the notice proves nothing for an identity GMS has not had a verdict cycle to look at.
    // Both timestamps from the same host, so the age has no clock skew in it; an unreadable file
    // emits an empty line and the parser keeps the old reading.
    "m=$(stat -c %Y /data/misc_ce/0/checkin/checkin_id_token 2>/dev/null); "
    "echo \"$m\"; date +%s");

// Ask GMS to check in again. This is what re-evaluates a newly-registered android_id; without it
// the verdict only refreshes on GMS's own ~11h schedule or a reboot.
static const QString kPlayCheckinPoke =
    QStringLiteral("am broadcast -a android.server.checkin.CHECKIN >/dev/null 2>&1");

PlayCertification probePlayCertification(Spawner &sp, Backend backend, const QString &containerName,
                                         const QString &sshHost, bool forceCheckin,
                                         const QString &snapshotInstance) {
    if (containerName.isEmpty()) return {};
    if (backend != Backend::Bare && sshHost.isEmpty()) return {};
    // Same two transports probeStatus uses: bare runs docker locally as argv (no quoting at all),
    // remote reaches it over ssh with the script single-quoted.
    const auto onDevice = [&](const QString &script) {
        if (backend == Backend::Bare)
            return sp.run({"docker", "exec", containerName, "sh", "-c", script}, {}, {});
        return sp.run(Spawner::sshArgv(sshHost, QStringLiteral("docker exec %1 sh -c '%2'")
                                                    .arg(containerName, script)),
                      {}, {});
    };
    if (forceCheckin) {
        onDevice(kPlayCheckinPoke);
        // Checkin is asynchronous: reading the notification back immediately would report the
        // PREVIOUS verdict and look like the registration had not worked.
        sp.waitMs(4000);
    }
    const PlayCertification c = parsePlayCertification(onDevice(kPlayCertScan).out);
    // Opportunistic and unconditional on the verdict: an UNCERTIFIED identity is worth keeping too.
    // It is still this instance's identity, the user may be about to register it, and losing it
    // between the reading and the registering is precisely the sequence that costs a session.
    if (!snapshotInstance.isEmpty() && !c.identityPair().isEmpty())
        saveCheckinSnapshot(snapshotInstance, c.identityPair());
    return c;
}

// ─────────────── Checkin identity snapshot / restore (bd remora-4ei.84) ───────────────
//
// A /data rebuild makes GSF check in fresh and mint a NEW identity, and the old one's Play Protect
// registration does not carry over: the instance silently de-certifies and Google sign-in stops
// working until the user pastes a new number into Google's form. Keeping a copy of the pair turns
// that manual step into a restore.
//
// This was originally deferred as too risky — "writing a stale id back into gservices.db risks a
// half-restored identity that is worse than a clean one". That reasoning was aimed at a CACHE.
// gservices.db is not where the identity lives, and writing the PAIR back into the file that IS is
// simply what GSF itself does; every cache re-seeds from it on the next start (bd remora-4ei.84).

// Readable AND collision-free: the profile name lower-cased for a human reading the directory, plus
// a hash so two profiles cannot land on one file by punctuation alone ("Android 17"/"Android-17").
static QString instanceSlug(const QString &instance) {
    QString stem;
    for (const QChar c : instance) stem += c.isLetterOrNumber() ? c.toLower() : QLatin1Char('-');
    return stem + QLatin1Char('-') +
           QString::fromLatin1(
               QCryptographicHash::hash(instance.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
}

QString checkinSnapshotPath(const QString &instance) {
    if (instance.isEmpty()) return QString();
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/remora/checkin/") + instanceSlug(instance);
}

// Byte-identical to the device's own file, so it can be copied back by hand exactly as well as by
// Remora — which matters for a credential the user may need to move between machines.
bool saveCheckinSnapshot(const QString &instance, const QString &pair) {
    const QString path = checkinSnapshotPath(instance);
    if (path.isEmpty() || pair.isEmpty()) return false;
    // Never rewrite an unchanged file: this runs off every status probe, and a snapshot whose mtime
    // churned on every GUI refresh would be unreadable as history.
    // A checkin can be made in this device's name with these two numbers, so they are owner-only
    // FROM THE FIRST BYTE: the file is created 0600 rather than tightened after writing, which left
    // a window where it existed with the umask's 0644 — and the directory is 0700, because on a
    // home that others can traverse the listing alone says which devices have an identity here.
    // Tightened before the unchanged-file early return, so an existing install is fixed too.
    const QString dir = QFileInfo(path).absolutePath();
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner);
    if (loadCheckinSnapshot(instance) == pair) return true;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    // open(…, permissions) only applies on creation; a snapshot from before this is tightened too.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool ok = f.write(pair.toUtf8()) == pair.toUtf8().size();
    f.close();
    return ok;
}

std::optional<QString> loadCheckinSnapshot(const QString &instance) {
    const QString path = checkinSnapshotPath(instance);
    if (path.isEmpty()) return std::nullopt;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QString pair = QString::fromUtf8(f.readAll()).trimmed();
    // Shape-check on the way out, not just on the way in: this value is written onto a device, and a
    // truncated or hand-edited file must fail here rather than half-way through a restore.
    static const QRegularExpression pairRe(QStringLiteral("\\A[0-9]{1,25}:[0-9]{1,25}\\z"));
    if (!pairRe.match(pair).hasMatch()) return std::nullopt;
    return pair;
}

// One sentence on where the snapshot stands relative to the device, shared so the CLI and the GUI
// cannot drift on the FACT. Each surface adds its own call to action, because the way to run a
// restore differs between them.
QString checkinSnapshotNote(const QString &instance, const PlayCertification &live) {
    const auto snap = loadCheckinSnapshot(instance);
    if (!snap)
        return QStringLiteral("none yet — a /data rebuild would need a fresh registration");
    if (*snap == live.identityPair())
        return QStringLiteral("saved, matches the device — a /data rebuild is recoverable");
    return QStringLiteral("saved, DIFFERS from the device (%1)").arg(*snap);
}

// Write the snapshot back onto the device and make GSF adopt it.
//
// ORDER MATTERS AND IS NOT NEGOTIABLE: force-stop FIRST. A live GMS holds the identity in memory and
// rewrites these files from it, so a restore performed against a running GMS can be silently undone —
// that is exactly what made the first attempt at this experiment uninterpretable.
//
// The caches are then REMOVED rather than rewritten, gservices.db included. Everything re-seeds from
// the authoritative file, so deleting them is both simpler and safer than trying to keep seven
// copies consistent; and a stale android_id left in gservices.db would be the one cache still
// disagreeing. gservices.db is server-provided config that GMS re-fetches at the next checkin.
CheckinRestore restoreCheckinIdentity(Spawner &sp, Backend backend, const QString &containerName,
                                      const QString &sshHost, const QString &instance) {
    CheckinRestore res;
    const auto snap = loadCheckinSnapshot(instance);
    if (!snap) {
        res.detail = QStringLiteral("no usable snapshot for this instance");
        return res;
    }
    res.pair = *snap;
    if (containerName.isEmpty() || (backend != Backend::Bare && sshHost.isEmpty())) {
        res.detail = QStringLiteral("no reachable container to restore onto");
        return res;
    }
    res.attempted = true;
    // No single quotes: the remote path wraps the whole script in them.
    const QString script =
        QStringLiteral(
            "G=/data/data/com.google.android.gms; S=/data/data/com.google.android.gsf; "
            "U=$(stat -c %u $G 2>/dev/null); "
            "if [ -z \"$U\" ]; then echo NO_GMS; exit 0; fi; "
            "am force-stop com.google.android.gms >/dev/null 2>&1; "
            "am force-stop com.google.android.gsf >/dev/null 2>&1; "
            "mkdir -p /data/misc_ce/0/checkin; "
            "printf %s %1 > /data/misc_ce/0/checkin/checkin_id_token || { echo WRITE_FAILED; exit 0; }; "
            "chown $U:$U /data/misc_ce/0/checkin/checkin_id_token; "
            "chmod 600 /data/misc_ce/0/checkin/checkin_id_token; "
            "rm -f $G/files/checkin_id_token $G/files/security_token $G/shared_prefs/Checkin.xml "
            "$G/shared_prefs/constellation_prefs.xml $G/databases/gservices.db "
            "$G/databases/gservices.db-journal $S/files/security_token "
            "$S/shared_prefs/CheckinService.xml; "
            "echo OK")
            .arg(*snap);
    const auto onDevice = [&](const QString &s) {
        if (backend == Backend::Bare)
            return sp.run({"docker", "exec", containerName, "sh", "-c", s}, {}, {});
        return sp.run(Spawner::sshArgv(sshHost, QStringLiteral("docker exec %1 sh -c '%2'")
                                                    .arg(containerName, s)),
                      {}, {});
    };
    const QString out = onDevice(script).out;
    if (out.contains(QLatin1String("NO_GMS"))) {
        res.detail = QStringLiteral("GMS is not installed on this device — nothing to restore onto");
        return res;
    }
    if (!out.contains(QLatin1String("OK"))) {
        res.detail = QStringLiteral("could not write the identity file (device not booted?)");
        return res;
    }
    res.ok = true;
    res.detail = QStringLiteral("identity written; GMS restarted to adopt it");
    return res;
}

// The data dir's shape, in one round trip on the host that will run docker. One word out:
// missing / empty / layered / flat.
//
// upper/ is the discriminator because it is what the container's init actually mounts — a dir has been
// used as an overlay diff if and only if that subdir is there. Checking for work/ as well would add
// nothing: init creates both together, and a dir with one and not the other is broken in a way this
// check cannot repair anyway.
//
// $D is deliberately UNQUOTED so a remote ~ expands — the remote defaults are literally
// "~/remora-data" and the remote shell is what resolves them (Resolver.cpp). The script contains
// no single quotes, so it survives being wrapped in them on the ssh path.
static QString dataShapeScan(const QString &dataDir) {
    return QStringLiteral(
               "D=%1; if [ ! -d $D ]; then echo missing; "
               "elif [ -d $D/upper ]; then echo layered; "
               "elif [ -n \"$(ls -A $D 2>/dev/null | head -1)\" ]; then echo flat; "
               "else echo empty; fi")
        .arg(dataDir);
}

DataDirShape probeDataDirShape(Spawner &sp, Backend backend, const QString &dataDir,
                               const QString &sshHost) {
    if (dataDir.isEmpty()) return DataDirShape::Missing;
    if (backend != Backend::Bare && sshHost.isEmpty()) return DataDirShape::Missing;
    const QString script = dataShapeScan(dataDir);
    // Same two transports as every other host-side probe: bare runs the shell locally, remote gets
    // there over ssh. Note this one does NOT go through `docker exec` — the container is exactly
    // what does not exist yet.
    const auto r = backend == Backend::Bare
                       ? sp.run({"sh", "-c", script}, {}, {})
                       : sp.run(Spawner::sshArgv(sshHost, script), {}, {});
    return parseDataDirShape(r.out);
}

ArchVariantProbe probeArchVariant(Spawner &sp, Backend backend, const QString &imageTag,
                                  const QString &sshHost) {
    if (imageTag.isEmpty() || isPlaceholderImageTag(imageTag)) return {};
    if (backend != Backend::Bare && sshHost.isEmpty()) return {};
    // The `arch_variant=` prefix is part of the FORMAT STRING rather than an echo around the
    // substitution, so the two answers stay tellable apart without nesting quotes inside quotes
    // through ssh's own single-quoting — and an inspect that fails outright (no such image) simply
    // prints no such line, which parses as "unknown" instead of shifting the cpuinfo line into the
    // variant slot.
    const QString script =
        QStringLiteral("docker image inspect --format "
                       "'arch_variant={{index .Config.Labels \"remora.arch_variant\"}}' '%1' "
                       "2>/dev/null; grep -m1 '^flags' /proc/cpuinfo 2>/dev/null")
            .arg(imageTag);
    // Same two transports as every other pre-container host probe: bare runs the shell locally,
    // remote gets there over ssh. Deliberately NOT `docker exec` — nothing has been created yet, and
    // the point is to refuse before anything is.
    const auto r = backend == Backend::Bare ? sp.run({"sh", "-c", script}, {}, {})
                                            : sp.run(Spawner::sshArgv(sshHost, script), {}, {});
    return parseArchVariantProbe(r.out);
}

// Nth line — NB not trimmed() first: an empty leading line is meaningful here (it is how
// kVenusScan says "no server"), and trimming would shift every later line up.
static std::optional<QString> lineOrNull(const QString &out, int idx) {
    const QString n = out.section(QLatin1Char('\n'), idx, idx).trimmed();
    if (n.isEmpty()) return std::nullopt;
    return n;
}

HostCapabilities probeCapabilities(Backend target, const QString &sshHost, Spawner &sp) {
    HostCapabilities c;
    c.modAshmem = parseLsmodHas(sp.run({"lsmod"}).out, "ashmem");
    c.inDockerGroup = parseIdGroups(sp.run({"id", "-nG"}).out, "docker");
    c.dockerDaemonOk =
        sp.run({"sh", "-c", "docker version --format '{{.Server.Version}}' >/dev/null 2>&1"}).rc == 0;
    c.adbPresent = sp.run({"sh", "-c", "command -v adb >/dev/null 2>&1"}).rc == 0;


    const auto probeSsh = [&](const QString &host) {
        const bool reach = sp.run(Spawner::sshArgv(host, "echo ok")).out.trimmed() ==
                           QLatin1String("ok");
        c.sshReachable = reach;
        if (reach)
            c.remoteDockerOk =
                sp.run(Spawner::sshArgv(host, "docker info >/dev/null 2>&1 && echo ok"))
                    .out.trimmed() == QLatin1String("ok");
    };

    if (target == Backend::Remote && !sshHost.isEmpty()) {
        probeSsh(sshHost);
        c.hostLan = parseHostLanScan(sp.run(Spawner::sshArgv(sshHost, kHostLanScan)).out);
        fillRenderNodes(c, parseRenderNodes(sp.run(Spawner::sshArgv(sshHost, kAllNodesScan)).out));
        c.videoNodes = sp.run(Spawner::sshArgv(sshHost, QStringLiteral("ls /dev/video* 2>/dev/null")))
                           .out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString venus = sp.run(Spawner::sshArgv(sshHost, kVenusScan)).out;
        c.venusServerPath = lineOrNull(venus, 0);
        c.venusLibDir = lineOrNull(venus, 1);
        // The REMOTE host's driver state, deliberately: its kernel is the one the container and
        // the render server live on (bd remora-81cp found exactly this on a remote deploy).
        c.nvidiaVersions = parseNvidiaVersionScan(
            sp.run(Spawner::sshArgv(sshHost, kNvidiaVersionScan)).out);
        c.python3Present =
            sp.run(Spawner::sshArgv(sshHost, QStringLiteral("command -v python3 >/dev/null 2>&1")))
                .rc == 0;
        const QString he =
            sp.run(Spawner::sshArgv(sshHost, hostEncodeScan(vendorNativeBinary(
                                                 QStringLiteral("remora-frame-encoder")))))
                .out;
        c.hostEncoderPath = lineOrNull(he, 0);
        c.ffmpegHevcVulkan = lineOrNull(he, 1).value_or(QString()) == QLatin1String("1");
        c.venusCapturePatched = lineOrNull(he, 2).value_or(QString()) == QLatin1String("1");
    } else if (target == Backend::Bare) {
        c.hostLan = parseHostLanScan(sp.run({"sh", "-c", kHostLanScan}).out);
        fillRenderNodes(c, parseRenderNodes(sp.run({"sh", "-c", kAllNodesScan}).out));
        c.videoNodes = sp.run({"sh", "-c", "ls /dev/video* 2>/dev/null"})
                           .out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString venus = sp.run({"sh", "-c", kVenusScan}).out;
        c.venusServerPath = lineOrNull(venus, 0);
        c.venusLibDir = lineOrNull(venus, 1);
        c.nvidiaVersions = parseNvidiaVersionScan(sp.run({"sh", "-c", kNvidiaVersionScan}).out);
        c.python3Present =
            sp.run({"sh", "-c", "command -v python3 >/dev/null 2>&1"}).rc == 0;
        const QString he =
            sp.run({"sh", "-c",
                    hostEncodeScan(vendorNativeBinary(QStringLiteral("remora-frame-encoder")))})
                .out;
        c.hostEncoderPath = lineOrNull(he, 0);
        c.ffmpegHevcVulkan = lineOrNull(he, 1).value_or(QString()) == QLatin1String("1");
        c.venusCapturePatched = lineOrNull(he, 2).value_or(QString()) == QLatin1String("1");
    }
    return c;
}

// The first non-empty line of a failed command's stderr, else of its stdout, else a stock phrase —
// docker's "Cannot connect to the Docker daemon at …" is one line and says everything.
static QString firstLine(const QString &err, const QString &out) {
    for (const QString &src : {err, out})
        for (const QString &l : src.split(QLatin1Char('\n')))
            if (!l.trimmed().isEmpty()) return l.trimmed();
    return QStringLiteral("docker ps failed with no output");
}

StatusReport probeStatus(Backend backend, Spawner &sp, const QString &sshHost,
                         const QString &containerName) {
    StatusReport r;
    r.backend = backend;
    r.host = probeCapabilities(backend, sshHost, sp);

    // Honour the CONFIGURED container name, like the remote rows below already do. This used to be
    // hardcoded "remora-bm", so every bare profile with its own container_name reported
    // running=false against a container that was up — and bare profiles with custom names stopped
    // being exotic the moment the backend became a first-class target (bd remora-4ei.61).
    const QString bareContainer =
        containerName.isEmpty() ? QStringLiteral("remora-bm") : containerName;
    // A failed `docker ps` is not an empty one. With the daemon down every consumer of this report
    // read running=false as "the container is stopped" — the Runners row, the library, this JSON —
    // while the container was up behind an unreachable socket. Carry the failure as its own fact.
    const ProcResult psLocal = sp.run({"docker", "ps", "--format", "{{.Names}}"});
    if (psLocal.rc != 0) r.bareDockerError = firstLine(psLocal.stderrTail, psLocal.out);
    const auto local = parseDockerPsNames(psLocal.rc == 0 ? psLocal.out : QString());
    r.bareRunning = local.contains(bareContainer);
    if (r.bareRunning)
        r.bareBoot = parseBootCompleted(
            sp.run({"docker", "exec", bareContainer, "getprop", "sys.boot_completed"}).out);
    if (r.bareRunning)
        // Same probe the remote rows run: the renderer SurfaceFlinger actually composited with. This
        // is the SwiftShader-fallback detector's raw datum (bd remora-4ei.79) — a host-mode deploy
        // that silently landed on software GL is indistinguishable from success without it.
        r.bareGles = parseSurfaceflingerGles(
            sp.run({"docker", "exec", bareContainer, "sh", "-c", "dumpsys SurfaceFlinger"}).out);

    // The remote rows need a configured ssh host. Unset means "not configured", which must read
    // as an absent/false status — NOT as a probe against somebody else's machine, which is what a
    // baked default used to do (bd remora-4ei.12).
    const QString guestContainer =
        containerName.isEmpty() ? QStringLiteral("remora-remote") : containerName;
    r.remoteSshOk = !sshHost.isEmpty() &&
                sp.run(Spawner::sshArgv(sshHost, "echo ok")).out.trimmed() == QLatin1String("ok");
    if (r.remoteSshOk) {
        const ProcResult psRemote =
            sp.run(Spawner::sshArgv(sshHost, "docker ps --format '{{.Names}}'"));
        if (psRemote.rc != 0) r.remoteDockerError = firstLine(psRemote.stderrTail, psRemote.out);
        const auto g = parseDockerPsNames(psRemote.rc == 0 ? psRemote.out : QString());
        r.remoteRunning = g.contains(guestContainer);
        if (r.remoteRunning) {
            r.remoteGpuNode = parseBakedGpuNode(
                sp.run(Spawner::sshArgv(
                           sshHost,
                           "docker inspect --format '{{range .Args}}{{println .}}{{end}}' " +
                               guestContainer))
                    .out);
            r.remoteBoot = parseBootCompleted(
                sp.run(Spawner::sshArgv(sshHost, "docker exec " + guestContainer + " getprop "
                                                "sys.boot_completed"))
                    .out);
            r.remoteGralloc = parseGralloc(
                sp.run(Spawner::sshArgv(sshHost, "docker exec " + guestContainer + " getprop "
                                                "ro.hardware.gralloc"))
                    .out);
            r.remoteGles = parseSurfaceflingerGles(
                sp.run(Spawner::sshArgv(sshHost, "docker exec " + guestContainer +
                                                    " sh -c 'dumpsys SurfaceFlinger'"))
                    .out);
        }
    }

    // Certification, for whichever container this backend actually has running. Only probed when
    // one is up: the scan needs a live dumpsys, and a down container must read as unknown.
    if (backend == Backend::Bare && r.bareRunning)
        r.playCert = probePlayCertification(sp, backend, bareContainer);
    else if (backend != Backend::Bare && r.remoteRunning)
        r.playCert = probePlayCertification(sp, backend, guestContainer, sshHost);

    // The adb FRONT DOOR (bd remora-yn4). Everything after a deploy reaches the device through
    // fwd's :5556, and the deploy asserts "adb forwarder up" exactly once — so when fwd is gone,
    // adb and the mirror report a dead device while adbd sits healthy on :5555, and no other row
    // here can say which of the two it is. Ports are read from /proc/net/tcp* (0x15B4 = 5556,
    // 0x15B3 = 5555, state 0A = LISTEN): the port is what consumers feel, pidof would call a
    // bound-nothing fwd healthy, and a LISTEN line cannot false-positive on fwd's established
    // relay connections, which sit in other states.
    const QString fwdScript = QStringLiteral(
        "echo fwd_l=$(grep -s ':15B4 ' /proc/net/tcp /proc/net/tcp6 | grep -c ' 0A ');"
        "echo adbd_l=$(grep -s ':15B3 ' /proc/net/tcp /proc/net/tcp6 | grep -c ' 0A ');"
        "echo svc=$(getprop init.svc.remora_fwd)");
    if (backend == Backend::Bare && r.bareRunning)
        r.forwarder = parseForwarderProbe(
            sp.run({"docker", "exec", bareContainer, "sh", "-c", fwdScript}).out);
    else if (backend != Backend::Bare && r.remoteRunning)
        r.forwarder = parseForwarderProbe(
            sp.run(Spawner::sshArgv(sshHost, "docker exec " + guestContainer + " sh -c '" +
                                                 fwdScript + QLatin1Char('\'')))
                .out);
    return r;
}

QString doctorReport(Spawner &sp, const RemoraConfig &cfg, Backend backend,
                     const QString &instance) {
    const RunContext ctx = makeContext(cfg, backend);
    QString out;
    QTextStream ts(&out);
    const auto first = [](const QString &s) { return s.section(QLatin1Char('\n'), 0, 0).trimmed(); };
    // One command on whichever host runs docker for this backend (local for bare, ssh else).
    const auto onDockerHost = [&](const QString &cmd) {
        return backend == Backend::Bare ? sp.run({"sh", "-c", cmd}, {}, {})
                                        : sp.run(Spawner::sshArgv(ctx.guest, cmd), {}, {});
    };

    ts << "# remora doctor — " << instance << " (backend " << backendToString(backend) << ")\n"
       << "generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";

    ts << "\n## resolved config\n"
       << "image_tag=" << ctx.rc.imageTag << "\n"
       << "container=" << ctx.rc.containerName << "\n"
       << "target=" << (ctx.rc.target.isEmpty() ? QStringLiteral("(macvlan auto-IP)")
                                                : ctx.rc.target) << "\n"
       << "display=" << ctx.rc.width << "x" << ctx.rc.height << " dpi " << ctx.rc.dpi
       << (ctx.rc.fps ? QStringLiteral(" fps %1").arg(*ctx.rc.fps) : QString()) << "\n"
       << "gpu_mode=" << gpuModeToString(ctx.rc.gpuMode)
       << (ctx.rc.gpuNode ? QStringLiteral(" node %1").arg(*ctx.rc.gpuNode) : QString()) << "\n";
    if (ctx.rc.sshHost) ts << "ssh_host=" << *ctx.rc.sshHost << "\n";

    ts << "\n## status probe\n"
       << statusToJson(probeStatus(backend, sp, ctx.rc.sshHost.value_or(QString()),
                                   ctx.rc.containerName));

    ts << "\n## readiness\n";
    const HostCapabilities caps =
        probeCapabilities(backend, ctx.rc.sshHost.value_or(QString()), sp);
    for (const PrereqResult &r :
         checkReadiness(backend, caps, ctx.rc.gpuMode == GpuMode::Host,
                        ctx.rc.networkMode == QLatin1String("macvlan"),
                        !ctx.rc.sharedInputs.isEmpty(), releaseNeedsAshmem(ctx.rc.androidVersion))) {
        ts << (r.ok ? "[  ok   ] " : "[MISSING] ") << r.label;
        if (!r.ok) ts << "  → " << r.remedy;
        ts << "\n";
    }

    ts << "\n## versions\n"
       << "adb: " << first(sp.run({"adb", "version"}, {}, {}).out) << "\n"
       << "mirror: built-in (remora mirror)\n"
       << "docker (local): " << first(sp.run({"docker", "--version"}, {}, {}).out) << "\n";
    if (backend != Backend::Bare)
        ts << "docker (" << ctx.guest << "): "
           << first(sp.run(Spawner::sshArgv(ctx.guest, QStringLiteral("docker --version")), {}, {}).out)
           << "\n";
    ts << "image " << ctx.rc.imageTag << ": "
       << first(onDockerHost(QStringLiteral("docker image inspect -f '{{.Id}} created "
                                            "{{.Created}}' %1 2>&1").arg(ctx.rc.imageTag)).out)
       << "\n"
       << "container " << ctx.rc.containerName << ": "
       << first(onDockerHost(QStringLiteral("docker inspect -f 'status {{.State.Status}} image "
                                            "{{.Image}} started {{.State.StartedAt}}' %1 2>&1")
                                 .arg(ctx.rc.containerName)).out)
       << "\n";

    QString err;
    const QString target = resolveAdbTarget(sp, ctx, &err);
    if (target.isEmpty()) {
        ts << "\n## device\nunreachable: " << err << "\n";
    } else {
        sp.run({"adb", "connect", target}, {}, {});
        ts << "\n## device (" << target << ")\n"
           << sp.run({"adb", "-s", target, "shell",
                      "echo fingerprint=$(getprop ro.build.fingerprint); "
                      "echo gralloc=$(getprop ro.hardware.gralloc); "
                      "echo egl=$(getprop ro.hardware.egl); "
                      "echo boot_completed=$(getprop sys.boot_completed); "
                      "echo uptime=$(uptime)"}, {}, {}).out;
        ts << "\n## recent logcat (-d -t 400)\n"
           << sp.run({"adb", "-s", target, "logcat", "-d", "-t", "400"}, {}, {}).out;
    }
    return out;
}

QString statusToJson(const StatusReport &r) {
    QJsonObject o;
    o["backend"] = backendToString(r.backend);

    QJsonObject bare;
    // `running` is omitted when docker itself could not be asked, so a consumer cannot read the
    // daemon being down as the container being stopped — the same rule play_cert follows below.
    if (r.bareDockerError) bare["docker_error"] = *r.bareDockerError;
    else bare["running"] = r.bareRunning;
    if (r.bareBoot) bare["boot_completed"] = *r.bareBoot;
    if (r.bareGles) bare["gles"] = *r.bareGles;
    o["bare"] = bare;

    QJsonObject remote;
    remote["ssh_ok"] = r.remoteSshOk;
    if (r.remoteDockerError) remote["docker_error"] = *r.remoteDockerError;
    else remote["running"] = r.remoteRunning;
    if (r.remoteBoot) remote["boot_completed"] = *r.remoteBoot;
    if (r.remoteGpuNode) remote["gpu_node"] = *r.remoteGpuNode;
    if (r.remoteGralloc) remote["gralloc"] = *r.remoteGralloc;
    if (r.remoteGles) remote["gles"] = *r.remoteGles;
    o["remote"] = remote;

    // Omitted entirely when unknown, so a consumer cannot read a missing probe as "uncertified".
    if (r.playCert.androidId || r.playCert.certified || r.playCert.pendingEvaluation) {
        QJsonObject cert;
        if (r.playCert.androidId) cert["gsf_android_id"] = *r.playCert.androidId;
        if (r.playCert.certified) cert["certified"] = *r.playCert.certified;
        // The third state (bd remora-4ei.90): no verdict is posted and the identity is too young
        // for absence to mean anything. "certified" is deliberately absent alongside it.
        if (r.playCert.pendingEvaluation) cert["pending_evaluation"] = true;
        o["play_cert"] = cert;
    }

    // Same omission rule as certification: absent when no running container was probed, so a
    // consumer cannot read "not probed" as "down" (bd remora-yn4).
    if (r.forwarder.fwdListening || r.forwarder.adbdListening) {
        QJsonObject door;
        if (r.forwarder.fwdListening) door["forwarder_listening"] = *r.forwarder.fwdListening;
        if (r.forwarder.adbdListening) door["adbd_listening"] = *r.forwarder.adbdListening;
        if (r.forwarder.service && !r.forwarder.service->isEmpty())
            door["service"] = *r.forwarder.service;
        o["adb_front_door"] = door;
    }

    QJsonObject host;
    // The probed host GPU identity, so status/check consumers can NAME the GPU instead of
    // inferring it from which vendor-specific fields happen to be set (bd remora-4ei.79).
    if (r.host.hostRenderNode) host["render_node"] = *r.host.hostRenderNode;
    if (r.host.hostRenderDriver) host["render_driver"] = *r.host.hostRenderDriver;
    if (r.host.hostNvidiaRenderNode) host["nvidia_node"] = *r.host.hostNvidiaRenderNode;
    host["in_docker_group"] = r.host.inDockerGroup;
    if (r.host.dockerDaemonOk) host["docker_daemon"] = *r.host.dockerDaemonOk;
    o["host"] = host;

    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

// One round trip that locates both ends of the mirror and names each one's log. Labelled output
// rather than a bare loop: either process can be absent, and "which of these two is missing" is
// exactly what the caller needs to know.
// The mirror is chosen by whether it still has a TUNNEL, not by being first.
//
// A husk — a client whose guest server died — keeps the encoder's video socket open, and being
// older it has the lower pid, so a plain `head -1` picked it every single time. Its compose
// counter is frozen, which reads as "nothing was composed", which makes judgeMirrorFlow answer
// not-starved for a mirror that is. That silently disabled the very self-heal this scan feeds
// (bd remora-u8f). Fixing it HERE rather than by reaping husks earlier is what makes it hold for
// every caller: `remora status` reaps nothing, and doConnect does not resolve an auto-IP target
// until well after the host-encoder step has already asked.
//
// Falling back to the first pid keeps the old behaviour in the two cases where the discriminator
// cannot speak: no `ss` on the host, and every candidate a husk — where the choice does not
// matter anyway, since none of them is composing.
static QString mirrorEndpointsScript(const ResolvedConfig &rc) {
    return QStringLiteral(
               "e=$(ps -C remora-frame-encoder -o pid=,args= 2>/dev/null | grep -F -- '%1' | "
               "awk '{print $1; exit}'); "
               "m=; first=; "
               "for p in $(pgrep -f -- '--external-video-socket=%2' 2>/dev/null); do "
               "[ -n \"$first\" ] || first=$p; "
               "if ss -tnHp 2>/dev/null | grep -q \"pid=$p,\"; then m=$p; break; fi; "
               "done; "
               "[ -n \"$m\" ] || m=$first; "
               "echo \"enc $e $(cat /proc/$e/cgroup 2>/dev/null | tr '\\n' ' ')\"; "
               "echo \"mir $m $(cat /proc/$m/cgroup 2>/dev/null | tr '\\n' ' ')\"")
        .arg(rc.hostEncodeFrameSocket, rc.hostEncodeVideoSocket);
}

// The tail is generous on purpose. The encoder prints once per 60 packets and the guest server
// once per 120 composed frames, so a lively mirror can push either counter several lines back
// within one sampling window — and a tail too short to contain the newest line reads as "no
// counter" and silently disables the check.
static QString readCounters(Spawner &sp, const QString &encLog, const QString &mirLog,
                            std::optional<qint64> &encoded, std::optional<qint64> &composed) {
    encoded.reset();
    composed.reset();
    if (!encLog.isEmpty()) {
        const ProcResult r =
            sp.run({QStringLiteral("sh"), QStringLiteral("-c"),
                    QStringLiteral("tail -n 400 %1 2>/dev/null").arg(encLog)}, {}, {});
        encoded = parseEncodedPackets(r.out);
    }
    if (!mirLog.isEmpty()) {
        const ProcResult r =
            sp.run({QStringLiteral("sh"), QStringLiteral("-c"),
                    QStringLiteral("tail -n 400 %1 2>/dev/null").arg(mirLog)}, {}, {});
        composed = parseComposedFrames(r.out);
    }
    return QString();
}

// Locate both ends of the mirror and name each one's log. Shared by the full comparison and by
// its pre-gate, so the two can never disagree about WHICH encoder and WHICH mirror they mean.
static bool findMirrorLogs(Spawner &sp, const ResolvedConfig &rc, QString &encLog, QString &mirLog) {
    encLog.clear();
    mirLog.clear();
    // Only meaningful when the mirror's video comes FROM the host encoder. On a device-encoded
    // profile there is no compose-versus-encode gap to measure — the mirror takes the device's own
    // stream — so the question this answers does not arise.
    if (!rc.hostEncode || !rc.hostEncodeDrivesMirror) return false;
    if (rc.hostEncodeFrameSocket.isEmpty() || rc.hostEncodeVideoSocket.isEmpty()) return false;
    const ProcResult ends = sp.run(
        {QStringLiteral("sh"), QStringLiteral("-c"), mirrorEndpointsScript(rc)}, {}, {});
    if (ends.rc != 0) return false;
    for (const QString &line : ends.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString t = line.trimmed();
        const bool isEnc = t.startsWith(QLatin1String("enc "));
        if (!isEnc && !t.startsWith(QLatin1String("mir "))) continue;
        const std::optional<QString> log = parseDetachedLogPath(t);
        if (!log) continue;  // running, but not out of one of our scopes — nothing to read
        (isEnc ? encLog : mirLog) = *log;
    }
    // Both ends are required, even by the pre-gate, which only reads one of them: an encoder with
    // no mirror attached has nothing to be starved OF, and its rate would be meaningless.
    return !encLog.isEmpty() && !mirLog.isEmpty();
}


// The gate's rule itself is mirrorGateIsClearlyHealthy in Parsers — a judgement, kept pure and
// testable, rather than inline here behind a Spawner where nothing could reach it (bd remora-yxy).
//
// Long enough that a healthy encoder prints at least once inside it — the counter advances per 60
// packets, so at 30 fps that is two seconds and four gives margin. Reading short biases the rate
// DOWN, which sends the caller on to the full comparison: the safe direction.
static constexpr int kGateMs = 4000;

std::optional<MirrorFlow> probeMirrorFlow(Spawner &sp, const ResolvedConfig &rc, int windowMs) {
    QString encLog, mirLog;
    if (!findMirrorLogs(sp, rc, encLog, mirLog)) return std::nullopt;

    // One window when everything is fine, a second only when the first looks bad. The counters
    // are printed in batches (120 composed, 60 encoded), so a single window's composed delta is
    // coarsely quantised and can over-read — which biases the ratio DOWNWARD, i.e. toward a false
    // alarm. Confirming keeps the healthy path at one window while making the accusation, whose
    // advice is "restart your container", something the tool has actually seen twice.
    //
    // The window opens with a GATE rather than a separate cheap probe beforehand. Starvation means
    // the publisher has stopped delivering and the encoder emits only its ~3.3 fps keepalive
    // (measured at 3.2 on the live fault), so an encoder already well above that a few seconds in
    // is carrying real frames and cannot be the starved case — return then and there. Folding it
    // into this window rather than running it first is the whole point: a gate with its own window
    // costs its seconds ON TOP whenever it fails to fire, which made an idle mirror SLOWER than no
    // gate at all (17.2s against 13.3s, measured). Sharing t0 means the fast path is the gate's
    // few seconds and every other path is the full window, never their sum.
    const auto sample = [&](std::optional<MirrorFlow> &into) -> bool {
        std::optional<qint64> enc0, comp0, encG, compG, enc1, comp1;
        readCounters(sp, encLog, mirLog, enc0, comp0);
        if (!enc0 || !comp0) return false;
        const int gateMs = qMin(kGateMs, windowMs);
        sp.waitMs(gateMs);
        readCounters(sp, encLog, mirLog, encG, compG);
        if (!encG || !compG) return false;
        if (mirrorGateIsClearlyHealthy(*compG - *comp0, *encG - *enc0, gateMs)) {
            // judgeMirrorFlow cannot call a window this short starved — which is exactly right,
            // because this one has already answered the only question the gate can answer.
            into = judgeMirrorFlow(*compG - *comp0, *encG - *enc0, gateMs);
            return true;
        }
        // Falling through costs the full window, and the batched counters make the gate's ratio
        // noisy — but the noise runs the safe way. The composed counter prints once per 120 frames
        // against the encoder's 60, so a short read OVER-states composed, which drags the ratio
        // DOWN and sends a healthy mirror here to be measured properly rather than a sick one
        // home unexamined. Paying a window we did not need is the cheaper mistake.
        if (windowMs > gateMs) {
            sp.waitMs(windowMs - gateMs);
            readCounters(sp, encLog, mirLog, enc1, comp1);
            if (!enc1 || !comp1) return false;
        } else {
            enc1 = encG;
            comp1 = compG;
        }
        into = judgeMirrorFlow(*comp1 - *comp0, *enc1 - *enc0, windowMs);
        return true;
    };
    std::optional<MirrorFlow> first;
    if (!sample(first) || !first) return std::nullopt;
    if (!first->starved) return first;
    std::optional<MirrorFlow> second;
    if (!sample(second) || !second) return first;
    // Report the confirming window's numbers — they are the ones the verdict rests on, and its
    // own `starved` is the answer: a second window that comes back healthy demotes the first to
    // the measurement artefact it probably was.
    return second;
}

}  // namespace remora
