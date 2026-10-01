#pragma once
#include <QImage>
#include <QOpenGLFunctions>
#include <QElapsedTimer>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QTimer>
#include <optional>

#include "CudaInterop.h"
#include "MirrorSession.h"

namespace remora::mirror {

// The mirror surface: uploads decoded frames to a GPU texture and lets the GPU scale them to the
// widget (stretched to fill), and translates Qt input into control messages.
// Window sizing follows the frame's aspect on first frame; afterwards the desktop owns the frame.
//
// GPU rather than QPainter: drawImage() with SmoothPixmapTransform resamples on the CPU, on the
// GUI thread, for every frame — 7.5 MP per frame at the daily-driver geometry, which made the
// mirror measurably slower (bd remora-28ix.2.1).
class MirrorWindow : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    MirrorWindow(MirrorSession *session, const QString &title, QString shotPath = {});
    ~MirrorWindow() override;

    // Splash mode: show immediately at a fixed size (status text, then boot animation), and
    // keep that geometry when the live mirror takes over — the adopted window keeps its
    // geometry through the hand-off.
    void showSplash(QSize initialSize);
    void setSplashFrame(const QImage &frame);
    // The animation reached its end (or gave up) before any live frame arrived: drop the splash
    // so the window is not left holding a finished animation.
    void endSplash();

    void setStatusText(const QString &text);
    void toggleDisplayMode();  // Alt+D; public for the headless debug hook

    // Window-mode plumbing (PIP, desktop, app windows), applied at first show. An explicit
    // size suppresses the 1:1 auto-sizing; position is best-effort (Wayland compositors own
    // placement — the KWin rules and scripts remain the authority there).
    void setExplicitGeometry(std::optional<QSize> size, std::optional<QPoint> pos);
    void setAlwaysOnTop(bool onTop);
    void setStartFullscreen(bool fs);
    void setShotDelayMs(int ms) { shotDelayMs_ = ms; }  // debug: let content draw before --shot

signals:
    // The first live frame has taken the window. Playback must stop here: decoding a PNG per
    // frame for an animation nobody can see is pure event-loop starvation, and it used to
    // continue for the entire life of the mirror.
    void liveVideoStarted();

protected:
    void initializeGL() override;
    void paintGL() override;
    void uploadPlane(const unsigned char *data, int stride, int w, int h, unsigned fmt,
                     bool realloc, QOpenGLBuffer *pbo);
    // Zero-copy decode-to-texture (bd remora-28ix.2.3): copy a decoded CUDA frame into the plane
    // textures device-to-device. False = interop refused; the caller downloads instead.
    bool uploadCudaFrame(AVFrame *f, bool resized);
    // Interop is off for good: warn loudly, and tell the decoder to go back to downloading.
    void interopFailed(const QString &why);
    // Fallback for CUDA frames already in flight when interop dies: hw → dlFrame_ here, then the
    // normal upload path takes it. False = the frame is lost; keep drawing the last texture.
    bool downloadFrame(AVFrame *f);
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void onFrame(const VideoFrame &frame);
    void requestDisplayReflow();
    void sendTouch(quint8 action, QPointF pos, float pressure, qint32 actionButton,
                   qint32 buttons);
    void sendKey(QKeyEvent *e, quint8 action);
    void sendBindAction(BindAction act, quint8 action);
    std::optional<BindAction> boundAction(QKeyEvent *e) const;
    FramePoint positionAt(QPointF widgetPos) const;

    MirrorSession *session_;
    QImage frame_;
    QString shotPath_;  // debug: save the first frame here and exit (offscreen verification)
    int shotDelayMs_ = 0;
    qint64 firstFrameAtMs_ = 0;
    QString statusText_;
    std::optional<QSize> explicitSize_;
    std::optional<QPoint> explicitPos_;
    bool startFullscreen_ = false;
    bool splash_ = false;  // splash frames paint until the animation ends (see endSplash)
    // A live frame that arrived while the ending was still playing, held so the ending is not
    // cut off. The mirror connects in a few hundred ms and the ending runs seconds, so without
    // this the ending is always preempted almost immediately.
    VideoFrame pendingLive_;  // arrived while the ending was still playing
    bool splashDone_ = false;
    VideoFrame live_;      // the frame on screen, referenced not copied
    QImage splashFrame_;   // boot animation only — PNGs, painted with QPainter
    bool sizedToVideo_ = false;
    // The frame currently on the GPU. Uploaded only when a new frame arrives, so a repaint that
    // is not driven by a frame (resize, expose) costs nothing but a redraw of the same texture.
    GLuint texY_ = 0, texUV_ = 0;   // NV12: luma plane + interleaved chroma
    QSize textureSize_;
    bool frameDirty_ = false;
    bool rowLengthSupported_ = false;
    // Streaming uploads: the frame is copied into a mapped buffer and the texture is filled FROM
    // that buffer, so the transfer becomes an async DMA instead of a call that blocks until the
    // driver has consumed client memory. Two buffers per plane, alternating, so frame N uploads
    // while frame N-1 is still being drawn.
    static constexpr int kPboCount = 2;
    // Type is fixed at construction — QOpenGLBuffer has no setType() — so these are held by
    // value and constructed in the member init list.
    QOpenGLBuffer pboY_[kPboCount];
    QOpenGLBuffer pboUV_[kPboCount];
    bool pboReady_ = false;
    int pboIndex_ = 0;
    // Zero-copy decode-to-texture: decoded CUDA frames land in texY_/texUV_ by device-to-device
    // copy — the ~32 ms/frame PCIe download that capped the window at ~30 fps never happens.
    // REMORA_MIRROR_NO_INTEROP=1 keeps the download path, as the A/B baseline in one binary.
    CudaInterop interop_;
    bool interopOn_ = false;          // this GL context + libcuda are both willing
    bool interopRegistered_ = false;  // textures registered at textureSize_
    AVFrame *dlFrame_ = nullptr;      // download target for frames stranded by an interop failure
    QOpenGLShaderProgram program_;
    qint64 fpsWindowMs_ = 0;
    int fpsFrames_ = 0;
    QElapsedTimer uploadTimer_;
    qint64 uploadUsTotal_ = 0;
    int uploadCount_ = 0;
    bool touchDown_ = false;
    qint32 buttons_ = 0;  // MotionEvent.BUTTON_* currently held, primary included
    // The action each bindable button STARTED with, so a release finishes what its press began
    // even if Shift changed in between.
    std::array<BindAction, kSlotCount> held_{};
    // Same idea for the keyboard: Qt key → the action its press started, so the release ends that
    // action and is swallowed instead of reaching the device.
    QHash<int, BindAction> heldKeys_;
    // Flex reflow (bd remora-28ix.3.3): a resize settles for a beat, then one RESIZE_DISPLAY
    // asks the device to reflow the display to the window — see resizeEvent().
    QTimer reflowTimer_;
    QSize reflowSent_;  // last size requested, so a settled drag sends once
};

}  // namespace remora::mirror
