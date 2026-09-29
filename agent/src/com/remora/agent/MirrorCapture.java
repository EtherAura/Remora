package com.remora.agent;

import android.hardware.display.DisplayManager;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.VirtualDisplay;
import android.hardware.display.VirtualDisplayConfig;
import android.view.DisplayInfo;
import android.view.Surface;

/**
 * Mirrors an existing display (display 0 unless told otherwise) into the consumer's surface, via
 * an AUTO_MIRROR virtual display created at the output size — the mirror scales its source to its
 * own bounds, so asking for the constrained size is the whole of how max_size is applied. No
 * filter, no GL pass, nothing between the display and the consumer.
 */
public final class MirrorCapture implements Capture {
    private final int sourceDisplayId;
    private final DisplayManager dm;

    private VirtualDisplay display;
    private Surface lastSurface;
    private float refreshRate = 60f;
    private int width, height;
    private int nudgeDensityDpi = 1;

    public MirrorCapture(int sourceDisplayId) {
        this.sourceDisplayId = sourceDisplayId;
        // Context-bound: createVirtualDisplay attributes the display to the caller's package, so
        // it must be the shell-attributed context (see ShellContext).
        this.dm = new DisplayManager(Main.context());
    }

    private DisplayInfo info() {
        DisplayInfo info = DisplayManagerGlobal.getInstance().getDisplayInfo(sourceDisplayId);
        if (info == null) throw new IllegalStateException("no display " + sourceDisplayId);
        return info;
    }

    @Override
    public int[] prepare(int alignment, int maxSize) {
        DisplayInfo info = info();
        refreshRate = info.getRefreshRate();
        int[] size = Sizes.constrain(info.logicalWidth, info.logicalHeight, alignment, maxSize,
                                     Integer.MAX_VALUE, Integer.MAX_VALUE, 0);
        width = size[0];
        height = size[1];
        return size;
    }

    @Override
    public void start(Surface surface) {
        stop();
        lastSurface = surface;
        // Request the SOURCE display's refresh rate: the stock helper leaves it unset and the
        // mirror then latches at 60 Hz whatever the source is doing, which silently undersamples
        // a higher-rate display into the consumer.
        VirtualDisplayConfig config =
                new VirtualDisplayConfig.Builder("remora-mirror", width, height, 1)
                        .setFlags(DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR)
                        .setDisplayIdToMirror(sourceDisplayId)
                        .setSurface(surface)
                        .setRequestedRefreshRate(refreshRate)
                        .build();
        display = dm.createVirtualDisplay(config);
        if (display == null) throw new IllegalStateException("could not create the mirror display");
    }

    @Override
    public void stop() {
        if (display != null) {
            display.release();
            display = null;
        }
    }

    @Override
    public int inputDisplayId() {
        // Events go to the SOURCE display: the mirror is an output, and injecting into it would
        // land on a display no app is running on.
        return sourceDisplayId;
    }

    @Override
    public void requestRecompose(boolean hard) {
        VirtualDisplay vd = display;
        if (vd == null) return;
        try {
            if (hard) {
                // Detach and reattach the output: the display device is torn down and rebuilt, so
                // it always composes. Can show as a flicker, hence the escalation rather than the
                // first move.
                Surface s = lastSurface;
                vd.setSurface(null);
                vd.setSurface(s);
            } else {
                // Same pixels, different declared density. A mirror scales its source to its own
                // bounds, so density here is metadata and cannot change an output pixel — but it
                // does make the display report CHANGED, which is what makes SurfaceFlinger compose
                // the mirror again. Toggled, so a second nudge works as well as the first.
                nudgeDensityDpi = nudgeDensityDpi == 1 ? 2 : 1;
                vd.resize(width, height, nudgeDensityDpi);
            }
        } catch (Exception e) {
            // Best-effort: never let a nudge take down a mirror that is otherwise working.
            Ln.w("could not force a recomposition: " + e);
        }
    }
}
