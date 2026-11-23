#pragma once
#include <QByteArray>
#include <QList>

struct xkb_context;
struct xkb_keymap;

namespace remora::hostagent {

// The desk's keymap, as KWin hands it to every Wayland client, for one question: which real
// modifier keys a keysym needs. KWin types a keysym (fake_input v6) by setting the modifier state
// itself around the key, and an input method holding the keyboard (Plasma's plasma-keyboard) learns
// that state only after the key — so the first capital after lowercase arrived lowercase, and the
// lowercase letter after it capital: "PULSE_SINK" typed from a session arrived as "pULSE_SINK".
// Holding the real Shift or AltGr key around the keysym lets the input method see the modifier
// first, as it does when a person types.
class DeskKeymap {
public:
    DeskKeymap();
    ~DeskKeymap();
    DeskKeymap(const DeskKeymap &) = delete;
    DeskKeymap &operator=(const DeskKeymap &) = delete;

    bool load(const QByteArray &text);  // XKB_KEYMAP_FORMAT_TEXT_V1, as wl_keyboard.keymap sends it
    bool loadNames(const char *layout, const char *variant = "");  // from xkeyboard-config
    bool isLoaded() const { return keymap_ != nullptr; }

    // The evdev codes of the modifier keys to hold while `keysym` is typed on `layout`, in press
    // order, for the key and level KWin itself picks (the lowest keycode, then the lowest level,
    // that has it). Empty when it needs none, and when the layout lacks it: KWin then types it on
    // a spare key of its own with no modifiers at all.
    QList<quint32> modifierKeysFor(quint32 keysym, quint32 layout = 0) const;
    // The keysym as `layout` spells it. Text arrives as Unicode keysyms (0x01000000 + code point)
    // for anything past Latin-1, and a layout lists the older named one where there is one — the
    // Euro sign is EuroSign, 0x20ac, on EurKEY's AltGr+5. Sent as Unicode, KWin finds no key for
    // it and swaps in a one-key keymap, and through an input method the character was lost.
    quint32 spelledOn(quint32 keysym, quint32 layout = 0) const;

private:
    quint32 keycodeOf(quint32 keysym, quint32 layout, quint32 preferred) const;
    bool hasKeysym(quint32 keysym, quint32 layout) const;

    xkb_context *context_ = nullptr;
    xkb_keymap *keymap_ = nullptr;
};

}  // namespace remora::hostagent
