#pragma once
#include <QImage>
#include <QObject>
#include <QProcess>
#include <QSize>
#include <QMutex>
#include <QQueue>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include "core/SplashFormats.h"

namespace remora::mirror {

struct SplashOptions {
    QString serial;
    QString cacheZip;     // --boot-animation-cache: pull lands here; a hit skips the pull
    QString statusPath;   // --boot-animation-status: the engine's append-only status file
};

// The boot-splash phase machine (in-process, so there is no window adoption — the mirror
// simply takes over the same widget). Runs three concerns on the event loop: the status-file
// tail (300 ms), the boot probe (2 s, getprop + screencap-lit check, async so animation never
// stalls), and frame playback paced to desc.txt. Hand-off fires once, on REMORA_ATTACH or on the probe's own booted+lit signal;
// the animation keeps painting until the first live frame replaces it.
// Decodes animation PNGs OFF the GUI thread into a small ring. Playback used to decode inline on
// every 16 ms tick, which on a loaded machine starved the event loop badly enough that an 8 s
// timer took 28.7 s to fire and the mirror looked hung (bd remora-28ix.2). A bounded queue keeps
// the memory cost trivial — the ending alone would be 326 MiB if fully preloaded.
class FramePrefetcher : public QThread {
    Q_OBJECT
public:
    // Switch to this part's frames. Adopts whatever setNextFrames() already decoded for it, so a
    // part change does not start from an empty queue.
    void setFrames(const QStringList &paths);
    // The part that comes AFTER the current one: decode a few of its frames while the current
    // part still plays. Without it every part change cost one dropped frame — the queue was
    // cleared and the very next tick had nothing to show — which landed exactly on the switch
    // into the ending and was the last thing still marking it (bd remora-xrlh).
    void setNextFrames(const QStringList &paths);
    // Next decoded frame, or a null image if the decoder has not caught up. Never blocks the
    // caller: a missed tick is one repeated frame, a blocked GUI thread is a hung mirror.
    QImage take();
    void stop();

protected:
    void run() override;

private:
    static constexpr int kQueueDepth = 8;
    // Only a handful: this buys the switch, not a second full pipeline, and every frame is a
    // decoded 1080x360 RGBA (1.5 MiB) that sits idle until the part changes.
    static constexpr int kPreloadDepth = 4;
    QMutex m_;
    QWaitCondition wake_;  // ONE condition: run() waits on it, setFrames/take/stop wake it
    QStringList paths_, nextPaths_;
    QQueue<QImage> ready_, nextReady_;
    int next_ = 0, nextIdx_ = 0;
    // A decode runs with the mutex released, so a part change can land mid-decode. The
    // generations say which list a finished frame was for: one that no longer matches is dropped
    // instead of being enqueued into the new part's queue, where it would show as a single stale
    // frame of the PREVIOUS part immediately after the switch.
    int gen_ = 0, nextGen_ = 0;
    int nextInFlight_ = 0;  // 0 or 1 — one decode at a time; see the promotion in setFrames
    bool stop_ = false;
};

class SplashController : public QObject {
    Q_OBJECT
public:
    SplashController(SplashOptions opts, QObject *parent = nullptr);
    ~SplashController() override;
    void begin();
    // Live video owns the window now: stop decoding animation frames. Cheap to call twice.
    void stopPlayback();

signals:
    void frame(const QImage &frame);
    void statusText(const QString &text);
    void handOff();
    // The animation is over — live video may take the window. Always fires after handOff(),
    // either when the ending finishes or when the cap below expires, so the window can never be
    // stranded on a splash by an animation that does not end.
    void animationDone();
    void abortRequested();

private:
    void pollStatus();
    void probeTick();
    void onProbeFinished();
    void tryAcquire();
    void onAcquireStep();
    void extractZip();
    void loadDesc();
    void advanceFrame();
    void maybeHandOff();

    SplashOptions o_;
    QTimer statusTimer_, probeTimer_, frameTimer_;
    FramePrefetcher prefetch_;
    QTemporaryDir extract_;
    BootAnimDesc desc_;
    QList<QStringList> partFrames_;  // absolute frame paths per part
    int part_ = 0, idx_ = 0, plays_ = 0, pauseLeft_ = 0;
    int feedingPart_ = -1;  // which part the prefetcher is currently decoding
    int pullCandidate_ = 0;
    bool animReady_ = false, bootingSeen_ = false, attachSeen_ = false;
    bool handedOff_ = false, aborted_ = false;
    // Boot is done, so the animation should wind up rather than keep looping: a repeat-forever
    // part ends at its current loop and playback moves on to the ending part. Android's own
    // bootanimation does exactly this when the system asks it to exit.
    bool finishing_ = false;
    bool animationDoneSent_ = false;
    QTimer endingCap_;  // upper bound on how long the ending may hold the window
    void finishAnimation();
    bool booted_ = false, ready_ = false;
    qint64 bootedAtMs_ = 0;  // lit-check grace anchor: 60 s after boot_completed, go anyway
    QString lastText_;
    QProcess *probe_ = nullptr;
    QProcess *acquire_ = nullptr;
};

}  // namespace remora::mirror
