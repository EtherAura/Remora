#pragma once

#include <QLocalSocket>
#include <QObject>

#include <atomic>

#include "core/MirrorProto.h"

namespace remora::mirror {

// The external video socket, read and demuxed on a thread of its own (bd remora-10bt).
//
// WHY THIS EXISTS. The encoder drops a consumer whose write it cannot complete in 500 ms, on the
// reasoning that a client which cannot take a packet in half a second is broken rather than slow.
// That is a fair rule against a socket nobody is reading — and the mirror used to be exactly that
// for stretches at a time. Reading, demuxing, decoding and presenting all ran on the GUI thread, so
// one slow decode or one compositor stall meant nothing serviced the socket until it finished, and
// a mirror doing its job perfectly was evicted mid-frame. Enlarging the encoder's send buffer bought
// ~20x more room (bd remora-10bt) but left the coupling: a long enough stall still ended the
// session.
//
// Here the socket is serviced by a thread that does nothing else. Whatever the GUI thread is doing —
// decoding, presenting, blocked in a swap — the bytes keep being taken, so the encoder's write never
// blocks on us and its eviction rule can only ever fire against a client that is genuinely gone.
//
// It demuxes too, rather than shipping raw bytes, for two reasons: the demuxer is pure byte-stream
// bookkeeping with no Qt or GPU state, and packet boundaries are what make the overload policy
// possible — see the drop rule in the .cpp. Everything past the packet (merger, decoder, CUDA/GL
// interop) stays on the GUI thread, where the decoder's context and the GL textures already live.
class VideoReader : public QObject {
    Q_OBJECT
public:
    // fd: an ALREADY-CONNECTED socket descriptor. The dial/retry dance stays on the GUI thread
    // where it can talk to the session; this object only ever sees a live connection.
    explicit VideoReader(qintptr fd);
    ~VideoReader() override;

    // Called from the GUI thread as each packet is consumed, so this thread can tell whether the
    // consumer is keeping up. Atomic and lock-free: it is on the decode path.
    void packetConsumed() { outstanding_.fetch_sub(1, std::memory_order_relaxed); }

public slots:
    void start();  // must run ON the reader thread: the socket is created here
    void stop();

signals:
    // One signal per demuxer event, queued across the thread boundary, which preserves both order
    // and the session's existing handling — the GUI side kept its switch, it just no longer owns
    // the socket that feeds it.
    void codecReady(quint8 codec);
    void formatReady(FrameFormat format);
    void packetReady(MediaPacket packet);
    void streamFailed(QString why);
    void socketClosed();

private:
    void onReadyRead();

    qintptr fd_;
    QLocalSocket *sock_ = nullptr;
    StreamDemuxer demux_{StreamDemuxer::Kind::Video};
    std::atomic<int> outstanding_{0};
    bool droppingToKeyFrame_ = false;
    qint64 dropped_ = 0;
};

}  // namespace remora::mirror
