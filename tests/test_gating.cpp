#include <QtTest>

#include "core/Features.h"
#include "core/Gating.h"
#include "core/ImageBuild.h"
#include "core/SourcePatches.h"
#include <QRegularExpression>

using namespace remora;

static bool hasGate(const QVector<Gate> &gates, const QString &key, const QString &sev) {
    for (const Gate &g : gates)
        if (g.key == key && g.severity == sev) return true;
    return false;
}

class TestGating : public QObject {
    Q_OBJECT
private slots:
    void normalizePullsDeps() {
        QCOMPARE(normalize({"gapps"}), QSet<QString>({"gapps", "arm_translate"}));
        QCOMPARE(normalize({"shamiko"}), QSet<QString>({"shamiko", "magisk_root", "play_spoof"}));
    }

    // features= is pinned exactly the way source_patches was: once a profile has written a
    // non-empty list, a feature registered later never joins it, the image is built without it and
    // nothing says so (bd remora-4ei.44 — the same hole that cost two builds on the patch side,
    // bd remora-4ei.38). Milder here, because a missing feature reads as an absent capability
    // rather than a build dying at 78%, but just as silent.
    void unenabledDefaultFeaturesAreReported() {
        const QSet<QString> defaults = defaultBuildSet();
        QVERIFY(defaults.contains(QStringLiteral("gapps")));
        // a profile pinned before gapps and magisk_root existed
        QSet<QString> pinned = defaults;
        pinned.remove(QStringLiteral("gapps"));
        pinned.remove(QStringLiteral("magisk_root"));
        const QStringList missed = unappliedDefaultFeatures(pinned);
        QCOMPARE(missed.size(), 2);
        QVERIFY(missed.contains(QStringLiteral("gapps")));
        QVERIFY(missed.contains(QStringLiteral("magisk_root")));
        // opt-in features are deliberately off and must never be reported — that noise is exactly
        // what would make the warning ignorable
        for (const char *k : {"microg", "widevine_l3", "hw_video_decode", "sensors", "shamiko"})
            QVERIFY2(!missed.contains(QLatin1String(k)),
                     qPrintable(QStringLiteral("opt-in feature reported: %1").arg(QLatin1String(k))));
        // the complete default set is complete by definition, and empty means "unset ⇒ defaults
        // apply" everywhere else in the codebase rather than a pin of nothing
        QVERIFY(unappliedDefaultFeatures(defaults).isEmpty());
        QVERIFY(unappliedDefaultFeatures({}).isEmpty());
    }

    // Why this warning is NOT version-scoped while the patch one is: no default-on feature is
    // release-specific across the releases Remora builds its own images for.
    // {16, 17} is deliberate, not laziness. The release registry covers 8..17, but only 16 and 17
    // have Remora images or source-build assets — so A8..A15 report arm_translate,
    // lineage_frameworks, magisk_root and webview_pairip_fix unbuildable purely for want of a base
    // to layer onto. That is expected, and
    // not the condition being guarded. Hardcoded per release like containerPatchSet, so that adding
    // A18 here is the moment someone re-evaluates whether the default set still holds for it.
    // If this fails, a default-on feature has become release-specific and needs the
    // SourcePatch::defaultOnFor treatment BEFORE it starts nagging the other profile about it.
    void everyDefaultFeatureAppliesToEveryRemoraRelease() {
        for (int v : {16, 17}) {
            const ImageRecipe r = buildRecipe(defaultBuildSet(v), v);
            QVERIFY2(r.unbuildable.isEmpty(),
                     qPrintable(QStringLiteral("A%1 cannot deliver default feature(s): %2 — a "
                                               "default-on feature has become release-specific")
                                    .arg(v)
                                    .arg(r.unbuildable.join(QStringLiteral(", ")))));
        }
    }

    void defaultsSelectGappsTag() {
        const ImageSelection s = selectImage(defaultBuildSet(16), 16);
        QCOMPARE(s.tag, QString("remora23:x86_64-gapps"));
        QVERIFY(!s.needsBuild);
    }

    // The flip (bd remora-28ix.3.4): mirror_agent is default-on for A17 alone, paired
    // with the A17 source tags advertising it — never unscoped, or every A16 profile would read
    // needsSourceBuild against golden images that carry no agent (the defaultsSelectGappsTag
    // shape). This is the defaultOnFor treatment the registry comment promised.
    void mirrorAgentDefaultIsA17Scoped() {
        QVERIFY(defaultBuildSet(17).contains(QStringLiteral("mirror_agent")));
        QVERIFY(!defaultBuildSet(16).contains(QStringLiteral("mirror_agent")));
        QVERIFY(!defaultBuildSet().contains(QStringLiteral("mirror_agent")));  // universal set
        // A17 defaults resolve to a REGISTERED tag that provides the agent — no build demanded,
        // no silent miss; the resolver's registry arm then marks the deploy agentBaked.
        const ImageSelection s = selectImage(defaultBuildSet(17), 17);
        QVERIFY(!s.needsBuild);
        const ImageTag *t = findImageTag(s.tag);
        QVERIFY(t && t->provides.contains(QStringLiteral("mirror_agent")));
        // ...and the scoped default never nags a pinned profile of another release
        QVERIFY(!unappliedDefaultFeatures({QStringLiteral("gapps")})
                     .contains(QStringLiteral("mirror_agent")));
    }

    void widevineSelectsWv() {
        // native_vulkan used to ride along here; it was retired with bd remora-xqt1 and the
        // -wv tag is selected by widevine_l3 alone.
        const ImageSelection s = selectImage({"widevine_l3"}, 16);
        QCOMPARE(s.tag, QString("remora23:x86_64-gapps-wv"));
        QVERIFY(!s.needsBuild);
    }

    // No registered tag provides shamiko. What matters is that the request does not SILENTLY
    // resolve to a tag lacking the hiding stack — it must demand a build instead, which is where
    // shamiko is actually baked — and that the closest base it names is a Remora image.
    void shamikoDemandsABuildNow() {
        const ImageSelection s = selectImage({"shamiko"}, 16);
        QVERIFY2(s.tag.startsWith(QLatin1String("remora23:")), qPrintable(s.tag));
        QVERIFY(s.needsBuild);
        QVERIFY(s.missing.contains(QStringLiteral("shamiko")));
    }

    void hwDecodeSelectsHwc2NoBuild() {
        // enabling HW decode resolves to the pre-built -hwc2 image (not "needs build")…
        const ImageSelection s = selectImage({"gapps", "widevine_l3",
                                              "hw_video_decode"}, 16);
        QCOMPARE(s.tag, QString("remora23:x86_64-gapps-wv-hwc2"));
        QVERIFY(!s.needsBuild);
        // …but the plain -wv set still prefers -gapps-wv (hw_video_decode would be an extra there).
        QCOMPARE(selectImage({"widevine_l3"}, 16).tag,
                 QString("remora23:x86_64-gapps-wv"));
    }

    void microgConflictsWithGapps() {
        // The de-Googled alternative is mutually exclusive with gapps — BOTH sides gate as a
        // block so the clash is visible whichever box the user ticked last.
        const QVector<Gate> g = featureGates({"gapps", "microg"});
        QVERIFY(hasGate(g, "microg", "block"));
        QVERIFY(hasGate(g, "gapps", "block"));
        // alone, microg carries only its info caveat — no block
        QVERIFY(!hasGate(featureGates({"microg"}), "microg", "block"));
    }

    void microgNeedsSourceBuild() {
        // No pre-built image carries microG — the recipe demands the rebuild honestly.
        const ImageRecipe r = buildRecipe({"microg", "magisk_root"}, 16, {}, {}, {});
        QVERIFY(r.needsSourceBuild);
        QVERIFY(r.sourceBuildHint.contains(QStringLiteral("microg")));
        // A16 rebuilds lunch the bp2a release config
        QVERIFY(r.sourceBuildHint.contains(QStringLiteral("bp2a")));
    }

    void sensorsSelectsHwc2NoBuild() {
        // The sensors HAL is now baked into the -hwc2 golden (verified live, remora-4ei.8) —
        // requesting it alongside the hwc2 set resolves there with no rebuild…
        const ImageSelection s = selectImage({"gapps", "widevine_l3",
                                              "hw_video_decode", "sensors"}, 16);
        QCOMPARE(s.tag, QString("remora23:x86_64-gapps-wv-hwc2"));
        QVERIFY(!s.needsBuild);
        QVERIFY(!buildRecipe({"gapps", "widevine_l3", "hw_video_decode",
                              "sensors"}, 16, {}, {}, {}).needsSourceBuild);
        // …and it's an EXTRA on the plain -wv set (that stays sensor-free), so a non-sensors
        // profile still selects -gapps-wv, not the richer -hwc2.
        QCOMPARE(selectImage({"widevine_l3"}, 16).tag,
                 QString("remora23:x86_64-gapps-wv"));
    }

    // A feature can require SOURCE PATCHES, and until requiresPatches existed that constraint
    // lived only in prose in the note field: nothing read it, nothing validated it, and the A17
    // source build shipped `sensors` enabled with neither of its patches — "No Sensors on the
    // device", devInitCheck -19, discovered live and by accident (bd remora-4ei.53).
    void featureRequiringPatchesIsReportedWhenTheyAreOff() {
        // sensors enabled, patches explicitly pinned to something that isn't them
        const QVector<FeaturePatchGap> gaps =
            featurePatchGaps({"sensors"}, {"minigbm_rendernode"}, 17);
        QCOMPARE(gaps.size(), 1);
        QCOMPARE(gaps.first().feature, QString("sensors"));
        QCOMPARE(gaps.first().missingPatches,
                 QStringList({"sensors_hal", "sensors_product"}));
        // half of them is still a gap, and only the absent half is named
        const QVector<FeaturePatchGap> half = featurePatchGaps({"sensors"}, {"sensors_hal"}, 17);
        QCOMPARE(half.size(), 1);
        QCOMPARE(half.first().missingPatches, QStringList({"sensors_product"}));
        // both on ⇒ silent. A warning that fires on a correct configuration is the one that
        // teaches people to ignore the whole class.
        QVERIFY(featurePatchGaps({"sensors"}, {"sensors_hal", "sensors_product"}, 17).isEmpty());
        // feature off ⇒ silent, however the patches are set: nothing wanted them.
        QVERIFY(featurePatchGaps({"gapps"}, {"minigbm_rendernode"}, 17).isEmpty());
    }

    // THE CASE MOST LIKELY TO HIT THIS, and the one where the sibling warning's convention is
    // WRONG: an unpinned profile. Everywhere else empty means "unset ⇒ defaults apply", and
    // unappliedDefaultPatches reads that as "nothing pinned away, nothing to report". Here it
    // must resolve instead — sensors' two patches are both defaultOn=false, so an empty patch
    // list means they are genuinely absent, which is precisely the A17 configuration that
    // shipped a sensor-less image.
    void emptyPatchListResolvesToDefaultsRatherThanSilence() {
        const QVector<FeaturePatchGap> gaps = featurePatchGaps({"sensors"}, {}, 17);
        QCOMPARE(gaps.size(), 1);
        QCOMPARE(gaps.first().missingPatches, QStringList({"sensors_hal", "sensors_product"}));
        // and an empty FEATURE set resolves to the default build set, which requires no patches —
        // so a profile that has pinned nothing at all is silent, as it should be.
        QVERIFY(featurePatchGaps({}, {}, 17).isEmpty());
        QVERIFY(featurePatchGaps({}, {}, 16).isEmpty());
    }

    // bd remora-82c.3, the FEATURE half. featurePatchGaps says "enable the patch"; this says the
    // patch is not enable-able HERE at all — its series has no assets for the version — so the
    // feature is off the menu, DERIVED from the assets rather than a per-feature version field.
    // sensors requires sensors_hal (dir hardware-interfaces) and sensors_product (dir
    // device-remora-sensors); the fake models exactly those.
    void featureIsUnbuildableWhenARequiredPatchHasNoAssets() {
        QMap<QString, QByteArray> files;
        VendorReader read = [&files](const QString &rel) {
            QList<QPair<QString, QByteArray>> out;
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                if (it.key() == rel || it.key().startsWith(rel + QLatin1Char('/')))
                    out << qMakePair(it.key(), it.value());
            return out;
        };
        // both required series present (shared) → buildable on every version, no gap
        files["source-patches/hardware-interfaces/0001.patch"] = "x";
        files["source-patches/device-remora-sensors/sensors.patch"] = "x";
        QVERIFY(featureVersionGaps({"sensors"}, 17, read).isEmpty());
        QVERIFY(featureVersionGaps({"sensors"}, 16, read).isEmpty());

        // sensors_product absorbed upstream on A17 (SKIP) → sensors un-buildable on 17, fine on 16,
        // and only the inapplicable half is named.
        files["source-patches/device-remora-sensors-a17/SKIP"] = "";
        const auto g17 = featureVersionGaps({"sensors"}, 17, read);
        QCOMPARE(g17.size(), 1);
        QCOMPARE(g17.first().feature, QStringLiteral("sensors"));
        QCOMPARE(g17.first().inapplicablePatches, QStringList({"sensors_product"}));
        QVERIFY(featureVersionGaps({"sensors"}, 16, read).isEmpty());

        // a feature not REQUESTED is never reported, even with its assets missing on 17
        QVERIFY(featureVersionGaps({"gapps"}, 17, read).isEmpty());
        // a requested feature with no required patches never gaps
        QVERIFY(featureVersionGaps({"arm_translate"}, 17, read).isEmpty());

        // no assets at all → both of sensors' required patches are inapplicable
        VendorReader none = [](const QString &) { return QList<QPair<QString, QByteArray>>(); };
        const auto gNone = featureVersionGaps({"sensors"}, 17, none);
        QCOMPARE(gNone.size(), 1);
        QCOMPARE(gNone.first().inapplicablePatches,
                 QStringList({"sensors_hal", "sensors_product"}));
    }

    // Every requiresPatches key must name a real registry entry. A typo would produce a warning
    // about a patch that cannot be enabled from any UI — permanently unfixable noise, which is
    // strictly worse than the silence this whole mechanism replaces.
    void everyRequiredPatchKeyExists() {
        QSet<QString> known;
        for (const SourcePatch &p : sourcePatches()) known.insert(p.key);
        for (const Feature &f : buildFeatures())
            for (const QString &p : f.requiresPatches)
                QVERIFY2(known.contains(p),
                         qPrintable(QStringLiteral("feature '%1' requires unknown patch '%2'")
                                        .arg(f.key, p)));
    }

    // ---- irrelevantPatches: the profile gate on the Source page (bd remora-82c.10) -------------

    // Every key a PatchNeeds names must be real, for the same reason requiresPatches is checked
    // above — but the failure here is worse than noise. A typo in needs.feature names a feature
    // nothing can ever enable, so the patch is hidden from the page FOREVER while staying enabled
    // in the pin, i.e. it goes on applying with no way to see or stop it.
    void everyPatchNeedKeyExists() {
        QSet<QString> patches;
        for (const SourcePatch &p : sourcePatches()) patches.insert(p.key);
        for (const SourcePatch &p : sourcePatches()) {
            if (!p.needs.feature.isEmpty())
                QVERIFY2(findFeature(p.needs.feature),
                         qPrintable(QStringLiteral("patch '%1' needs unknown feature '%2'")
                                        .arg(p.key, p.needs.feature)));
            if (!p.needs.patch.isEmpty()) {
                QVERIFY2(patches.contains(p.needs.patch),
                         qPrintable(QStringLiteral("patch '%1' needs unknown patch '%2'")
                                        .arg(p.key, p.needs.patch)));
                QVERIFY2(p.needs.patch != p.key,
                         qPrintable(QStringLiteral("patch '%1' needs itself").arg(p.key)));
            }
        }
    }

    // Every registry entry's patch FILES must exist under vendor/source-patches. A registered
    // entry whose file is missing is silently inert — the apply loop finds nothing to do and says
    // nothing — which is the mirror of the failure that left hide-stock-updater-entry.patch on
    // disk registered by NOTHING for several releases. Skipped rather than failed when the vendor
    // tree is not beside the binary, so an installed-binary test run does not fail spuriously.
    void everyRegisteredPatchFileExists() {
        QDir root(QCoreApplication::applicationDirPath());
        QString base;
        for (int up = 0; up < 4; ++up) {
            const QString cand = root.absoluteFilePath(QStringLiteral("vendor/source-patches"));
            if (QFileInfo::exists(cand)) { base = cand; break; }
            if (!root.cdUp()) break;
        }
        if (base.isEmpty()) QSKIP("vendor/source-patches not found next to the test binary");
        for (const SourcePatch &p : sourcePatches()) {
            for (const QString &f : p.patches) {
                const QString path = base + QLatin1Char('/') + p.dir + QLatin1Char('/') + f;
                QVERIFY2(QFileInfo::exists(path),
                         qPrintable(QStringLiteral("patch '%1' registers a missing file: %2")
                                        .arg(p.key, path)));
            }
        }
    }

    // A fully-provisioned profile gates nothing away. This is the live A17 profile's shape, and it
    // is the case a mistake here would be most expensive on: silently hiding half the golden stack
    // from the one configuration that uses all of it.
    // DERIVED from the registry, not spelled out. These two tests mean "a profile that provides
    // everything the patches ask for", and a literal list makes that false the moment a patch
    // declares a new needs.feature — which is a spurious failure in the test rather than a real
    // one in the code, and it teaches the next person to edit the expectation instead of reading
    // it. usb_audio_policy was the first to do it (bd remora-4ei.37).
    static QSet<QString> featuresPatchesAskFor() {
        QSet<QString> f;
        for (const SourcePatch &p : sourcePatches())
            if (!p.needs.feature.isEmpty()) f.insert(p.needs.feature);
        return f;
    }

    void fullProfileGatesNothing() {
        PatchProfile all;
        all.features = featuresPatchesAskFor();
        all.lineageTree = true;
        all.venus = true;
        for (const SourcePatch &p : sourcePatches()) all.enabledPatches << p.key;
        // The set must be non-trivial, or "gates nothing" would pass by providing nothing.
        QVERIFY(all.features.contains(QStringLiteral("sensors")));
        QVERIFY(irrelevantPatches(all).isEmpty());
    }

    void aospTreeHidesEveryLineagePatch() {
        PatchProfile aosp;
        aosp.features = featuresPatchesAskFor();
        aosp.lineageTree = false;
        aosp.venus = true;
        for (const SourcePatch &p : sourcePatches()) aosp.enabledPatches << p.key;
        const QMap<QString, QString> out = irrelevantPatches(aosp);
        // Exactly the entries that patch vendor/lineage, device/lineage or the lineage-sdk's
        // reach into frameworks/base — none of which an aosp sync brings down (bd remora-82c.4).
        // lineage_legal_url and lineage_version_props left this list with their
        // registry entries (bd remora-31sq): both patched a device project that no longer exists
        // in ANY tree, and their content is native in device/remora. They were
        // lineage-only AND now unreachable; this list is about the first property, so their
        // absence here is a consequence of deletion rather than a change of policy.
        const QStringList expect = {"drop_lineage_updater",   "lineage_backport_deps",
                                    "lineage_platform_backport",
                                    "lineage_platform_res",   "lineage_sdk_features",
                                    "lineage_soong",          "no_lineage_sepolicy"};
        QCOMPARE(out.keys(), expect);
        // The reason names the SOURCE KIND, not the backport — broadest cause first, so the row
        // says the thing the user can act on.
        QVERIFY(out.value("lineage_backport_deps").contains("AOSP"));
    }

    void venusPatchNeedsTheVenusPath() {
        PatchProfile p;
        p.features = {"sensors"};
        p.lineageTree = true;
        p.venus = false;
        for (const SourcePatch &s : sourcePatches()) p.enabledPatches << s.key;
        QVERIFY(irrelevantPatches(p).contains("minigbm_gbm_mesa_vtest"));
        // and the Intel gralloc beside it is NOT gated — it is the default path, not the NVIDIA one
        QVERIFY(!irrelevantPatches(p).contains("minigbm_rendernode"));
        p.venus = true;
        QVERIFY(!irrelevantPatches(p).contains("minigbm_gbm_mesa_vtest"));
    }

    // The pair that reached a live A17 image with the feature on and neither patch applied
    // (bd remora-4ei.53). featurePatchGaps warns about that; this is the other half — with the
    // feature OFF they are not decisions at all and should not be on the page.
    void sensorsPatchesFollowTheirFeature() {
        PatchProfile p;
        p.lineageTree = true;
        p.venus = true;
        for (const SourcePatch &s : sourcePatches()) p.enabledPatches << s.key;
        QMap<QString, QString> off = irrelevantPatches(p);
        QVERIFY(off.contains("sensors_hal"));
        QVERIFY(off.contains("sensors_product"));
        p.features = {"sensors"};
        QMap<QString, QString> on = irrelevantPatches(p);
        QVERIFY(!on.contains("sensors_hal"));
        QVERIFY(!on.contains("sensors_product"));
    }

    // The amdgpu DRI-fallback patch is UNVALIDATED ON AMD HARDWARE, so the two properties that
    // keep it harmless are worth pinning rather than leaving to a reviewer noticing a flipped bool:
    // it is offered only alongside Mesa-from-source (it fixes nothing without it), and it is in no
    // release's default set. Delete the second half when an AMD host has proven render and VCE
    // HEVC encode on a Mesa-26 image — that is the event that makes default-on honest.
    void amdgpuDriFallbackIsOfferedOnlyWithMesaSource() {
        PatchProfile p;
        p.lineageTree = true;
        p.venus = true;
        for (const SourcePatch &s : sourcePatches()) p.enabledPatches << s.key;
        QVERIFY(irrelevantPatches(p).contains("minigbm_amdgpu_no_dri"));
        p.features = {"mesa_source"};
        QVERIFY(!irrelevantPatches(p).contains("minigbm_amdgpu_no_dri"));
        for (int v : {16, 17})
            QVERIFY2(!defaultSourcePatchSet(v).contains("minigbm_amdgpu_no_dri"),
                     "unvalidated on AMD hardware — must not be in any golden stack");
    }

    void backportDepsFollowTheBackport() {
        PatchProfile p;
        p.features = {"sensors"};
        p.lineageTree = true;
        p.venus = true;
        for (const SourcePatch &s : sourcePatches())
            if (s.key != QLatin1String("lineage_platform_backport")) p.enabledPatches << s.key;
        QVERIFY(irrelevantPatches(p).contains("lineage_backport_deps"));
        p.enabledPatches << QStringLiteral("lineage_platform_backport");
        QVERIFY(!irrelevantPatches(p).contains("lineage_backport_deps"));
    }

    // Gating must never be able to hide a patch by ARGUING ITSELF INTO IT: a needs.patch chain
    // that loops would make two entries hide each other with no profile able to show either.
    void patchNeedsChainsTerminate() {
        QHash<QString, QString> next;
        for (const SourcePatch &p : sourcePatches())
            if (!p.needs.patch.isEmpty()) next.insert(p.key, p.needs.patch);
        for (auto it = next.cbegin(); it != next.cend(); ++it) {
            QSet<QString> seen{it.key()};
            QString cur = it.value();
            while (next.contains(cur)) {
                QVERIFY2(!seen.contains(cur),
                         qPrintable(QStringLiteral("needs.patch cycle through '%1'").arg(cur)));
                seen.insert(cur);
                cur = next.value(cur);
            }
        }
    }

    // No gatesParkedBlocks test: zygisk_hiding was the registry's only Parked entry and it is
    // gone. The parked→block branch in featureGates stays for the next parked feature; it has no
    // fixture to test it against until one exists.

    // bd remora-xqt1 — hw_video_decode used to REQUIRE native_vulkan as a "you are on the VF GPU
    // path" marker, and warned without it. That feature is retired and the edge with it: c2-va is
    // a VA-API codec on iHD that never calls Vulkan, and the GPU path is a deploy-time fact the
    // host resolves per boot, not something a build feature can assert. So it stands alone now.
    void hwDecodeNoLongerRequiresAVulkanFeature() {
        const QVector<Gate> g = featureGates({"hw_video_decode"});
        QVERIFY(!hasGate(g, "hw_video_decode", "warn"));
        QVERIFY(!hasGate(g, "hw_video_decode", "block"));
        QVERIFY(hasGate(g, "hw_video_decode", "info"));  // the caveat itself survives
    }

    void gatesUnmetRequires() {
        QVERIFY(hasGate(featureGates({"gapps"}), "gapps", "warn"));  // needs arm_translate
    }

    // --- vendored PIF profile staleness (bd remora-4ei.47) ---
    void pifStalenessReportsOnlyOldProfiles() {
        const QDate today(2026, 7, 29);
        auto json = [](const QString &patch) {
            return QStringLiteral("{\"MODEL\":\"Pixel 6\",\"SECURITY_PATCH\":\"%1\"}").arg(patch);
        };
        // The profile actually vendored when this was written: 15 months old and blocklisted.
        const auto stale = stalePifProfile(json(QStringLiteral("2025-04-05")), today);
        QVERIFY(stale.has_value());
        QCOMPARE(stale->monthsOld, 15);
        QCOMPARE(stale->securityPatch, QStringLiteral("2025-04-05"));
        // Fresh is silent — a warning that fires on a good config is what teaches people to
        // ignore the class (the lesson bd remora-4ei.53 paid for).
        QVERIFY(!stalePifProfile(json(QStringLiteral("2026-07-05")), today).has_value());
    }

    void pifStalenessCountsWholeMonths() {
        const QDate today(2026, 7, 29);
        // Exactly at the threshold reports; one day short of it does not. Guards the day-of-month
        // correction — without it would read 6 months and warn early.
        QVERIFY(stalePifProfile(QStringLiteral("{\"SECURITY_PATCH\":\"2026-01-29\"}"), today).has_value());
        QVERIFY(!stalePifProfile(QStringLiteral("{\"SECURITY_PATCH\":\"2026-01-30\"}"), today).has_value());
    }

    void pifStalenessReportsUndatableButNotAbsent() {
        const QDate today(2026, 7, 29);
        // No SECURITY_PATCH, a malformed one, and unparseable JSON are all UNDATABLE, which is
        // reported with monthsOld -1: a profile you cannot date is not one you may call fresh.
        for (const QString &j : {QStringLiteral("{\"MODEL\":\"Pixel 6\"}"),
                                 QStringLiteral("{\"SECURITY_PATCH\":\"April 2025\"}"),
                                 QStringLiteral("not json at all")}) {
            const auto r = stalePifProfile(j, today);
            QVERIFY2(r.has_value(), qPrintable(j));
            QCOMPARE(r->monthsOld, -1);
        }
        // But NO profile at all is silence — that is the feature's problem, not this check's.
        QVERIFY(!stalePifProfile(QString(), today).has_value());
        QVERIFY(!stalePifProfile(QStringLiteral("   \n "), today).has_value());
    }

    // bd remora-4ei.55: the entry records a property of the PRE-BUILT images and has no
    // source-build implementation anywhere in the tree, so it must not read as delivered.
    void pairipDeclaresItselfUnimplemented() {
        const Feature *f = findFeature("webview_pairip_fix");
        QVERIFY(f);
        // Caveat, not Ok. Ok is the claim that cost this bead its investigation.
        QCOMPARE(f->status, FeatureStatus::Caveat);
        // Caveat is deliberate over Parked/Deferred: the feature is defaultOn, so a block would
        // stop every profile and a warn would fire on every build — for something that may well
        // be true on the pre-built tags. Info carries the note without crying wolf.
        const QVector<Gate> g = featureGates({"webview_pairip_fix", "arm_translate"});
        QVERIFY(hasGate(g, "webview_pairip_fix", "info"));
        QVERIFY(!hasGate(g, "webview_pairip_fix", "block"));
        QVERIFY(!hasGate(g, "webview_pairip_fix", "warn"));
        // If anyone ever DOES implement it, they must declare the patches — which is the whole
        // mechanism bd remora-4ei.53 added. An implementation with no declaration reproduces the
        // sensors defect exactly.
        QVERIFY(f->requiresPatches.isEmpty());
    }

    // --- Android version selection (remora-9hu) ---
    // 8-15 are not Remora builds — they have no releaseConfig, lunchProduct or container patch
    // set. The versions are deliberately KEPT in the registry, so what these pin is that an
    // assetless version degrades HONESTLY: named, no image, and a refusal that says which version
    // rather than a silent fallback to a neighbouring one.
    void versionWithNoAssetsResolvesNoImage() {
        const ImageSelection s = selectImage({}, 13);
        QVERIFY2(isPlaceholderImageTag(s.tag), qPrintable(s.tag));
        // It must NAME the version it could not serve. A bare empty tag would let a caller render
        // "" and leave the user guessing which of ten versions was the problem.
        QVERIFY2(s.tag.contains(QLatin1String("13")), qPrintable(s.tag));
        // and it must not quietly hand back a DIFFERENT version's image
        const ImageSelection a17 = selectImage({}, 17);
        QVERIFY(a17.tag.startsWith("remora24:"));
        QVERIFY(a17.tag != s.tag);
    }

    void versionWithNoAssetsRefusesRatherThanBuilding() {
        // gapps on A13: no baked image, and the feature gap is still reported rather than swallowed.
        const ImageSelection s = selectImage({"gapps"}, 13);
        QVERIFY2(isPlaceholderImageTag(s.tag), qPrintable(s.tag));
        QVERIFY(s.needsBuild);
        // A13 cannot be source-built either (no release config), so the build path refuses too —
        // that refusal lives in sourceBuildSteps and is covered by
        // aospSourceKindIsVersionGuardedAndDocumented in test_engine.
        QVERIFY(releaseConfigFor(13).isEmpty());
    }

    // EVERY REGISTERED TAG IS A REMORA BUILD. Spelled as its own assertion rather than left
    // implicit in the two above: a stray entry naming some other project's image would put a
    // version back on offer that Remora does not build, and the per-version tests would not notice.
    void registryHoldsOnlyRemoraBuiltTags() {
        static const QRegularExpression own(QStringLiteral("^remora\\d+:x86_64"));
        for (const ImageTag &t : imageTags())
            QVERIFY2(own.match(t.tag).hasMatch(), qPrintable(t.tag));
    }

    void supportedVersions() {
        QVERIFY(supportedAndroidVersions().contains(16));
        QVERIFY(supportedAndroidVersions().contains(8));
        QVERIFY(supportedAndroidVersions().contains(17));
    }

    // The sentinel must name the profile's OWN Android version. bd remora-85i reported a deploy
    // failing with "(no pre-built image for A17)" while the active instance was Android 16, which
    // reads as the resolver deriving the wrong version — so pin that the number is carried through
    // rather than baked in, and that a version WITH pre-built tags never reaches the sentinel.
    void placeholderNamesTheRequestedVersion() {
        // Both UNSUPPORTED versions on purpose: any version with a registered tag stops hitting
        // the sentinel the day someone builds for it. This asserted 17 until A17 images were
        // registered, which broke it — a real version makes this test expire silently.
        QCOMPARE(selectImage({}, 98).tag, QString("(no pre-built image for A98)"));
        QCOMPARE(selectImage({}, 99).tag, QString("(no pre-built image for A99)"));
        // A16 has real tags, so even an unsatisfiable feature set resolves to a best-effort tag
        // with the gaps reported — never the sentinel. An A16 profile therefore cannot emit an
        // "A17" message, which is what made that report suspicious rather than reproducible.
        const ImageSelection a16 = selectImage({"widevine_l3", "hw_video_decode", "sensors"}, 16);
        QVERIFY(!isPlaceholderImageTag(a16.tag));
        QVERIFY(!a16.tag.isEmpty());
    }

    // THE MIRROR AUDIT, AS AN INVARIANT (docs/IMAGE_MIRROR.md, bd remora-28ix.6). That document
    // decides which tags may be published, and it says of itself that the verdict can go stale
    // because "tags gain features; a feature added to a tag can change its verdict, and nothing
    // enforces that automatically". This is that enforcement.
    //
    // The one thing that must never drift is SHAMIKO. Every other component in a published image
    // is either ours, permissively licensed, or a GPL obligation we can meet by offering source —
    // and GApps/Widevine ride under an explicit accepted-exposure decision. Shamiko is different
    // in kind: it is closed-source freeware, NO licence grants redistribution at all, so an image
    // carrying it cannot be mirrored under any framing, disclaimer or policy change.
    //
    // It is also the easiest one to add by accident, because it looks like part of the hiding
    // stack that play_spoof already pulls in. It is not: shamiko REQUIRES play_spoof, but
    // play_spoof does not imply shamiko, and that asymmetry is exactly what a future edit could
    // erase without anyone noticing until an unpublishable image had been mirrored.
    void publishableTagsCarryNoUnredistributableComponent() {
        int checked = 0;
        for (const ImageTag &t : imageTags()) {
            if (!t.tag.startsWith("remora24:")) continue;  // only tags Remora itself builds
            ++checked;
            QVERIFY2(!t.provides.contains("shamiko"),
                     qPrintable(QStringLiteral(
                                    "%1 provides shamiko — closed-source freeware with no "
                                    "redistribution licence. This tag can no longer be mirrored; "
                                    "either drop the feature or remove the tag from "
                                    "docs/IMAGE_MIRROR.md's publishable register.")
                                    .arg(t.tag)));
        }
        // Guard the guard: if the tag family is ever renamed, the loop above would silently check
        // nothing and pass. The register documents four A17 tags; fewer means the prefix moved.
        QVERIFY2(checked >= 4,
                 qPrintable(QStringLiteral("only %1 remora24:* tags found — the tag family was "
                                           "renamed and this audit stopped checking anything")
                                .arg(checked)));
    }

    void a17ResolvesToItsSourceBuiltImage() {
        // LineageOS ships no A17 prebuilt, so A17 is source-built — but the built image
        // must still RESOLVE. This test previously asserted the opposite (a17HasNoPrebuiltImages),
        // which was true only while nothing had been built for A17; once one had been, the Image
        // page reported that nothing resolved right after a successful build, and this test
        // actively defended that behaviour.
        // The expected tag carries NO "-src": a build produces "<...>-src", onPromoteImage()
        // strips it, and the promoted name is what a deploy uses.
        const ImageSelection s = selectImage({}, 17);
        QVERIFY(!isPlaceholderImageTag(s.tag));
        QCOMPARE(s.tag, QString("remora24:x86_64"));
        const ImageSelection full =
            selectImage({"gapps", "widevine_l3", "hw_video_decode"}, 17);
        QCOMPARE(full.tag, QString("remora24:x86_64-gapps-wv-hwc2"));
        QVERIFY(!full.tag.endsWith("-src"));
        // With the built image registered, the DEFAULT feature set is satisfied and no rebuild is
        // needed — that is the whole point of registering it. A rebuild is required only for
        // features no A17 image provides; microg is one (it is mutually exclusive with gapps, and
        // every A17 variant ships gapps or nothing).
        QVERIFY(!buildRecipe(defaultBuildSet(), 17).needsSourceBuild);
        const ImageRecipe r = buildRecipe({"microg"}, 17);
        QVERIFY(r.needsSourceBuild);
        // the A17 rebuild hint must name A17's release config (cp2a), never A16's bp2a — in both
        // the lineage (do-build.sh) and stock-AOSP hint flavors.
        QVERIFY(r.sourceBuildHint.contains(QStringLiteral("cp2a")));
        QVERIFY(buildRecipe({"microg"}, 17, {}, {}, QStringLiteral("aosp"))
                    .sourceBuildHint.contains(QStringLiteral("cp2a")));
    }

    // --- per-version assets are EXACT, never a range ---
    //
    // Both mappings below used an open-ended comparison, and both therefore answered confidently
    // for versions nobody had prepared assets for: `>= 17 ? a17 : a16` handed an Android 18 tree
    // the A17 container patch set, and `>= 17 ? cp2a : bp2a` disagreed with do-build.sh's own
    // `case ... *) bp2a` for the same version. An unknown version must yield EMPTY so the caller
    // can refuse, because the nearest neighbour IS the bug — the same rule bd remora-p4p set for
    // the pinned manifest, and the one containerPatchSet's own header already claimed to follow.
    void perVersionAssetsAreExactNotRanges() {
        QCOMPARE(containerPatchSet(16), QStringLiteral("android-16.0.0_r2"));
        QCOMPARE(containerPatchSet(17), QStringLiteral("android-17.0.0"));
        QVERIFY(containerPatchSet(18).isEmpty());  // must NOT silently become the A17 set
        QVERIFY(containerPatchSet(15).isEmpty());  // nor A16's, going the other way
        QCOMPARE(releaseConfigFor(16), QStringLiteral("bp2a"));
        QCOMPARE(releaseConfigFor(17), QStringLiteral("cp2a"));
        QVERIFY(releaseConfigFor(18).isEmpty());
    }

    // bd remora-82c.2. The per-version facts now come from ONE registry, and every accessor reads
    // from it — so this pins that they cannot drift apart, and that an unknown version is empty
    // EVERYWHERE (the property the scattered literals kept getting wrong one site at a time).
    void perVersionFactsAllComeFromTheRegistry() {
        // supportedAndroidVersions is exactly the registry's version column, in order.
        QVector<int> fromRegistry;
        for (const AndroidRelease &r : androidReleases()) fromRegistry << r.version;
        QCOMPARE(supportedAndroidVersions(), fromRegistry);
        QCOMPARE(fromRegistry, (QVector<int>{8, 9, 10, 11, 12, 13, 14, 15, 16, 17}));

        for (const AndroidRelease &r : androidReleases()) {
            // every accessor agrees with the row it reads from
            QCOMPARE(releaseConfigFor(r.version), r.releaseConfig);
            QCOMPARE(containerPatchSet(r.version), r.containerPatchSet);
            QCOMPARE(lineageBranchForAndroid(r.version),
                     r.lineageMinor.isEmpty() ? QString()
                                              : QStringLiteral("lineage-") + r.lineageMinor);
            // a source-buildable version (has a release config) must also have a container patch
            // set — you cannot build one without the other, and offering it half-declared is the
            // exact silent gap this registry exists to close.
            if (!r.releaseConfig.isEmpty()) QVERIFY(!r.containerPatchSet.isEmpty());
        }

        // an unknown version is nullptr, and empty from every accessor — no nearest-neighbour.
        QVERIFY(androidRelease(18) == nullptr);
        QVERIFY(androidRelease(7) == nullptr);
        QVERIFY(releaseConfigFor(18).isEmpty());
        QVERIFY(containerPatchSet(18).isEmpty());
        QVERIFY(lineageBranchForAndroid(18).isEmpty());
        QVERIFY(lineageBranchForAndroid(7).isEmpty());
        // known versions keep their exact Lineage branch (spot-check the irregular minors)
        QCOMPARE(lineageBranchForAndroid(17), QStringLiteral("lineage-24.0"));
        QCOMPARE(lineageBranchForAndroid(15), QStringLiteral("lineage-22.2"));
        QCOMPARE(lineageBranchForAndroid(8), QStringLiteral("lineage-15.1"));
    }

    // …and an unknown version must FAIL the build rather than patch the tree with someone else's
    // set. apply-container-patches.sh only checks that the directory exists, and android-17.0.0
    // does exist — so nothing downstream could have caught the crossing.
    void unknownVersionRefusesContainerPatchesLoudly() {
        const QString cmd = buildContainerPatchCommand(QStringLiteral("/tree"),
                                                       QStringLiteral("/sbuild"), 18);
        QVERIFY(cmd.contains(QStringLiteral("exit 1")));
        QVERIFY(cmd.contains(QStringLiteral("Android 18")));      // names the version
        QVERIFY(!cmd.contains(QStringLiteral("android-17.0.0"))); // and never reaches for A17's
    }

    // --- source-ref ↔ Android version mapping (remora-8kn) ---
    void refVersionParsing() {
        QCOMPARE(refAndroidVersion("lineage-24.0"), std::optional<int>(17));
        QCOMPARE(refAndroidVersion("lineage-23.0"), std::optional<int>(16));
        QCOMPARE(refAndroidVersion("lineage-22.2"), std::optional<int>(15));
        QCOMPARE(refAndroidVersion("lineage-15.1"), std::optional<int>(8));
        QCOMPARE(refAndroidVersion("lineage-23.0-legacy-um"), std::optional<int>(16));
        QCOMPARE(refAndroidVersion("android-17.0.0_r1"), std::optional<int>(17));
        QCOMPARE(refAndroidVersion("android-8.1.0_r33"), std::optional<int>(8));
        QCOMPARE(refAndroidVersion("android16-release"), std::optional<int>(16));
        // Any other <prefix>-<version> ref carries no version claim, which is what the callers
        // refuse on: the parser must not map a branch naming that points at nothing in either tree
        // to a release.
        QVERIFY(!refAndroidVersion("vendor-16.0.0"));
        QVERIFY(!refAndroidVersion("vendor-8.1.0"));
        // unversioned refs carry no version claim — the UI keeps them selectable
        QVERIFY(!refAndroidVersion("master"));
        QVERIFY(!refAndroidVersion("main"));
        QVERIFY(!refAndroidVersion("android-security-release"));
        QVERIFY(!refAndroidVersion("cm-14.1"));  // pre-lineage-15.1 era
    }

    void lineageBranchMapping() {
        QCOMPARE(lineageBranchForAndroid(17), QString("lineage-24.0"));
        QCOMPARE(lineageBranchForAndroid(16), QString("lineage-23.0"));
        QCOMPARE(lineageBranchForAndroid(8), QString("lineage-15.1"));
        QVERIFY(lineageBranchForAndroid(7).isEmpty());
    }

    // --- custom image recipe ---
    // A request Remora cannot serve from any registered tag must fall to a SOURCE build and say
    // which features drove it, rather than overlaying onto whatever scores least-bad. widevine_l3
    // remains the only overlay-able feature (native_vulkan was retired in bd remora-xqt1: a
    // build-time ro.hardware.vulkan is write-once and beat every runtime decision, so the ICD
    // became a boot arg).
    void recipeWithNoServableBaseGoesToSource() {
        const ImageRecipe r = buildRecipe({"shamiko", "widevine_l3"}, 16);
        // needsBuild is specifically "an OVERLAY can add the missing features" (!overlayMissing
        // .isEmpty()). With no registered A16 base left to overlay onto there is nothing to layer,
        // so it is correctly FALSE here and needsSourceBuild is what carries the request. Asserting
        // needsBuild would be asserting the old, base-having world.
        QVERIFY(!r.needsBuild);
        QVERIFY(r.needsSourceBuild);
        // the closest base the recipe names is a Remora image, and the Dockerfile layers onto it
        QVERIFY2(r.baseTag.startsWith(QLatin1String("remora23:")), qPrintable(r.baseTag));
        QVERIFY2(r.dockerfile.startsWith(QStringLiteral("FROM ") + r.baseTag),
                 qPrintable(r.dockerfile));
        // and it still names what forced the build rather than failing mutely
        QVERIFY(!r.sourceBuildHint.isEmpty());
        QVERIFY(!r.unbuildable.isEmpty());
        QVERIFY(!r.dockerfile.contains("ro.hardware.vulkan"));  // the retired pin stays gone
    }

    // bd remora-sgkg. WITHOUT an explicit base there is no way to ask for a GApps overlay, and
    // this is the arm that pins why: selectImage ranks the registry by fewest MISSING features,
    // and every Android version has a *_gapps tag registered beside its clean one — so any
    // request containing gapps scores miss=0 against the baked tag and the overlay branch is
    // unreachable. This is ALSO the regression guard for the daily-driver path: a profile that
    // asks for gapps must keep resolving to a baked image and must NOT start rebuilding.
    // SCOPED TO 16/17 SINCE, and the narrowing is the point rather than a concession:
    // 11-15 no longer have a baked *_gapps tag to prefer, because they no longer have any tag. The
    // property being pinned — "a gapps request resolves to a baked image instead of starting an
    // overlay rebuild" — is only meaningful where a baked image exists, and the daily-driver
    // regression this guards has always been on the versions Remora actually builds.
    void recipePrefersABakedGappsBaseWhenNoBaseIsNamed() {
        for (const int v : {16, 17}) {
            const ImageRecipe r = buildRecipe({"gapps"}, v);
            QVERIFY2(r.overlays.isEmpty(), qPrintable(QStringLiteral("A%1").arg(v)));
            QVERIFY2(!r.dockerfile.contains(QLatin1String("COPY gapps/")),
                     qPrintable(QStringLiteral("A%1").arg(v)));
            QVERIFY2(r.baseTag.contains(QLatin1String("gapps")), qPrintable(r.baseTag));
        }
    }

    // …and WITH one, the clean-mirror story works: name the GApps-free image you downloaded and
    // GApps is layered onto it by COPY, with no source tree involved. This is the bead's whole
    // reason to exist — before it, this request was answered with "run an hour-scale rebuild".
    void recipeOverlaysGappsOntoANamedCleanBase() {
        const ImageRecipe r = buildRecipe({"gapps"}, 17, QString(), QString(), QString(),
                                          QStringLiteral("remora24:x86_64"));
        QCOMPARE(r.baseTag, QString("remora24:x86_64"));
        QCOMPARE(r.overlays, QStringList{"gapps"});
        QVERIFY(r.needsBuild);
        QVERIFY(!r.needsSourceBuild);  // the point: no AOSP tree required
        QVERIFY(r.unbuildable.isEmpty());
        QVERIFY(r.dockerfile.contains("FROM remora24:x86_64"));
        // Both partitions. system_ext carries GoogleServicesFramework and is the easy one to
        // forget — without it GmsCore installs and check-in has nothing to talk to.
        QVERIFY(r.dockerfile.contains("COPY gapps/product/ /system/product/"));
        QVERIFY(r.dockerfile.contains("COPY gapps/system_ext/ /system/system_ext/"));
        // and the operator is told about BOTH payload trees, or the stage step passes and the
        // build quietly produces an image carrying half of GApps
        QVERIFY(r.payloads.contains(QStringLiteral("gapps/product/")));
        QVERIFY(r.payloads.contains(QStringLiteral("gapps/system_ext/")));
    }

    // An override must not silently launder a request: features the named base does NOT provide
    // and no overlay can add still have to come back as needing a source build. remora24:x86_64
    // bakes no widevine_l3, and that is overlay-able; hw_video_decode is not.
    void recipeOverrideStillReportsUnbuildables() {
        const ImageRecipe r =
            buildRecipe({"gapps", "widevine_l3", "hw_video_decode"}, 17, QString(), QString(),
                        QString(), QStringLiteral("remora24:x86_64"));
        QCOMPARE(r.overlays, (QStringList{"gapps", "widevine_l3"}));
        QCOMPARE(r.unbuildable, QStringList{"hw_video_decode"});
        QVERIFY(r.needsSourceBuild);
        QVERIFY(r.dockerfile.contains("COPY gapps/product/"));
        QVERIFY(r.dockerfile.contains("COPY wv/vendor/"));
    }

    // An UNKNOWN base is taken to provide nothing rather than being refused — Remora cannot know
    // what a hand-built or renamed image contains, and naming every requested feature is what an
    // operator can actually act on. The overlay-able half is still layered on.
    void recipeUnknownBaseProvidesNothing() {
        const ImageRecipe r = buildRecipe({"gapps", "magisk_root"}, 17, QString(), QString(),
                                          QString(), QStringLiteral("someones:homebrew"));
        QCOMPARE(r.baseTag, QString("someones:homebrew"));
        QCOMPARE(r.overlays, QStringList{"gapps"});
        QVERIFY(r.unbuildable.contains(QStringLiteral("magisk_root")));
        QVERIFY(r.dockerfile.contains("FROM someones:homebrew"));
    }

    void recipeDefaultsNeedNoBuild() {
        QVERIFY(!buildRecipe(defaultBuildSet(), 16).needsBuild);
    }
};

QTEST_MAIN(TestGating)
#include "test_gating.moc"
