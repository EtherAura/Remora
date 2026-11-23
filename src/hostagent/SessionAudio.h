#pragma once
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>

class QLocalSocket;
struct OpusEncoder;

namespace remora::hostagent {

// A session's audio (protocol v3's audio connection): the app's sound, not the desk's, and played
// on the phone instead of the desk's speakers. Through the PulseAudio interface PipeWire serves,
// with the tools every such desk has:
//
//   pactl load-module module-null-sink   a sink of the session's own, "remora-phone-<pid>-<id>"
//   PULSE_SINK=<sink>                    in the launched app's environment: its streams, and its
//                                        children's, START on the sink, so not a moment of them
//                                        plays on the desk (the session loads the sink first)
//   pactl move-sink-input                the streams of processes it did not launch — a
//                                        single-instance app's running process, a D-Bus-activated
//                                        one — matched by the processes behind the session's
//                                        windows and their children (a browser plays from a
//                                        child), moved as they appear (pactl subscribe)
//   parec <sink>.monitor                 48 kHz stereo s16 from it, encoded here as 20 ms Opus,
//                                        once a client has attached
//
// Unloading the sink at the end sends every stream back to the desk's default output, and a
// stream opened later by an app still pointed at it falls back to that output too.
class SessionAudio : public QObject {
    Q_OBJECT
public:
    SessionAudio(quint32 session, int bitRate, QObject *parent = nullptr);
    ~SessionAudio() override;

    // Loads the session's sink and starts claiming the session's streams. False with *error when
    // there is no PulseAudio service to ask.
    bool openSink(QString *error);
    QString sinkName() const { return sinkName_; }
    // Sends the sink's sound to `client`. False with *error when it cannot be captured or
    // encoded; the caller then ends the audio stream with END (unavailable), which costs the
    // session nothing.
    bool attach(QLocalSocket *client, QString *error);
    bool attached() const { return client_ != nullptr; }
    // The processes whose streams belong to the session (its windows' owners). Streams already
    // playing move now; later ones as they appear.
    void setProcesses(const QSet<int> &pids);

private:
    void claimStreams();
    void onPcm();
    void stop();

    quint32 session_;
    QPointer<QLocalSocket> client_;
    int bitRate_;
    QString sinkName_;
    QString moduleIndex_;
    QSet<int> pids_;
    QSet<QString> moved_;
    QProcess *subscribe_ = nullptr, *parec_ = nullptr;
    OpusEncoder *opus_ = nullptr;
    QByteArray pcm_;
    quint64 samples_ = 0;
};

}  // namespace remora::hostagent
