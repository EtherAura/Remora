#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace remora {

// The Windows half of the app menu (bd remora-7oih): what Linux writes as .desktop files in a
// launcher folder, Windows gets as shortcuts in a Start-menu folder. Pure, so what each shortcut
// says is tested offline; engine/AppMenu decides what to write and Spawner::writeShortcut writes
// it.

// One app as `remora mirror --list-apps` prints it: " * Name  pkg" (system) or " - Name  pkg"
// (user). The package is the last field; the name is everything before it. The same contract
// remora-app-menu.sh parses with awk.
struct ListedApp {
    QString pkg;
    QString name;
};
QVector<ListedApp> parseListApps(const QString &raw);

// One .lnk, described. Arguments are already one Windows command line (windowsArgs).
struct Shortcut {
    QString path;         // the .lnk file
    QString target;       // the program it starts
    QString arguments;
    QString workingDir;
    QString iconPath;     // a .ico; empty means the program's own icon
    QString description;  // the tooltip
    // The AppUserModelID. A window whose process sets the same id is paired with this entry on
    // the taskbar, which shows the entry's name and icon: what StartupWMClass does on Linux.
    QString appId;
};

// Quoting a program's own argv parser (CommandLineToArgvW, and Qt's on Windows) undoes:
// backslashes are literal unless they precede a quote, so only those are doubled.
QString windowsQuoteArg(const QString &arg);
QString windowsArgs(const QStringList &args);

// A file name Windows accepts for an entry called `name`: no <>:"/\|?* or control characters,
// no trailing dot or space, not a device name (CON, NUL, COM1…), never empty.
QString windowsFileName(const QString &name);

// A PNG wrapped as a one-image .ico, which Windows reads natively at up to 256×256 (the agent's
// icon renders are 192). Empty when the bytes are not a PNG of that size.
QByteArray icoFromPng(const QByteArray &png);

}  // namespace remora
