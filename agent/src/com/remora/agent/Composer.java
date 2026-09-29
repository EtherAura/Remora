package com.remora.agent;

import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

/**
 * kind=compose (bd remora-28ix.3.3): compose the display without encoding it.
 *
 * When the video is encoded on the HOST — Remora's Vulkan frame encoder — the device must still
 * COMPOSE, because that composition is the frame the host encoder consumes. Composition only
 * happens if something owns the display's Surface, normally MediaCodec's input surface. Drop the
 * encoder and the display has no consumer at all, so SurfaceFlinger composes nothing and the host
 * encoder starves: measured before this existed, a 40-frame probe on the host imported zero.
 *
 * So the consumer here is an ImageReader. SurfaceFlinger composes into its buffers exactly as it
 * would into MediaCodec's, and the images are acquired and closed immediately — nothing on the
 * device reads a pixel. The point is only to own the Surface and keep the queue moving.
 *
 * There is no video connection for a compose session, so this runs off the control connection and
 * lives as long as it does.
 */
public final class Composer {
    /** Enough for the compositor to stay ahead of the drain without stale frames piling up. */
    private static final int MAX_IMAGES = 4;
    /**
     * No codec here, so no codec alignment to respect. 8 keeps the size even for any downstream
     * chroma subsampling — the host encoder converts RGBA to NV12 — without imposing a codec's
     * constraints on a path that has none.
     */
    private static final int SIZE_ALIGNMENT = 8;

    /**
     * When to try forcing a composition after the capture starts, in ms. Spread out because what
     * is being waited for is the mirror having content to compose, and on a cold boot that can be
     * seconds after the display exists. Cheap: one call each, and they stop mattering as soon as
     * the display drives itself.
     */
    private static final int[] NUDGE_DELAYS_MS = {120, 400, 1200, 3000, 6000};

    private final Session session;
    private final int sessionId;
    private final Capture capture;
    private final int maxSize;

    private volatile long composedFrames;
    private volatile boolean stopped;
    private final Object idle = new Object();

    public Composer(Session session, int maxSize) {
        this.session = session;
        this.sessionId = session.id;
        this.capture = session.capture();
        this.maxSize = maxSize;
    }

    public void stop() {
        stopped = true;
        synchronized (idle) {
            idle.notifyAll();
        }
    }

    public long composedFrames() {
        return composedFrames;
    }

    /** Blocks until stop(). */
    public void run() {
        HandlerThread drainThread = new HandlerThread("compose-drain");
        drainThread.start();
        final Handler drain = new Handler(drainThread.getLooper());
        ImageReader reader = null;
        boolean started = false;
        try {
            final int[] size = capture.prepare(SIZE_ALIGNMENT, maxSize);
            // RGBA_8888 so the host encoder knows the pixel layout it is importing, and
            // USAGE_GPU_COLOR_OUTPUT because SurfaceFlinger RENDERS INTO these buffers (client
            // composition). Without the render usage, gralloc routes some of the pool down its
            // CPU/gbm path, which on the host materialises as linear dumb buffers in system
            // memory — unimportable into Vulkan, so every frame composed into one is a hole in
            // the mirror (bd remora-e5x.18.5).
            reader = ImageReader.newInstance(size[0], size[1], PixelFormat.RGBA_8888, MAX_IMAGES,
                                             HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
                                                     | HardwareBuffer.USAGE_GPU_COLOR_OUTPUT);
            // Draining is not optional: a full queue blocks the producer, which here is
            // SurfaceFlinger's composition of the display — composition would simply stop, taking
            // the host encoder's frames with it.
            reader.setOnImageAvailableListener(r -> {
                try {
                    Image image = r.acquireLatestImage();
                    if (image != null) {
                        image.close();
                        long n = ++composedFrames;
                        // The first few individually, then every 120. A stalled mirror and a
                        // working one both print exactly "composed 1 frames" and then nothing —
                        // the difference between them lives entirely in frames 2..4, so those
                        // are the ones worth naming (bd remora-e5x.18.4).
                        if (n <= 4 || n % 120 == 0) {
                            Ln.i("session " + sessionId + " composed " + n + " frames");
                        }
                    }
                } catch (IllegalStateException e) {
                    // The queue is momentarily exhausted; the next callback drains it.
                }
            }, drain);

            Surface surface = reader.getSurface();
            capture.start(surface);
            started = true;
            // A new display exists only now, and input has been waiting for it (Controller).
            session.notifyDisplayReady();
            Ln.i("session " + sessionId + " composing " + size[0] + "x" + size[1]
                 + " without encoding (the host encodes)");

            // ...except on a still screen, where it composes ONCE and stops. A mirror composes
            // only when its source does, and a source composes only on damage — so on a settled
            // home screen the single frame that lands when the display is created is the only one
            // there will ever be, and it was composed before the mirror had content, so it is
            // blank. Everything downstream then looks healthy while the user sees grey: the host
            // encoder republishes that one frame, the client decodes it, the connect reports ok.
            // So make the damage ourselves, a few times, backing off (bd remora-e5x.18.4).
            nudgeFirstFrames(drain);

            // Nothing to pump: the display composes on its own once it has a consumer.
            synchronized (idle) {
                while (!stopped) idle.wait();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (Exception e) {
            Ln.e("session " + sessionId + " compose error", e);
        } finally {
            if (started) capture.stop();
            if (reader != null) reader.close();
            drainThread.quitSafely();
            Ln.i("session " + sessionId + " compose stopped after " + composedFrames + " frames");
        }
    }

    /**
     * Force a composition at each scheduled point for as long as the display is not producing any
     * of its own.
     *
     * Deliberately not "stop once a second frame lands": the frame a nudge produces is only as
     * good as what was on screen when it fired, and on a cold boot the mirror can attach before
     * the launcher has drawn. Stopping at the first success would freeze the mirror on an
     * almost-blank frame — the same bug, one step later. So run the whole schedule, which ends
     * late enough that the last attempt cannot be too early.
     *
     * "Not producing any of its own" is the test rather than "produced nothing at all", because
     * the nudges themselves advance the counter. A display that has moved on by more than one
     * frame since the previous look is being driven by the device and must be left alone —
     * nudging a live mirror is all cost and no benefit.
     *
     * Every attempt is HARD. Measured: the soft nudge does produce a frame, and the frame is
     * still blank — the composition a reconfiguration triggers happens while the mirror is
     * detached from its source and captures nothing. Only reattaching the output composes real
     * content.
     */
    private void nudgeFirstFrames(Handler handler) {
        final long[] seen = {0};
        for (int delay : NUDGE_DELAYS_MS) {
            handler.postDelayed(() -> {
                if (stopped) return;
                long n = composedFrames;
                if (n - seen[0] > 1) {
                    seen[0] = n;  // the device is driving it; nothing to force
                    return;
                }
                Ln.i("session " + sessionId
                     + " display is composing nothing on its own — forcing a composition");
                capture.requestRecompose(true);
                seen[0] = composedFrames;
            }, delay);
        }
    }
}
