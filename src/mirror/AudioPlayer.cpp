#include "AudioPlayer.h"

#include <QAudioFormat>
#include <QDateTime>
#include <QMediaDevices>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace remora::mirror {

// The stream contract's fixed output format (v1: 48 kHz stereo; we play s16 interleaved).
static constexpr int kRate = 48000;
static constexpr int kChannels = 2;
static constexpr int kBytesPerFrame = kChannels * 2;

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {
    // Retries held audio while the sink is draining. Only runs when something is held, so an
    // idle session costs nothing.
    drain_.setInterval(5);
    connect(&drain_, &QTimer::timeout, this, &AudioPlayer::flush);
}

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::init(quint32 codecId, QString *error) {
    codecId_ = codecId;
    raw_ = codecId == kCodecRawAudio;

    QAudioFormat fmt;
    fmt.setSampleRate(kRate);
    fmt.setChannelCount(kChannels);
    fmt.setSampleFormat(QAudioFormat::Int16);
    // Say WHICH two channels these are. Without a channel config Qt leaves the format
    // ChannelConfigUnknown and the stream reaches PipeWire as the unpositioned map AUX0,AUX1
    // instead of FL,FR — so it neither mixes into a surround sink's front pair nor matches the
    // FL/FR entry WirePlumber saved for it, and restore-stream re-applied a stale per-channel
    // volume of 0.0 on every launch: the mirror played, silently, forever (bd remora-8g24).
    fmt.setChannelConfig(QAudioFormat::ChannelConfigStereo);
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) {
        if (error) *error = QStringLiteral("no audio output device");
        return false;
    }
    if (!dev.isFormatSupported(fmt)) {
        // 48 kHz s16 stereo is as vanilla as PCM gets; a device refusing it is worth naming.
        if (error) *error = QStringLiteral("output device rejects 48 kHz s16 stereo");
        return false;
    }

    sink_ = new QAudioSink(dev, fmt, this);
    bool ok = false;
    int bufferMs = qEnvironmentVariableIntValue("REMORA_AUDIO_BUFFER_MS", &ok);
    if (!ok || bufferMs < 20 || bufferMs > 1000) bufferMs = 60;
    sink_->setBufferSize(qsizetype(kRate) * kBytesPerFrame * bufferMs / 1000);
    io_ = sink_->start();
    if (!io_) {
        if (error) *error = QStringLiteral("audio sink failed to start");
        delete sink_;
        sink_ = nullptr;
        return false;
    }
    return true;
}

// The codec headers travel in the stream's CONFIG packet (OpusHead / AudioSpecificConfig / the
// fLaC header slice — the server pre-slices them to exactly the extradata libavcodec wants), so
// the codec cannot open until that packet has arrived. Raw needs no codec at all.
bool AudioPlayer::openCodec(QString *error) {
    AVCodecID id;
    switch (codecId_) {
        case kCodecOpus: id = AV_CODEC_ID_OPUS; break;
        case kCodecAac: id = AV_CODEC_ID_AAC; break;
        case kCodecFlac: id = AV_CODEC_ID_FLAC; break;
        default:
            if (error) *error = QStringLiteral("unknown audio codec id 0x%1").arg(codecId_, 8, 16);
            return false;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        if (error) *error = QStringLiteral("no decoder for audio codec %1").arg(codec ? codec->name : "?");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!ctx_ || !frame_ || !pkt_) {
        if (error) *error = QStringLiteral("audio decoder allocation failed");
        return false;
    }
    if (!extradata_.isEmpty()) {
        ctx_->extradata = static_cast<uint8_t *>(av_mallocz(extradata_.size()
                                                            + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(ctx_->extradata, extradata_.constData(), size_t(extradata_.size()));
        ctx_->extradata_size = extradata_.size();
    }
    ctx_->pkt_timebase = {1, 1000000};  // the wire PTS is microseconds
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        if (error) *error = QStringLiteral("audio decoder open failed");
        return false;
    }
    return true;
}

void AudioPlayer::submit(const MediaPacket &packet) {
    if (failed_ || !sink_) return;

    if (raw_) {
        play(packet.payload.constData(), packet.payload.size());
        return;
    }
    if (packet.config) {
        extradata_ = packet.payload;
        return;
    }
    if (!opened_) {
        QString err;
        if (!openCodec(&err)) {
            // Audio failing must not take the session down — video is the load-bearing half.
            qWarning("mirror: audio disabled: %s", qUtf8Printable(err));
            failed_ = true;
            return;
        }
        opened_ = true;
    }

    if (av_new_packet(pkt_, packet.payload.size()) < 0) return;
    memcpy(pkt_->data, packet.payload.constData(), size_t(packet.payload.size()));
    if (packet.ptsUs) pkt_->pts = qint64(*packet.ptsUs);
    const int sendRc = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (sendRc < 0) return;  // one bad packet; the stream recovers on the next

    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        // Convert whatever the decoder produced (FLTP for opus) to s16 interleaved. The context
        // is created against the FIRST frame's layout and reused; the wire contract fixes the
        // format, so a mid-stream change is a broken stream, not a case to chase.
        if (!swr_) {
            AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_S16, kRate, &frame_->ch_layout,
                                    AVSampleFormat(frame_->format), frame_->sample_rate, 0,
                                    nullptr) < 0
                || swr_init(swr_) < 0) {
                qWarning("mirror: audio disabled: resampler init failed");
                failed_ = true;
                av_frame_unref(frame_);
                return;
            }
        }
        const int maxOut = swr_get_out_samples(swr_, frame_->nb_samples);
        pcm_.resize(qsizetype(maxOut) * kBytesPerFrame);
        uint8_t *out[1] = {reinterpret_cast<uint8_t *>(pcm_.data())};
        const int got = swr_convert(swr_, out, maxOut,
                                    const_cast<const uint8_t **>(frame_->extended_data),
                                    frame_->nb_samples);
        if (got > 0) play(pcm_.constData(), qsizetype(got) * kBytesPerFrame);
        av_frame_unref(frame_);
    }
}

void AudioPlayer::play(const char *data, qint64 bytes) {
    pending_.append(data, bytes);
    flush();
}

void AudioPlayer::flush() {
    if (!io_) return;

    // Trim a REAL backlog from the front, so playback resumes at the live edge rather than
    // working through stale audio. Whole frames only.
    const qsizetype maxBacklog =
        qsizetype(kRate) * kBytesPerFrame * kMaxBacklogMs / 1000 / kBytesPerFrame * kBytesPerFrame;
    if (pending_.size() > maxBacklog) {
        const qsizetype excess = (pending_.size() - maxBacklog) / kBytesPerFrame * kBytesPerFrame;
        pending_.remove(0, excess);
        dropped_ += excess;
    }

    // Offer only whole frames: a partial write that ends mid-frame would resume misaligned.
    const qsizetype offer = pending_.size() / kBytesPerFrame * kBytesPerFrame;
    if (offer > 0) {
        const qint64 written = io_->write(pending_.constData(), offer);
        if (written > 0) {
            // The sink may accept a non-frame multiple; keep the ragged tail for the next pass
            // rather than discarding it, or every partial write would misalign the stream.
            played_ += written;
            pending_.remove(0, written);
        }
    }
    // Per-second stats: a drop figure that keeps CLIMBING is a real fault; one that stops after
    // connect is the backlog trim doing its job.
    if (!qEnvironmentVariableIsEmpty("REMORA_MIRROR_FPS")) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!lastReportMs_) lastReportMs_ = now;
        if (now - lastReportMs_ >= 1000) {
            qInfo("audio: %lld ms held, %lld ms dropped this second",
                  qint64(pending_.size()) * 1000 / (kRate * kBytesPerFrame),
                  (dropped_ - reportedDropped_) * 1000 / (kRate * kBytesPerFrame));
            reportedDropped_ = dropped_;
            lastReportMs_ = now;
        }
    }
    if (pending_.isEmpty()) drain_.stop();
    else if (!drain_.isActive()) drain_.start();
}

void AudioPlayer::stop() {
    if (sink_) {
        sink_->stop();  // also invalidates io_
        delete sink_;
        sink_ = nullptr;
        io_ = nullptr;
    }
    drain_.stop();
    pending_.clear();
    if (swr_) swr_free(&swr_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (frame_) av_frame_free(&frame_);
    if (pkt_) av_packet_free(&pkt_);
    opened_ = false;
}

}  // namespace remora::mirror
