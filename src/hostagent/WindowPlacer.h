#pragma once
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QPair>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>

namespace remora::hostagent {

// Puts a session's app on its virtual output, through a KWin script per session (KWin's
// scripting is the one place that can move a window onto an output; Wayland gives a client no
// say over another's windows). The script reports back over D-Bus to this object, which the
// agent exports as org.remora.HostAgent /Placement.
//
// Which window is "the app's" is decided by desktop-file id, never by process id: a
// single-instance app — a browser, most KDE apps — answers a launch with a new window of the
// process already running, so the launched process may own nothing. And an app that only raises
// a window it already has is answered by moving that one.
class WindowPlacer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.remora.HostAgent.Placement")
public:
    explicit WindowPlacer(QObject *parent = nullptr);
    ~WindowPlacer() override;

    // Exports this object on the session bus. False with *error when the name is taken — another
    // agent is running — or there is no session bus.
    bool exportOnBus(QString *error);

    // How the session's windows sit on its output: filling it, maximized and borderless, where
    // tiling scripts leave a window alone and nothing of the desktop shows; or `width` and
    // `height` percent of its sides, centred, with a title bar; or, `desktop`, as any window of
    // the desk's at that size, never maximized, which the desk's tiling scripts arrange as they
    // would any other.
    struct Window {
        int width = 100, height = 100;
        bool desktop = false;
    };
    // Starts placing `appId`'s next window on `output` for `session`; call before launching the
    // app, so its first window cannot arrive before the script is listening.
    bool watch(quint32 session, const QString &appId, const QString &output, Window window,
               QString *error);
    // For a launch nothing answered: take the app's window if it is the active one. A
    // single-instance app that is already active raises nothing, so no other signal comes.
    void adoptIfUnplaced(quint32 session);
    // The session's output changed (a reflow): its windows move to `output`, and later ones go
    // there too.
    void moveTo(quint32 session, const QString &output);
    // The session's windows are placed again as `window` says.
    void setWindow(quint32 session, Window window);
    // Stops placing for `session` and gives its windows back to the desk: one that was already
    // there goes back where it was; one opened for the phone is minimized, never closed.
    void release(quint32 session);

    // Called by the session's KWin script (D-Bus), each with one JSON object.
public slots:
    Q_SCRIPTABLE void placed(const QString &json);
    Q_SCRIPTABLE void windowClosed(const QString &json);
    Q_SCRIPTABLE void focus(const QString &json);
    Q_SCRIPTABLE void pointerEntered(const QString &json);

signals:
    void windowPlaced(quint32 session, const QString &windowId, int pid, bool isNew);
    // The smallest a placed window can be, in logical pixels.
    void windowMinimum(quint32 session, QSize logical);
    void allWindowsClosed(quint32 session);
    // Keyboard focus went to one of the session's windows (true) or to any other (false).
    void focusChanged(quint32 session, bool ours);
    // The pointer came onto the session's output from `deskPos` (global, logical).
    void deskPointer(quint32 session, QPointF deskPos);

private:
    QString placeScript(quint32 session, bool adopt, const QStringList &move = {}) const;
    void replace(quint32 session);  // the session's script again, for every window it placed
    bool runScript(const QString &name, const QString &source, QString *error);
    void runOnce(const QString &name, const QString &source);  // a script that only acts once
    void unloadScript(const QString &name);

    struct Placed {
        bool isNew = true;
        QRect was;           // where a window already on the desk was, unmaximized
        bool border = true;
        bool allDesktops = false;
        bool minimized = false;
        int maximized = 0;   // KWin's MaximizeMode: 1 vertically, 2 horizontally, 3 both
        QStringList activities;  // empty: every activity
    };
    QHash<quint32, QHash<QString, Placed>> windows_;
    struct Watched {
        QString appId, output;
        Window window;
    };
    QHash<quint32, Watched> watched_;
    QString scriptDir_;
};

}  // namespace remora::hostagent
