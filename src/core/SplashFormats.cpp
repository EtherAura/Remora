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

}  // namespace remora::mirror
