#include "engine/GuestVm.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include "core/Guest.h"
#include "core/ManagedHost.h"

#ifdef Q_OS_WIN
#include <windows.h>

#include <string>
#endif

namespace remora {

namespace {

// Loopback ports, chosen once at provisioning and kept: a profile's adb target is derived from
// them, so moving them would strand every target already resolved. Shifted together in steps of
// 100 when the defaults are taken.
constexpr int kDefaultQmpPort = 47021;
constexpr int kDefaultSshPort = 47022;
constexpr int kDefaultAdbFirst = 47555;
constexpr int kAdbCount = 20;
const QString kUser = QStringLiteral("remora");

struct State {
    qint64 pid = 0;
    int qmpPort = kDefaultQmpPort;
    int sshPort = kDefaultSshPort;
    int adbLocalFirst = kDefaultAdbFirst;
    int adbCount = kAdbCount;
    QString accel;
    QString imageSha;  // system.qcow2's, from the manifest the overlay was made against
    QString imageDir;
};

QString path(const QString &name) { return GuestVm::stateDir() + QLatin1Char('/') + name; }

State readState() {
    State s;
    QFile f(path(QStringLiteral("state.json")));
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    s.pid = o.value(QStringLiteral("pid")).toInteger();
    s.qmpPort = o.value(QStringLiteral("qmpPort")).toInt(kDefaultQmpPort);
    s.sshPort = o.value(QStringLiteral("sshPort")).toInt(kDefaultSshPort);
    s.adbLocalFirst = o.value(QStringLiteral("adbLocalFirst")).toInt(kDefaultAdbFirst);
    s.adbCount = o.value(QStringLiteral("adbCount")).toInt(kAdbCount);
    s.accel = o.value(QStringLiteral("accel")).toString();
    s.imageSha = o.value(QStringLiteral("imageSha")).toString();
    s.imageDir = o.value(QStringLiteral("imageDir")).toString();
    return s;
}

bool writeState(const State &s) {
    QJsonObject o;
    o.insert(QStringLiteral("pid"), s.pid);
    o.insert(QStringLiteral("qmpPort"), s.qmpPort);
    o.insert(QStringLiteral("sshPort"), s.sshPort);
    o.insert(QStringLiteral("adbLocalFirst"), s.adbLocalFirst);
    o.insert(QStringLiteral("adbCount"), s.adbCount);
    o.insert(QStringLiteral("accel"), s.accel);
    o.insert(QStringLiteral("imageSha"), s.imageSha);
    o.insert(QStringLiteral("imageDir"), s.imageDir);
    QFile f(path(QStringLiteral("state.json")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(o).toJson()) > 0;
}

void registerEndpoint(const State &s) {
    ManagedHost h;
    h.alias = guestAlias();
    h.user = kUser;
    h.sshPort = s.sshPort;
    h.identityFile = QDir::toNativeSeparators(path(QStringLiteral("client")));
    h.knownHostsFile = QDir::toNativeSeparators(path(QStringLiteral("known_hosts")));
    h.adbLocalFirst = s.adbLocalFirst;
    h.adbCount = s.adbCount;
    h.accelerator = s.accel;
    registerManagedHost(h);
}

// One QMP exchange: greeting, capabilities, then `cmd` (or nothing — a liveness probe). QEMU
// answers on this port for exactly as long as it runs, which makes it a better liveness test
// than a pid that Windows may have handed to something else since.
bool qmp(int port, const char *cmd, int timeoutMs = 2000) {
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, quint16(port));
    if (!s.waitForConnected(timeoutMs) || !s.waitForReadyRead(timeoutMs)) return false;
    s.readAll();
    s.write("{\"execute\":\"qmp_capabilities\"}\n");
    if (!s.waitForReadyRead(timeoutMs)) return false;
    s.readAll();
    if (cmd) {
        s.write(cmd);
        s.waitForBytesWritten(timeoutMs);
        s.waitForReadyRead(timeoutMs);
    }
    return true;
}

bool portFree(int port) {
    QTcpServer srv;
    return srv.listen(QHostAddress::LocalHost, quint16(port));
}

bool blockFree(const State &s) {
    if (!portFree(s.qmpPort) || !portFree(s.sshPort)) return false;
    for (int i = 0; i < s.adbCount; ++i)
        if (!portFree(s.adbLocalFirst + i)) return false;
    return true;
}

QString manifestLine(const QString &dir, const QString &file) {
    QFile f(dir + QStringLiteral("/manifest.txt"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    if (file.isEmpty()) return lines.value(0).trimmed();
    for (const QString &l : lines)
        if (l.endsWith(QStringLiteral("  ") + file)) return l.section(QLatin1Char(' '), 0, 0);
    return {};
}

QString tail(const QString &file, int lines) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QStringList l = QString::fromUtf8(f.readAll()).trimmed().split(QLatin1Char('\n'));
    return l.mid(qMax(0, int(l.size()) - lines)).join(QLatin1Char('\n'));
}

void say(const LineSink &log, const QString &text) {
    if (log) log(QStringLiteral("stdout"), text);
}

QString qemuImg() {
    const QString env = qEnvironmentVariable("REMORA_GUEST_QEMU_IMG");
    if (!env.isEmpty()) return env;
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("REMORA_GUEST_QEMU"))
        return QCoreApplication::applicationDirPath() + QStringLiteral("/qemu/qemu-img.exe");
#endif
    return QStringLiteral("qemu-img");
}

bool bundledWinq() {
#ifdef Q_OS_WIN
    return qEnvironmentVariableIsEmpty("REMORA_GUEST_QEMU") &&
           QFileInfo(QCoreApplication::applicationDirPath() +
                     QStringLiteral("/qemu/remora-winq-alpha10.txt")).isFile();
#else
    return false;
#endif
}

QString firmwareDir() {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("REMORA_GUEST_QEMU") && !bundledWinq())
        return QCoreApplication::applicationDirPath() + QStringLiteral("/qemu/share");
#endif
    return {};
}

const QString kWhpxMissing = QStringLiteral(
    "Remora runs Android in a small Linux virtual machine of its own, and Windows lets it use "
    "the processor's virtualization only through the Windows Hypervisor Platform, which is not "
    "available. Turn it on in Settings > System > Optional features > More Windows features > "
    "Windows Hypervisor Platform (or, as administrator: dism /online /enable-feature "
    "/featurename:HypervisorPlatform /all), then restart Windows. If it is already on, "
    "virtualization (Intel VT-x or AMD-V) is turned off in the PC's firmware settings. "
    "guest_accel=tcg runs the guest without it, for testing only: Android boots and its apps can "
    "be listed, but the mirror cannot show the screen.");

#ifdef Q_OS_WIN
// A Windows restart ends the session with the guest running, and QEMU, which has no window, is
// simply terminated: a power cut to the guest, whose last seconds of writes are lost and whose
// disks need journal recovery at the next boot (seen: Windows was down 8 s after the
// restart began). The watcher is what stops the guest properly first. One per guest, held by
// name; a stop waits on the same name, so whoever stopped the guest (an installer about to
// replace remora-gui.exe) finds the watcher gone too.
std::wstring watchMutexName(int qmpPort) {
    return L"Local\\RemoraGuestWatch-" + std::to_wstring(qmpPort);
}

void awaitWatcher(int qmpPort) {
    HANDLE m = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, watchMutexName(qmpPort).c_str());
    if (!m) return;
    const DWORD r = WaitForSingleObject(m, 5000);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(m);
    CloseHandle(m);
}

// remora-gui.exe, beside this program: it has no console, and Windows ends a console program at
// once, whatever it asked for. This program when there is none (a dev tree).
QString watcherProgram() {
    const QString gui = QCoreApplication::applicationDirPath() + QStringLiteral("/remora-gui.exe");
    return QFileInfo::exists(gui) ? gui : QCoreApplication::applicationFilePath();
}

struct Watch {
    GuestVm *vm;
    const LineSink *log;
};
Watch *g_watch = nullptr;

LRESULT CALLBACK watchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_QUERYENDSESSION) return TRUE;
    if (msg == WM_ENDSESSION) {
        // Only once the end is certain: a restart the user cancels must not cost them the guest.
        if (wp && g_watch) {
            say(*g_watch->log, QStringLiteral("Windows is ending the session: powering the "
                                              "guest down first"));
            say(*g_watch->log, g_watch->vm->stop(*g_watch->log).detail);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
#endif

}  // namespace

QString GuestVm::stateDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/remora/guest");
}

QString GuestVm::imageDir() {
    const QString env = qEnvironmentVariable("REMORA_GUEST_IMAGE_DIR");
    if (!env.isEmpty()) return env;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/guest");
}

QString GuestVm::qemuProgram() {
    const QString env = qEnvironmentVariable("REMORA_GUEST_QEMU");
    if (!env.isEmpty()) return env;
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + QStringLiteral("/qemu/qemu-system-x86_64.exe");
#else
    return QStringLiteral("qemu-system-x86_64");
#endif
}

void GuestVm::registerFromState() {
    if (!localDockerIsGuest() && !QFileInfo::exists(path(QStringLiteral("state.json")))) return;
    registerEndpoint(readState());
}

GuestVm::Status GuestVm::status() const {
    Status st;
    const QString img = imageDir();
    st.shipped = QFileInfo::exists(img + QStringLiteral("/system.qcow2"));
    st.image = manifestLine(img, {});
    st.provisioned = QFileInfo::exists(path(QStringLiteral("data.qcow2"))) &&
                     QFileInfo::exists(path(QStringLiteral("client")));
    const State s = readState();
    st.running = qmp(s.qmpPort, nullptr, 1000);
    st.pid = st.running ? s.pid : 0;
    st.sshPort = s.sshPort;
    st.accel = s.accel;
    return st;
}

StepResult GuestVm::ensureRunning(const ResolvedConfig &rc, const LineSink &log) {
    State s = readState();
    // Unlogged: while the guest boots, every probe fails with ssh's own "connection refused" or
    // "timed out during banner exchange", which says nothing a user can act on.
    const auto ready = [&] {
        return sp_.run(Spawner::sshArgv(guestAlias(), QStringLiteral(
                                            "docker info >/dev/null 2>&1 && echo ok")))
                   .out.trimmed() == QLatin1String("ok");
    };
    if (qmp(s.qmpPort, nullptr, 1000)) {
        registerEndpoint(s);
        if (ready()) return StepResult::good(QStringLiteral("guest running (%1)").arg(s.accel));
        // Up but not answering yet: a start another process began moments ago. Wait below.
    } else {
        const QString img = imageDir();
        for (const char *f : {"vmlinuz", "system.qcow2", "cmdline", "manifest.txt"})
            if (!QFileInfo::exists(img + QLatin1Char('/') + QLatin1String(f)))
                return StepResult::fail(
                    QStringLiteral("Remora's Linux guest is not installed: %1 has no %2. "
                                   "Reinstall Remora.")
                        .arg(QDir::toNativeSeparators(img), QLatin1String(f)));
        if (!QDir().mkpath(stateDir()))
            return StepResult::fail(QStringLiteral("cannot create %1").arg(stateDir()));

        // Keys: the client's, and the host key the guest is HANDED (see guest/rootfs/etc/init.d/
        // remora-guest), which is what lets known_hosts pin it before the first connection.
        for (const auto &[file, comment] : {std::pair{QStringLiteral("client"),
                                                      QStringLiteral("remora-client")},
                                            std::pair{QStringLiteral("ssh_host_ed25519_key"),
                                                      QStringLiteral("remora-guest")}}) {
            if (QFileInfo::exists(path(file))) continue;
            const ProcResult r = sp_.run({QStringLiteral("ssh-keygen"), QStringLiteral("-q"),
                                          QStringLiteral("-t"), QStringLiteral("ed25519"),
                                          QStringLiteral("-N"), QString(), QStringLiteral("-C"),
                                          comment, QStringLiteral("-f"),
                                          QDir::toNativeSeparators(path(file))},
                                         {}, log);
            if (r.rc != 0)
                return StepResult::fail(QStringLiteral("ssh-keygen could not make %1").arg(file),
                                        r.stderrTail);
        }

        // Ports: kept from the last start when they are still free, else the next free block.
        if (!blockFree(s)) {
            State t = s;
            bool found = false;
            for (int i = 1; i <= 20 && !found; ++i) {
                t.qmpPort = kDefaultQmpPort + 100 * i;
                t.sshPort = kDefaultSshPort + 100 * i;
                t.adbLocalFirst = kDefaultAdbFirst + 100 * i;
                found = blockFree(t);
            }
            if (!found)
                return StepResult::fail(QStringLiteral(
                    "no free block of loopback ports for the guest (tried 47021 upward)"));
            s = t;
        }
        {
            QFile pub(path(QStringLiteral("ssh_host_ed25519_key.pub")));
            QFile kh(path(QStringLiteral("known_hosts")));
            if (!pub.open(QIODevice::ReadOnly) ||
                !kh.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return StepResult::fail(QStringLiteral("cannot write the guest's known_hosts"));
            const QStringList key =
                QString::fromUtf8(pub.readAll()).trimmed().split(QLatin1Char(' '));
            kh.write(QStringLiteral("[127.0.0.1]:%1 %2 %3\n")
                         .arg(s.sshPort)
                         .arg(key.value(0), key.value(1))
                         .toUtf8());
        }

        // Disks. The overlay is rebuilt whenever the shipped image changed (an update) or moved
        // (its backing path is absolute); the data disk never is.
        const QString sysImg = QDir(img).absoluteFilePath(QStringLiteral("system.qcow2"));
        const QString sha = manifestLine(img, QStringLiteral("system.qcow2"));
        const QString overlay = path(QStringLiteral("system-overlay.qcow2"));
        if (!QFileInfo::exists(overlay) || s.imageSha != sha || s.imageDir != img) {
            QFile::remove(overlay);
            const ProcResult r = sp_.run({qemuImg(), QStringLiteral("create"), QStringLiteral("-q"),
                                          QStringLiteral("-f"), QStringLiteral("qcow2"),
                                          QStringLiteral("-b"), QDir::toNativeSeparators(sysImg),
                                          QStringLiteral("-F"), QStringLiteral("qcow2"),
                                          QDir::toNativeSeparators(overlay)},
                                         {}, log);
            if (r.rc != 0)
                return StepResult::fail(QStringLiteral("qemu-img could not create the system "
                                                       "overlay"),
                                        r.stderrTail);
            if (s.imageSha.isEmpty()) say(log, QStringLiteral("guest system: %1").arg(manifestLine(img, {})));
            else say(log, QStringLiteral("guest system updated: %1").arg(manifestLine(img, {})));
            s.imageSha = sha;
            s.imageDir = img;
        }
        const QString data = path(QStringLiteral("data.qcow2"));
        if (!QFileInfo::exists(data)) {
            // Sparse: it takes what the guest writes, up to this.
            const ProcResult r = sp_.run({qemuImg(), QStringLiteral("create"), QStringLiteral("-q"),
                                          QStringLiteral("-f"), QStringLiteral("qcow2"),
                                          QDir::toNativeSeparators(data), QStringLiteral("256G")},
                                         {}, log);
            if (r.rc != 0)
                return StepResult::fail(QStringLiteral("qemu-img could not create the data disk"),
                                        r.stderrTail);
        }

        QString accel = rc.guestAccel;
        if (accel.isEmpty() || accel == QLatin1String("auto")) {
#ifdef Q_OS_WIN
            accel = QStringLiteral("whpx");
#else
            accel = QStringLiteral("kvm");
#endif
        }
        QFile cmdline(img + QStringLiteral("/cmdline"));
        if (!cmdline.open(QIODevice::ReadOnly))
            return StepResult::fail(QStringLiteral("cannot read the guest's kernel command line"));

        GuestLaunch g;
        g.qemu = qemuProgram();
        g.firmwareDir = firmwareDir();
        g.accel = accel;
        g.winq = bundledWinq();
        g.memoryMb = rc.guestMemoryMb;
        // Half the machine, from 2 to 8: Android's first boot compiles every app, and it is
        // the machine's other half that the user is sitting at.
        g.cpus = rc.guestCpus > 0 ? rc.guestCpus : qBound(2, QThread::idealThreadCount() / 2, 8);
        g.kernel = QDir::toNativeSeparators(img + QStringLiteral("/vmlinuz"));
        if (QFileInfo::exists(img + QStringLiteral("/initramfs")))
            g.initrd = QDir::toNativeSeparators(img + QStringLiteral("/initramfs"));
        g.cmdline = QString::fromUtf8(cmdline.readAll()).trimmed();
        g.systemDisk = QDir::toNativeSeparators(overlay);
        g.dataDisk = QDir::toNativeSeparators(data);
        g.sshPort = s.sshPort;
        g.adbLocalFirst = s.adbLocalFirst;
        g.adbCount = s.adbCount;
        g.qmpPort = s.qmpPort;
        g.authorizedKey = QDir::toNativeSeparators(path(QStringLiteral("client.pub")));
        g.hostKey = QDir::toNativeSeparators(path(QStringLiteral("ssh_host_ed25519_key")));
        g.consoleLog = QDir::toNativeSeparators(path(QStringLiteral("console.log")));

        say(log, QStringLiteral("starting Remora's Linux guest (%1, %2 MB, %3 CPUs)")
                     .arg(accel)
                     .arg(g.memoryMb)
                     .arg(g.cpus));
        if (g.winq) say(log, QStringLiteral("guest hypervisor: bundled WINQ Alpha 10"));
        const Detached d = sp_.spawnDetached(buildGuestQemuArgv(g));
        if (d.pid <= 0)
            return StepResult::fail(QStringLiteral("could not start %1").arg(g.qemu));
        s.pid = d.pid;
        s.accel = accel;
        writeState(s);
        registerEndpoint(s);

        // A missing accelerator fails within a second or two, before the guest exists at all;
        // catch it by name so the user reads what to turn on rather than QEMU's own words.
        sp_.waitMs(3000);
        if (!sp_.detachedAlive(d)) {
            const QString why = tail(d.log, 6);
            if (why.contains(QLatin1String("No accelerator found")) ||
                why.contains(QLatin1String("failed to initialize whpx")))
                return StepResult::fail(kWhpxMissing, why);
            if (why.contains(QLatin1String("failed to initialize kvm")))
                return StepResult::fail(QStringLiteral("QEMU cannot use KVM here (/dev/kvm "
                                                       "missing or not yours)"),
                                        why);
            return StepResult::fail(QStringLiteral("the guest's QEMU exited at once"), why);
        }
#ifdef Q_OS_WIN
        sp_.spawnDetached({watcherProgram(), QStringLiteral("guest"), QStringLiteral("watch")});
#endif
    }

    // Boot to ssh is ~10 s on a hypervisor; TCG is an order of magnitude slower.
    const int budgetMs = (s.accel == QLatin1String("tcg") ? 900 : 180) * 1000;
    for (int waited = 0; waited < budgetMs; waited += 2000) {
        if (ready())
            return StepResult::good(QStringLiteral("guest running (%1)").arg(s.accel));
        if (!qmp(s.qmpPort, nullptr, 1000))
            return StepResult::fail(QStringLiteral("the guest stopped while booting"),
                                    tail(path(QStringLiteral("console.log")), 12));
        sp_.waitMs(2000);
    }
    return StepResult::fail(QStringLiteral("the guest did not answer over ssh within %1 s")
                                .arg(budgetMs / 1000),
                            tail(path(QStringLiteral("console.log")), 12));
}

StepResult GuestVm::stop(const LineSink &log, int graceMs) {
    const State s = readState();
    if (!qmp(s.qmpPort, nullptr, 1000)) return StepResult::good(QStringLiteral("guest not running"));
    // A shutdown stops every container first, and under TCG that alone outlasts a minute: the
    // quit below is a power cut, so give an emulated guest the time a clean one needs.
    if (s.accel == QLatin1String("tcg")) graceMs = qMax(graceMs, 240000);
    say(log, QStringLiteral("powering the guest down"));
    qmp(s.qmpPort, "{\"execute\":\"system_powerdown\"}\n");
    const auto stopped = [&s](const QString &detail) {
#ifdef Q_OS_WIN
        awaitWatcher(s.qmpPort);
#endif
        return StepResult::good(detail);
    };
    for (int waited = 0; waited < graceMs; waited += 1000) {
        sp_.waitMs(1000);
        if (!qmp(s.qmpPort, nullptr, 500)) return stopped(QStringLiteral("guest stopped"));
    }
    qmp(s.qmpPort, "{\"execute\":\"quit\"}\n");
    for (int waited = 0; waited < 10000; waited += 1000) {
        sp_.waitMs(1000);
        if (!qmp(s.qmpPort, nullptr, 500))
            return stopped(QStringLiteral("guest stopped (it did not power down within %1 s, so "
                                          "QEMU was told to quit)")
                               .arg(graceMs / 1000));
    }
    return StepResult::fail(QStringLiteral("the guest's QEMU (pid %1) would not stop").arg(s.pid));
}

int GuestVm::watchSessionEnd(const LineSink &log) {
#ifdef Q_OS_WIN
    const State s = readState();
    if (!qmp(s.qmpPort, nullptr, 1000)) return 0;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, watchMutexName(s.qmpPort).c_str());
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    HANDLE qemu = OpenProcess(SYNCHRONIZE, FALSE, DWORD(s.pid));
    if (!qemu) {
        say(log, QStringLiteral("guest watch: cannot open the guest's QEMU (pid %1)").arg(s.pid));
        return 1;
    }
    // Windows ends processes from the highest level down, and QEMU has the default 0x280: this
    // one is asked first, while the guest still runs, and QEMU is not ended until it answers.
    SetProcessShutdownParameters(0x3FF, 0);
    WNDCLASSW wc{};
    wc.lpfnWndProc = watchProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RemoraGuestWatch";
    RegisterClassW(&wc);
    // Hidden but top-level: a message-only window is never told the session is ending.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Remora guest", WS_OVERLAPPEDWINDOW, 0, 0,
                                0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    // Registered up front: Windows ends a process with no visible window five seconds into a
    // shutdown unless it has said why it needs longer, and then shows this until it is done.
    // Only for a shutdown begun in this session: one begun from another (`shutdown /r` over
    // ssh) ended this process after five seconds all the same (seen).
    ShutdownBlockReasonCreate(hwnd, L"Stopping Android, so that nothing it is saving is lost");
    Watch w{this, &log};
    g_watch = &w;
    for (bool quit = false; !quit;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &qemu, FALSE, INFINITE, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break;  // QEMU is gone (or the wait failed)
        MSG m;
        while (!quit && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            quit = m.message == WM_QUIT;
            DispatchMessageW(&m);
        }
    }
    g_watch = nullptr;
    ShutdownBlockReasonDestroy(hwnd);
    DestroyWindow(hwnd);
    CloseHandle(qemu);
    // The mutex is left to this process's exit, which is what a stop waiting on it waits for.
    return 0;
#else
    Q_UNUSED(log);
    return 0;
#endif
}

StepResult GuestVm::resetSystem(const LineSink &log) {
    const StepResult st = stop(log);
    if (!st.ok) return st;
    QFile::remove(path(QStringLiteral("system-overlay.qcow2")));
    State s = readState();
    s.imageSha.clear();
    writeState(s);
    return StepResult::good(QStringLiteral("guest system reset; the next start boots the shipped "
                                           "image, and the data disk is kept"));
}

}  // namespace remora
