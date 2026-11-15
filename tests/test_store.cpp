#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

#include "store/Store.h"

using namespace remora;

// Whether `path` resolves strictly inside `root`, compared component by component. Every symlink
// is followed, the leaf included: an existing leaf is canonicalized whole, and only a leaf that
// does not exist at all (not even as a dangling link) is placed under its canonicalized parent.
// Anything that cannot be resolved (a dangling link, a missing ancestor, a missing root) is
// reported outside, never judged by its spelling.
static bool within(const QString &path, const QString &root) {
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return false;
    const QFileInfo leaf(path);
    QString resolved;
    if (leaf.exists() || leaf.isSymLink()) {
        resolved = leaf.canonicalFilePath();
    } else {
        const QString name = leaf.fileName();
        const QString parent = QFileInfo(leaf.absolutePath()).canonicalFilePath();
        if (parent.isEmpty() || name.isEmpty() || name == QStringLiteral(".") ||
            name == QStringLiteral(".."))
            return false;
        resolved = parent + QLatin1Char('/') + name;
    }
    if (resolved.isEmpty())
        return false;
    const QStringList r = canonicalRoot.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QStringList p = resolved.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return p.size() > r.size() && std::equal(r.cbegin(), r.cend(), p.cbegin());
}

// The store's one hard contract beyond round-tripping: a save must never destroy keys the
// writing binary's model does not know. Old binaries save routinely — the GUI writes settings
// on exit, and a GUI left running across an install keeps its pre-install model — so every
// config key ever added is one stale save away from silent deletion (bd remora-9hvy: an old
// GUI, exiting so its replacement could start, wiped a freshly hand-added audio= on its way
// out). These tests play the "newer binary" by planting keys no current model writes.
class TestStore : public QObject {
    Q_OBJECT

    QString rc(const QTemporaryDir &dir) const { return dir.filePath(QStringLiteral("remorarc")); }

private slots:
    // Every case below writes to a QTemporaryDir, but the store's default path is the user's real
    // remorarc. ctest gives each suite a HOME and XDG_* inside the build tree; this refuses to run
    // at all unless the default path really resolves inside it, so a later case that reaches the
    // default path can only ever touch the build tree, and a passing suite proves the isolation.
    void initTestCase() {
        const QString path = defaultRemorarcPath();
        if (!within(path, QStringLiteral(REMORA_TEST_HOME)))
            QFAIL(qPrintable(QStringLiteral("default remorarc %1 is not inside the isolated test "
                                            "HOME %2; run the suite through ctest")
                                 .arg(path, QStringLiteral(REMORA_TEST_HOME))));
    }

    // The guard above is only as good as within(): a symlinked leaf, a sibling whose name merely
    // starts with the root's, or a link that resolves nowhere must all count as outside.
    void withinFollowsLinksAndComparesComponents() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QDir b(base.path());
        QVERIFY(b.mkpath(QStringLiteral("home/.config")));
        QVERIFY(b.mkpath(QStringLiteral("homex/.config")));
        QVERIFY(b.mkpath(QStringLiteral("outside")));
        const QString root = b.filePath(QStringLiteral("home"));
        const QString cfg = b.filePath(QStringLiteral("home/.config"));
        const QString outsideFile = b.filePath(QStringLiteral("outside/remorarc"));
        for (const QString &f : {outsideFile, cfg + QStringLiteral("/real")}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        // Inside: a file not yet created, an existing one, and a link that stays in the root.
        QVERIFY(within(cfg + QStringLiteral("/remorarc"), root));
        QVERIFY(within(cfg + QStringLiteral("/real"), root));
        QVERIFY(QFile::link(cfg + QStringLiteral("/real"), cfg + QStringLiteral("/inner")));
        QVERIFY(within(cfg + QStringLiteral("/inner"), root));

        // An existing leaf link whose target is outside, though its parent is inside.
        QVERIFY(QFile::link(outsideFile, cfg + QStringLiteral("/leaf")));
        QVERIFY(!within(cfg + QStringLiteral("/leaf"), root));
        // A sibling that shares the root's spelling as a string prefix.
        QVERIFY(!within(b.filePath(QStringLiteral("homex/.config/remorarc")), root));
        // Dangling links, whether the missing target would be outside or inside.
        QVERIFY(QFile::link(b.filePath(QStringLiteral("outside/none")),
                            cfg + QStringLiteral("/dangling-out")));
        QVERIFY(!within(cfg + QStringLiteral("/dangling-out"), root));
        QVERIFY(QFile::link(cfg + QStringLiteral("/none"), cfg + QStringLiteral("/dangling-in")));
        QVERIFY(!within(cfg + QStringLiteral("/dangling-in"), root));
        // An ancestor link leading out, a missing ancestor, a missing root, the root itself.
        QVERIFY(QFile::link(b.filePath(QStringLiteral("outside")), cfg + QStringLiteral("/dir")));
        QVERIFY(!within(cfg + QStringLiteral("/dir/remorarc"), root));
        QVERIFY(!within(root + QStringLiteral("/missing/remorarc"), root));
        QVERIFY(!within(cfg + QStringLiteral("/remorarc"), b.filePath(QStringLiteral("gone"))));
        QVERIFY(!within(root, root));
    }

    void saveInstancePreservesUnknownKeys() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-x"));
            s.setValue("width", 1920);
            s.setValue("from_the_future", QStringLiteral("keep me"));
            s.endGroup();
            s.sync();
        }
        RemoraConfig c = loadInstance(path, QStringLiteral("x"));
        QCOMPARE(c.display.width, std::optional<int>(1920));
        c.mirror.audio = true;
        saveInstance(path, QStringLiteral("x"), c);

        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("Instance-x"));
        QCOMPARE(s.value("from_the_future").toString(), QStringLiteral("keep me"));
        QCOMPARE(s.value("width").toInt(), 1920);
        QCOMPARE(s.value("audio").toBool(), true);
    }

    // Device names carry spaces ("Keychron K8 Pro Keyboard"), so shared_inputs joins on newline —
    // the one character an evdev name cannot hold (bd remora-4ei.36). The round trip must return
    // the exact list, and an emptied list must remove the key, or unticking every device would
    // resurrect the shares on the next load.
    void sharedInputsRoundTripByNewline() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        RemoraConfig c;
        c.input.sharedInputs = QStringList{QStringLiteral("Keychron K8 Pro Keyboard"),
                                           QStringLiteral("Logitech USB Receiver Mouse")};
        saveInstance(path, QStringLiteral("x"), c);
        QCOMPARE(loadInstance(path, QStringLiteral("x")).input.sharedInputs, c.input.sharedInputs);

        c.input.sharedInputs.clear();
        saveInstance(path, QStringLiteral("x"), c);
        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("Instance-x"));
        QVERIFY(!s.contains(QStringLiteral("shared_inputs")));
    }

    void saveInstanceStillDeletesKnownUnsetKeys() {
        // "Unset optional deletes the key" is load-bearing — an fps the user cleared must fall
        // back to the resolver default, not linger — and must keep working without the group
        // remove that used to provide it.
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-x"));
            s.setValue("fps", 60);
            s.setValue("features", QStringLiteral("gapps"));
            s.setValue("gpu_mode", QStringLiteral("host"));
            s.setValue("camera_devices", QStringLiteral("/dev/video0"));
            s.endGroup();
            s.sync();
        }
        RemoraConfig c = loadInstance(path, QStringLiteral("x"));
        c.display.fps.reset();
        c.image.features.clear();
        c.gpu.mode.reset();
        c.input.cameraDevices.reset();
        saveInstance(path, QStringLiteral("x"), c);

        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("Instance-x"));
        QVERIFY(!s.contains("fps"));
        QVERIFY(!s.contains("features"));
        QVERIFY(!s.contains("gpu_mode"));
        QVERIFY(!s.contains("camera_devices"));
    }

    // An engaged-but-empty camera list means "cameras off", unset means auto-detect, so the
    // empty list must persist as a present, empty camera_devices= rather than a removed key —
    // the remove-on-empty list writer would silently turn "off" back into "auto".
    void explicitlyEmptyCameraDevicesRoundTrip() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        RemoraConfig c;
        c.input.cameraDevices = QStringList{};
        saveInstance(path, QStringLiteral("x"), c);

        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-x"));
            QVERIFY(s.contains(QStringLiteral("camera_devices")));
            QVERIFY(s.value(QStringLiteral("camera_devices")).toString().isEmpty());
        }

        const RemoraConfig loaded = loadInstance(path, QStringLiteral("x"));
        QVERIFY(loaded.input.cameraDevices.has_value());
        QVERIFY(loaded.input.cameraDevices->isEmpty());
    }

    // bd remora-6laf.8 — the Google Play option and the terms version it was accepted under
    // persist; the overlay a deploy materialized never does, since it belongs to one base and a
    // saved copy would pin the profile to that base for good.
    void googlePlayRoundTripsAndTheOverlayNeverPersists() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        RemoraConfig c;
        c.image.googlePlay = true;
        c.image.playTerms = QStringLiteral("litegapps-a17-20261001-v1");
        c.image.playOverlay = QStringLiteral("remora-local:play-08c62ac56c4d-07db5d1f5dc7");
        saveInstance(path, QStringLiteral("x"), c);
        const RemoraConfig loaded = loadInstance(path, QStringLiteral("x"));
        QCOMPARE(loaded.image.googlePlay, std::optional<bool>(true));
        QCOMPARE(loaded.image.playTerms, std::optional<QString>(*c.image.playTerms));
        QVERIFY(!loaded.image.playOverlay.has_value());
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-x"));
            for (const QString &k : s.allKeys())
                QVERIFY(!s.value(k).toString().contains(QStringLiteral("remora-local")));
        }
        // off removes both keys, so turning it back on asks for the terms again
        c.image.googlePlay.reset();
        c.image.playTerms.reset();
        saveInstance(path, QStringLiteral("x"), c);
        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("Instance-x"));
        QVERIFY(!s.contains(QStringLiteral("google_play")));
        QVERIFY(!s.contains(QStringLiteral("play_terms")));
    }

    void saveInstanceTouchesOnlyItsOwnGroup() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-other/width", 800);
            s.setValue("Defaults/fps", 60);
            s.sync();
        }
        saveInstance(path, QStringLiteral("x"), loadInstance(path, QStringLiteral("x")));

        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("Instance-other/width").toInt(), 800);
        QCOMPARE(s.value("Defaults/fps").toInt(), 60);
    }

    // loadInstance reads [Defaults] then [Instance-<name>]: an instance scalar overrides the
    // default, a key only [Defaults] sets fills in, and the retired-vm marker is sticky — a
    // profile that says vm in either place stays a vm profile, so preflight still refuses it when
    // the instance layer names a live backend.
    void loadInstanceLayersDefaultsUnderInstance() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Defaults/fps", 60);
            s.setValue("Defaults/dns", QStringLiteral("1.1.1.1 9.9.9.9"));
            s.setValue("Defaults/backend", QStringLiteral("vm"));
            s.setValue("Instance-x/fps", 30);
            s.setValue("Instance-x/backend", QStringLiteral("remote"));
            s.sync();
        }
        const RemoraConfig c = loadInstance(path, QStringLiteral("x"));
        QCOMPARE(c.display.fps, std::optional<int>(30));
        QCOMPARE(c.network.dns,
                 (QStringList{QStringLiteral("1.1.1.1"), QStringLiteral("9.9.9.9")}));
        QCOMPARE(c.backend.backend, std::optional<Backend>(Backend::Remote));
        QVERIFY(c.backend.retiredVmBackend);
    }

    void saveImageSourcesPreservesUnknownKeysAndDeletes() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Source-mirror/enabled", true);
            s.setValue("Source-mirror/location", QStringLiteral("https://img.example"));
            s.setValue("Source-mirror/auth_token_2027", QStringLiteral("keep me"));
            s.setValue("Source-doomed/enabled", true);
            s.setValue("Source-doomed/tags", QStringLiteral("a:1"));
            // A retired built-in's leftover: enabled only, describes nothing — load drops it,
            // and the save after that load must still clean its group up (bd remora-2ylt).
            s.setValue("Source-Self-built/enabled", false);
            s.sync();
        }
        QList<ImageSource> sources = loadImageSources(path);
        QCOMPARE(sources.size(), 2);
        for (int i = 0; i < sources.size(); ++i)
            if (sources[i].name == QLatin1String("doomed")) sources.removeAt(i--);
        saveImageSources(path, sources);

        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("Source-mirror/auth_token_2027").toString(), QStringLiteral("keep me"));
        QCOMPARE(s.value("Source-mirror/location").toString(), QStringLiteral("https://img.example"));
        QVERIFY(!s.childGroups().contains(QStringLiteral("Source-doomed")));
        QVERIFY(!s.childGroups().contains(QStringLiteral("Source-Self-built")));
    }

    void saveDeploymentsPreservesUnknownKeysAndDeletes() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Deployment-a/profile", QStringLiteral("p"));
            s.setValue("Deployment-a/pinned_gpu_2027", QStringLiteral("keep me"));
            s.setValue("Deployment-b/profile", QStringLiteral("q"));
            s.sync();
        }
        QList<Deployment> deps = loadDeployments(path);
        QCOMPARE(deps.size(), 2);
        for (int i = 0; i < deps.size(); ++i)
            if (deps[i].name == QLatin1String("b")) deps.removeAt(i--);
        saveDeployments(path, deps);

        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("Deployment-a/pinned_gpu_2027").toString(), QStringLiteral("keep me"));
        QCOMPARE(s.value("Deployment-a/profile").toString(), QStringLiteral("p"));
        QVERIFY(!s.childGroups().contains(QStringLiteral("Deployment-b")));
    }

    // An explicit save — even of nothing — marks the list initialised, so a user who deleted
    // every runner is not re-seeded. Host and image are optional: a blank one is removed from the
    // file, not written as an empty value that a reader could mistake for an override.
    void saveDeploymentsFlagsEmptySaveAndRoundTripsHostImage() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        saveDeployments(path, {});
        {
            QSettings s(path, QSettings::IniFormat);
            QCOMPARE(s.value("Remora/deployments_initialized").toBool(), true);
        }
        QVERIFY(loadDeployments(path).isEmpty());

        Deployment d;
        d.name = QStringLiteral("a");
        d.profile = QStringLiteral("p");
        d.host = QStringLiteral("h");
        d.image = QStringLiteral("i");
        saveDeployments(path, {d});
        QList<Deployment> deps = loadDeployments(path);
        QCOMPARE(deps.size(), 1);
        QCOMPARE(deps[0].name, QStringLiteral("a"));
        QCOMPARE(deps[0].profile, QStringLiteral("p"));
        QCOMPARE(deps[0].host, QStringLiteral("h"));
        QCOMPARE(deps[0].image, QStringLiteral("i"));

        d.host.clear();
        d.image.clear();
        saveDeployments(path, {d});
        QSettings s(path, QSettings::IniFormat);
        QVERIFY(!s.contains(QStringLiteral("Deployment-a/host")));
        QVERIFY(!s.contains(QStringLiteral("Deployment-a/image")));
        QCOMPARE(s.value("Deployment-a/profile").toString(), QStringLiteral("p"));
        s.beginGroup(QStringLiteral("Deployment-a"));
        QCOMPARE(s.childKeys(), QStringList{QStringLiteral("profile")});
        s.endGroup();
        QCOMPARE(s.value("Remora/deployments_initialized").toBool(), true);
    }

    // The Run table is seeded from listInstances, so every other section sharing the file must
    // stay out of it: a filter regression would offer Defaults or a deployment as a profile.
    void listInstancesReturnsOnlyProfilesSorted() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-b/width", 800);
            s.setValue("Remora/active_instance", QStringLiteral("b"));
            s.setValue("Source-s/enabled", true);
            s.setValue("Instance-a/width", 1024);
            s.setValue("Deployment-d/profile", QStringLiteral("a"));
            s.setValue("Defaults/fps", 60);
            s.sync();
        }
        QCOMPARE(listInstances(path), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    }

    void activeInstanceFallsBackWhenUnset() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        QCOMPARE(activeInstance(path), QStringLiteral("default"));
        QCOMPARE(activeInstance(path, QStringLiteral("other")), QStringLiteral("other"));

        // A top-level key lands in QSettings' reserved [General] section; it is not the setting.
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("active_instance", QStringLiteral("c"));
            s.sync();
        }
        QCOMPARE(activeInstance(path), QStringLiteral("default"));
    }

    // The selection lives in [Remora], not [General]: QSettings reserves [General] for top-level
    // keys, so a hand-written [General] group would never be read back.
    void setActiveInstanceRoundTripsThroughRemoraGroup() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        setActiveInstance(path, QStringLiteral("b"));
        QCOMPARE(activeInstance(path), QStringLiteral("b"));

        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("Remora/active_instance").toString(), QStringLiteral("b"));
        QVERIFY(!s.contains(QStringLiteral("active_instance")));
        QCOMPARE(s.childGroups(), QStringList{QStringLiteral("Remora")});
    }

    void deleteInstanceRemovesOnlyItsOwnGroup() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-a/width", 1024);
            s.setValue("Instance-a/fps", 30);
            s.setValue("Instance-b/width", 800);
            s.setValue("Instance-b/from_the_future", QStringLiteral("keep me"));
            s.setValue("Defaults/fps", 60);
            s.sync();
        }
        deleteInstance(path, QStringLiteral("a"));

        QCOMPARE(listInstances(path), QStringList{QStringLiteral("b")});
        QSettings s(path, QSettings::IniFormat);
        QVERIFY(!s.childGroups().contains(QStringLiteral("Instance-a")));
        QVERIFY(!s.contains(QStringLiteral("Instance-a/width")));
        QCOMPARE(s.value("Instance-b/width").toInt(), 800);
        QCOMPARE(s.value("Instance-b/from_the_future").toString(), QStringLiteral("keep me"));
        QCOMPARE(s.value("Defaults/fps").toInt(), 60);
        s.beginGroup(QStringLiteral("Instance-b"));
        QCOMPARE(s.childKeys().size(), 2);
        s.endGroup();
        s.beginGroup(QStringLiteral("Defaults"));
        QCOMPARE(s.childKeys(), QStringList{QStringLiteral("fps")});
    }

    // ─── service --remove: the unit-removal adapter against a fake manager ───

    // The named regression: the .service file is gone, its drop-in remains, and the manager still
    // has the unit loaded and running. Its disable and stop fail; a successful reload must not
    // turn that into success, and the drop-in must survive untouched.
    void removeKeepsDropInOfRunningOrphanThatCannotStop() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-gone.service.d/override.conf"),
              "[Service]\nEnvironment=X=1\n");
        FakeManager m;
        m.units[QStringLiteral("remora-gone.service")] = {QStringLiteral("not-found"),
                                                          QStringLiteral("active"), 1, 1};
        m.units[QStringLiteral("remora-gone.service")].stopErr =
            QStringLiteral("Failed to stop remora-gone.service: Access denied");
        m.restAbsent();
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY(!r.ok);
        QCOMPARE(read(unitDir + QStringLiteral("/remora-gone.service.d/override.conf")),
                 QByteArray("[Service]\nEnvironment=X=1\n"));
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.errors[0].startsWith(QStringLiteral("remora-gone.service: ")));
        QVERIFY(r.errors[0].contains(QStringLiteral("Access denied")));
        QVERIFY(r.errors[0].contains(QStringLiteral("ActiveState=active")));
        QVERIFY(m.ran(QStringLiteral("daemon-reload"), {}));
        QVERIFY(m.ran(QStringLiteral("stop"), QStringLiteral("remora-gone.service")));
    }

    // Same missing file, but the manager has the unit LOADED (its file lives elsewhere, or the
    // reload has not happened): disable fails, and nothing about the local tree excuses it.
    void removeKeepsDropInOfLoadedOrphan() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-gone.service.d/override.conf"), "keep\n");
        FakeManager m;
        m.restAbsent();
        m.units[QStringLiteral("remora-gone.service")] = {QStringLiteral("loaded"),
                                                          QStringLiteral("active"), 1, 0};
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY(!r.ok);
        QCOMPARE(read(unitDir + QStringLiteral("/remora-gone.service.d/override.conf")),
                 QByteArray("keep\n"));
        QVERIFY(r.errors.join('\n').contains(QStringLiteral("LoadState=loaded")));
        // stop is only the route for a not-found unit; a loaded one that will not disable fails.
        QVERIFY(!m.ran(QStringLiteral("stop"), QStringLiteral("remora-gone.service")));
    }

    // No local file and a manager that cannot answer — no bus, permission, garbage, half an
    // answer. None of them is "already absent".
    void removeNeverMisreportsUnknownStateAsAbsent_data() {
        QTest::addColumn<int>("queryRc");
        QTest::addColumn<QString>("queryOut");
        QTest::newRow("no bus") << 1 << QString();
        QTest::newRow("permission") << 4 << QString();
        QTest::newRow("malformed") << 0 << QStringLiteral("garbage\n");
        QTest::newRow("half") << 0 << QStringLiteral("LoadState=not-found\n");
        QTest::newRow("empty value") << 0 << QStringLiteral("LoadState=\nActiveState=inactive\n");
        QTest::newRow("unknown state") << 0
                                       << QStringLiteral("LoadState=bad-setting\nActiveState=inactive\n");
    }
    void removeNeverMisreportsUnknownStateAsAbsent() {
        QFETCH(int, queryRc);
        QFETCH(QString, queryOut);
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-gone.service.d/override.conf"), "keep\n");
        FakeManager m;
        m.restAbsent();
        FakeManager::Unit &u = m.units[QStringLiteral("remora-gone.service")];
        u = {QStringLiteral("not-found"), QStringLiteral("inactive"), 1, 0};
        u.queryRc = queryRc;
        u.queryOut = queryOut;
        u.overrideQuery = true;
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY(!r.ok);
        QVERIFY(QFileInfo::exists(unitDir + QStringLiteral("/remora-gone.service.d/override.conf")));
        QVERIFY(r.errors.join('\n').contains(QStringLiteral("remora-gone.service")));
        QVERIFY(!r.removed.contains(unitDir + QStringLiteral("/remora-gone.service.d")));
    }

    // Explicit not-found + inactive is the one answer that makes a failed disable harmless: stale
    // drop-ins go, and running the cleanup again succeeds again.
    void removeConfirmedAbsentIsRepeatable() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-gone.service.d/override.conf"), "stale\n");
        write(unitDir + QStringLiteral("/remora-updater-gone.service.d/a.conf"), "stale\n");
        FakeManager m;
        m.restAbsent();
        UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY2(r.ok, qPrintable(r.errors.join('\n')));
        QVERIFY(!QFileInfo::exists(unitDir + QStringLiteral("/remora-gone.service.d")));
        QVERIFY(!QFileInfo::exists(unitDir + QStringLiteral("/remora-updater-gone.service.d")));
        r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY2(r.ok, qPrintable(r.errors.join('\n')));
        QVERIFY(r.removed.isEmpty());
    }

    // A not-found unit still running is stopped, then removed once the manager confirms it.
    void removeStopsRunningOrphanThenDeletes() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-gone.service.d/override.conf"), "x\n");
        FakeManager m;
        m.restAbsent();
        m.units[QStringLiteral("remora-gone.service")] = {QStringLiteral("not-found"),
                                                          QStringLiteral("active"), 1, 0};
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY2(r.ok, qPrintable(r.errors.join('\n')));
        QVERIFY(!QFileInfo::exists(unitDir + QStringLiteral("/remora-gone.service.d")));
        QVERIFY(m.ran(QStringLiteral("stop"), QStringLiteral("remora-gone.service")));
    }

    void removeKeepsInstalledUnitWhoseDisableFails() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-p.service"), "[Unit]\n");
        FakeManager m;
        m.allPresent(QStringLiteral("p"));
        m.units[QStringLiteral("remora-p.service")].disableRc = 1;
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("p"), m.runner());
        QVERIFY(!r.ok);
        QCOMPARE(read(unitDir + QStringLiteral("/remora-p.service")), QByteArray("[Unit]\n"));
    }

    void removeFailsWhenReloadFails() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-p.service"), "[Unit]\n");
        FakeManager m;
        m.allPresent(QStringLiteral("p"));
        m.reloadRc = 1;
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("p"), m.runner());
        QVERIFY(!r.ok);
        QVERIFY(r.errors.join('\n').contains(QStringLiteral("daemon-reload failed")));
    }

    // A normal cleanup — profile present or already gone, the adapter is the same: all three units
    // and their exact drop-in directories go, each only after its disable ran, and nothing else.
    void removeDeletesExactlyTheProfilesUnits() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-other/width", 800);
            s.sync();
        }
        for (const QString &u : serviceUnitNames(QStringLiteral("p"))) {
            write(unitDir + '/' + u, "[Unit]\n");
            write(unitDir + '/' + u + QStringLiteral(".d/override.conf"), "o\n");
        }
        for (const QString &u : serviceUnitNames(QStringLiteral("other")))
            write(unitDir + '/' + u, "[Unit]\n");
        write(unitDir + QStringLiteral("/remora-p.service.wants/keep"), "k\n");
        write(unitDir + QStringLiteral("/remora-pp.service"), "k\n");
        write(unitDir + QStringLiteral("/unrelated.service"), "k\n");
        FakeManager m;
        m.unitDir = unitDir;
        m.allPresent(QStringLiteral("p"));
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("p"), m.runner());
        QVERIFY2(r.ok, qPrintable(r.errors.join('\n')));
        QCOMPARE(r.removed.size(), 6);
        for (const QString &u : serviceUnitNames(QStringLiteral("p"))) {
            QVERIFY(!QFileInfo::exists(unitDir + '/' + u));
            QVERIFY(!QFileInfo::exists(unitDir + '/' + u + QStringLiteral(".d")));
            QVERIFY2(m.fileExistedAtDisable.value(u), qPrintable(u));
        }
        for (const QString &u : serviceUnitNames(QStringLiteral("other")))
            QVERIFY(QFileInfo::exists(unitDir + '/' + u));
        QVERIFY(QFileInfo::exists(unitDir + QStringLiteral("/remora-p.service.wants/keep")));
        QVERIFY(QFileInfo::exists(unitDir + QStringLiteral("/remora-pp.service")));
        QVERIFY(QFileInfo::exists(unitDir + QStringLiteral("/unrelated.service")));
        QCOMPARE(listInstances(path), QStringList{QStringLiteral("other")});
        QCOMPARE(m.calls.last(), (QStringList{"systemctl", "--user", "daemon-reload"}));
    }

    // A drop-in directory that is a link, and links inside a real one, are unlinked; what they
    // point at is untouched.
    void removeUnlinksSymlinksWithoutFollowing() {
        QTemporaryDir dir, outside;
        const QString unitDir = dir.path();
        write(outside.path() + QStringLiteral("/target-dir/precious"), "p\n");
        write(outside.path() + QStringLiteral("/precious-file"), "p\n");
        QVERIFY(QFile::link(outside.path() + QStringLiteral("/target-dir"),
                            unitDir + QStringLiteral("/remora-gone.service.d")));
        QVERIFY(QDir().mkpath(unitDir + QStringLiteral("/remora-updater-gone.service.d")));
        QVERIFY(QFile::link(outside.path() + QStringLiteral("/target-dir"),
                            unitDir + QStringLiteral("/remora-updater-gone.service.d/dirlink")));
        QVERIFY(QFile::link(outside.path() + QStringLiteral("/precious-file"),
                            unitDir + QStringLiteral("/remora-updater-gone.service.d/a.conf")));
        QVERIFY(QFile::link(outside.path() + QStringLiteral("/missing"),
                            unitDir + QStringLiteral("/remora-autosleep-gone.service")));
        FakeManager m;
        m.restAbsent();
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("gone"), m.runner());
        QVERIFY2(r.ok, qPrintable(r.errors.join('\n')));
        QVERIFY(QDir(unitDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System)
                    .isEmpty());
        QCOMPARE(read(outside.path() + QStringLiteral("/target-dir/precious")), QByteArray("p\n"));
        QCOMPARE(read(outside.path() + QStringLiteral("/precious-file")), QByteArray("p\n"));
    }

    // The decision cmdService takes before requireInstance would have refused a missing profile.
    void serviceRemovalRefusalDecision() {
        // an absent profile, with or without any configuration at all
        QVERIFY(serviceRemovalRefusal(QStringLiteral("gone"), {}).isEmpty());
        QVERIFY(serviceRemovalRefusal(QStringLiteral("gone"), {QStringLiteral("other")}).isEmpty());
        QVERIFY(serviceRemovalRefusal(QStringLiteral("p"), {QStringLiteral("p")}).isEmpty());
        QVERIFY(!serviceRemovalRefusal(QString(), {}).isEmpty());
        QVERIFY(!serviceRemovalRefusal(QString(300, QLatin1Char('a')), {}).isEmpty());
        // escaping collision: "a b" and "a-b" share every unit
        QVERIFY(!serviceRemovalRefusal(QStringLiteral("a b"), {QStringLiteral("a-b")}).isEmpty());
        // family collisions, both directions
        QVERIFY(!serviceRemovalRefusal(QStringLiteral("updater-x"), {QStringLiteral("x")}).isEmpty());
        QVERIFY(!serviceRemovalRefusal(QStringLiteral("x"), {QStringLiteral("autosleep-x")})
                     .isEmpty());
        // unsafe spellings become plain unit names: no separator, glob or leading option
        for (const QString &n : {QStringLiteral("../../etc"), QStringLiteral("a/b"),
                                 QStringLiteral("*"), QStringLiteral("--now"),
                                 QStringLiteral("x\ny")}) {
            for (const QString &u : serviceUnitNames(n)) {
                QVERIFY2(!u.contains('/') && !u.contains('*') && !u.contains('\n'), qPrintable(u));
                QVERIFY(u.startsWith(QStringLiteral("remora-")));
            }
        }
        QCOMPARE(serviceUnitNames(QStringLiteral("my phone")),
                 (QStringList{"remora-my-phone.service", "remora-updater-my-phone.service",
                              "remora-autosleep-my-phone.service"}));
    }

    // A traversal-shaped name stays inside the unit directory.
    void removeTraversalNameStaysInUnitDir() {
        QTemporaryDir root;
        const QString unitDir = root.path() + QStringLiteral("/a/b");
        QVERIFY(QDir().mkpath(unitDir));
        write(root.path() + QStringLiteral("/a/etc.service"), "k\n");
        write(root.path() + QStringLiteral("/etc.service.d/k"), "k\n");
        FakeManager m;
        m.restAbsent();
        const UnitRemovalResult r =
            removeServiceUnits(unitDir, QStringLiteral("../../etc"), m.runner());
        QVERIFY(r.ok);
        QVERIFY(r.removed.isEmpty());
        QVERIFY(QFileInfo::exists(root.path() + QStringLiteral("/a/etc.service")));
        QVERIFY(QFileInfo::exists(root.path() + QStringLiteral("/etc.service.d/k")));
    }

private:
    static void write(const QString &p, const QByteArray &data) {
        QVERIFY(QDir().mkpath(QFileInfo(p).absolutePath()));
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
    static QByteArray read(const QString &p) {
        QFile f(p);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    // A systemd --user manager modelled per unit, independently of any file on disk: what it has
    // loaded, whether it runs, and how disable / stop / show answer.
    struct FakeManager {
        struct Unit {
            QString load, active;
            int disableRc = 0, stopRc = 0;
            QString stopErr;
            bool overrideQuery = false;
            int queryRc = 0;
            QString queryOut;
        };
        QMap<QString, Unit> units;
        int reloadRc = 0;
        QString unitDir;  // when set, disable records whether the unit's file was still there
        QList<QStringList> calls;
        QMap<QString, bool> fileExistedAtDisable;

        // Every unit of the test's absent profiles not modelled yet: explicitly not-found and
        // inactive, with disable failing the way it does for a unit that has no file.
        void restAbsent() {
            QStringList all = serviceUnitNames(QStringLiteral("gone"));
            all += serviceUnitNames(QStringLiteral("../../etc"));
            for (const QString &u : all)
                if (!units.contains(u))
                    units[u] = {QStringLiteral("not-found"), QStringLiteral("inactive"), 1, 0};
        }
        void allPresent(const QString &profile) {
            for (const QString &u : serviceUnitNames(profile))
                units[u] = {QStringLiteral("loaded"), QStringLiteral("active"), 0, 0};
        }
        bool ran(const QString &verb, const QString &unit) const {
            for (const QStringList &c : calls)
                if (c.size() > 2 && c[2] == verb && (unit.isEmpty() || c.last() == unit)) return true;
            return false;
        }
        UnitCommandRunner runner() {
            return [this](const QStringList &v) -> UnitCommandResult {
                calls << v;
                if (v.size() < 3 || v[0] != QLatin1String("systemctl") ||
                    v[1] != QLatin1String("--user"))
                    return {127, {}, QStringLiteral("unexpected argv")};
                const QString verb = v[2];
                if (verb == QLatin1String("daemon-reload"))
                    return {reloadRc, {}, reloadRc ? QStringLiteral("reload refused") : QString()};
                const QString unit = v.last();
                if (!units.contains(unit)) return {127, {}, QStringLiteral("unmodelled unit")};
                Unit &u = units[unit];
                if (verb == QLatin1String("disable")) {
                    if (!unitDir.isEmpty())
                        fileExistedAtDisable[unit] = QFileInfo::exists(unitDir + '/' + unit);
                    if (u.disableRc != 0)
                        return {u.disableRc, {},
                                QStringLiteral("Failed to disable unit: Unit file %1 does not "
                                               "exist.").arg(unit)};
                    u.active = QStringLiteral("inactive");
                    return {};
                }
                if (verb == QLatin1String("stop")) {
                    if (u.stopRc != 0) return {u.stopRc, {}, u.stopErr};
                    u.active = QStringLiteral("inactive");
                    return {};
                }
                if (verb == QLatin1String("show")) {
                    if (u.overrideQuery)
                        return {u.queryRc, u.queryOut,
                                u.queryRc ? QStringLiteral("Failed to connect to bus") : QString()};
                    return {0, QStringLiteral("LoadState=%1\nActiveState=%2\n").arg(u.load, u.active),
                            {}};
                }
                return {127, {}, QStringLiteral("unexpected verb")};
            };
        }
    };
};

QTEST_MAIN(TestStore)
#include "test_store.moc"
