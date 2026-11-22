#include "DeskLayout.h"

#include <QProcess>
#include <QTimer>

namespace remora::hostagent {

namespace {
constexpr int kPollMs = 100;
// Two seconds for KWin to list a new output, then to show that the repair took. Measured: it
// lists within a few hundred milliseconds.
constexpr int kMaxTries = 20;
const QString kKScreenDoctor = QStringLiteral("kscreen-doctor");
}  // namespace

DeskLayout::DeskLayout(QObject *parent) : QObject(parent), poll_(new QTimer(this)) {
    poll_->setInterval(kPollMs);
    connect(poll_, &QTimer::timeout, this, &DeskLayout::attempt);
}

QByteArray DeskLayout::query(QString *error) {
    QProcess p;
    p.start(kKScreenDoctor, {QStringLiteral("-j")});
    if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        *error = QStringLiteral("kscreen-doctor -j failed: %1")
                     .arg(p.error() == QProcess::FailedToStart
                              ? QStringLiteral("not installed")
                              : QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        return {};
    }
    return p.readAllStandardOutput();
}

bool DeskLayout::apply(const QStringList &args, QString *error) {
    QProcess p;
    p.start(kKScreenDoctor, args);
    if (!p.waitForFinished(5000) || p.exitCode() != 0) {
        *error = QStringLiteral("kscreen-doctor %1 failed: %2")
                     .arg(args.join(QLatin1Char(' ')),
                          QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

bool DeskLayout::snapshot(QString *error) {
    const QByteArray json = query(error);
    if (json.isEmpty()) return false;
    before_ = outputs::parseKScreenJson(json);
    if (before_.isEmpty()) {
        *error = QStringLiteral("kscreen-doctor listed no outputs");
        return false;
    }
    return true;
}

void DeskLayout::repair(const QList<outputs::VirtualPlacement> &ours) {
    ours_ = ours;
    tries_ = 0;
    applied_ = false;
    poll_->start();
    attempt();
}

void DeskLayout::attempt() {
    QString error;
    const QByteArray json = query(&error);
    if (json.isEmpty()) {
        poll_->stop();
        emit settled(false, error);
        return;
    }
    const auto now = outputs::parseKScreenJson(json);
    now_ = now;
    bool listed = true;
    for (const auto &v : ours_) {
        bool found = false;
        for (const auto &s : now) found = found || s.name == v.name;
        listed = listed && found;
    }
    const QStringList args = outputs::repairArgs(before_, now, ours_);
    if (listed && args.isEmpty()) {
        poll_->stop();
        emit settled(true, QString());
        return;
    }
    if (++tries_ > kMaxTries) {
        poll_->stop();
        emit settled(false, listed ? QStringLiteral("KWin did not take the layout: ") +
                                         args.join(QLatin1Char(' '))
                                   : QStringLiteral("KWin never listed the virtual output"));
        return;
    }
    // Applied once already and not yet visible: give KWin the poll interval rather than piling
    // up identical requests.
    if (!listed || (applied_ && tries_ % 5 != 0)) return;
    if (!apply(args, &error)) {
        poll_->stop();
        emit settled(false, error);
        return;
    }
    applied_ = true;
}

QString DeskLayout::primaryOutput() const {
    for (const auto &s : before_)
        if (s.enabled && s.priority == 1) return s.name;
    return {};
}

std::optional<QRect> DeskLayout::geometryOf(const QString &name) const {
    for (const auto &s : now_)
        if (s.name == name) return QRect(s.pos, s.logicalSize());
    return std::nullopt;
}

void DeskLayout::restore() {
    poll_->stop();
    QString error;
    const QByteArray json = query(&error);
    if (json.isEmpty()) return;
    const QStringList args = outputs::repairArgs(before_, outputs::parseKScreenJson(json), {});
    if (!args.isEmpty()) apply(args, &error);
}

}  // namespace remora::hostagent
