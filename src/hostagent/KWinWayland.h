#pragma once
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QString>

struct wl_display;
struct wl_registry;
struct zkde_screencast_unstable_v1;
struct zkde_screencast_stream_unstable_v1;
struct org_kde_kwin_fake_input;
class QSocketNotifier;

// The host agent's line to KWin (docs/PHONE_APP.md §3): two restricted Wayland
// interfaces, granted to this executable by the X-KDE-Wayland-Interfaces line of its desktop
// entry, so neither asks the desk for consent — nobody is at the desk to give it.
//
//   zkde_screencast_unstable_v1  stream_virtual_output: an output only the stream shows, sized to
//                                the phone, published as a PipeWire node
//   org_kde_kwin_fake_input      pointer, touch and keys, in the compositor's global coordinates
//
// Plain libwayland-client, dispatched from the Qt event loop: one connection, one thread.

namespace remora::hostagent {

class KWinWayland;

// One virtual output and the stream that shows it. KWin removes the output when the stream
// closes, which is what destroying this does.
class VirtualOutput : public QObject {
    Q_OBJECT
public:
    ~VirtualOutput() override;
    // The connector name KWin gives it: "Virtual-" + the name asked for.
    QString outputName() const { return QStringLiteral("Virtual-") + name_; }
    QSize pixelSize() const { return pixels_; }

signals:
    // The PipeWire object serial of the stream's node — what PW_KEY_TARGET_OBJECT takes.
    void ready(quint64 pipewireSerial);
    void failed(const QString &why);
    void closed();

private:
    friend class KWinWayland;
    VirtualOutput(KWinWayland *wl, const QString &name, QSize pixels);

    static void onClosed(void *data, zkde_screencast_stream_unstable_v1 *);
    static void onCreated(void *data, zkde_screencast_stream_unstable_v1 *, uint32_t node);
    static void onFailed(void *data, zkde_screencast_stream_unstable_v1 *, const char *error);
    static void onSerial(void *data, zkde_screencast_stream_unstable_v1 *, uint32_t hi,
                         uint32_t lo);

    KWinWayland *wl_;
    zkde_screencast_stream_unstable_v1 *stream_ = nullptr;
    QString name_;
    QSize pixels_;
    bool settled_ = false;
};

class KWinWayland : public QObject {
    Q_OBJECT
public:
    explicit KWinWayland(QObject *parent = nullptr);
    ~KWinWayland() override;

    // Connects to $WAYLAND_DISPLAY and binds both interfaces. False with *error naming what is
    // missing — most often the desktop entry that grants them (see grantHint()).
    bool connectToCompositor(QString *error);
    bool hasFakeInput() const { return fakeInput_ != nullptr; }

    // `pixels` is the output's mode and the stream's frame size. The protocol calls these
    // logical, but KWin (6.8, measured) makes them the mode, and it ignores the scale it is sent
    // in favour of what it saved for the last EDID-less output — so the scale is DeskLayout's to
    // apply, not this request's. The name must be unique among live outputs.
    VirtualOutput *createVirtualOutput(const QString &name, const QString &description,
                                       QSize pixels);

    // Fake input, in global logical coordinates. evdev codes (linux/input-event-codes.h).
    void pointerMotionAbsolute(QPointF global);
    void pointerButton(quint32 button, bool pressed);
    void pointerAxis(bool horizontal, double value);
    void touchDown(quint32 id, QPointF global);
    void touchMotion(quint32 id, QPointF global);
    void touchUp(quint32 id);
    void touchFrame();
    void key(quint32 evdevKey, bool pressed);
    // A keysym KWin maps onto the active layout itself (fake_input v6); false when unavailable.
    bool keysym(quint32 keysym, bool pressed);

    void flush();

signals:
    void disconnected(const QString &why);

private:
    friend class VirtualOutput;
    static void onGlobal(void *data, wl_registry *, uint32_t name, const char *interface,
                         uint32_t version);
    static void onGlobalRemove(void *data, wl_registry *, uint32_t name);
    void dispatch();

    wl_display *display_ = nullptr;
    wl_registry *registry_ = nullptr;
    zkde_screencast_unstable_v1 *screencast_ = nullptr;
    org_kde_kwin_fake_input *fakeInput_ = nullptr;
    uint32_t screencastVersion_ = 0, fakeInputVersion_ = 0;
    QSocketNotifier *notifier_ = nullptr;
};

// What to tell the operator when the restricted interfaces are withheld.
QString grantHint(const QString &executable);

}  // namespace remora::hostagent
