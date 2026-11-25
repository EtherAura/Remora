// The last `remora up` of a device (remora-4l8p): see LastUp in Engine.h.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include "engine/Engine.h"

namespace remora {

namespace {
// Four short lines. A longer file is not one this wrote, so it is not read at all.
constexpr qint64 kMaxRecordBytes = 256;
}  // namespace

const QStringList &upStepNames() {
    // In the order a run meets them: the refusal before anything runs, Remora's own Linux guest,
    // the two overlays, then the deploy chain both deployers share. A reconnect runs three of its
    // steps (phantom-kbd, shared-inputs, connect) and nothing else.
    static const QStringList names{
        QStringLiteral("conflict"),      QStringLiteral("guest"),
        QStringLiteral("play-overlay"),  QStringLiteral("addon-overlay"),
        QStringLiteral("preflight"),     QStringLiteral("ensure-image"),
        QStringLiteral("host-prereqs"),  QStringLiteral("provision"),
        QStringLiteral("spoof-arm"),     QStringLiteral("boot-wait"),
        QStringLiteral("netfix"),        QStringLiteral("ashmem-node"),
        QStringLiteral("camera-nodes"),  QStringLiteral("audio-nodes"),
        QStringLiteral("phantom-kbd"),   QStringLiteral("shared-inputs"),
        QStringLiteral("play-spoof"),    QStringLiteral("connect")};
    return names;
}

QString lastUpResultName(LastUp::Result r) {
    switch (r) {
        case LastUp::Result::Ok: return QStringLiteral("ok");
        case LastUp::Result::Failed: return QStringLiteral("failed");
        case LastUp::Result::Unfinished: break;
    }
    return QStringLiteral("unfinished");
}

QString lastUpPath(const QString &containerName) {
    // docker's own rule for a container name, so the name is one file in the directory.
    static const QRegularExpression dockerName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$"));
    if (!dockerName.match(containerName).hasMatch()) return {};
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/remora/last-up/") + containerName;
}

bool saveLastUp(const QString &path, const LastUp &up) {
    if (path.isEmpty() || !up.at.isValid() || up.exitCode < 0 || up.exitCode > 255) return false;
    // Only a failure names a step, and only a step on the list: anything else is stored as none.
    const QString step = up.result == LastUp::Result::Failed && upStepNames().contains(up.failedStep)
                             ? up.failedStep
                             : QString();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QStringLiteral("at=%1\nresult=%2\nstep=%3\nexit=%4\n")
                .arg(up.at.toUTC().toString(Qt::ISODate), lastUpResultName(up.result), step,
                     QString::number(up.exitCode))
                .toUtf8());
    return f.commit();
}

std::optional<LastUp> loadLastUp(const QString &path) {
    if (path.isEmpty()) return std::nullopt;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray raw = f.read(kMaxRecordBytes + 1);
    if (raw.size() > kMaxRecordBytes) return std::nullopt;
    // Exactly the four keys saveLastUp writes, each once.
    QHash<QString, QString> kv;
    for (const QString &line : QString::fromUtf8(raw).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || kv.contains(line.left(eq))) return std::nullopt;
        kv.insert(line.left(eq), line.mid(eq + 1));
    }
    static const QStringList keys{QStringLiteral("at"), QStringLiteral("result"),
                                  QStringLiteral("step"), QStringLiteral("exit")};
    if (kv.size() != keys.size()) return std::nullopt;
    for (const QString &k : keys)
        if (!kv.contains(k)) return std::nullopt;

    LastUp up;
    up.at = QDateTime::fromString(kv.value(QStringLiteral("at")), Qt::ISODate);
    if (!up.at.isValid()) return std::nullopt;
    const QString result = kv.value(QStringLiteral("result"));
    if (result == QLatin1String("ok")) up.result = LastUp::Result::Ok;
    else if (result == QLatin1String("failed")) up.result = LastUp::Result::Failed;
    else if (result == QLatin1String("unfinished")) up.result = LastUp::Result::Unfinished;
    else return std::nullopt;
    bool isInt = false;
    up.exitCode = kv.value(QStringLiteral("exit")).toInt(&isInt);
    if (!isInt || up.exitCode < 0 || up.exitCode > 255) return std::nullopt;
    up.failedStep = kv.value(QStringLiteral("step"));
    if (!up.failedStep.isEmpty() && !upStepNames().contains(up.failedStep)) return std::nullopt;
    // And the fields agree: a failure exited non-zero, and only a failure names a step.
    if (up.result == LastUp::Result::Failed ? up.exitCode == 0
                                            : up.exitCode != 0 || !up.failedStep.isEmpty())
        return std::nullopt;
    return up;
}

}  // namespace remora
