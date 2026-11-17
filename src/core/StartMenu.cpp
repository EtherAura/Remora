#include "core/StartMenu.h"

#include <QRegularExpression>
#include <QtEndian>

namespace remora {

QVector<ListedApp> parseListApps(const QString &raw) {
    static const QRegularExpression kEntry(QStringLiteral("^\\s*[-*]\\s+(.*\\S)\\s+(\\S+)\\s*$"));
    QVector<ListedApp> out;
    for (const QString &line : raw.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = kEntry.match(line);
        if (!m.hasMatch()) continue;
        ListedApp a{m.captured(2), m.captured(1).trimmed()};
        // The script's own filter: a package has a dot, and an entry has a name.
        if (a.pkg.contains(QLatin1Char('.')) && !a.name.isEmpty()) out << a;
    }
    return out;
}

QString windowsQuoteArg(const QString &arg) {
    static const QRegularExpression kNeedsQuotes(QStringLiteral("[\\s\"]"));
    if (!arg.isEmpty() && !arg.contains(kNeedsQuotes)) return arg;
    QString out(QLatin1Char('"'));
    int slashes = 0;
    for (const QChar c : arg) {
        if (c == QLatin1Char('\\')) {
            ++slashes;
            continue;
        }
        // Backslashes before a quote are doubled, and the quote escaped; any others are literal.
        out += QString(c == QLatin1Char('"') ? slashes * 2 + 1 : slashes, QLatin1Char('\\'));
        out += c;
        slashes = 0;
    }
    // Before the closing quote, so that it still closes.
    out += QString(slashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}

QString windowsArgs(const QStringList &args) {
    QStringList quoted;
    for (const QString &a : args) quoted << windowsQuoteArg(a);
    return quoted.join(QLatin1Char(' '));
}

QString windowsFileName(const QString &name) {
    QString s;
    for (const QChar c : name)
        s += (c.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c)) ? QLatin1Char('_')
                                                                                : c;
    s = s.trimmed();
    while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' '))) s.chop(1);
    if (s.size() > 100) s = s.left(100).trimmed();
    // A device name is reserved with any extension, so "NUL.lnk" opens the device, not a file.
    static const QRegularExpression kDevice(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (s.isEmpty() || kDevice.match(s).hasMatch()) s += QLatin1Char('_');
    return s;
}

QByteArray icoFromPng(const QByteArray &png) {
    static const QByteArray kSignature("\x89PNG\r\n\x1a\n", 8);
    // The IHDR chunk is always first: width and height are its first two fields.
    if (png.size() < 24 || !png.startsWith(kSignature) || png.mid(12, 4) != "IHDR") return {};
    const quint32 w = qFromBigEndian<quint32>(png.constData() + 16);
    const quint32 h = qFromBigEndian<quint32>(png.constData() + 20);
    if (w == 0 || h == 0 || w > 256 || h > 256) return {};
    QByteArray ico(6 + 16, '\0');
    char *d = ico.data();
    qToLittleEndian<quint16>(1, d + 2);  // type: icon
    qToLittleEndian<quint16>(1, d + 4);  // one image
    d[6] = char(w == 256 ? 0 : w);       // 0 means 256
    d[7] = char(h == 256 ? 0 : h);
    qToLittleEndian<quint16>(1, d + 10);   // colour planes
    qToLittleEndian<quint16>(32, d + 12);  // bits per pixel
    qToLittleEndian<quint32>(quint32(png.size()), d + 14);
    qToLittleEndian<quint32>(6 + 16, d + 18);  // where the image starts
    return ico + png;
}

}  // namespace remora
