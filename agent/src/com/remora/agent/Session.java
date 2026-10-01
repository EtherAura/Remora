package com.remora.agent;

import java.util.Map;

/**
 * One live session: what the control connection created, what a later video connection attaches
 * to by id, and the capture both of them act on.
 *
 * Sessions are independent and concurrent (main mirror + PIP + N app windows), and each one dies
 * with its CONTROL connection — which is what preserves the engine's supervision model: kill the
 * client, the session goes with it, and the agent needs no host-side reaping. A video connection
 * closing takes the video down and leaves the session alive, so a client can reattach without
 * renegotiating.
 */
public final class Session {
    public final int id;
    public final Map<String, String> opts;
    private final Capture capture;

    private volatile ScreenEncoder encoder;
    private volatile Composer composer;
    private volatile AudioEncoder audio;
    private volatile boolean closed;
    private final Object displayReady = new Object();

    public Session(int id, Map<String, String> opts, Capture capture) {
        this.id = id;
        this.opts = opts;
        this.capture = capture;
    }

    public Capture capture() {
        return capture;
    }

    /**
     * The display input events should target, or -1 if there is not one yet. For a mirror that is
     * known immediately; for a new display it only exists once the video connection has started
     * the capture, so a client that sends input the instant its control connection is up would
     * otherwise inject into nothing.
     */
    public int awaitInputDisplay(long timeoutMs) {
        final long deadline = System.currentTimeMillis() + timeoutMs;
        synchronized (displayReady) {
            int id = capture.inputDisplayId();
            while (id < 0 && !closed) {
                long remaining = deadline - System.currentTimeMillis();
                if (remaining <= 0) return -1;
                try {
                    displayReady.wait(remaining);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return -1;
                }
                id = capture.inputDisplayId();
            }
            return id;
        }
    }

    /** Called once a capture has a display, to release anyone blocked in awaitInputDisplay(). */
    public void notifyDisplayReady() {
        synchronized (displayReady) {
            displayReady.notifyAll();
        }
    }

    /** Registers the encoder for this session; false if the session is already gone. */
    public synchronized boolean attachEncoder(ScreenEncoder e) {
        if (closed) return false;
        encoder = e;
        return true;
    }

    public synchronized void detachEncoder(ScreenEncoder e) {
        if (encoder == e) encoder = null;
    }

    public synchronized boolean attachComposer(Composer c) {
        if (closed) return false;
        composer = c;
        return true;
    }

    /** Registers the audio encoder; false if the session is already gone. */
    public synchronized boolean attachAudio(AudioEncoder a) {
        if (closed) return false;
        audio = a;
        return true;
    }

    public synchronized void detachAudio(AudioEncoder a) {
        if (audio == a) audio = null;
    }

    /** The client's window was resized: reflow a new display rather than stretch its pixels. */
    public void requestResize(int width, int height) {
        if (!(capture instanceof NewDisplayCapture)) {
            // A mirror's size follows its source; resizing it would desynchronise the two.
            return;
        }
        ((NewDisplayCapture) capture).requestResize(width, height);
        ScreenEncoder e = encoder;
        // Rebuild the encoder around the new size, and mark the next FORMAT record as a CLIENT
        // resize so the client knows not to resize its window back.
        if (e != null) e.onClientResize();
    }

    /** Called when the control connection closes: stop the video/compose and release the display. */
    public synchronized void close() {
        closed = true;
        ScreenEncoder e = encoder;
        if (e != null) e.stop();
        Composer c = composer;
        if (c != null) c.stop();
        AudioEncoder a = audio;
        if (a != null) a.stop();
        if (capture instanceof NewDisplayCapture) ((NewDisplayCapture) capture).release();
        notifyDisplayReady();  // unblock anyone waiting on a display that will never arrive
    }

    public boolean isClosed() {
        return closed;
    }
}
