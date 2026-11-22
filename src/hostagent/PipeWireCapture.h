#pragma once
#include <QHash>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstdint>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_proxy;
struct pw_stream;
struct pw_buffer;
struct spa_hook;
struct spa_pod;
struct spa_source;

namespace remora::hostagent {

// The virtual output's frames, from KWin's PipeWire stream to remora-frame-encoder's frame socket
// (docs/PHONE_APP.md §3). The host agent plays the role the Venus render server plays
// for host_encode: it publishes DMA-BUFs, one fd per buffer, and the encoder imports them on the
// GPU that rendered them — so a frame never leaves that GPU.
//
// KWin offers the layouts (DRM format modifiers) its renderer can produce, and the consumer has
// to name the ones it accepts. Nothing here renders, and the encoder imports on KWin's own GPU,
// so the agent accepts exactly what KWin offers: it reads the node's EnumFormat before
// connecting. Shared memory stays as the fallback, which the encoder's copy path reads.
//
// The stream runs on PipeWire's own thread; frames go to the encoder from there, and only the
// outcome signals cross to the Qt thread (queued).
//
// The newest frame is held, not handed straight back: KWin cannot draw into the buffer the
// encoder may still be reading, and when the encoder asks for that buffer's fd again (it dropped
// it, or never had it) the frame goes back at once. A still screen sends no next frame, so a
// request answered only with the next one left the phone on whatever it last had.
class PipeWireCapture : public QObject {
    Q_OBJECT
public:
    explicit PipeWireCapture(QObject *parent = nullptr);
    ~PipeWireCapture() override;

    // `serial` is the stream node's object serial (VirtualOutput::ready); `frameSocket` the
    // encoder's --frame-socket, already listening. Asynchronous: streaming() or failed() follows.
    bool start(quint64 serial, const QString &frameSocket, QString *error);
    void stop();

signals:
    void streaming(QSize size, bool dmabuf);
    void failed(const QString &why);

private:
    struct Buffer {
        quint32 resId = 0;
        bool fdSent = false;
    };

    // PipeWire's callbacks, run on its thread; defined with PipeWire's own signatures in the .cpp.
    friend struct PwCallbacks;

    void connectStream();  // once the offer is known
    void publish(pw_buffer *b);
    void readFdRequests();
    void onSocket(uint32_t mask);  // the encoder's fd requests, or its hangup
    void fail(const QString &why);

    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_registry *registry_ = nullptr;
    pw_proxy *node_ = nullptr;
    pw_stream *stream_ = nullptr;
    spa_hook *coreHook_ = nullptr, *registryHook_ = nullptr, *nodeHook_ = nullptr,
             *streamHook_ = nullptr;

    quint64 serial_ = 0;
    int enumSeq_ = -1;
    bool connected_ = false;
    int sock_ = -1;

    // KWin's offer, per video format: the modifiers it can produce.
    QHash<quint32, QList<quint64>> offer_;

    // The negotiated format.
    QSize size_;
    quint32 fourcc_ = 0;
    quint64 modifier_ = 0;
    bool dmabuf_ = false;

    QHash<pw_buffer *, Buffer> buffers_;
    pw_buffer *held_ = nullptr;  // the newest frame, dequeued until a newer one arrives
    spa_source *sockSource_ = nullptr;
    quint32 nextResId_ = 1;
    std::atomic<bool> announced_{false};
};

}  // namespace remora::hostagent
