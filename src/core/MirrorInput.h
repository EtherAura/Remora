#pragma once
#include <QHash>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <Qt>
#include <array>
#include <optional>

// Input translation for the mirror client: Qt events → the Android constants the control
// protocol carries (MirrorProto). Pure lookup tables and arithmetic — the event loop lives in
// src/mirror. Values are AOSP KeyEvent/MotionEvent constants; they are a wire contract with the
// server's input injection, pinned in test_mirrorproto.cpp.

namespace remora::mirror {

// KeyEvent / MotionEvent actions (u8 on the wire).
inline constexpr quint8 kKeyActionDown = 0, kKeyActionUp = 1;
inline constexpr quint8 kMotionActionDown = 0, kMotionActionUp = 1, kMotionActionMove = 2;

// MotionEvent.BUTTON_* bitmask.
inline constexpr qint32 kButtonPrimary = 1, kButtonSecondary = 2, kButtonTertiary = 4;

// AKEYCODE_* the client sends for non-text keys (and letters/digits under a modifier).
inline constexpr qint32 kKeycodeHome = 3, kKeycodeAppSwitch = 187;

// MotionEvent.BUTTON_* for the buttons a pass-through click travels as.
inline constexpr qint32 kButtonBack = 8, kButtonForward = 16;

// Input bindings (`mouse_bind` / `key_bind` / `shortcut_mod`, the Viewer's Navigation card) ------
//
// The action a bound button or key performs. The characters are the ones the card writes and the
// ones a hand-edited remorarc already uses, so this enum IS the file format's vocabulary. The
// pass-through/ignore pair is mouse-only: a key that does neither is simply not bound.
enum class BindAction : quint8 {
    PassThrough,    // '+' — no shortcut: pass the click to Android as that mouse button
    Ignore,         // '-' — swallow it entirely
    Back,           // 'b'
    Home,           // 'h'
    AppSwitch,      // 's' — the card calls it Recents
    Notifications,  // 'n'
};

// The bindable buttons, in `mouse_bind` vector order. Left click is never bindable — it is the
// touch pointer, and a mirror whose tap could be rebound would have no way to tap.
enum MouseSlot { kSlotRight = 0, kSlotMiddle = 1, kSlotSide4 = 2, kSlotSide5 = 3, kSlotCount = 4 };

// "WXYZ:wxyz" — the four buttons unshifted, then the same four with Shift held.
struct MouseBindings {
    std::array<BindAction, kSlotCount> plain{}, shifted{};
    BindAction at(int slot, bool shift) const {
        return shift ? shifted[size_t(slot)] : plain[size_t(slot)];
    }
};

// What an unset mouse_bind resolves to (Resolver.cpp): right=Back, middle=Home, 4th=Recents,
// 5th=Notifications, and every shifted click passed through.
MouseBindings defaultMouseBindings();

// Parses a `mouse_bind` vector. nullopt for anything malformed — a wrong length or an unknown
// character silently rebinds or kills a button, so the caller reports it instead of guessing.
std::optional<MouseBindings> parseMouseBind(const QString &spec);

// The MotionEvent.BUTTON_* a passed-through click on this slot carries.
qint32 slotButton(int slot);

// Key names ---------------------------------------------------------------------------------------
//
// `key_bind` spells keys the way SDL does — the spelling the knob has always used. This table is
// the ONE place that mapping lives: the Viewer's card writes names through it and the client reads
// them back through it, so a name the card can produce is always a name the client honours. (A
// second table in the UI is exactly the drift this project keeps paying for.) Qt Core only — no
// QKeySequence here, that is QtGui.
std::optional<int> qtKeyFromSdlName(const QString &name);  // case-insensitive
QString sdlKeyName(int qtKey);  // empty when the key has no bindable name

// key_bind ----------------------------------------------------------------------------------------
//
// "Escape:b,F2:h" — comma-separated <key>:<action> pairs, no modifier. nullopt when any pair is
// malformed, names an unknown key, or asks for '+'/'-' (which mean nothing for a key).
std::optional<QHash<int, BindAction>> parseKeyBind(const QString &spec);

// shortcut_mod ------------------------------------------------------------------------------------
//
// "lctrl+lalt,lsuper" — comma-separated ALTERNATIVES, each a '+'-joined combination drawn from
// lctrl/rctrl/lalt/ralt/lsuper/rsuper. Shift is never a shortcut modifier: it is what selects a
// mouse_bind's second half.
//
// Qt reports modifiers without a side (Qt::AltModifier, not left-vs-right) and Wayland gives no
// reliable native scancode to recover one, so lalt and ralt both arm Alt here. That is already how
// the built-in Alt+D/Alt+F chords behave; the knob's left/right spelling is accepted (and kept in
// the config) rather than honoured. Empty spec = the default, Alt or Super.
std::optional<QList<Qt::KeyboardModifiers>> parseShortcutMod(const QString &spec);
QList<Qt::KeyboardModifiers> defaultShortcutMods();
// The MOD+letter chords for the four navigation actions.
std::optional<BindAction> chordAction(int qtKey);

// Qt::Key → AKEYCODE_*, for the navigation/editing/function keys plus letters and digits.
// Printable input travels as INJECT_TEXT instead; this map is for everything that isn't text.
std::optional<qint32> androidKeycode(int qtKey);

// Qt modifiers → AMETA_* state. Both the generic and the -LEFT bit are set (Android's
// KeyEvent.normalizeMetaState() expects the pairing).
qint32 androidMetaState(Qt::KeyboardModifiers mods);

// Widget coordinates → video-frame coordinates under stretched fit (the mirror always renders
// stretched: the frame fills the widget on both axes independently).
QPoint mapToVideo(QPoint widgetPos, QSize widgetSize, QSize videoSize);

}  // namespace remora::mirror
