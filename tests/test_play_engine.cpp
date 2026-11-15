#include <QtTest>
#include "engine/PlayInstall.h"

using namespace remora;
class PlaySpawner : public Spawner {
public:
    QVector<QStringList> calls;
    bool copyFails = false;
    QString directory = "/tmp/remora-play.ABCDE12345\n";
    // What the installer itself answers (the python3 call, local or over ssh).
    ProcResult install{0, "ok", {}};
    ProcResult run(const QStringList &argv, const QMap<QString, QString> & = {}, const LineSink & = {}) override {
        calls << argv;
        if (argv.join(' ').contains("mktemp")) return {0, directory, {}};
        if (argv[0] == "scp" && copyFails) return {1, {}, "copy failed"};
        if (argv[0] != "scp" && argv.join(' ').contains("install.py")) return install;
        return {0, "ok", {}};
    }
    Detached spawnDetached(const QStringList &, const QMap<QString, QString> &) override { return {}; }
    bool detachedAlive(const Detached &) override { return false; }
};
class PlayEngineTest : public QObject {
    Q_OBJECT
private slots:
    void consentBeforeRemoteNetwork() {
        PlaySpawner sp;
        QVERIFY(runPlayInstall(sp, {"--base", "base:clean"}, "user@host").rc != 0);
        QVERIFY(sp.calls.isEmpty());
    }
    void rejectSshOptionInjection() {
        for (const auto &host : {QString("-oProxyCommand=bad"), QString("user@host\ncommand"), QString("user@host;id")}) {
            PlaySpawner sp;
            QVERIFY(runPlayInstall(sp, {"--rollback", "remora-local:play"}, host).rc != 0);
            QVERIFY(sp.calls.isEmpty());
        }
    }
    void remoteRunsDownloadThereAndQuotesArguments() {
        PlaySpawner sp;
        QCOMPARE(runPlayInstall(sp, {"--base", "base:clean", "--archive", "/private/a'b $(touch BAD).zip",
                                     "--accept-terms", playTermsVersion()}, "user@host").rc, 0);
        QCOMPARE(sp.calls.size(), 4);
        QCOMPARE(sp.calls[1][0], QString("scp"));
        // Only our source and JSON travel over SCP, never a Google archive/APK.
        QVERIFY(sp.calls[1].join(' ').contains("install.py"));
        QVERIFY(!sp.calls[1].join(' ').contains(".zip"));
        QCOMPARE(sp.calls[2][0], QString("ssh"));
        QVERIFY(sp.calls[2].last().contains("'\\''"));
        QVERIFY(sp.calls[2].last().contains("'--archive'"));
        QVERIFY(sp.calls[3].last().contains("rm -rf -- "));
        QVERIFY(sp.calls[3].last().contains("/tmp/remora-play.ABCDE12345"));
        QVERIFY(sp.calls[3].contains("BatchMode=yes"));
    }
    void copyFailureStillCleansOwnedDirectory() {
        PlaySpawner sp;
        sp.copyFails = true;
        QVERIFY(runPlayInstall(sp, {"--rollback", "remora-local:play"}, "user@host").rc != 0);
        QCOMPARE(sp.calls.size(), 3);
        QVERIFY(sp.calls.last().last().contains("rm -rf -- "));
    }
    // bd remora-6laf.8 — the google_play profile option, materialized for one deploy.
    static RemoraConfig playProfile() {
        RemoraConfig c;
        c.image.androidVersion = 17;
        c.image.imageTag = QStringLiteral("remora24:x86_64-public");
        c.image.googlePlay = true;
        c.image.playTerms = playTermsVersion();
        return c;
    }
    static QString receipt(bool reused) {
        return QStringLiteral("Building COPY-only local Play overlay…\nREMORA_PLAY_RESULT={\"tag\":"
                              "\"remora-local:play-08c62ac56c4d-07db5d1f5dc7\",\"image\":\"sha256:"
                              "1848\",\"base\":\"sha256:08c6\"%1}\n")
            .arg(reused ? QStringLiteral(",\"reused\":true") : QString());
    }
    void playOverlayOffOrIncludedRunsNothing() {
        PlaySpawner sp;
        RemoraConfig off = playProfile();
        off.image.googlePlay = false;
        QVERIFY(ensurePlayOverlay(sp, off, Backend::Bare).ok);
        RemoraConfig included = playProfile();
        included.image.imageTag = QStringLiteral("remora24:x86_64-gapps-wv-hwc2");
        const StepResult r = ensurePlayOverlay(sp, included, Backend::Bare);
        QVERIFY(r.ok);
        QVERIFY(r.detail.contains("already includes"));
        QVERIFY(!included.image.playOverlay.has_value());
        QVERIFY(sp.calls.isEmpty());
    }
    // Accepting the provider's terms is the user's act: a missing or outdated acceptance refuses
    // before anything is downloaded, and names how to give it.
    void playOverlayNeedsTheCurrentTermsBeforeAnyCommand() {
        PlaySpawner sp;
        RemoraConfig never = playProfile();
        never.image.playTerms.reset();
        QVERIFY(!ensurePlayOverlay(sp, never, Backend::Bare).ok);
        RemoraConfig stale = playProfile();
        stale.image.playTerms = QStringLiteral("litegapps-a17-19990101-v0");
        const StepResult r = ensurePlayOverlay(sp, stale, Backend::Bare);
        QVERIFY(!r.ok);
        QVERIFY(r.detail.contains("terms changed"));
        QVERIFY(sp.calls.isEmpty());
    }
    void playOverlayBuildsOnTheResolvedBaseAndRecordsTheTag() {
        PlaySpawner sp;
        sp.install = {0, receipt(false), {}};
        RemoraConfig c = playProfile();
        // an overlay left from an earlier deploy is never the base: it is cleared, not layered on
        c.image.playOverlay = QStringLiteral("remora-local:play-oldbase00000-07db5d1f5dc7");
        const StepResult r = ensurePlayOverlay(sp, c, Backend::Bare);
        QVERIFY2(r.ok, qPrintable(r.detail));
        QCOMPARE(c.image.playOverlay,
                 std::optional<QString>("remora-local:play-08c62ac56c4d-07db5d1f5dc7"));
        QVERIFY(r.detail.startsWith("built "));
        QCOMPARE(sp.calls.size(), 1);
        const QStringList &argv = sp.calls[0];
        QCOMPARE(argv[0], QString("python3"));
        QCOMPARE(argv.mid(argv.size() - 4),
                 (QStringList{"--base", "remora24:x86_64-public", "--accept-terms",
                              playTermsVersion()}));
        sp.install = {0, receipt(true), {}};
        QVERIFY(ensurePlayOverlay(sp, c, Backend::Bare).detail.startsWith("reused "));
        QCOMPARE(sp.calls.last().mid(sp.calls.last().size() - 4).value(1),
                 QString("remora24:x86_64-public"));
    }
    void playOverlayFailureLeavesNoOverlay() {
        PlaySpawner sp;
        sp.install = {1, {}, "Play installation failed: Base contains conflicting Google/microG package"};
        RemoraConfig c = playProfile();
        const StepResult r = ensurePlayOverlay(sp, c, Backend::Bare);
        QVERIFY(!r.ok);
        QVERIFY(r.stderrTail.contains("conflicting"));
        QVERIFY(!c.image.playOverlay.has_value());
        // a zero exit without a receipt is not a success either
        sp.install = {0, "no receipt here", {}};
        QVERIFY(!ensurePlayOverlay(sp, c, Backend::Bare).ok);
        QVERIFY(!c.image.playOverlay.has_value());
    }
    // Built where the container runs: a remote profile's overlay is made on its ssh host, and a
    // remote profile with no host refuses rather than building one here.
    void playOverlayRunsOnTheRemoteDockerHost() {
        PlaySpawner sp;
        sp.install = {0, receipt(false), {}};
        RemoraConfig c = playProfile();
        c.backend.sshHost = QStringLiteral("user@host");
        QVERIFY(ensurePlayOverlay(sp, c, Backend::Remote).ok);
        QCOMPARE(sp.calls.size(), 4);
        QCOMPARE(sp.calls[1][0], QString("scp"));
        QCOMPARE(sp.calls[2][0], QString("ssh"));
        QVERIFY(sp.calls[2].contains("user@host"));
        QVERIFY(sp.calls[2].last().contains("'remora24:x86_64-public'"));
        PlaySpawner none;
        RemoraConfig nohost = playProfile();
        QVERIFY(!ensurePlayOverlay(none, nohost, Backend::Remote).ok);
        QVERIFY(none.calls.isEmpty());
    }
    void malformedDirectoryIsNeverUsedOrRemoved() {
        PlaySpawner sp;
        sp.directory = "/tmp/remora-play.bad; rm -rf /";
        QVERIFY(runPlayInstall(sp, {"--rollback", "remora-local:play"}, "user@host").rc != 0);
        QCOMPARE(sp.calls.size(), 1);
    }
};
QTEST_GUILESS_MAIN(PlayEngineTest)
#include "test_play_engine.moc"
