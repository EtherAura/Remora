#pragma once
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

namespace remora {

// The workspace's Phone page (docs/PHONE_APP.md §2): pair a phone by its QR code, see
// whether the link is letting phones in, and forget a paired one. The same operations as
// `remora link pair | serve | devices | forget`, through engine/Link.h.
class PhonePage : public QWidget {
    Q_OBJECT
public:
    explicit PhonePage(QWidget *parent = nullptr);

    // Re-reads what can change behind the page's back: whether the link is serving, and the
    // paired phones. Called on every visit.
    void refresh();

private:
    void newCode();
    void startLink();
    void forgetSelected();
    void tick();

    QLabel *qr_ = nullptr;
    QLineEdit *uri_ = nullptr;
    QPushButton *copy_ = nullptr;
    QLabel *expiry_ = nullptr;
    QLabel *linkState_ = nullptr;
    QPushButton *startLink_ = nullptr;
    QListWidget *devices_ = nullptr;
    QPushButton *forget_ = nullptr;
    QTimer *countdown_ = nullptr;
    qint64 expiresAtMs_ = 0;
};

}  // namespace remora
