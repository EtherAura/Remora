#pragma once
#include <QByteArray>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>

// Keeping the desk's monitors where they were while the host agent adds a virtual output for a
// phone (docs/PHONE_APP.md §3). Pure: kscreen-doctor's JSON in, its argv out.
//
// Why this exists, measured on Plasma 6.8:
//  - A new output makes a set of outputs KWin has never seen, so it lays the whole desk out again
//    from scratch and saves that layout — monitors move, and they can overlap.
//  - KWin assigns the new output a priority above existing monitors, and Plasma places panels by
//    priority, so a desk panel moves onto the phone's screen.
//  - stream_virtual_output takes its width and height as the mode's PIXELS and ignores the scale
//    it is given; a virtual output has no EDID, so KWin applies whatever it saved for the last
//    one, or 1.
// So after every creation the agent applies the whole layout: every output that existed before at
// its old position and priority, and its own outputs parked right of them, lowest priority, at the
// scale the phone asked for. Idempotent, and KWin saves it, so the next creation starts right.

namespace remora::outputs {

struct OutputState {
    QString name;
    bool enabled = false;
    QPoint pos;            // logical
    double scale = 1.0;
    int priority = 0;      // 1 = primary; 0 = none (disabled)
    QSize pixelSize;       // the current mode, before rotation
    int rotation = 1;      // kscreen: 1 none, 2 left, 4 inverted, 8 right
    // Non-zero when this output mirrors another (kscreen's id of the source). A mirror copy has
    // no place of its own on the desk: KWin shows it at its source's geometry and does not
    // advertise it to clients, and its stored position and priority are not what is on screen.
    int replicationSource = 0;
    bool hdr = false;      // its screen buffers are 16 bits a channel
    // Logical extent: the mode, rotated, divided by the scale.
    QSize logicalSize() const;
};

// kscreen-doctor -j. A virtual output's entry has no "enabled" key, so "connected" stands in.
// Empty on malformed input.
QList<OutputState> parseKScreenJson(const QByteArray &json);

struct VirtualPlacement {
    QString name;   // the connector name, "Virtual-…"
    double scale = 1.0;
    bool apart = false;  // not an extension of the desk: no edge of it touches another output
};

// How far an output kept apart stands from the rest. KWin moves the pointer to the output nearest
// where it would go, so a gap only holds if no single motion can land nearer the far side.
constexpr int kApartGap = 2000;

// kscreen-doctor arguments that put every enabled output of `before` back where it was and park
// `ours` to the right of them, top-aligned, in order, at priorities after the existing ones: the
// desk's extensions edge to edge, then the ones kept apart, each kApartGap beyond the last.
// Outputs in `now` that are neither in `before` nor ours are left alone, and so is every mirror
// copy: telling one where to be is telling KWin to stop mirroring. Empty when nothing needs to
// change.
QStringList repairArgs(const QList<OutputState> &before, const QList<OutputState> &now,
                       const QList<VirtualPlacement> &ours);

// The largest scale, in KWin's steps of 1/120, at which an output of `pixels` still holds a window
// whose minimum size is `minLogical`: KWin keeps a window that cannot fit off the output, so an
// app with a large minimum (Elisa's is 612x459) opened on the desk instead of the phone. Never
// below 0.5; infinity when there is nothing to fit.
double fitScale(QSize pixels, QSize minLogical);

}  // namespace remora::outputs
