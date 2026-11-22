#include "Gpu.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace remora::hostagent {

QString renderNodeForConnector(const QString &connector) {
    // /sys/class/drm/card<N>-<connector> is the connector; card<N>/device/drm/ lists both the
    // card and its render node for that PCI device.
    const QDir drm(QStringLiteral("/sys/class/drm"));
    for (const QString &entry : drm.entryList({QStringLiteral("card*-") + connector}, QDir::AllEntries)) {
        const QString card = entry.section(QLatin1Char('-'), 0, 0);
        const QDir dev(drm.filePath(card + QStringLiteral("/device/drm")));
        const QStringList render = dev.entryList({QStringLiteral("renderD*")}, QDir::AllEntries);
        if (!render.isEmpty()) return QStringLiteral("/dev/dri/") + render.first();
    }
    return {};
}

std::optional<gpumem::VramUsage> vramUsage(const QString &renderNode) {
    const QString device =
        QFileInfo(QStringLiteral("/sys/class/drm/") + QFileInfo(renderNode).fileName() +
                  QStringLiteral("/device"))
            .canonicalFilePath();
    if (device.isEmpty()) return std::nullopt;
    const QString driver = QFileInfo(QFileInfo(device + QStringLiteral("/driver")).canonicalFilePath()).fileName();
    if (driver == QLatin1String("nvidia")) {
        QProcess p;
        p.start(QStringLiteral("nvidia-smi"),
                {QStringLiteral("--query-gpu=memory.used,memory.total"),
                 QStringLiteral("--format=csv,noheader,nounits"), QStringLiteral("-i"),
                 gpumem::nvidiaBusId(QFileInfo(device).fileName())});
        if (!p.waitForFinished(5000) || p.exitCode() != 0) return std::nullopt;
        return gpumem::parseNvidiaSmi(QString::fromUtf8(p.readAllStandardOutput()));
    }
    if (driver == QLatin1String("amdgpu")) {
        auto read = [&](const char *name) {
            QFile f(device + QLatin1Char('/') + QLatin1String(name));
            return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()) : QString();
        };
        return gpumem::fromAmdgpuBytes(read("mem_info_vram_used"), read("mem_info_vram_total"));
    }
    return std::nullopt;
}

}  // namespace remora::hostagent
