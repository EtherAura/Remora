#pragma once

#include "engine/Engine.h"
#include "engine/SelfImage.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace remora {

// One pass of `remora update-watch`, the host half of the in-Android updater, with everything it
// touches injected: main.cpp wires the real profile, catalog, host round trip and deploy, and the
// tests drive the same pass with recorders.

// How often the signed catalog is asked about the official image once it has answered. Until its
// first answer it is asked on every pass.
inline constexpr int kReleaseCheckSecs = 6 * 3600;

// What one pass carries to the next. A fresh process (a start, or the successor after a self-
// restart) begins from the default: no offer, and no catalog answer yet.
struct UpdateWatchState {
    QJsonObject offer;    // the catalog's newer release for the official image, when there is one
    QString offerFor;     // the tag the last answered catalog check was for; empty = no answer yet
    QString lastError;    // the last Apply's failure, shown on the device's update screen
    QDateTime nextCheck;  // invalid = check on the next pass
    int selfExecErr = 0;  // the exec failure last reported, so each one is said once
    bool heldSaid = false;
};

struct UpdateWatchProfile {
    bool hasUpdater = false;  // the image carries the updater app (an updater directory resolves)
    QString imageTag;
};

struct UpdateWatchOps {
    std::function<UpdateWatchProfile()> profile;  // re-read every pass
    std::function<ProcResult()> checkRelease;     // the catalog, about the official tag
    // Refresh the device's state.json (the offer and last error are what its screen shows) and
    // report whether the Apply signal is waiting. Only reads the signal.
    std::function<bool(const QJsonObject &offer, const QString &lastError)> probe;
    std::function<ProcResult()> fetchRelease;  // download, verify and import the official release
    std::function<bool()> deploy;              // redeploy the device onto the profile's tag
    std::function<void()> clearApply;          // remove the Apply signal
    std::function<QDateTime()> now;            // UTC
    std::function<void(const QString &)> say;  // the watcher's log
};

enum class UpdateWatchPass {
    Restarted,  // the replaced program was exec'd (observable only with a test double)
    NoUpdater,  // nothing on the device to serve this pass
    Idle,       // no Apply waiting
    Held,       // an Apply waits for the catalog's first answer; its signal stays
    Applied,
    Failed,     // the Apply ran and failed; the device's screen says why
};

// The self-restart check comes first, before the signal is read: an Apply from the previous pass
// has finished and removed its signal, and one waiting now is left for the successor. A waiting
// Apply on a profile that follows the official image is held, signal kept, until the catalog has
// answered since this process started: before that the watcher cannot know which release the
// device asked for, and a fresh process (the successor of a self-restart among them) whose first
// check failed would otherwise redeploy the image it already runs and drop the request.
UpdateWatchPass runUpdateWatchPass(SelfImageWatch &self, UpdateWatchState &st,
                                   const UpdateWatchOps &ops);

}  // namespace remora
