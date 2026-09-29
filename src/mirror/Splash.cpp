#include "Splash.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace remora::mirror {

void FramePrefetcher::setFrames(const QStringList &paths) {
    QMutexLocker lock(&m_);
    if (paths == nextPaths_ && !nextReady_.isEmpty()) {
        // This is the part setNextFrames() was decoding ahead: adopt its queue instead of
        // starting empty, which is what makes the switch cost no frame at all. Resume decoding
        // BEFORE any frame still in flight for it — that one is about to be dropped by the
        // generation check, and skipping it would leave a hole in the sequence.
        ready_ = std::move(nextReady_);
        next_ = nextIdx_ - nextInFlight_;
    } else {
        ready_.clear();
        next_ = 0;
    }
    paths_ = paths;
    ++gen_;  // anything still decoding for the old list must not land in this queue
    nextPaths_.clear();
    nextReady_.clear();
    nextIdx_ = 0;
    ++nextGen_;
    wake_.wakeAll();
}

void FramePrefetcher::setNextFrames(const QStringList &paths) {
    QMutexLocker lock(&m_);
    if (paths == nextPaths_) return;  // idempotent: advanceFrame re-announces the same part
    nextPaths_ = paths;
    nextReady_.clear();
    nextIdx_ = 0;
    ++nextGen_;
    wake_.wakeAll();
}

QImage FramePrefetcher::take() {
    QMutexLocker lock(&m_);
    if (ready_.isEmpty()) return {};
    QImage img = ready_.dequeue();
    wake_.wakeAll();  // a slot freed up — let the decoder run again
    return img;
}

void FramePrefetcher::stop() {
    {
        QMutexLocker lock(&m_);
        stop_ = true;
    }
    wake_.wakeAll();
    wait(2000);
}

void FramePrefetcher::run() {
    for (;;) {
        QString path;
        bool ahead = false;  // filling the NEXT part rather than the current one
        int gen = 0;
        {
            QMutexLocker lock(&m_);
            for (;;) {
                if (stop_) return;
                // The current part always wins: the next part is only worth decoding with the
                // playing one's queue already full, which at 60 fps it is almost all the time.
                if (!paths_.isEmpty() && ready_.size() < kQueueDepth) {
                    ahead = false;
                    break;
                }
                if (nextIdx_ < nextPaths_.size() && nextReady_.size() < kPreloadDepth) {
                    ahead = true;
                    break;
                }
                wake_.wait(&m_);
            }
            if (ahead) {
                path = nextPaths_.at(nextIdx_++);
                ++nextInFlight_;
                gen = nextGen_;
            } else {
                if (next_ >= paths_.size()) next_ = 0;  // the loop part repeats
                path = paths_.at(next_++);
                gen = gen_;
            }
        }
        QImage img(path);  // the expensive part, and the whole point of being off the GUI thread
        QMutexLocker lock(&m_);
        if (ahead) --nextInFlight_;
        if (stop_) return;
        if (img.isNull()) continue;
        // Dropped when the part changed while this was decoding — see the generation comment in
        // the header.
        if (ahead) {
            if (gen == nextGen_) nextReady_.enqueue(img);
        } else if (gen == gen_) {
            ready_.enqueue(img);
        }
    }
}


// The fork's lit heuristic: a compressed screencap of a rendered frame is far bigger than one
// of a black screen. Threshold and grace are its measured constants.
static constexpr qint64 kLitScreencapBytes = 100000;
static constexpr qint64 kLitGraceMs = 60000;

SplashController::SplashController(SplashOptions opts, QObject *parent)
    : QObject(parent), o_(std::move(opts)) {
    statusTimer_.setInterval(300);
    connect(&statusTimer_, &QTimer::timeout, this, &SplashController::pollStatus);
    probeTimer_.setInterval(2000);
    connect(&probeTimer_, &QTimer::timeout, this, &SplashController::probeTick);
    connect(&frameTimer_, &QTimer::timeout, this, &SplashController::advanceFrame);
    // The ending's hold on the window is bounded: 8 s covers a real ending (A17's is 3.5 s) with
    // room to spare, and guarantees a stuck animation costs a short delay rather than the mirror.
    endingCap_.setSingleShot(true);
    // Must cover the worst honest case: the looping part finishes its CURRENT cycle first (1.5 s
    // for the A17 animation) and only then does the 3.5 s ending play — 5 s exactly, so a 5 s cap
    // truncated it every time. 8 s leaves headroom without letting a stuck animation hold the
    // mirror for long.
    endingCap_.setInterval(8000);
    connect(&endingCap_, &QTimer::timeout, this, [this] {
        qWarning("mirror: the boot animation did not finish within the cap — showing the mirror");
        finishAnimation();
    });
}

SplashController::~SplashController() {
    // An abort mid-acquisition must not destroy live QProcess children (Qt warns loudly);
    // reap them first. The extract dir was made writable right after unzip, so its removal
    // succeeds even when we die between unzip and playback.
    for (QProcess *p : findChildren<QProcess *>()) {
        if (p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(500);
        }
    }
}

void SplashController::begin() {
    if (!o_.statusPath.isEmpty()) statusTimer_.start();
    probeTimer_.start();
    // The first ticks are deferred into the event loop: begin() runs before app.exec(), and a
    // marker already in the status file would otherwise emit into QCoreApplication::exit()
    // before the loop exists — which Qt silently ignores, leaving the splash immortal.
    QTimer::singleShot(0, this, &SplashController::tryAcquire);
    QTimer::singleShot(0, this, &SplashController::pollStatus);
    QTimer::singleShot(0, this, &SplashController::probeTick);
}

// ---- status file ----

void SplashController::pollStatus() {
    if (o_.statusPath.isEmpty() || handedOff_ || aborted_) return;
    QFile f(o_.statusPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    const SplashStatus s = parseSplashStatus(QString::fromUtf8(f.readAll()));
    if (s.abort) {
        aborted_ = true;
        emit abortRequested();
        return;
    }
    if (s.text != lastText_) {
        lastText_ = s.text;
        emit statusText(lastText_);
    }
    if (s.booting && !bootingSeen_) {
        bootingSeen_ = true;
        if (animReady_ && !frameTimer_.isActive()) frameTimer_.start();
    }
    if (s.attach && !attachSeen_) {
        attachSeen_ = true;
        maybeHandOff();
    }
}

// ---- boot probe ----

void SplashController::probeTick() {
    if (handedOff_ || aborted_ || (probe_ && probe_->state() != QProcess::NotRunning)) return;
    // A rebooting device drops off adb; for TCP serials re-offer the connection each tick
    // (a no-op when already connected), exactly like the reference splash.
    if (o_.serial.contains(QLatin1Char(':')))
        QProcess::startDetached(QStringLiteral("adb"), {QStringLiteral("connect"), o_.serial});
    if (!probe_) {
        probe_ = new QProcess(this);
        probe_->setProcessChannelMode(QProcess::MergedChannels);
        connect(probe_, &QProcess::finished, this, &SplashController::onProbeFinished);
    }
    // One round trip for both signals: boot_completed, then the lit check's byte count.
    probe_->start(QStringLiteral("adb"),
                  {QStringLiteral("-s"), o_.serial, QStringLiteral("shell"),
                   QStringLiteral("getprop sys.boot_completed; screencap -p 2>/dev/null | wc -c")});
}

void SplashController::onProbeFinished() {
    const QStringList lines = QString::fromUtf8(probe_->readAll())
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) return;  // device offline mid-boot — keep probing
    const bool bootedNow = lines.first().trimmed() == QLatin1String("1");
    if (bootedNow && !booted_) {
        booted_ = true;
        bootedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    }
    const qint64 shotBytes = lines.size() > 1 ? lines.last().trimmed().toLongLong() : 0;
    if (booted_ && (shotBytes > kLitScreencapBytes ||
                    QDateTime::currentMSecsSinceEpoch() - bootedAtMs_ > kLitGraceMs)) {
        ready_ = true;
        probeTimer_.stop();  // the probe's job is done; the reference thread exits here too
        maybeHandOff();
    }
    // The animation zip may only become pullable once adb is back — retry acquisition here.
    if (!animReady_) tryAcquire();
}

// ---- animation acquisition ----

void SplashController::tryAcquire() {
    if (animReady_ || (acquire_ && acquire_->state() != QProcess::NotRunning)) return;
    if (o_.cacheZip.isEmpty()) return;  // no cache path = no animation, status text only
    if (QFileInfo::exists(o_.cacheZip)) {
        extractZip();
        return;
    }
    if (!acquire_) {
        acquire_ = new QProcess(this);
        connect(acquire_, &QProcess::finished, this, &SplashController::onAcquireStep);
    }
    const QStringList candidates = bootAnimSearchPaths();
    if (pullCandidate_ >= candidates.size()) {
        pullCandidate_ = 0;  // full miss: start over on the next probe tick (device offline?)
        return;
    }
    acquire_->start(QStringLiteral("adb"),
                    {QStringLiteral("-s"), o_.serial, QStringLiteral("pull"),
                     candidates[pullCandidate_], o_.cacheZip});
}

void SplashController::onAcquireStep() {
    if (acquire_->exitCode() == 0 && QFileInfo::exists(o_.cacheZip)) {
        extractZip();
        return;
    }
    ++pullCandidate_;
    tryAcquire();
}

void SplashController::extractZip() {
    if (!extract_.isValid()) return;
    // Same tool the reference splash forks; the zip is Android's own, always unzip-friendly.
    auto *unzip = new QProcess(this);
    connect(unzip, &QProcess::finished, this, [this, unzip](int rc, QProcess::ExitStatus) {
        unzip->deleteLater();
        if (rc != 0) return;
        // Android zips carry read-only entries, which block QTemporaryDir's removal at exit —
        // make the extracted tree deletable before anything else happens.
        QProcess::execute(QStringLiteral("chmod"),
                          {QStringLiteral("-R"), QStringLiteral("u+w"), extract_.path()});
        loadDesc();
    });
    unzip->start(QStringLiteral("unzip"),
                 {QStringLiteral("-oq"), o_.cacheZip, QStringLiteral("-d"), extract_.path()});
}

void SplashController::loadDesc() {
    QFile f(extract_.path() + QLatin1String("/desc.txt"));
    if (!f.open(QIODevice::ReadOnly)) return;
    desc_ = parseBootAnimDesc(QString::fromUtf8(f.readAll()));
    if (!desc_.valid()) return;
    partFrames_.clear();
    for (const BootAnimPart &p : desc_.parts) {
        QDir dir(extract_.path() + QLatin1Char('/') + p.dir);
        QStringList frames = dir.entryList({QStringLiteral("*.png"), QStringLiteral("*.jpg")},
                                           QDir::Files, QDir::Name);
        for (QString &fr : frames) fr = dir.absoluteFilePath(fr);
        partFrames_ << frames;
    }
    animReady_ = true;
    const double fps = qBound(1.0, desc_.fps, 120.0);
    frameTimer_.setInterval(int(1000.0 / fps));
    if (bootingSeen_ || o_.statusPath.isEmpty()) frameTimer_.start();
}

// ---- playback ----

void SplashController::advanceFrame() {
    if (part_ >= partFrames_.size()) return;
    if (pauseLeft_ > 0) {
        --pauseLeft_;
        return;
    }
    const QStringList &frames = partFrames_[part_];
    if (frames.isEmpty()) {
        part_ = (part_ + 1) % partFrames_.size();
        return;
    }
    if (feedingPart_ != part_) {
        feedingPart_ = part_;
        prefetch_.setFrames(frames);
        if (!prefetch_.isRunning()) prefetch_.start();
        // Start decoding the part AFTER this one straight away. The playing part keeps the
        // decoder's attention while its queue has room, so this costs nothing until the pipeline
        // is full — and by the time the switch comes (1.5 s of looping, for the A17 animation)
        // the ending's first frames are already waiting.
        if (part_ + 1 < partFrames_.size()) prefetch_.setNextFrames(partFrames_[part_ + 1]);
    }
    // A null take() means the decoder has not caught up: hold the previous frame for this tick
    // rather than block. One repeated frame is invisible; a stalled GUI thread is not. idx_ must
    // NOT advance here — it counts frames CONSUMED, and advancing on a miss would walk it out of
    // step with the decoder's own position and cut the part short.
    const QImage img = prefetch_.take();
    if (!qEnvironmentVariableIsEmpty("REMORA_MIRROR_FPS")) {
        static qint64 win = 0; static int shown = 0, missed = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!win) win = now;
        img.isNull() ? ++missed : ++shown;
        if (now - win >= 1000) {
            qInfo("splash: part %d, %d frames shown, %d decoder misses", part_, shown, missed);
            shown = missed = 0; win = now;
        }
    }
    if (img.isNull()) return;
    emit frame(img);
    if (++idx_ < frames.size()) return;

    idx_ = 0;
    pauseLeft_ = desc_.parts[part_].pause;
    ++plays_;
    switch (bootAnimPartStep(desc_.parts[part_], plays_, part_ + 1 >= partFrames_.size(),
                             finishing_)) {
        case PartStep::Repeat:
            break;
        case PartStep::Advance:
            plays_ = 0;
            ++part_;
            break;
        case PartStep::Finished:
            frameTimer_.stop();  // hold the ending's last frame until live video replaces it
            finishAnimation();
            break;
    }
}

void SplashController::stopPlayback() {
    frameTimer_.stop();
    endingCap_.stop();
    prefetch_.stop();  // nothing should decode frames nobody will see
}

// Fires exactly once, whichever way the animation ends: the ending played out, the cap expired,
// or there was no animation to play at all. The window is waiting on this, so a path that can
// skip it would strand the user on a splash.
void SplashController::finishAnimation() {
    if (animationDoneSent_) return;
    animationDoneSent_ = true;
    endingCap_.stop();
    emit animationDone();
}

// ---- hand-off ----

void SplashController::maybeHandOff() {
    if (handedOff_ || aborted_ || !ready_) return;
    // `ready` is necessary but NOT sufficient (fork boot_anim.c, bd remora-82x): with a status
    // file, only the engine's REMORA_ATTACH — written when adb is verified and the chain is
    // about to adopt — permits hand-off. Ready alone would adopt at the FIRST boot_completed,
    // under which the play-spoof stop/start then collapses the session. Manual launches
    // (no status file) keep ready-alone semantics.
    if (!o_.statusPath.isEmpty() && !attachSeen_) return;
    handedOff_ = true;
    // Boot is done, so let the animation wind up: the repeat-forever part stops at the end of its
    // current loop and the ending part plays. This costs the session nothing — hand-off starts
    // the mirror NOW, in parallel, and the animation keeps painting until the first live frame
    // lands, so the ending fills exactly the gap that used to show a looping middle part.
    finishing_ = true;
    // The window holds live video until animationDone(), so the ending is actually seen rather
    // than preempted by the first frame. Cap it: an animation with no ending part, a zip that
    // failed to load, or a part list that never reaches PartStep::Finished must not be able to
    // keep the mirror hidden. Generous enough for a real ending (the A17 one is 3.5 s).
    // Hold video ONLY when an animation is actually on screen to play out. A reconnect writes
    // REMORA_ATTACH with no REMORA_BOOTING, so playback never starts (loadDesc only starts the
    // timer once booting is seen) — and holding frames for an animation that is not running
    // showed a black window while the decoder burned CPU on frames nobody saw.
    if (!animReady_ || partFrames_.isEmpty() || !frameTimer_.isActive()) {
        finishAnimation();  // nothing is playing: live video takes the window immediately
    } else {
        // Something IS on screen and will play out its ending. Holding video for it is only safe
        // because decoding now happens off this thread; the cap is the backstop either way.
        endingCap_.start();
    }
    emit handOff();
}

}  // namespace remora::mirror
