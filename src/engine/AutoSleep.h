#pragma once

namespace remora {

// The idle episode of `remora auto-sleep`: how long the instance has had no mirror client, and
// whether this episode's one freeze has been attempted. cmdAutoSleep owns one for the watcher's
// whole life and drives it once per poll; the profile, target, client count and freeze stay there.
struct AutoSleepEpisode {
    int idleSec = 0;
    bool slept = false;  // one freeze per idle episode — a returning client re-arms

    // Auto-sleep disabled, or a client is attached: the episode starts over.
    void reset() {
        idleSec = 0;
        slept = false;
    }

    // A poll with no client: counts pollSec, and says whether this poll should attempt the freeze.
    bool idle(int pollSec, int minutes) {
        idleSec += pollSec;
        return !slept && idleSec >= minutes * 60;
    }

    // Called after the freeze ran, even on failure: don't hammer a broken freeze.
    void markAttempted() { slept = true; }
};

}  // namespace remora
