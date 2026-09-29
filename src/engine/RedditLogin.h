#pragma once
#include <QString>

#include "engine/Engine.h"

namespace remora {

// Log the official Reddit app in as `username` inside the target container, from `cookie` (the raw
// reddit_session JWT — no "reddit_session=" prefix). Root-free at the SOURCE: the cookie comes from
// a plain web login. The container writes use `docker exec` (container root) because the app's data
// dir is app-owned and adb root is disabled. Proven approach — bd remora-6zj / remora-9lj.
//
// Steps: build the DataStore artifacts (pure core) → force-stop → write the datastore files →
// register the c3d4-style AccountManager row (stop framework, offline-edit the account db since the
// container's own sqlite3 crashes, restart) → re-arm the LOGGED_IN pointer → launch. The app mints a
// fresh access token from the cookie on first launch.
//
// Returns true on success. `log` receives progress lines.
bool injectRedditSession(Spawner &sp, const RunContext &ctx, const QString &username,
                         const QString &cookie, const LineSink &log = {});

}  // namespace remora
