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
};

QTEST_MAIN(TestStore)
#include "test_store.moc"
