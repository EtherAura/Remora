#include "MirrorWindow.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QOpenGLContext>
#include <QScreen>
#include <QWheelEvent>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "core/MirrorInput.h"

namespace remora::mirror {

MirrorWindow::MirrorWindow(MirrorSession *session, const QString &title, QString shotPath)
    : session_(session), shotPath_(std::move(shotPath)),
      pboY_{QOpenGLBuffer(QOpenGLBuffer::PixelUnpackBuffer),
            QOpenGLBuffer(QOpenGLBuffer::PixelUnpackBuffer)},
      pboUV_{QOpenGLBuffer(QOpenGLBuffer::PixelUnpackBuffer),
             QOpenGLBuffer(QOpenGLBuffer::PixelUnpackBuffer)} {
    setWindowTitle(title);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);  // moves only while a button is down — hover is not touch
    connect(session_, &MirrorSession::frameReady, this, &MirrorWindow::onFrame);
    connect(session_, &MirrorSession::deviceClipboard, this,
            [](const QString &text) { QGuiApplication::clipboard()->setText(text); });
    reflowTimer_.setSingleShot(true);
    reflowTimer_.setInterval(300);  // a drag settles before the display reflows under it
    connect(&reflowTimer_, &QTimer::timeout, this, &MirrorWindow::requestDisplayReflow);
}

MirrorWindow::~MirrorWindow() {
    if (dlFrame_) av_frame_free(&dlFrame_);
}

void MirrorWindow::setExplicitGeometry(std::optional<QSize> size, std::optional<QPoint> pos) {
    explicitSize_ = size;
    explicitPos_ = pos;
}

void MirrorWindow::setAlwaysOnTop(bool onTop) { setWindowFlag(Qt::WindowStaysOnTopHint, onTop); }

void MirrorWindow::setStartFullscreen(bool fs) { startFullscreen_ = fs; }

void MirrorWindow::showSplash(QSize initialSize) {
    splash_ = true;
    sizedToVideo_ = true;  // the adopted window keeps its geometry — no live-frame resize
    resize(initialSize.isEmpty() ? QSize(1024, 576) : initialSize);
    show();
}

void MirrorWindow::setSplashFrame(const QImage &frame) {
    if (!splash_) return;  // a straggling animation tick must not repaint over live video
    splashFrame_ = frame;
    update();
}

void MirrorWindow::setStatusText(const QString &text) {
    statusText_ = text;
    update();
}

void MirrorWindow::endSplash() {
    splashDone_ = true;
    if (!splash_) return;
    splash_ = false;
    statusText_.clear();
    emit liveVideoStarted();
    if (!pendingLive_.isNull()) {
        const VideoFrame held = pendingLive_;
        pendingLive_ = VideoFrame{};
        onFrame(held);  // the frame that waited on the ending takes the normal path
        return;
    }
    update();
}

void MirrorWindow::resizeEvent(QResizeEvent *e) {
    QOpenGLWidget::resizeEvent(e);
    // Flex reflow (bd remora-28ix.3.3): on a new display the window IS the display, so a resized
    // window asks the device to reflow content to the new size instead of letting paintGL()
    // stretch the old pixels. Debounced so a drag becomes one request once it settles.
    if (!session_->reflowsOnResize() || !sizedToVideo_ || splash_) return;
    reflowTimer_.start();
}

void MirrorWindow::requestDisplayReflow() {
    // PHYSICAL pixels, matching the 1:1 contract the initial sizing establishes (logical =
    // frame / dpr). Comparing against the stream size drops the programmatic resizes that
    // already mirror it; comparing against the last request keeps a settled drag to one send.
    const QSize phys{qMin(int(width() * devicePixelRatioF()), 0xFFFF),
                     qMin(int(height() * devicePixelRatioF()), 0xFFFF)};
    if (phys == live_.size || phys == reflowSent_ || phys.isEmpty()) return;
    reflowSent_ = phys;
    session_->sendControl(resizeDisplay(quint16(phys.width()), quint16(phys.height())));
}

void MirrorWindow::onFrame(const VideoFrame &frame) {
    if (splash_) {
        // Live video ALWAYS wins the window the moment it exists. Holding frames so the ending
        // could play out cost responsiveness far more than the ending was worth: playback decodes
        // a PNG per frame on this thread, so on a loaded machine the animation starves the event
        // loop and a held mirror stayed blank for tens of seconds. The ending still plays — it
        // just plays in the gap before the first frame arrives, which is where it belongs.
        // The animation gets to finish its ending: the splash signals animationDone() when the
        // last part completes or its cap expires, and endSplash() hands the window over then.
        // Decoding runs off the GUI thread now, so holding here cannot starve the event loop —
        // which is what made an earlier attempt at this look like a hung mirror.
        if (!splashDone_) {
            pendingLive_ = frame;
            return;
        }
        splash_ = false;
        statusText_.clear();
        emit liveVideoStarted();  // stops playback: nothing should decode frames nobody sees
    }
    live_ = frame;
    frameDirty_ = true;
    if (!sizedToVideo_) {
        sizedToVideo_ = true;
        if (explicitSize_) {
            resize(*explicitSize_);  // PIP / profile geometry: the caller's size wins
        } else {
            // Size once so PHYSICAL pixels match the stream 1:1 (logical = frame / dpr) — sizing
            // by frame pixels directly painted every stream pixel across dpr² physical ones on
            // scaled outputs, which is the blur the first validation round reported. Capped into
            // the work area; after this the desktop owns the frame.
            const qreal dpr = devicePixelRatioF();
            QSize target = frame.size / dpr;
            if (QScreen *s = screen())
                target = target.boundedTo(s->availableSize() * 0.85);
            QSize aspect = frame.size;
            aspect.scale(target, Qt::KeepAspectRatio);
            resize(aspect);
        }
        if (explicitPos_) move(*explicitPos_);
        startFullscreen_ ? showFullScreen() : show();
    }
    update();
    if (!shotPath_.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!firstFrameAtMs_) firstFrameAtMs_ = now;
        if (now - firstFrameAtMs_ < shotDelayMs_) return;  // let the content draw first
        live_.toImage().save(shotPath_);  // convert only here, never on the render path
        shotPath_.clear();
        QCoreApplication::exit(0);
    }
}

static void makePlaneTexture(QOpenGLFunctions *f, GLuint tex) {
    f->glBindTexture(GL_TEXTURE_2D, tex);
    // Linear filtering IS the scale — what QPainter did per pixel on the CPU is a sampler state.
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void MirrorWindow::initializeGL() {
    initializeOpenGLFunctions();
    glGenTextures(1, &texY_);
    glGenTextures(1, &texUV_);
    makePlaneTexture(this, texY_);
    makePlaneTexture(this, texUV_);

    // GL2/ES2 common subset — what QOpenGLFunctions guarantees, so this runs on a core profile,
    // on GLES, and under Venus alike.
    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, R"(
        attribute vec2 pos;
        attribute vec2 uv;
        varying vec2 vUv;
        void main() { vUv = uv; gl_Position = vec4(pos, 0.0, 1.0); }
    )");
    // NV12 -> RGB on the GPU, BT.601 limited range: the same conversion swscale was doing on the
    // CPU at 7.4 ms/frame. LUMINANCE/LUMINANCE_ALPHA are the ES2-portable single- and two-channel
    // formats, which is why the chroma sample reads .ra rather than .rg.
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, R"(
        #ifdef GL_ES
        precision mediump float;
        #endif
        varying vec2 vUv;
        uniform sampler2D texY;
        uniform sampler2D texUV;
        void main() {
            float y = texture2D(texY, vUv).r;
            vec2 uv = texture2D(texUV, vUv).ra - vec2(0.5, 0.5);
            y = (y - 0.0625) * 1.164382;
            gl_FragColor = vec4(y + 1.596027 * uv.y,
                                y - 0.391762 * uv.x - 0.812968 * uv.y,
                                y + 2.017232 * uv.x, 1.0);
        }
    )");
    program_.bindAttributeLocation("pos", 0);
    program_.bindAttributeLocation("uv", 1);
    if (!program_.link()) qWarning("mirror: shader link failed: %s", qUtf8Printable(program_.log()));

    // Desktop GL and ES3 have it in core; ES2 needs the extension. Without it every padded plane
    // uploads a row at a time, which is thousands of driver round trips per frame.
    const auto *ctx = QOpenGLContext::currentContext();
    rowLengthSupported_ = ctx && (!ctx->isOpenGLES() || ctx->format().majorVersion() >= 3 ||
                                  ctx->hasExtension(QByteArrayLiteral("GL_EXT_unpack_subimage")));
    // REMORA_MIRROR_NO_ROWLEN=1 forces the row-at-a-time path, so the two can be compared in one
    // binary on one workload instead of across builds.
    if (!qEnvironmentVariableIsEmpty("REMORA_MIRROR_NO_ROWLEN")) rowLengthSupported_ = false;
    if (!qEnvironmentVariableIsEmpty("REMORA_MIRROR_FPS"))
        qInfo("mirror: UNPACK_ROW_LENGTH %s", rowLengthSupported_ ? "supported (1 call/plane)"
                                                                  : "UNSUPPORTED (row-by-row)");

    // Pixel-unpack buffers need GL 2.1 / ES3; without them the plain path is still correct.
    // REMORA_MIRROR_NO_PBO=1 forces it off so the two can be compared in one binary.
    const bool pboAllowed = ctx && (!ctx->isOpenGLES() || ctx->format().majorVersion() >= 3) &&
                            qEnvironmentVariableIsEmpty("REMORA_MIRROR_NO_PBO");
    if (pboAllowed) {
        pboReady_ = true;
        for (int i = 0; i < kPboCount && pboReady_; ++i) {
            for (QOpenGLBuffer *b : {&pboY_[i], &pboUV_[i]}) {
                if (!b->create()) pboReady_ = false;
                else b->setUsagePattern(QOpenGLBuffer::StreamDraw);
            }
        }
    }
    if (!qEnvironmentVariableIsEmpty("REMORA_MIRROR_FPS"))
        qInfo("mirror: PBO streaming %s", pboReady_ ? "on (async upload)" : "off (blocking upload)");

    // Zero-copy decode-to-texture (bd remora-28ix.2.3): if this context can take CUDA frames
    // directly, tell the decoder to stop downloading them — the ~32 ms/frame PCIe round trip
    // was the whole gap between ~30 fps drawn and the 58-60 the stream delivers. ES contexts
    // are out (cuGraphicsGLRegisterImage is desktop-GL only); a per-texture registration failure
    // later (cross-GPU split) falls back at the first frame. Arming is a no-op unless the
    // decoder's hw device is CUDA. REMORA_MIRROR_NO_INTEROP=1 keeps the download path — the
    // A/B baseline in one binary.
    const char *gate = !ctx ? "no GL context"
                       : ctx->isOpenGLES() ? "GLES context"
                       : !CudaInterop::available() ? "libcuda unavailable"
                       : !qEnvironmentVariableIsEmpty("REMORA_MIRROR_NO_INTEROP")
                           ? "REMORA_MIRROR_NO_INTEROP"
                           : nullptr;
    interopOn_ = !gate;
    if (interopOn_) session_->setKeepHwFrames(true);
    if (!qEnvironmentVariableIsEmpty("REMORA_MIRROR_FPS"))
        qInfo("mirror: CUDA/GL zero-copy %s%s%s",
              interopOn_ ? "armed (frames stay on the GPU)" : "off (", gate ? gate : "",
              gate ? ")" : "");
}

void MirrorWindow::paintGL() {
    // REMORA_MIRROR_FPS=1 reports what the WINDOW actually draws, once a second. Stream-side
    // measurements cannot see this, and an offscreen run never paints at all — which is how a
    // rendering regression hid behind a healthy stream for a whole session (bd remora-28ix.2.1).
    static const bool fpsLog = !qEnvironmentVariableIsEmpty("REMORA_MIRROR_FPS");
    if (fpsLog) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!fpsWindowMs_) fpsWindowMs_ = now;
        ++fpsFrames_;
        if (now - fpsWindowMs_ >= 1000) {
            qInfo("mirror: %.1f fps drawn (%dx%d -> %dx%d), upload %.1f ms/frame",
                  fpsFrames_ * 1000.0 / double(now - fpsWindowMs_), textureSize_.width(),
                  textureSize_.height(), width(), height(),
                  uploadCount_ ? double(uploadUsTotal_) / double(uploadCount_) / 1000.0 : 0.0);
            uploadUsTotal_ = 0; uploadCount_ = 0;
            fpsFrames_ = 0;
            fpsWindowMs_ = now;
        }
    }

    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    // The splash is PNG frames, not decoded video: it keeps the QPainter path, which costs
    // nothing at animation rates.
    if (splash_) {
        if (!splashFrame_.isNull()) {
            QPainter p(this);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            QSizeF fit = splashFrame_.size();
            fit.scale(size(), Qt::KeepAspectRatio);
            p.drawImage(QRectF(QPointF((width() - fit.width()) / 2, (height() - fit.height()) / 2),
                               fit),
                        splashFrame_);
            if (!statusText_.isEmpty()) {
                const QRect bar(0, height() - 44, width(), 44);
                p.fillRect(bar, QColor(0, 0, 0, 160));
                p.setPen(Qt::white);
                p.drawText(bar, Qt::AlignCenter, statusText_);
            }
        }
        return;
    }

    if (live_.isNull() || !program_.isLinked()) return;
    AVFrame *f = live_.av.get();
    const bool cudaFrame = f->format == AV_PIX_FMT_CUDA;
    if (f->format != AV_PIX_FMT_NV12 && !cudaFrame) {
        // Only NV12 has a shader path; anything else falls back rather than showing nothing.
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(rect(), live_.toImage());
        return;
    }

    if (frameDirty_) {
        frameDirty_ = false;
        if (fpsLog) uploadTimer_.start();
        const bool resized = live_.size != textureSize_;
        textureSize_ = live_.size;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // A CUDA frame goes texture-to-texture on the GPU; failure downloads it here so the
        // frames already in flight when interop dies still draw (interopFailed() has told the
        // decoder to resume downloading, so this in-place transfer is transitional).
        AVFrame *src = f;
        if (cudaFrame) src = uploadCudaFrame(f, resized) ? nullptr
                                                         : (downloadFrame(f) ? dlFrame_ : nullptr);
        if (src) {
            // Y: one byte per pixel. UV: interleaved, half resolution in both axes, so two bytes
            // per sample — 1.5 bytes/pixel total against BGRA's 4, which is most of the win.
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texY_);
            uploadPlane(src->data[0], src->linesize[0], textureSize_.width(),
                        textureSize_.height(), GL_LUMINANCE, resized,
                        pboReady_ ? &pboY_[pboIndex_] : nullptr);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texUV_);
            uploadPlane(src->data[1], src->linesize[1], textureSize_.width() / 2,
                        textureSize_.height() / 2, GL_LUMINANCE_ALPHA, resized,
                        pboReady_ ? &pboUV_[pboIndex_] : nullptr);
            pboIndex_ = (pboIndex_ + 1) % kPboCount;  // next frame writes the other buffer
        }
        if (fpsLog) {
            // Deliberately NOT glFinish: this measures how long the GUI THREAD is held, which is
            // what a streaming upload is meant to shorten. Forcing completion here would erase
            // exactly the asynchrony being measured.
            uploadUsTotal_ += uploadTimer_.nsecsElapsed() / 1000;
            ++uploadCount_;
        }
    }

    const GLfloat verts[] = {-1.f, 1.f, 1.f, 1.f, -1.f, -1.f, 1.f, -1.f};
    const GLfloat uvs[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    program_.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY_);
    program_.setUniformValue("texY", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texUV_);
    program_.setUniformValue("texUV", 1);
    program_.enableAttributeArray(0);
    program_.enableAttributeArray(1);
    program_.setAttributeArray(0, verts, 2);
    program_.setAttributeArray(1, uvs, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray(0);
    program_.disableAttributeArray(1);
    program_.release();
}

// One plane into the bound texture.
//
// Decoders pad linesize — measured stride 3776 for a 3760-wide plane — so the "stride equals
// width" case rarely fires. GL_UNPACK_ROW_LENGTH lets the padded block go up in ONE call; without
// it the fallback is a glTexSubImage2D per row, which measured 13 ms/frame against 2.5 (~2988
// driver round trips per frame).
//
// With a pixel-unpack buffer bound the texture is filled FROM that buffer, so the transfer is an
// async DMA rather than a call that blocks until the driver has consumed client memory. The cost
// is one memcpy into the mapped buffer; the gain is that it no longer stalls the GUI thread.
void MirrorWindow::uploadPlane(const unsigned char *data, int stride, int w, int h, unsigned fmt,
                               bool realloc, QOpenGLBuffer *pbo) {
    constexpr GLenum kUnpackRowLength = 0x0CF2;  // absent from the ES2 subset
    if (realloc) glTexImage2D(GL_TEXTURE_2D, 0, GLint(fmt), w, h, 0, fmt, GL_UNSIGNED_BYTE, nullptr);
    const int bpp = fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
    const bool padded = stride != w * bpp;

    if (padded && !rowLengthSupported_) {
        for (int y = 0; y < h; ++y)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1, fmt, GL_UNSIGNED_BYTE,
                            data + qsizetype(y) * stride);
        return;
    }
    if (padded) glPixelStorei(kUnpackRowLength, stride / bpp);

    if (pbo && pbo->isCreated() && pbo->bind()) {
        const int bytes = stride * h;
        // Orphan first: allocating over the old contents lets the driver hand back fresh storage
        // instead of waiting for the previous upload to drain.
        pbo->allocate(bytes);
        if (void *dst = pbo->map(QOpenGLBuffer::WriteOnly)) {
            memcpy(dst, data, size_t(bytes));
            pbo->unmap();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, nullptr);
        } else {
            pbo->release();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, data);
            if (padded) glPixelStorei(kUnpackRowLength, 0);
            return;
        }
        pbo->release();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, data);
    }
    if (padded) glPixelStorei(kUnpackRowLength, 0);
}

// The zero-copy path: the decoded frame and the textures are both NVIDIA-side, so the "upload"
// is a device-to-device copy — no PCIe crossing in either direction. Registration binds to the
// texture's current storage, so a resize (new glTexImage2D) forces a re-register.
bool MirrorWindow::uploadCudaFrame(AVFrame *f, bool resized) {
    if (!interopOn_) return false;
    if (resized || !interopRegistered_) {
        interopRegistered_ = false;
        const int w = textureSize_.width(), h = textureSize_.height();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texY_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                     nullptr);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texUV_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w / 2, h / 2, 0, GL_LUMINANCE_ALPHA,
                     GL_UNSIGNED_BYTE, nullptr);
        QString why;
        if (!interop_.registerTextures(texY_, texUV_, session_->decoderCudaContext(), &why)) {
            interopFailed(why);
            return false;
        }
        interopRegistered_ = true;
    }
    QString why;
    if (!interop_.upload(f, &why)) {
        interopFailed(why);
        return false;
    }
    return true;
}

void MirrorWindow::interopFailed(const QString &why) {
    // LOUD and permanent, like the decoder's mid-stream download failure: past this point every
    // frame pays the PCIe download again, and a silent fallback would read as "the mirror got
    // slow for no reason".
    qWarning("mirror: CUDA/GL zero-copy failed (%s) — falling back to the download path",
             qUtf8Printable(why));
    interopOn_ = false;
    interopRegistered_ = false;
    interop_.unregister();
    session_->setKeepHwFrames(false);
}

bool MirrorWindow::downloadFrame(AVFrame *f) {
    if (!dlFrame_) dlFrame_ = av_frame_alloc();
    if (!dlFrame_) return false;
    av_frame_unref(dlFrame_);
    if (av_hwframe_transfer_data(dlFrame_, f, 0) < 0) {
        qWarning("mirror: hw frame download failed after interop fallback — frame dropped");
        return false;
    }
    return dlFrame_->format == AV_PIX_FMT_NV12;
}

Position MirrorWindow::positionAt(QPointF widgetPos) const {
    const QSize video = session_->videoSize().isEmpty() ? frame_.size() : session_->videoSize();
    const QPoint mapped = mapToVideo(widgetPos.toPoint(), size(), video);
    return {mapped.x(), mapped.y(), quint16(video.width()), quint16(video.height())};
}

void MirrorWindow::sendTouch(quint8 action, QPointF pos, float pressure, qint32 actionButton,
                             qint32 buttons) {
    session_->sendControl(injectTouch(action, kPointerIdMouse, positionAt(pos), pressure,
                                      actionButton, buttons));
}

// Which mouse_bind slot a Qt button occupies, or -1 for one the vector has no row for. Left is
// deliberately absent: it is the touch pointer, and a rebindable tap would leave no way to tap.
static int slotFor(Qt::MouseButton b) {
    switch (b) {
        case Qt::RightButton: return kSlotRight;
        case Qt::MiddleButton: return kSlotMiddle;
        case Qt::BackButton: return kSlotSide4;
        case Qt::ForwardButton: return kSlotSide5;
        default: return -1;
    }
}

void MirrorWindow::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        touchDown_ = true;
        buttons_ |= kButtonPrimary;
        sendTouch(kMotionActionDown, e->position(), 1.0f, kButtonPrimary, buttons_);
        return;
    }
    const int slot = slotFor(e->button());
    if (slot < 0) return;
    // Resolved at PRESS and remembered: releasing after letting Shift go must finish the action
    // the press started, not the other half of the vector.
    const BindAction act = session_->mouseBindings().at(slot, e->modifiers() & Qt::ShiftModifier);
    held_[size_t(slot)] = act;
    switch (act) {
        case BindAction::Ignore: break;
        case BindAction::PassThrough: {
            // No shortcut on this button: the click itself goes to Android. The device tells a
            // real mouse from a finger by the non-primary button bits, so both halves carry them.
            const qint32 b = slotButton(slot);
            buttons_ |= b;
            sendTouch(touchDown_ ? kMotionActionMove : kMotionActionDown, e->position(), 1.0f, b,
                      buttons_);
            break;
        }
        case BindAction::Back: session_->sendControl(backOrScreenOn(kKeyActionDown)); break;
        case BindAction::Home:
            session_->sendControl(injectKeycode(kKeyActionDown, kKeycodeHome, 0, 0));
            break;
        case BindAction::AppSwitch:
            session_->sendControl(injectKeycode(kKeyActionDown, kKeycodeAppSwitch, 0, 0));
            break;
        case BindAction::Notifications:
            // One-shot: the panel has no press/release pair to mirror.
            session_->sendControl(expandNotificationPanel());
            break;
    }
}

void MirrorWindow::mouseMoveEvent(QMouseEvent *e) {
    if (touchDown_ || buttons_) sendTouch(kMotionActionMove, e->position(), 1.0f, 0, buttons_);
}

void MirrorWindow::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        touchDown_ = false;
        buttons_ &= ~kButtonPrimary;
        sendTouch(kMotionActionUp, e->position(), 0.0f, kButtonPrimary, buttons_);
        return;
    }
    const int slot = slotFor(e->button());
    if (slot < 0) return;
    const BindAction act = held_[size_t(slot)];
    held_[size_t(slot)] = BindAction::Ignore;
    switch (act) {
        case BindAction::Ignore: break;
        case BindAction::PassThrough: {
            const qint32 b = slotButton(slot);
            buttons_ &= ~b;
            sendTouch(buttons_ ? kMotionActionMove : kMotionActionUp, e->position(),
                      buttons_ ? 1.0f : 0.0f, b, buttons_);
            break;
        }
        case BindAction::Back: session_->sendControl(backOrScreenOn(kKeyActionUp)); break;
        case BindAction::Home:
            session_->sendControl(injectKeycode(kKeyActionUp, kKeycodeHome, 0, 0));
            break;
        case BindAction::AppSwitch:
            session_->sendControl(injectKeycode(kKeyActionUp, kKeycodeAppSwitch, 0, 0));
            break;
        case BindAction::Notifications: break;  // sent on press
    }
}

void MirrorWindow::wheelEvent(QWheelEvent *e) {
    session_->sendControl(injectScroll(positionAt(e->position()),
                                       float(e->angleDelta().x()) / 120.0f,
                                       float(e->angleDelta().y()) / 120.0f, 0));
}

void MirrorWindow::sendKey(QKeyEvent *e, quint8 action) {
    if (const auto keycode = androidKeycode(e->key())) {
        session_->sendControl(
            injectKeycode(action, *keycode, 0, androidMetaState(e->modifiers())));
    }
}

void MirrorWindow::toggleDisplayMode() {
    // Alt+D switches mirror ↔ desktop (fork semantics): launch the OTHER side's command from
    // the environment — the engine injects both as opaque shell lines — then close this window
    // so the toggle switches rather than stacks. REMORA_DESKTOP_NEW_WINDOW keeps this window
    // open (stacking). The launched line backgrounds the real client, and startDetached
    // reparents it, so it outlives this window.
    const bool isDesktop = qEnvironmentVariableIsSet("REMORA_IS_DESKTOP");
    const QString cmd = QString::fromLocal8Bit(
        qgetenv(isDesktop ? "REMORA_MIRROR_CMD" : "REMORA_DESKTOP_CMD"));
    if (cmd.isEmpty()) {
        qWarning("mirror: display toggle not configured");
        return;
    }
    if (!QProcess::startDetached(QStringLiteral("sh"), {QStringLiteral("-c"), cmd})) {
        qWarning("mirror: could not switch display mode");
        return;
    }
    qInfo("%s", isDesktop ? "mirror: switched to mirror" : "mirror: switched to desktop-mode display");
    if (!qEnvironmentVariableIsSet("REMORA_DESKTOP_NEW_WINDOW")) close();  // switch, not stack
}

// Send an action's DOWN half. Notifications is the odd one: a panel has no press/release pair.
void MirrorWindow::sendBindAction(BindAction act, quint8 action) {
    switch (act) {
        case BindAction::Back: session_->sendControl(backOrScreenOn(action)); break;
        case BindAction::Home:
            session_->sendControl(injectKeycode(action, kKeycodeHome, 0, 0));
            break;
        case BindAction::AppSwitch:
            session_->sendControl(injectKeycode(action, kKeycodeAppSwitch, 0, 0));
            break;
        case BindAction::Notifications:
            if (action == kKeyActionDown) session_->sendControl(expandNotificationPanel());
            break;
        case BindAction::PassThrough:
        case BindAction::Ignore: break;  // mouse-only; a key never carries these
    }
}

// What this key press means as a binding, or nullopt to let it through to the device as input.
std::optional<BindAction> MirrorWindow::boundAction(QKeyEvent *e) const {
    // MOD+b/h/s/n, on whichever modifier shortcut_mod armed.
    if (session_->isShortcutMod(e->modifiers())) {
        if (const auto act = chordAction(e->key())) return act;
        return std::nullopt;
    }
    // A direct key_bind is unmodified by definition — Shift included, so a bound letter still
    // types its capital.
    if (e->modifiers() != Qt::NoModifier) return std::nullopt;
    const auto it = session_->keyBindings().constFind(e->key());
    return it != session_->keyBindings().constEnd() ? std::optional(*it) : std::nullopt;
}

void MirrorWindow::keyPressEvent(QKeyEvent *e) {
    // The client's own chords ride the same modifier as the navigation ones: Alt+D toggles
    // mirror ↔ desktop, Alt+F toggles fullscreen. Consumed — never forwarded.
    if (session_->isShortcutMod(e->modifiers()) && !e->isAutoRepeat()) {
        if (e->key() == Qt::Key_D) {
            toggleDisplayMode();
            return;
        }
        if (e->key() == Qt::Key_F) {
            isFullScreen() ? showNormal() : showFullScreen();
            return;
        }
    }
    // Navigation bindings (key_bind, and the MOD+b/h/s/n chords). Held so the release finishes
    // what the press began even if the modifier is let go first, and so the release is swallowed
    // rather than reaching the device as a stray key.
    if (const auto act = boundAction(e)) {
        if (!e->isAutoRepeat()) {
            heldKeys_.insert(e->key(), *act);
            sendBindAction(*act, kKeyActionDown);
        }
        return;
    }
    // Ctrl+V pastes the host clipboard as a device paste — the whole message, not a key.
    if (e->key() == Qt::Key_V && e->modifiers() == Qt::ControlModifier) {
        session_->sendControl(setClipboard(0, true, QGuiApplication::clipboard()->text()));
        return;
    }
    // Plain printable input travels as text (the sdk keyboard's IME path handles layouts far
    // better than per-key mapping); modified or non-printable keys go as keycodes.
    const bool modified = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (!modified && !e->text().isEmpty() && e->text().at(0).isPrint()) {
        session_->sendControl(injectText(e->text()));
        return;
    }
    sendKey(e, kKeyActionDown);
}

void MirrorWindow::keyReleaseEvent(QKeyEvent *e) {
    // Keyed on the PRESS, not on what the modifiers say now: releasing Alt before the letter is
    // the normal way to let a chord go, and the device must still see the action end.
    const auto held = heldKeys_.constFind(e->key());
    if (held != heldKeys_.constEnd()) {
        if (e->isAutoRepeat()) return;
        const BindAction act = *held;
        heldKeys_.erase(held);
        sendBindAction(act, kKeyActionUp);
        return;
    }
    const bool modified = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (!modified && !e->text().isEmpty() && e->text().at(0).isPrint()) return;  // text sent on press
    sendKey(e, kKeyActionUp);
}

}  // namespace remora::mirror
