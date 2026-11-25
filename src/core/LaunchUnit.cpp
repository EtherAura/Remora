#include "LaunchUnit.h"

#include <QList>

namespace remora::launchunit {

namespace {

// systemd's unit name escaping undone: \xNN is that byte.
QString unescape(const QString &s) {
    QByteArray out;
    const QByteArray in = s.toUtf8();
    for (qsizetype i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] == 'x') {
            bool ok = false;
            const int byte = in.mid(i + 2, 2).toInt(&ok, 16);
            if (ok) {
                out.append(char(byte));
                i += 3;
                continue;
            }
        }
        out.append(in[i]);
    }
    return QString::fromUtf8(out);
}

}  // namespace

QString launchedApp(const QByteArray &procCgroup) {
    // cgroup v2: the line "0::<path>"; the unit is the path's last component.
    QString path;
    for (const QByteArray &line : procCgroup.split('\n'))
        if (line.startsWith("0::")) path = QString::fromUtf8(line.mid(3)).trimmed();
    const QString unit = path.section(QLatin1Char('/'), -1);
    if (!unit.startsWith(QLatin1String("app-"))) return {};
    QString stem;
    if (unit.endsWith(QLatin1String(".service"))) {
        stem = unit.mid(4, unit.size() - 4 - 8);
        const qsizetype at = stem.indexOf(QLatin1Char('@'));
        if (at >= 0) stem.truncate(at);
    } else if (unit.endsWith(QLatin1String(".scope"))) {
        stem = unit.mid(4, unit.size() - 4 - 6);
        const qsizetype dash = stem.lastIndexOf(QLatin1Char('-'));  // before the random part
        if (dash <= 0) return {};
        stem.truncate(dash);
    } else {
        return {};
    }
    // The id is the last part: a launcher's name, if any, comes before it.
    return unescape(stem.section(QLatin1Char('-'), -1));
}

}  // namespace remora::launchunit
