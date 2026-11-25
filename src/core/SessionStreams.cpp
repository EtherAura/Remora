#include "SessionStreams.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace remora::sessionaudio {

namespace {

// pactl writes ids as numbers in some places and as strings in others (a sink input's `client`).
qint64 number(const QJsonValue &v) { return v.toVariant().toLongLong(); }

}  // namespace

QHash<qint64, int> clientProcesses(const QByteArray &clientsJson) {
    QHash<qint64, int> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(clientsJson).array()) {
        const QJsonObject client = v.toObject();
        const QJsonObject props = client.value(QStringLiteral("properties")).toObject();
        int pid = int(number(props.value(QStringLiteral("pipewire.sec.pid"))));
        if (pid <= 0) pid = int(number(props.value(QStringLiteral("application.process.id"))));
        if (pid > 0 && client.contains(QStringLiteral("index")))
            out.insert(number(client.value(QStringLiteral("index"))), pid);
    }
    return out;
}

int streamProcess(const QJsonObject &sinkInput, const QHash<qint64, int> &clients) {
    const QJsonObject props = sinkInput.value(QStringLiteral("properties")).toObject();
    const int pid = int(number(props.value(QStringLiteral("application.process.id"))));
    if (pid > 0) return pid;
    const QJsonValue client = sinkInput.value(QStringLiteral("client"));
    if (client.isUndefined() || client.isNull()) return 0;
    return clients.value(number(client), 0);
}

}  // namespace remora::sessionaudio
