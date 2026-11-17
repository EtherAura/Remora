#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

// The mirror registry's pure half (bd remora-7oih.1): what one running mirror client says about
// itself, and the selection rules every "find / stop / count the mirrors" site applies to that.
//
// Those sites used to be pgrep/pkill patterns over the Linux command line (`remora mirror … -s
// <target> --window-title <name>`), which is two assumptions at once: that there is a local
// shell with procps, and that the mirror's argv reads the way Linux prints it. Neither holds on
// Windows, where every one of them returned nothing — a reconnect left the old mirror beside the
// new one, Stop left windows open, the PIP toggle only ever opened, and auto-sleep counted zero
// clients and froze a device in use. The registry replaces the patterns; the filesystem and
// process work behind it lives in the engine (engine/MirrorRegistry.h).

namespace remora {

// Window-title prefix that marks a desktop-mode window (the device on its own display, as opposed
// to the mirror or to one app's window). It is both the user-visible taskbar name and the thing
// RunContext::desktopWindow() tests, so `remora desktop` and the window Alt+D spawns must spell it
// the same way — hence one constant rather than the two string literals this used to be. Here
// rather than in the engine because a mirror classifies ITSELF by it when it registers.
inline constexpr QLatin1String kDesktopTitlePrefix{"Remora-desktop:"};

// The PIP window's Wayland app_id / X11 class. The class rule keys on it, and the registry tells
// a PIP from the main mirror by it — both are the same binary on the same target.
inline constexpr QLatin1String kPipAppId{"remora-pip"};

// Which window a mirror process is. Told apart exactly the way the old patterns told them apart,
// by the flags its launcher gave it: --boot-animation (the splash that becomes the mirror),
// --app-id=remora-pip, --new-display (an app window, or the desktop when titled with the prefix).
enum class MirrorKind { Main, Splash, Pip, App, Desktop };
QString mirrorKindName(MirrorKind k);
std::optional<MirrorKind> parseMirrorKind(const QString &name);
MirrorKind mirrorKindFor(bool newDisplay, bool bootAnimation, const QString &appId,
                         const QString &title);

// What the mirror reports about its own device session — the self-report that stands in for
// `ss` where there is no `ss` (Windows):
//  Pending  no device session yet: bringing one up, or a splash still animating the boot
//  Live     the session came up
//  Lost     it came up, and its tunnel to the agent has since closed under it — a husk
//  Unknown  nobody said: a mirror found by its command line rather than by its registration
enum class MirrorSessionState { Unknown, Pending, Live, Lost };
QString mirrorSessionName(MirrorSessionState s);

struct MirrorEntry {
    qint64 pid = 0;
    QString serial;  // the adb target, exactly as `-s` gave it
    QString title;   // the window title the mirror shows
    MirrorKind kind = MirrorKind::Main;
    qint64 startedMs = 0;  // ms since the epoch; 0 = not known (an unregistered mirror)
    MirrorSessionState session = MirrorSessionState::Unknown;
    // false: found by the Linux command-line fallback, i.e. started by a Remora that predates the
    // registry. It has no self-report and no start time, so only the `ss` husk test can judge it.
    bool registered = true;
};

// The registry entry's on-disk form. Versioned JSON so a later field does not orphan the entries
// an older client wrote; decode refuses what is not an entry (no pid, no serial) rather than
// guessing at it, because a decoded pid ends up in a stop call.
QByteArray encodeMirrorEntry(const MirrorEntry &e);
std::optional<MirrorEntry> decodeMirrorEntry(const QByteArray &json);

// A mirror client's command line, read back into the entry it would have registered — the Linux
// upgrade fallback, for mirrors an older Remora started (they never registered). nullopt for
// anything that is not a `remora mirror` session: another program, a wrapper shell whose command
// string merely mentions one, or a --list-apps one-shot, which has no window and is no client.
std::optional<MirrorEntry> mirrorEntryFromArgv(const QStringList &argv);

// Is a mirror titled `entryTitle` THIS instance's mirror, for a session titled `windowTitle`?
// The rule the old mirrorTitlePattern spelled as a regex: the instance name, or its legacy
// "Remora:" form; an empty instance matches only the legacy prefix, because "any title" would
// take app and desktop windows with it. Exact rather than the pattern's accidental prefix match,
// which let profile "A" reap profile "A 2"'s mirror on a shared target.
bool isInstanceMirrorTitle(const QString &entryTitle, const QString &windowTitle);

// Every lookup is one of these. An empty field does not constrain.
struct MirrorQuery {
    QString target;            // the adb serial, exactly — `( |$)` was the regex's way to say it
    QList<MirrorKind> kinds;   // empty = every kind
    // Set: isInstanceMirrorTitle against this session title.
    std::optional<QString> instanceTitle;
    QString exactTitle;        // non-empty: the title must be exactly this (the PIP toggle)
};
QVector<MirrorEntry> selectMirrors(const QVector<MirrorEntry> &all, const MirrorQuery &q);

// This instance's mirror on a target: the main window and the splash that becomes it, never a
// PIP, an app or a desktop window. What doConnect, the splash and sleep reap, and what the husk
// reaper may judge.
QVector<MirrorEntry> instanceMirrors(const QVector<MirrorEntry> &all, const QString &target,
                                     const QString &windowTitle);

QList<qint64> mirrorPids(const QVector<MirrorEntry> &es);

// Husks by self-report: a REGISTERED mirror whose session came up and was then lost, and that is
// past the age floor. Never Pending (a splash animates a cold boot for minutes with no tunnel at
// all), never Unknown — absent evidence must not read as dead.
QVector<MirrorEntry> selfReportedHusks(const QVector<MirrorEntry> &es, qint64 nowMs,
                                       qint64 minAgeMs);

// The name of the Win32 event a mirror waits on for a graceful stop (Local\ = this session's
// namespace, where the GUI and its mirrors live). WM_CLOSE alone cannot reach a mirror that has
// not shown its window yet — and a starved, rejected attempt never does — so the stop seam sets
// this as well; both ends build the name here so they cannot drift apart.
QString mirrorStopEventName(qint64 pid);

}  // namespace remora
