#include "engine/PublicImages.h"
#include "engine/SourceBuild.h"
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace remora {

QJsonObject publicImagesResult(const QString &output) {
    for (const auto &line : output.split('\n'))
        if (line.startsWith("REMORA_IMAGES_RESULT="))
            return QJsonDocument::fromJson(line.mid(21).toUtf8()).object();
    return {};
}

ProcResult runPublicImages(Spawner &sp, const QStringList &args, const QString &host,
                           const LineSink &log) {
    const QString helper = vendorScript("release", "catalog.py");
    const QString key = vendorScript("release", "image-signing-key.pem");
    // No trust anchor, no listing: refusing is the only answer that cannot be mistaken for an
    // empty catalog.
    if (!QFile::exists(helper) || !QFile::exists(key))
        return {2, {}, "Public image helper/trust anchor is missing"};
    if (host.isEmpty())
        return sp.run(QStringList{"python3", helper} + args + QStringList{"--key", key}, {}, log);
    // The host lands in an ssh/scp argv, so it is held to a plain user@host shape: nothing that
    // could read as an option or a second word.
    if (host.startsWith('-') || !QRegularExpression("^[A-Za-z0-9_.@%:\\[\\]-]+$").match(host).hasMatch())
        return {2, {}, "Invalid SSH host"};
    const auto ssh = [&host](const QString &command) {
        auto a = Spawner::sshArgv(host, command, 10);
        a.insert(1, "BatchMode=yes");
        a.insert(1, "-o");
        return a;
    };
    const auto made = sp.run(ssh("umask 077; mktemp -d /tmp/remora-images.XXXXXXXXXX"));
    if (made.rc) return made;
    const auto dir = made.out.trimmed();
    // Only a directory mktemp could have produced is ever removed below.
    if (!QRegularExpression("^/tmp/remora-images\\.[A-Za-z0-9]{10}$").match(dir).hasMatch())
        return {2, {}, "Unexpected remote staging directory"};
    auto result = sp.run({"scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "--",
                          helper, key, host + ":" + dir + "/"},
                         {}, log);
    if (!result.rc) {
        QStringList command{"python3", dir + "/catalog.py"};
        command += args;
        command << "--key" << dir + "/image-signing-key.pem";
        QStringList quoted;
        for (const auto &a : command) quoted << shellQuote(a);
        result = sp.run(ssh(quoted.join(' ')), {}, log);
    }
    const auto clean = sp.run(ssh("rm -rf -- " + shellQuote(dir)));
    if (clean.rc && log) log("stderr", "Could not remove owned staging directory: " + dir);
    return result;
}

}  // namespace remora
