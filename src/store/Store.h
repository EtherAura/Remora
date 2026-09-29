#pragma once
#include <QString>
#include <QStringList>

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

}  // namespace remora
