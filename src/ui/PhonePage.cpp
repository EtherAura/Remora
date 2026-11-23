#include "PhonePage.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

#include "engine/Engine.h"
#include "engine/Link.h"

namespace remora {

namespace {
constexpr qint64 kCodeLifetimeMs = 10 * 60 * 1000;  // engine/Link.cpp's pairing lifetime

bool linkServing() {
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, link::kDefaultPort);
    return probe.waitForConnected(300);
}

// The QR code as qrencode draws it. Nothing in Qt makes one, and the CLI already depends on
// qrencode for the same picture; without it the page shows the link alone. Error correction at
// level M rather than qrencode's L: a phone held at an angle to the screen misreads modules, and
// M survives twice as many for one size step (phone/test measures it).
QPixmap qrFor(const QString &text) {
    QProcess p;
    p.start(QStringLiteral("qrencode"), {QStringLiteral("-t"), QStringLiteral("PNG"),
                                         QStringLiteral("-s"), QStringLiteral("8"),
                                         QStringLiteral("-m"), QStringLiteral("2"),
                                         QStringLiteral("-l"), QStringLiteral("M"),
                                         QStringLiteral("-o"), QStringLiteral("-"), text});
    QPixmap pm;
    if (p.waitForFinished(5000) && p.exitCode() == 0) pm.loadFromData(p.readAllStandardOutput(), "PNG");
    return pm;
}
}  // namespace

PhonePage::PhonePage(QWidget *parent) : QWidget(parent) {
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);

    auto *pair = new QGroupBox(QStringLiteral("Pair a phone"));
    auto *pv = new QVBoxLayout(pair);
    auto *intro = new QLabel(QStringLiteral(
        "Scan the code with the Remora phone app, or with the phone's camera. A code works once, "
        "for ten minutes; a paired phone can then open this computer's apps and every running "
        "profile's Android."));
    intro->setWordWrap(true);
    pv->addWidget(intro);
    qr_ = new QLabel();
    qr_->setAlignment(Qt::AlignCenter);
    qr_->setMinimumHeight(120);
    qr_->setText(QStringLiteral("Press New code to pair a phone."));
    pv->addWidget(qr_);
    auto *uriRow = new QHBoxLayout();
    uri_ = new QLineEdit();
    uri_->setReadOnly(true);
    uri_->setPlaceholderText(QStringLiteral("No code yet"));
    copy_ = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copy"));
    copy_->setEnabled(false);
    connect(copy_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(uri_->text()); });
    uriRow->addWidget(uri_, 1);
    uriRow->addWidget(copy_);
    pv->addLayout(uriRow);
    auto *codeRow = new QHBoxLayout();
    auto *newCode = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                    QStringLiteral("New code"));
    connect(newCode, &QPushButton::clicked, this, &PhonePage::newCode);
    expiry_ = new QLabel();
    codeRow->addWidget(newCode);
    codeRow->addWidget(expiry_, 1);
    pv->addLayout(codeRow);
    col->addWidget(pair);

    auto *serve = new QGroupBox(QStringLiteral("Link"));
    auto *sv = new QHBoxLayout(serve);
    linkState_ = new QLabel();
    linkState_->setWordWrap(true);
    startLink_ = new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                                 QStringLiteral("Start"));
    startLink_->setToolTip(QStringLiteral("Run `remora link serve` in the background. "
                                          "remora-link.service keeps it running with the session."));
    connect(startLink_, &QPushButton::clicked, this, &PhonePage::startLink);
    sv->addWidget(linkState_, 1);
    sv->addWidget(startLink_);
    col->addWidget(serve);

    auto *paired = new QGroupBox(QStringLiteral("Paired phones"));
    auto *dv = new QVBoxLayout(paired);
    devices_ = new QListWidget();
    devices_->setMinimumHeight(80);
    dv->addWidget(devices_);
    forget_ = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Forget"));
    forget_->setEnabled(false);
    connect(devices_, &QListWidget::currentRowChanged, this,
            [this](int row) { forget_->setEnabled(row >= 0); });
    connect(forget_, &QPushButton::clicked, this, &PhonePage::forgetSelected);
    auto *fr = new QHBoxLayout();
    fr->addWidget(forget_);
    fr->addStretch(1);
    dv->addLayout(fr);
    col->addWidget(paired);

    countdown_ = new QTimer(this);
    countdown_->setInterval(1000);
    connect(countdown_, &QTimer::timeout, this, &PhonePage::tick);
}

void PhonePage::refresh() {
    const bool serving = linkServing();
    linkState_->setText(serving
                            ? QStringLiteral("Letting paired phones in on port %1.").arg(link::kDefaultPort)
                            : QStringLiteral("Not running: phones cannot reach this computer until "
                                             "it is."));
    startLink_->setVisible(!serving);
    devices_->clear();
    for (const link::Device &d : link::pairedDevices()) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1   —   paired %2")
                .arg(d.name, QDateTime::fromMSecsSinceEpoch(d.pairedAtMs).toString(
                                 QStringLiteral("yyyy-MM-dd hh:mm"))),
            devices_);
        item->setData(Qt::UserRole, d.name);
        item->setToolTip(QStringLiteral("Certificate %1").arg(d.fingerprint));
    }
    if (devices_->count() == 0) devices_->addItem(QStringLiteral("No phone is paired yet."));
    forget_->setEnabled(false);
}

void PhonePage::newCode() {
    QString why;
    const auto uri = link::beginPairing(link::kDefaultPort, &why);
    if (!uri) {
        qr_->setText(why);
        return;
    }
    uri_->setText(*uri);
    copy_->setEnabled(true);
    const QPixmap pm = qrFor(*uri);
    if (pm.isNull()) qr_->setText(QStringLiteral("Install qrencode to show this as a QR code."));
    else qr_->setPixmap(pm);
    expiresAtMs_ = QDateTime::currentMSecsSinceEpoch() + kCodeLifetimeMs;
    countdown_->start();
    tick();
    refresh();
}

void PhonePage::tick() {
    const qint64 left = expiresAtMs_ - QDateTime::currentMSecsSinceEpoch();
    if (left <= 0) {
        countdown_->stop();
        qr_->clear();
        uri_->clear();
        copy_->setEnabled(false);
        expiry_->setText(QStringLiteral("That code has expired."));
        return;
    }
    expiry_->setText(QStringLiteral("Valid for %1:%2 more.")
                         .arg(left / 60000)
                         .arg((left / 1000) % 60, 2, 10, QLatin1Char('0')));
}

void PhonePage::startLink() {
    RealSpawner sp;
    sp.spawnDetached({QCoreApplication::applicationFilePath(), QStringLiteral("link"),
                      QStringLiteral("serve")},
                     {});
    // It takes a moment to make its identity the first time and start listening.
    QTimer::singleShot(1500, this, &PhonePage::refresh);
}

void PhonePage::forgetSelected() {
    const QListWidgetItem *item = devices_->currentItem();
    if (!item || !item->data(Qt::UserRole).isValid()) return;
    link::forgetDevice(item->data(Qt::UserRole).toString());
    refresh();
}

}  // namespace remora
