#include "MirrorSession.h"

#include <unistd.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLockFile>
#include <QPointer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>
#include <memory>

namespace remora::mirror {

MirrorSession::MirrorSession(SessionOptions opts, QObject *parent)
    : QObject(parent), opts_(std::move(opts)) {
    firstFrameTimer_.setSingleShot(true);
    connect(&firstFrameTimer_, &QTimer::timeout, this, [this] {
        if (!frameSeen_) fail(QStringLiteral("no video frame within the first-frame timeout"));
    });
}

MirrorSession::~MirrorSession() { stop(); }

// Free, not a member, so the worker thread can run it: it owns a LOCAL QProcess and reads no
// member state, which is the whole reason the adb steps are safe to move off the GUI thread while
// every QObject in the session stays on it.
static bool runAdbBlocking(const QStringList &argv, QString *error, QString *output = nullptr) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(argv.first(), argv.mid(1));
    if (!p.waitForFinished(15000) || p.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("%1 failed: %2")
                         .arg(argv.join(QLatin1Char(' ')),
                              QString::fromUtf8(p.readAll()).trimmed());
        return false;
    }
    if (output) *output = QString::fromUtf8(p.readAll());
    return true;
}

// adb's own words for "there is no device behind that serial": the daemon cannot see it, it is
// offline or unauthorized, or a TCP serial it never connected to. Every one of these fails a
// `forward` before the device end is even asked, so none says anything about the image.
static bool adbLostDevice(const QString &err) {
    for (const char *sig : {"not found", "device offline", "unauthorized", "no devices/emulators",
                            "failed to connect", "Connection refused", "cannot connect"})
        if (err.contains(QLatin1String(sig))) return true;
    return false;
}

bool MirrorSession::runAdb(const QStringList &argv, QString *error) {
    return runAdbBlocking(argv, error);
}

void MirrorSession::runAdbAsync(const QStringList &argv, std::function<void(bool, QString)> done) {
    // QPointer, because bring-up outlives nothing: a mirror told to quit mid-push would otherwise
    // resume a chain through a destroyed session. adb itself is left to finish — it is a
    // subprocess with its own lifetime, and killing it mid-push corrupts the pushed jar.
    QPointer<MirrorSession> self(this);
    QThreadPool::globalInstance()->start([self, argv, done = std::move(done)]() mutable {
        QString err;
        const bool ok = runAdbBlocking(argv, &err);
        // Back onto the GUI thread. Posted to qApp rather than to `self` so the delivery does not
        // depend on an object that may be gone by now; the guard inside does that job.
        QMetaObject::invokeMethod(
            qApp,
            [self, ok, err, done = std::move(done)]() mutable {
                if (self) done(ok, err);
            },
            Qt::QueuedConnection);
    });
}

void MirrorSession::runAdbCaptureAsync(const QStringList &argv,
                                       std::function<void(bool, QString)> done) {
    // runAdbAsync with the command's merged output on success (an error message on failure).
    QPointer<MirrorSession> self(this);
    QThreadPool::globalInstance()->start([self, argv, done = std::move(done)]() mutable {
        QString text;
        const bool ok = runAdbBlocking(argv, &text, &text);
        QMetaObject::invokeMethod(
            qApp,
            [self, ok, text, done = std::move(done)]() mutable {
                if (self) done(ok, text);
            },
            Qt::QueuedConnection);
    });
}

void MirrorSession::warmDecoder() { decoder_.warmHardware(opts_.hwDecode); }

// The adb server listens on every registered forward itself, so a leaked forward and a live
// session's forward look identical from the outside — ownership has to be recorded. A QLockFile
// per forwarded port does it with kernel truth: the lock dies with the process (SIGKILL
// included), and QLockFile's own PID check reaps a crashed owner's file (bd remora-6f92).
static QString tunnelLockPath(quint16 port) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return dir + QStringLiteral("/remora-mirror-tunnel-%1.lock").arg(port);
}

void MirrorSession::takeTunnelLock(quint16 port) {
    tunnelLock_.reset(new QLockFile(tunnelLockPath(port)));
    tunnelLock_->setStaleLockTime(0);  // stale = owner PID gone, never mere age
    // Best-effort: a lock that cannot be taken must not fail the bring-up — the cost is only
    // that a sweep may later treat this session's forward as unowned.
    if (!tunnelLock_->tryLock(0))
        qWarning("mirror: could not lock %s — a stale-tunnel sweep may not spare this session",
                 qUtf8Printable(tunnelLockPath(port)));
}

void MirrorSession::startAgent() { tryAgentPort(kTunnelPortFirst); }

void MirrorSession::tryAgentPort(quint16 from) {
    quint16 p = from;
    for (; p <= kTunnelPortLast; ++p) {
        // Bind-test the port ourselves: adb forward happily reuses a port another client already
        // holds, and the second session would then reach the first one's tunnel. The test is a
        // bind() on a local socket — microseconds, so it stays on this thread; only the adb call
        // below is worth a worker.
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, p)) continue;
        probe.close();
        break;
    }
    if (p > kTunnelPortLast) {
        agentUnavailable(QStringLiteral("no usable tunnel port in %1..%2 for the agent")
                             .arg(kTunnelPortFirst)
                             .arg(kTunnelPortLast));
        return;
    }
    const quint16 port = p;
    runAdbAsync(adbAgentTunnelArgv(QStringLiteral("adb"), opts_.serial, port, false),
                [this, port](bool ok, QString err) {
                    if (!ok) {
                        // A forward can fail for two reasons that used to look the same: the
                        // port is already forwarded for someone else, or adb has no such device.
                        // Walking every port on the second and then blaming the IMAGE sent the
                        // operator to a multi-hour rebuild for a device that was simply down or
                        // mis-addressed (`mirror -s 127.0.0.1:1`).
                        if (adbLostDevice(err)) {
                            finishStart(false, QStringLiteral("device %1 is not reachable over "
                                                              "adb — %2")
                                                   .arg(opts_.serial, err));
                            return;
                        }
                        tryAgentPort(port + 1);  // that port forwards for someone else; next
                        return;
                    }
                    port_ = port;
                    tunnelOpen_ = true;
                    takeTunnelLock(port);
                    agentHandshake();
                });
}

// One tunnel connection taken as far as a completed RMRA hello, without blocking.
//
// This shape occurs three times (control, video, audio) and used to be written three times as
// waitForConnected + a waitForReadyRead loop. Those waits are not cheap despite the "localhost"
// address: each one crosses the adb forward to a device that may be on the LAN, and three of them
// in a row cost a measured 45-75 ms of frozen GUI thread — ~4 frames of the boot animation, at the
// exact moment the splash hands over (bd remora-xrlh).
//
// `done` runs exactly once: with the socket on success, or with nullptr and a reason. The socket
// is a child of this session, so a teardown mid-handshake takes it (and, because `this` is the
// connection context, these handlers) with it.
void MirrorSession::openAgentConnection(const QString &connectError, const QString &helloError,
                                        std::function<void(QTcpSocket *, QString)> done) {
    auto *sock = new QTcpSocket(this);
    struct Dial {
        QByteArray hello;
        bool settled = false;
        QMetaObject::Connection onConnected, onRead, onError;
        QTimer *deadline = nullptr;
    };
    auto d = std::make_shared<Dial>();
    d->deadline = new QTimer(sock);
    d->deadline->setSingleShot(true);

    // Every exit goes through here, so the hello's own handlers are ALWAYS disconnected before
    // the socket is handed on. Leaving them attached would let this readyRead keep consuming the
    // session reply and the video stream behind the real reader's back.
    auto settle = [this, sock, d, done](bool ok, const QString &err) {
        if (d->settled) return;
        d->settled = true;
        QObject::disconnect(d->onConnected);
        QObject::disconnect(d->onRead);
        QObject::disconnect(d->onError);
        d->deadline->stop();
        d->deadline->deleteLater();
        if (!ok) {
            sock->deleteLater();
            done(nullptr, err);
            return;
        }
        done(sock, QString());
    };

    d->onConnected = connect(sock, &QTcpSocket::connected, this,
                             [sock] { sock->write(agentHello()); });
    d->onError = connect(sock, &QTcpSocket::errorOccurred, this,
                         [settle, connectError](QAbstractSocket::SocketError) {
                             settle(false, connectError);
                         });
    d->onRead = connect(sock, &QTcpSocket::readyRead, this, [sock, d, settle, helloError] {
        d->hello.append(sock->readAll());
        if (d->hello.size() < kAgentHelloSize) return;
        // Nothing can follow the hello: the agent answers, it never speaks first, so there is no
        // trailing byte here to carry into the next reader.
        settle(parseAgentHello(d->hello).has_value(), helloError);
    });
    connect(d->deadline, &QTimer::timeout, this, [settle, connectError] {
        settle(false, connectError);
    });
    d->deadline->start(5000);
    sock->connectToHost(QHostAddress::LocalHost, port_);
}

void MirrorSession::agentHandshake() {
    // No dummy byte: the agent is already listening or it is not there. A forward whose device
    // end has no listener fails the connect outright, which is the probe's "old image" answer —
    // EXCEPT while the boot grace runs (opts_.agentWaitMs, set by the splash flow): a cold boot
    // composites the screen 10+ s before the agent service starts, and the splash used to take
    // the permanent refusal for that gap and die with it (bd remora-j6yf). A refused CONNECT
    // retries until the grace expires; a bad hello is something answering that is not an agent,
    // which no amount of waiting fixes.
    const QString notListening = QStringLiteral("nothing is listening on localabstract:%1")
                                     .arg(QString::fromLatin1(kAgentSocketName));
    openAgentConnection(notListening, QStringLiteral("no RMRA hello from the device"),
                        [this, notListening](QTcpSocket *sock, QString err) {
                            if (!sock) {
                                if (err == notListening && !stopping_ &&
                                    QDateTime::currentMSecsSinceEpoch() < agentGraceUntilMs_) {
                                    if (!agentWaitLogged_) {
                                        agentWaitLogged_ = true;
                                        qInfo("mirror: agent not up yet — retrying through the "
                                              "boot grace (%d s)", opts_.agentWaitMs / 1000);
                                    }
                                    QTimer::singleShot(1000, this,
                                                       [this] { if (!stopping_) agentHandshake(); });
                                    return;
                                }
                                agentUnavailable(err);
                                return;
                            }
                            agentControlReady(sock);
                        });
}

void MirrorSession::agentControlReady(QTcpSocket *sock) {
    // From here it IS an agent, whatever happens next — which is what stop() needs to know to
    // tear down the right tunnel, and what makes every failure below final rather than a fallback.
    agent_ = true;

    ServerOptions so;
    so.videoCompose = compose_;
    so.audio = opts_.audio;  // its own connection (role 2), opened after the session exists
    so.videoCodec = compose_ ? QString() : opts_.videoCodec;
    so.videoEncoder = compose_ ? QString() : opts_.videoEncoder;
    so.videoBitRate = compose_ ? std::nullopt : opts_.videoBitRate;
    so.maxSize = compose_ ? std::nullopt : opts_.maxSize;
    so.maxFps = opts_.maxFps;
    if (!opts_.newDisplay.isEmpty()) so.newDisplay = opts_.newDisplay;
    so.startApp = opts_.startApp;  // a session opt, not a v1 START_APP control message
    so.extraParams = opts_.serverParams;
    // The KIND is the coarse switch, the params refine it. A new display is its own kind — the
    // agent owns the display's whole lifecycle — and it wins over compose, which then rides
    // along as video_compose=true (a new display whose frames the host encoder consumes).
    SessionKind kind = !opts_.newDisplay.isEmpty() ? SessionKind::NewDisplay
                       : compose_                  ? SessionKind::Compose
                                                   : SessionKind::Mirror;
    sock->write(agentSessionRequest(kind, agentSessionParams(so)));

    // The reply arrives in its own time; accumulate until it parses whole. Anything past it is
    // already the control stream's first device message, so it is handed on rather than dropped.
    struct Reply {
        QByteArray buf;
        QMetaObject::Connection onRead;
        QTimer *deadline = nullptr;
    };
    auto r = std::make_shared<Reply>();
    r->deadline = new QTimer(sock);
    r->deadline->setSingleShot(true);

    auto give = [this, sock, r](bool ok, const QString &err, quint32 id, const QByteArray &rest) {
        QObject::disconnect(r->onRead);
        r->deadline->stop();
        r->deadline->deleteLater();
        if (!ok) {
            sock->deleteLater();
            finishStart(false, err);  // it IS an agent, so this is a failure, not a fallback
            return;
        }
        agentSessionOpen(sock, id, rest);
    };

    r->onRead = connect(sock, &QTcpSocket::readyRead, this, [sock, r, give] {
        r->buf.append(sock->readAll());
        int consumed = 0;
        const auto sr = parseSessionReply(r->buf, &consumed);
        if (!sr) return;
        if (sr->status != 0) {
            give(false, QStringLiteral("the agent declined the session: %1").arg(sr->reason), 0, {});
            return;
        }
        give(true, QString(), sr->sessionId, r->buf.mid(consumed));
    });
    connect(r->deadline, &QTimer::timeout, this, [give] {
        give(false, QStringLiteral("the agent did not answer the session request"), 0, {});
    });
    r->deadline->start(5000);
}

void MirrorSession::agentSessionOpen(QTcpSocket *sock, quint32 id, const QByteArray &leftover) {
    sessionId_ = id;
    // The connection IS the control socket from here; anything already read past the reply is
    // the first device message.
    control_ = sock;
    control_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(control_, &QTcpSocket::readyRead, this, &MirrorSession::onControlData);
    if (!leftover.isEmpty()) deviceMsgs_.feed(leftover);
    onControlData();

    // Compose mode takes its pixels from the host encoder's socket, so it opens no video
    // connection at all — the same shape as v1's video=false.
    if (compose_) {
        connectExternalVideoAsync([this](bool ok, QString err) {
            if (!ok) {
                finishStart(false, err);
                return;
            }
            agentAudioThenDone();
        });
        return;
    }
    openAgentConnection(QStringLiteral("could not open the agent video connection"),
                        QStringLiteral("no RMRA hello on the agent video connection"),
                        [this](QTcpSocket *vid, QString err) {
                            if (!vid) {
                                finishStart(false, err);
                                return;
                            }
                            agentVideoReady(vid);
                        });
}

void MirrorSession::agentVideoReady(QTcpSocket *vid) {
    vid->write(agentVideoAttach(sessionId_));
    video_ = vid;
    connect(video_, &QTcpSocket::readyRead, this, &MirrorSession::onVideoData);
    connect(video_, &QTcpSocket::disconnected, this, [this] {
        if (!stopping_) emit finished();
    });
    onVideoData();
    agentAudioThenDone();
}

void MirrorSession::agentAudioThenDone() {
    // Audio rides its own connection, so it is independent of both the video connection and
    // compose mode — a compose session gets audio the same way a mirror does. A failure here is
    // NOT fatal: losing audio should never cost the picture (the v1 path treats it as fatal only
    // because the socket order makes a missing audio socket a desync).
    if (!opts_.audio) {
        agentDone();
        return;
    }
    openAgentConnection(QStringLiteral("could not open the agent audio connection"),
                        QStringLiteral("no RMRA hello on the agent audio connection"),
                        [this](QTcpSocket *aud, QString err) {
                            if (!aud) {
                                qWarning("mirror: %s — audio off", qUtf8Printable(err));
                                agentDone();
                                return;
                            }
                            aud->write(agentAudioAttach(sessionId_));
                            audio_ = aud;
                            connect(audio_, &QTcpSocket::readyRead, this,
                                    &MirrorSession::onAudioData);
                            onAudioData();
                            agentDone();
                        });
}

void MirrorSession::agentDone() {
    firstFrameTimer_.start(opts_.firstFrameTimeoutMs);
    finishStart(true);
}

void MirrorSession::agentUnavailable(const QString &why) {
    // No fallback since the v1 purge (bd remora-28ix.5): the device half is the agent the image
    // bakes, so an unreachable agent means the image predates the bake or the device is not up —
    // the operator's news either way, never a path to substitute silently. stop() returns the
    // tunnel port if the probe got that far.
    // Only the device END not answering is evidence about the image. Running out of tunnel ports
    // is a host-side fact — seventeen forwards held by other clients, or a device adb never got
    // to — and prescribing a rebuild for it was the wrong advice every time.
    if (why.startsWith(QLatin1String("no usable tunnel port"))) {
        finishStart(false, QStringLiteral("could not open an adb tunnel to the device (%1) — "
                                          "is it up and connected? (`adb devices`)").arg(why));
        return;
    }
    finishStart(false, QStringLiteral("no in-image agent (%1) — rebuild the image with the "
                                      "mirror_agent feature (the v1 fallback is gone, "
                                      "bd remora-28ix.5)").arg(why));
}

// The encoder is the listening side and binds its socket before any frame exists, so a mirror
// that starts first has to wait for it. Event-driven for exactly the reason the agent handshake
// is: for a host-encoded profile this runs AT HAND-OFF, and blocking here froze the boot
// animation for as long as the encoder took to appear — up to the full retry budget, ~12 s of
// dead GUI thread in the worst case (bd remora-xrlh).
//
// The budget is kept in the shape the reference client used: at most kExtVideoAttempts connects,
// 50 ms apart, under an overall deadline. Per-attempt timeouts are gone — a unix connect either
// resolves or fails at once, so what the old 250 ms wait actually measured was the gap between
// retries, which is now the retry timer.
void MirrorSession::connectExternalVideoAsync(std::function<void(bool, QString)> done) {
    static constexpr int kExtVideoAttempts = 40;
    static constexpr int kExtVideoRetryMs = 50;
    static constexpr int kExtVideoDeadlineMs = 12000;

    extVideo_ = new QLocalSocket(this);
    struct Dial {
        int attempts = 0;
        bool settled = false;
        QMetaObject::Connection onConnected, onError;
        QTimer *deadline = nullptr;
    };
    auto d = std::make_shared<Dial>();
    d->deadline = new QTimer(extVideo_);
    d->deadline->setSingleShot(true);
    const QString err = QStringLiteral("could not connect to external video socket %1")
                            .arg(opts_.externalVideoSocket);

    auto settle = [this, d, done](bool ok, const QString &why) {
        if (d->settled) return;
        d->settled = true;
        QObject::disconnect(d->onConnected);
        QObject::disconnect(d->onError);
        d->deadline->stop();
        d->deadline->deleteLater();
        if (!ok) {
            done(false, why);
            return;
        }
        startVideoReader();
        done(true, QString());
    };

    d->onConnected = connect(extVideo_, &QLocalSocket::connected, this,
                             [settle] { settle(true, QString()); });
    d->onError = connect(extVideo_, &QLocalSocket::errorOccurred, this,
                         [this, d, settle, err](QLocalSocket::LocalSocketError) {
                             if (d->settled) return;
                             if (++d->attempts >= kExtVideoAttempts) {
                                 settle(false, err);
                                 return;
                             }
                             extVideo_->abort();  // reusable after abort; the fork reused it too
                             QTimer::singleShot(kExtVideoRetryMs, this, [this, d] {
                                 if (!d->settled) extVideo_->connectToServer(opts_.externalVideoSocket);
                             });
                         });
    connect(d->deadline, &QTimer::timeout, this, [settle, err] { settle(false, err); });
    d->deadline->start(kExtVideoDeadlineMs);
    extVideo_->connectToServer(opts_.externalVideoSocket);
}

// Teardown removes a session's forward tunnel, so what leaks is every exit that never runs it:
// a killed client, a crash — and the engine kills mirrors with TERM/KILL routinely. Sweeping at
// start is the only shape that cleans up after those (bd remora-28ix.2.4, remora-6f92). Liveness
// is the owner's lock file, not a device-side snapshot, and our own forward does not exist yet
// at session start; a live pre-lock-scheme session loses only its LISTENER — established streams
// survive a forward removal, and mirrors never redial mid-session. The v1 reverse-tunnel sweep
// (scrcpy_*) went with the v1 path itself: no jar server, no reverse tunnels to leak.
void MirrorSession::sweepStaleTunnels() {
    const QString adb = QStringLiteral("adb");
    runAdbCaptureAsync(adbForwardListArgv(adb), [this, adb](bool ok, QString list) {
        if (!ok) return;
        for (quint16 port : mirrorForwardPorts(list, opts_.serial)) {
            QLockFile probe(tunnelLockPath(port));
            probe.setStaleLockTime(0);
            if (!probe.tryLock(0)) continue;  // a live session owns this forward
            probe.unlock();
            qInfo("mirror: removing stale forward tunnel tcp:%u", port);
            runAdbAsync(adbAgentTunnelArgv(adb, opts_.serial, port, true), [](bool, QString) {});
        }
    });
}

void MirrorSession::startAsync() {
    compose_ = !opts_.externalVideoSocket.isEmpty();
    // The grace clock starts at bring-up, not per attempt: a boot that never produces an agent
    // must still end in the refusal, on schedule, no matter how many retries fit in between.
    agentGraceUntilMs_ = QDateTime::currentMSecsSinceEpoch() + opts_.agentWaitMs;
    sweepStaleTunnels();
    startAgent();
}

// Every bring-up path ends here, so started() is emitted exactly once however the chain unwound
// — including the fallback, which walks through two failures before its first real attempt.
void MirrorSession::finishStart(bool ok, const QString &error) {
    if (startDone_) return;
    startDone_ = true;
    emit started(ok, error);
}

void MirrorSession::onVideoData() {
    if (!video_) return;
    // No preamble: v1's 64-byte device-name header went with the v1 path — the agent's hello
    // already identified both ends before this socket carried a byte.
    demux_.feed(video_->readAll());
    drainVideoStream();
}

void MirrorSession::onExtVideoData() {
    if (!extVideo_) return;
    // The encoder socket has no preamble: codec id, session packet, frames.
    demux_.feed(extVideo_->readAll());
    drainVideoStream();
}

// Hand the settled connection to a thread that does nothing but read it (bd remora-10bt). Past
// this point extVideo_ is released to the reader and nulled here: two threads must never both hold
// that socket, and the pointer being gone is what makes that unrepresentable rather than merely
// discouraged.
void MirrorSession::startVideoReader() {
    const qintptr fd = extVideo_->socketDescriptor();
    if (fd == -1) {  // nothing to adopt: keep the old same-thread path rather than lose the stream
        connect(extVideo_, &QLocalSocket::readyRead, this, &MirrorSession::onExtVideoData);
        connect(extVideo_, &QLocalSocket::disconnected, this, [this] {
            if (!stopping_) emit finished();
        });
        onExtVideoData();
        return;
    }
    // DUP, not hand-over: a QLocalSocket owns its descriptor and closes it on teardown, so passing
    // the original meant the reader adopted an fd this thread was about to close — measured as
    // "QSocketNotifier: Invalid socket 27" and a mirror that never connected. Each side now owns
    // its own descriptor for the same connection, and each may close its own whenever it likes.
    const qintptr dupFd = ::dup(int(fd));
    if (dupFd == -1) return fail(QStringLiteral("could not duplicate the video socket descriptor"));
    extVideo_->close();
    extVideo_->deleteLater();
    extVideo_ = nullptr;

    readerThread_ = new QThread(this);
    readerThread_->setObjectName(QStringLiteral("mirror-video-reader"));
    reader_ = new VideoReader(dupFd);
    reader_->moveToThread(readerThread_);
    connect(readerThread_, &QThread::started, reader_, &VideoReader::start);
    connect(readerThread_, &QThread::finished, reader_, &QObject::deleteLater);
    // Queued by construction (different threads), which is what preserves ordering between these
    // and keeps every one of them running on the session's thread, exactly as before.
    connect(reader_, &VideoReader::codecIdReady, this, &MirrorSession::onReaderCodecId);
    connect(reader_, &VideoReader::sessionInfo, this, &MirrorSession::onReaderSession);
    connect(reader_, &VideoReader::packetReady, this, &MirrorSession::onReaderPacket);
    connect(reader_, &VideoReader::streamFailed, this, &MirrorSession::fail);
    connect(reader_, &VideoReader::socketClosed, this, [this] {
        if (!stopping_) emit finished();
    });
    readerThread_->start();
}

void MirrorSession::onReaderCodecId(quint32 codecId) {
    mergeConfig_ = codecId != kCodecAv1;
    QString err;
    if (!decoder_.init(codecId, opts_.hwDecode, &err)) return fail(err);
    if (opts_.recordPath.isEmpty()) return;
    if (!recorder_.open(opts_.recordPath, codecId, opts_.audio && !audioDead_, &err))
        return fail(err);
    recording_ = true;
    if (audioCodecSeen_) recorder_.setAudioCodec(audioDemux_.codecId());
}

void MirrorSession::onReaderSession(const SessionInfo &info) {
    videoSize_ = QSize(int(info.width), int(info.height));
    if (recording_) recorder_.setSize(videoSize_);
    emit videoSizeChanged(videoSize_);
}

void MirrorSession::onReaderPacket(const MediaPacket &packet) {
    // Tell the reader this one is off its books BEFORE the decode, not after: the count is how it
    // measures whether we are keeping up, and charging it for time we are already spending would
    // make it drop frames because we are busy decoding the very packet it is waiting on.
    if (reader_) reader_->packetConsumed();
    decodePacket(packet);
}

// The one place both transports converge (bd remora-e5x.12). Host encode arrives here from the
// reader thread's queued packets, device encode straight off the tunnel's demuxer — identical
// handling either way, which is what makes the two paths comparable at all.
//
// REMORA_MIRROR_STATS instruments exactly what the fps counter cannot see. That one counts frames
// PRESENTED, so it measures the window; this counts bytes and decode time as they arrive, so it
// measures the STREAM. Both are needed: a comparison that reports only presented fps cannot tell a
// slow encoder from a busy compositor, and the duty-cycle trap on this bead's own history came from
// having just the one number.
void MirrorSession::decodePacket(MediaPacket pkt) {
    static const bool statsOn = !qEnvironmentVariableIsEmpty("REMORA_MIRROR_STATS");
    if (statsOn) {
        if (!statsWindowMs_) statsWindowMs_ = QDateTime::currentMSecsSinceEpoch();
        statsBytes_ += pkt.payload.size();
        ++statsPackets_;
    }
    if (recording_) {
        // The recorder sees the RAW packet stream (config → extradata), before the decoder's
        // merger folds parameter sets into frame data.
        QString rerr;
        if (!recorder_.push(pkt, &rerr)) {
            qWarning("mirror: %s — recording stopped", qUtf8Printable(rerr));
            recorder_.finalize();
            recording_ = false;
        }
    }
    std::optional<MediaPacket> ready = mergeConfig_ ? merger_.merge(std::move(pkt))
                                                    : std::optional<MediaPacket>(std::move(pkt));
    if (!ready) return;
    QString err;
    QElapsedTimer decodeTimer;
    if (statsOn) decodeTimer.start();
    const auto frames = decoder_.decode(ready->payload, &err);
    if (statsOn) {
        const qint64 us = decodeTimer.nsecsElapsed() / 1000;
        statsDecodeUs_ += us;
        if (us > statsDecodeMaxUs_) statsDecodeMaxUs_ = us;
        ++statsDecodes_;
    }
    if (!err.isEmpty()) qWarning("mirror: %s", qUtf8Printable(err));
    for (const VideoFrame &f : frames) {
        if (!frameSeen_) {
            frameSeen_ = true;
            firstFrameTimer_.stop();
        }
        emit frameReady(f);
    }
    if (!statsOn) return;
    // Per second, on the same cadence as the fps line so the two can be read side by side.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 span = now - statsWindowMs_;
    if (span < 1000) return;
    qInfo("mirror: stream %.1f Mbit/s, %.0f pkt/s, decode %.1f ms avg / %.1f ms max",
          double(statsBytes_) * 8.0 / double(span) / 1000.0,
          double(statsPackets_) * 1000.0 / double(span),
          statsDecodes_ ? double(statsDecodeUs_) / double(statsDecodes_) / 1000.0 : 0.0,
          double(statsDecodeMaxUs_) / 1000.0);
    statsWindowMs_ = now;
    statsBytes_ = statsPackets_ = statsDecodeUs_ = statsDecodes_ = statsDecodeMaxUs_ = 0;
}

void MirrorSession::drainVideoStream() {
    for (;;) {
        // Take whatever else has landed BEFORE decoding the next packet (bd remora-10bt). This
        // loop decodes a whole batch without returning to the event loop, so readyRead cannot
        // fire meanwhile and the bytes pile up unread in the socket — which is precisely when the
        // encoder's send blocks and its 500 ms eviction timer starts against a mirror that is
        // working perfectly, just busy. Reading here costs one syscall per packet and keeps the
        // queue drained across the batch. It cannot desynchronise the stream: the demuxer consumes
        // a byte stream and holds partial frames itself.
        if (extVideo_ && extVideo_->bytesAvailable() > 0) demux_.feed(extVideo_->readAll());
        const auto e = demux_.next();
        if (e == StreamDemuxer::Event::NeedMoreData) return;
        switch (e) {
            case StreamDemuxer::Event::CodecId: {
                mergeConfig_ = demux_.codecId() != kCodecAv1;
                QString err;
                if (!decoder_.init(demux_.codecId(), opts_.hwDecode, &err)) return fail(err);
                if (!opts_.recordPath.isEmpty()) {
                    if (!recorder_.open(opts_.recordPath, demux_.codecId(),
                                        opts_.audio && !audioDead_, &err))
                        return fail(err);
                    recording_ = true;
                    // The audio socket may have announced its codec before the video stream
                    // opened the recorder — replay what it missed, or the header would wait
                    // the full hold cap for news that already arrived.
                    if (audioCodecSeen_) recorder_.setAudioCodec(audioDemux_.codecId());
                }
                break;
            }
            case StreamDemuxer::Event::Session: {
                const auto s = demux_.session();
                videoSize_ = QSize(int(s.width), int(s.height));
                if (recording_) recorder_.setSize(videoSize_);
                emit videoSizeChanged(videoSize_);
                break;
            }
            case StreamDemuxer::Event::Packet:
                decodePacket(demux_.takePacket());
                break;
            case StreamDemuxer::Event::StreamDisabled:
                return fail(QStringLiteral("the server disabled the video stream"));
            case StreamDemuxer::Event::ConfigError:
                return fail(QStringLiteral("server-side configuration error on the video stream"));
            case StreamDemuxer::Event::Error:
                return fail(QStringLiteral("video stream framing error: %1").arg(demux_.errorString()));
            case StreamDemuxer::Event::NeedMoreData: break;  // unreachable
        }
    }
}

void MirrorSession::onAudioData() {
    if (!audio_ || audioDead_) return;
    audioDemux_.feed(audio_->readAll());
    drainAudioStream();
}

void MirrorSession::drainAudioStream() {
    for (;;) {
        const auto e = audioDemux_.next();
        if (e == StreamDemuxer::Event::NeedMoreData) return;
        switch (e) {
            case StreamDemuxer::Event::CodecId: {
                audioCodecSeen_ = true;
                if (recording_) recorder_.setAudioCodec(audioDemux_.codecId());
                QString err;
                if (!audioPlayer_.init(audioDemux_.codecId(), &err)) {
                    qWarning("mirror: audio disabled: %s", qUtf8Printable(err));
                    audioDead_ = true;
                    return;
                }
                qInfo("mirror: audio on (codec id 0x%08x, 48 kHz stereo)", audioDemux_.codecId());
                break;
            }
            case StreamDemuxer::Event::Packet: {
                const MediaPacket pkt = audioDemux_.takePacket();
                // The recorder taps the RAW stream here, before AudioPlayer's keep-it-live drop
                // policy — a recording wants every packet with its original stamps, not the
                // player's latency trims baked into the file (bd remora-28ix.2.5).
                if (recording_) {
                    QString rerr;
                    if (!recorder_.pushAudio(pkt, &rerr))
                        qWarning("mirror: %s — audio write skipped", qUtf8Printable(rerr));
                }
                audioPlayer_.submit(pkt);
                break;
            }
            // Audio failure never takes the session down — video is the load-bearing half. The
            // reference client treats a config error as fatal; an opt-in extra losing itself
            // loudly beats losing the mirror (deliberate divergence, bd remora-28ix.2.2).
            // The recorder hears about it too, so a held header stops waiting for a stream
            // that is not coming.
            case StreamDemuxer::Event::StreamDisabled:
                qWarning("mirror: the server disabled the audio stream");
                audioDead_ = true;
                if (recording_) recorder_.audioUnavailable();
                return;
            case StreamDemuxer::Event::ConfigError:
                qWarning("mirror: server-side audio configuration error — audio off");
                audioDead_ = true;
                if (recording_) recorder_.audioUnavailable();
                return;
            case StreamDemuxer::Event::Error:
                qWarning("mirror: audio framing error: %s — audio off",
                         qUtf8Printable(audioDemux_.errorString()));
                audioDead_ = true;
                if (recording_) recorder_.audioUnavailable();
                return;
            case StreamDemuxer::Event::Session:
            case StreamDemuxer::Event::NeedMoreData:
                break;  // audio has no session packets
        }
    }
}

void MirrorSession::onControlData() {
    if (!control_) return;
    // start_app rode the session-create params (agentSessionParams); v1's TYPE_START_APP control
    // message went with the v1 path.
    deviceMsgs_.feed(control_->readAll());
    while (auto m = deviceMsgs_.next()) {
        if (m->type == DeviceMessage::Type::Clipboard) emit deviceClipboard(m->clipboardText);
        // AckClipboard / UhidOutput: nothing to do yet (no uhid devices, no sequenced sets).
    }
    if (deviceMsgs_.error()) {
        // Unknown type = no resync point. Stop reading; the video stream is unaffected.
        disconnect(control_, &QTcpSocket::readyRead, this, &MirrorSession::onControlData);
        qWarning("mirror: unknown device message — control channel reads stopped");
    }
}

void MirrorSession::sendControl(const QByteArray &msg) {
    if (control_ && control_->state() == QAbstractSocket::ConnectedState) control_->write(msg);
}

void MirrorSession::fail(const QString &why) {
    if (stopping_) return;
    qWarning("mirror: %s", qUtf8Printable(why));
    emit failed(why);
}

void MirrorSession::stop() {
    if (stopping_) return;
    stopping_ = true;
    recorder_.finalize();  // the container's moov lands here — every exit path runs stop()
    if (audioPlayer_.playedBytes() || audioPlayer_.droppedBytes())
        qInfo("mirror: audio played %lld bytes, dropped %lld",
              audioPlayer_.playedBytes(), audioPlayer_.droppedBytes());
    audioPlayer_.stop();
    if (video_) video_->close();
    if (audio_) audio_->close();
    if (extVideo_) extVideo_->close();
    // Stop the reader IN its own thread and wait for it: the socket and demuxer live there, and
    // tearing the session down underneath a thread still demuxing into queued signals is the one
    // way this refactor could turn a clean exit into a crash.
    if (readerThread_) {
        if (reader_) QMetaObject::invokeMethod(reader_, "stop", Qt::BlockingQueuedConnection);
        readerThread_->quit();
        readerThread_->wait(2000);
        reader_ = nullptr;  // deleted by the thread's finished() -> deleteLater
    }
    if (control_) control_->close();
    if (tunnelOpen_) {
        // Closing the control socket is what ends an agent session — the agent reaps on
        // disconnect, so there is no server process to stop and no stale session to leave behind.
        runAdb(adbAgentTunnelArgv(QStringLiteral("adb"), opts_.serial, port_, true), nullptr);
    }
    tunnelOpen_ = false;
    tunnelLock_.reset();  // after the removal: the port stays owned until it stops existing
}

bool agentListApps(const QString &serial, QByteArray *out, QString *error, bool *absent) {
    // list-apps over the agent: kind=4, the ok reply, then one u32-prefixed UTF-8 blob
    // (docs/MIRROR_AGENT.md). A free function rather than a MirrorSession mode on purpose —
    // there is no session here: no window, no stream, nothing to supervise, and the bring-up
    // dance is small enough that entangling it with startAgent's lifetime buys nothing.
    if (absent) *absent = false;
    const QString adb = QStringLiteral("adb");
    QString adbErr;
    const auto adbRun = [&](const QStringList &argv) {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(argv.first(), argv.mid(1));
        const bool ok = p.waitForFinished(15000) && p.exitCode() == 0;
        adbErr = ok ? QString() : QString::fromUtf8(p.readAll()).trimmed();
        return ok;
    };

    quint16 port = 0;
    for (quint16 p = kTunnelPortFirst; p <= kTunnelPortLast && !port; ++p) {
        QTcpServer probe;  // bind-test first: adb forward happily stacks onto a taken port
        if (!probe.listen(QHostAddress::LocalHost, p)) continue;
        probe.close();
        if (adbRun(adbAgentTunnelArgv(adb, serial, p, false))) port = p;
        else if (adbLostDevice(adbErr)) {
            // Not a port in use — adb has no such device. Say that; the image is not in question.
            if (error) *error = QStringLiteral("device %1 is not reachable over adb — %2")
                                    .arg(serial, adbErr);
            return false;
        }
    }
    if (!port) {
        // Not evidence of absence: an agent that is missing shows up as the device end refusing
        // the connect (below), never as the host running out of forwards.
        if (error) *error = QStringLiteral("no usable tunnel port in %1..%2 for the agent")
                                .arg(kTunnelPortFirst).arg(kTunnelPortLast);
        return false;
    }
    const auto fail = [&](const QString &why, bool notAnAgent) {
        adbRun(adbAgentTunnelArgv(adb, serial, port, true));
        if (absent) *absent = notAnAgent;
        if (error) *error = why;
        return false;
    };

    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, port);
    if (!sock.waitForConnected(2000))
        return fail(QStringLiteral("nothing is listening on localabstract:%1")
                        .arg(QString::fromLatin1(kAgentSocketName)), true);
    sock.write(agentHello());
    QByteArray hello;
    while (hello.size() < kAgentHelloSize && sock.waitForReadyRead(3000))
        hello.append(sock.readAll());
    if (!parseAgentHello(hello))
        return fail(QStringLiteral("no RMRA hello from the device"), true);

    sock.write(agentSessionRequest(SessionKind::ListApps, {}));
    QByteArray buf;
    std::optional<SessionReply> sr;
    int consumed = 0;
    for (;;) {
        sr = parseSessionReply(buf, &consumed);
        if (sr) break;
        if (!sock.waitForReadyRead(5000))
            return fail(QStringLiteral("the agent did not answer the list-apps request"), false);
        buf.append(sock.readAll());
    }
    if (sr->status != 0)
        return fail(QStringLiteral("the agent declined list-apps: %1").arg(sr->reason), false);

    // u32 length, then the listing. The blob is bounded only by the app count, but a framing
    // error must not become an unbounded read — 8 MiB is orders of magnitude past any device.
    buf = buf.mid(consumed);
    while (buf.size() < 4 && sock.waitForReadyRead(5000)) buf.append(sock.readAll());
    if (buf.size() < 4)
        return fail(QStringLiteral("the agent sent no app listing"), false);
    const quint32 len = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar *>(buf.constData()));
    if (len > (8u << 20))
        return fail(QStringLiteral("implausible app-listing length %1").arg(len), false);
    while (quint32(buf.size()) < 4 + len && sock.waitForReadyRead(5000))
        buf.append(sock.readAll());
    if (quint32(buf.size()) < 4 + len)
        return fail(QStringLiteral("the app listing was cut short"), false);
    *out = buf.mid(4, int(len));
    adbRun(adbAgentTunnelArgv(adb, serial, port, true));
    return true;
}

}  // namespace remora::mirror
