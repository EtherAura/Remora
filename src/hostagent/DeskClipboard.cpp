#include "DeskClipboard.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>

namespace remora::hostagent {

namespace {
const QString kService = QStringLiteral("org.kde.klipper");
const QString kPath = QStringLiteral("/klipper");
const QString kInterface = QStringLiteral("org.kde.klipper.klipper");
}  // namespace

DeskClipboard::DeskClipboard(QObject *parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    available_ = bus.isConnected() && bus.interface() &&
                 bus.interface()->isServiceRegistered(kService).value();
    if (!available_) {
        qInfo("host agent: Klipper is not running — clipboard sync is off");
        return;
    }
    bus.connect(kService, kPath, kInterface, QStringLiteral("clipboardHistoryUpdated"), this,
                SLOT(onHistoryUpdated()));
    const QDBusReply<QString> now =
        QDBusInterface(kService, kPath, kInterface).call(QStringLiteral("getClipboardContents"));
    if (now.isValid()) last_ = now.value();
}

void DeskClipboard::set(const QString &text) {
    if (!available_ || text == last_) return;
    last_ = text;  // first, so the history update this causes is recognised as ours
    QDBusInterface(kService, kPath, kInterface).call(QStringLiteral("setClipboardContents"), text);
}

void DeskClipboard::onHistoryUpdated() {
    const QDBusReply<QString> now =
        QDBusInterface(kService, kPath, kInterface).call(QStringLiteral("getClipboardContents"));
    if (!now.isValid() || now.value() == last_) return;
    last_ = now.value();
    emit changed(last_);
}

}  // namespace remora::hostagent
