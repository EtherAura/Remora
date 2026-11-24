#include "OutputLayout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

namespace remora::outputs {

QSize OutputState::logicalSize() const {
    const bool sideways = rotation == 2 || rotation == 8;
    const int w = sideways ? pixelSize.height() : pixelSize.width();
    const int h = sideways ? pixelSize.width() : pixelSize.height();
    const double s = scale > 0 ? scale : 1.0;
    // Rounded up, as KWin rounds a fractional logical extent.
    return {int(std::ceil(w / s)), int(std::ceil(h / s))};
}

QList<OutputState> parseKScreenJson(const QByteArray &json) {
    QList<OutputState> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    for (const QJsonValue &v : doc.object().value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject o = v.toObject();
        OutputState s;
        s.name = o.value(QStringLiteral("name")).toString();
        if (s.name.isEmpty()) continue;
        const QJsonValue enabled = o.value(QStringLiteral("enabled"));
        s.enabled = enabled.isBool() ? enabled.toBool()
                                     : o.value(QStringLiteral("connected")).toBool();
        const QJsonObject pos = o.value(QStringLiteral("pos")).toObject();
        s.pos = {pos.value(QStringLiteral("x")).toInt(), pos.value(QStringLiteral("y")).toInt()};
        s.scale = o.value(QStringLiteral("scale")).toDouble(1.0);
        s.priority = o.value(QStringLiteral("priority")).toInt();
        s.rotation = o.value(QStringLiteral("rotation")).toInt(1);
        s.replicationSource = o.value(QStringLiteral("replicationSource")).toInt(0);
        s.hdr = o.value(QStringLiteral("hdr")).toBool(false);
        const QString mode = o.value(QStringLiteral("currentModeId")).toVariant().toString();
        for (const QJsonValue &m : o.value(QStringLiteral("modes")).toArray()) {
            const QJsonObject mo = m.toObject();
            if (mo.value(QStringLiteral("id")).toVariant().toString() != mode) continue;
            const QJsonObject size = mo.value(QStringLiteral("size")).toObject();
            s.pixelSize = {size.value(QStringLiteral("width")).toInt(),
                           size.value(QStringLiteral("height")).toInt()};
        }
        out << s;
    }
    return out;
}

namespace {
const OutputState *find(const QList<OutputState> &list, const QString &name) {
    for (const OutputState &s : list)
        if (s.name == name) return &s;
    return nullptr;
}

QString scaleArg(double scale) {
    // kscreen-doctor parses the tail as a number: "2.625", never "2,625" or "2.62500".
    return QString::number(scale, 'g', 6);
}

// KWin keeps a scale in 120ths (wp_fractional_scale_v1) and rounds what it is asked for: a phone
// at 109 dpi asks for 0.68125 and gets 0.68333, so a check for the exact value never settled.
// Compared, and asked for, in KWin's own steps.
int scaleSteps(double scale) {
    return int(std::lround(scale * 120.0));
}
}  // namespace

double fitScale(QSize pixels, QSize minLogical) {
    if (pixels.isEmpty() || minLogical.isEmpty()) return std::numeric_limits<double>::infinity();
    const double most = std::min(double(pixels.width()) / minLogical.width(),
                                 double(pixels.height()) / minLogical.height());
    // Down to a whole step: rounded up, the output would come out a pixel too small again.
    return std::max(std::floor(most * 120.0) / 120.0, 0.5);
}

QStringList repairArgs(const QList<OutputState> &before, const QList<OutputState> &now,
                       const QList<VirtualPlacement> &ours) {
    QStringList args;
    int right = 0, top = 0, lastPriority = 0;
    bool any = false;
    for (const OutputState &b : before) {
        if (!b.enabled || b.replicationSource != 0) continue;
        const int r = b.pos.x() + b.logicalSize().width();
        right = any ? std::max(right, r) : r;
        top = any ? std::min(top, b.pos.y()) : b.pos.y();
        lastPriority = std::max(lastPriority, b.priority);
        any = true;
        const OutputState *n = find(now, b.name);
        if (!n || !n->enabled || n->replicationSource != 0) continue;  // gone, or now a mirror
        if (n->pos != b.pos)
            args << QStringLiteral("output.%1.position.%2,%3").arg(b.name).arg(b.pos.x()).arg(b.pos.y());
        if (n->priority != b.priority && b.priority > 0)
            args << QStringLiteral("output.%1.priority.%2").arg(b.name).arg(b.priority);
    }
    int x = right;
    int priority = lastPriority;
    for (const VirtualPlacement &v : ours) {
        const OutputState *n = find(now, v.name);
        if (!n) continue;  // KWin has not listed it yet; the caller retries
        ++priority;
        const QPoint want(x, top);
        if (n->pos != want)
            args << QStringLiteral("output.%1.position.%2,%3").arg(v.name).arg(want.x()).arg(want.y());
        if (n->priority != priority)
            args << QStringLiteral("output.%1.priority.%2").arg(v.name).arg(priority);
        const int steps = scaleSteps(v.scale);
        if (scaleSteps(n->scale) != steps)
            args << QStringLiteral("output.%1.scale.%2").arg(v.name, scaleArg(steps / 120.0));
        OutputState placed = *n;
        placed.scale = steps / 120.0;
        x += placed.logicalSize().width();
    }
    return args;
}

}  // namespace remora::outputs
