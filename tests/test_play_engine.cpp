#include <QtTest>
#include "engine/PlayInstall.h"

using namespace remora;
class PlaySpawner : public Spawner {
public:
    QVector<QStringList> calls;
    bool copyFails = false;
    QString directory = "/tmp/remora-play.ABCDE12345\n";
    ProcResult run(const QStringList &argv, const QMap<QString, QString> & = {}, const LineSink & = {}) override {
        calls << argv;
        if (argv.join(' ').contains("mktemp")) return {0, directory, {}};
        if (argv[0] == "scp" && copyFails) return {1, {}, "copy failed"};
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
    void malformedDirectoryIsNeverUsedOrRemoved() {
        PlaySpawner sp;
        sp.directory = "/tmp/remora-play.bad; rm -rf /";
        QVERIFY(runPlayInstall(sp, {"--rollback", "remora-local:play"}, "user@host").rc != 0);
        QCOMPARE(sp.calls.size(), 1);
    }
};
QTEST_GUILESS_MAIN(PlayEngineTest)
#include "test_play_engine.moc"
