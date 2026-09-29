#include "Recorder.h"

#include <QDateTime>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace remora::mirror {

// How long the first video packet may wait for the audio side to resolve. The audio codec id
// arrives within the stream's first bytes, so in practice this expires only when something is
// wrong — and a recording that starts video-only beats one that never starts.
static constexpr qint64 kAudioHoldCapMs = 3000;

Recorder::~Recorder() { finalize(); }

bool Recorder::open(const QString &path, quint32 codecId, bool expectAudio, QString *error) {
    codecId_ = codecId;
    audioResolved_ = !expectAudio;
    const QByteArray local = path.toLocal8Bit();
    // Container from the extension (mp4/mkv); the inherited default vocabulary.
    if (avformat_alloc_output_context2(&fmt_, nullptr, nullptr, local.constData()) < 0 || !fmt_) {
        if (error) *error = QStringLiteral("no container format for %1").arg(path);
        return false;
    }
    if (avio_open(&fmt_->pb, local.constData(), AVIO_FLAG_WRITE) < 0) {
        if (error) *error = QStringLiteral("cannot open %1 for writing").arg(path);
        return false;
    }
    return true;
}

void Recorder::setAudioCodec(quint32 codecId) {
    audioCodecId_ = codecId;
    audioResolved_ = true;
}

void Recorder::audioUnavailable() {
    // Keep any codec id already seen — this only stops the header from waiting longer.
    audioResolved_ = true;
}

bool Recorder::writeHeader(QString *error) {
    AVCodecID id;
    switch (codecId_) {
        case kCodecH264: id = AV_CODEC_ID_H264; break;
        case kCodecH265: id = AV_CODEC_ID_HEVC; break;
        case kCodecAv1: id = AV_CODEC_ID_AV1; break;
        default:
            if (error) *error = QStringLiteral("unrecordable codec id 0x%1").arg(codecId_, 8, 16);
            return false;
    }
    stream_ = avformat_new_stream(fmt_, nullptr);
    if (!stream_) return false;
    stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream_->codecpar->codec_id = id;
    stream_->codecpar->width = size_.width();
    stream_->codecpar->height = size_.height();
    if (!extradata_.isEmpty()) {
        // The config payload verbatim (Annex-B parameter sets) — libavformat's muxers parse
        // and convert as the container demands, same as the reference recorder relies on.
        auto *ed = static_cast<uint8_t *>(av_mallocz(extradata_.size()
                                                     + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ed) return false;
        memcpy(ed, extradata_.constData(), extradata_.size());
        stream_->codecpar->extradata = ed;
        stream_->codecpar->extradata_size = extradata_.size();
    }
    stream_->time_base = {1, 1000000};  // wire PTS are microseconds

    if (audioCodecId_) {
        AVCodecID aid = AV_CODEC_ID_NONE;
        switch (audioCodecId_) {
            case kCodecOpus: aid = AV_CODEC_ID_OPUS; break;
            case kCodecAac: aid = AV_CODEC_ID_AAC; break;
            case kCodecFlac: aid = AV_CODEC_ID_FLAC; break;
            case kCodecRawAudio: aid = AV_CODEC_ID_PCM_S16LE; break;
        }
        // A codec the container refuses (raw PCM in mp4) degrades to a video-only recording
        // rather than failing the whole file — audio is the opt-in extra here as everywhere.
        if (aid == AV_CODEC_ID_NONE
            || avformat_query_codec(fmt_->oformat, aid, FF_COMPLIANCE_NORMAL) <= 0) {
            qWarning("mirror: container %s cannot carry audio codec 0x%08x — recording video only",
                     fmt_->oformat->name, audioCodecId_);
        } else if ((audioStream_ = avformat_new_stream(fmt_, nullptr))) {
            audioStream_->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            audioStream_->codecpar->codec_id = aid;
            audioStream_->codecpar->sample_rate = 48000;  // the wire contract: 48 kHz stereo
            av_channel_layout_default(&audioStream_->codecpar->ch_layout, 2);
            if (!audioExtradata_.isEmpty()) {
                auto *aed = static_cast<uint8_t *>(av_mallocz(audioExtradata_.size()
                                                              + AV_INPUT_BUFFER_PADDING_SIZE));
                if (!aed) return false;
                memcpy(aed, audioExtradata_.constData(), audioExtradata_.size());
                audioStream_->codecpar->extradata = aed;
                audioStream_->codecpar->extradata_size = audioExtradata_.size();
            }
            audioStream_->time_base = {1, 1000000};
        }
    }

    if (avformat_write_header(fmt_, nullptr) < 0) {
        if (error) *error = QStringLiteral("container header write failed");
        return false;
    }
    headerWritten_ = true;
    return true;
}

bool Recorder::writePacket(const MediaPacket &pkt, AVStream *stream, QString *error) {
    AVPacket *p = av_packet_alloc();
    if (av_new_packet(p, pkt.payload.size()) < 0) {
        av_packet_free(&p);
        return true;
    }
    memcpy(p->data, pkt.payload.constData(), pkt.payload.size());
    const qint64 pts = pkt.ptsUs ? qint64(*pkt.ptsUs) : 0;
    if (ptsOrigin_ < 0) ptsOrigin_ = pts;  // normalize: recordings start at t=0
    // Both streams share the origin (device clock is common), so A/V sync is the wire's own.
    // An audio packet stamped just before the chosen origin clamps to 0 instead of going
    // negative — a one-packet nudge at worst, and muxers reject negative dts.
    p->pts = qMax<qint64>(0, pts - ptsOrigin_);
    p->dts = p->pts;  // no B-frames on the wire, either stream; the reference recorder's policy
    if (pkt.keyFrame) p->flags |= AV_PKT_FLAG_KEY;
    p->stream_index = stream->index;
    av_packet_rescale_ts(p, {1, 1000000}, stream->time_base);
    const int rc = av_interleaved_write_frame(fmt_, p);
    av_packet_free(&p);
    if (rc < 0 && error) *error = QStringLiteral("record write failed (rc %1)").arg(rc);
    return rc >= 0;
}

// Header just landed: drain the packets that waited for it. The shared origin must be the
// EARLIEST stamp about to be written, or whichever stream started later would clamp to 0 and
// desync — so it is chosen over the whole backlog before anything is written.
bool Recorder::flushPending(QString *error) {
    for (const MediaPacket &pkt : pendingVideo_)
        if (pkt.ptsUs && (ptsOrigin_ < 0 || qint64(*pkt.ptsUs) < ptsOrigin_))
            ptsOrigin_ = qint64(*pkt.ptsUs);
    if (audioStream_)
        for (const MediaPacket &pkt : pendingAudio_)
            if (pkt.ptsUs && (ptsOrigin_ < 0 || qint64(*pkt.ptsUs) < ptsOrigin_))
                ptsOrigin_ = qint64(*pkt.ptsUs);
    bool ok = true;
    for (const MediaPacket &pkt : pendingVideo_)
        ok = ok && writePacket(pkt, stream_, error);
    if (audioStream_)
        for (const MediaPacket &pkt : pendingAudio_)
            ok = ok && writePacket(pkt, audioStream_, error);
    pendingVideo_.clear();
    pendingAudio_.clear();
    return ok;
}

bool Recorder::push(const MediaPacket &pkt, QString *error) {
    if (finalized_ || !fmt_) return true;
    if (pkt.config) {
        extradata_ = pkt.payload;  // becomes stream extradata; never written as data
        return true;
    }
    if (!headerWritten_) {
        if (!audioResolved_) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (holdSinceMs_ < 0) holdSinceMs_ = now;
            if (now - holdSinceMs_ < kAudioHoldCapMs) {
                pendingVideo_ << pkt;
                return true;
            }
            qWarning("mirror: audio stream never resolved — recording continues without it");
            audioResolved_ = true;
        }
        if (!writeHeader(error)) return false;
        if (!flushPending(error)) return false;
    }
    return writePacket(pkt, stream_, error);
}

bool Recorder::pushAudio(const MediaPacket &pkt, QString *error) {
    if (finalized_ || !fmt_) return true;
    if (pkt.config) {
        audioExtradata_ = pkt.payload;  // OpusHead / AudioSpecificConfig / STREAMINFO
        return true;
    }
    if (!headerWritten_) {
        // Bounded: the video-side hold cap flushes or abandons this well before it grows —
        // the cap only fails to run if video never flows, in which case cap the list itself.
        if (pendingAudio_.size() < 1024) pendingAudio_ << pkt;
        return true;
    }
    if (!audioStream_) return true;  // container carries no audio (refused codec)
    return writePacket(pkt, audioStream_, error);
}

void Recorder::finalize() {
    if (finalized_) return;
    finalized_ = true;
    if (!fmt_) return;
    if (headerWritten_) av_write_trailer(fmt_);
    if (fmt_->pb) avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
}

}  // namespace remora::mirror
