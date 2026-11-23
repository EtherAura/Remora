#include "HostSession.h"

#include <QFileInfo>
#include <QLocalSocket>
#include <QTimer>

#include <cmath>
#include <csignal>
#include <sys/prctl.h>

#include "DeskClipboard.h"
#include "DeskLayout.h"
#include "Gpu.h"
#include "KWinWayland.h"
#include "PipeWireCapture.h"
#include "SessionAudio.h"
#include "WindowPlacer.h"

namespace remora::hostagent {

using namespace remora::mirror;

namespace {
// How long a session waits for its client's first video connection, and for one that went to
// come back (past the phone app's own five minutes away).
constexpr int kFirstVideoMs = 30'000, kVideoAwaySec = 330;
}  // namespace

std::optional<SessionParams> SessionParams::fromOptions(const QHash<QString, QString> &o,
                                                        QString *error) {
    SessionParams p;
    // new_display=WxH[/dpi]; empty or absent keeps the phone-shaped default.
    const QString nd = o.value(QStringLiteral("new_display"));
    if (!nd.isEmpty() && nd != QLatin1String("auto")) {
        const QString dims = nd.section(QLatin1Char('/'), 0, 0);
        const int x = dims.indexOf(QLatin1Char('x'), 0, Qt::CaseInsensitive);
        const int w = x > 0 ? dims.left(x).toInt() : 0, h = x > 0 ? dims.mid(x + 1).toInt() : 0;
        if (w < 200 || h < 200 || w > 8192 || h > 8192) {
            *error = QStringLiteral("new_display=%1 is not a size this computer can show (200 to "
                                    "8192 pixels a side)")
                         .arg(nd);
            return std::nullopt;
        }
        p.pixels = {w, h};
        const int dpi = nd.contains(QLatin1Char('/')) ? nd.section(QLatin1Char('/'), 1).toInt() : 0;
        p.scale = dpi > 0 ? dpi / 160.0 : 1.0;
    }
    const QString codec = o.value(QStringLiteral("video_codec"), QStringLiteral("h264"));
    if (codec == QLatin1String("h264")) p.codec = QStringLiteral("h264");
    else if (codec == QLatin1String("h265")) p.codec = QStringLiteral("hevc");
    else {
        *error = QStringLiteral("video_codec=%1 is not offered for this computer's apps; ask for "
                                "h264 or h265")
                     .arg(codec);
        return std::nullopt;
    }
    if (o.contains(QStringLiteral("video_bit_rate")))
        p.bitRate = o.value(QStringLiteral("video_bit_rate")).toInt();
    if (o.contains(QStringLiteral("max_fps")))
        p.maxFps = qBound(1, int(o.value(QStringLiteral("max_fps")).toDouble()), 240);
    p.maxSize = o.value(QStringLiteral("max_size")).toInt();
    p.audio = o.value(QStringLiteral("audio")) != QLatin1String("false");
    if (o.contains(QStringLiteral("audio_bit_rate")))
        p.audioBitRate = qBound(16'000, o.value(QStringLiteral("audio_bit_rate")).toInt(), 512'000);
    p.startApp = o.value(QStringLiteral("start_app"));
    // The in-image agent's "+pkg" (force-stop first) and "?label" forms have no meaning here.
    if (p.startApp.startsWith(QLatin1Char('+'))) p.startApp.remove(0, 1);
    p.closeOnAppExit = o.value(QStringLiteral("close_on_app_exit")) == QLatin1String("true");
    return p;
}

HostSession::HostSession(const Services &services, quint32 id, const QString &outputName,
                         const SessionParams &params, QObject *parent)
    : QObject(parent), s_(services), id_(id), outputName_(outputName), params_(params),
      baseName_(outputName), refPixels_(params.pixels), refScale_(params.scale),
      wantedPixels_(params.pixels) {
    reflowTimer_ = new QTimer(this);
    reflowTimer_->setSingleShot(true);
    reflowTimer_->setInterval(250);  // a burst of RESIZEs (a drag, a fold) becomes one reflow
    connect(reflowTimer_, &QTimer::timeout, this, &HostSession::beginReflow);
    focusTimer_ = new QTimer(this);
    focusTimer_->setSingleShot(true);
    focusTimer_->setInterval(1500);
    connect(focusTimer_, &QTimer::timeout, this, [this] {
        deliver(keys_.expire());
        qWarning("host agent: session %u: keys dropped — its window did not get keyboard focus back",
                 id_);
    });
    // Focus is not usable the instant KWin reports it: a key injected right then reached nothing,
    // neither the window that lost focus nor the one that gained it. Measured with a calculator.
    focusSettle_ = new QTimer(this);
    focusSettle_->setSingleShot(true);
    focusSettle_->setInterval(100);
    connect(focusSettle_, &QTimer::timeout, this, [this] {
        focusTimer_->stop();
        deliver(keys_.focusChanged(true));
    });
    connect(s_.placer, &WindowPlacer::focusChanged, this, [this](quint32 session, bool ours) {
        if (session != id_) return;
        if (ours) return focusSettle_->start();
        focusSettle_->stop();
        deliver(keys_.focusChanged(false));
        regainFocus();  // keys held while it was settling still wait for it
    });
    connect(s_.placer, &WindowPlacer::windowMinimum, this, [this](quint32 session, QSize min) {
        if (session != id_) return;
        const QSize grown = minLogical_.expandedTo(min);
        if (grown == minLogical_) return;
        minLogical_ = grown;
        reflowTimer_->start();  // the scale, lowered until it fits (wantedScale)
    });
    connect(s_.placer, &WindowPlacer::windowPlaced, this,
            [this](quint32 session, const QString &, int pid, bool) {
                if (session != id_ || pid <= 0) return;
                pids_.insert(pid);
                if (audio_) audio_->setProcesses(pids_);
            });
    frameSocket_ = s_.runtimeDir + QStringLiteral("/s%1-frames.sock").arg(id);
    videoSocket_ = s_.runtimeDir + QStringLiteral("/s%1-video.sock").arg(id);
    encoder_ = new QProcess(this);
    encoder_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    // The encoder holds GPU memory; it must not outlive the agent however the agent ends.
    encoder_->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    connect(encoder_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        if (!ended_)
            fail(QStringLiteral("the encoder stopped (%1 %2)")
                     .arg(st == QProcess::CrashExit ? QStringLiteral("crashed,")
                                                    : QStringLiteral("exit"))
                     .arg(code));
    });
}

HostSession::~HostSession() { end(QStringLiteral("the agent is stopping")); }

QString HostSession::outputName() const { return QStringLiteral("Virtual-") + outputName_; }

QList<outputs::VirtualPlacement> HostSession::placements() const {
    QList<outputs::VirtualPlacement> out{{outputName(), params_.scale}};
    if (nextOutput_) out.append({QStringLiteral("Virtual-") + nextName_, nextScale_});
    return out;
}

void HostSession::start() {
    QString description = QStringLiteral("Remora phone");
    if (!params_.startApp.isEmpty()) description += QStringLiteral(": ") + params_.startApp;
    output_ = s_.wl->createVirtualOutput(outputName_, description, params_.pixels);
    connect(output_, &VirtualOutput::ready, this, &HostSession::onOutputReady);
    connect(output_, &VirtualOutput::failed, this, [this](const QString &e) {
        fail(QStringLiteral("KWin refused the virtual output: ") + e);
    });
    connect(output_, &VirtualOutput::closed, this,
            [this] { fail(QStringLiteral("KWin removed the virtual output")); });
}

void HostSession::onOutputReady(quint64 serial) {
    serial_ = serial;
    layoutWaiting_ = true;
    connect(s_.desk, &DeskLayout::settled, this, &HostSession::onDeskSettled);
    emit outputLive(id_);  // the agent re-lays the desk out with this output in it
}

void HostSession::onDeskSettled(bool ok, const QString &detail) {
    refreshGeometry();
    if (std::exchange(rescaleWaiting_, false)) {
        // Placed again at the new scale: a window that did not fit before was left on the desk,
        // and one that did is maximized to the output's new extent.
        s_.placer->moveTo(id_, outputName());
    }
    if (reflowWaiting_) {
        reflowWaiting_ = false;
        onReflowSettled();
        return;
    }
    if (!layoutWaiting_) return;
    layoutWaiting_ = false;
    if (!ok) {
        // A desk that will not take the layout is the operator's to see, but the session can
        // still serve: the output exists, only its place on the desk is off.
        qWarning("host agent: session %u: the desk layout did not settle: %s", id_,
                 qUtf8Printable(detail));
    }
    startEncoder();
}

void HostSession::refreshGeometry() {
    if (const auto g = s_.desk->geometryOf(outputName())) input_.setOutput(QRectF(*g));
}

void HostSession::startEncoder() {
    const QString node = renderNodeForConnector(s_.desk->primaryOutput());
    if (node.isEmpty()) {
        fail(QStringLiteral("could not tell which GPU drives %1").arg(s_.desk->primaryOutput()));
        return;
    }
    QFile::remove(frameSocket_);
    QFile::remove(videoSocket_);
    QStringList args{QStringLiteral("--frame-socket"), frameSocket_,
                     QStringLiteral("--video-socket"), videoSocket_,
                     QStringLiteral("--drm-node"), node,
                     QStringLiteral("--codec"), params_.codec,
                     QStringLiteral("--bit-rate"), QString::number(params_.bitRate),
                     QStringLiteral("--fps"), QString::number(params_.maxFps),
                     // A video connection may go and come back: the phone app lets its picture
                     // go while the phone is away (the screen off, another app in front) and ends
                     // the session itself after five minutes. This is the backstop, a little
                     // later; a client that never attaches at all is ended in takeControl.
                     QStringLiteral("--idle-timeout"), QString::number(kVideoAwaySec)};
    if (params_.maxSize > 0) args << QStringLiteral("--max-size") << QString::number(params_.maxSize);
    encoder_->start(s_.encoder, args);
    // The encoder creates both sockets once it listens; poll for the frame socket.
    auto *wait = new QTimer(this);
    auto tries = std::make_shared<int>(0);
    connect(wait, &QTimer::timeout, this, [this, wait, tries] {
        if (ended_) return wait->deleteLater();
        if (!QFileInfo::exists(frameSocket_)) {
            if (++*tries < 100) return;
            wait->deleteLater();
            return fail(QStringLiteral("the encoder never started listening"));
        }
        wait->deleteLater();
        startCapture();
    });
    wait->start(50);
}

void HostSession::startCapture() {
    capture_ = new PipeWireCapture(this);
    connect(capture_, &PipeWireCapture::failed, this, &HostSession::fail);
    connect(capture_, &PipeWireCapture::streaming, this, [this](QSize, bool) { launchApp(); });
    QString error;
    if (!capture_->start(serial_, frameSocket_, &error)) fail(error);
}

void HostSession::launchApp() {
    if (readyEmitted_) return;
    if (!params_.startApp.isEmpty()) {
        QString error;
        if (!s_.placer->watch(id_, params_.startApp, outputName(), &error)) return fail(error);
        if (params_.closeOnAppExit) {
            connect(s_.placer, &WindowPlacer::allWindowsClosed, this, [this](quint32 session) {
                if (session == id_) end(QStringLiteral("the app closed its last window"));
            });
        }
        // kstart goes through KIO: activation token, a systemd scope of the app's own, and
        // D-Bus activation for an app that asks for it — the way the desk's own launcher starts
        // it, so a single-instance app answers exactly as it would there. KIO hands the app this
        // environment, so the app starts pointed at the session's sink (openAudioSink).
        QProcess kstart;
        kstart.setProgram(QStringLiteral("kstart"));
        kstart.setArguments({QStringLiteral("--application"), params_.startApp});
        if (params_.audio && openAudioSink()) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("PULSE_SINK"), audio_->sinkName());
            kstart.setProcessEnvironment(env);
        }
        if (!kstart.startDetached())
            return fail(QStringLiteral("could not run kstart to launch %1").arg(params_.startApp));
        QTimer::singleShot(2500, this, [this] {
            if (!ended_) s_.placer->adoptIfUnplaced(id_);
        });
    }
    readyEmitted_ = true;
    emit ready(true, QString());
}

void HostSession::takeControl(QLocalSocket *control, const QByteArray &pending) {
    control_ = control;
    control->setParent(this);
    connect(control, &QLocalSocket::readyRead, this, &HostSession::onControlData);
    connect(control, &QLocalSocket::disconnected, this,
            [this] { end(QStringLiteral("the client closed the session")); });
    // A client that never attaches its video must not pin the GPU for long.
    QTimer::singleShot(kFirstVideoMs, this, [this] {
        if (!videoAttached_) end(QStringLiteral("no video connection came"));
    });
    if (!pending.isEmpty()) {
        parser_.feed(pending);
        onControlData();
    }
}

void HostSession::onControlData() {
    if (control_) parser_.feed(control_->readAll());
    while (const auto m = parser_.next()) {
        if (m->type == ControlRecord::Resize) {
            requestResize(QSize(m->width, m->height), m->dpi);
            continue;
        }
        if (m->type == ControlRecord::Clipboard) {
            s_.clipboard->set(m->text);
            // paste: the in-image agent pastes into the focused field; here that is Ctrl+V into
            // the focused window, once the desk's clipboard holds the text.
            if (m->paste) {
                using Op = hostinput::InputOp;
                constexpr quint32 kCtrl = 29, kV = 47;
                inject({Op{Op::Key, {}, kCtrl, true}, Op{Op::Key, {}, kV, true},
                        Op{Op::Key, {}, kV, false}, Op{Op::Key, {}, kCtrl, false}});
            }
            if (m->sequence && control_) control_->write(clipboardAckRecord(m->sequence));
            continue;
        }
        inject(input_.translate(*m));
    }
    if (parser_.error()) end(QStringLiteral("the client broke the control framing"));
}

void HostSession::requestResize(QSize pixels, int dpi) {
    if (!readyEmitted_ || ended_ || pixels.width() < 200 || pixels.height() < 200 ||
        pixels.width() > 8192 || pixels.height() > 8192)
        return;
    wantedPixels_ = pixels;
    // A density given with the size is the client's own (the phone app's scale): it anchors the
    // long-side rule from here on, so a later resize without one keeps that scale.
    if (dpi > 0) {
        refPixels_ = pixels;
        refScale_ = dpi / 160.0;
    }
    reflowTimer_->start();
}

double HostSession::wantedScale() const {
    // Density follows the long side, as the in-image agent's reflow does: a rotation keeps it,
    // and a window grown on a desk shows the same UI larger rather than more of it smaller.
    const int refLong = std::max(refPixels_.width(), refPixels_.height());
    const int newLong = std::max(wantedPixels_.width(), wantedPixels_.height());
    const double rule = refLong > 0 ? refScale_ * newLong / refLong : refScale_;
    return std::min(rule, outputs::fitScale(wantedPixels_, minLogical_));
}

bool HostSession::reflowWanted() const {
    // In KWin's own steps of 1/120: a scale it would store the same is no change.
    return wantedPixels_ != params_.pixels ||
           std::lround(wantedScale() * 120) != std::lround(params_.scale * 120);
}

void HostSession::beginReflow() {
    if (ended_) return;
    if (nextOutput_) return reflowTimer_->start();  // one at a time; the latest size wins after
    if (!reflowWanted()) return;
    if (wantedPixels_ == params_.pixels) {
        // Only the scale: the same output, laid out again at the new one. Its pixels, and so the
        // capture and the encoder, stay as they are; the app is laid out again in what it covers.
        params_.scale = wantedScale();
        qInfo("host agent: session %u rescaled to @%g", id_, params_.scale);
        rescaleWaiting_ = true;
        emit outputLive(id_);
        return;
    }
    nextPixels_ = wantedPixels_;
    nextScale_ = wantedScale();
    nextName_ = outputName_ == baseName_ ? baseName_ + QStringLiteral("-b") : baseName_;
    nextOutput_ = s_.wl->createVirtualOutput(nextName_, QStringLiteral("Remora phone"), nextPixels_);
    connect(nextOutput_, &VirtualOutput::ready, this, [this](quint64 serial) {
        nextSerial_ = serial;
        reflowWaiting_ = true;
        emit outputLive(id_);  // the layout now places both outputs
    });
    connect(nextOutput_, &VirtualOutput::failed, this, [this](const QString &e) {
        abandonReflow(QStringLiteral("KWin refused the new output: ") + e);
    });
    connect(nextOutput_, &VirtualOutput::closed, this,
            [this] { abandonReflow(QStringLiteral("KWin removed the new output")); });
}

void HostSession::onReflowSettled() {
    s_.placer->moveTo(id_, QStringLiteral("Virtual-") + nextName_);
    nextCapture_ = new PipeWireCapture(this);
    connect(nextCapture_, &PipeWireCapture::streaming, this, &HostSession::finishReflow);
    connect(nextCapture_, &PipeWireCapture::failed, this, &HostSession::abandonReflow);
    QString error;
    if (!nextCapture_->start(nextSerial_, frameSocket_, &error)) abandonReflow(error);
}

void HostSession::finishReflow() {
    if (!nextOutput_ || !nextCapture_) return;
    // The new capture is a second publisher on the same encoder, which follows whichever one is
    // delivering; stopping the old one leaves it no choice, and its frames' new size makes it
    // send FORMAT and CONFIG before the next frame.
    if (capture_) {
        capture_->disconnect(this);
        capture_->stop();
        capture_->deleteLater();
    }
    if (output_) {
        output_->disconnect(this);
        delete output_;
    }
    capture_ = std::exchange(nextCapture_, nullptr);
    output_ = std::exchange(nextOutput_, nullptr);
    capture_->disconnect(this);
    connect(capture_, &PipeWireCapture::failed, this, &HostSession::fail);
    output_->disconnect(this);
    connect(output_, &VirtualOutput::closed, this,
            [this] { fail(QStringLiteral("KWin removed the virtual output")); });
    outputName_ = nextName_;
    params_.pixels = nextPixels_;
    params_.scale = nextScale_;
    qInfo("host agent: session %u reflowed to %dx%d @%g", id_, params_.pixels.width(),
          params_.pixels.height(), params_.scale);
    emit outputLive(id_);  // one output again
    if (reflowWanted()) reflowTimer_->start();  // resized again meanwhile
}

void HostSession::abandonReflow(const QString &why) {
    qWarning("host agent: session %u: reflow abandoned: %s", id_, qUtf8Printable(why));
    reflowWaiting_ = false;
    if (nextCapture_) {
        nextCapture_->disconnect(this);
        nextCapture_->stop();
        nextCapture_->deleteLater();
        nextCapture_ = nullptr;
    }
    if (nextOutput_) {
        nextOutput_->disconnect(this);
        delete nextOutput_;
        nextOutput_ = nullptr;
        s_.placer->moveTo(id_, outputName());  // back where the stream still is
        emit outputLive(id_);
    }
}

void HostSession::inject(const QList<hostinput::InputOp> &ops) {
    deliver(keys_.pass(ops));
    regainFocus();
}

// Keys wait while another window has focus: the phone's window gets it back, once per wait.
void HostSession::regainFocus() {
    if (!keys_.holding() || ended_ || focusTimer_->isActive() || focusSettle_->isActive()) return;
    s_.placer->activate(id_);
    focusTimer_->start();
}

void HostSession::deliver(const QList<hostinput::InputOp> &ops) {
    using Op = hostinput::InputOp;
    for (const Op &op : ops) {
        switch (op.kind) {
            case Op::PointerMotion: s_.wl->pointerMotionAbsolute(op.at); break;
            case Op::PointerButton: s_.wl->pointerButton(op.code, op.pressed); break;
            case Op::PointerAxis: s_.wl->pointerAxis(op.horizontal, op.value); break;
            case Op::TouchDown: s_.wl->touchDown(op.code, op.at); break;
            case Op::TouchMotion: s_.wl->touchMotion(op.code, op.at); break;
            case Op::TouchUp: s_.wl->touchUp(op.code); break;
            case Op::TouchFrame: s_.wl->touchFrame(); break;
            case Op::Key: s_.wl->key(op.code, op.pressed); break;
            case Op::Keysym: s_.wl->keysym(op.code, op.pressed); break;
        }
    }
    if (!ops.isEmpty()) s_.wl->flush();
}

void HostSession::sendDeskClipboard(const QString &text) {
    if (control_ && control_->state() == QLocalSocket::ConnectedState)
        control_->write(deviceClipboardRecord(text));
}

namespace {
// How far a client may fall behind before the relay skips ahead (~0.5 s at 8 Mbit/s), and how far
// it must drain before the stream resumes. Live video is worth only its newest frame: a client
// that decodes slower than the stream arrives must lose frames, or it shows the past forever.
constexpr qint64 kBehindBytes = 512 << 10, kCaughtUpBytes = 64 << 10;
}  // namespace

void HostSession::attachVideo(QLocalSocket *video) {
    // One video connection at a time: a reattach replaces the old one.
    if (video_) video_->deleteLater();
    video_ = video;
    videoAttached_ = true;
    video->setParent(this);
    videoStarted_ = false;
    connect(video, &QLocalSocket::bytesWritten, this, [this, video] {
        if (video == video_ && skipping_ && video->bytesToWrite() < kCaughtUpBytes) attachEncoder();
    });
    connect(video, &QLocalSocket::disconnected, this, [this, video] {
        // The video going leaves the session up, so the client can reattach.
        if (video == video_ && encoderVideo_) encoderVideo_->disconnectFromServer();
    });
    attachEncoder();
}

// The relay, record by record: the encoder's video socket speaks the stream a video connection
// carries (START, FORMAT, CONFIG, FRAME), so records pass through as they are — until the client
// falls behind. Then this attachment is dropped, and a fresh one made once the client has
// drained: the encoder opens every new consumer with a keyframe, so the client resumes on the
// present picture, cleanly, instead of on a backlog that only grows.
void HostSession::attachEncoder() {
    if (encoderVideo_) {
        encoderVideo_->disconnect(this);
        encoderVideo_->abort();
        encoderVideo_->deleteLater();
    }
    skipping_ = false;
    relay_ = std::make_unique<RecordReader>(kStreamRecordMax);
    auto *enc = new QLocalSocket(this);
    encoderVideo_ = enc;
    connect(enc, &QLocalSocket::readyRead, this, [this, enc] {
        if (!video_ || enc != encoderVideo_) return;
        relay_->feed(enc->readAll());
        while (const auto r = relay_->next()) {
            // A START opens the client's stream once; a re-attachment's own is not repeated.
            if (r->type == quint8(StreamRecord::Start)) {
                if (videoStarted_) continue;
                videoStarted_ = true;
            }
            if (r->type == quint8(StreamRecord::Frame) && video_->bytesToWrite() > kBehindBytes) {
                qInfo("host agent: session %u: the client is behind; skipping ahead", id_);
                skipping_ = true;
                enc->disconnect(this);
                enc->abort();
                return;
            }
            video_->write(record(r->type, r->body));
        }
    });
    connect(enc, &QLocalSocket::errorOccurred, this, [this, enc](QLocalSocket::LocalSocketError) {
        if (enc != encoderVideo_ || !video_ || skipping_) return;
        video_->write(streamEndRecord(kEndFailed, QStringLiteral("the encoder's video socket: ") +
                                                      enc->errorString()));
        video_->flush();
        video_->disconnectFromServer();
    });
    enc->connectToServer(videoSocket_);
}

// The sink comes before the app, so the app can be launched pointed at it: its sound starts on
// the phone, never on the desk first. A client that says it takes no sound (audio=false, a
// desktop mirror without --audio) gets none of this. One that wants it attaches moments after the
// session is up; if none does, the sink goes and the app's streams fall back to the desk.
bool HostSession::openAudioSink() {
    auto *a = new SessionAudio(id_, params_.audioBitRate, this);
    QString error;
    if (!a->openSink(&error)) {
        qWarning("host agent: session %u: %s", id_, qUtf8Printable(error));
        delete a;
        return false;
    }
    audio_ = a;
    audio_->setProcesses(pids_);
    QTimer::singleShot(5000, this, [this, a] {
        if (audio_ != a || a->attached()) return;
        audio_ = nullptr;
        delete a;
        qInfo("host agent: session %u: no audio connection — the app's sound stays on this "
              "computer", id_);
    });
    return true;
}

void HostSession::attachAudio(QLocalSocket *audio) {
    audio->setParent(this);
    const auto unavailable = [audio](const QString &why) {
        // Unavailable, not failed: a session without its sound is still the session.
        audio->write(streamEndRecord(kEndUnavailable, why));
        audio->flush();
        audio->disconnectFromServer();
    };
    // The sink loaded at launch, unless a client already has it or it was given up: a reattach
    // replaces the old stream (its streams go back to the desk first), and a late client gets a
    // new sink with the app's streams moved onto it.
    if (!audio_ || audio_->attached()) {
        delete audio_;
        audio_ = nullptr;
        if (!openAudioSink()) return unavailable(QStringLiteral(
            "pactl could not create a sink for the session — is PipeWire's PulseAudio service "
            "running?"));
    }
    auto *a = audio_;
    QString error;
    if (!a->attach(audio, &error)) {
        audio_ = nullptr;
        delete a;
        return unavailable(error);
    }
    connect(audio, &QLocalSocket::disconnected, this, [this, a] {
        if (audio_ != a) return;
        audio_ = nullptr;
        a->deleteLater();
    });
}

void HostSession::fail(const QString &why) {
    if (!readyEmitted_) {
        readyEmitted_ = true;
        emit ready(false, why);
    }
    end(why);
}

void HostSession::end(const QString &why) {
    if (ended_) return;
    ended_ = true;
    inject(input_.releaseAll());
    delete audio_;  // first: its streams go back to the desk's speakers
    audio_ = nullptr;
    s_.placer->release(id_);
    if (capture_) capture_->stop();
    if (nextCapture_) nextCapture_->stop();
    delete nextOutput_;
    nextOutput_ = nullptr;
    if (encoder_->state() != QProcess::NotRunning) {
        // Never block the agent on an encoder's exit (NVENC teardown takes its time), and never
        // destroy a QProcess whose child still runs: hand it to the agent, which outlives this
        // session, and let it reap itself — SIGKILL if SIGTERM was not enough.
        QProcess *enc = encoder_;
        enc->disconnect(this);
        enc->setParent(parent());
        connect(enc, &QProcess::finished, enc, &QObject::deleteLater);
        enc->terminate();
        QTimer::singleShot(3000, enc, [enc] { enc->kill(); });
    }
    if (video_) video_->disconnectFromServer();
    if (control_) control_->disconnectFromServer();
    delete output_;  // closes the stream; KWin removes the output
    output_ = nullptr;
    s_.wl->flush();
    QFile::remove(frameSocket_);
    QFile::remove(videoSocket_);
    emit ended(id_, why);
}

}  // namespace remora::hostagent
