#include "core/Gating.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>

#include "core/Features.h"
#include "core/SourcePatches.h"

namespace remora {

std::optional<PifProfileAge> stalePifProfile(const QString &pifJson, const QDate &today,
                                             int maxMonths) {
    if (pifJson.trimmed().isEmpty()) return std::nullopt;

    const QJsonObject o = QJsonDocument::fromJson(pifJson.toUtf8()).object();
    const QString patch = o.value(QStringLiteral("SECURITY_PATCH")).toString();

    // PIF profiles carry SECURITY_PATCH as yyyy-MM-dd. Anything else — absent, malformed, or a
    // JSON parse failure leaving an empty object — is undatable rather than fresh.
    const QDate d = QDate::fromString(patch, QStringLiteral("yyyy-MM-dd"));
    if (!d.isValid()) return PifProfileAge{patch, -1};

    // Whole months, so a profile is "3 months old" for the whole of its fourth month rather than
    // flipping on a day boundary. daysTo/30 would drift a month every five years.
    int months = (today.year() - d.year()) * 12 + (today.month() - d.month());
    if (today.day() < d.day()) --months;
    if (months < maxMonths) return std::nullopt;
    return PifProfileAge{patch, months};
}

QSet<QString> normalize(const QSet<QString> &requested) {
    QSet<QString> out = requested;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const QString &key : QList<QString>(out.begin(), out.end())) {
            const Feature *f = findFeature(key);
            if (!f) continue;
            for (const QString &dep : f->requires_) {
                if (!out.contains(dep)) {
                    out.insert(dep);
                    changed = true;
                }
            }
        }
    }
    return out;
}

QVector<Gate> featureGates(const QSet<QString> &requested) {
    QVector<Gate> gates;
    QList<QString> keys(requested.begin(), requested.end());
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys) {
        const Feature *f = findFeature(key);
        if (!f) {
            gates.append({key, false, QStringLiteral("unknown feature"), QStringLiteral("block")});
            continue;
        }
        for (const QString &dep : f->requires_)
            if (!requested.contains(dep))
                gates.append({key, false, QStringLiteral("requires %1").arg(dep),
                              QStringLiteral("warn")});
        for (const QString &other : f->conflicts)
            if (requested.contains(other))
                gates.append({key, false, QStringLiteral("conflicts with %1").arg(other),
                              QStringLiteral("block")});
        switch (f->status) {
            case FeatureStatus::Parked:
                gates.append({key, false, QStringLiteral("parked/broken — %1").arg(f->note),
                              QStringLiteral("block")});
                break;
            case FeatureStatus::Deferred:
                gates.append({key, false, QStringLiteral("deferred/unbuilt — %1").arg(f->note),
                              QStringLiteral("warn")});
                break;
            case FeatureStatus::Caveat:
                gates.append({key, true, f->note, QStringLiteral("info")});
                break;
            default:
                break;
        }
    }
    return gates;
}

QVector<FeaturePatchGap> featurePatchGaps(const QSet<QString> &enabledFeatures,
                                          const QStringList &enabledPatches, int androidVersion) {
    // Resolve both "unset" cases before comparing — see the header for why an empty patch list
    // must NOT short-circuit to "nothing missing" the way unappliedDefaultPatches does.
    const QSet<QString> feats =
        enabledFeatures.isEmpty() ? defaultBuildSet(androidVersion) : enabledFeatures;
    const QStringList patches =
        enabledPatches.isEmpty() ? defaultSourcePatchSet(androidVersion) : enabledPatches;
    const QSet<QString> have(patches.begin(), patches.end());

    QVector<FeaturePatchGap> gaps;  // registry order, so the report reads like the feature list
    for (const Feature &f : buildFeatures()) {
        if (!feats.contains(f.key)) continue;
        QStringList missing;
        for (const QString &p : f.requiresPatches)
            if (!have.contains(p)) missing << p;
        if (!missing.isEmpty()) gaps.append({f.key, missing});
    }
    return gaps;
}

QVector<FeatureVersionGap> featureVersionGaps(const QSet<QString> &enabledFeatures,
                                              int androidVersion, const VendorReader &read) {
    const QSet<QString> feats =
        enabledFeatures.isEmpty() ? defaultBuildSet(androidVersion) : enabledFeatures;
    // The applicable set is DERIVED from the assets, once, then membership-tested — the same set
    // the GUI filters patch rows by and the apply loop resolves, so "offered" and "buildable"
    // cannot drift apart (bd remora-82c.3).
    const QStringList applicableList = applicableSourcePatchKeys(androidVersion, read);
    const QSet<QString> applicable(applicableList.begin(), applicableList.end());
    QVector<FeatureVersionGap> gaps;  // registry order, so the report reads like the feature list
    for (const Feature &f : buildFeatures()) {
        if (!feats.contains(f.key)) continue;
        QStringList bad;
        for (const QString &pk : f.requiresPatches)
            if (!applicable.contains(pk)) bad << pk;
        if (!bad.isEmpty()) gaps.append({f.key, bad});
    }
    return gaps;
}

QMap<QString, QString> featureRequiredPatchOwners(const QSet<QString> &enabledFeatures) {
    const QSet<QString> feats = enabledFeatures.isEmpty() ? defaultBuildSet() : enabledFeatures;
    QMap<QString, QString> owners;
    for (const Feature &f : buildFeatures()) {
        if (!feats.contains(f.key)) continue;
        for (const QString &p : f.requiresPatches)
            owners.insert(p, owners.contains(p) ? owners.value(p) + QStringLiteral(", ") + f.key
                                                : f.key);
    }
    return owners;
}

QMap<QString, QString> irrelevantPatches(const PatchProfile &profile) {
    const QSet<QString> have(profile.enabledPatches.begin(), profile.enabledPatches.end());
    QMap<QString, QString> out;
    for (const SourcePatch &p : sourcePatches()) {
        const PatchNeeds &n = p.needs;
        // First unmet need wins. One reason is what a row has space to say, and the order runs
        // broadest-first so the answer names the largest thing that is wrong: on an AOSP tree the
        // useful sentence is "no Lineage projects", not "the backport is off" for an entry whose
        // backport could not apply there either.
        if (n.lineageTree && !profile.lineageTree)
            out.insert(p.key, QStringLiteral("the source kind is AOSP, which never syncs the "
                                             "LineageOS projects this patches"));
        else if (n.venus && !profile.venus)
            out.insert(p.key, QStringLiteral("the Venus/NVIDIA path is not configured, and its "
                                             "gralloc cannot allocate without a vtest server"));
        else if (!n.feature.isEmpty() && !profile.features.contains(n.feature))
            out.insert(p.key, QStringLiteral("the '%1' build feature is off").arg(n.feature));
        else if (!n.patch.isEmpty() && !have.contains(n.patch))
            out.insert(p.key, QStringLiteral("it is inert without the '%1' patch").arg(n.patch));
    }
    return out;
}

ImageSelection selectImage(const QSet<QString> &requested, int androidVersion) {
    const QSet<QString> want = normalize(requested);
    const ImageTag *best = nullptr;
    QSet<QString> bMiss;
    int bMissN = 1 << 30, bExtraN = 1 << 30;

    // Fewest missing features wins; ties go to the tag carrying the fewest unrequested extras.
    for (const ImageTag &tag : imageTags()) {
        if (tag.android != androidVersion) continue;
        const QSet<QString> miss = want - tag.provides;
        const int extraN = int((tag.provides - want).size());
        if (!best || miss.size() < bMissN || (miss.size() == bMissN && extraN < bExtraN)) {
            best = &tag;
            bMiss = miss;
            bMissN = miss.size();
            bExtraN = extraN;
        }
    }

    if (!best)  // no pre-built image for this Android version
        return {QStringLiteral("(no pre-built image for A%1)").arg(androidVersion), want, true};
    return {best->tag, bMiss, !bMiss.isEmpty()};
}

bool isPlaceholderImageTag(const QString &tag) {
    return tag.startsWith(QLatin1Char('('));
}

}  // namespace remora
