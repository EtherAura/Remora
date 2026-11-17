#include <QtTest>

#include "TestPlatform.h"
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
        // AddonPaths::stageOnHost: the scratch dir, the Dockerfile that travels by scp, and the
        // fetched payload copied back from the host.
        if (j.contains(QLatin1String("mktemp -d /tmp/remora-addons.")))
            return {0, QStringLiteral("/tmp/remora-addons.AbCdEf1234\n"), {}};
        if (argv.value(0) == QLatin1String("scp")) {
            const QString from = argv.value(argv.size() - 2), to = argv.last();
            if (to.endsWith(QLatin1String("/ctx/Dockerfile"))) {
                QFile f(from);
                if (f.open(QIODevice::ReadOnly)) dockerfile = QString::fromUtf8(f.readAll());
            }
            if (from.contains(QLatin1String("/fetched/magisk")) && !fetchInto.isEmpty())
                writeMagisk(to);
            return {0, {}, {}};
        }
        if (j.contains(QLatin1String("fetch-payloads.sh"))) {
            const QString into = env.value("REMORA_PAYLOAD_DIR");
            if (!fetchInto.isEmpty() && !into.isEmpty()) writeMagisk(into);
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
                sourcePath(QStringLiteral("vendor")),
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
        // No host is refused before anything runs, where this machine's docker is a local daemon.
        // Where it is Remora's own guest (Windows), a blank host is the guest, and the layer is
        // built there.
        RemoraConfig nohost = profile({"arm_translate"});
        {
            const ScopedLocalDockerIsGuest daemon(false);
            AddonSpawner none;
            QVERIFY(!ensureAddonOverlay(none, nohost, Backend::Remote, {}, paths()).ok);
            QVERIFY(none.calls.isEmpty());
        }
        const ScopedLocalDockerIsGuest guest(true);
        AddonSpawner toGuest;
        const StepResult g = ensureAddonOverlay(toGuest, nohost, Backend::Remote, {}, paths());
        QVERIFY2(g.ok, qPrintable(g.detail + g.stderrTail));
        QVERIFY(toGuest.ran(guestAlias()));
    }

    // bd remora-6laf.3: root is never added for the user, so a spoof list without it is refused
    // before anything runs; the add-on also turns the runtime option on, unless the profile says no.
    void spoofWithoutRootIsRefusedAndTheOptionFollowsTheAddon() {
        AddonSpawner sp;
        RemoraConfig c = profile({"play_spoof"});
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Bare, {}, paths());
        QVERIFY(!r.ok);
        QVERIFY2(r.detail.contains(QLatin1String("magisk_root")), qPrintable(r.detail));
        QVERIFY(sp.calls.isEmpty());
        QVERIFY(resolve(c, Backend::Bare).playSpoof);
        c.advanced.playSpoof = false;
        QVERIFY(!resolve(c, Backend::Bare).playSpoof);
        QVERIFY(!resolve(profile({"magisk_root"}), Backend::Bare).playSpoof);
    }

    // The whole stack in one layer: the spoof tree and Shamiko staged by ONE call of the script the
    // manual route uses, with nb_sigfix because ARM translation is in the same layer.
    void spoofStackIsStagedOnceWithNbSigfixAlongsideArm() {
        putArm();
        const QString z = tmp_.path() + QStringLiteral("/payloads/zygisk_pif/");
        for (const char *f : {"rez-mod/lib64/libzygisk.so", "rez-mod/bin/zygiskd64",
                              "rez-mod/bin/zygisk-ptrace64", "pifork/zygisk/x86_64.so",
                              "shamiko/zygisk/x86_64.so", "shamiko/module.prop"})
            QVERIFY(AddonSpawner::put(z + QLatin1String(f)));
        AddonSpawner::writeMagisk(tmp_.path() + QStringLiteral("/payloads"));
        AddonSpawner sp;
        RemoraConfig c = profile({"shamiko", "play_spoof", "magisk_root", "arm_translate"});
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Bare, {}, paths());
        QVERIFY2(r.ok, qPrintable(r.detail + r.stderrTail));
        int stagings = 0;
        for (const QStringList &call : sp.calls)
            if (call.join(QLatin1Char(' ')).contains(QLatin1String("stage-spoof-layer.sh"))) {
                ++stagings;
                QVERIFY(call.contains(QStringLiteral("--spoof")));
                QVERIFY(call.contains(QStringLiteral("--shamiko")));
                QVERIFY(call.contains(QStringLiteral("--nb-sigfix")));
                QCOMPARE(call.count(QStringLiteral("--payload")), 1);  // one shared directory
            }
        QCOMPARE(stagings, 1);
        QVERIFY(sp.dockerfile.contains("COPY spoof/ /system/etc/remora-spoof/\n"));
        QVERIFY(sp.dockerfile.contains("COPY shamiko/ /system/etc/remora-spoof/shamiko/\n"));
        QVERIFY(sp.dockerfile.contains(
            "LABEL org.remora.addons.list=\"arm_translate magisk_root play_spoof shamiko\""));
    }

    // bd remora-7oih.3 — a client with no local sh, cp or tar (Windows) stages on the Docker host:
    // the payload and Remora's halves are copied into a fresh mktemp directory there, the same
    // staging script runs there against the copies, and docker build reads the context there. Only
    // ssh and scp run here, and the scratch directory goes when the step returns.
    void hostStagedLayerIsAssembledAndBuiltOnTheDockerHost() {
        putArm();
        RemoraConfig c = profile({"arm_translate"});
        c.backend.sshHost = QStringLiteral("user@dockerhost");
        AddonSpawner local;
        RemoraConfig lc = c;
        QVERIFY(ensureAddonOverlay(local, lc, Backend::Remote, {}, paths()).ok);

        AddonSpawner sp;
        AddonPaths onHost = paths();
        onHost.stageOnHost = true;
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Remote, {}, onHost);
        QVERIFY2(r.ok, qPrintable(r.detail + r.stderrTail));
        const QString d = QStringLiteral("/tmp/remora-addons.AbCdEf1234");
        bool payloadSent = false, scriptSent = false, staged = false, built = false;
        for (const QStringList &call : sp.calls) {
            const QString j = call.join(QLatin1Char(' '));
            QVERIFY2(call.value(0) == QLatin1String("ssh") || call.value(0) == QLatin1String("scp"),
                     qPrintable(j));
            if (call.value(0) == QLatin1String("scp")) {
                if (call.last() == QStringLiteral("user@dockerhost:") + d + QStringLiteral("/payloads") &&
                    call.value(call.size() - 2).endsWith(QLatin1String("/payloads/ndk_translation")))
                    payloadSent = true;
                if (call.last().endsWith(d + QStringLiteral("/sb/stage-arm-layer.sh")))
                    scriptSent = true;
            } else {
                if (j.contains(d + QStringLiteral("/sb/stage-arm-layer.sh")) &&
                    j.contains(d + QStringLiteral("/payloads/ndk_translation")))
                    staged = true;
                if (j.contains(QLatin1String("docker")) && j.contains(QLatin1String("build")) &&
                    j.contains(d + QStringLiteral("/ctx")))
                    built = true;
            }
        }
        QVERIFY(payloadSent);
        QVERIFY(scriptSent);
        QVERIFY(staged);
        QVERIFY(built);
        QVERIFY(sp.ran(QStringLiteral("7f454c46")));  // modes restored before anything is staged
        QVERIFY(sp.dockerfile.startsWith(QLatin1String("FROM remora24:x86_64-public\n")));
        // The same inputs make the same layer, whichever side assembled it.
        QCOMPARE(c.image.addonOverlay, lc.image.addonOverlay);
        QVERIFY(sp.calls.last().join(QLatin1Char(' ')).contains(QStringLiteral("rm -rf")));
        QVERIFY(sp.calls.last().join(QLatin1Char(' ')).contains(d));
        // Nothing was assembled here.
        QVERIFY(QDir(tmp_.path() + QStringLiteral("/cache"))
                    .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                    .isEmpty());
    }

    // …and a missing Magisk is fetched ON the host, where curl and unzip are, then copied back
    // into the payload dir here so the recipe reads the same bytes as a local fetch.
    void hostStagedMagiskIsFetchedOnTheHostAndKeptHere() {
        QTemporaryDir own;
        QVERIFY(own.isValid());
        AddonPaths onHost{{own.path() + QStringLiteral("/payloads")},
                          sourcePath(QStringLiteral("vendor")),
                          own.path() + QStringLiteral("/cache"),
                          true};
        AddonSpawner sp;
        sp.fetchInto = QStringLiteral("set");
        RemoraConfig c = profile({"magisk_root"});
        c.backend.sshHost = QStringLiteral("user@dockerhost");
        const StepResult r = ensureAddonOverlay(sp, c, Backend::Remote, {}, onHost);
        QVERIFY2(r.ok, qPrintable(r.detail + r.stderrTail));
        QVERIFY(sp.ran(QStringLiteral("REMORA_PAYLOAD_DIR=")));
        QVERIFY(QFileInfo::exists(own.path() + QStringLiteral("/payloads/magisk/magisk/magiskinit")));
        for (const QStringList &call : sp.calls)
            QVERIFY(call.value(0) == QLatin1String("ssh") || call.value(0) == QLatin1String("scp"));
        // A host-staged profile with no docker host to stage on is refused before anything runs.
        AddonSpawner none;
        RemoraConfig bare = profile({"magisk_root"});
        QVERIFY(!ensureAddonOverlay(none, bare, Backend::Bare, {}, onHost).ok);
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
