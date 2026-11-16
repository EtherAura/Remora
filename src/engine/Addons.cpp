#include "engine/Addons.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/Builders.h"
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

// Remora's own files each add-on stages beside its payload. In the fingerprint, so a change to how
// the layer is made rebuilds it, as a change to the payload does.
QStringList ownFiles(const QString &feature, const QString &vendorDir) {
    const QString sb = vendorDir + QStringLiteral("/source-build");
    if (feature == QLatin1String("magisk_root"))
        return {sb + "/features/magisk/magisk.rc", sb + "/features/magisk/magisk-setup.sh"};
    if (feature == QLatin1String("arm_translate"))
        return {sb + "/stage-arm-layer.sh", sb + "/ndk-overlay.sh",
                sb + "/patch-arm-translation.py"};
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

// Every file under the payload, in a fixed order, and Remora's own files: the layer's inputs.
bool fingerprint(QCryptographicHash &h, const AddonPayload &p, const QString &vendorDir) {
    QStringList rel;
    QDirIterator it(p.dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = QDir(p.dir).relativeFilePath(it.next());
        if (!f.contains(QLatin1String("__pycache__"))) rel << f;
    }
    rel.sort();
    h.addData(p.feature.toUtf8());
    for (const QString &f : rel)
        if (!addFile(h, p.dir + QLatin1Char('/') + f, f)) return false;
    for (const QString &f : ownFiles(p.feature, vendorDir))
        if (!addFile(h, f, QFileInfo(f).fileName())) return false;
    return true;
}

QString quoteJoin(const QStringList &argv) {
    QStringList q;
    for (const QString &a : argv) q << shellQuote(a);
    return q.join(QLatin1Char(' '));
}

}  // namespace

QString userPayloadDir() {
    const QString env = qEnvironmentVariable("REMORA_PAYLOAD_DIR");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/remora/payloads");
}

AddonPaths defaultAddonPaths() {
    const QString vendor = QStringLiteral(REMORA_VENDOR_DIR);
    return {{userPayloadDir(), vendor + QStringLiteral("/source-build/features")},
            vendor,
            QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                QStringLiteral("/remora")};
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
    QVector<AddonPayload> payloads;
    for (const QString &f : pending) {
        AddonPayload p = locateAddonPayload(f, paths);
        if (p.dir.isEmpty() && p.fetchable && !paths.payloadRoots.isEmpty()) {
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

    // The context, assembled on this computer: Docker reads it from here, or from the tar stream
    // ssh carries to a remote host. Removed when this returns, whatever happened.
    QDir().mkpath(paths.stageParent);
    QTemporaryDir ctx(paths.stageParent + QStringLiteral("/addon-build-XXXXXX"));
    if (!ctx.isValid())
        return StepResult::fail(QStringLiteral("cannot create a build directory in %1")
                                    .arg(paths.stageParent));
    const QString c = ctx.path();
    const QString sb = paths.vendorDir + QStringLiteral("/source-build");
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
    if (!df.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        df.write(addonLayerDockerfile(pending, rc.imageTag, rc.androidVersion, baseId, rootId, recipe)
                     .toUtf8()) < 0)
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
