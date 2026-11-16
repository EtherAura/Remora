#include <QtTest>

#include "core/ImageBuild.h"
#include "core/Resolver.h"
#include "engine/Addons.h"

using namespace remora;

// bd remora-6laf.10 — the addons= profile option, materialized for one deploy.
class AddonSpawner : public Spawner {
public:
    QVector<QStringList> calls;
    QVector<QMap<QString, QString>> envs;
    // `docker image inspect` of the image the layer goes over: "<id> <org.remora.play.base>".
    QString baseInspect = QStringLiteral("sha256:08c62ac56c4d0000 <no value>\n");
    // `docker image inspect` of the layer's own tag, asking its recipe label.
    ProcResult have{1, {}, QStringLiteral("No such image")};
    ProcResult build{0, {}, {}};
    QString dockerfile;  // what docker build was given, read while the context still exists
    QString fetchInto;   // set: a fetch-payloads.sh call creates the Magisk payload there
    ProcResult run(const QStringList &argv, const QMap<QString, QString> &env = {},
                   const LineSink & = {}) override {
        calls << argv;
        envs << env;
        const QString j = argv.join(QLatin1Char(' '));
        if (j.contains(QLatin1String("{{.Id}}"))) return {0, baseInspect, {}};
        if (j.contains(QLatin1String("org.remora.addons.recipe"))) return have;
        if (j.contains(QLatin1String("fetch-payloads.sh"))) {
            if (!fetchInto.isEmpty()) writeMagisk(env.value("REMORA_PAYLOAD_DIR"));
            return {fetchInto.isEmpty() ? 1 : 0, {}, {}};
        }
        if (j.contains(QLatin1String("docker build"))) {
            // Local: docker build -t <tag> <dir>. Remote: tar -C '<dir>' -cf - . | ssh … docker build.
            QString dir = argv.value(0) == QLatin1String("docker") ? argv.value(4) : QString();
            if (dir.isEmpty()) {
                const QRegularExpressionMatch m =
                    QRegularExpression(QStringLiteral("tar -C '([^']+)'")).match(j);
                dir = m.captured(1);
            }
            QFile f(dir + QStringLiteral("/Dockerfile"));
            if (f.open(QIODevice::ReadOnly)) dockerfile = QString::fromUtf8(f.readAll());
            return build;
        }
        return {0, {}, {}};
    }
    Detached spawnDetached(const QStringList &, const QMap<QString, QString> &) override { return {}; }
    bool detachedAlive(const Detached &) override { return false; }

    static bool put(const QString &path) {
        if (!QDir().mkpath(QFileInfo(path).path())) return false;
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write("x\n") == 2;
    }
    static void writeMagisk(const QString &root) {
        for (const char *f : {"magisk", "magiskboot", "magiskinit", "magiskpolicy", "busybox",
                              "magisk.apk", "stub.apk", "util_functions.sh", "boot_patch.sh"})
            put(root + QStringLiteral("/magisk/magisk/") + QLatin1String(f));
    }
    bool built() const {
        for (const QStringList &c : calls)
            if (c.join(QLatin1Char(' ')).contains(QLatin1String("docker build"))) return true;
        return false;
    }
    bool ran(const QString &sub) const {
        for (const QStringList &c : calls)
            if (c.join(QLatin1Char(' ')).contains(sub)) return true;
        return false;
    }
};

class AddonsTest : public QObject {
    Q_OBJECT
    QTemporaryDir tmp_;
    AddonPaths paths() const {
        return {{tmp_.path() + QStringLiteral("/payloads"), tmp_.path() + QStringLiteral("/none")},
                QStringLiteral(REMORA_SOURCE_VENDOR_DIR),
                tmp_.path() + QStringLiteral("/cache")};
    }
    static RemoraConfig profile(const QStringList &addons) {
        RemoraConfig c;
        c.image.androidVersion = 17;
        c.image.imageTag = QStringLiteral("remora24:x86_64-public");
        c.image.addons = addons;
        return c;
    }
    void putArm() const {
        const QString r = tmp_.path() + QStringLiteral("/payloads/ndk_translation/");
        for (const char *f : {"lib64/libnb.so", "lib64/libndk_translation.so",
                              "guest/system/bin/arm64/linker64",
                              "bin/ndk_translation_program_runner_binfmt_misc_arm64",
                              "etc/cpuinfo.arm64.txt"})
            QVERIFY(AddonSpawner::put(r + QLatin1String(f)));
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // The resolver: the layer goes over whatever Play left in imageTag, and every question about
    // what the image can do is still asked of the registered base.
    void resolverSwapsTheLayerInLast() {
        RemoraConfig c = profile({"arm_translate", "magisk_root", "not_an_addon"});
        ResolvedConfig r = resolve(c, Backend::Bare);
        QCOMPARE(r.addons, (QStringList{"arm_translate", "magisk_root"}));  // unknown dropped
        QCOMPARE(r.addonsPending(), r.addons);
        QCOMPARE(r.imageTag, QString("remora24:x86_64-public"));  // nothing materialized yet
        c.image.addonOverlay = QStringLiteral("remora-local:addons-aaa-bbb");
        r = resolve(c, Backend::Bare);
        QCOMPARE(r.imageTag, QString("remora-local:addons-aaa-bbb"));
        QCOMPARE(r.addonBaseTag, QString("remora24:x86_64-public"));
        QCOMPARE(r.capabilityTag(), QString("remora24:x86_64-public"));
        // With Google Play too: the add-on layer is over the Play layer, capabilities its base's.
        c.image.googlePlay = true;
        c.image.playOverlay = QStringLiteral("remora-local:play-ccc-ddd");
        r = resolve(c, Backend::Bare);
        QCOMPARE(r.imageTag, QString("remora-local:addons-aaa-bbb"));
        QCOMPARE(r.addonBaseTag, QString("remora-local:play-ccc-ddd"));
        QCOMPARE(r.capabilityTag(), QString("remora24:x86_64-public"));
    }

    // A registered image that already bakes an add-on has nothing to add for it, and no layer
    // is swapped in for nothing.
    void resolverCountsWhatTheBaseCarries() {
        RemoraConfig c = profile({"magisk_root"});
        c.image.imageTag = QStringLiteral("remora24:x86_64-gapps");
        c.image.addonOverlay = QStringLiteral("remora-local:addons-aaa-bbb");
        const ResolvedConfig r = resolve(c, Backend::Bare);
        QCOMPARE(r.addonsIncluded, QStringList{"magisk_root"});
        QVERIFY(r.addonsPending().isEmpty());
        QCOMPARE(r.imageTag, QString("remora24:x86_64-gapps"));
    }

    void dockerfileCarriesTheLabelsAndTheTagBothHashes() {
        const QString df = addonLayerDockerfile({"magisk_root", "arm_translate"},
                                                QStringLiteral("remora-local:play-ccc-ddd"), 17,
                                                QStringLiteral("sha256:base"),
                                                QStringLiteral("sha256:root"),
                                                QStringLiteral("cafe"));
        QVERIFY(df.startsWith(QLatin1String("FROM remora-local:play-ccc-ddd\n")));
        QVERIFY(df.contains("COPY ndkguest/system/ /system/\n"));
        QVERIFY(df.contains("COPY magisk/magisk.rc /system/etc/init/magisk.rc\n"));
        QVERIFY(df.contains("LABEL remora.public_release=\"false\"\n"));
        QVERIFY(df.contains("LABEL org.remora.addons.list=\"arm_translate magisk_root\"\n"));
        QVERIFY(df.contains("LABEL org.remora.addons.base=\"sha256:base\"\n"));
        QVERIFY(df.contains("LABEL org.remora.addons.root=\"sha256:root\"\n"));
        QVERIFY(df.contains("LABEL org.remora.addons.recipe=\"cafe\"\n"));
        QVERIFY(!df.contains(QLatin1String("RUN ")));
        QCOMPARE(addonLayerTag(QStringLiteral("sha256:08c62ac56c4d0123"),
                               QStringLiteral("07db5d1f5dc7aaaa")),
                 QString("remora-local:addons-08c62ac56c4d-07db5d1f5dc7"));
    }

    void offOrUnknownRunsNothing() {
        AddonSpawner sp;
        RemoraConfig off = profile({});
        QVERIFY(ensureAddonOverlay(sp, off, Backend::Bare, {}, paths()).ok);
        RemoraConfig bad = profile({"gapps"});  // Play has its own option, not an add-on
        const StepResult r = ensureAddonOverlay(sp, bad, Backend::Bare, {}, paths());
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains(QLatin1String("'gapps'")));
        QVERIFY(sp.calls.isEmpty());
    }

    // ARM translation's payload cannot be fetched: refused by name, with the directory to put it
    // in, before anything touches Docker.
    void missingProprietaryPayloadIsRefusedByName() {
        AddonSpawner sp;
        RemoraConfig c = profile({"arm_translate"});
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Bare, {}, paths());
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(tmp_.path() + QStringLiteral("/payloads/ndk_translation")),
                 qPrintable(r.detail));
        QVERIFY(!sp.built());
        QVERIFY(!c.image.addonOverlay);
    }

    // Magisk is fetched when missing, into the user's payload directory, then layered.
    void magiskIsFetchedThenBuiltThenReused() {
        AddonSpawner sp;
        sp.fetchInto = tmp_.path() + QStringLiteral("/payloads");
        RemoraConfig c = profile({"magisk_root"});
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Bare, {}, paths());
        QVERIFY2(r.ok, qPrintable(r.detail + r.stderrTail));
        QVERIFY(sp.ran(QStringLiteral("fetch-payloads.sh magisk")));
        bool envOk = false;
        for (const auto &e : sp.envs)
            if (e.value("REMORA_PAYLOAD_DIR") == sp.fetchInto) envOk = true;
        QVERIFY(envOk);
        QVERIFY(sp.built());
        QVERIFY(r.detail.startsWith(QLatin1String("built remora-local:addons-08c62ac56c4d-")));
        QVERIFY(c.image.addonOverlay && c.image.addonOverlay->startsWith("remora-local:addons-"));
        QVERIFY(sp.dockerfile.startsWith(QLatin1String("FROM remora24:x86_64-public\n")));
        QVERIFY(sp.dockerfile.contains("LABEL org.remora.addons.root=\"sha256:08c62ac56c4d0000\""));
        // The staging copies the payload and Remora's own two files beside it.
        QVERIFY(sp.ran(QStringLiteral("/payloads/magisk/magisk")));
        QVERIFY(sp.ran(QStringLiteral("features/magisk/magisk-setup.sh")));
        // The context is gone once the step returns.
        QVERIFY(QDir(tmp_.path() + QStringLiteral("/cache")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

        // The next deploy finds the layer by its recipe label and builds nothing.
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("org.remora.addons.recipe=\"([0-9a-f]+)\"")).match(sp.dockerfile);
        QVERIFY(m.hasMatch());
        AddonSpawner again;
        again.have = {0, m.captured(1) + QLatin1Char('\n'), {}};
        RemoraConfig c2 = profile({"magisk_root"});
        const StepResult r2 = ensureAddonOverlay(again, c2, Backend::Bare, {}, paths());
        QVERIFY2(r2.ok, qPrintable(r2.detail));
        QVERIFY(r2.detail.startsWith(QLatin1String("reused ")));
        QVERIFY(!again.built());
        QCOMPARE(c2.image.addonOverlay, c.image.addonOverlay);
        // A changed payload is a different recipe: rebuilt, not reused.
        QVERIFY(AddonSpawner::put(tmp_.path() + QStringLiteral("/payloads/magisk/magisk/extra")));
        AddonSpawner changed;
        changed.have = again.have;
        RemoraConfig c3 = profile({"magisk_root"});
        QVERIFY(ensureAddonOverlay(changed, c3, Backend::Bare, {}, paths()).ok);
        QVERIFY(changed.built());
        QVERIFY(c3.image.addonOverlay != c.image.addonOverlay);
    }

    // Over Google Play: the layer goes over the Play layer, and its root label is the Play
    // layer's base, which is what an image update replaces.
    void overPlayRecordsThePlayBaseAsRoot() {
        putArm();
        AddonSpawner sp;
        sp.baseInspect = QStringLiteral("sha256:1848aaaa sha256:08c6root\n");
        RemoraConfig c = profile({"arm_translate"});
        c.image.googlePlay = true;
        c.image.playTerms = QStringLiteral("t");
        c.image.playOverlay = QStringLiteral("remora-local:play-08c62ac56c4d-07db5d1f5dc7");
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Bare, {}, paths());
        QVERIFY2(r.ok, qPrintable(r.detail + r.stderrTail));
        QVERIFY(sp.dockerfile.startsWith(
            QLatin1String("FROM remora-local:play-08c62ac56c4d-07db5d1f5dc7\n")));
        QVERIFY(sp.dockerfile.contains("LABEL org.remora.addons.base=\"sha256:1848aaaa\""));
        QVERIFY(sp.dockerfile.contains("LABEL org.remora.addons.root=\"sha256:08c6root\""));
        QVERIFY(c.image.addonOverlay->startsWith("remora-local:addons-1848aaaa-"));
        // ARM is staged by the same script the manual route uses.
        QVERIFY(sp.ran(QStringLiteral("stage-arm-layer.sh")));
        // and the deploy then runs the add-on layer, with capabilities still the public base's
        const ResolvedConfig rc = resolve(c, Backend::Bare);
        QCOMPARE(rc.imageTag, *c.image.addonOverlay);
        QCOMPARE(rc.capabilityTag(), QString("remora24:x86_64-public"));
    }

    // Remote: every Docker question over ssh, and the context streamed to `docker build -`.
    void remoteBuildsOnTheDockerHost() {
        putArm();
        AddonSpawner sp;
        RemoraConfig c = profile({"arm_translate"});
        c.backend.sshHost = QStringLiteral("user@dockerhost");
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Remote, {}, paths());
        QVERIFY2(r.ok, qPrintable(r.detail + r.stderrTail));
        bool inspectOverSsh = false, streamed = false;
        for (const QStringList &call : sp.calls) {
            const QString j = call.join(QLatin1Char(' '));
            if (call.value(0) == QLatin1String("ssh") && j.contains(QLatin1String("{{.Id}}")))
                inspectOverSsh = true;
            if (call.value(0) == QLatin1String("sh") && j.contains(QLatin1String("tar -C ")) &&
                j.contains(QLatin1String("user@dockerhost")) &&
                j.contains(QLatin1String("docker build -t")) && j.contains(QLatin1String(" -'")))
                streamed = true;
        }
        QVERIFY(inspectOverSsh);
        QVERIFY(streamed);
        QVERIFY(!sp.dockerfile.isEmpty());
        RemoraConfig nohost = profile({"arm_translate"});
        AddonSpawner none;
        QVERIFY(!ensureAddonOverlay(none, nohost, Backend::Remote, {}, paths()).ok);
        QVERIFY(none.calls.isEmpty());
    }

    void buildFailureRecordsNoLayer() {
        putArm();
        AddonSpawner sp;
        sp.build = {1, {}, QStringLiteral("COPY failed")};
        RemoraConfig c = profile({"arm_translate"});
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Bare, {}, paths());
        QVERIFY(!r.ok);
        QCOMPARE(r.stderrTail, QString("COPY failed"));
        QVERIFY(!c.image.addonOverlay);
    }
};

QTEST_MAIN(AddonsTest)
#include "test_addons.moc"
