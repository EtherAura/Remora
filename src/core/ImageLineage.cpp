#include "core/ImageLineage.h"

#include <QHash>
#include <QReadWriteLock>
#include <algorithm>

#include "core/Features.h"
#include "core/Resolved.h"

namespace remora {

namespace {
QReadWriteLock gLock;
QHash<QString, QString> gRoots;  // dockerHost '\n' tag -> registered root ("" = none)

QString key(const QString &dockerHost, const QString &tag) {
    return dockerHost + QLatin1Char('\n') + tag;
}
}  // namespace

QString imageHostKey(const ResolvedConfig &r) {
    return r.backend == Backend::Bare ? QString() : r.sshHost.value_or(QString()).trimmed();
}

void recordImageRoot(const QString &dockerHost, const QString &tag, const QString &rootTag) {
    QWriteLocker l(&gLock);
    gRoots.insert(key(dockerHost, tag), rootTag);
}

std::optional<QString> recordedImageRoot(const QString &dockerHost, const QString &tag) {
    QReadLocker l(&gLock);
    const auto it = gRoots.constFind(key(dockerHost, tag));
    if (it == gRoots.constEnd()) return std::nullopt;
    return *it;
}

void forgetImageRoots() {
    QWriteLocker l(&gLock);
    gRoots.clear();
}

QString registeredTagAmong(const QStringList &tags) {
    for (const QString &t : tags)
        if (findImageTag(t)) return t;
    QString best;
    for (const QString &t : tags)
        for (const ImageTag &r : imageTags())
            if (r.tag.size() > best.size() && t.startsWith(r.tag + QLatin1Char('-')))
                best = r.tag;
    return best;
}

QString parseImageRootProbe(const QString &out, const QString &pin) {
    QStringList rootTags, layerLines;
    QStringList *section = nullptr;
    for (const QString &raw : out.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("ROOT")) section = &rootTags;
        else if (line == QLatin1String("LAYERS")) section = &layerLines;
        else if (!line.isEmpty() && section) *section << line;
    }
    if (const QString root = registeredTagAmong(rootTags); !root.isEmpty()) return root;

    struct Image { QStringList layers, tags; };
    QList<Image> images;
    for (const QString &l : layerLines) {
        const QString layers = l.section(QLatin1Char(' '), 0, 0);
        const QString tags = l.section(QLatin1Char(' '), 1);
        images.append({layers.split(QLatin1Char(','), Qt::SkipEmptyParts),
                       tags.split(QLatin1Char(','), Qt::SkipEmptyParts)});
    }
    // docker lists a name given without a tag as name:latest.
    const QString named = pin.section(QLatin1Char('/'), -1).contains(QLatin1Char(':'))
                              ? pin : pin + QStringLiteral(":latest");
    const auto self = std::find_if(images.cbegin(), images.cend(),
                                   [&](const Image &i) { return i.tags.contains(named); });
    if (self == images.cend() || self->layers.isEmpty()) return QString();
    // Exact names only from here on: layers are evidence, and the suffix rule is a guess from a
    // name, which is all the label path has left once a root's plain tag has moved on.
    const auto exact = [](const QStringList &tags) {
        for (const QString &t : tags)
            if (findImageTag(t)) return t;
        return QString();
    };
    // The pin under a registered name as well is that image, retagged.
    if (const QString same = exact(self->tags); !same.isEmpty()) return same;
    QString best;
    qsizetype bestDepth = 0;
    for (const Image &i : images) {
        const qsizetype n = i.layers.size();
        if (n == 0 || n > self->layers.size() || n <= bestDepth) continue;
        if (self->layers.mid(0, n) != i.layers) continue;
        if (const QString t = exact(i.tags); !t.isEmpty()) {
            best = t;
            bestDepth = n;
        }
    }
    return best;
}

}  // namespace remora
