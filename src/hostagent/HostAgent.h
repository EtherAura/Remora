#pragma once
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QString>

#include "DeskClipboard.h"
#include "DeskLayout.h"
#include "core/MirrorProto.h"
#include "KWinWayland.h"
#include "WindowPlacer.h"

class QLocalSocket;

namespace remora::hostagent {

class HostSession;

// The host agent: protocol v3 on a unix socket for this computer's own apps
// (docs/PHONE_APP.md §3). Serves list-apps and new-display sessions; a mirror or compose
// session — "show me the whole screen" — is refused, because what this agent offers is one app
// on a screen of the client's shape, never the desk itself.
//
// The socket lives in $XDG_RUNTIME_DIR, which only this user can enter: whoever reaches it drives
// the desk, so it is never a TCP port.
class HostAgent : public QObject {
    Q_OBJECT
public:
    explicit HostAgent(QObject *parent = nullptr);
    ~HostAgent() override;

    static QString defaultSocketPath();

    bool start(const QString &socketPath, const QString &encoder, QString *error);
    // What a session must leave free for the desk's monitors, in MiB; kAutoReserve sizes it from
    // the monitors that are lit (gpumem::monitorReserveMiB).
    static constexpr qint64 kAutoReserve = -1;
    void setVramReserve(qint64 mib) { vramReserveMiB_ = mib; }

signals:
    void fatal(const QString &why);

private:
    void onNewConnection();
    void onConnectionData(QLocalSocket *sock);
    void dispatch(QLocalSocket *sock, const mirror::ConnectionRequest &req, const QByteArray &rest);
    void startSession(QLocalSocket *sock, const mirror::ConnectionRequest &req,
                      const QByteArray &rest);
    void relayoutDesk();
    static bool deskLocked();
    static void lockDesk();
    QString freeOutputName() const;

    KWinWayland wl_;
    DeskLayout desk_;
    WindowPlacer placer_;
    DeskClipboard clipboard_;
    QLocalServer server_;
    QString encoder_, runtimeDir_;
    QHash<QLocalSocket *, QByteArray> handshakes_;  // connections still in hello/request
    QHash<QLocalSocket *, bool> helloDone_;
    QHash<quint32, HostSession *> sessions_;
    quint32 nextSession_ = 1;
    bool lockedAtStart_ = false;  // the desk was locked when the first of the live sessions began
    qint64 vramReserveMiB_ = kAutoReserve;
};

}  // namespace remora::hostagent
