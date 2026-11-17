#pragma once
#include <QString>
#include <QStringList>

#include <functional>

#include "core/Config.h"

namespace remora {

// remorarc — an INI store (~/.config/remorarc), one [Instance-<name>] group per instance plus a
// [Defaults] group. Layered read: dataclass defaults < [Defaults] < [Instance-<name>].
QString defaultRemorarcPath();
QStringList listInstances(const QString &path);
QString activeInstance(const QString &path, const QString &fallback = QStringLiteral("default"));
void setActiveInstance(const QString &path, const QString &name);
RemoraConfig loadInstance(const QString &path, const QString &name);
void saveInstance(const QString &path, const QString &name, const RemoraConfig &cfg);
void deleteInstance(const QString &path, const QString &name);

// A user-configured image source — there are no built-ins any more (bd remora-2ylt): the
// upstream catalogs are retired, and self-built images need no source entry because the library
// always lists the build host's archives and docker images unconditionally.
struct ImageSource {
    QString name;
    bool enabled = true;
    QStringList tags;  // pinned tags (optional when a location is set)
    // Where this source's images LIVE (custom sources; empty = the source is just a tag list).
    // Three shapes, told apart by inspection: "http(s)://…" is a published-image mirror serving
    // <url>/index.txt — one image per line, '<docker-tag> [<archive-file> [<bytes>]]', '#'
    // comments (the contract bd remora-2ylt froze so the mirror can be built against it);
    // "user@host:/dir" is an ssh host directory of docker-save archives; a bare "/dir" is the
    // same directory locally. The image library lists every enabled location's images and marks
    // each LOCAL or NEEDS FETCH.
    QString location;
};
QList<ImageSource> loadImageSources(const QString &path);
void saveImageSources(const QString &path, const QList<ImageSource> &sources);

// A deployment = a profile summoned on a specific docker host (the Run page orchestrates these).
// Persisted as [Deployment-<name>]; multiple deployments may reuse one profile on different hosts.
struct Deployment {
    QString name;
    QString profile;  // the [Instance-<profile>] it deploys
    QString host;     // docker/deploy host override (blank = the profile's own ssh_host)
    QString image;    // image tag override (blank = the profile's resolved image)
};
QList<Deployment> loadDeployments(const QString &path);
void saveDeployments(const QString &path, const QList<Deployment> &deployments);

// The systemd --user units `remora service` generates for a profile: remora-<n>,
// remora-updater-<n> and remora-autosleep-<n>, where <n> is the profile name with everything
// outside [A-Za-z0-9_.-] replaced by '-'. Removal works from these names alone, so it can still
// clean up a profile whose section is already gone.
QString serviceUnitInstance(const QString &profile);
QStringList serviceUnitNames(const QString &profile);
// Why `service <name> --remove` must not act on `name`, or empty when it may — the profile need
// not exist. Refused: a name with no usable unit name, one too long for a unit file, and one whose
// units would include a unit of a DIFFERENT existing profile (escaping and the unit families can
// collide: "a b" and "a-b" share units, and profile "updater-x" is x's watcher).
QString serviceRemovalRefusal(const QString &name, const QStringList &knownProfiles);

// The systemctl call behind removal, injected so the store suite runs it against a fake manager.
struct UnitCommandResult {
    int rc = 0;
    QString out;
    QString err;
};
using UnitCommandRunner = std::function<UnitCommandResult(const QStringList &argv)>;
struct UnitRemovalResult {
    bool ok = true;
    QStringList removed;  // paths deleted
    QStringList errors;   // one line per unit that was kept, plus a failed reload
    bool noServiceManager = false;  // there was no systemctl to ask; see kNoServiceManagerRc
};
// The runner's rc for "systemctl could not be started at all" — RealSpawner's, and the shell's,
// code for a missing program. systemctl itself never exits with it.
inline constexpr int kNoServiceManagerRc = 127;
// Stop + disable each of the profile's units, then delete its file and its <unit>.d drop-in
// directory from unitDir, then daemon-reload. A unit whose disable fails is deleted only when the
// manager explicitly reports it not-found and inactive (stopping a running orphan first); a
// missing local file proves nothing about what is loaded. Anything uncertain keeps that unit's
// files and fails. Symlinks are unlinked, never followed.
//
// No systemctl at all (Windows, or a Linux without systemd) is not uncertain: nothing can have
// loaded these units, so there is nothing to disable or reload, and their files — if a unit was
// ever written — are only files. They are deleted and the removal succeeds; before, every call
// failed, the unit state read "unknown" and Delete profile could never delete (bd remora-7oih.2).
UnitRemovalResult removeServiceUnits(const QString &unitDir, const QString &profile,
                                     const UnitCommandRunner &run);

// The workspace's Delete profile, in the order that leaves no unit behind: the same refusal as
// `service --remove` (against every profile in path), then removeServiceUnits, and only once that
// fully succeeded — reload included — beforeSectionDelete (the app-menu folder) and the profile's
// section. Any failure keeps the section so the deletion can be retried; units already removed
// by then stay removed, and error says so.
struct ProfileDeletionResult {
    bool deleted = false;
    QString error;  // for display; empty once deleted
    UnitRemovalResult units;
};
ProfileDeletionResult deleteProfileAndServices(
    const QString &path, const QString &unitDir, const QString &profile,
    const UnitCommandRunner &run, const std::function<void(const QString &)> &beforeSectionDelete);

}  // namespace remora
