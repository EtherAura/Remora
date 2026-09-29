#pragma once
#include <QThread>

#include "core/Config.h"
#include "engine/Engine.h"

namespace remora {

// Runs deploy/reconnect off the GUI thread. Implements LogSink; its line()/state() emit signals
// that Qt auto-queues to the GUI thread. (Emit-from-run-thread is the standard QThread pattern.)
class EngineWorker : public QThread, public LogSink {
    Q_OBJECT
public:
    // Smart: reconnect if running, full deploy otherwise. Fullscreen: like Smart but launches the
    // separate --new-display fullscreen session instead of the windowed mirror. Sleep: freeze the
    // device in place (docker pause) — the next Connect wakes it in ~1s with the session intact.
    enum Mode { Reconnect, Smart, Fullscreen, Sleep };
    EngineWorker(Backend backend, RemoraConfig cfg, QString windowTitle, Mode mode,
                 QObject *parent = nullptr);

    void line(const QString &step, const QString &stream, const QString &text) override;
    void state(const QString &step, StepState st, const QString &detail,
               const QString &stderrTail) override;

signals:
    void sigState(QString step, QString state, QString detail, QString stderrTail);
    void sigLine(QString text);  // raw command output, for the terminal-style log pane
    void sigConflict(QString holder);
    void sigDone(bool ok);
    void sigMenuSynced(bool ok);  // post-connect app-menu refresh finished (after sigDone)

protected:
    void run() override;

private:
    Backend backend_;
    RemoraConfig cfg_;
    QString windowTitle_;
    Mode mode_;
};

// Live readiness probe → emits (ready, rows) where each row is [label, ok, remedy, severity].
class ReadinessWorker : public QThread {
    Q_OBJECT
public:
    ReadinessWorker(Backend backend, QString sshHost, bool gpuHost, QObject *parent = nullptr);
signals:
    void sigReady(bool ready, QStringList lines);

protected:
    void run() override;

private:
    Backend backend_;
    QString sshHost_;
    bool gpuHost_;
};

// Manual desktop app-menu operations from the Integration card: refresh a profile's launcher
// folder (device must be running) or remove it (purely local file cleanup).
class AppMenuWorker : public QThread, public LogSink {
    Q_OBJECT
public:
    enum Op { Refresh, Remove };
    AppMenuWorker(Op op, Backend backend, RemoraConfig cfg, QString instance,
                  QObject *parent = nullptr);

    void line(const QString &step, const QString &stream, const QString &text) override;
    void state(const QString &step, StepState st, const QString &detail,
               const QString &stderrTail) override;

signals:
    void sigLine(QString text);
    void sigDone(bool ok, QString detail);

protected:
    void run() override;

private:
    Op op_;
    Backend backend_;
    RemoraConfig cfg_;
    QString instance_;
    QString lastDetail_;
};

// One-shot device screenshot into outPath: resolve the profile's live adb target, assert the
// link, capture (binary-safe exec-out). Off the GUI thread — a remote target may cost an ssh
// round trip. sigDone's detail is the saved path on success, the error otherwise.
class ShotWorker : public QThread {
    Q_OBJECT
public:
    ShotWorker(Backend backend, RemoraConfig cfg, QString outPath, QObject *parent = nullptr);
signals:
    void sigDone(bool ok, QString detail);

protected:
    void run() override;

private:
    Backend backend_;
    RemoraConfig cfg_;
    QString outPath_;
};

// Play Protect certification for the active profile's device, off the GUI thread: remote costs an
// ssh round trip, and recheck deliberately waits several seconds for GMS to check in again.
// `known` is false when the container is down or unreachable — the card must be able to say
// "unknown" rather than accuse a sleeping device of being uncertified (bd remora-4ei.83).
// `instance` is not decoration: it keys the checkin-identity snapshot, so a worker constructed
// without it reads the device and keeps nothing — which would leave GUI-only users with no snapshot
// to restore from after a /data rebuild (bd remora-4ei.84).
class CertificationWorker : public QThread {
    Q_OBJECT
public:
    CertificationWorker(Backend backend, RemoraConfig cfg, bool recheck, QString instance,
                        bool restore = false, QObject *parent = nullptr);
signals:
    // `snapshotNote` is checkinSnapshotNote()'s sentence; `restoreDetail` is empty unless a restore
    // was asked for, in which case it says what happened either way. `pendingNote` is non-empty for
    // the third state (bd remora-4ei.90): the identity is too young for absence-of-a-verdict to
    // mean certified — `certified` is false then, but must render as pending, not as uncertified.
    void sigDone(bool known, bool certified, QString androidId, QString snapshotNote,
                 QString restoreDetail, QString pendingNote);

protected:
    void run() override;

private:
    Backend backend_;
    RemoraConfig cfg_;
    bool recheck_;
    QString instance_;
    bool restore_;
};

class StopWorker : public QThread {
    Q_OBJECT
public:
    StopWorker(Backend backend, QString sshHost, QString containerName = QString(),
               QString target = QString(), QObject *parent = nullptr);
signals:
    void sigDone(bool ok);

protected:
    void run() override;

private:
    Backend backend_;
    QString sshHost_;
    QString containerName_;  // per-profile container; empty is a no-op (nothing to identify)
    QString target_;  // the container's adb serial — lets teardown reap the mirror clients too
};

// Fetch an app's ARM64 build from Google Play (reusing the device's own Google login) and sideload
// it into the profile's container as arm64 — the fix for apps whose x86_64 build is broken (e.g.
// Flutter apps whose x86_64 split omits libflutter.so and crashes). Runs remora-arm-fetch.sh on the
// deploy host (staged fresh over scp for remote; locally for bare), streaming its output.
// package "--update-all" runs the sweep mode instead: re-fetch every injected arm64 app.
class ArmAppInstallWorker : public QThread {
    Q_OBJECT
public:
    ArmAppInstallWorker(Backend backend, RemoraConfig cfg, QString package,
                        QObject *parent = nullptr);
signals:
    void sigLine(QString text);
    void sigDone(bool ok, QString detail);

protected:
    void run() override;

private:
    Backend backend_;
    RemoraConfig cfg_;
    QString package_;
};

// Runs one app-portability operation (engine/AppTransfer.cpp) off the GUI thread: transfer apps
// between two profiles' devices, export APKs to a folder, or import from one. Both endpoints are
// resolved BEFORE anything is touched and the failure names its side — a GUI transfer must not
// die half way with a half-migrated device (bd remora-4ei.75). Per-app ok/FAIL lines stream to
// the log pane via sigLine.
class TransferAppsWorker : public QThread {
    Q_OBJECT
public:
    enum class Op { Transfer, Export, Import };
    // srcName/dstName label the endpoints in messages; dir is the export/import folder (unused
    // for Transfer); empty packages = every third-party app on the source.
    TransferAppsWorker(Op op, Backend srcBackend, RemoraConfig srcCfg, QString srcName,
                       Backend dstBackend, RemoraConfig dstCfg, QString dstName,
                       QStringList packages, QString dir, bool dryRun, bool withData = false,
                       QObject *parent = nullptr);
signals:
    void sigLine(QString text);
    void sigDone(bool ok, QString detail);

protected:
    void run() override;

private:
    // Resolve one profile to a ready adb serial; empty + a named error when unreachable.
    QString endpoint(Spawner &sp, Backend backend, const RemoraConfig &cfg, const QString &name,
                     QString *err);
    Op op_;
    Backend srcBackend_, dstBackend_;
    RemoraConfig srcCfg_, dstCfg_;
    QString srcName_, dstName_;
    QStringList packages_;
    QString dir_;
    bool dryRun_;
    bool withData_;  // Transfer only: carry /data/data per installed app (bd remora-4ei.73)
};

}  // namespace remora
