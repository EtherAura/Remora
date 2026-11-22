#include "SessionAudio.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <csignal>
#include <opus/opus.h>
#include <sys/prctl.h>

#include "core/MirrorProto.h"

namespace remora::hostagent {

using namespace remora::mirror;

namespace {

constexpr int kRate = 48000, kChannels = 2;
constexpr int kFrameSamples = 960;  // 20 ms, Opus's natural frame
constexpr int kFrameBytes = kFrameSamples * kChannels * 2;
// A client that stops reading must not grow this process without bound: past this much unsent
// audio, frames are dropped rather than queued (about two seconds at 128 kbit/s).
constexpr qint64 kMaxBacklog = 32 * 1024;

QByteArray pactl(const QStringList &args, bool *ok) {
    QProcess p;
    p.start(QStringLiteral("pactl"), args);
    *ok = p.waitForFinished(3000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    return p.readAllStandardOutput();
}

int parentOf(int pid) {
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    // "pid (comm) state ppid …" — comm may hold spaces and parentheses, so read after the LAST ')'.
    const QByteArray stat = f.readAll();
    const QList<QByteArray> fields = stat.mid(stat.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 1 ? fields[1].toInt() : 0;
}

bool descendsFrom(int pid, const QSet<int> &roots) {
    for (int depth = 0; depth < 32 && pid > 1; ++depth) {
        if (roots.contains(pid)) return true;
        pid = parentOf(pid);
    }
    return false;
}

// RFC 7845's identification header — what the client hands libavcodec as the Opus extradata.
QByteArray opusHead(int preSkip) {
    QByteArray h("OpusHead");
    h.append(char(1));          // version
    h.append(char(kChannels));
    h.append(char(preSkip & 0xff)).append(char((preSkip >> 8) & 0xff));  // little-endian
    for (int i = 0; i < 4; ++i) h.append(char((kRate >> (8 * i)) & 0xff));
    h.append(char(0)).append(char(0));  // output gain
    h.append(char(0));                  // channel mapping family 0: mono or stereo
    return h;
}

}  // namespace

SessionAudio::SessionAudio(quint32 session, QLocalSocket *client, int bitRate, QObject *parent)
    : QObject(parent), session_(session), client_(client), bitRate_(bitRate),
      sinkName_(QStringLiteral("remora-phone-%1").arg(session)) {}

SessionAudio::~SessionAudio() { stop(); }

bool SessionAudio::start(QString *error) {
    bool ok = false;
    const QByteArray module = pactl({QStringLiteral("load-module"),
                                     QStringLiteral("module-null-sink"),
                                     QStringLiteral("sink_name=") + sinkName_,
                                     QStringLiteral("sink_properties=device.description=Remora-phone-%1")
                                         .arg(session_)},
                                    &ok);
    if (!ok) {
        *error = QStringLiteral("pactl could not create a sink for the session — is PipeWire's "
                                "PulseAudio service running?");
        return false;
    }
    moduleIndex_ = QString::fromUtf8(module).trimmed();

    int err = 0;
    opus_ = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_AUDIO, &err);
    if (!opus_) {
        *error = QStringLiteral("could not create an Opus encoder: %1")
                     .arg(QString::fromUtf8(opus_strerror(err)));
        return false;
    }
    opus_encoder_ctl(opus_, OPUS_SET_BITRATE(bitRate_));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(opus_, OPUS_GET_LOOKAHEAD(&lookahead));
    client_->write(streamStartRecord(kCodecOpus));
    client_->write(streamConfigRecord(opusHead(int(lookahead))));

    const auto orphanProof = [] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); };
    parec_ = new QProcess(this);
    parec_->setChildProcessModifier(orphanProof);
    connect(parec_, &QProcess::readyReadStandardOutput, this, &SessionAudio::onPcm);
    parec_->start(QStringLiteral("parec"),
                  {QStringLiteral("--device=") + sinkName_ + QStringLiteral(".monitor"),
                   QStringLiteral("--format=s16le"), QStringLiteral("--rate=%1").arg(kRate),
                   QStringLiteral("--channels=%1").arg(kChannels), QStringLiteral("--raw"),
                   QStringLiteral("--latency-msec=20")});

    subscribe_ = new QProcess(this);
    subscribe_->setChildProcessModifier(orphanProof);
    connect(subscribe_, &QProcess::readyReadStandardOutput, this, [this] {
        bool fresh = false;
        while (subscribe_->canReadLine())
            fresh = subscribe_->readLine().contains("'new' on sink-input") || fresh;
        if (fresh) claimStreams();
    });
    subscribe_->start(QStringLiteral("pactl"), {QStringLiteral("subscribe")});
    claimStreams();
    return true;
}

void SessionAudio::setProcesses(const QSet<int> &pids) {
    pids_ = pids;
    claimStreams();
}

void SessionAudio::claimStreams() {
    if (pids_.isEmpty() || moduleIndex_.isEmpty()) return;
    bool ok = false;
    const QByteArray json =
        pactl({QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"),
               QStringLiteral("sink-inputs")},
              &ok);
    if (!ok) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject in = v.toObject();
        const QString index = QString::number(in.value(QStringLiteral("index")).toInteger());
        if (moved_.contains(index)) continue;
        const int pid = in.value(QStringLiteral("properties"))
                            .toObject()
                            .value(QStringLiteral("application.process.id"))
                            .toString()
                            .toInt();
        if (pid <= 0 || !descendsFrom(pid, pids_)) continue;
        bool moved = false;
        pactl({QStringLiteral("move-sink-input"), index, sinkName_}, &moved);
        if (moved) {
            moved_.insert(index);
            qInfo("host agent: session %u: audio from pid %d now plays on the phone", session_, pid);
        }
    }
}

void SessionAudio::onPcm() {
    pcm_.append(parec_->readAllStandardOutput());
    unsigned char packet[4000];
    while (pcm_.size() >= kFrameBytes) {
        const int n = opus_encode(opus_, reinterpret_cast<const opus_int16 *>(pcm_.constData()),
                                  kFrameSamples, packet, sizeof packet);
        const quint64 ptsUs = samples_ * 1'000'000ull / kRate;
        samples_ += kFrameSamples;
        pcm_.remove(0, kFrameBytes);
        if (n <= 0 || !client_ || client_->bytesToWrite() > kMaxBacklog) continue;
        client_->write(streamFrameRecord(ptsUs, false, QByteArray(reinterpret_cast<char *>(packet), n)));
    }
}

void SessionAudio::stop() {
    for (QProcess *p : {parec_, subscribe_}) {
        if (!p || p->state() == QProcess::NotRunning) continue;
        p->terminate();
        if (!p->waitForFinished(1000)) {
            p->kill();
            p->waitForFinished(500);
        }
    }
    parec_ = subscribe_ = nullptr;
    // Unloading the sink sends every stream on it back to the desk's default output.
    if (!moduleIndex_.isEmpty()) {
        bool ok = false;
        pactl({QStringLiteral("unload-module"), moduleIndex_}, &ok);
        moduleIndex_.clear();
    }
    if (opus_) opus_encoder_destroy(opus_);
    opus_ = nullptr;
}

}  // namespace remora::hostagent
