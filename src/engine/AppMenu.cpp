// Desktop app-menu integration: one launcher folder per profile, one entry per installed app, the
// agent's IconDump rendering the real icons on the device. On Linux the folder is .desktop files
// (remora-app-menu.sh does the file work); on Windows it is a Start-menu folder of shortcuts,
// written here. The generate/remove work runs through the injectable Spawner; the read-only entry
// scan is plain filesystem.
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

#include "core/Resolver.h"
#include "engine/Engine.h"

namespace remora {

// Must match slugify() in vendor/host-scripts/remora-app-menu.sh.
QString appMenuSlug(const QString &instance) {
    QString s = instance.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    s.remove(QRegularExpression(QStringLiteral("^-|-$")));
    return s;
}

static QString dataHome() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
}

// "<pkg>=desktop|freeform" list helpers — the stored answer of the first-launch prompt.
QString appModeFor(const RemoraConfig &cfg, const QString &pkg) {
    const QString prefix = pkg + QLatin1Char('=');
    for (const QString &m : cfg.integration.appModes)
        if (m.startsWith(prefix)) return m.mid(prefix.size());
    return {};
}

void setAppMode(RemoraConfig &cfg, const QString &pkg, const QString &mode) {
    const QString prefix = pkg + QLatin1Char('=');
    QStringList &modes = cfg.integration.appModes;
    for (int i = modes.size() - 1; i >= 0; --i)
        if (modes[i].startsWith(prefix)) modes.removeAt(i);
    if (!mode.isEmpty()) modes << prefix + mode;
}

// ---- arm64 pins ----------------------------------------------------------
// An app with no working x86_64 build is installed as arm64 and runs under the native bridge.
// Play does not know that: it sees an x86_64 device and re-delivers the x86_64 split at the next
// update, which puts the app back to whatever was broken about it (TikTok: dies at spawn;
// Flutter apps: the split omits libflutter.so). Pinning records "this one is arm64 on purpose"
// so the guard can undo that silently, instead of the user rediscovering it as a crash.

// Same file-static as Spawner.cpp/Chain.cpp keep: the probe script below is full of quotes and
// '$', so it cannot ride the plain "sh -c \"%1\"" interpolation those files warn about.
static QString shSingleQuote(const QString &s) {
    return QLatin1Char('\'') + QString(s).replace(QLatin1String("'"), QLatin1String("'\\''"))
           + QLatin1Char('\'');
}

bool isArmPinned(const RemoraConfig &cfg, const QString &pkg) {
    return cfg.integration.armApps.contains(pkg);
}

void setArmPinned(RemoraConfig &cfg, const QString &pkg, bool pinned) {
    QStringList &pins = cfg.integration.armApps;
    pins.removeAll(pkg);
    if (pinned) pins << pkg;
}

QStringList armPinsNeedingRepair(Spawner &sp, const RemoraConfig &cfg, Backend backend) {
    const QStringList &pins = cfg.integration.armApps;
    if (pins.isEmpty()) return {};
    const ResolvedConfig rc = resolve(cfg, backend);
    // Ask the device in ONE exec: for each pin, print the package iff it is installed and its
    // primaryCpuAbi is no longer arm64. A package that is not installed at all is NOT a repair
    // candidate — reinstalling something the user removed would be worse than the crash.
    QString script;
    for (const QString &pkg : pins) {
        // Pins come from package names the device itself reported; guard anyway, since a
        // hand-edited remorarc reaches this shell.
        static const QRegularExpression kPkg(QStringLiteral("^[A-Za-z0-9_.]+$"));
        if (!kPkg.match(pkg).hasMatch()) continue;
        script += QStringLiteral("d=$(dumpsys package %1 2>/dev/null); "
                                 "case \"$d\" in *primaryCpuAbi*) "
                                 "echo \"$d\" | grep -q 'primaryCpuAbi=arm64-v8a' || echo %1;; "
                                 "esac; ")
                      .arg(pkg);
    }
    if (script.isEmpty()) return {};
    const QString c = rc.containerName;
    const ProcResult r =
        backend == Backend::Bare
            ? sp.run({"docker", "exec", c, "sh", "-c", script}, {}, {})
            : sp.run(Spawner::sshArgv(rc.sshHost.value_or(QString()),
                                      QStringLiteral("docker exec %1 sh -c %2")
                                          .arg(c, shSingleQuote(script))),
                     {}, {});
    if (r.rc != 0) return {};  // device down / docker gone — nothing to repair right now
    QStringList out;
    for (const QString &l : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString p = l.trimmed();
        if (pins.contains(p)) out << p;
    }
    return out;
}

// ---- the Windows Start menu ----------------------------------------------
// The folder holds shortcuts, and the icons they point at are .ico files Remora converts from the
// agent's renders. Both are kept apart from anything a user or another program put there: the
// icons and a manifest live in Remora's own data directory, and the manifest names the shortcut
// files Remora wrote. A refresh replaces those and a removal deletes those, so a Start-menu folder
// that happens to share a profile's name keeps everything that is not Remora's.

static QString menuStateRoot() { return dataHome() + QStringLiteral("/remora/menu"); }

QString startMenuRoot() {
    // Qt's test mode moves the data locations under qttest but, on Windows, not this one: it
    // stayed the real Start menu on the test VM, and a suite would have written into it.
    if (QStandardPaths::isTestModeEnabled()) return dataHome() + QStringLiteral("/remora/start-menu");
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
}
static QString menuStateDir(const QString &slug) { return menuStateRoot() + QLatin1Char('/') + slug; }

struct MenuFile {
    QString file;  // in the manifest's folder
    QString pkg;   // empty for the device entries (mirror, desktop, PIP)
    QString name;
};
struct MenuManifest {
    QString profile, folder;
    QVector<MenuFile> files;
};

static MenuManifest readManifest(const QString &slug) {
    QFile f(menuStateDir(slug) + QStringLiteral("/entries.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    MenuManifest m{o.value(QStringLiteral("profile")).toString(),
                   o.value(QStringLiteral("folder")).toString(), {}};
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray()) {
        const QJsonObject e = v.toObject();
        const MenuFile mf{e.value(QStringLiteral("file")).toString(),
                          e.value(QStringLiteral("pkg")).toString(),
                          e.value(QStringLiteral("name")).toString()};
        // A file name is all a removal goes by, so one that is not a plain name in the folder
        // (a hand-edited "..\\x") is not Remora's to delete.
        if (!mf.file.isEmpty() && windowsFileName(mf.file) == mf.file) m.files << mf;
    }
    return m;
}

static bool writeManifest(const QString &slug, const MenuManifest &m) {
    QJsonArray files;
    for (const MenuFile &f : m.files)
        files.append(QJsonObject{{QStringLiteral("file"), f.file},
                                 {QStringLiteral("pkg"), f.pkg},
                                 {QStringLiteral("name"), f.name}});
    const QJsonObject o{{QStringLiteral("profile"), m.profile},
                        {QStringLiteral("folder"), m.folder},
                        {QStringLiteral("files"), files}};
    QDir().mkpath(menuStateDir(slug));
    QSaveFile f(menuStateDir(slug) + QStringLiteral("/entries.json"));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson());
    return f.commit();
}

QString appMenuIconForAppId(const QString &appId) {
    // remora-<slug>-<pkg>: a package name has no '-', so the last one ends the slug.
    static const QString kPrefix = QStringLiteral("remora-");
    if (!appId.startsWith(kPrefix)) return {};
    const QString rest = appId.mid(kPrefix.size());
    const qsizetype dash = rest.lastIndexOf(QLatin1Char('-'));
    if (dash <= 0) return {};
    const QString ico = menuStateDir(rest.left(dash)) + QLatin1Char('/') + rest.mid(dash + 1) +
                        QStringLiteral(".ico");
    return QFileInfo::exists(ico) ? ico : QString();
}

QVector<AppMenuEntry> appMenuEntries(const QString &slug) {
    QVector<AppMenuEntry> out;
    // A Windows menu's apps, from its manifest. Read on every platform: a Linux machine has no
    // manifest, and the .desktop scan below finds nothing on Windows.
    QSet<QString> seen;
    for (const MenuFile &f : readManifest(slug).files)
        if (!f.pkg.isEmpty() && !seen.contains(f.pkg)) {
            seen.insert(f.pkg);
            out << AppMenuEntry{f.pkg, f.name};
        }
    const QDir apps(dataHome() + QStringLiteral("/applications"));
    // ONE ARTIFACT SCHEME (bd remora-28ix.4 step 4). This used to scan the pre-rename
    // names too; the next refresh regenerated under remora-* and swept the old names, so the window
    // was always meant to close, and it has. A leftover pre-rename artifact is now invisible here
    // and has to be deleted by hand from ~/.local/share/{applications,desktop-directories}.
    const QString cat = QStringLiteral("Categories=X-Remora-") + slug + QLatin1Char(';');
    const QStringList files = apps.entryList({QStringLiteral("remora-*.desktop")}, QDir::Files);
    for (const QString &f : files) {
        QFile af(apps.filePath(f));
        if (!af.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString body = QString::fromUtf8(af.readAll());
        if (!body.contains(QLatin1Char('\n') + cat + QLatin1Char('\n'))) continue;
        AppMenuEntry e;
        for (const QString &line : body.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("Name=")) && e.name.isEmpty())
                e.name = line.mid(5);
            else if (line.startsWith(QStringLiteral("X-Remora-App=")))
                e.pkg = line.mid(int(qstrlen("X-Remora-App=")));
        }
        // app entries only (mirror/desktop links carry no pkg)
        if (!e.pkg.isEmpty() && !seen.contains(e.pkg)) out << e;
    }
    std::sort(out.begin(), out.end(),
              [](const AppMenuEntry &a, const AppMenuEntry &b) { return a.name < b.name; });
    return out;
}

// The program a Windows entry starts: remora-gui.exe, the Windows-subsystem build, so launching an
// app never opens a console. This program when there is none beside it (a test, a dev tree).
static QString windowsEntryProgram() {
    const QString gui = QCoreApplication::applicationDirPath() + QStringLiteral("/remora-gui.exe");
    return QFileInfo::exists(gui) ? gui : QCoreApplication::applicationFilePath();
}

// The agent's IconDump renders every icon on the device in one app_process pass, which is what
// composites adaptive and vector icons correctly; each PNG becomes the .ico a shortcut needs. As
// in remora-app-menu.sh, nothing on disk is replaced until a render has produced something, and
// only an app the device no longer lists loses its icon: an unlucky moment mid-boot must not
// genericise a menu that was fine. Returns how many icons were converted.
static int renderStartMenuIcons(Spawner &sp, const QString &target, const QStringList &pkgs,
                                const QString &stateDir, LogSink &sink, const QString &step) {
    const QString dev = QStringLiteral("/data/local/tmp/remora-icons");
    sp.run({QStringLiteral("adb"), QStringLiteral("-s"), target, QStringLiteral("shell"),
            QStringLiteral("mkdir -p %1; rm -f %1/*.png").arg(dev)},
           {}, {});
    const ProcResult r = sp.run(
        {QStringLiteral("adb"), QStringLiteral("-s"), target, QStringLiteral("shell"),
         QStringLiteral("CLASSPATH=/system_ext/framework/remora-agent.jar app_process /system/bin "
                        "com.remora.agent.IconDump %1 %2")
             .arg(dev, pkgs.join(QLatin1Char(' ')))},
        {}, {});
    int rendered = 0;
    for (const QString &l : r.out.split(QLatin1Char('\n')))
        if (l.startsWith(QStringLiteral("OK "))) ++rendered;
    int converted = 0;
    if (rendered > 0) {
        // `adb pull <dir> <dest>` nests or not depending on whether dest exists, so the stage
        // starts empty and the PNGs are found by search, whichever layout it was.
        const QString pull = stateDir + QStringLiteral("/pull");
        QDir(pull).removeRecursively();
        sp.run({QStringLiteral("adb"), QStringLiteral("-s"), target, QStringLiteral("pull"), dev,
                pull},
               {}, {});
        QDirIterator it(pull, {QStringLiteral("*.png")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo png(it.next());
            QFile in(png.filePath());
            if (!pkgs.contains(png.completeBaseName()) || !in.open(QIODevice::ReadOnly)) continue;
            const QByteArray ico = icoFromPng(in.readAll());
            if (ico.isEmpty()) continue;  // a failed render, not an icon
            QSaveFile out(stateDir + QLatin1Char('/') + png.completeBaseName() +
                          QStringLiteral(".ico"));
            if (out.open(QIODevice::WriteOnly) && out.write(ico) == ico.size() && out.commit())
                ++converted;
        }
        QDir(pull).removeRecursively();
    }
    if (converted > 0) {
        QDir icons(stateDir);
        for (const QString &f : icons.entryList({QStringLiteral("*.ico")}, QDir::Files))
            if (!pkgs.contains(QFileInfo(f).completeBaseName())) icons.remove(f);
        sink.line(step, QStringLiteral("stdout"),
                  QStringLiteral("  rendered %1 icon(s)").arg(converted));
    } else {
        sink.line(step, QStringLiteral("stdout"),
                  QStringLiteral("  (icon render produced nothing — keeping the icons already on "
                                 "disk. Either the device is mid-boot, or its image predates "
                                 "IconDump in the agent. Entries without one use Remora's.)"));
    }
    return converted;
}

// The same folder remora-app-menu.sh writes, as Start-menu shortcuts: named after the profile, one
// entry per app under its real icon, and the device's three entries. Each app entry carries the
// window id the app's mirror runs under as its AppUserModelID, so the taskbar shows the app's
// name and icon, as StartupWMClass makes a Linux one do.
static bool startMenuSync(Spawner &sp, const RemoraConfig &cfg, Backend backend,
                          const QString &instance, LogSink &sink) {
    const QString step = QStringLiteral("app-menu");
    const QString slug = appMenuSlug(instance);
    if (slug.isEmpty()) {
        sink.state(step, StepState::Failed,
                   QStringLiteral("profile name '%1' yields an empty slug").arg(instance));
        return false;
    }
    sink.state(step, StepState::Running,
               QStringLiteral("refreshing the '%1' Start-menu folder").arg(instance));
    const MenuManifest old = readManifest(slug);
    const QString stateDir = menuStateDir(slug);
    MenuManifest next{instance, startMenuRoot() + QLatin1Char('/') + windowsFileName(instance), {}};
    int oldApps = 0;
    for (const MenuFile &f : old.files)
        if (!f.pkg.isEmpty()) ++oldApps;

    const RunContext ctx = makeContext(cfg, backend, std::nullopt, QString());
    const QString target = resolveAdbTarget(sp, ctx);
    QVector<ListedApp> apps;
    if (!target.isEmpty()) {
        sp.run({QStringLiteral("adb"), QStringLiteral("connect"), target}, {}, {});
        const auto enumerate = [&] {
            apps = parseListApps(sp.run({QCoreApplication::applicationFilePath(),
                                         QStringLiteral("mirror"), QStringLiteral("-s"), target,
                                         QStringLiteral("--list-apps")},
                                        {}, {})
                                     .out);
        };
        enumerate();
        // remora-app-menu.sh's guards, for the same reasons. A device that lists no apps at all
        // right after its boot may still be settling (an emulated guest did, for several
        // seconds), so a first menu waits as a refresh does before calling it a failure; a list
        // that shrank below half the menu is the same condition, retried for a while and then
        // left alone (kept, not failed: bd remora-5sd).
        for (int tries = 0; apps.isEmpty() && oldApps <= 3 && tries < 8; ++tries) {
            sink.line(step, QStringLiteral("stdout"),
                      QStringLiteral("  device lists no apps yet (still booting?) — retrying…"));
            sp.waitMs(8000);
            enumerate();
        }
        if (apps.isEmpty() && oldApps <= 3) {
            sink.state(step, StepState::Failed,
                       QStringLiteral("no apps found on %1 (is the device connected?)").arg(target));
            return false;
        }
        for (int tries = 0; oldApps > 3 && apps.size() * 2 < oldApps && tries < 8; ++tries) {
            sink.line(step, QStringLiteral("stdout"),
                      QStringLiteral("  device reports only %1 of %2 apps (still booting?) — "
                                     "retrying…")
                          .arg(apps.size())
                          .arg(oldApps));
            sp.waitMs(8000);
            enumerate();
        }
        if (oldApps > 3 && apps.size() * 2 < oldApps) {
            sink.state(step, StepState::Skipped,
                       QStringLiteral("device still settling — kept the existing '%1' menu (it "
                                      "refreshes on the next Apps visit)")
                           .arg(instance));
            return true;
        }
    }

    // A package name reaches the device's shell and a file name, so only a plain one is used.
    static const QRegularExpression kPkg(QStringLiteral("^[A-Za-z0-9_.]+$"));
    QStringList pkgs;
    QVector<ListedApp> usable;
    for (const ListedApp &a : apps)
        if (kPkg.match(a.pkg).hasMatch() && !pkgs.contains(a.pkg)) {
            pkgs << a.pkg;
            usable << a;
        }
    if (!target.isEmpty()) {
        QDir().mkpath(stateDir);
        sink.line(step, QStringLiteral("stdout"),
                  QStringLiteral("Found %1 apps. Rendering icons on device…").arg(usable.size()));
        renderStartMenuIcons(sp, target, pkgs, stateDir, sink, step);
    }

    const QString program = windowsEntryProgram();
    const QString workDir = QFileInfo(program).absolutePath();
    QSet<QString> taken;  // file names in the folder, which Windows compares without case
    QVector<Shortcut> links;
    const auto add = [&](const QString &name, const QString &pkg, const QStringList &args,
                         const QString &description, const QString &icon, const QString &appId) {
        QString file = windowsFileName(name);
        if (taken.contains(file.toLower()) && !pkg.isEmpty())
            file = windowsFileName(QStringLiteral("%1 (%2)").arg(name, pkg));
        if (taken.contains(file.toLower())) return;
        taken.insert(file.toLower());
        file += QStringLiteral(".lnk");
        links << Shortcut{next.folder + QLatin1Char('/') + file, program, windowsArgs(args),
                          workDir, icon, description, appId};
        next.files << MenuFile{file, pkg, name};
    };
    add(QStringLiteral("Android (mirror)"), {}, {QStringLiteral("up"), instance},
        QStringLiteral("%1 — mirror the main display (no specific app)").arg(instance), {}, {});
    add(QStringLiteral("Android (desktop)"), {}, {QStringLiteral("desktop"), instance},
        QStringLiteral("%1 — a separate desktop-mode display (no specific app)").arg(instance), {},
        {});
    add(QStringLiteral("Android (PIP)"), {}, {QStringLiteral("pip"), instance},
        QStringLiteral("%1 — toggle a small always-on-top second mirror").arg(instance), {}, {});
    for (const ListedApp &a : usable) {
        const QString ico = stateDir + QLatin1Char('/') + a.pkg + QStringLiteral(".ico");
        add(a.name, a.pkg,
            {QStringLiteral("app"), instance, a.pkg, QStringLiteral("--title"), a.name},
            QStringLiteral("%1 — %2 (%3)").arg(a.name, instance, a.pkg),
            QFileInfo::exists(ico) ? ico : QString(),
            QStringLiteral("remora-%1-%2").arg(slug, a.pkg));
    }
    // No device: the app list cannot be read, so the app entries already there stay as they are.
    // Only the device entries are rewritten; those are profile metadata and need no device, so a
    // profile that is not running still gets them (remora-app-menu.sh --plain, for the same reason).
    if (target.isEmpty())
        for (const MenuFile &f : old.files)
            if (!f.pkg.isEmpty() && old.folder == next.folder && !taken.contains(
                    QFileInfo(f.file).completeBaseName().toLower())) {
                taken.insert(QFileInfo(f.file).completeBaseName().toLower());
                next.files << f;
            }

    QDir().mkpath(next.folder);
    QString error;
    for (const Shortcut &l : links)
        if (const QString e = sp.writeShortcut(l); !e.isEmpty() && error.isEmpty()) error = e;
    // Remora's files from the last refresh that this one did not write again: uninstalled apps.
    QSet<QString> kept;
    for (const MenuFile &f : next.files) kept.insert(next.folder + QLatin1Char('/') + f.file);
    for (const MenuFile &f : old.files) {
        const QString path = old.folder + QLatin1Char('/') + f.file;
        if (!old.folder.isEmpty() && !kept.contains(path)) QFile::remove(path);
    }
    if (!old.folder.isEmpty() && old.folder != next.folder) QDir().rmdir(old.folder);
    if (!writeManifest(slug, next) && error.isEmpty())
        error = QStringLiteral("could not write %1/entries.json").arg(stateDir);
    if (!error.isEmpty()) {
        sink.state(step, StepState::Failed,
                   QStringLiteral("Start-menu refresh failed: %1").arg(error));
        return false;
    }
    sink.state(step, StepState::Ok,
               target.isEmpty()
                   ? QStringLiteral("'%1' is not running — refreshed its device entries; the app "
                                    "list needs the device")
                         .arg(instance)
                   : QStringLiteral("Start-menu folder '%1' refreshed (%2 apps)")
                         .arg(instance)
                         .arg(usable.size()));
    return true;
}

// Delete what the manifest says Remora wrote, then the folder if that left it empty, then
// Remora's own copy of the icons. Nothing runs: it is a handful of local files.
static bool startMenuRemoveSlug(const QString &slug, const QString &label, LogSink &sink) {
    const QString step = QStringLiteral("app-menu");
    const MenuManifest m = readManifest(slug);
    if (!m.folder.isEmpty()) {
        for (const MenuFile &f : m.files) QFile::remove(m.folder + QLatin1Char('/') + f.file);
        QDir().rmdir(m.folder);
    }
    QDir(menuStateDir(slug)).removeRecursively();
    sink.state(step, StepState::Ok,
               m.folder.isEmpty() ? QStringLiteral("'%1' has no Start-menu folder").arg(label)
                                  : QStringLiteral("Start-menu folder '%1' removed").arg(label));
    return true;
}

bool appMenuSync(Spawner &sp, const RemoraConfig &cfg, Backend backend, const QString &instance,
                 LogSink &sink) {
    const QString step = QStringLiteral("app-menu");
    if (sp.localOs() == LocalOs::Windows) return startMenuSync(sp, cfg, backend, instance, sink);
    sink.state(step, StepState::Running, QStringLiteral("refreshing the '%1' app-menu folder")
                                             .arg(instance));
    const RunContext ctx = makeContext(cfg, backend, std::nullopt, QString());
    const QString target = resolveAdbTarget(sp, ctx);
    // Each entry launches `<this-binary> app "<profile>" <pkg> --title "<Name>"` — quoted, profile
    // and app names have spaces. The folder also gets plain-device entries: mirror, desktop mode
    // and the PIP toggle — all three are per-profile, so each folder drives its OWN device.
    const QString self = QCoreApplication::applicationFilePath();
    const QString execTmpl =
        QStringLiteral("\"%1\" app \"%2\" %PKG% --title \"%NAME%\"").arg(self, instance);
    const QString execMirror = QStringLiteral("\"%1\" up \"%2\"").arg(self, instance);
    const QString execDesktop = QStringLiteral("\"%1\" desktop \"%2\"").arg(self, instance);
    const QString execPip = QStringLiteral("\"%1\" pip \"%2\"").arg(self, instance);
    const QString script =
        vendorScript(QStringLiteral("host-scripts"), QStringLiteral("remora-app-menu.sh"));

    // No device: refresh the DEVICE-LEVEL entries anyway and leave the app list alone. Only the app
    // list needs adb — mirror/desktop/PIP are pure profile metadata. Bailing here meant a profile
    // got its buttons only while its container happened to be running, which is precisely when a
    // launcher entry is not what you reach for; with two profiles and one container at a time, the
    // idle profile could never be given a PIP entry at all.
    if (target.isEmpty()) {
        const ProcResult p = sp.run(
            {QStringLiteral("bash"), script, QStringLiteral("--plain"), instance, execMirror,
             execDesktop, execPip},
            {}, [&](const QString &stream, const QString &text) { sink.line(step, stream, text); });
        sink.state(step, p.rc == 0 ? StepState::Ok : StepState::Failed,
                   p.rc == 0 ? QStringLiteral("'%1' is not running — refreshed its device entries; "
                                              "the app list needs the device")
                                   .arg(instance)
                             : QStringLiteral("could not resolve the device's adb address"),
                   p.stderrTail);
        return p.rc == 0;
    }
    sp.run({QStringLiteral("adb"), QStringLiteral("connect"), target}, {}, {});
    QMap<QString, QString> env;
    // The fallback renderer, for an image whose agent predates IconDump; absent in a public
    // checkout, where the script says so and the entries stay generic.
    env.insert(QStringLiteral("ICONDUMP_DEX"),
               vendorScript(QStringLiteral("host-scripts"), QStringLiteral("icondump.dex")));
    // The script's app enumeration runs the in-house client (remora mirror --list-apps): hand it
    // the binary this engine IS. Nothing is pushed to the device for it — the in-image agent
    // answers.
    env.insert(QStringLiteral("REMORA_BIN"), self);
    const ProcResult r = sp.run(
        {QStringLiteral("bash"), script, target, execTmpl, instance, execMirror, execDesktop,
         execPip},
        env, [&](const QString &stream, const QString &text) { sink.line(step, stream, text); });
    // rc 3 is the script's boot-settling guard KEEPING the existing menu because the device is
    // not answering with a full app list yet — its designed outcome on a cold deploy, where this
    // step runs moments after boot. Reported as a skip with the reason, not a failure: a step
    // that fails on every single run trains the eye to ignore step states (bd remora-5sd).
    const bool kept = r.rc == 3;
    sink.state(step,
               r.rc == 0 ? StepState::Ok : (kept ? StepState::Skipped : StepState::Failed),
               r.rc == 0
                   ? QStringLiteral("menu folder '%1' refreshed").arg(instance)
                   : (kept ? QStringLiteral("device still settling — kept the existing '%1' "
                                            "menu (it refreshes on the next Apps visit)")
                                 .arg(instance)
                           : QStringLiteral("menu refresh failed (rc %1)").arg(r.rc)),
               r.stderrTail);
    return r.rc == 0 || kept;
}

bool appMenuRemove(Spawner &sp, const QString &instance, LogSink &sink) {
    const QString step = QStringLiteral("app-menu");
    if (sp.localOs() == LocalOs::Windows) {
        const QString slug = appMenuSlug(instance);
        return slug.isEmpty() || startMenuRemoveSlug(slug, instance, sink);
    }
    const ProcResult r = sp.run(
        {QStringLiteral("bash"),
         vendorScript(QStringLiteral("host-scripts"), QStringLiteral("remora-app-menu.sh")),
         QStringLiteral("--remove"), instance},
        {}, [&](const QString &stream, const QString &text) { sink.line(step, stream, text); });
    sink.state(step, r.rc == 0 ? StepState::Ok : StepState::Failed,
               r.rc == 0 ? QStringLiteral("menu folder '%1' removed").arg(instance)
                         : QStringLiteral("menu removal failed (rc %1)").arg(r.rc),
               r.stderrTail);
    return r.rc == 0;
}

// Every menu folder Remora has written, a profile that no longer exists included: what an
// uninstall takes with it. A folder is found by what Remora left beside it (the manifest on
// Windows, the .directory entry on Linux), so nothing Remora did not write is involved.
bool appMenuRemoveAll(Spawner &sp, LogSink &sink) {
    bool ok = true;
    if (sp.localOs() == LocalOs::Windows) {
        const QDir root(menuStateRoot());
        for (const QString &slug : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString profile = readManifest(slug).profile;
            ok = startMenuRemoveSlug(slug, profile.isEmpty() ? slug : profile, sink) && ok;
        }
        return ok;
    }
    const QDir dirs(dataHome() + QStringLiteral("/desktop-directories"));
    for (const QString &f : dirs.entryList({QStringLiteral("remora-*.directory")}, QDir::Files)) {
        // remora-<slug>.directory; the script slugifies what it is given, and a slug is its own.
        const QString slug = f.mid(7, f.size() - 7 - int(qstrlen(".directory")));
        ok = appMenuRemove(sp, slug, sink) && ok;
    }
    return ok;
}

}  // namespace remora
