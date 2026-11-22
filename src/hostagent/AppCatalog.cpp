#include "AppCatalog.h"

#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QFile>
#include <QLocale>
#include <QBuffer>
#include <QIcon>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>

namespace remora::hostagent {

QList<desktop::DesktopEntry> installedApps() {
    QList<QPair<QString, QString>> files;
    // $XDG_DATA_HOME/applications first, then each $XDG_DATA_DIRS entry: the precedence order
    // collectApps expects, so a user's copy decides over the system's.
    for (const QString &dir :
         QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation)) {
        const QDir root(dir);
        QStringList found;
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) found << root.relativeFilePath(it.next());
        found.sort();  // a stable order within one directory
        for (const QString &rel : found) {
            QFile f(root.filePath(rel));
            if (!f.open(QIODevice::ReadOnly)) continue;
            files.append({desktop::desktopFileId(rel), QString::fromUtf8(f.readAll())});
        }
    }
    const QStringList desktops =
        qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    QList<desktop::DesktopEntry> apps =
        desktop::collectApps(files, desktops, QLocale::system().name());
    apps.removeIf([](const desktop::DesktopEntry &e) {
        return !e.tryExec.isEmpty() && QStandardPaths::findExecutable(e.tryExec).isEmpty() &&
               !QFile::exists(e.tryExec);
    });
    return apps;
}

std::optional<desktop::DesktopEntry> findApp(const QString &id) {
    for (const auto &e : installedApps())
        if (e.id == id) return e;
    return std::nullopt;
}

QByteArray appIconPng(const QString &id, int size, QString *error) {
    const auto app = findApp(id);
    if (!app) {
        *error = QStringLiteral("no app %1 on this computer").arg(id);
        return {};
    }
    const QIcon icon = QFileInfo(app->icon).isAbsolute() ? QIcon(app->icon)
                                                         : QIcon::fromTheme(app->icon);
    const QPixmap pm = icon.pixmap(size, size);
    if (pm.isNull()) {
        *error = QStringLiteral("%1 has no icon this computer can draw (%2)")
                     .arg(id, app->icon.isEmpty() ? QStringLiteral("none named") : app->icon);
        return {};
    }
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    pm.toImage().save(&buf, "PNG");
    return png;
}

void useDeskIconTheme() {
    const QString kdeglobals =
        QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    const QString theme = QSettings(kdeglobals, QSettings::IniFormat)
                              .value(QStringLiteral("Icons/Theme"), QStringLiteral("breeze"))
                              .toString();
    // The offscreen platform's theme names no icon directories either; these are the XDG ones a
    // desk's own platform theme would give.
    QStringList paths;
    for (const QString &dir :
         QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        paths << dir + QStringLiteral("/icons");
    paths << QDir::homePath() + QStringLiteral("/.icons");
    QIcon::setThemeSearchPaths(paths);
    QIcon::setFallbackSearchPaths({QStringLiteral("/usr/share/pixmaps")});
    QIcon::setThemeName(theme);
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
}

}  // namespace remora::hostagent
