package com.remora.agent;

import android.view.Surface;

/**
 * A source of composed frames, behind the smallest interface that lets one encode loop and one
 * compose loop serve both session kinds (bd remora-28ix.3.3).
 *
 * The fork splits this into SurfaceCapture with ScreenCapture / NewDisplayCapture / CameraCapture
 * subclasses carrying crop, orientation locks, rotation angles and a GL filter chain. None of that
 * is in Remora's path — the mirror is the whole display and the only scaling is max_size, which
 * the virtual display applies by itself — so what remains is: work out a size, hand the display a
 * surface, and be pokeable when nothing is composing.
 */
public interface Capture {
    /**
     * Decide the output size for this attempt. Called before every start(), so a display that
     * changed size since the last one is picked up. {@code alignment} is the consumer's
     * requirement (a codec's, or 8 for the compose path, which has no codec).
     */
    int[] prepare(int alignment, int maxSize);

    /** Point the display at this consumer surface. */
    void start(Surface surface);

    /** Release the display (the surface belongs to the caller). */
    void stop();

    /**
     * The display input events should target. For a mirror that is the mirrored display; for a
     * new display it is the one this capture created, and it is only known after start().
     */
    int inputDisplayId();

    /**
     * Force a composition. A mirror display only composes when its source does, so a settled
     * screen composes nothing and a consumer that has just attached would sit on one stale frame
     * forever. {@code hard} detaches and reattaches the surface (unambiguous, can flicker); the
     * soft form only dirties the display.
     */
    void requestRecompose(boolean hard);
}
