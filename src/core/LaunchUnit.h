#pragma once
#include <QByteArray>
#include <QString>

// The desktop file an app was launched from, read off its process's cgroup, for the host agent's
// window placement (docs/PHONE_APP.md). Pure: /proc/<pid>/cgroup's text in.
//
// KIO (kstart, and the desk's own launcher) starts an app in a systemd unit named after its
// desktop file: app-<id>@<random>.service, or app-<id>-<random>.scope; some launchers put their
// own name before the id (app-flatpak-<id>-<n>.scope); and systemd escapes the id, so a '-' in it
// is \x2d and every unescaped '-' is a separator. A window that names no desktop file of its own
// (an X11 app whose class maps to none, such as Wine's) is still known by the unit its process
// runs in, its children's included.

namespace remora::launchunit {

// The desktop file id, without ".desktop"; empty when the process runs in no such unit.
QString launchedApp(const QByteArray &procCgroup);

}  // namespace remora::launchunit
