#pragma once
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <optional>

#include "MirrorProto.h"

// Protocol v3 control messages, turned into the input a desktop compositor takes — the host
// agent's half of "use a Linux app from the phone" (docs/PHONE_APP.md §3). Pure: a
// message and the output's geometry in, a list of operations out; the agent replays them through
// KWin's fake input. Coordinates leave here GLOBAL and logical, because that is what fake input
// takes: the output's position on the desk plus the frame fraction times its size.

namespace remora::hostinput {

struct InputOp {
    enum Kind { PointerMotion, PointerButton, PointerAxis, TouchDown, TouchMotion, TouchUp,
                TouchFrame, Key, Keysym };
    Kind kind = PointerMotion;
    QPointF at;            // PointerMotion, TouchDown, TouchMotion
    quint32 code = 0;      // PointerButton: BTN_*; Key: KEY_*; Keysym: an X keysym; Touch*: id
    bool pressed = false;  // PointerButton, Key, Keysym
    bool horizontal = false;  // PointerAxis
    double value = 0;         // PointerAxis, in wl_pointer axis units (15 per wheel notch)

    bool operator==(const InputOp &) const = default;
};

// The evdev key code for an Android KeyEvent keycode, for the keys a desktop app understands.
std::optional<quint32> evdevKey(qint32 androidKeycode);

// The X keysym that types this character: Latin-1 maps to itself, other Unicode to
// 0x01000000 + codepoint, and the control characters a text field acts on to their function keys.
std::optional<quint32> keysymFor(char32_t c);

class Translator {
public:
    // The output the session's frames show, in global logical coordinates.
    void setOutput(QRectF geometry) { output_ = geometry; }
    QRectF output() const { return output_; }

    QList<InputOp> translate(const mirror::ControlMessage &m);

    // Everything still held — buttons, touches, keys — released, for a session ending mid-press:
    // a button left down on the desk would follow the user's real pointer around.
    QList<InputOp> releaseAll();

private:
    QPointF global(mirror::FramePoint p) const;
    QList<InputOp> pointer(const mirror::ControlMessage &m);
    QList<InputOp> touch(const mirror::ControlMessage &m);

    QRectF output_;
    QSet<quint32> buttons_, touches_, keys_;
};

// Keys go only to the session's own window. Fake input types into whatever window has keyboard
// focus, and the phone shares the desk's one seat: a window activated at the desk, by the person
// there or by a program, took the phone's next keystrokes — seen in testing, a digit typed on the
// phone never reached its calculator because a desk window had focus. So a key that arrives while
// another window has focus is held until the session's window has it again (the agent asks KWin
// to activate it), and dropped if that does not happen. Everything else passes: a pointer or a
// touch goes to the virtual output by position, and a click there focuses the app by itself.
class KeyGate {
public:
    // The ops to inject now, in order; keys that must wait are kept. holding() then says so.
    QList<InputOp> pass(const QList<InputOp> &ops);
    // The session's window gained (true) or lost keyboard focus; on gaining it, the keys held.
    QList<InputOp> focusChanged(bool ours);
    // Focus did not come back: the held keys are dropped, never typed into another window. What
    // comes back is only the releases of keys the session's window had pressed, which must not
    // stay down in the seat.
    QList<InputOp> expire();

    bool holding() const { return !held_.isEmpty(); }

private:
    bool focused_ = false;
    QList<InputOp> held_;
    QSet<quint64> down_;  // keys and keysyms pressed into the session's window, not yet released
};

}  // namespace remora::hostinput
