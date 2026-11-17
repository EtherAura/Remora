#include "engine/Addons.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/Builders.h"
#include "core/Features.h"
#include "core/ImageBuild.h"
#include "core/Resolver.h"
#include "engine/SourceBuild.h"  // shellQuote

#ifndef REMORA_VENDOR_DIR
#define REMORA_VENDOR_DIR "vendor"
#endif

namespace remora {

namespace {

// What each add-on needs from its payload, by the files that prove it is there. Binaries only:
// a public clone carries the scripts beside them (vendor/PAYLOADS.md), so a directory or a script
// existing proves nothing.
struct Spec {
    QString feature, name;
    QStringList keys;
    bool fetchable;
};
const QVector<Spec> &specs() {
    static const QVector<Spec> s{
        {QStringLiteral("magisk_root"), QStringLiteral("magisk"),
         {"magisk/magisk", "magisk/magiskboot", "magisk/magiskinit", "magisk/magiskpolicy",
          "magisk/busybox", "magisk/magisk.apk", "magisk/stub.apk", "magisk/util_functions.sh",
          "magisk/boot_patch.sh"},
         true},
        {QStringLiteral("arm_translate"), QStringLiteral("ndk_translation"),
         {"lib64/libnb.so", "lib64/libndk_translation.so", "guest/system/bin/arm64/linker64",
          "bin/ndk_translation_program_runner_binfmt_misc_arm64", "etc/cpuinfo.arm64.txt"},
         false},
        // The Play spoof stack and Shamiko share a payload directory, zygisk_pif, as they share
        // the source build's. Their scripts are in every checkout; these files are not.
        {QStringLiteral("play_spoof"), QStringLiteral("zygisk_pif"),
         {"rez-mod/lib64/libzygisk.so", "rez-mod/bin/zygiskd64", "rez-mod/bin/zygisk-ptrace64",
          "pifork/zygisk/x86_64.so"},
         false},
        {QStringLiteral("shamiko"), QStringLiteral("zygisk_pif"),
         {"shamiko/zygisk/x86_64.so", "shamiko/module.prop"},
         false},
        {QStringLiteral("widevine_l3"), QStringLiteral("widevine"),
         {"vendor/lib64/libwvaidl.so", "vendor/bin/hw/android.hardware.drm-service-lazy.widevine"},
         false},
    };
    return s;
}
const Spec *specFor(const QString &feature) {
    for (const Spec &s : specs())
        if (s.feature == feature) return &s;
    return nullptr;
}

// Remora's own files (or whole directories) each add-on stages beside its payload. In the
// fingerprint, so a change to how the layer is made rebuilds it, as a change to the payload does.
// The Play spoof stack's are the vendored zygisk_pif tree its payload is laid under, and nb_sigfix,
// which it carries when the image has ARM translation.
QStringList ownFiles(const QString &feature, const QString &vendorDir) {
    const QString sb = vendorDir + QStringLiteral("/source-build");
    if (feature == QLatin1String("magisk_root"))
        return {sb + "/features/magisk/magisk.rc", sb + "/features/magisk/magisk-setup.sh"};
    if (feature == QLatin1String("arm_translate"))
        return {sb + "/stage-arm-layer.sh", sb + "/ndk-overlay.sh",
                sb + "/patch-arm-translation.py"};
    if (feature == QLatin1String("play_spoof"))
        return {sb + "/stage-spoof-layer.sh", sb + "/spoof-overlay.sh",
                sb + "/features/zygisk_pif", sb + "/features/nb_sigfix"};
    if (feature == QLatin1String("shamiko"))
        return {sb + "/stage-spoof-layer.sh", sb + "/spoof-overlay.sh"};
    return {};
}

bool addFile(QCryptographicHash &h, const QString &path, const QString &label) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    h.addData(label.toUtf8());
    h.addData(QByteArrayView("\0", 1));
    QCryptographicHash fh(QCryptographicHash::Sha256);
    if (!fh.addData(&f)) return false;
    h.addData(fh.result());
    return true;
}

// A file, or every file under a directory in a fixed order, labelled by its path under `label`.
bool addPath(QCryptographicHash &h, const QString &path, const QString &label) {
    if (!QFileInfo(path).isDir()) return addFile(h, path, label);
    QStringList rel;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = QDir(path).relativeFilePath(it.next());
        if (!f.contains(QLatin1String("__pycache__"))) rel << f;
    }
    rel.sort();
    for (const QString &f : rel)
        if (!addFile(h, path + QLatin1Char('/') + f, label + QLatin1Char('/') + f)) return false;
    return true;
}

// Every file under the payload and Remora's own files: the layer's inputs.
bool fingerprint(QCryptographicHash &h, const AddonPayload &p, const QString &vendorDir) {
    h.addData(p.feature.toUtf8());
    if (!addPath(h, p.dir, QStringLiteral("payload"))) return false;
    for (const QString &f : ownFiles(p.feature, vendorDir))
        if (!addPath(h, f, QFileInfo(f).fileName())) return false;
    return true;
}

QString quoteJoin(const QStringList &argv) {
    QStringList q;
    for (const QString &a : argv) q << shellQuote(a);
    return q.join(QLatin1Char(' '));
}

// A scratch directory on the Docker host for AddonPaths::stageOnHost, made on first use and removed
// with everything in it when this goes out of scope. The path is held to the exact shape mktemp
// gives, so nothing else can ever reach the rm.
class HostScratch {
public:
    HostScratch(Spawner &sp, const QString &host, const LineSink &log)
        : sp_(sp), host_(host), log_(log) {}
    ~HostScratch() {
        if (!dir_.isEmpty()) sp_.run(ssh(QStringLiteral("rm -rf -- ") + shellQuote(dir_)), {}, {});
    }
    QStringList ssh(const QString &cmd) const {
        QStringList a = Spawner::sshArgv(host_, cmd, 10);
        a.insert(1, QStringLiteral("BatchMode=yes"));
        a.insert(1, QStringLiteral("-o"));
        return a;
    }
    // The directory, made now if it is not yet; empty with *err set when it could not be.
    QString dir(QString *err) {
        if (!dir_.isEmpty()) return dir_;
        const ProcResult made =
            sp_.run(ssh(QStringLiteral("umask 077; mktemp -d /tmp/remora-addons.XXXXXXXXXX")), {},
                    log_);
        const QString d = made.out.trimmed();
        if (made.rc != 0 ||
            !QRegularExpression(QStringLiteral("^/tmp/remora-addons\\.[A-Za-z0-9]{10}$"))
                 .match(d)
                 .hasMatch()) {
            *err = QStringLiteral("could not make a staging directory on %1").arg(host_);
            return QString();
        }
        dir_ = d;
        return dir_;
    }
    ProcResult run(const QStringList &argv) { return sp_.run(ssh(quoteJoin(argv)), {}, log_); }
    // scp, either way; `toHost` copies local → remote. -r so a directory travels whole.
    ProcResult copy(const QString &from, const QString &to, bool toHost) {
        return sp_.run(Spawner::scpArgv({toHost ? from : host_ + QLatin1Char(':') + from,
                                         toHost ? host_ + QLatin1Char(':') + to : to},
                                        {QStringLiteral("-q"), QStringLiteral("-r"),
                                         QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                                         QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10")}),
                       {}, log_);
    }

private:
    Spawner &sp_;
    QString host_;
    LineSink log_;
    QString dir_;
};

// Executable again what can execute. A file copied off a Windows filesystem arrives without its
// mode — NTFS has none to send — and the layer's COPY keeps whatever mode the context holds, so
// zygiskd64 or a guest linker would land in the image unrunnable. Content decides, not a path list:
// every ELF and every #! script under `dir` becomes 0755, which is the mode each one carries in a
// checkout that kept it.
QString restoreExecBits(const QString &dir) {
    return QStringLiteral("find %1 -type f | while IFS= read -r f; do "
                          "m=$(head -c 4 \"$f\" | od -An -tx1 | tr -d ' \\n'); "
                          "case \"$m\" in 7f454c46|2321*) chmod 755 \"$f\";; esac; done")
        .arg(shellQuote(dir));
}

}  // namespace

QString userPayloadDir() {
    const QString env = qEnvironmentVariable("REMORA_PAYLOAD_DIR");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/remora/payloads");
}

AddonPaths defaultAddonPaths() {
    const QString vendor = vendorDir();
#ifdef Q_OS_WIN
    const bool onHost = true;
#else
    const bool onHost = false;
#endif
    return {{userPayloadDir(), vendor + QStringLiteral("/source-build/features")},
            vendor,
            QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                QStringLiteral("/remora"),
            onHost};
}

AddonPayload locateAddonPayload(const QString &feature, const AddonPaths &paths) {
    AddonPayload p;
    p.feature = feature;
    const Spec *s = specFor(feature);
    if (!s) return p;
    p.name = s->name;
    p.fetchable = s->fetchable;
    for (const QString &root : paths.payloadRoots) {
        const QString dir = root + QLatin1Char('/') + s->name;
        bool all = true;
        for (const QString &k : s->keys)
            if (!QFileInfo::exists(dir + QLatin1Char('/') + k)) {
                all = false;
                break;
            }
        if (all) {
            p.dir = dir;
            break;
        }
    }
    return p;
}

StepResult ensureAddonOverlay(Spawner &sp, RemoraConfig &cfg, Backend backend,
                              const LineSink &log, const AddonPaths &paths) {
    // Cleared first, as the Play overlay is: a layer recorded by an earlier deploy belongs to the
    // image it was built over.
    cfg.image.addonOverlay.reset();
    for (const QString &a : cfg.image.addons)
        if (!addonFeatures().contains(a))
            return StepResult::fail(QStringLiteral("addons= names '%1', which is not an add-on — "
                                                   "the add-ons are %2")
                                        .arg(a, addonFeatures().join(QStringLiteral(", "))));
    const ResolvedConfig rc = resolve(cfg, backend);
    if (rc.addons.isEmpty()) return StepResult::good(QStringLiteral("no add-ons"));
    // A requirement (play_spoof needs magisk_root, shamiko both) is never added on the user's
    // behalf: root is a consent of its own (the Image page asks), so a list without it is refused.
    for (const QString &a : rc.addons)
        if (const Feature *f = findFeature(a))
            for (const QString &req : f->requires_)
                if (!rc.addons.contains(req))
                    return StepResult::fail(QStringLiteral("the %1 add-on needs %2 — add %2 to "
                                                           "addons= too, or take %1 out")
                                                .arg(a, req));
    const QStringList pending = rc.addonsPending();
    if (pending.isEmpty())
        return StepResult::good(QStringLiteral("%1 already carries %2 — nothing to add")
                                    .arg(rc.capabilityTag(), rc.addons.join(QStringLiteral(", "))));
    // Built where the container runs, like the Play layer: the image underneath is there, and the
    // result has to be.
    const QString host = backend == Backend::Remote ? rc.sshHost.value_or(QString()) : QString();
    if (backend == Backend::Remote && host.isEmpty())
        return StepResult::fail(QStringLiteral("add-ons need the remote Docker host — set ssh_host "
                                               "first"));
    const auto onHost = [&](const QStringList &argv) {
        return host.isEmpty() ? sp.run(argv, {}, {})
                              : sp.run(Spawner::sshArgv(host, quoteJoin(argv)), {}, {});
    };

    // The payloads, on this computer. Magisk is fetched when it is missing, from its recorded
    // upstream with the pinned hash; the others are proprietary with no recorded source, so only
    // the user can supply them.
    HostScratch scratch(sp, host, log);
    if (paths.stageOnHost && host.isEmpty())
        return StepResult::fail(QStringLiteral(
            "add-ons are staged on the Docker host, and this profile names none — set ssh_host"));
    QVector<AddonPayload> payloads;
    for (const QString &f : pending) {
        AddonPayload p = locateAddonPayload(f, paths);
        if (p.dir.isEmpty() && p.fetchable && !paths.payloadRoots.isEmpty() && paths.stageOnHost) {
            // Fetched where the shell, curl and unzip are, then copied back into the payload dir
            // here, so the fingerprint below reads the same bytes either way.
            QString err;
            const QString d = scratch.dir(&err);
            if (d.isEmpty()) return StepResult::fail(err);
            const QString root = paths.payloadRoots.first();
            if (log) log(QStringLiteral("stdout"),
                         QStringLiteral("fetching the %1 payload on %2 into %3").arg(p.name, host, root));
            ProcResult fr = scratch.copy(paths.vendorDir + QStringLiteral("/fetch-payloads.sh"),
                                         d + QStringLiteral("/fetch-payloads.sh"), true);
            if (fr.rc == 0)
                fr = sp.run(scratch.ssh(QStringLiteral("REMORA_PAYLOAD_DIR=%1 sh %2 %3")
                                            .arg(shellQuote(d + QStringLiteral("/fetched")),
                                                 shellQuote(d + QStringLiteral("/fetch-payloads.sh")),
                                                 shellQuote(p.name))),
                            {}, log);
            if (fr.rc == 0) {
                QDir().mkpath(root);
                fr = scratch.copy(d + QStringLiteral("/fetched/") + p.name, root, false);
            }
            if (fr.rc != 0)
                return StepResult::fail(QStringLiteral("could not fetch the %1 payload for %2 on %3")
                                            .arg(p.name, f, host),
                                        fr.stderrTail);
            p = locateAddonPayload(f, paths);
        } else if (p.dir.isEmpty() && p.fetchable && !paths.payloadRoots.isEmpty()) {
            if (log) log(QStringLiteral("stdout"),
                         QStringLiteral("fetching the %1 payload into %2").arg(p.name, paths.payloadRoots.first()));
            const ProcResult fr =
                sp.run({QStringLiteral("sh"), paths.vendorDir + QStringLiteral("/fetch-payloads.sh"), p.name},
                       {{QStringLiteral("REMORA_PAYLOAD_DIR"), paths.payloadRoots.first()}}, log);
            if (fr.rc != 0)
                return StepResult::fail(QStringLiteral("could not fetch the %1 payload for %2")
                                            .arg(p.name, f),
                                        fr.stderrTail);
            p = locateAddonPayload(f, paths);
        }
        if (p.dir.isEmpty())
            return StepResult::fail(
                QStringLiteral("%1 needs the %2 payload, which is proprietary and which Remora "
                               "cannot download — put it in %3/%2 (laid out as "
                               "vendor/source-build/features/%2; see vendor/PAYLOADS.md), or take "
                               "%1 out of addons=")
                    .arg(f, p.name,
                         paths.payloadRoots.isEmpty() ? userPayloadDir() : paths.payloadRoots.first()));
        payloads << p;
    }

    // The image underneath, by ID, on the Docker host. A Play layer names its own base in a label;
    // that is the registered image an update replaces, so it is the root the updater compares.
    const ProcResult b = onHost({QStringLiteral("docker"), QStringLiteral("image"),
                                 QStringLiteral("inspect"), QStringLiteral("-f"),
                                 QStringLiteral("{{.Id}} {{index .Config.Labels \"org.remora.play.base\"}}"),
                                 rc.imageTag});
    const QStringList ids = b.out.trimmed().split(QLatin1Char(' '));
    const QString baseId = ids.value(0);
    if (b.rc != 0 || !baseId.startsWith(QLatin1String("sha256:")))
        return StepResult::fail(QStringLiteral("%1, the image this profile deploys, is not on the "
                                               "Docker host").arg(rc.imageTag),
                                b.stderrTail);
    const QString rootId =
        ids.value(1).startsWith(QLatin1String("sha256:")) ? ids.value(1) : baseId;

    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(addonLayerDockerfile(pending, rc.imageTag, rc.androidVersion, baseId, rootId, QString())
                  .toUtf8());
    for (const AddonPayload &p : payloads)
        if (!fingerprint(h, p, paths.vendorDir))
            return StepResult::fail(QStringLiteral("could not read the %1 payload in %2")
                                        .arg(p.name, p.dir));
    const QString recipe = QString::fromLatin1(h.result().toHex());
    const QString tag = addonLayerTag(baseId, recipe);
    const QString what = pending.join(QStringLiteral(", "));

    const ProcResult have =
        onHost({QStringLiteral("docker"), QStringLiteral("image"), QStringLiteral("inspect"),
                QStringLiteral("-f"),
                QStringLiteral("{{index .Config.Labels \"org.remora.addons.recipe\"}}"), tag});
    if (have.rc == 0 && have.out.trimmed() == recipe) {
        cfg.image.addonOverlay = tag;
        return StepResult::good(
            QStringLiteral("reused %1 — %2 over %3").arg(tag, what, rc.imageTag));
    }

    const QString sb = paths.vendorDir + QStringLiteral("/source-build");
    const QString dockerfile =
        addonLayerDockerfile(pending, rc.imageTag, rc.androidVersion, baseId, rootId, recipe);
    if (paths.stageOnHost) {
        // The same staging as below, every command run on the Docker host against copies of the
        // same inputs: Remora's halves under <d>/sb at their paths in source-build, each payload at
        // <d>/payloads/<name>, the context at <d>/ctx.
        QString err;
        const QString d = scratch.dir(&err);
        if (d.isEmpty()) return StepResult::fail(err);
        const QString hsb = d + QStringLiteral("/sb"), hp = d + QStringLiteral("/payloads"),
                      hc = d + QStringLiteral("/ctx");
        QStringList own, made{hsb, hp, hc};
        for (const AddonPayload &p : payloads)
            for (const QString &f : ownFiles(p.feature, paths.vendorDir))
                if (!own.contains(f)) {
                    own << f;
                    made << hsb + QLatin1Char('/') + QFileInfo(QDir(sb).relativeFilePath(f)).path();
                }
        made.removeDuplicates();
        const auto failed = [&](const QString &what, const ProcResult &r) {
            return StepResult::fail(QStringLiteral("%1 on %2 failed").arg(what, host), r.stderrTail);
        };
        ProcResult r = scratch.run(QStringList{QStringLiteral("mkdir"), QStringLiteral("-p")} + made);
        if (r.rc != 0) return failed(QStringLiteral("preparing the add-on context"), r);
        for (const QString &f : own) {
            const QString rel = QDir(sb).relativeFilePath(f);
            // A directory lands INSIDE the target with scp -r, so it is copied to its parent.
            r = scratch.copy(f, QFileInfo(f).isDir() ? hsb + QLatin1Char('/') + QFileInfo(rel).path()
                                                     : hsb + QLatin1Char('/') + rel,
                             true);
            if (r.rc != 0) return failed(QStringLiteral("copying %1").arg(rel), r);
        }
        QStringList sent;
        for (const AddonPayload &p : payloads) {
            if (sent.contains(p.name)) continue;
            sent << p.name;
            r = scratch.copy(p.dir, hp, true);
            if (r.rc != 0) return failed(QStringLiteral("copying the %1 payload").arg(p.name), r);
        }
        r = sp.run(scratch.ssh(restoreExecBits(d)), {}, log);
        if (r.rc != 0) return failed(QStringLiteral("restoring file modes"), r);
        const auto at = [&](const AddonPayload &p) { return hp + QLatin1Char('/') + p.name; };
        bool spoofOnHost = false;
        for (const AddonPayload &p : payloads) {
            QVector<QStringList> steps;
            if (p.feature == QLatin1String("magisk_root")) {
                steps << QStringList{QStringLiteral("mkdir"), QStringLiteral("-p"),
                                     hc + QStringLiteral("/magisk")}
                      << QStringList{QStringLiteral("cp"), QStringLiteral("-a"),
                                     at(p) + QStringLiteral("/magisk"),
                                     hc + QStringLiteral("/magisk/magisk")}
                      << QStringList{QStringLiteral("cp"),
                                     hsb + QStringLiteral("/features/magisk/magisk.rc"),
                                     hsb + QStringLiteral("/features/magisk/magisk-setup.sh"),
                                     hc + QStringLiteral("/magisk/")};
            } else if (p.feature == QLatin1String("arm_translate")) {
                steps << QStringList{QStringLiteral("sh"), hsb + QStringLiteral("/stage-arm-layer.sh"),
                                     hc, at(p)};
            } else if (p.feature == QLatin1String("play_spoof") ||
                       p.feature == QLatin1String("shamiko")) {
                if (spoofOnHost) continue;
                spoofOnHost = true;
                QStringList argv{QStringLiteral("sh"), hsb + QStringLiteral("/stage-spoof-layer.sh"),
                                 hc, QStringLiteral("--payload"), at(p)};
                if (pending.contains(QLatin1String("play_spoof"))) {
                    argv << QStringLiteral("--spoof");
                    if (rc.addons.contains(QLatin1String("arm_translate")))
                        argv << QStringLiteral("--nb-sigfix")
                             << hsb + QStringLiteral("/features/nb_sigfix");
                }
                if (pending.contains(QLatin1String("shamiko"))) argv << QStringLiteral("--shamiko");
                steps << argv;
            } else if (p.feature == QLatin1String("widevine_l3")) {
                steps << QStringList{QStringLiteral("mkdir"), QStringLiteral("-p"),
                                     hc + QStringLiteral("/wv")}
                      << QStringList{QStringLiteral("cp"), QStringLiteral("-a"),
                                     at(p) + QStringLiteral("/vendor"),
                                     hc + QStringLiteral("/wv/vendor")};
            }
            for (const QStringList &s : steps) {
                r = scratch.run(s);
                if (r.rc != 0)
                    return StepResult::fail(
                        QStringLiteral("staging %1 for the add-on layer failed").arg(p.feature),
                        r.stderrTail);
            }
        }
        // The Dockerfile travels as a file: scp is the one copy that needs no shell on this side.
        QTemporaryDir local;
        QFile df(local.path() + QStringLiteral("/Dockerfile"));
        if (!local.isValid() || !df.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            df.write(dockerfile.toUtf8()) < 0)
            return StepResult::fail(QStringLiteral("cannot write a Dockerfile in %1").arg(local.path()));
        df.close();
        r = scratch.copy(df.fileName(), hc + QStringLiteral("/Dockerfile"), true);
        if (r.rc != 0) return failed(QStringLiteral("copying the Dockerfile"), r);
        const ProcResult built = scratch.run(buildImageBuildArgv(tag, hc));
        if (built.rc != 0)
            return StepResult::fail(QStringLiteral("could not build the add-on layer (%1) over %2")
                                        .arg(what, rc.imageTag),
                                    built.stderrTail);
        cfg.image.addonOverlay = tag;
        return StepResult::good(QStringLiteral("built %1 — %2 over %3").arg(tag, what, rc.imageTag));
    }

    // The context, assembled on this computer: Docker reads it from here, or from the tar stream
    // ssh carries to a remote host. Removed when this returns, whatever happened.
    QDir().mkpath(paths.stageParent);
    QTemporaryDir ctx(paths.stageParent + QStringLiteral("/addon-build-XXXXXX"));
    if (!ctx.isValid())
        return StepResult::fail(QStringLiteral("cannot create a build directory in %1")
                                    .arg(paths.stageParent));
    const QString c = ctx.path();
    bool spoofStaged = false;
    for (const AddonPayload &p : payloads) {
        ProcResult r{0, {}, {}};
        if (p.feature == QLatin1String("magisk_root")) {
            QDir().mkpath(c + QStringLiteral("/magisk"));
            r = sp.run({QStringLiteral("cp"), QStringLiteral("-a"), p.dir + QStringLiteral("/magisk"),
                        c + QStringLiteral("/magisk/magisk")}, {}, log);
            if (r.rc == 0)
                r = sp.run({QStringLiteral("cp"), sb + QStringLiteral("/features/magisk/magisk.rc"),
                            sb + QStringLiteral("/features/magisk/magisk-setup.sh"),
                            c + QStringLiteral("/magisk/")}, {}, log);
        } else if (p.feature == QLatin1String("arm_translate")) {
            // The script the manual route uses, so the patched bridge is the same either way.
            r = sp.run({QStringLiteral("sh"), sb + QStringLiteral("/stage-arm-layer.sh"), c, p.dir},
                       {}, log);
        } else if (p.feature == QLatin1String("play_spoof") ||
                   p.feature == QLatin1String("shamiko")) {
            // One call stages both, so it runs for whichever comes first and is skipped for the
            // other. nb_sigfix rides the stack only where there is a bridge for it to hook.
            if (spoofStaged) continue;
            spoofStaged = true;
            QStringList argv{QStringLiteral("sh"), sb + QStringLiteral("/stage-spoof-layer.sh"), c};
            for (const AddonPayload &q : payloads)
                if (q.name == p.name && !argv.contains(q.dir)) argv << QStringLiteral("--payload") << q.dir;
            if (pending.contains(QLatin1String("play_spoof"))) {
                argv << QStringLiteral("--spoof");
                if (rc.addons.contains(QLatin1String("arm_translate")))
                    argv << QStringLiteral("--nb-sigfix") << sb + QStringLiteral("/features/nb_sigfix");
            }
            if (pending.contains(QLatin1String("shamiko"))) argv << QStringLiteral("--shamiko");
            r = sp.run(argv, {}, log);
        } else if (p.feature == QLatin1String("widevine_l3")) {
            QDir().mkpath(c + QStringLiteral("/wv"));
            r = sp.run({QStringLiteral("cp"), QStringLiteral("-a"), p.dir + QStringLiteral("/vendor"),
                        c + QStringLiteral("/wv/vendor")}, {}, log);
        }
        if (r.rc != 0)
            return StepResult::fail(QStringLiteral("staging %1 for the add-on layer failed").arg(p.feature),
                                    r.stderrTail);
    }
    QFile df(c + QStringLiteral("/Dockerfile"));
    if (!df.open(QIODevice::WriteOnly | QIODevice::Truncate) || df.write(dockerfile.toUtf8()) < 0)
        return StepResult::fail(QStringLiteral("cannot write %1").arg(df.fileName()));
    df.close();

    const ProcResult built =
        host.isEmpty()
            ? sp.run(buildImageBuildArgv(tag, c), {}, log)
            : sp.run({QStringLiteral("sh"), QStringLiteral("-c"),
                      QStringLiteral("tar -C %1 -cf - . | %2")
                          .arg(shellQuote(c),
                               quoteJoin(Spawner::sshArgv(
                                   host, QStringLiteral("docker build -t %1 -").arg(shellQuote(tag)))))},
                     {}, log);
    if (built.rc != 0)
        return StepResult::fail(QStringLiteral("could not build the add-on layer (%1) over %2")
                                    .arg(what, rc.imageTag),
                                built.stderrTail);
    cfg.image.addonOverlay = tag;
    return StepResult::good(QStringLiteral("built %1 — %2 over %3").arg(tag, what, rc.imageTag));
}

}  // namespace remora
