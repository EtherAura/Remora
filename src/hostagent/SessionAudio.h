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
//   pactl load-module module-null-sink   a sink of the session's own, "remora-phone-<id>"
//   pactl move-sink-input                the app's playback streams onto it — matched by the
//                                        processes behind the session's windows and their
//                                        children (a browser plays from a child process)
//   pactl subscribe                      streams the app opens later, as they appear
//   parec <sink>.monitor                 48 kHz stereo s16 from it, encoded here as 20 ms Opus
//
// Unloading the sink at the end sends every stream back to the desk's default output.
class SessionAudio : public QObject {
    Q_OBJECT
public:
    SessionAudio(quint32 session, QLocalSocket *client, int bitRate, QObject *parent = nullptr);
    ~SessionAudio() override;

    // False with *error when the sink or the capture cannot be set up; the caller then ends the
    // audio stream with END (unavailable), which costs the session nothing.
    bool start(QString *error);
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
