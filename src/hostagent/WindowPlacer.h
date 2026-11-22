#pragma once
#include <QHash>
#include <QObject>
#include <QPair>
#include <QRect>
#include <QSet>
#include <QString>

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

    // Starts placing `appId`'s next window on `output` for `session`; call before launching the
    // app, so its first window cannot arrive before the script is listening.
    bool watch(quint32 session, const QString &appId, const QString &output, QString *error);
    // For a launch nothing answered: take the app's window if it is the active one. A
    // single-instance app that is already active raises nothing, so no other signal comes.
    void adoptIfUnplaced(quint32 session);
    // The session's output changed (a reflow): its windows move to `output`, and later ones go
    // there too.
    void moveTo(quint32 session, const QString &output);
    // Gives the session's window keyboard focus (the one that had it last); focusChanged follows.
    void activate(quint32 session);
    // Stops placing for `session` and gives its windows back to the desk: one that was already
    // there goes back where it was; one opened for the phone is minimized, never closed.
    void release(quint32 session);

    // Called by the session's KWin script (D-Bus), each with one JSON object.
public slots:
    Q_SCRIPTABLE void placed(const QString &json);
    Q_SCRIPTABLE void windowClosed(const QString &json);
    Q_SCRIPTABLE void focus(const QString &json);

signals:
    void windowPlaced(quint32 session, const QString &windowId, int pid, bool isNew);
    void allWindowsClosed(quint32 session);
    // Keyboard focus went to one of the session's windows (true) or to any other (false).
    void focusChanged(quint32 session, bool ours);

private:
    QString placeScript(quint32 session, bool adopt, const QStringList &move = {}) const;
    bool runScript(const QString &name, const QString &source, QString *error);
    void runOnce(const QString &name, const QString &source);  // a script that only acts once
    void unloadScript(const QString &name);

    struct Placed {
        bool isNew = true;
        QRect was;           // where a window already on the desk was
        bool border = true;
        bool allDesktops = false;
    };
    QHash<quint32, QHash<QString, Placed>> windows_;
    QHash<quint32, QPair<QString, QString>> watched_;  // session → (app id, output)
    QHash<quint32, QString> lastFocused_;              // session → its window focused last
    QString scriptDir_;
};

}  // namespace remora::hostagent
