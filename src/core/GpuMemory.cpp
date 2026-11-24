#include "GpuMemory.h"

#include <QStringList>

namespace remora::gpumem {

namespace {
// 220 + 64 a megapixel stays above every measurement in GpuMemory.h.
constexpr qint64 kEncoderMiB = 220, kEncoderMiBPerMegapixel = 64;
// KWin's screencast pool (four) and the app's own buffers (three), BGRA at the output's size.
constexpr int kOutputFrames = 4 + 3;
constexpr int kScanoutBuffers = 3;
constexpr qint64 kMinReserveMiB = 256;

qint64 ceilMiB(qint64 bytes) {
    return (bytes + (1 << 20) - 1) >> 20;
}
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
    const qint64 px = qint64(pixels.width()) * pixels.height();
    const qint64 encoderMiB = kEncoderMiB + (px * kEncoderMiBPerMegapixel + 999999) / 1000000;
    return encoderMiB + ceilMiB(px * 4 * kOutputFrames) + reserveMiB;
}

qint64 monitorReserveMiB(const QList<outputs::OutputState> &outputs) {
    qint64 bytes = 0;
    for (const outputs::OutputState &o : outputs) {
        if (!o.enabled || o.pixelSize.isEmpty()) continue;
        bytes += qint64(o.pixelSize.width()) * o.pixelSize.height() * (o.hdr ? 8 : 4)
                 * kScanoutBuffers;
    }
    return qMax(kMinReserveMiB, 2 * ceilMiB(bytes));
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
