#include "engine/PlayInstall.h"
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
QString playNotice() {
    const auto m = manifest();
    return QStringLiteral(
        "Download %1 — %2 directly from the provider on the selected Docker host. "
        "This creates a separate local image; the original image, running instances and Android "
        "data are retained. Use the new image with fresh Android data.\n\n"
        "Google APKs remain proprietary; the installer licence does not license them. Remora "
        "is not Google licensed or certified. Service access, Play Integrity, DRM and app "
        "compatibility are not guaranteed. No attestation spoofing is added. "
        "Do not publish or mirror the resulting image.\n\n"
        "Third-party terms: %3\nGoogle terms: %4\nAcceptance version: %5")
        .arg(m.value("provider").toString(), m.value("release").toString(),
             m.value("terms_url").toString(), m.value("google_terms_url").toString(),
             m.value("terms_version").toString());
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
        return {2, {}, playNotice() + "\nUse --accept-terms " + playTermsVersion()};
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
    auto result = sp.run({"scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                          "--", script, data, host + ":" + dir + "/"}, {}, log);
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
}
