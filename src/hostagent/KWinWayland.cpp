#include "KWinWayland.h"

#include <QDBusInterface>
#include <QDBusReply>
#include <QSocketNotifier>

#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fake-input-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

namespace remora::hostagent {

namespace {
// Rendered into the frames rather than sent as metadata: a phone showing a desktop app in pointer
// mode needs to see where the pointer is, and the encoder has no cursor plane to composite.
constexpr uint32_t kPointerEmbedded = ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED;
}  // namespace

// --- VirtualOutput --------------------------------------------------------------------------------

VirtualOutput::VirtualOutput(KWinWayland *wl, const QString &name, QSize pixels)
    : QObject(wl), wl_(wl), name_(name), pixels_(pixels) {}

VirtualOutput::~VirtualOutput() {
    if (stream_) {
        zkde_screencast_stream_unstable_v1_close(stream_);
        wl_->flush();
    }
}

void VirtualOutput::onClosed(void *data, zkde_screencast_stream_unstable_v1 *) {
    auto *self = static_cast<VirtualOutput *>(data);
    // KWin destroys its end after this event; ours must not send close on top of it.
    zkde_screencast_stream_unstable_v1_destroy(self->stream_);
    self->stream_ = nullptr;
    emit self->closed();
}

// Before version 6 the stream announced a node id instead of a serial. Ids are reused and serials
// are not, which is why v6 changed it — but an id still resolves while the node lives, and
// PW_KEY_TARGET_OBJECT accepts either.
void VirtualOutput::onCreated(void *data, zkde_screencast_stream_unstable_v1 *, uint32_t node) {
    auto *self = static_cast<VirtualOutput *>(data);
    if (self->wl_->screencastVersion_ >= 6 || self->settled_) return;
    self->settled_ = true;
    emit self->ready(node);
}

void VirtualOutput::onFailed(void *data, zkde_screencast_stream_unstable_v1 *, const char *error) {
    auto *self = static_cast<VirtualOutput *>(data);
    self->settled_ = true;
    emit self->failed(QString::fromUtf8(error));
}

void VirtualOutput::onSerial(void *data, zkde_screencast_stream_unstable_v1 *, uint32_t hi,
                             uint32_t lo) {
    auto *self = static_cast<VirtualOutput *>(data);
    if (self->settled_) return;
    self->settled_ = true;
    emit self->ready((quint64(hi) << 32) | lo);
}

// --- KWinWayland ----------------------------------------------------------------------------------

KWinWayland::KWinWayland(QObject *parent) : QObject(parent) {}

KWinWayland::~KWinWayland() {
    // Children first: each VirtualOutput closes its stream on this connection.
    qDeleteAll(findChildren<VirtualOutput *>(Qt::FindDirectChildrenOnly));
    if (fakeInput_) {
        if (fakeInputVersion_ >= ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION)
            org_kde_kwin_fake_input_destroy(fakeInput_);
        else
            wl_proxy_destroy(reinterpret_cast<wl_proxy *>(fakeInput_));
    }
    if (screencast_) zkde_screencast_unstable_v1_destroy(screencast_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (seat_) wl_seat_destroy(seat_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

bool KWinWayland::connectToCompositor(QString *error) {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        *error = QStringLiteral("no Wayland compositor to connect to (WAYLAND_DISPLAY is %1)")
                     .arg(qEnvironmentVariableIsSet("WAYLAND_DISPLAY")
                              ? qEnvironmentVariable("WAYLAND_DISPLAY")
                              : QStringLiteral("unset"));
        return false;
    }
    // Listener tables live in the members that register them: the callbacks are private.
    static const wl_registry_listener registryListener = {&onGlobal, &onGlobalRemove};
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener, this);
    // Blocking, once, at startup: the globals must be known before anything is asked of them.
    if (wl_display_roundtrip(display_) < 0) {
        *error = QStringLiteral("the Wayland connection failed during setup");
        return false;
    }
    if (!screencast_) {
        *error = QStringLiteral("KWin did not offer zkde_screencast_unstable_v1 to this program");
        return false;
    }
    if (screencastVersion_ < ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_VIRTUAL_OUTPUT_WITH_DESCRIPTION_SINCE_VERSION) {
        *error = QStringLiteral("KWin's screencast interface is version %1; virtual outputs with a "
                                "description need 4 (Plasma 6)")
                     .arg(screencastVersion_);
        return false;
    }
    if (fakeInput_) {
        org_kde_kwin_fake_input_authenticate(fakeInput_, "Remora",
                                             "Shows this computer's apps on a paired phone");
    }
    notifier_ = new QSocketNotifier(wl_display_get_fd(display_), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &KWinWayland::dispatch);
    flush();
    return true;
}

void KWinWayland::onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface,
                           uint32_t version) {
    auto *self = static_cast<KWinWayland *>(data);
    const QByteArray iface(interface);
    if (iface == zkde_screencast_unstable_v1_interface.name) {
        self->screencastVersion_ =
            std::min<uint32_t>(version, zkde_screencast_unstable_v1_interface.version);
        self->screencast_ = static_cast<zkde_screencast_unstable_v1 *>(wl_registry_bind(
            registry, name, &zkde_screencast_unstable_v1_interface, self->screencastVersion_));
    } else if (iface == org_kde_kwin_fake_input_interface.name) {
        self->fakeInputVersion_ =
            std::min<uint32_t>(version, org_kde_kwin_fake_input_interface.version);
        self->fakeInput_ = static_cast<org_kde_kwin_fake_input *>(wl_registry_bind(
            registry, name, &org_kde_kwin_fake_input_interface, self->fakeInputVersion_));
    } else if (iface == wl_seat_interface.name && !self->seat_) {
        self->seat_ = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min<uint32_t>(version, 5)));
        static const wl_seat_listener seatListener = {
            &onSeatCapabilities, [](void *, wl_seat *, const char *) {}};
        wl_seat_add_listener(self->seat_, &seatListener, self);
    }
}

void KWinWayland::onGlobalRemove(void *, wl_registry *, uint32_t) {}

void KWinWayland::onSeatCapabilities(void *data, wl_seat *seat, uint32_t capabilities) {
    auto *self = static_cast<KWinWayland *>(data);
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) || self->keyboard_) return;
    self->keyboard_ = wl_seat_get_keyboard(seat);
    // Only the keymap matters; the rest is for a focused client, which this never is.
    static const wl_keyboard_listener keyboardListener = {
        &onKeymap,
        [](void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {},
        [](void *, wl_keyboard *, uint32_t, wl_surface *) {},
        [](void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void *, wl_keyboard *, int32_t, int32_t) {},
    };
    wl_keyboard_add_listener(self->keyboard_, &keyboardListener, self);
}

// Sent on binding and again whenever the desk's layouts change.
void KWinWayland::onKeymap(void *data, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size) {
    auto *self = static_cast<KWinWayland *>(data);
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 1) {
        void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            // NUL-terminated by the protocol; the terminator is not part of the text.
            if (!self->keymap_.load(QByteArray(static_cast<const char *>(map), int(size) - 1)))
                qWarning("host agent: could not read the desk's keymap; typed capitals may lose "
                         "their Shift");
            munmap(map, size);
        }
    }
    close(fd);
}

// The layout KWin types on: the one active at the desk. One layout, and any failure, is 0.
quint32 KWinWayland::currentLayout() const {
    QDBusInterface layouts(QStringLiteral("org.kde.keyboard"), QStringLiteral("/Layouts"),
                           QStringLiteral("org.kde.KeyboardLayouts"));
    const QDBusReply<uint> index = layouts.call(QStringLiteral("getLayout"));
    return index.isValid() ? index.value() : 0;
}

void KWinWayland::dispatch() {
    if (wl_display_dispatch(display_) < 0) {
        notifier_->setEnabled(false);
        emit disconnected(QStringLiteral("the Wayland connection to KWin closed"));
        return;
    }
    flush();
}

void KWinWayland::flush() {
    if (display_) wl_display_flush(display_);
}

VirtualOutput *KWinWayland::createVirtualOutput(const QString &name, const QString &description,
                                                QSize pixels) {
    auto *out = new VirtualOutput(this, name, pixels);
    out->stream_ = zkde_screencast_unstable_v1_stream_virtual_output_with_description(
        screencast_, name.toUtf8().constData(), description.toUtf8().constData(),
        pixels.width(), pixels.height(), wl_fixed_from_double(1.0), kPointerEmbedded);
    static const zkde_screencast_stream_unstable_v1_listener streamListener = {
        &VirtualOutput::onClosed, &VirtualOutput::onCreated, &VirtualOutput::onFailed,
        &VirtualOutput::onSerial};
    zkde_screencast_stream_unstable_v1_add_listener(out->stream_, &streamListener, out);
    flush();
    return out;
}

void KWinWayland::pointerMotionAbsolute(QPointF global) {
    if (!fakeInput_) return;
    org_kde_kwin_fake_input_pointer_motion_absolute(fakeInput_, wl_fixed_from_double(global.x()),
                                                    wl_fixed_from_double(global.y()));
}

void KWinWayland::pointerButton(quint32 button, bool pressed) {
    if (!fakeInput_) return;
    org_kde_kwin_fake_input_button(fakeInput_, button, pressed ? 1 : 0);
}

void KWinWayland::pointerAxis(bool horizontal, double value) {
    if (!fakeInput_) return;
    // wl_pointer axis ids: 0 vertical, 1 horizontal.
    org_kde_kwin_fake_input_axis(fakeInput_, horizontal ? 1 : 0, wl_fixed_from_double(value));
}

void KWinWayland::touchDown(quint32 id, QPointF global) {
    if (!fakeInput_) return;
    org_kde_kwin_fake_input_touch_down(fakeInput_, id, wl_fixed_from_double(global.x()),
                                       wl_fixed_from_double(global.y()));
}

void KWinWayland::touchMotion(quint32 id, QPointF global) {
    if (!fakeInput_) return;
    org_kde_kwin_fake_input_touch_motion(fakeInput_, id, wl_fixed_from_double(global.x()),
                                         wl_fixed_from_double(global.y()));
}

void KWinWayland::touchUp(quint32 id) {
    if (fakeInput_) org_kde_kwin_fake_input_touch_up(fakeInput_, id);
}

void KWinWayland::touchFrame() {
    if (fakeInput_) org_kde_kwin_fake_input_touch_frame(fakeInput_);
}

void KWinWayland::key(quint32 evdevKey, bool pressed) {
    if (!fakeInput_ || fakeInputVersion_ < ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION)
        return;
    if (pressed) pressedKeys_.insert(evdevKey);
    else pressedKeys_.remove(evdevKey);
    org_kde_kwin_fake_input_keyboard_key(fakeInput_, evdevKey, pressed ? 1 : 0);
}

bool KWinWayland::keysym(quint32 keysym, bool pressed) {
    if (!fakeInput_ || fakeInputVersion_ < ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEYSYM_SINCE_VERSION)
        return false;
    const bool keys = fakeInputVersion_ >= ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION;
    if (pressed) {
        const quint32 layout = currentLayout();
        Typed t{keymap_.spelledOn(keysym, layout), {}};
        // A modifier the session already holds (a hardware Shift on the phone) stays its own.
        if (keys)
            for (const quint32 m : keymap_.modifierKeysFor(t.sent, layout))
                if (!pressedKeys_.contains(m)) t.held << m;
        for (const quint32 m : t.held) org_kde_kwin_fake_input_keyboard_key(fakeInput_, m, 1);
        org_kde_kwin_fake_input_keyboard_keysym(fakeInput_, t.sent, 1);
        typed_.insert(keysym, t);
        return true;
    }
    const Typed t = typed_.take(keysym);
    org_kde_kwin_fake_input_keyboard_keysym(fakeInput_, t.sent ? t.sent : keysym, 0);
    for (auto m = t.held.crbegin(); m != t.held.crend(); ++m)
        org_kde_kwin_fake_input_keyboard_key(fakeInput_, *m, 0);
    return true;
}

QString grantHint(const QString &executable) {
    return QStringLiteral("KWin grants these interfaces to a program whose desktop entry names "
                          "them: io.github.EtherAura.Remora.HostAgent.desktop, which the install "
                          "puts in share/applications, with Exec=%1 (then run kbuildsycoca6)")
        .arg(executable);
}

}  // namespace remora::hostagent
