#pragma once
#include <QAudioSink>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include "core/MirrorProto.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace remora::mirror {

// The audio half of a mirror session: encoded packets from the audio socket in, PCM out through
// QAudioSink (bd remora-28ix.2.2). The stream is v1's fixed contract — 48 kHz stereo, opus by
// default — so there is no format negotiation, only decode and play.
//
// Latency policy: the sink's buffer is the jitter buffer (REMORA_AUDIO_BUFFER_MS, default 60 ms,
// near the reference client's 50). A sink that will not take everything right now is normal —
// it means its buffer is momentarily full — so the remainder is HELD and retried, not discarded.
// Dropping only happens when the held audio exceeds kMaxBacklogMs, i.e. a real backlog rather
// than ordinary buffer pressure, and then the OLDEST audio goes so playback skips to live.
//
// Everything here is frame-aligned. QIODevice::write() may accept a byte count that is not a
// multiple of the 4-byte frame, and resuming mid-frame swaps the channels and misaligns every
// sample after it — which is heard as a continuous crackle, not a glitch.
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // Codec id from the audio stream header (kCodecOpus/kCodecAac/kCodecFlac/kCodecRawAudio).
    // The decoder opens lazily on the first media packet, because opus/aac/flac carry their
    // codec headers in a CONFIG packet that arrives first and becomes extradata.
    bool init(quint32 codecId, QString *error);
    void submit(const MediaPacket &packet);
    void stop();

    qint64 playedBytes() const { return played_; }
    qint64 droppedBytes() const { return dropped_; }

private:
    bool openCodec(QString *error);
    void play(const char *data, qint64 bytes);
    void flush();  // push as much of pending_ as the sink will take, frame-aligned

    // How much held audio is tolerated before the oldest is trimmed. This is LATENCY, not safety
    // margin: whatever sits here plays later than the video it belongs to. Measured live, the
    // device's capture clock runs slightly fast against the host sound card, so the queue always
    // creeps up to this bound and is then trimmed — at 200 ms that meant a permanent ~200 ms of
    // audio lag, at 80 ms the trims are a few ms each and effectively inaudible.
    static constexpr int kMaxBacklogMs = 80;

    quint32 codecId_ = 0;
    bool raw_ = false;      // kCodecRawAudio: the payload already is s16le 48 kHz stereo
    bool opened_ = false;
    bool failed_ = false;
    QByteArray extradata_;  // stashed from the config packet until the codec opens

    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    SwrContext *swr_ = nullptr;

    QAudioSink *sink_ = nullptr;
    QIODevice *io_ = nullptr;  // owned by the sink
    QByteArray pcm_;           // reused conversion buffer
    QByteArray pending_;       // decoded PCM the sink has not taken yet
    QTimer drain_;             // retries pending_ while the sink is busy
    qint64 played_ = 0, dropped_ = 0;
    qint64 lastReportMs_ = 0, reportedDropped_ = 0;
};

}  // namespace remora::mirror
