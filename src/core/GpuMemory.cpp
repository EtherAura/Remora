#include "GpuMemory.h"

#include <QStringList>

namespace remora::gpumem {

namespace {
constexpr qint64 kEncoderMiB = 640;
constexpr int kStreamBuffers = 4;  // KWin's screencast pool, BGRA
}  // namespace

std::optional<VramUsage> parseNvidiaSmi(const QString &out) {
    // One line per GPU queried; the caller asks for one.
    const QStringList fields = out.trimmed().section(QLatin1Char('\n'), 0, 0).split(QLatin1Char(','));
    if (fields.size() != 2) return std::nullopt;
    bool okUsed = false, okTotal = false;
    VramUsage u{fields[0].trimmed().toLongLong(&okUsed), fields[1].trimmed().toLongLong(&okTotal)};
    if (!okUsed || !okTotal || u.totalMiB <= 0 || u.usedMiB < 0 || u.usedMiB > u.totalMiB)
        return std::nullopt;
    return u;
}

std::optional<VramUsage> fromAmdgpuBytes(const QString &used, const QString &total) {
    bool okUsed = false, okTotal = false;
    const qint64 u = used.trimmed().toLongLong(&okUsed), t = total.trimmed().toLongLong(&okTotal);
    if (!okUsed || !okTotal || t <= 0 || u < 0 || u > t) return std::nullopt;
    return VramUsage{u >> 20, t >> 20};
}

QString nvidiaBusId(const QString &pciAddress) {
    // sysfs pads the PCI domain to four digits, nvidia-smi to eight.
    const QString domain = pciAddress.section(QLatin1Char(':'), 0, 0);
    const QString rest = pciAddress.section(QLatin1Char(':'), 1);
    if (domain.isEmpty() || rest.isEmpty()) return pciAddress;
    return domain.rightJustified(8, QLatin1Char('0')) + QLatin1Char(':') + rest;
}

qint64 sessionNeedMiB(QSize pixels, qint64 reserveMiB) {
    const qint64 frameBytes = qint64(pixels.width()) * pixels.height() * 4;
    const qint64 streamMiB = (frameBytes * kStreamBuffers + (1 << 20) - 1) >> 20;
    return kEncoderMiB + streamMiB + reserveMiB;
}

QString vramRefusal(const VramUsage &usage, qint64 needMiB) {
    if (usage.freeMiB() >= needMiB) return {};
    return QStringLiteral("the GPU that drives this computer's screens has %1 MiB of its %2 MiB free, "
                          "and a session needs about %3 — starting one could leave the screens "
                          "themselves without memory. Close something that uses the GPU and try "
                          "again")
        .arg(usage.freeMiB())
        .arg(usage.totalMiB)
        .arg(needMiB);
}

}  // namespace remora::gpumem
