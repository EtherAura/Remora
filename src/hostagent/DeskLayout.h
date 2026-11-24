#pragma once
#include <QList>
#include <QObject>
#include <QRect>
#include <QStringList>

#include <optional>

#include "core/OutputLayout.h"

class QTimer;

namespace remora::hostagent {

// The desk's monitors, held still while the agent's virtual outputs come and go
// (core/OutputLayout.h has the why). Talks to KWin through kscreen-doctor, which every Plasma
// install carries.
class DeskLayout : public QObject {
    Q_OBJECT
public:
    explicit DeskLayout(QObject *parent = nullptr);

    // Records the desk as it is. Call while none of the agent's outputs exist — the snapshot is
    // what every later repair restores. False with *error when kscreen-doctor cannot be run.
    bool snapshot(QString *error);
    bool hasSnapshot() const { return !before_.isEmpty(); }
    const QList<outputs::OutputState> &snapshotOutputs() const { return before_; }
    // The snapshot's primary output (priority 1): the one KWin renders for.
    QString primaryOutput() const;
    // An output's global logical rectangle, as of the latest look at the desk.
    std::optional<QRect> geometryOf(const QString &name) const;

    // Applies the layout for these outputs of ours. KWin lists a new output a moment after its
    // stream is ready, so this polls (briefly) until every one of them is listed, applies the
    // repair, and checks it took. settled() reports the outcome; a later call supersedes one
    // still in progress.
    void repair(const QList<outputs::VirtualPlacement> &ours);

    // Puts the desk back exactly as snapshotted (after the last output of ours is gone).
    void restore();

signals:
    void settled(bool ok, const QString &detail);

private:
    void attempt();
    static QByteArray query(QString *error);
    static bool apply(const QStringList &args, QString *error);

    QList<outputs::OutputState> before_, now_;
    QList<outputs::VirtualPlacement> ours_;
    QTimer *poll_ = nullptr;
    int tries_ = 0;
    bool applied_ = false;
};

}  // namespace remora::hostagent
