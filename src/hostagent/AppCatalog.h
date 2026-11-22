#pragma once
#include <QList>
#include <QString>

#include "core/DesktopEntries.h"

namespace remora::hostagent {

// This computer's launchable apps as a launcher shows them: every applications directory in XDG
// precedence order, reduced by core/DesktopEntries, then without entries whose TryExec program is
// not installed. Read fresh on every call — apps come and go, and a listing is cheap.
QList<desktop::DesktopEntry> installedApps();

// One app by its desktop-file id, or nullopt when no visible entry has it.
std::optional<desktop::DesktopEntry> findApp(const QString &id);

// The app's icon as a size×size PNG, from the desk's icon theme (or the absolute path its entry
// names). Empty with *error when there is no such app or it has no icon to draw.
QByteArray appIconPng(const QString &id, int size, QString *error);

// Points Qt's icon lookup at the desk's icon theme and the XDG icon directories. The agent runs
// on the offscreen platform, which knows neither, so without this no theme icon resolves at all.
void useDeskIconTheme();

}  // namespace remora::hostagent
