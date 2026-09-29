#include "MirrorInput.h"

#include <QStringList>

namespace remora::mirror {

std::optional<qint32> androidKeycode(int qtKey) {
    // Letters and digits are contiguous in both key spaces.
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) return 29 + (qtKey - Qt::Key_A);
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) return 7 + (qtKey - Qt::Key_0);
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12) return 131 + (qtKey - Qt::Key_F1);
    switch (qtKey) {
        case Qt::Key_Escape: return 111;      // AKEYCODE_ESCAPE
        case Qt::Key_Tab:
        case Qt::Key_Backtab: return 61;      // AKEYCODE_TAB (shift arrives via metastate)
        case Qt::Key_Backspace: return 67;    // AKEYCODE_DEL
        case Qt::Key_Return:
        case Qt::Key_Enter: return 66;        // AKEYCODE_ENTER
        case Qt::Key_Insert: return 124;      // AKEYCODE_INSERT
        case Qt::Key_Delete: return 112;      // AKEYCODE_FORWARD_DEL
        case Qt::Key_Home: return 122;        // AKEYCODE_MOVE_HOME
        case Qt::Key_End: return 123;         // AKEYCODE_MOVE_END
        case Qt::Key_Left: return 21;         // AKEYCODE_DPAD_LEFT
        case Qt::Key_Up: return 19;           // AKEYCODE_DPAD_UP
        case Qt::Key_Right: return 22;        // AKEYCODE_DPAD_RIGHT
        case Qt::Key_Down: return 20;         // AKEYCODE_DPAD_DOWN
        case Qt::Key_PageUp: return 92;       // AKEYCODE_PAGE_UP
        case Qt::Key_PageDown: return 93;     // AKEYCODE_PAGE_DOWN
        case Qt::Key_Space: return 62;        // AKEYCODE_SPACE
        default: return std::nullopt;
    }
}

qint32 androidMetaState(Qt::KeyboardModifiers mods) {
    qint32 meta = 0;
    if (mods & Qt::ShiftModifier) meta |= 0x1 | 0x40;        // SHIFT_ON | SHIFT_LEFT_ON
    if (mods & Qt::AltModifier) meta |= 0x2 | 0x10;          // ALT_ON | ALT_LEFT_ON
    if (mods & Qt::ControlModifier) meta |= 0x1000 | 0x2000; // CTRL_ON | CTRL_LEFT_ON
    if (mods & Qt::MetaModifier) meta |= 0x10000 | 0x20000;  // META_ON | META_LEFT_ON
    return meta;
}

static std::optional<BindAction> actionFromChar(QChar c) {
    switch (c.unicode()) {
        case '+': return BindAction::PassThrough;
        case '-': return BindAction::Ignore;
        case 'b': return BindAction::Back;
        case 'h': return BindAction::Home;
        case 's': return BindAction::AppSwitch;
        case 'n': return BindAction::Notifications;
        default: return std::nullopt;
    }
}

MouseBindings defaultMouseBindings() {
    return *parseMouseBind(QStringLiteral("bhsn:++++"));
}

std::optional<MouseBindings> parseMouseBind(const QString &spec) {
    // Exactly "WXYZ:wxyz". The shifted half is not optional: the card always writes both, and a
    // half-vector would leave four buttons to a default the caller cannot see.
    const QStringList halves = spec.split(QLatin1Char(':'));
    if (halves.size() != 2) return std::nullopt;
    MouseBindings b;
    for (int half = 0; half < 2; ++half) {
        if (halves[half].size() != kSlotCount) return std::nullopt;
        for (int i = 0; i < kSlotCount; ++i) {
            const auto a = actionFromChar(halves[half].at(i));
            if (!a) return std::nullopt;
            (half == 0 ? b.plain : b.shifted)[size_t(i)] = *a;
        }
    }
    return b;
}

// SDL's spelling first — that is what gets written back. Anything Qt's own PortableText would
// produce for the same key follows as an alias, so a hand-edited remorarc using either spelling
// still parses.
namespace {
struct NamedKey {
    const char *name;
    int qtKey;
    bool canonical;  // false = accepted on read, never written
};
const NamedKey kNamedKeys[] = {
    {"Escape", Qt::Key_Escape, true},        {"Esc", Qt::Key_Escape, false},
    {"Tab", Qt::Key_Tab, true},              {"Backspace", Qt::Key_Backspace, true},
    {"Return", Qt::Key_Return, true},        {"Enter", Qt::Key_Enter, false},
    {"Space", Qt::Key_Space, true},          {"Insert", Qt::Key_Insert, true},
    {"Ins", Qt::Key_Insert, false},          {"Delete", Qt::Key_Delete, true},
    {"Del", Qt::Key_Delete, false},          {"Home", Qt::Key_Home, true},
    {"End", Qt::Key_End, true},              {"PageUp", Qt::Key_PageUp, true},
    {"PgUp", Qt::Key_PageUp, false},         {"PageDown", Qt::Key_PageDown, true},
    {"PgDown", Qt::Key_PageDown, false},     {"Left", Qt::Key_Left, true},
    {"Right", Qt::Key_Right, true},          {"Up", Qt::Key_Up, true},
    {"Down", Qt::Key_Down, true},            {"PrintScreen", Qt::Key_Print, true},
    {"Print", Qt::Key_Print, false},         {"Pause", Qt::Key_Pause, true},
    {"Application", Qt::Key_Menu, true},     {"Menu", Qt::Key_Menu, false},
    {"CapsLock", Qt::Key_CapsLock, true},    {"ScrollLock", Qt::Key_ScrollLock, true},
    {"NumLockClear", Qt::Key_NumLock, true}, {"NumLock", Qt::Key_NumLock, false},
};
}  // namespace

std::optional<int> qtKeyFromSdlName(const QString &name) {
    const QString n = name.trimmed();
    if (n.isEmpty()) return std::nullopt;
    for (const NamedKey &k : kNamedKeys)
        if (n.compare(QLatin1String(k.name), Qt::CaseInsensitive) == 0) return k.qtKey;
    // Letters, digits and function keys are contiguous in both spaces, same as androidKeycode().
    if (n.size() == 1) {
        const QChar c = n.at(0).toUpper();
        if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            return Qt::Key_A + (c.unicode() - u'A');
        if (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            return Qt::Key_0 + (c.unicode() - u'0');
        return std::nullopt;
    }
    if (n.at(0).toUpper() == QLatin1Char('F')) {
        bool ok = false;
        const int n2 = QStringView(n).mid(1).toInt(&ok);
        if (ok && n2 >= 1 && n2 <= 24) return Qt::Key_F1 + (n2 - 1);
    }
    return std::nullopt;
}

QString sdlKeyName(int qtKey) {
    for (const NamedKey &k : kNamedKeys)
        if (k.canonical && k.qtKey == qtKey) return QString::fromLatin1(k.name);
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
        return QString(QChar(u'A' + (qtKey - Qt::Key_A)));
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
        return QString(QChar(u'0' + (qtKey - Qt::Key_0)));
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24)
        return QStringLiteral("F%1").arg(qtKey - Qt::Key_F1 + 1);
    return QString();
}

std::optional<QHash<int, BindAction>> parseKeyBind(const QString &spec) {
    QHash<int, BindAction> out;
    if (spec.trimmed().isEmpty()) return out;  // no bindings is a valid answer, not a failure
    const QStringList pairs = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const int colon = pair.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0 || colon != pair.size() - 2) return std::nullopt;
        const auto key = qtKeyFromSdlName(pair.left(colon));
        const auto act = actionFromChar(pair.at(colon + 1));
        if (!key || !act) return std::nullopt;
        // '+' and '-' describe what to do with a CLICK; a key that does neither is just unbound,
        // so accepting them here would silently mean something they cannot mean.
        if (*act == BindAction::PassThrough || *act == BindAction::Ignore) return std::nullopt;
        out.insert(*key, *act);
    }
    return out;
}

QList<Qt::KeyboardModifiers> defaultShortcutMods() {
    return {Qt::AltModifier, Qt::MetaModifier};  // the fork's lalt,lsuper
}

std::optional<QList<Qt::KeyboardModifiers>> parseShortcutMod(const QString &spec) {
    if (spec.trimmed().isEmpty()) return defaultShortcutMods();
    QList<Qt::KeyboardModifiers> out;
    for (const QString &alt : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        Qt::KeyboardModifiers mods;
        for (const QString &tok : alt.split(QLatin1Char('+'), Qt::SkipEmptyParts)) {
            const QString t = tok.trimmed().toLower();
            if (t == QLatin1String("lctrl") || t == QLatin1String("rctrl"))
                mods |= Qt::ControlModifier;
            else if (t == QLatin1String("lalt") || t == QLatin1String("ralt"))
                mods |= Qt::AltModifier;
            else if (t == QLatin1String("lsuper") || t == QLatin1String("rsuper"))
                mods |= Qt::MetaModifier;
            else
                return std::nullopt;
        }
        if (mods == Qt::NoModifier) return std::nullopt;
        if (!out.contains(mods)) out.append(mods);  // lalt,ralt collapse to one Alt
    }
    return out;
}

std::optional<BindAction> chordAction(int qtKey) {
    switch (qtKey) {
        case Qt::Key_B: return BindAction::Back;
        case Qt::Key_H: return BindAction::Home;
        case Qt::Key_S: return BindAction::AppSwitch;
        case Qt::Key_N: return BindAction::Notifications;
        default: return std::nullopt;
    }
}

qint32 slotButton(int slot) {
    switch (slot) {
        case kSlotRight: return kButtonSecondary;
        case kSlotMiddle: return kButtonTertiary;
        case kSlotSide4: return kButtonBack;
        case kSlotSide5: return kButtonForward;
        default: return 0;
    }
}

QPoint mapToVideo(QPoint widgetPos, QSize widgetSize, QSize videoSize) {
    if (widgetSize.isEmpty() || videoSize.isEmpty()) return {0, 0};
    const auto scale = [](int v, int from, int to) {
        int r = int(qint64(v) * to / from);
        if (r < 0) r = 0;
        if (r >= to) r = to - 1;
        return r;
    };
    return {scale(widgetPos.x(), widgetSize.width(), videoSize.width()),
            scale(widgetPos.y(), widgetSize.height(), videoSize.height())};
}

}  // namespace remora::mirror
