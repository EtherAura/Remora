#pragma once
#include <QWidget>

#include "core/Backend.h"
#include "core/Config.h"

namespace remora {

// Embedded reddit.com login — the root-free source for the Reddit login feature (bd remora-6zj,
// increment 2). The user signs in in a real WebView (their own password, never seen by Remora);
// Remora reads the reddit_session cookie from that WebView's own cookie jar and the username from an
// authenticated /api/me.json probe, then runs the container transplant (engine/RedditLogin) on a
// worker thread and streams progress. Nothing touches disk: the profile is off-the-record and the
// cookie lives only in memory.
class RedditLoginWindow : public QWidget {
    Q_OBJECT
public:
    RedditLoginWindow(RemoraConfig cfg, Backend backend, QWidget *parent = nullptr);
    ~RedditLoginWindow() override;

private:
    void startTransplant(const QString &username, const QString &cookie);
    void appendLog(const QString &line);
    void onTransplantDone(bool ok, const QString &username);

    struct Impl;
    Impl *d_;
};

// Standalone entry for `remora reddit-login [<profile>]` with no --cookie: builds its own
// QApplication (WebEngine needs one) and shows the login window. Mirrors runGui() (also in
// namespace remora); defined in RedditLoginWindow.cpp.
int runRedditLoginGui(int argc, char **argv);

}  // namespace remora
