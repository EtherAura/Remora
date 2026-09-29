#include "mirror/VideoReader.h"

#include <QDebug>

namespace remora::mirror {

// How many demuxed packets may be in flight to the GUI thread before this one decides the consumer
// is not keeping up. Each queued packet is a compressed frame, so the cap is memory-cheap; what it
// really bounds is LATENCY. A mirror is live video — a viewer wants the current frame, never a
// faithful replay of the last two seconds — so falling behind is answered by skipping ahead, which
// is the behaviour the encoder's own drop-rather-than-nurse rule assumes of its consumers.
static constexpr int kMaxOutstanding = 64;  // ~1 s at 60 fps

VideoReader::VideoReader(qintptr fd) : fd_(fd) {
    // Explicit rather than relying on auto-registration: a queued connection whose argument type is
    // unknown fails at CONNECT time with a warning and then silently delivers nothing, which would
    // look exactly like the black mirror this whole area exists to stop.
    qRegisterMetaType<MediaPacket>("MediaPacket");
    qRegisterMetaType<SessionInfo>("SessionInfo");
}

VideoReader::~VideoReader() {
    if (sock_) sock_->abort();
}

void VideoReader::start() {
    sock_ = new QLocalSocket(this);
    // setSocketDescriptor, not connectToServer: the GUI thread already dialled and settled this
    // connection. Adopting the descriptor here is what puts the READING on this thread without
    // moving the connect/retry logic — which needs the session — off the GUI thread with it.
    if (!sock_->setSocketDescriptor(fd_, QLocalSocket::ConnectedState, QIODevice::ReadOnly)) {
        emit streamFailed(QStringLiteral("could not adopt the video socket on the reader thread: %1")
                              .arg(sock_->errorString()));
        return;
    }
    connect(sock_, &QLocalSocket::readyRead, this, &VideoReader::onReadyRead);
    connect(sock_, &QLocalSocket::disconnected, this, [this] { emit socketClosed(); });
    onReadyRead();  // whatever arrived between the dial and this thread starting
}

void VideoReader::stop() {
    if (!sock_) return;
    sock_->abort();
    sock_->deleteLater();
    sock_ = nullptr;
}

void VideoReader::onReadyRead() {
    if (!sock_) return;
    demux_.feed(sock_->readAll());
    for (;;) {
        // Take anything that landed while the previous packet was being demuxed. The point of this
        // thread is that the socket is never left unread, and that includes the time spent in this
        // very loop.
        if (sock_->bytesAvailable() > 0) demux_.feed(sock_->readAll());
        switch (demux_.next()) {
            case StreamDemuxer::Event::NeedMoreData:
                return;
            case StreamDemuxer::Event::CodecId:
                emit codecIdReady(demux_.codecId());
                break;
            case StreamDemuxer::Event::Session:
                emit sessionInfo(demux_.session());
                break;
            case StreamDemuxer::Event::Packet: {
                MediaPacket pkt = demux_.takePacket();
                // Overload: skip to the next key frame rather than queue an ever-growing past.
                // Config packets are never dropped — they are what the next key frame needs to be
                // decodable, and losing one turns the resync into a dead stream.
                if (droppingToKeyFrame_ && !pkt.keyFrame && !pkt.config) {
                    ++dropped_;
                    break;
                }
                if (droppingToKeyFrame_) {
                    qInfo("mirror: consumer fell behind — skipped %lld packets to a key frame",
                          static_cast<long long>(dropped_));
                    dropped_ = 0;
                    droppingToKeyFrame_ = false;
                } else if (outstanding_.load(std::memory_order_relaxed) >= kMaxOutstanding &&
                           !pkt.config) {
                    droppingToKeyFrame_ = true;
                    ++dropped_;
                    break;
                }
                outstanding_.fetch_add(1, std::memory_order_relaxed);
                emit packetReady(std::move(pkt));
                break;
            }
            case StreamDemuxer::Event::StreamDisabled:
                emit streamFailed(QStringLiteral("the server disabled the video stream"));
                return;
            case StreamDemuxer::Event::ConfigError:
                emit streamFailed(
                    QStringLiteral("server-side configuration error on the video stream"));
                return;
            case StreamDemuxer::Event::Error:
                emit streamFailed(
                    QStringLiteral("video stream framing error: %1").arg(demux_.errorString()));
                return;
        }
    }
}

}  // namespace remora::mirror
