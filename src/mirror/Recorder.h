#pragma once
#include <QList>
#include <QSize>
#include <QString>

#include "core/MirrorProto.h"

struct AVFormatContext;
struct AVStream;

namespace remora::mirror {

// Records the received packets into a container without re-encoding (config payload becomes
// stream extradata verbatim, dts = pts, PTS normalized to the first frame). The container finalizes on close — finalize() must run on every exit path,
// which the session's stop() guarantees (including SIGTERM via the signal pipe).
//
// Audio rides in the same container (bd remora-28ix.2.5): the session taps the audio demuxer's
// RAW packets — before AudioPlayer's keep-it-live drop policy touches them — and pushes them
// here with their original timestamps. Because libavformat wants every stream declared before
// the header, and the audio socket announces its codec on its own schedule, the header is HELD
// until the audio side resolves (codec known, declared unavailable, or the wait cap expires);
// packets buffer meanwhile and flush with one shared PTS origin so the streams stay in sync.
class Recorder {
public:
    Recorder() = default;
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    ~Recorder();

    // expectAudio: hold the header for an audio stream (the session passes its --audio flag).
    bool open(const QString &path, quint8 codecId, bool expectAudio, QString *error);
    void setSize(QSize size) { size_ = size; }  // from session packets, pre-header
    // The audio stream's codec id arrived — the header can carry an audio stream.
    void setAudioCodec(quint8 codecId);
    // Stop holding the header: audio died, was disabled, or never showed up.
    void audioUnavailable();
    bool push(const MediaPacket &pkt, QString *error);       // video packets
    bool pushAudio(const MediaPacket &pkt, QString *error);  // audio packets
    void finalize();  // write trailer + close; idempotent

private:
    bool writeHeader(QString *error);
    bool writePacket(const MediaPacket &pkt, AVStream *stream, QString *error);
    bool flushPending(QString *error);

    AVFormatContext *fmt_ = nullptr;
    AVStream *stream_ = nullptr;       // video; owned by fmt_
    AVStream *audioStream_ = nullptr;  // owned by fmt_; null when the container has no audio
    QByteArray extradata_, audioExtradata_;
    QSize size_;
    quint8 codecId_ = 0, audioCodecId_ = 0;
    bool audioResolved_ = true;  // false = header held for setAudioCodec/audioUnavailable
    qint64 holdSinceMs_ = -1;    // when the first video packet started waiting (the cap's clock)
    QList<MediaPacket> pendingVideo_, pendingAudio_;  // buffered while the header is held
    qint64 ptsOrigin_ = -1;
    bool headerWritten_ = false, finalized_ = false;
};

}  // namespace remora::mirror
