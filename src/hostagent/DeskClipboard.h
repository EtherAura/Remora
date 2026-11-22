#pragma once
#include <QObject>
#include <QString>

namespace remora::hostagent {

// The desk's clipboard, through Klipper — Plasma's clipboard service, which every Plasma session
// runs. A Wayland client may only read the clipboard while it has focus, and the agent never has
// focus; Klipper is the session's own reader and writer, over D-Bus.
class DeskClipboard : public QObject {
    Q_OBJECT
public:
    explicit DeskClipboard(QObject *parent = nullptr);

    // False when Klipper is not on the session bus (clipboard sync is then off, not fatal).
    bool available() const { return available_; }

    // Sets the desk's clipboard. The change this causes is not reported back through changed(),
    // so a phone never receives its own text as an echo.
    void set(const QString &text);

signals:
    void changed(const QString &text);

private slots:
    void onHistoryUpdated();

private:
    bool available_ = false;
    QString last_;
};

}  // namespace remora::hostagent
