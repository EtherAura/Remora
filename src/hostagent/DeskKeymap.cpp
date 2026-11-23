#include "DeskKeymap.h"

#include <xkbcommon/xkbcommon.h>

namespace remora::hostagent {

namespace {
constexpr quint32 kEvdevOffset = 8;  // xkb keycodes are evdev codes plus 8
constexpr quint32 kLeftShift = 42 + kEvdevOffset, kRightAlt = 100 + kEvdevOffset;
}  // namespace

DeskKeymap::DeskKeymap() : context_(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {}

DeskKeymap::~DeskKeymap() {
    if (keymap_) xkb_keymap_unref(keymap_);
    if (context_) xkb_context_unref(context_);
}

bool DeskKeymap::load(const QByteArray &text) {
    if (!context_) return false;
    xkb_keymap *k = xkb_keymap_new_from_buffer(context_, text.constData(), size_t(text.size()),
                                               XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!k) return false;
    if (keymap_) xkb_keymap_unref(keymap_);
    keymap_ = k;
    return true;
}

bool DeskKeymap::loadNames(const char *layout, const char *variant) {
    if (!context_) return false;
    const xkb_rule_names names{nullptr, nullptr, layout, variant, nullptr};
    xkb_keymap *k = xkb_keymap_new_from_names(context_, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!k) return false;
    if (keymap_) xkb_keymap_unref(keymap_);
    keymap_ = k;
    return true;
}

// A key whose first level is `keysym`, the preferred one if it is.
quint32 DeskKeymap::keycodeOf(quint32 keysym, quint32 layout, quint32 preferred) const {
    const auto firstLevelIs = [&](xkb_keycode_t key) {
        const xkb_keysym_t *syms = nullptr;
        const int n = xkb_keymap_key_get_syms_by_level(keymap_, key, layout, 0, &syms);
        return n > 0 && syms[0] == keysym;
    };
    if (firstLevelIs(preferred)) return preferred;
    const xkb_keycode_t max = xkb_keymap_max_keycode(keymap_);
    for (xkb_keycode_t key = xkb_keymap_min_keycode(keymap_); key <= max; ++key)
        if (firstLevelIs(key)) return key;
    return 0;
}

bool DeskKeymap::hasKeysym(quint32 keysym, quint32 layout) const {
    const xkb_keycode_t max = xkb_keymap_max_keycode(keymap_);
    for (xkb_keycode_t key = xkb_keymap_min_keycode(keymap_); key < max; ++key) {
        const xkb_level_index_t levels = xkb_keymap_num_levels_for_key(keymap_, key, layout);
        for (xkb_level_index_t l = 0; l < levels; ++l) {
            const xkb_keysym_t *syms = nullptr;
            const int n = xkb_keymap_key_get_syms_by_level(keymap_, key, layout, l, &syms);
            for (int i = 0; i < n; ++i)
                if (syms[i] == keysym) return true;
        }
    }
    return false;
}

quint32 DeskKeymap::spelledOn(quint32 keysym, quint32 layout) const {
    constexpr quint32 kUnicode = 0x01000000;
    if (!keymap_ || keysym < kUnicode || keysym > kUnicode + 0x10ffff) return keysym;
    if (layout >= xkb_keymap_num_layouts(keymap_)) layout = 0;
    if (hasKeysym(keysym, layout)) return keysym;
    const xkb_keysym_t named = xkb_utf32_to_keysym(keysym - kUnicode);
    return named != XKB_KEY_NoSymbol && hasKeysym(named, layout) ? named : keysym;
}

QList<quint32> DeskKeymap::modifierKeysFor(quint32 keysym, quint32 layout) const {
    if (!keymap_) return {};
    if (layout >= xkb_keymap_num_layouts(keymap_)) layout = 0;

    // KWin's own search (Xkb::keycodeFromKeysym), to the letter — its loop stops short of the
    // maximum keycode — so this answers for the key KWin will press.
    xkb_keycode_t target = 0;
    xkb_level_index_t level = 0;
    const xkb_keycode_t max = xkb_keymap_max_keycode(keymap_);
    for (xkb_keycode_t key = xkb_keymap_min_keycode(keymap_); key < max && !target; ++key) {
        const xkb_level_index_t levels = xkb_keymap_num_levels_for_key(keymap_, key, layout);
        for (xkb_level_index_t l = 0; l < levels && !target; ++l) {
            const xkb_keysym_t *syms = nullptr;
            const int n = xkb_keymap_key_get_syms_by_level(keymap_, key, layout, l, &syms);
            for (int i = 0; i < n; ++i)
                if (syms[i] == keysym) {
                    target = key;
                    level = l;
                    break;
                }
        }
    }
    if (!target || level == 0) return {};

    // Which real keys reach that level, asked of the keymap rather than read off modifier masks:
    // Shift, AltGr (ISO_Level3_Shift), or both. A level reached only some other way (Control,
    // a locked group) is left to KWin.
    const quint32 shift = keycodeOf(XKB_KEY_Shift_L, layout, kLeftShift);
    const quint32 altGr = keycodeOf(XKB_KEY_ISO_Level3_Shift, layout, kRightAlt);
    const QList<QList<quint32>> candidates = {{shift}, {altGr}, {shift, altGr}};
    for (const QList<quint32> &keys : candidates) {
        if (keys.contains(0u)) continue;
        xkb_state *state = xkb_state_new(keymap_);
        if (!state) return {};
        xkb_state_update_mask(state, 0, 0, 0, 0, 0, layout);
        for (const quint32 k : keys) xkb_state_update_key(state, k, XKB_KEY_DOWN);
        const bool reaches = xkb_state_key_get_level(state, target, layout) == level;
        xkb_state_unref(state);
        if (reaches) {
            QList<quint32> evdev;
            for (const quint32 k : keys) evdev << k - kEvdevOffset;
            return evdev;
        }
    }
    return {};
}

}  // namespace remora::hostagent
