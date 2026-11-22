#include "HostInput.h"

#include <QHash>

#include <algorithm>
#include <utility>

namespace remora::hostinput {

namespace {

// android.view.MotionEvent actions, as the protocol carries them.
constexpr quint8 kDown = 0, kUp = 1, kMove = 2, kCancel = 3, kPointerDown = 5, kPointerUp = 6,
                 kButtonPress = 11, kButtonRelease = 12;
// linux/input-event-codes.h
constexpr quint32 kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112, kBtnSide = 0x113,
                  kBtnExtra = 0x114;
constexpr quint32 kKeyBack = 158;
// One wheel notch, in wl_pointer axis units — libinput's convention, which apps scale by.
constexpr double kAxisPerNotch = 15.0;

// MotionEvent.BUTTON_* bits → evdev buttons.
QList<quint32> evdevButtons(qint32 mask) {
    QList<quint32> out;
    if (mask & 1) out << kBtnLeft;
    if (mask & 2) out << kBtnRight;
    if (mask & 4) out << kBtnMiddle;
    if (mask & 8) out << kBtnSide;
    if (mask & 16) out << kBtnExtra;
    return out;
}

}  // namespace

std::optional<quint32> evdevKey(qint32 k) {
    // KEYCODE_A..Z (29..54) and KEYCODE_0..9 (7..16) are runs on the Android side but not on the
    // evdev side, which follows the physical QWERTY rows.
    static const quint32 letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    if (k >= 29 && k <= 54) return letters[k - 29];
    if (k == 7) return 11;                       // KEY_0
    if (k >= 8 && k <= 16) return quint32(k - 6);  // KEY_1..KEY_9 = 2..10
    if (k >= 131 && k <= 140) return quint32(59 + k - 131);  // F1..F10
    static const QHash<qint32, quint32> keys = {
        {4, kKeyBack},   // BACK
        {19, 103},       // DPAD_UP
        {20, 108},       // DPAD_DOWN
        {21, 105},       // DPAD_LEFT
        {22, 106},       // DPAD_RIGHT
        {24, 115},       // VOLUME_UP
        {25, 114},       // VOLUME_DOWN
        {55, 51},        // COMMA
        {56, 52},        // PERIOD
        {57, 56},        // ALT_LEFT
        {58, 100},       // ALT_RIGHT
        {59, 42},        // SHIFT_LEFT
        {60, 54},        // SHIFT_RIGHT
        {61, 15},        // TAB
        {62, 57},        // SPACE
        {66, 28},        // ENTER
        {67, 14},        // DEL (backspace)
        {68, 41},        // GRAVE
        {69, 12},        // MINUS
        {70, 13},        // EQUALS
        {71, 26},        // LEFT_BRACKET
        {72, 27},        // RIGHT_BRACKET
        {73, 43},        // BACKSLASH
        {74, 39},        // SEMICOLON
        {75, 40},        // APOSTROPHE
        {76, 53},        // SLASH
        {82, 127},       // MENU → KEY_COMPOSE, the context-menu key
        {85, 164},       // MEDIA_PLAY_PAUSE
        {92, 104},       // PAGE_UP
        {93, 109},       // PAGE_DOWN
        {111, 1},        // ESCAPE
        {112, 111},      // FORWARD_DEL
        {113, 29},       // CTRL_LEFT
        {114, 97},       // CTRL_RIGHT
        {115, 58},       // CAPS_LOCK
        {116, 70},       // SCROLL_LOCK
        {117, 125},      // META_LEFT
        {118, 126},      // META_RIGHT
        {120, 99},       // SYSRQ
        {121, 119},      // BREAK
        {122, 102},      // MOVE_HOME
        {123, 107},      // MOVE_END
        {124, 110},      // INSERT
        {141, 87},       // F11
        {142, 88},       // F12
        {143, 69},       // NUM_LOCK
        {164, 113},      // VOLUME_MUTE
    };
    const auto it = keys.constFind(k);
    if (it == keys.constEnd()) return std::nullopt;
    return *it;
}

std::optional<quint32> keysymFor(char32_t c) {
    switch (c) {
        case U'\n': case U'\r': return 0xff0d;  // Return
        case U'\t': return 0xff09;               // Tab
        case U'\b': return 0xff08;               // BackSpace
        default: break;
    }
    if (c < 0x20 || c == 0x7f) return std::nullopt;  // other controls type nothing
    if (c <= 0xff) return quint32(c);                 // Latin-1 keysyms are the codepoints
    if (c > 0x10ffff) return std::nullopt;
    return 0x01000000u | quint32(c);
}

QPointF Translator::global(mirror::FramePoint p) const {
    return {output_.x() + p.x * output_.width(), output_.y() + p.y * output_.height()};
}

QList<InputOp> Translator::translate(const mirror::ControlMessage &m) {
    QList<InputOp> ops;
    switch (m.type) {
        case mirror::ControlRecord::Pointer:
            return m.tool == mirror::PointerTool::Mouse ? pointer(m) : touch(m);
        case mirror::ControlRecord::Scroll: {
            // The wheel turns whatever is under the pointer, so the pointer goes there first.
            InputOp move{InputOp::PointerMotion, global(m.at)};
            ops << move;
            // Android's axes count notches up and left as positive; Wayland's run down and right.
            if (m.vScroll != 0)
                ops << InputOp{InputOp::PointerAxis, {}, 0, false, false, -m.vScroll * kAxisPerNotch};
            if (m.hScroll != 0)
                ops << InputOp{InputOp::PointerAxis, {}, 0, false, true, -m.hScroll * kAxisPerNotch};
            return ops;
        }
        case mirror::ControlRecord::Key: {
            const auto code = evdevKey(m.keycode);
            if (!code) return ops;
            const bool down = m.action == 0;
            if (down) keys_.insert(*code);
            else keys_.remove(*code);
            ops << InputOp{InputOp::Key, {}, *code, down};
            return ops;
        }
        case mirror::ControlRecord::Back: {
            const bool down = m.action == 0;
            if (down) keys_.insert(kKeyBack);
            else keys_.remove(kKeyBack);
            ops << InputOp{InputOp::Key, {}, kKeyBack, down};
            return ops;
        }
        case mirror::ControlRecord::Text: {
            const auto codepoints = m.text.toUcs4();
            for (const char32_t c : codepoints) {
                const auto sym = keysymFor(c);
                if (!sym) continue;
                ops << InputOp{InputOp::Keysym, {}, *sym, true}
                    << InputOp{InputOp::Keysym, {}, *sym, false};
            }
            return ops;
        }
        default:
            return ops;  // panels, clipboard and resize are the session's, not input
    }
}

QList<InputOp> Translator::pointer(const mirror::ControlMessage &m) {
    QList<InputOp> ops;
    ops << InputOp{InputOp::PointerMotion, global(m.at)};
    // The buttons the message says are held after it, adjusted by its own action: a DOWN or
    // BUTTON_PRESS holds its actionButton, an UP or BUTTON_RELEASE lets it go, CANCEL lets all go.
    QSet<quint32> want;
    for (const quint32 b : evdevButtons(m.buttons)) want.insert(b);
    const QList<quint32> act = evdevButtons(m.actionButton ? m.actionButton : 1);
    if (m.action == kDown || m.action == kButtonPress) {
        for (const quint32 b : act) want.insert(b);
    } else if (m.action == kUp || m.action == kButtonRelease) {
        for (const quint32 b : act) want.remove(b);
    } else if (m.action == kCancel) {
        want.clear();
    }
    // Releases before presses, each in a fixed order, so the sequence is deterministic.
    QList<quint32> held(buttons_.begin(), buttons_.end()), next(want.begin(), want.end());
    std::sort(held.begin(), held.end());
    std::sort(next.begin(), next.end());
    for (const quint32 b : held)
        if (!want.contains(b)) ops << InputOp{InputOp::PointerButton, {}, b, false};
    for (const quint32 b : next)
        if (!buttons_.contains(b)) ops << InputOp{InputOp::PointerButton, {}, b, true};
    buttons_ = want;
    return ops;
}

QList<InputOp> Translator::touch(const mirror::ControlMessage &m) {
    QList<InputOp> ops;
    const quint32 id = m.pointerId;
    switch (m.action) {
        case kDown:
        case kPointerDown:
            touches_.insert(id);
            ops << InputOp{InputOp::TouchDown, global(m.at), id};
            break;
        case kMove:
            if (!touches_.contains(id)) return ops;  // a move for a finger that never landed
            ops << InputOp{InputOp::TouchMotion, global(m.at), id};
            break;
        case kUp:
        case kPointerUp:
            if (!touches_.remove(id)) return ops;
            ops << InputOp{InputOp::TouchUp, {}, id};
            break;
        case kCancel: {
            QList<quint32> ids(touches_.begin(), touches_.end());
            std::sort(ids.begin(), ids.end());
            for (const quint32 t : ids) ops << InputOp{InputOp::TouchUp, {}, t};
            touches_.clear();
            break;
        }
        default:
            return ops;  // hover and the rest carry nothing for a finger
    }
    ops << InputOp{InputOp::TouchFrame};
    return ops;
}

QList<InputOp> Translator::releaseAll() {
    QList<InputOp> ops;
    auto sorted = [](const QSet<quint32> &s) {
        QList<quint32> l(s.begin(), s.end());
        std::sort(l.begin(), l.end());
        return l;
    };
    for (const quint32 b : sorted(buttons_)) ops << InputOp{InputOp::PointerButton, {}, b, false};
    for (const quint32 t : sorted(touches_)) ops << InputOp{InputOp::TouchUp, {}, t};
    if (!touches_.isEmpty()) ops << InputOp{InputOp::TouchFrame};
    for (const quint32 k : sorted(keys_)) ops << InputOp{InputOp::Key, {}, k, false};
    buttons_.clear();
    touches_.clear();
    keys_.clear();
    return ops;
}

namespace {
bool isKey(const InputOp &op) { return op.kind == InputOp::Key || op.kind == InputOp::Keysym; }
quint64 keyId(const InputOp &op) { return quint64(op.kind) << 32 | op.code; }
}  // namespace

QList<InputOp> KeyGate::pass(const QList<InputOp> &ops) {
    QList<InputOp> now;
    for (const InputOp &op : ops) {
        if (!isKey(op)) {
            now << op;
            continue;
        }
        const quint64 id = keyId(op);
        // Once one key waits, every later one waits behind it: keys must arrive in order, or a
        // modifier's release overtakes the key it modifies.
        if (!held_.isEmpty()) {
            held_ << op;
        } else if (focused_) {
            if (op.pressed) down_.insert(id);
            else down_.remove(id);
            now << op;
        } else if (!op.pressed && down_.remove(id)) {
            now << op;  // its press went to the session's window; nothing typed by releasing it
        } else {
            held_ << op;
        }
    }
    return now;
}

QList<InputOp> KeyGate::focusChanged(bool ours) {
    focused_ = ours;
    return ours ? pass(std::exchange(held_, {})) : QList<InputOp>{};
}

QList<InputOp> KeyGate::expire() {
    QList<InputOp> releases;
    for (const InputOp &op : std::exchange(held_, {}))
        if (!op.pressed && down_.remove(keyId(op))) releases << op;
    return releases;
}

}  // namespace remora::hostinput
