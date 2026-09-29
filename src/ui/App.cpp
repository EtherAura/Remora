#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QStandardPaths>
#include <QTimer>

#include "core/Branding.h"
#include "ui/MainWindow.h"

namespace remora {

// Entry for the `remora gui` subcommand. Kept in its own TU so main.cpp stays a QCoreApplication
// CLI (Widgets pulls in QApplication + the whole GUI stack — only linked into this path).
int runGui(int argc, char **argv) {
    // The workspace can spawn an embedded WebEngine login window (RedditLoginWindow); WebEngine
    // wants a shared GL context set before the QApplication exists.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("remora"));
    QApplication::setApplicationDisplayName(QStringLiteral("Remora"));
    // Ties the Wayland window to the installed .desktop entry (taskbar icon, activities). The id
    // lives in core/Branding.h because the MIRROR needs the same one — it had no identity at all
    // and rendered with the compositor's generic glyph (bd remora-28ix.2.8).
    QGuiApplication::setDesktopFileName(QString::fromLatin1(kDesktopId));
    // Every icon in the workspace is QIcon::fromTheme (themeIcon in MainWindow.cpp). A session
    // with no platform theme — a bare window manager, ssh -X, the offscreen platform the docs
    // screenshots render on — hands Qt neither an icon theme NAME nor the XDG icon DIRECTORIES
    // (measured on offscreen: themeName '' and themeSearchPaths [":/icons"]), so every icon-only
    // control — the Browse buttons beside a path, the run-row actions — came up as an empty
    // square. Supply both from what is installed; a desktop that names a theme is left alone.
    if (QIcon::themeName().isEmpty() && QIcon::fallbackThemeName().isEmpty()) {
        QStringList dirs = QIcon::themeSearchPaths();
        for (const QString &d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
            if (!dirs.contains(d + QLatin1String("/icons"))) dirs << d + QLatin1String("/icons");
        if (!dirs.contains(QDir::homePath() + QLatin1String("/.icons")))
            dirs << QDir::homePath() + QLatin1String("/.icons");
        QIcon::setThemeSearchPaths(dirs);
        for (const char *cand : {"breeze", "Adwaita", "Papirus", "hicolor"}) {
            bool found = false;
            for (const QString &dir : dirs)
                if (QFile::exists(dir + QLatin1Char('/') + QLatin1String(cand)
                                  + QLatin1String("/index.theme")))
                    found = true;
            if (found) {
                QIcon::setFallbackThemeName(QLatin1String(cand));
                break;
            }
        }
    }
    // Set the window icon explicitly too: the desktop-file association drives the taskbar, but
    // the titlebar/window icon needs an actual QIcon or the WM falls back to a generic glyph.
    QIcon appIcon = QIcon::fromTheme(QString::fromLatin1(kDesktopId));
    if (appIcon.isNull())
        appIcon = QIcon(QString::fromLatin1(kIconPathTemplate).arg(QString::fromLatin1(kDesktopId)));
    if (!appIcon.isNull())
        QApplication::setWindowIcon(appIcon);
    MainWindow w;
    w.show();

    // REMORA_SHOT=<path>: render the window through Qt to a PNG and quit. Deterministic, works even
    // on the offscreen platform — no external capture tool or compositor cooperation needed.
    // REMORA_SHOT_PAGE=<sidebar title> selects which page is shown (default: the first).
    if (qEnvironmentVariableIsSet("REMORA_SHOT")) {
        const QString path = qEnvironmentVariable("REMORA_SHOT");
        if (qEnvironmentVariableIsSet("REMORA_SHOT_PAGE"))
            w.selectPage(qEnvironmentVariable("REMORA_SHOT_PAGE"));
        if (qEnvironmentVariableIsSet("REMORA_SHOT_LOG")) w.setLogVisible(true);
        if (qEnvironmentVariableIsSet("REMORA_SHOT_COLLAPSED")) w.setSidebarCollapsed(true);
        // No resize of its own: the shot renders at the window's default size (MainWindow's
        // constructor), which is sized to the widest page — one number, so the docs cannot drift
        // from what a launch shows. warnIfPageOverflows() below is the check that it still fits.
        // REMORA_SHOT_DELAY_MS: how long the page gets to settle before the grab. The default
        // suits static pages; the Image page's library scan pays a docker system df (~0.7s
        // measured), so its rows need a longer delay to be in the picture.
        const int delay = qEnvironmentVariableIntValue("REMORA_SHOT_DELAY_MS") > 0
                              ? qEnvironmentVariableIntValue("REMORA_SHOT_DELAY_MS")
                              : 500;
        QTimer::singleShot(delay, &app, [&w, path] {
            w.warnIfPageOverflows();
            w.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}

}  // namespace remora
