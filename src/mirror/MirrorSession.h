#pragma once
#include <QImage>
#include <QObject>
#include <QLocalSocket>
#include <QProcess>
#include <QSize>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <memory>

class QLockFile;

#include "AudioPlayer.h"
#include "Decoder.h"
#include "Recorder.h"
#include "VideoReader.h"
#include "core/MirrorInput.h"
#include "core/MirrorProto.h"

namespace remora::mirror {

struct SessionOptions {
    QString serial;      // adb target
    QString videoCodec;  // empty = h264
    QString videoEncoder;         // pin a specific device encoder (the h265 hwc2 case)
    QString hwDecode;             // Decoder hw mode: off|auto|cuda|nvdec|vaapi|/dev/dri/…
    // Opt-in audio (bd remora-28ix.2.2): the second tunnel socket, opus by default, played via
    // QAudioSink. Off by default until it has proven itself as the daily driver.
    bool audio = false;
    QString recordPath;           // mux received packets here (mp4/mkv); finalized on stop
    QString newDisplay;           // desktop mode: "WxH/dpi" or "auto" (server-chosen)
    QString startApp;             // launched at session creation (the start_app session param)
    QStringList serverParams;     // raw key=value passthrough → ServerOptions.extraParams
    // No backend choice since the v1 purge (bd remora-28ix.5): the device half is the agent the
    // image bakes, and a session that cannot reach it fails plainly instead of substituting.
    // Compose mode: read the video from this host unix socket (the frame encoder listens; we
    // connect). The device runs video=false video_compose=true and opens no video socket.
    QString externalVideoSocket;
    std::optional<int> videoBitRate, maxSize;
    std::optional<double> maxFps;
    // What the non-primary mouse buttons do (remorarc mouse_bind / the Viewer's Navigation card).
    MouseBindings mouseBind = defaultMouseBindings();
    // The keyboard half of the same card: unmodified keys bound straight to an action, and the
    // modifier(s) that arm the MOD+b/h/s/n chords (and the client's own Alt+D/Alt+F).
    QHash<int, BindAction> keyBind;
    QList<Qt::KeyboardModifiers> shortcutMods = defaultShortcutMods();
    // A mirror that never receives a frame shows nothing but pins the encoder and holds VRAM —
    // the ghost this first-frame timeout exists to kill.
    int firstFrameTimeoutMs = 45000;
    // How long "nothing is listening on the agent socket" is retried before it becomes the
    // rebuild-the-image refusal. Zero = refuse immediately (the 28ix.5 contract for a session
    // against a supposedly-running device). The boot-splash flow sets this, because it connects
    // the moment the screen composites and the agent service starts 10+ s later — a cold boot
    // took the permanent refusal for that transient gap and the splash window died with it
    // (bd remora-j6yf).
    int agentWaitMs = 0;
};

// list-apps over the agent (kind=4): one request/response, no session, no window. Fills *out with
// the " * Name  pkg" lines verbatim; *absent set means "no agent answered" — since the v1 purge
// that is a refusal with the rebuild advice, not a fallback cue.
bool agentListApps(const QString &serial, QByteArray *out, QString *error, bool *absent);

// One mirror session: push the server, open the tunnel, spawn app_process, then demux/decode the
// video socket and speak the control socket. Single-threaded over the Qt event loop; teardown
// order on failure follows the contract: stop the server, close the sockets, remove the tunnel.
class MirrorSession : public QObject {
    Q_OBJECT
public:
    explicit MirrorSession(SessionOptions opts, QObject *parent = nullptr);
    ~MirrorSession() override;

    // Bring-up (push + tunnel + spawn, or the agent handshake), reporting through started().
    //
    // ASYNCHRONOUS because the caller is usually painting: the splash hands off by calling this,
    // and while this ran synchronously the adb subprocesses blocked the GUI thread mid-animation
    // — measured 0.17 s for the jar push plus 0.06 s for the tunnel, ~14 dropped frames at the
    // animation's 60 fps, landing on the instant that also triggers the ending and so announcing
    // it every time (bd remora-xrlh). Only the adb subprocess calls move to a worker; every
    // QObject here stays on this thread, so steady-state socket traffic is exactly as it was.
    void startAsync();
    // Build the hardware decoder's device context NOW. Call it while nothing is animating — the
    // splash does, before its first frame — because it costs ~226 ms and would otherwise be spent
    // on the first packet, which arrives exactly at hand-off (bd remora-xrlh).
    void warmDecoder();
    void stop();

    void sendControl(const QByteArray &msg);
    QSize videoSize() const { return videoSize_; }
    // Whether a resized window may ask the device to REFLOW the display to match, instead of
    // letting the GPU stretch old pixels (bd remora-28ix.3.3): only for a display this session
    // created — a mirror's size follows its source — and only over an agent session, which is
    // what implements RESIZE_DISPLAY.
    bool reflowsOnResize() const { return agent_ && !opts_.newDisplay.isEmpty(); }
    // Zero-copy hand-off (bd remora-28ix.2.3): the render window arms this once it knows its GL
    // context can take CUDA frames directly, and disarms it if interop fails mid-stream. A no-op
    // unless the decoder's hw device is CUDA.
    void setKeepHwFrames(bool keep) { decoder_.setKeepHwFrames(keep); }
    void *decoderCudaContext() const { return decoder_.cudaContext(); }
    const MouseBindings &mouseBindings() const { return opts_.mouseBind; }
    const QHash<int, BindAction> &keyBindings() const { return opts_.keyBind; }
    // Whether these modifiers are exactly one of the configured shortcut alternatives.
    bool isShortcutMod(Qt::KeyboardModifiers m) const { return opts_.shortcutMods.contains(m); }

signals:
    // Bring-up finished. Exactly one emission per startAsync(), always queued back onto this
    // thread, so a caller may treat it as it used to treat start()'s return value.
    void started(bool ok, const QString &error);
    void frameReady(const VideoFrame &frame);
    void videoSizeChanged(QSize size);
    void deviceClipboard(const QString &text);
    void failed(const QString &why);
    void finished();

private:
    void onVideoData();
    void onAudioData();
    void drainAudioStream();
    void onExtVideoData();
    void onControlData();
    void drainVideoStream();
    // The external video socket's half of drainVideoStream(), fed by VideoReader from its own
    // thread instead of by a readAll() on this one (bd remora-10bt). Same handling, same order —
    // only the demuxing moved.
    void startVideoReader();
    void onReaderCodec(quint8 codec);
    void onReaderFormat(const FrameFormat &format);
    void onReaderPacket(const MediaPacket &packet);
    // Both transports' shared tail: recorder tap, config merge, decode, emit. Also the
    // only place that can measure the STREAM (bytes, decode cost) rather than the window
    // — REMORA_MIRROR_STATS (bd remora-e5x.12).
    void decodePacket(MediaPacket pkt);
    void fail(const QString &why);
    bool runAdb(const QStringList &argv, QString *error);
    // Same, on a pooled worker thread; `done` runs back on this thread. The adb steps are the
    // only part of bring-up worth moving — they are whole subprocesses, and runAdb touches no
    // member state, so nothing here changes thread affinity. `done` is skipped if the session
    // died while adb ran.
    void runAdbAsync(const QStringList &argv, std::function<void(bool, QString)> done);
    // Same, delivering the command's output instead of just success (the sweep reads listings).
    void runAdbCaptureAsync(const QStringList &argv, std::function<void(bool, QString)> done);
    // Housekeeping at every session start: remove forward tunnels dead sessions left behind
    // (bd remora-28ix.2.4, remora-6f92). Gates nothing, never touches this session's own
    // tunnels, and dies with the session — an abandoned sweep is just housekeeping the next
    // session redoes.
    void sweepStaleTunnels();
    // Record ownership of a forwarded port for the sweep's liveness check (bd remora-6f92).
    void takeTunnelLock(quint16 port);
    // The bring-up chain, each step resuming on this thread after its adb hop. Split by where
    // the blocking is, not by phase: everything outside runAdbAsync is local and fast.
    // Bring-up: forward tunnel, hello, session request. Event-driven end to end — no
    // waitForConnected/waitForReadyRead anywhere, so the caller's event loop keeps running (the
    // splash keeps painting) for the whole handshake.
    void startAgent();
    void tryAgentPort(quint16 from);   // first port that binds locally AND forwards
    // Connect one tunnel socket and complete its RMRA hello. `done` runs exactly once: the socket
    // on success, nullptr plus one of the two reasons on failure.
    void openAgentConnection(const QString &connectError, const QString &helloError,
                             std::function<void(QTcpSocket *, QString)> done);
    void agentHandshake();                       // control connection
    void agentControlReady(QTcpSocket *sock);    // …then the session request
    void agentSessionOpen(QTcpSocket *sock, quint32 id, const QByteArray &leftover);
    void agentVideoReady(QTcpSocket *vid);
    void agentAudioThenDone();                   // audio is optional and never fatal
    void agentDone();
    // Nothing answered as an agent: a plain refusal with the rebuild advice — no fallback since
    // the v1 purge (bd remora-28ix.5).
    void agentUnavailable(const QString &why);
    // One place to end a bring-up, so every path emits started() exactly once.
    void finishStart(bool ok, const QString &error = {});
    bool startDone_ = false;
    // Compose mode, either backend: dial the host encoder's unix socket the video comes from.
    void connectExternalVideoAsync(std::function<void(bool, QString)> done);

    SessionOptions opts_;
    quint16 port_ = 0;
    bool tunnelOpen_ = false;
    bool agent_ = false;        // the control socket speaks to the in-image agent (protocol v2)
    qint64 agentGraceUntilMs_ = 0;  // epoch ms: retry a refused agent connect until then
    bool agentWaitLogged_ = false;  // the waiting notice logs once, not once per retry
    quint32 sessionId_ = 0;     // agent-assigned
    bool audioCodecSeen_ = false;  // audio announced before the recorder opened — replay it
    std::unique_ptr<QLockFile> tunnelLock_;  // ownership of our forwarded port (bd remora-6f92)
    QTcpSocket *video_ = nullptr;    // owned via parent
    QTcpSocket *audio_ = nullptr;    // owned via parent; only with opts_.audio
    QTcpSocket *control_ = nullptr;  // owned via parent
    QLocalSocket *extVideo_ = nullptr;  // compose mode: the encoder's unix socket
    // ...until the dial settles, at which point its descriptor is handed to reader_ and this
    // pointer is dropped: from then on nothing on THIS thread touches that socket, which is the
    // whole point (bd remora-10bt).
    QThread *readerThread_ = nullptr;
    VideoReader *reader_ = nullptr;
    bool compose_ = false;
    StreamDemuxer demux_{StreamDemuxer::Kind::Video};
    StreamDemuxer audioDemux_{StreamDemuxer::Kind::Audio};
    AudioPlayer audioPlayer_;
    bool audioDead_ = false;  // failed or server-disabled: reads stop, video is unaffected
    PacketMerger merger_;
    // REMORA_MIRROR_STATS accumulators, reset each reporting second.
    qint64 statsWindowMs_ = 0, statsBytes_ = 0, statsPackets_ = 0;
    qint64 statsDecodeUs_ = 0, statsDecodes_ = 0, statsDecodeMaxUs_ = 0;
    bool mergeConfig_ = true;  // h264/h265 prepend SPS/PPS; av1 passes through
    Decoder decoder_;
    Recorder recorder_;
    bool recording_ = false;
    DeviceMessageParser deviceMsgs_;
    QTimer firstFrameTimer_;
    bool frameSeen_ = false;
    bool stopping_ = false;
    QSize videoSize_;
};

}  // namespace remora::mirror
