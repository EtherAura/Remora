#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "store/Store.h"

using namespace remora;

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
};

QTEST_MAIN(TestStore)
#include "test_store.moc"
