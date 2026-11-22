#pragma once
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QSize>
#include <QString>

#include <memory>

#include "core/HostInput.h"
#include "core/OutputLayout.h"
#include "core/MirrorProto.h"

class QLocalSocket;
class QTimer;

namespace remora::hostagent {

class DeskClipboard;
class DeskLayout;
class KWinWayland;
class PipeWireCapture;
class SessionAudio;
class VirtualOutput;
class WindowPlacer;

// What a new-display request asks for, from its key=value options.
struct SessionParams {
    QSize pixels{1080, 2400};
    double scale = 2.625;          // dpi / 160, Android's density baseline
    QString codec = QStringLiteral("h264");  // the agent protocol's default
    int bitRate = 8'000'000;
    int maxFps = 60;
    int maxSize = 0;
    int audioBitRate = 128'000;
    QString startApp;              // desktop-file id; empty shows the bare output
    bool closeOnAppExit = false;

    // nullopt with *error for options this agent cannot serve.
    static std::optional<SessionParams> fromOptions(const QHash<QString, QString> &options,
                                                    QString *error);
};

// One app on one virtual output, served as an agent session: the output and its stream, the
// encoder fed from it, the app placed on it, and the session's input injected into it. It lives
// exactly as long as its control connection, like the in-image agent's sessions.
class HostSession : public QObject {
    Q_OBJECT
public:
    struct Services {
        KWinWayland *wl;
        DeskLayout *desk;
        WindowPlacer *placer;
        DeskClipboard *clipboard;
        QString encoder;   // remora-frame-encoder
        QString runtimeDir;
    };

    HostSession(const Services &services, quint32 id, const QString &outputName,
                const SessionParams &params, QObject *parent = nullptr);
    ~HostSession() override;

    quint32 id() const { return id_; }
    QString outputName() const;
    // The name the session was given; its output alternates between this and this + "-b", because
    // a reflow brings the new output up before the old one goes.
    QString baseName() const { return baseName_; }
    // This session's outputs as the desk layout must place them: one, or two during a reflow.
    QList<outputs::VirtualPlacement> placements() const;

    // Brings the session up; ready() reports the outcome once.
    void start();
    // The client's control connection, once the session is accepted. `pending` is whatever
    // arrived behind the request.
    void takeControl(QLocalSocket *control, const QByteArray &pending);
    void attachVideo(QLocalSocket *video);
    // The audio connection: the app's own sound, moved off the desk's speakers (SessionAudio).
    void attachAudio(QLocalSocket *audio);
    // The desk's clipboard changed: tell the client, as the in-image agent does.
    void sendDeskClipboard(const QString &text);
    // The desk was re-laid out (another session came or went): the output may have moved.
    void refreshGeometry();

    void end(const QString &why);

signals:
    // The output is wanted in the desk's layout from now on (or no longer).
    void outputLive(quint32 id);
    void ready(bool ok, const QString &reason);
    void ended(quint32 id, const QString &why);

private:
    void onOutputReady(quint64 serial);
    void onDeskSettled(bool ok, const QString &detail);
    void startEncoder();
    void startCapture();
    void launchApp();
    void onControlData();
    void attachEncoder();
    void requestResize(QSize pixels);
    void beginReflow();
    void onReflowSettled();
    void finishReflow();
    void abandonReflow(const QString &why);
    void inject(const QList<hostinput::InputOp> &ops);  // through the key gate
    void deliver(const QList<hostinput::InputOp> &ops);  // straight to KWin
    void regainFocus();
    void fail(const QString &why);

    Services s_;
    quint32 id_;
    QString outputName_;
    SessionParams params_;
    VirtualOutput *output_ = nullptr;
    PipeWireCapture *capture_ = nullptr;
    QProcess *encoder_ = nullptr;  // outlives the session while it exits: reaped by the agent
    QPointer<QLocalSocket> control_;
    QPointer<QLocalSocket> video_, encoderVideo_;
    std::unique_ptr<mirror::RecordReader> relay_;  // the encoder's stream, record by record
    bool videoStarted_ = false, skipping_ = false;
    mirror::ControlMessageParser parser_;
    hostinput::Translator input_;
    hostinput::KeyGate keys_;
    QTimer *focusTimer_ = nullptr;  // how long held keys wait for the window to get focus back
    QTimer *focusSettle_ = nullptr;  // focus regained, not yet usable
    quint64 serial_ = 0;
    bool readyEmitted_ = false, ended_ = false, layoutWaiting_ = false;

    // Reflow (RESIZE): KWin cannot resize a virtual output, so a new one comes up beside the old,
    // the app moves over, a capture of the new one replaces the old capture on the same encoder,
    // and only then does the old output go.
    QString baseName_;
    QSize refPixels_;
    double refScale_ = 1.0;
    QSize wantedPixels_;
    QTimer *reflowTimer_ = nullptr;
    VirtualOutput *nextOutput_ = nullptr;
    PipeWireCapture *nextCapture_ = nullptr;
    QString nextName_;
    QSize nextPixels_;
    double nextScale_ = 1.0;
    quint64 nextSerial_ = 0;
    bool reflowWaiting_ = false;
    QString frameSocket_, videoSocket_;
    SessionAudio *audio_ = nullptr;
    QSet<int> pids_;  // the processes behind the session's windows, for its audio
};

}  // namespace remora::hostagent
