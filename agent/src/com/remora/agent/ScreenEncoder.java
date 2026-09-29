package com.remora.agent;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Range;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Map;

/**
 * The video half of any session that encodes on the device: it drives a {@link Capture} into a
 * MediaCodec encoder and writes v1's stream framing to the video connection. Which display that
 * is belongs to the capture — a mirror of an existing one (kind=mirror, bd remora-28ix.3.2) or
 * one this session created (kind=new-display, bd remora-28ix.3.3) — and the encode loop cannot
 * tell the difference.
 *
 * The SurfaceEncoder essentials, ported: encoder selection by name or type, codec-capability size
 * constraints, downsize-on-error before the first frame, and the bounded first-output wait after
 * an error recovery.
 */
public final class ScreenEncoder {
    private static final int DEFAULT_I_FRAME_INTERVAL = 10;      // seconds
    private static final int REPEAT_FRAME_DELAY_US = 100_000;    // repeat a static frame after 100ms
    private static final String KEY_MAX_FPS_TO_ENCODER = "max-fps-to-encoder";

    // Descending, and only consulted before the first frame — see prepareRetry().
    private static final int[] MAX_SIZE_FALLBACK = {2560, 1920, 1600, 1280, 1024, 800};
    private static final int MAX_CONSECUTIVE_ERRORS = 3;
    // After an error recovery the restarted codec owes an immediate config packet, so silence
    // past this deadline means it is wedged rather than idle (see encode()).
    private static final int RECOVERY_FIRST_OUTPUT_TIMEOUT_MS = 5000;

    private final Session session;
    private final int sessionId;
    private final Capture capture;
    private final String codecName;      // "h264" | "h265" | "av1"
    private final String mimeType;
    private final int codecId;           // the u32 on the wire
    private final String encoderName;    // pinned component, or null for auto
    private final int videoBitRate;
    private final float maxFps;
    private final VideoStreamer streamer;

    private int maxSize;
    private boolean firstFrameSent;
    private int consecutiveErrors;
    private boolean recovering;
    private volatile boolean clientResized;
    private volatile boolean stopped;
    private volatile MediaCodec runningCodec;  // signalled with an EOS to interrupt encode()

    public ScreenEncoder(Session session, VideoStreamer streamer) throws ConfigurationException {
        final Map<String, String> opts = session.opts;
        this.session = session;
        this.sessionId = session.id;
        this.capture = session.capture();
        this.streamer = streamer;
        this.codecName = opts.containsKey("video_codec") ? opts.get("video_codec") : "h264";
        switch (codecName) {
            case "h264":
                mimeType = MediaFormat.MIMETYPE_VIDEO_AVC;
                codecId = 0x68_32_36_34;
                break;
            case "h265":
                mimeType = MediaFormat.MIMETYPE_VIDEO_HEVC;
                codecId = 0x68_32_36_35;
                break;
            case "av1":
                mimeType = MediaFormat.MIMETYPE_VIDEO_AV1;
                codecId = 0x00_61_76_31;
                break;
            default:
                throw new ConfigurationException("unknown video codec: " + codecName);
        }
        final String enc = opts.get("video_encoder");
        this.encoderName = enc != null && !enc.isEmpty() ? enc : null;
        this.videoBitRate = intOpt(opts, "video_bit_rate", 8_000_000);
        this.maxSize = intOpt(opts, "max_size", 0);
        this.maxFps = floatOpt(opts, "max_fps", 0);
    }

    private static int intOpt(Map<String, String> opts, String key, int fallback) {
        try {
            String v = opts.get(key);
            return v != null && !v.isEmpty() ? Integer.parseInt(v) : fallback;
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static float floatOpt(Map<String, String> opts, String key, float fallback) {
        try {
            String v = opts.get(key);
            return v != null && !v.isEmpty() ? Float.parseFloat(v) : fallback;
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    /** Thrown for a setup problem the client must be told about rather than retried. */
    public static final class ConfigurationException extends Exception {
        public ConfigurationException(String message) {
            super(message);
        }
    }

    public void stop() {
        stopped = true;
        MediaCodec codec = runningCodec;
        if (codec != null) {
            try {
                codec.signalEndOfInputStream();  // unblocks dequeueOutputBuffer with an EOS
            } catch (Exception e) {
                // racing teardown — the encode loop is exiting anyway
            }
        }
    }

    /** Blocks until the stream ends (client disconnect, stop(), or a definitive encode failure). */
    public void run() {
        // Some devices deadlock if the encoding thread has no Looper (upstream scrcpy 4143).
        if (Looper.myLooper() == null) Looper.prepare();
        try {
            streamCapture();
        } catch (ConfigurationException e) {
            Ln.w("session " + sessionId + " video: " + e.getMessage());
            try {
                streamer.writeDisabled(true);  // config error: the client stops
            } catch (IOException io) {
                // the client already left
            }
        } catch (IOException e) {
            // A broken pipe is the normal end: the client closed the socket.
            Ln.i("session " + sessionId + " video stream ended: " + e);
        } finally {
            Ln.i("session " + sessionId + " video stopped");
        }
    }

    private void streamCapture() throws IOException, ConfigurationException {
        MediaCodec codec = createMediaCodec();
        MediaFormat format = createFormat();
        MediaCodecInfo.VideoCapabilities caps =
                codec.getCodecInfo().getCapabilitiesForType(mimeType).getVideoCapabilities();
        streamer.writeCodecId(codecId);

        try {
            boolean alive;
            do {
                final int[] size = capture.prepare(alignment(caps), maxSize);
                clampToCodec(size, caps);
                format.setInteger(MediaFormat.KEY_WIDTH, size[0]);
                format.setInteger(MediaFormat.KEY_HEIGHT, size[1]);

                Surface surface = null;
                boolean codecStarted = false;
                boolean captureStarted = false;
                try {
                    codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                    surface = codec.createInputSurface();
                    capture.start(surface);
                    captureStarted = true;
                    // A new display exists only now, and input has been waiting for it (Controller).
                    session.notifyDisplayReady();
                    codec.start();
                    codecStarted = true;
                    runningCodec = codec;

                    if (stopped) {
                        alive = false;
                    } else {
                        // The session packet's resize bit tells the client "you asked for this
                        // size" apart from "the device changed size under you" — it resizes its
                        // window only for the latter.
                        final boolean wasClientResize = clientResized;
                        clientResized = false;
                        streamer.writeSessionMeta(size[0], size[1], wasClientResize);
                        encode(codec);
                        alive = !stopped;
                    }
                } catch (IllegalStateException | IllegalArgumentException | IOException e) {
                    if (isBrokenPipe(e)) throw e;  // the client left; retrying encodes into a void
                    Ln.w("session " + sessionId + " capture/encoding error: " + e);
                    if (!prepareRetry(size[0], size[1])) throw e;
                    recovering = true;
                    alive = true;
                } finally {
                    runningCodec = null;
                    if (captureStarted) capture.stop();
                    if (codecStarted) {
                        try {
                            codec.stop();
                        } catch (IllegalStateException e) {
                            // already dead — reset() below is what matters
                        }
                    }
                    codec.reset();
                    if (surface != null) surface.release();
                }
            } while (alive);
        } finally {
            codec.release();
        }
    }

    private static int alignment(MediaCodecInfo.VideoCapabilities caps) {
        final int a = Math.max(caps.getWidthAlignment(), caps.getHeightAlignment());
        return a > 0 ? a : 2;
    }

    /**
     * The capture sized itself to its display and to max_size; this cuts the result to what the
     * CODEC can actually take. An encoder configured past its capabilities fails at start() with
     * nothing useful in the message, so this is what turns "3760x1992 into a 1920-wide encoder"
     * into a working stream rather than a mystery.
     */
    private void clampToCodec(int[] size, MediaCodecInfo.VideoCapabilities caps) {
        final Range<Integer> widths = caps.getSupportedWidths();
        final Range<Integer> heights = caps.getSupportedHeights();
        final int maxW, maxH;
        if (size[0] < size[1]) {
            maxH = heights.getUpper();
            maxW = caps.getSupportedWidthsFor(maxH).getUpper();
        } else {
            maxW = widths.getUpper();
            maxH = caps.getSupportedHeightsFor(maxW).getUpper();
        }
        final int[] clamped = Sizes.constrain(size[0], size[1], alignment(caps), 0, maxW, maxH,
                                              Math.max(widths.getLower(), heights.getLower()));
        size[0] = clamped[0];
        size[1] = clamped[1];
    }

    /**
     * The client resized its window, so the display was reflowed: interrupt the encode loop with
     * an EOS and let the do/while rebuild the codec around the new size.
     */
    public void onClientResize() {
        clientResized = true;
        MediaCodec codec = runningCodec;
        if (codec != null) {
            try {
                codec.signalEndOfInputStream();
            } catch (Exception e) {
                // racing teardown — the loop is exiting anyway
            }
        }
    }

    /**
     * Waiting forever for output is right in steady state — no output means a static screen —
     * but not straight after an error recovery, where the restarted codec owes a config packet
     * immediately and REPEAT_PREVIOUS_FRAME_AFTER guarantees it input. Silence there means a
     * wedged codec (a dead media engine, say), and without a deadline the socket stays open and
     * the client freezes on its last frame, indistinguishable from working.
     */
    private void encode(MediaCodec codec) throws IOException {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean requireOutput = recovering;
        long deadline = requireOutput
                ? SystemClock.elapsedRealtime() + RECOVERY_FIRST_OUTPUT_TIMEOUT_MS : 0;
        boolean eos;
        do {
            int id;
            if (requireOutput) {
                id = codec.dequeueOutputBuffer(info, 100_000);  // µs
                if (id == MediaCodec.INFO_TRY_AGAIN_LATER
                        && SystemClock.elapsedRealtime() >= deadline) {
                    throw new IOException("no encoder output within "
                                          + RECOVERY_FIRST_OUTPUT_TIMEOUT_MS + "ms after recovery");
                }
            } else {
                id = codec.dequeueOutputBuffer(info, -1);
            }
            try {
                if (id >= 0) {
                    recovering = false;  // producing output again — back to blocking waits
                    requireOutput = false;
                }
                eos = id >= 0 && (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                if (id >= 0 && info.size > 0) {
                    if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        firstFrameSent = true;
                        consecutiveErrors = 0;
                    }
                    ByteBuffer buffer = codec.getOutputBuffer(id);
                    streamer.writePacket(buffer, info);
                }
            } finally {
                if (id >= 0) codec.releaseOutputBuffer(id, false);
            }
        } while (!eos);
    }

    private boolean prepareRetry(int width, int height) {
        if (firstFrameSent) {
            // Past the first frame, downsizing would be a surprise — retry the same size a few
            // times and then give up, so a permanently broken encoder ends the stream instead of
            // spinning.
            if (++consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) return false;
            SystemClock.sleep(50);
            return true;
        }
        final int current = Math.max(width, height);
        for (int candidate : MAX_SIZE_FALLBACK) {
            if (candidate < current) {
                maxSize = candidate;
                Ln.i("session " + sessionId + " retrying video at max_size=" + candidate);
                return true;
            }
        }
        return false;
    }

    private MediaCodec createMediaCodec() throws IOException, ConfigurationException {
        if (encoderName != null) {
            try {
                MediaCodec codec = MediaCodec.createByCodecName(encoderName);
                // A name pin that silently selects the wrong type would produce a stream the
                // client cannot decode, so check rather than trust.
                if (!codec.getCodecInfo().getSupportedTypes()[0].equalsIgnoreCase(mimeType)) {
                    codec.release();
                    throw new ConfigurationException("encoder " + encoderName + " is not a "
                                                     + codecName + " encoder");
                }
                Ln.i("session " + sessionId + " video encoder (pinned): " + encoderName);
                return codec;
            } catch (IllegalArgumentException e) {
                throw new ConfigurationException("unknown encoder: " + encoderName);
            }
        }
        MediaCodec codec = MediaCodec.createEncoderByType(mimeType);
        Ln.i("session " + sessionId + " video encoder (auto): " + codec.getName());
        return codec;
    }

    private MediaFormat createFormat() {
        MediaFormat format = new MediaFormat();
        format.setString(MediaFormat.KEY_MIME, mimeType);
        format.setInteger(MediaFormat.KEY_BIT_RATE, videoBitRate);
        // Required to configure, but the real rate is variable — this is not a cap.
        format.setInteger(MediaFormat.KEY_FRAME_RATE, 60);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                          MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, DEFAULT_I_FRAME_INTERVAL);
        // Show the very first frame, and recover quality on a screen that has stopped changing.
        format.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, REPEAT_FRAME_DELAY_US);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);   // real-time
        format.setInteger(MediaFormat.KEY_LATENCY, 1);    // emit a frame as soon as one is queued
        if (maxFps > 0) {
            format.setFloat(KEY_MAX_FPS_TO_ENCODER, maxFps);
            // c2-va needs the vendor key as well: CCodec routes the standard one to the
            // InputSurface, which that stack does not service, while the vendor key reaches the
            // component and drops frames at its input queue (bd remora-rjh). Unknown vendor keys
            // are ignored elsewhere, so this is a no-op on any other encoder.
            format.setInteger("vendor.remora-max-fps.value", Math.round(maxFps));
        }
        return format;
    }

    private static boolean isBrokenPipe(Exception e) {
        Throwable t = e;
        while (t != null) {
            String m = t.getMessage();
            if (m != null && (m.contains("Broken pipe") || m.contains("EPIPE"))) return true;
            t = t.getCause();
        }
        return false;
    }
}
