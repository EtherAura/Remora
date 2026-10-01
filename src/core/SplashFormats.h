#pragma once
#include <QList>
#include <QString>

// Pure parsers for the boot-splash: Android's bootanimation desc.txt and the append-only
// status file the engine writes (Orchestrator truncates it at splash start; Chain appends
// progress lines and control markers). The file formats are the contract, pinned in
// test_mirrorproto.cpp.

namespace remora::mirror {

struct BootAnimPart {
    QChar type;     // 'c' = complete, 'p' = interruptible; others pass through untouched
    int count = 1;  // 0 = loop forever
    int pause = 0;  // frames of pause after each playthrough
    QString dir;    // frame directory relative to the zip root
};

struct BootAnimDesc {
    int width = 0, height = 0;
    double fps = 0;
    QList<BootAnimPart> parts;
    bool valid() const { return width > 0 && height > 0 && fps > 0 && !parts.isEmpty(); }
};

// desc.txt: "<width> <height> <fps>" then one "{c|p} <count> <pause> <dir>" line per part.
// Unknown line shapes are skipped rather than fatal — vendor zips carry extensions.
BootAnimDesc parseBootAnimDesc(const QString &descTxt);

// What playback does when a part reaches the end of a playthrough.
enum class PartStep {
    Repeat,    // stay on this part
    Advance,   // move to the next part
    Finished,  // the animation is over — hold the final frame
};

// The part-advance rule, given the part just completed, how many playthroughs it has had, and
// whether boot has finished.
//
// `count == 0` means "repeat until boot completes", so a looping part needs `finishing` to ever
// yield — without it the parts after it are unreachable. That was a real bug: the A17 animation
// is "c 1 0 part0 / c 0 0 part1 / c 1 0 part2", and part2 (the ending) never played because
// part1 looped forever (bd remora-28ix.2). A completed LAST part holds rather than repeats once
// finishing, or the ending reads as a loop that never ends.
PartStep bootAnimPartStep(const BootAnimPart &part, int playthroughs, bool isLastPart,
                          bool finishing);

struct SplashStatus {
    bool booting = false;  // REMORA_BOOTING: a fresh boot started — play the animation
    bool attach = false;   // REMORA_ATTACH: hand off now (no boot happened)
    bool abort = false;    // REMORA_ABORT: close the splash
    QString text;          // last non-marker, non-empty line — shown as on-screen status
};

// Order carries meaning in an append-only file: REMORA_BOOTING clears prior ATTACH/ABORT so a
// stale marker from an earlier phase cannot fire after a new boot begins.
SplashStatus parseSplashStatus(const QString &fileContents);

// Android's bootanimation.zip search order, which the acquirer tries in sequence.
QStringList bootAnimSearchPaths();

}  // namespace remora::mirror
