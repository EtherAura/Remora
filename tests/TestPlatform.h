#pragma once
// What the suites need to run on a Windows client as well as on Linux (bd remora-7oih.7).

#include <QDir>
#include <QString>
#include <QtTest>

#include "core/ManagedHost.h"

// A case that cannot hold on Windows says so on its first line, with the reason. Either it needs
// what only a POSIX host has — a tool run for real (sh, git, make, python3), testing shell meant
// for a Linux docker or build host; a symbolic link, which QFile::link makes as a .lnk shortcut on
// Windows; mode bits; /proc — or it is about bare, docker on this machine, which Windows refuses
// at preflight (bareNeedsLinuxMessage) and resolves as its own guest over ssh (effectiveBackend).
// A case about the platform's decision itself — which readiness rows a client gets, where a blank
// docker host goes — states the seam that makes it (HostCapabilities::localOs,
// ScopedLocalDockerIsGuest) and runs everywhere.
#ifdef Q_OS_WIN
#define SKIP_ON_WINDOWS(why) QSKIP(why)
#else
#define SKIP_ON_WINDOWS(why) static_cast<void>(0)
#endif

// A file of the checkout the suite was built from: fixtures, vendor files, the sources it audits.
// The path is compiled in, which is right where the suite was built; a cross-built Windows suite
// runs on another machine, where REMORA_TEST_SOURCE_DIR names the checkout instead.
inline QString sourcePath(const QString &rel) {
    QString root = qEnvironmentVariable("REMORA_TEST_SOURCE_DIR");
    if (root.isEmpty()) root = QStringLiteral(REMORA_TEST_SOURCE_ROOT);
    return QDir::cleanPath(root + QLatin1Char('/') + rel);
}

// A Windows engine reads vendor/ from REMORA_VENDOR_DIR, or from beside the executable, where the
// package puts it (vendorDir, engine/Chain.cpp). A suite reads the checkout's, as a Linux build
// does through the path compiled into it.
inline void useCheckoutVendorDir() {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("REMORA_VENDOR_DIR"))
        qputenv("REMORA_VENDOR_DIR", sourcePath(QStringLiteral("vendor")).toLocal8Bit());
#endif
}

// Where this machine's docker is, for one scope: a local daemon (Linux's answer) or Remora's own
// guest (Windows'). The platform's answer comes back when the scope ends, early returns included.
class ScopedLocalDockerIsGuest {
public:
    explicit ScopedLocalDockerIsGuest(bool guest) : was_(remora::localDockerIsGuest()) {
        remora::setLocalDockerIsGuest(guest);
    }
    ~ScopedLocalDockerIsGuest() { remora::setLocalDockerIsGuest(was_); }
    ScopedLocalDockerIsGuest(const ScopedLocalDockerIsGuest &) = delete;
    ScopedLocalDockerIsGuest &operator=(const ScopedLocalDockerIsGuest &) = delete;

private:
    bool was_;
};
