#include "engine/PlayInstall.h"
#include "core/Resolver.h"
#include "engine/SourceBuild.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace remora {
static QJsonObject manifest() {
    QFile f(vendorScript("play", "package.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
QString playTermsVersion() { return manifest().value("terms_version").toString(); }
QString playNotice(const QString &what) {
    const auto m = manifest();
    return QStringLiteral(
        "Package: %1 — %2, downloaded directly from the provider.\n\n%6\n\n"
        "Google APKs remain proprietary; the installer licence does not license them. Remora "
        "is not Google licensed or certified. Service access, Play Integrity, DRM and app "
        "compatibility are not guaranteed. No attestation spoofing is added. "
        "Do not publish or mirror the resulting image.\n\n"
        "Third-party terms: %3\nGoogle terms: %4\nAcceptance version: %5")
        .arg(m.value("provider").toString(), m.value("release").toString(),
             m.value("terms_url").toString(), m.value("google_terms_url").toString(),
             m.value("terms_version").toString(), what);
}
ProcResult runPlayInstall(Spawner &sp, const QStringList &arguments, const QString &host,
                          const LineSink &log) {
    if (!host.isEmpty() && (host.startsWith('-') ||
        !QRegularExpression("^[A-Za-z0-9_.@%:\\[\\]-]+$").match(host).hasMatch()))
        return {2, {}, "Invalid SSH host"};
    const QString script = vendorScript("play", "install.py");
    const QString data = vendorScript("play", "package.json");
    if (!QFile::exists(script) || playTermsVersion().isEmpty())
        return {2, {}, "Installed Play installer/manifest is missing"};
    // Validate before SSH/SCP as well as in Python: no network before explicit consent.
    const int accept = arguments.indexOf("--accept-terms");
    if (!arguments.contains("--show-terms") && !arguments.contains("--rollback") &&
        (accept < 0 || arguments.value(accept + 1) != playTermsVersion()))
        return {2, {}, playNotice("This creates a separate local image on the Docker host; the "
                                  "original image, running instances and Android data are "
                                  "retained. Use the new image with fresh Android data.") +
                           "\nUse --accept-terms " + playTermsVersion()};
#ifdef Q_OS_WIN
    // No python3 or docker here (bd remora-7oih.3). The terms are printed by Remora itself —
    // nothing runs and nothing is downloaded to read them — and an install goes to a Linux docker
    // host or nowhere.
    if (arguments.contains("--show-terms"))
        return {0, playNotice(QStringLiteral("This downloads directly from the provider on the "
                                             "Docker host and creates a separate local image "
                                             "there; the original image and Android data are "
                                             "retained.")) + '\n',
                {}};
    if (host.isEmpty())
        return {2, {}, QStringLiteral("Google Play is installed on a Linux docker host, and on "
                                      "Windows there is no local one — pass --host user@host, or "
                                      "set ssh_host on the profile")};
#endif
    if (host.isEmpty() || arguments.contains("--show-terms"))
        return sp.run(QStringList{"python3", script, "--manifest", data} + arguments, {}, log);
    const auto ssh = [&host](const QString &command) {
        auto argv = Spawner::sshArgv(host, command, 10);
        argv.insert(1, "BatchMode=yes");
        argv.insert(1, "-o");
        return argv;
    };
    const auto made = sp.run(ssh(
        "umask 077; mktemp -d /tmp/remora-play.XXXXXXXXXX"));
    if (made.rc) return made;
    const QString dir = made.out.trimmed();
    if (!QRegularExpression("^/tmp/remora-play\\.[A-Za-z0-9]{10}$").match(dir).hasMatch())
        return {2, {}, "Remote mktemp returned an unexpected path"};
    auto result = sp.run(Spawner::scpArgv({script, data, host + ":" + dir + "/"},
                                          {"-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"}),
                         {}, log);
    if (!result.rc) {
        QStringList cmd{"python3", dir + "/install.py", "--manifest", dir + "/package.json"};
        cmd += arguments;
        QStringList quoted;
        for (const auto &a : cmd) quoted << shellQuote(a);
        result = sp.run(ssh(quoted.join(' ')), {}, log);
    }
    const auto cleaned = sp.run(ssh("rm -rf -- " + shellQuote(dir)));
    if (cleaned.rc && log) log("stderr", "Could not remove owned remote staging directory: " + dir);
    return result;
}
QJsonObject playInstallResult(const QString &output) {
    for (const auto &line : output.split('\n'))
        if (line.startsWith("REMORA_PLAY_RESULT="))
            return QJsonDocument::fromJson(line.mid(19).toUtf8()).object();
    return {};
}
StepResult ensurePlayOverlay(Spawner &sp, RemoraConfig &cfg, Backend backend, const LineSink &log) {
    // Cleared first: an overlay recorded by an earlier deploy belongs to the base it was built on,
    // and resolving through it would deploy that base however far the profile has moved on.
    cfg.image.playOverlay.reset();
    const ResolvedConfig rc = resolve(cfg, backend);
    if (!rc.googlePlay) return StepResult::good("Google Play off");
    if (rc.playIncluded)
        return StepResult::good(rc.imageTag + " already includes Google apps — nothing to add");
    const QString terms = playTermsVersion();
    if (terms.isEmpty()) return StepResult::fail("Installed Play installer/manifest is missing");
    if (rc.playTerms != terms)
        return StepResult::fail(rc.playTerms.isEmpty()
            ? QStringLiteral("Google Play is on but its provider terms were never accepted — turn "
                             "it on from the Image page, or read `remora play-install --show-terms` "
                             "and set play_terms=%1").arg(terms)
            : QStringLiteral("the Google Play provider terms changed (accepted %1, now %2) — turn "
                             "Google Play off and on again on the Image page to read and accept "
                             "them").arg(rc.playTerms, terms));
    // Built where the container runs: the payload is downloaded and the layer created on that
    // Docker host, the same rule `remora play-install --host` follows.
    const QString host = backend == Backend::Remote ? rc.sshHost.value_or(QString()) : QString();
    if (backend == Backend::Remote && host.isEmpty())
        return StepResult::fail("Google Play needs the remote Docker host — set ssh_host first");
    const ProcResult r =
        runPlayInstall(sp, {"--base", rc.imageTag, "--accept-terms", rc.playTerms}, host, log);
    const QJsonObject result = playInstallResult(r.out);
    const QString tag = result.value("tag").toString();
    if (r.rc != 0 || tag.isEmpty())
        return StepResult::fail("could not add Google Play to " + rc.imageTag,
                                r.stderrTail.isEmpty() ? r.out.right(2000) : r.stderrTail);
    cfg.image.playOverlay = tag;
    return StepResult::good(QStringLiteral("%1 %2 — Google Play over %3")
                                .arg(result.value("reused").toBool() ? QStringLiteral("reused")
                                                                     : QStringLiteral("built"),
                                     tag, rc.imageTag));
}
}
