#include "HostAgent.h"

#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>

#include "AppCatalog.h"
#include "Gpu.h"
#include "HostSession.h"

#include <algorithm>

namespace remora::hostagent {

using namespace remora::mirror;

QString HostAgent::defaultSocketPath() {
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
           QStringLiteral("/remora-host-agent/agent.sock");
}

HostAgent::HostAgent(QObject *parent) : QObject(parent) {
    connect(&wl_, &KWinWayland::disconnected, this, &HostAgent::fatal);
    // Every settle may have moved an output; every session re-reads where its output is.
    connect(&desk_, &DeskLayout::settled, this, [this] {
        for (HostSession *s : std::as_const(sessions_)) s->refreshGeometry();
    });
    connect(&clipboard_, &DeskClipboard::changed, this, [this](const QString &text) {
        for (HostSession *s : std::as_const(sessions_)) s->sendDeskClipboard(text);
    });
    connect(&placer_, &WindowPlacer::windowPlaced, this,
            [](quint32 session, const QString &id, int pid, bool isNew) {
                qInfo("host agent: session %u: placed %s window %s (pid %d)", session,
                      isNew ? "a new" : "an existing", qUtf8Printable(id), pid);
            });
}

HostAgent::~HostAgent() {
    const auto sessions = sessions_.values();
    sessions_.clear();
    qDeleteAll(sessions);
    // An ended session hands its encoder here to exit in its own time, SIGTERM already sent. At
    // shutdown there is no own time left: wait for each, so NVENC tears down cleanly, rather than
    // let QObject destroy a QProcess whose child still runs (Qt kills it outright, and says so).
    for (QProcess *enc : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        if (enc->state() == QProcess::NotRunning) continue;
        if (!enc->waitForFinished(3000)) {
            enc->kill();
            enc->waitForFinished(1000);
        }
    }
    if (desk_.hasSnapshot()) desk_.restore();
}

bool HostAgent::start(const QString &socketPath, const QString &encoder, QString *error) {
    encoder_ = encoder;
    if (!QFileInfo(encoder_).isExecutable()) {
        *error = QStringLiteral("remora-frame-encoder is not built (%1) — it needs FFmpeg 7 and "
                                "the Vulkan headers; see the build's configure output")
                     .arg(encoder_);
        return false;
    }
    if (!wl_.connectToCompositor(error)) {
        *error += QStringLiteral("\n  ") + grantHint(QCoreApplication::applicationFilePath());
        return false;
    }
    if (!wl_.hasFakeInput()) {
        *error = QStringLiteral("KWin did not offer fake input to this program — the phone could "
                                "watch but not touch. ") +
                 grantHint(QCoreApplication::applicationFilePath());
        return false;
    }
    if (!placer_.exportOnBus(error)) return false;

    runtimeDir_ = QFileInfo(socketPath).absolutePath();
    QDir().mkpath(runtimeDir_);
    QFile::setPermissions(runtimeDir_, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
    // The bus name above is the single-instance guard, so a socket file left here is stale.
    QLocalServer::removeServer(socketPath);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen(socketPath)) {
        *error = QStringLiteral("could not listen on %1: %2").arg(socketPath, server_.errorString());
        return false;
    }
    connect(&server_, &QLocalServer::newConnection, this, &HostAgent::onNewConnection);
    return true;
}

void HostAgent::onNewConnection() {
    while (QLocalSocket *sock = server_.nextPendingConnection()) {
        sock->setParent(this);
        handshakes_.insert(sock, {});
        helloDone_.insert(sock, false);
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] { onConnectionData(sock); });
        connect(sock, &QLocalSocket::disconnected, this, [this, sock] {
            if (handshakes_.remove(sock)) {
                helloDone_.remove(sock);
                sock->deleteLater();
            }
        });
    }
}

void HostAgent::onConnectionData(QLocalSocket *sock) {
    auto it = handshakes_.find(sock);
    if (it == handshakes_.end()) return;  // handed on: no longer ours to read
    it->append(sock->readAll());
    if (!helloDone_.value(sock)) {
        if (it->size() < kAgentHelloSize) return;
        const auto hello = parseClientHello(it->left(kAgentHelloSize));
        // Answer every hello with our own, even a mismatched one, so the client can name the
        // side to update; then only a matching client goes on.
        sock->write(agentHelloReply());
        if (!hello || hello->version != kAgentProtoVersion) {
            sock->flush();
            sock->disconnectFromServer();
            return;
        }
        it->remove(0, kAgentHelloSize);
        helloDone_[sock] = true;
    }
    int consumed = 0;
    QString error;
    const auto req = parseConnectionRequest(*it, &consumed, &error);
    if (!req) {
        if (!error.isEmpty()) sock->disconnectFromServer();
        return;
    }
    const QByteArray rest = it->mid(consumed);
    handshakes_.erase(it);
    helloDone_.remove(sock);
    disconnect(sock, nullptr, this, nullptr);
    dispatch(sock, *req, rest);
}

void HostAgent::dispatch(QLocalSocket *sock, const ConnectionRequest &req, const QByteArray &rest) {
    auto close = [sock] {
        QObject::connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        sock->flush();
        sock->disconnectFromServer();
    };
    switch (req.role) {
        case ConnRole::Video: {
            HostSession *s = sessions_.value(req.sessionId);
            if (!s) {
                sock->write(streamEndRecord(kEndFailed, QStringLiteral("no session %1").arg(req.sessionId)));
                return close();
            }
            s->attachVideo(sock);
            return;
        }
        case ConnRole::Audio: {
            HostSession *s = sessions_.value(req.sessionId);
            if (!s) {
                // Carry on without: the client treats this as "no audio", never as a failure.
                sock->write(streamEndRecord(kEndUnavailable,
                                            QStringLiteral("no session %1").arg(req.sessionId)));
                return close();
            }
            s->attachAudio(sock);
            return;
        }
        case ConnRole::Control:
            break;
    }
    switch (req.kind) {
        case SessionKind::ListApps:
            sock->write(listAppsReply(desktop::listAppsBlob(installedApps())));
            return close();
        case SessionKind::NewDisplay:
            return startSession(sock, req, rest);
        case SessionKind::AppIcon: {
            QString error;
            const int size = qBound(16, req.options.value(QStringLiteral("size"), QStringLiteral("192")).toInt(), 512);
            const QByteArray png = appIconPng(req.options.value(QStringLiteral("app")), size, &error);
            sock->write(png.isEmpty() ? sessionRefused(error) : listAppsReply(png));
            return close();
        }
        case SessionKind::Mirror:
        case SessionKind::Compose:
            sock->write(sessionRefused(QStringLiteral(
                "this computer serves its apps one at a time on a screen of their own, never the "
                "whole desk: ask for a new display")));
            return close();
    }
}

void HostAgent::startSession(QLocalSocket *sock, const ConnectionRequest &req,
                             const QByteArray &rest) {
    QString error;
    const auto params = SessionParams::fromOptions(req.options, &error);
    if (params && !params->startApp.isEmpty() && !findApp(params->startApp))
        error = QStringLiteral("no app %1 on this computer").arg(params->startApp);
    if (!params || !error.isEmpty()) {
        sock->write(sessionRefused(error));
        connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        sock->flush();
        sock->disconnectFromServer();
        return;
    }
    // The desk as it is before any output of ours: what every repair puts back. And whether it is
    // locked: nobody is at a locked desk, so whatever the phone unlocks there, the desk is locked
    // again when the phone leaves (the lock screen covers the virtual output too, so using an app
    // from a locked desk means unlocking it from the phone).
    if (sessions_.isEmpty()) lockedAtStart_ = deskLocked();
    if (sessions_.isEmpty() && !desk_.snapshot(&error)) {
        sock->write(sessionRefused(error));
        sock->flush();
        sock->disconnectFromServer();
        return;
    }
    // Before anything touches the desk: a session on a GPU without room for it can leave the
    // desk's own monitors without scanout memory (core/GpuMemory.h).
    if (const auto usage = vramUsage(renderNodeForConnector(desk_.primaryOutput()))) {
        const QString refusal =
            gpumem::vramRefusal(*usage, gpumem::sessionNeedMiB(params->pixels, vramReserveMiB_));
        if (!refusal.isEmpty()) {
            qInfo("host agent: refused a session: %s", qUtf8Printable(refusal));
            sock->write(sessionRefused(refusal));
            sock->flush();
            sock->disconnectFromServer();
            return;
        }
    }
    const quint32 id = nextSession_++;
    auto *s = new HostSession({&wl_, &desk_, &placer_, &clipboard_, encoder_, runtimeDir_}, id,
                              freeOutputName(), *params, this);
    sessions_.insert(id, s);
    sock->setParent(s);
    connect(s, &HostSession::outputLive, this, &HostAgent::relayoutDesk);
    connect(s, &HostSession::ready, this, [this, s, sock, rest](bool ok, const QString &why) {
        if (!ok) {
            if (sock->state() == QLocalSocket::ConnectedState) {
                sock->write(sessionRefused(why));
                sock->flush();
            }
            return;  // end() follows from the session itself
        }
        sock->write(sessionAccepted(s->id()));
        sock->flush();
        s->takeControl(sock, rest);
    });
    connect(s, &HostSession::ended, this, [this](quint32 sid, const QString &why) {
        HostSession *gone = sessions_.take(sid);
        qInfo("host agent: session %u ended: %s", sid, qUtf8Printable(why));
        if (gone) gone->deleteLater();
        if (!sessions_.isEmpty()) return relayoutDesk();
        desk_.restore();
        if (lockedAtStart_ && !deskLocked()) {
            qInfo("host agent: the desk was locked when the phone came; locking it again");
            lockDesk();
        }
    });
    // A client that leaves before its session is ready takes the session with it.
    connect(sock, &QLocalSocket::disconnected, s, [s] { s->end(QStringLiteral("the client left")); });
    s->start();
}

void HostAgent::relayoutDesk() {
    QList<outputs::VirtualPlacement> ours;
    QList<quint32> ids = sessions_.keys();
    std::sort(ids.begin(), ids.end());  // stable left-to-right order: oldest session first
    for (const quint32 id : ids) ours << sessions_[id]->placements();
    desk_.repair(ours);
}

bool HostAgent::deskLocked() {
    const QDBusReply<bool> active =
        QDBusInterface(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                       QStringLiteral("org.freedesktop.ScreenSaver"))
            .call(QStringLiteral("GetActive"));
    return active.isValid() && active.value();
}

void HostAgent::lockDesk() {
    QDBusInterface(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                   QStringLiteral("org.freedesktop.ScreenSaver"))
        .call(QStringLiteral("Lock"));
}

QString HostAgent::freeOutputName() const {
    // The lowest free number, so a desk keeps meeting the same few names — KWin saves a layout
    // per set of outputs, and a repaired one is then applied before any repair runs.
    for (int n = 1;; ++n) {
        const QString name = QStringLiteral("remora-%1").arg(n);
        bool used = false;
        for (const HostSession *s : sessions_) used = used || s->baseName() == name;
        if (!used) return name;
    }
}

}  // namespace remora::hostagent
