#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <optional>

// This computer's launchable apps, from their desktop entries — the host agent's list-apps
// (docs/PHONE_APP.md §1). Pure: the agent reads the files, in XDG precedence order, and
// hands their text here. Implements the parts of the Desktop Entry Specification a launcher
// needs: the [Desktop Entry] group, localized names, Hidden/NoDisplay, OnlyShowIn/NotShowIn and
// the first-file-wins precedence by desktop-file id.

namespace remora::desktop {

struct DesktopEntry {
    QString id;       // desktop-file id without ".desktop": "org.kde.dolphin" — KWin's
                      // desktopFileName for the app's windows, and what kstart --application takes
    QString name;     // localized
    QString icon;     // a theme name or an absolute path
    QString tryExec;  // the caller drops the entry when this program is not installed
};

// "kde/foo.desktop" (relative to an applications directory) → "kde-foo".
QString desktopFileId(const QString &relativePath);

// Remora's own menu entries (remora-*.desktop): each one launches an ANDROID app in a mirror on
// the desk, so listing them as Linux apps would open a mirror on the desk instead of anything on
// the phone.
bool isRemoraLauncher(const QString &id);

// One file. nullopt when it does not describe an application a launcher shows: not
// Type=Application, Hidden, NoDisplay, excluded for these desktops (XDG_CURRENT_DESKTOP split on
// ':'), or neither Exec nor DBusActivatable. `locale` is "lang_COUNTRY@MODIFIER" in any partial
// form ("de_DE", "sr@latin", "fr").
std::optional<DesktopEntry> parseDesktopEntry(const QString &id, const QString &text,
                                              const QStringList &currentDesktops,
                                              const QString &locale);

// Every (id, text) pair in precedence order — $XDG_DATA_HOME first, then $XDG_DATA_DIRS — reduced
// to what a launcher shows. The FIRST file for an id decides, so a user's Hidden=true copy masks
// the system entry rather than falling through to it. Sorted by name, case-insensitively.
QList<DesktopEntry> collectApps(const QList<QPair<QString, QString>> &files,
                                const QStringList &currentDesktops, const QString &locale);

// The list-apps listing (docs/MIRROR_AGENT.md): " - Name  id" per app. Every line is " - "
// because none of these is an Android system app; the id is last, as the agent's listing has it.
QByteArray listAppsBlob(const QList<DesktopEntry> &apps);

}  // namespace remora::desktop
