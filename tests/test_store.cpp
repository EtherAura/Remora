#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

#include "TestPlatform.h"
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

    // Where the default remorarc must resolve for the suite to run. On Linux, the HOME and XDG_*
    // ctest sets, inside the build tree. Windows asks the known-folder API, which no environment
    // variable redirects, so there it is QStandardPaths' test mode: a qttest folder of its own
    // beside the real per-user data, made here so the guard can resolve it. Empty when test mode
    // did not take, which the guard reads as outside.
    static QString isolatedRoot() {
#ifdef Q_OS_WIN
        QStandardPaths::setTestModeEnabled(true);
        const QString root =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (!root.endsWith(QLatin1String("/qttest"))) return QString();
        const QString path = defaultRemorarcPath();
        if (path.startsWith(root + QLatin1Char('/')))
            QDir().mkpath(QFileInfo(path).absolutePath());
        return root;
#else
        return QStringLiteral(REMORA_TEST_HOME);
#endif
    }

private slots:
    // Every case below writes to a QTemporaryDir, but the store's default path is the user's real
    // remorarc. The suite refuses to run at all unless the default path really resolves inside an
    // isolated root (isolatedRoot), so a later case that reaches the default path can only ever
    // touch that, and a passing suite proves the isolation.
    void initTestCase() {
        const QString root = isolatedRoot();
        const QString path = defaultRemorarcPath();
        if (!within(path, root))
            QFAIL(qPrintable(QStringLiteral("default remorarc %1 is not inside the isolated root "
                                            "%2; run the suite through ctest")
                                 .arg(path, root)));
    }

    // The guard above is only as good as within(): a symlinked leaf, a sibling whose name merely
    // starts with the root's, or a link that resolves nowhere must all count as outside.
    void withinFollowsLinksAndComparesComponents() {
        SKIP_ON_WINDOWS("QFile::link makes a .lnk shortcut on Windows, not a symbolic link");
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

    // bd remora-6laf.10 — addons= persists as a list; the layer a deploy built never does, for the
    // same reason the Play overlay does not.
    void addonsRoundTripAndTheLayerNeverPersists() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        RemoraConfig c;
        c.image.addons = QStringList{"arm_translate", "magisk_root"};
        c.image.addonOverlay = QStringLiteral("remora-local:addons-08c62ac56c4d-07db5d1f5dc7");
        saveInstance(path, QStringLiteral("x"), c);
        const RemoraConfig loaded = loadInstance(path, QStringLiteral("x"));
        QCOMPARE(loaded.image.addons, c.image.addons);
        QVERIFY(!loaded.image.addonOverlay.has_value());
        {
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("Instance-x"));
            for (const QString &k : s.allKeys())
                QVERIFY(!s.value(k).toString().contains(QStringLiteral("remora-local")));
        }
        c.image.addons.clear();
        saveInstance(path, QStringLiteral("x"), c);
        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("Instance-x"));
        QVERIFY(!s.contains(QStringLiteral("addons")));
    }

    // saveInstance and loadInstance each name every key by hand, in two independent lists, so a
    // typo or a missing line in either one drops that setting on the next GUI save. Every
    // persisted field gets its own distinct non-default value, so a dropped key comes back unset
    // and two swapped keys come back crossed. container_name is left out on purpose: the loader
    // derives it when unset, which its own test covers. The non-persisted overlay and loader
    // markers are left at their defaults.
    void everyPersistedKeyRoundTrips() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        RemoraConfig c;

        c.backend.backend = Backend::Remote;
        c.backend.sshHost = QStringLiteral("ssh-host-value");
        c.backend.dataDir = QStringLiteral("/srv/data-dir");
        c.backend.dataBaseDir = QStringLiteral("/srv/data-base-dir");
        c.backend.hostAdbPort = 1001;
        c.backend.adbBindAddress = QStringLiteral("10.0.0.2");
        c.backend.guestAccel = QStringLiteral("tcg");
        c.backend.guestMemoryMb = 1002;
        c.backend.guestCpus = 1003;

        c.display.width = 1004;
        c.display.height = 1005;
        c.display.dpi = 1006;
        c.display.fps = 1007;
        c.display.maxFps = 1008;
        c.display.useMemfd = false;
        c.display.timezone = QStringLiteral("Etc/GMT-3");

        c.gpu.mode = GpuMode::Host;
        c.gpu.gpuNode = QStringLiteral("/dev/dri/renderD201");
        c.gpu.gpuDriver = QStringLiteral("gpu-driver-value");
        c.gpu.gralloc = QStringLiteral("gralloc-value");
        c.gpu.hwcomposer = QStringLiteral("hwcomposer-value");
        c.gpu.vulkan = QStringLiteral("vulkan-value");
        c.gpu.vaDriver = QStringLiteral("va-driver-value");
        c.gpu.venus = false;
        c.gpu.venusSocketDir = QStringLiteral("/srv/venus-socket-dir");
        c.gpu.venusLibDir = QStringLiteral("/srv/venus-lib-dir");
        c.gpu.venusServerPath = QStringLiteral("/srv/venus-server");

        c.image.imageTag = QStringLiteral("image-tag-value");
        c.image.features = QStringList{QStringLiteral("feature_a"), QStringLiteral("feature_b")};
        c.image.androidVersion = 1009;
        c.image.buildHost = QStringLiteral("build-host-value");
        c.image.sourceHost = QStringLiteral("source-host-value");
        c.image.buildDir = QStringLiteral("/srv/build-dir");
        c.image.sourceTree = QStringLiteral("/srv/source-tree");
        c.image.sourceKind = QStringLiteral("source-kind-value");
        c.image.sourcePinned = true;
        c.image.buildNice = 1010;
        c.image.buildJobs = 1011;
        c.image.buildMemGiB = 1012;
        c.image.soongMemGiB = 1013;
        c.image.sourceRepo = QStringLiteral("source-repo-value");
        c.image.sourceRef = QStringLiteral("source-ref-value");
        c.image.builtCommit = QStringLiteral("built-commit-value");
        c.image.builtRecipe = QStringLiteral("built-recipe-value");
        c.image.sourcePatches =
            QStringList{QStringLiteral("minigbm_rendernode"), QStringLiteral("soong_gomemlimit")};
        c.image.archVariant = QStringLiteral("arch-variant-value");
        c.image.googlePlay = true;
        c.image.playTerms = QStringLiteral("play-terms-value");
        c.image.addons = QStringList{QStringLiteral("addon_a"), QStringLiteral("addon_b")};
        c.image.outputDir = QStringLiteral("/srv/output-dir");

        c.network.networkMode = QStringLiteral("network-mode-value");
        c.network.dockerNetwork = QStringLiteral("docker-network-value");
        c.network.macvlanIp = QStringLiteral("10.0.1.1");
        c.network.macvlanParent = QStringLiteral("macvlan-parent-value");
        c.network.macvlanSubnet = QStringLiteral("10.0.2.0/24");
        c.network.macvlanGateway = QStringLiteral("10.0.3.1");
        c.network.macvlanRange = QStringLiteral("10.0.4.0/28");
        c.network.macvlanHostRoute = false;
        c.network.macvlanShimIp = QStringLiteral("10.0.5.1");
        c.network.dns = QStringList{QStringLiteral("10.0.6.1"), QStringLiteral("10.0.6.2")};
        c.network.proxyType = QStringLiteral("proxy-type-value");
        c.network.proxyHost = QStringLiteral("proxy-host-value");
        c.network.proxyPort = 1014;

        c.input.mouseBind = QStringLiteral("mouse-bind-value");
        c.input.shortcutMod = QStringLiteral("lctrl,ralt");
        c.input.keyBind = QStringLiteral("F7:b,F8:h");
        c.input.kbdIdentity = QStringLiteral("kbd-identity-value");
        c.input.cameraDevices =
            QStringList{QStringLiteral("/dev/video7"), QStringLiteral("/dev/video8")};
        c.input.sharedInputs = QStringList{QStringLiteral("Example Desk Keyboard"),
                                           QStringLiteral("Example Wireless Mouse")};
        c.input.cpufreqTopology = false;

        c.mirror.videoBitRate = QStringLiteral("video-bit-rate-value");
        c.mirror.videoCodec = QStringLiteral("video-codec-value");
        c.mirror.hostEncode = true;
        c.mirror.hostDecode = true;
        c.mirror.hostEncodeMaxSize = 1015;
        c.mirror.hwDecode = false;
        c.mirror.audio = true;
        c.mirror.maxSize = 1016;
        c.mirror.windowWidth = 1017;
        c.mirror.windowHeight = 1018;
        c.mirror.windowX = 1019;
        c.mirror.windowY = 1020;
        c.mirror.pipWidth = 1021;
        c.mirror.pipHeight = 1022;
        c.mirror.windowFullscreen = true;
        c.mirror.fullscreenSeparateDisplay = true;
        c.mirror.fullscreenDisplay = QStringLiteral("fullscreen-display-value");
        c.mirror.fullscreenApp = QStringLiteral("fullscreen-app-value");
        c.mirror.freeformDisplay = QStringLiteral("freeform-display-value");
        c.mirror.desktopNewWindow = true;
        c.mirror.extra = QStringList{QStringLiteral("--extra-a"), QStringLiteral("--extra-b")};

        c.integration.desktopMenu = false;
        c.integration.appModes =
            QStringList{QStringLiteral("app.a=desktop"), QStringLiteral("app.b=freeform")};
        c.integration.autoUpdateApps = true;
        c.integration.autoUpdateHours = 1023;
        c.integration.armApps = QStringList{QStringLiteral("arm.a"), QStringLiteral("arm.b")};
        c.integration.armGuard = false;
        c.integration.sharedFolders =
            QStringList{QStringLiteral("/srv/share-a:a"), QStringLiteral("/srv/share-b:b")};
        c.integration.companionApps =
            QStringList{QStringLiteral("/srv/companion-a"), QStringLiteral("/srv/companion-b")};

        c.advanced.useOverlayfs = false;
        c.advanced.useCodec2 = false;
        c.advanced.autoSleepMinutes = 1024;
        c.advanced.roOverrides =
            QStringList{QStringLiteral("ro.a=1"), QStringLiteral("ro.b=2")};
        c.advanced.rootHiding = QStringLiteral("root-hiding-value");
        c.advanced.playSpoof = true;
        c.advanced.denylistPackages =
            QStringList{QStringLiteral("deny.a"), QStringLiteral("deny.b")};

        saveInstance(path, QStringLiteral("x"), c);
        const RemoraConfig l = loadInstance(path, QStringLiteral("x"));

        QCOMPARE(l.backend.backend, c.backend.backend);
        QCOMPARE(l.backend.sshHost, c.backend.sshHost);
        QCOMPARE(l.backend.dataDir, c.backend.dataDir);
        QCOMPARE(l.backend.dataBaseDir, c.backend.dataBaseDir);
        QCOMPARE(l.backend.hostAdbPort, c.backend.hostAdbPort);
        QCOMPARE(l.backend.adbBindAddress, c.backend.adbBindAddress);
        QCOMPARE(l.backend.guestAccel, c.backend.guestAccel);
        QCOMPARE(l.backend.guestMemoryMb, c.backend.guestMemoryMb);
        QCOMPARE(l.backend.guestCpus, c.backend.guestCpus);

        QCOMPARE(l.display.width, c.display.width);
        QCOMPARE(l.display.height, c.display.height);
        QCOMPARE(l.display.dpi, c.display.dpi);
        QCOMPARE(l.display.fps, c.display.fps);
        QCOMPARE(l.display.maxFps, c.display.maxFps);
        QCOMPARE(l.display.useMemfd, c.display.useMemfd);
        QCOMPARE(l.display.timezone, c.display.timezone);

        QCOMPARE(l.gpu.mode, c.gpu.mode);
        QCOMPARE(l.gpu.gpuNode, c.gpu.gpuNode);
        QCOMPARE(l.gpu.gpuDriver, c.gpu.gpuDriver);
        QCOMPARE(l.gpu.gralloc, c.gpu.gralloc);
        QCOMPARE(l.gpu.hwcomposer, c.gpu.hwcomposer);
        QCOMPARE(l.gpu.vulkan, c.gpu.vulkan);
        QCOMPARE(l.gpu.vaDriver, c.gpu.vaDriver);
        QCOMPARE(l.gpu.venus, c.gpu.venus);
        QCOMPARE(l.gpu.venusSocketDir, c.gpu.venusSocketDir);
        QCOMPARE(l.gpu.venusLibDir, c.gpu.venusLibDir);
        QCOMPARE(l.gpu.venusServerPath, c.gpu.venusServerPath);

        QCOMPARE(l.image.imageTag, c.image.imageTag);
        QCOMPARE(l.image.features, c.image.features);
        QCOMPARE(l.image.androidVersion, c.image.androidVersion);
        QCOMPARE(l.image.buildHost, c.image.buildHost);
        QCOMPARE(l.image.sourceHost, c.image.sourceHost);
        QCOMPARE(l.image.buildDir, c.image.buildDir);
        QCOMPARE(l.image.sourceTree, c.image.sourceTree);
        QCOMPARE(l.image.sourceKind, c.image.sourceKind);
        QCOMPARE(l.image.sourcePinned, c.image.sourcePinned);
        QCOMPARE(l.image.buildNice, c.image.buildNice);
        QCOMPARE(l.image.buildJobs, c.image.buildJobs);
        QCOMPARE(l.image.buildMemGiB, c.image.buildMemGiB);
        QCOMPARE(l.image.soongMemGiB, c.image.soongMemGiB);
        QCOMPARE(l.image.sourceRepo, c.image.sourceRepo);
        QCOMPARE(l.image.sourceRef, c.image.sourceRef);
        QCOMPARE(l.image.builtCommit, c.image.builtCommit);
        QCOMPARE(l.image.builtRecipe, c.image.builtRecipe);
        QCOMPARE(l.image.sourcePatches, c.image.sourcePatches);
        QCOMPARE(l.image.archVariant, c.image.archVariant);
        QCOMPARE(l.image.googlePlay, c.image.googlePlay);
        QCOMPARE(l.image.playTerms, c.image.playTerms);
        QCOMPARE(l.image.addons, c.image.addons);
        QCOMPARE(l.image.outputDir, c.image.outputDir);

        QCOMPARE(l.network.networkMode, c.network.networkMode);
        QCOMPARE(l.network.dockerNetwork, c.network.dockerNetwork);
        QCOMPARE(l.network.macvlanIp, c.network.macvlanIp);
        QCOMPARE(l.network.macvlanParent, c.network.macvlanParent);
        QCOMPARE(l.network.macvlanSubnet, c.network.macvlanSubnet);
        QCOMPARE(l.network.macvlanGateway, c.network.macvlanGateway);
        QCOMPARE(l.network.macvlanRange, c.network.macvlanRange);
        QCOMPARE(l.network.macvlanHostRoute, c.network.macvlanHostRoute);
        QCOMPARE(l.network.macvlanShimIp, c.network.macvlanShimIp);
        QCOMPARE(l.network.dns, c.network.dns);
        QCOMPARE(l.network.proxyType, c.network.proxyType);
        QCOMPARE(l.network.proxyHost, c.network.proxyHost);
        QCOMPARE(l.network.proxyPort, c.network.proxyPort);

        QCOMPARE(l.input.mouseBind, c.input.mouseBind);
        QCOMPARE(l.input.shortcutMod, c.input.shortcutMod);
        QCOMPARE(l.input.keyBind, c.input.keyBind);
        QCOMPARE(l.input.kbdIdentity, c.input.kbdIdentity);
        QCOMPARE(l.input.cameraDevices, c.input.cameraDevices);
        QCOMPARE(l.input.sharedInputs, c.input.sharedInputs);
        QCOMPARE(l.input.cpufreqTopology, c.input.cpufreqTopology);

        QCOMPARE(l.mirror.videoBitRate, c.mirror.videoBitRate);
        QCOMPARE(l.mirror.videoCodec, c.mirror.videoCodec);
        QCOMPARE(l.mirror.hostEncode, c.mirror.hostEncode);
        QCOMPARE(l.mirror.hostDecode, c.mirror.hostDecode);
        QCOMPARE(l.mirror.hostEncodeMaxSize, c.mirror.hostEncodeMaxSize);
        QCOMPARE(l.mirror.hwDecode, c.mirror.hwDecode);
        QCOMPARE(l.mirror.audio, c.mirror.audio);
        QCOMPARE(l.mirror.maxSize, c.mirror.maxSize);
        QCOMPARE(l.mirror.windowWidth, c.mirror.windowWidth);
        QCOMPARE(l.mirror.windowHeight, c.mirror.windowHeight);
        QCOMPARE(l.mirror.windowX, c.mirror.windowX);
        QCOMPARE(l.mirror.windowY, c.mirror.windowY);
        QCOMPARE(l.mirror.pipWidth, c.mirror.pipWidth);
        QCOMPARE(l.mirror.pipHeight, c.mirror.pipHeight);
        QCOMPARE(l.mirror.windowFullscreen, c.mirror.windowFullscreen);
        QCOMPARE(l.mirror.fullscreenSeparateDisplay, c.mirror.fullscreenSeparateDisplay);
        QCOMPARE(l.mirror.fullscreenDisplay, c.mirror.fullscreenDisplay);
        QCOMPARE(l.mirror.fullscreenApp, c.mirror.fullscreenApp);
        QCOMPARE(l.mirror.freeformDisplay, c.mirror.freeformDisplay);
        QCOMPARE(l.mirror.desktopNewWindow, c.mirror.desktopNewWindow);
        QCOMPARE(l.mirror.extra, c.mirror.extra);

        QCOMPARE(l.integration.desktopMenu, c.integration.desktopMenu);
        QCOMPARE(l.integration.appModes, c.integration.appModes);
        QCOMPARE(l.integration.autoUpdateApps, c.integration.autoUpdateApps);
        QCOMPARE(l.integration.autoUpdateHours, c.integration.autoUpdateHours);
        QCOMPARE(l.integration.armApps, c.integration.armApps);
        QCOMPARE(l.integration.armGuard, c.integration.armGuard);
        QCOMPARE(l.integration.sharedFolders, c.integration.sharedFolders);
        QCOMPARE(l.integration.companionApps, c.integration.companionApps);

        QCOMPARE(l.advanced.useOverlayfs, c.advanced.useOverlayfs);
        QCOMPARE(l.advanced.useCodec2, c.advanced.useCodec2);
        QCOMPARE(l.advanced.autoSleepMinutes, c.advanced.autoSleepMinutes);
        QCOMPARE(l.advanced.roOverrides, c.advanced.roOverrides);
        QCOMPARE(l.advanced.rootHiding, c.advanced.rootHiding);
        QCOMPARE(l.advanced.playSpoof, c.advanced.playSpoof);
        QCOMPARE(l.advanced.denylistPackages, c.advanced.denylistPackages);
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

    // The loader trims adb_bind and gpu_mode, so a padded value reads cleanly. QSettings quotes a
    // padded value on write and keeps the padding on read, which the first block checks: without
    // it the store's trim would have nothing to do. gpuModeFromString trims too, so a padded valid
    // mode alone cannot tell whether the store trimmed; a padded unrecognised one, kept aside for
    // preflight to quote back, can.
    void loadInstanceTrimsAdbBindAndGpuMode() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-x/adb_bind", QStringLiteral(" 127.0.0.1 "));
            s.setValue("Instance-x/gpu_mode", QStringLiteral(" host "));
            s.setValue("Instance-y/gpu_mode", QStringLiteral(" hots "));
            s.sync();
        }
        {
            QSettings s(path, QSettings::IniFormat);
            QCOMPARE(s.value("Instance-x/adb_bind").toString(), QStringLiteral(" 127.0.0.1 "));
            QCOMPARE(s.value("Instance-x/gpu_mode").toString(), QStringLiteral(" host "));
            QCOMPARE(s.value("Instance-y/gpu_mode").toString(), QStringLiteral(" hots "));
        }
        const RemoraConfig c = loadInstance(path, QStringLiteral("x"));
        QCOMPARE(c.backend.adbBindAddress, std::optional<QString>(QStringLiteral("127.0.0.1")));
        QCOMPARE(c.gpu.mode, std::optional<GpuMode>(GpuMode::Host));
        QVERIFY(!c.gpu.unrecognisedMode.has_value());

        const RemoraConfig y = loadInstance(path, QStringLiteral("y"));
        QVERIFY(!y.gpu.mode.has_value());
        QCOMPARE(y.gpu.unrecognisedMode, std::optional<QString>(QStringLiteral("hots")));
    }

    // shortcut_mod is comma-separated alternatives, and a comma is QSettings' INI list separator:
    // a hand-written unquoted value arrives as a QStringList, whose toString() is empty — the
    // setting silently became the client default. The store must not drop what the user wrote.
    void loadInstanceJoinsHandWrittenCommaShortcutMod() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("[Instance-x]\nshortcut_mod=lctrl,lalt\n");
        }
        QCOMPARE(loadInstance(path, QStringLiteral("x")).input.shortcutMod,
                 std::optional<QString>(QStringLiteral("lctrl,lalt")));
    }

    // bd remora-82c.5 — source_patches is a pin, so the loader maps a retired key onto the entry
    // that absorbed it; the profile keeps the capability, in order, and the next save writes the
    // canonical spelling back.
    void loadInstanceCanonicalisesPinnedSourcePatches() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-x/source_patches",
                       QStringLiteral("minigbm_rendernode statusbar_clock_hide soong_gomemlimit"));
            s.sync();
        }
        const QStringList canonical{QStringLiteral("minigbm_rendernode"),
                                    QStringLiteral("statusbar_hide_list"),
                                    QStringLiteral("soong_gomemlimit")};
        const RemoraConfig c = loadInstance(path, QStringLiteral("x"));
        QCOMPARE(c.image.sourcePatches, canonical);

        saveInstance(path, QStringLiteral("x"), c);
        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("Instance-x"));
        QCOMPARE(s.value("source_patches").toString(),
                 QStringLiteral("minigbm_rendernode statusbar_hide_list soong_gomemlimit"));
        QCOMPARE(loadInstance(path, QStringLiteral("x")).image.sourcePatches, canonical);
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

    void imageSourcesRoundTripFields() {
        QTemporaryDir dir;
        const QString path = rc(dir);
        ImageSource a;
        a.name = QStringLiteral("a");
        a.enabled = false;
        a.tags = {QStringLiteral("x:1"), QStringLiteral("y:2")};
        a.location = QStringLiteral("https://img.example");
        ImageSource b;
        b.name = QStringLiteral("b");
        b.enabled = true;
        b.tags = {QStringLiteral("z:3")};
        saveImageSources(path, {a, b});
        {
            QSettings s(path, QSettings::IniFormat);
            QVERIFY(s.contains(QStringLiteral("Source-a/location")));
            QVERIFY(!s.contains(QStringLiteral("Source-b/location")));
            // Hand-planted, as an older or hand-edited remorarc would have them: no enabled key.
            s.setValue("Source-c/tags", QStringLiteral("t:1"));
            s.setValue("Source-d/location", QStringLiteral("  https://pad.example  "));
            s.sync();
        }

        QHash<QString, ImageSource> byName;
        for (const ImageSource &src : loadImageSources(path)) byName.insert(src.name, src);
        QCOMPARE(byName.size(), 4);
        QVERIFY(byName.contains(QStringLiteral("a")));
        QCOMPARE(byName[QStringLiteral("a")].enabled, false);
        QCOMPARE(byName[QStringLiteral("a")].tags, a.tags);
        QCOMPARE(byName[QStringLiteral("a")].location, a.location);
        QVERIFY(byName.contains(QStringLiteral("b")));
        QCOMPARE(byName[QStringLiteral("b")].enabled, true);
        QCOMPARE(byName[QStringLiteral("b")].tags, b.tags);
        QVERIFY(byName[QStringLiteral("b")].location.isEmpty());
        QVERIFY(byName.contains(QStringLiteral("c")));
        QCOMPARE(byName[QStringLiteral("c")].enabled, true);
        QCOMPARE(byName[QStringLiteral("c")].tags, QStringList{QStringLiteral("t:1")});
        QVERIFY(byName.contains(QStringLiteral("d")));
        QCOMPARE(byName[QStringLiteral("d")].enabled, true);
        QCOMPARE(byName[QStringLiteral("d")].location, QStringLiteral("https://pad.example"));
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

    // bd remora-7oih.2 — no systemctl at all: a Windows client, or a Linux without systemd. Every
    // call used to fail, the state read "unknown", and the unit was kept — so Delete profile could
    // never delete a profile there. Nothing can have loaded a unit without a manager: ask once,
    // delete whatever unit files exist, skip the reload, and succeed.
    void removeWithNoServiceManagerDeletesTheFiles() {
        QTemporaryDir dir;
        const QString unitDir = dir.path();
        write(unitDir + QStringLiteral("/remora-p.service"), "[Unit]\n");
        write(unitDir + QStringLiteral("/remora-updater-p.service.d/override.conf"), "o\n");
        write(unitDir + QStringLiteral("/remora-other.service"), "keep\n");
        FakeManager m;
        m.noManager = true;
        const UnitRemovalResult r = removeServiceUnits(unitDir, QStringLiteral("p"), m.runner());
        QVERIFY2(r.ok, qPrintable(r.errors.join(QLatin1Char('\n'))));
        QVERIFY(r.noServiceManager);
        QCOMPARE(m.calls.size(), 1);  // the first disable said it all
        QVERIFY(!m.ran(QStringLiteral("daemon-reload"), {}));
        QVERIFY(!QFileInfo::exists(unitDir + QStringLiteral("/remora-p.service")));
        QVERIFY(!QFileInfo::exists(unitDir + QStringLiteral("/remora-updater-p.service.d")));
        QCOMPARE(read(unitDir + QStringLiteral("/remora-other.service")), QByteArray("keep\n"));
        QCOMPARE(r.removed.size(), 2);
        // and on a machine that never had a unit, nothing is created where none was
        QTemporaryDir empty;
        const QString none = empty.filePath(QStringLiteral("systemd/user"));
        QVERIFY(removeServiceUnits(none, QStringLiteral("p"), m.runner()).ok);
        QVERIFY(!QFileInfo::exists(none));
    }

    // …which is what makes the workspace's Delete profile work there.
    void deleteProfileWithNoServiceManager() {
        QTemporaryDir dir;
        const QString unitDir = dir.filePath(QStringLiteral("units"));
        const QString path = rc(dir);
        seedProfiles(path, unitDir);
        FakeManager m;
        m.noManager = true;
        int hooks = 0;
        const ProfileDeletionResult r = deleteProfileAndServices(
            path, unitDir, QStringLiteral("p"), m.runner(), [&](const QString &) { ++hooks; });
        QVERIFY2(r.deleted, qPrintable(r.error));
        QCOMPARE(hooks, 1);
        QCOMPARE(listInstances(path), QStringList{QStringLiteral("other")});
        expectUnrelatedIntact(path, unitDir);
    }

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
        SKIP_ON_WINDOWS("QFile::link makes a .lnk shortcut on Windows, not a symbolic link");
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

    // ─── workspace Delete profile: deleteProfileAndServices, the coordinator onDeleteProfile runs ───
    // The app-menu step is the GUI's callback (removeAppMenuFolder); here it only records when it
    // ran, so what is proven is its ordering, not the menu files themselves.

    // The named regression: every disable and the reload run while the section still exists, the
    // callback runs after the reload, and then exactly that section goes.
    void deleteProfileRemovesServicesBeforeSection() {
        QTemporaryDir dir;
        const QString unitDir = dir.filePath(QStringLiteral("units"));
        const QString path = rc(dir);
        seedProfiles(path, unitDir);
        FakeManager m;
        m.unitDir = unitDir;
        m.allPresent(QStringLiteral("p"));
        bool sectionThroughout = true;
        const UnitCommandRunner inner = m.runner();
        const UnitCommandRunner run = [&](const QStringList &v) {
            if (!listInstances(path).contains(QStringLiteral("p"))) sectionThroughout = false;
            return inner(v);
        };
        QStringList hookRan;
        bool hookSawSection = false, hookAfterReload = false;
        const ProfileDeletionResult r = deleteProfileAndServices(
            path, unitDir, QStringLiteral("p"), run, [&](const QString &profile) {
                hookRan << profile;
                hookSawSection = listInstances(path).contains(QStringLiteral("p"));
                hookAfterReload =
                    m.calls.last() == QStringList{"systemctl", "--user", "daemon-reload"};
            });
        QVERIFY2(r.deleted, qPrintable(r.error));
        QVERIFY(r.error.isEmpty());
        QVERIFY(sectionThroughout);
        QCOMPARE(hookRan, QStringList{QStringLiteral("p")});
        QVERIFY(hookSawSection);
        QVERIFY(hookAfterReload);
        for (const QString &u : serviceUnitNames(QStringLiteral("p"))) {
            QVERIFY2(m.ran(QStringLiteral("disable"), u), qPrintable(u));
            QVERIFY2(m.fileExistedAtDisable.value(u), qPrintable(u));
            QVERIFY(!QFileInfo::exists(unitDir + '/' + u));
            QVERIFY(!QFileInfo::exists(unitDir + '/' + u + QStringLiteral(".d")));
        }
        QCOMPARE(r.units.removed.size(), 6);
        QCOMPARE(listInstances(path), QStringList{QStringLiteral("other")});
        expectUnrelatedIntact(path, unitDir);
    }

    // A profile that never installed a service: every unit is confirmed not-found and inactive.
    void deleteProfileWithoutServicesSucceeds() {
        QTemporaryDir dir;
        const QString unitDir = dir.filePath(QStringLiteral("units"));
        const QString path = rc(dir);
        seedProfiles(path, unitDir);
        for (const QString &u : serviceUnitNames(QStringLiteral("p"))) {
            QVERIFY(QFile::remove(unitDir + '/' + u));
            QVERIFY(QDir(unitDir + '/' + u + QStringLiteral(".d")).removeRecursively());
        }
        FakeManager m;
        m.absent(QStringLiteral("p"));
        int hooks = 0;
        const ProfileDeletionResult r = deleteProfileAndServices(
            path, unitDir, QStringLiteral("p"), m.runner(), [&](const QString &) { ++hooks; });
        QVERIFY2(r.deleted, qPrintable(r.error));
        QCOMPARE(hooks, 1);
        QVERIFY(r.units.removed.isEmpty());
        QCOMPARE(listInstances(path), QStringList{QStringLiteral("other")});
        expectUnrelatedIntact(path, unitDir);
    }

    // Any cleanup failure keeps the profile, skips the menu step and explains itself.
    void deleteProfileKeepsSectionWhenCleanupFails_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("loaded unit disable fails") << 0;
        QTest::newRow("running orphan stop fails") << 1;
        QTest::newRow("manager unreachable") << 2;
        QTest::newRow("daemon-reload fails") << 3;
    }
    void deleteProfileKeepsSectionWhenCleanupFails() {
        QFETCH(int, kind);
        QTemporaryDir dir;
        const QString unitDir = dir.filePath(QStringLiteral("units"));
        const QString path = rc(dir);
        seedProfiles(path, unitDir);
        FakeManager m;
        m.allPresent(QStringLiteral("p"));
        const QString main = QStringLiteral("remora-p.service");
        const QString updater = QStringLiteral("remora-updater-p.service");
        QString expect;
        if (kind == 0) {
            m.units[main].disableRc = 1;
            expect = QStringLiteral("LoadState=loaded");
        } else if (kind == 1) {
            m.units[updater] = {QStringLiteral("not-found"), QStringLiteral("active"), 1, 1};
            m.units[updater].stopErr = QStringLiteral("Failed to stop: Access denied");
            expect = QStringLiteral("Access denied");
        } else if (kind == 2) {
            m.units[main].disableRc = 1;
            m.units[main].overrideQuery = true;
            m.units[main].queryRc = 1;
            expect = QStringLiteral("manager state unknown");
        } else {
            m.reloadRc = 1;
            expect = QStringLiteral("daemon-reload failed");
        }
        int hooks = 0;
        const ProfileDeletionResult r = deleteProfileAndServices(
            path, unitDir, QStringLiteral("p"), m.runner(), [&](const QString &) { ++hooks; });
        QVERIFY(!r.deleted);
        QCOMPARE(hooks, 0);
        QVERIFY(r.error.contains(QStringLiteral("'p' was kept")));
        QVERIFY2(r.error.contains(expect), qPrintable(r.error));
        QVERIFY(m.ran(QStringLiteral("daemon-reload"), {}));
        QCOMPARE(listInstances(path), (QStringList{QStringLiteral("other"), QStringLiteral("p")}));
        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("Instance-p/width").toInt(), 720);
        // units removed before the failure stay removed, and the message does not hide it
        if (!r.units.removed.isEmpty())
            QVERIFY(r.error.contains(QStringLiteral("Already removed (not restored)")));
        if (kind == 3) QCOMPARE(r.units.removed.size(), 6);
        if (kind == 0 || kind == 2)
            QCOMPARE(read(unitDir + '/' + main), QByteArray("[Unit]\n"));
        if (kind == 1)
            QCOMPARE(read(unitDir + '/' + updater + QStringLiteral(".d/override.conf")),
                     QByteArray("o\n"));
        expectUnrelatedIntact(path, unitDir);
    }

    // The CLI's collision refusal applies to the workspace too, before any side effect.
    void deleteProfileRefusesSharedUnits_data() {
        QTest::addColumn<QString>("victim");
        QTest::addColumn<QString>("owner");
        QTest::newRow("escaping") << QStringLiteral("a b") << QStringLiteral("a-b");
        QTest::newRow("family") << QStringLiteral("updater-x") << QStringLiteral("x");
    }
    void deleteProfileRefusesSharedUnits() {
        QFETCH(QString, victim);
        QFETCH(QString, owner);
        QTemporaryDir dir;
        const QString unitDir = dir.filePath(QStringLiteral("units"));
        const QString path = rc(dir);
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue(QStringLiteral("Instance-%1/width").arg(victim), 1);
            s.setValue(QStringLiteral("Instance-%1/width").arg(owner), 2);
            s.sync();
        }
        for (const QString &u : serviceUnitNames(victim) + serviceUnitNames(owner))
            write(unitDir + '/' + u, "[Unit]\n");
        FakeManager m;
        m.allPresent(victim);
        m.allPresent(owner);
        int hooks = 0;
        const ProfileDeletionResult r = deleteProfileAndServices(
            path, unitDir, victim, m.runner(), [&](const QString &) { ++hooks; });
        QVERIFY(!r.deleted);
        QCOMPARE(hooks, 0);
        QVERIFY(m.calls.isEmpty());
        QVERIFY2(r.error.contains(QStringLiteral("belongs to profile '%1'").arg(owner)),
                 qPrintable(r.error));
        QVERIFY(listInstances(path).contains(victim));
        QVERIFY(listInstances(path).contains(owner));
        for (const QString &u : serviceUnitNames(victim) + serviceUnitNames(owner))
            QVERIFY(QFileInfo::exists(unitDir + '/' + u));
    }

private:
    // Profiles p (to delete) and other, [Defaults], p's three units each with a drop-in, other's
    // units, and lookalikes that must survive.
    static void seedProfiles(const QString &path, const QString &unitDir) {
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue("Instance-p/width", 720);
            s.setValue("Instance-other/width", 800);
            s.setValue("Defaults/fps", 60);
            s.sync();
        }
        for (const QString &u : serviceUnitNames(QStringLiteral("p"))) {
            write(unitDir + '/' + u, "[Unit]\n");
            write(unitDir + '/' + u + QStringLiteral(".d/override.conf"), "o\n");
        }
        for (const QString &u : serviceUnitNames(QStringLiteral("other"))) {
            write(unitDir + '/' + u, "[Unit]\n");
            write(unitDir + '/' + u + QStringLiteral(".d/override.conf"), "o\n");
        }
        write(unitDir + QStringLiteral("/remora-pp.service"), "k\n");
        write(unitDir + QStringLiteral("/unrelated.service"), "k\n");
    }
    static void expectUnrelatedIntact(const QString &path, const QString &unitDir) {
        QSettings s(path, QSettings::IniFormat);
        QCOMPARE(s.value("Instance-other/width").toInt(), 800);
        QCOMPARE(s.value("Defaults/fps").toInt(), 60);
        for (const QString &u : serviceUnitNames(QStringLiteral("other"))) {
            QCOMPARE(read(unitDir + '/' + u), QByteArray("[Unit]\n"));
            QCOMPARE(read(unitDir + '/' + u + QStringLiteral(".d/override.conf")), QByteArray("o\n"));
        }
        QVERIFY(QFileInfo::exists(unitDir + QStringLiteral("/remora-pp.service")));
        QVERIFY(QFileInfo::exists(unitDir + QStringLiteral("/unrelated.service")));
    }

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
        void absent(const QString &profile) {
            for (const QString &u : serviceUnitNames(profile))
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
        // No systemctl on this machine at all: every call is the spawner's could-not-start.
        bool noManager = false;
        UnitCommandRunner runner() {
            return [this](const QStringList &v) -> UnitCommandResult {
                calls << v;
                if (noManager)
                    return {kNoServiceManagerRc, {}, QStringLiteral("failed to start systemctl")};
                if (v.size() < 3 || v[0] != QLatin1String("systemctl") ||
                    v[1] != QLatin1String("--user"))
                    return {99, {}, QStringLiteral("unexpected argv")};
                const QString verb = v[2];
                if (verb == QLatin1String("daemon-reload"))
                    return {reloadRc, {}, reloadRc ? QStringLiteral("reload refused") : QString()};
                const QString unit = v.last();
                if (!units.contains(unit)) return {99, {}, QStringLiteral("unmodelled unit")};
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
                return {99, {}, QStringLiteral("unexpected verb")};
            };
        }
    };
};

QTEST_MAIN(TestStore)
#include "test_store.moc"
