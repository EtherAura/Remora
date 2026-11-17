#include "SplashFormats.h"

namespace remora::mirror {

PartStep bootAnimPartStep(const BootAnimPart &part, int playthroughs, bool isLastPart,
                          bool finishing) {
    const bool exhausted = part.count == 0 ? finishing : playthroughs >= part.count;
    if (!exhausted) return PartStep::Repeat;
    if (!isLastPart) return PartStep::Advance;
    // The tail part while still booting is usually the loop, so hold it; once finishing, this was
    // the ending and replaying it would look like a loop that never ends.
    return finishing ? PartStep::Finished : PartStep::Repeat;
}

BootAnimDesc parseBootAnimDesc(const QString &descTxt) {
    BootAnimDesc d;
    for (const QString &raw : descTxt.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        const QStringList tok = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (d.width == 0) {
            // Header: W H FPS.
            if (tok.size() >= 3) {
                d.width = tok[0].toInt();
                d.height = tok[1].toInt();
                d.fps = tok[2].toDouble();
            }
            continue;
        }
        if (tok.size() >= 4 && tok[0].size() == 1 && tok[0][0].isLetter()) {
            BootAnimPart p;
            p.type = tok[0][0];
            p.count = tok[1].toInt();
            p.pause = tok[2].toInt();
            p.dir = tok[3];
            d.parts << p;
        }
    }
    return d;
}

SplashStatus parseSplashStatus(const QString &fileContents) {
    SplashStatus s;
    for (const QString &raw : fileContents.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line == QLatin1String("REMORA_BOOTING")) {
            s.booting = true;
            s.attach = false;  // a marker from before this boot must not fire after it
            s.abort = false;
        } else if (line == QLatin1String("REMORA_ATTACH")) {
            s.attach = true;
        } else if (line == QLatin1String("REMORA_ABORT")) {
            s.abort = true;
        } else {
            s.text = line;
        }
    }
    return s;
}

QStringList bootAnimSearchPaths() {
    return {QStringLiteral("/product/media/bootanimation.zip"),
            QStringLiteral("/oem/media/bootanimation.zip"),
            QStringLiteral("/system/media/bootanimation.zip"),
            QStringLiteral("/system/product/media/bootanimation.zip"),
            QStringLiteral("/data/local/bootanimation.zip")};
}

static quint32 le(QByteArrayView b, qint64 at, int width) {
    quint32 v = 0;
    for (int i = width - 1; i >= 0; --i) v = (v << 8) | quint8(b[at + i]);
    return v;
}

ZipIndex indexZip(QByteArrayView zip) {
    ZipIndex out;
    const auto fail = [&out](const QString &why) {
        out.entries.clear();
        out.error = why;
        return out;
    };
    const qint64 n = zip.size();
    // The end-of-central-directory record is the last 22 bytes, unless a comment (up to 64 KiB)
    // follows it — so scan back over at most that far for its signature.
    qint64 eocd = -1;
    for (qint64 at = n - 22; at >= 0 && at >= n - 22 - 0xFFFF; --at)
        if (le(zip, at, 4) == 0x06054b50) {
            eocd = at;
            break;
        }
    if (eocd < 0) return fail(QStringLiteral("no end-of-central-directory record"));
    const quint32 count = le(zip, eocd + 10, 2);
    const quint32 cdSize = le(zip, eocd + 12, 4);
    const quint32 cdOffset = le(zip, eocd + 16, 4);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF)
        return fail(QStringLiteral("ZIP64 archive"));
    if (qint64(cdOffset) + cdSize > eocd) return fail(QStringLiteral("central directory out of range"));
    qint64 at = cdOffset;
    for (quint32 i = 0; i < count; ++i) {
        if (at + 46 > eocd || le(zip, at, 4) != 0x02014b50)
            return fail(QStringLiteral("central directory entry %1 is damaged").arg(i));
        const quint32 flags = le(zip, at + 8, 2);
        const quint32 csize = le(zip, at + 20, 4);
        const quint32 nameLen = le(zip, at + 28, 2);
        const qint64 next = at + 46 + nameLen + le(zip, at + 30, 2) + le(zip, at + 32, 2);
        const quint32 local = le(zip, at + 42, 4);
        if (next > eocd) return fail(QStringLiteral("central directory entry %1 is damaged").arg(i));
        if (csize == 0xFFFFFFFF || local == 0xFFFFFFFF) return fail(QStringLiteral("ZIP64 archive"));
        const QByteArrayView rawName = zip.sliced(at + 46, nameLen);
        ZipEntry e;
        // Bit 11 says UTF-8; otherwise the name is CP437, whose ASCII half is all a boot
        // animation ever uses.
        e.name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        e.method = (flags & 0x1) ? quint16(0xFFFF) : quint16(le(zip, at + 10, 2));
        e.size = csize;
        // The data follows the LOCAL header, whose name and extra lengths need not match the
        // directory's copies.
        if (qint64(local) + 30 > cdOffset || le(zip, local, 4) != 0x04034b50)
            return fail(QStringLiteral("local header of %1 is damaged").arg(e.name));
        e.offset = qint64(local) + 30 + le(zip, local + 26, 2) + le(zip, local + 28, 2);
        if (e.offset + e.size > cdOffset)
            return fail(QStringLiteral("data of %1 runs past the archive").arg(e.name));
        out.entries << e;
        at = next;
    }
    return out;
}

bool zipEntryNameIsSafe(const QString &name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('/'))) return false;
    if (name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char(':')) ||
        name.contains(QChar(0)))
        return false;
    for (const QString &part : name.split(QLatin1Char('/')))
        if (part == QLatin1String("..")) return false;
    return true;
}

}  // namespace remora::mirror
