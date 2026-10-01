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
 * MediaCodec encoder and writes protocol v3's stream records to the video connection
 * (docs/MIRROR_PROTOCOL.md §2). Which display that is belongs to the capture — a mirror of an
 * existing one (kind=mirror, bd remora-28ix.3.2) or one this session created (kind=new-display,
 * bd remora-28ix.3.3) — and the encode loop cannot tell the difference.
 *
 * The encode essentials: encoder selection by name or type, codec-capability size constraints,
 * downsize-on-error before the first frame, the bounded first-output wait after an error
 * recovery, and a pinned encoder that produces nothing replaced once by the platform default
 * (bd remora-hsdz). Whatever stops the stream other than the client leaving or the session
 * closing is reported to the client as END with the reason, never as a bare close.
 */
public final class ScreenEncoder {
    private static final int DEFAULT_I_FRAME_INTERVAL = 10;      // seconds
    private static final int REPEAT_FRAME_DELAY_US = 100_000;    // repeat a static frame after 100ms
    private static final String KEY_MAX_FPS_TO_ENCODER = "max-fps-to-encoder";

    // Descending, and only consulted before the first frame — see prepareRetry().
    private static final int[] MAX_SIZE_FALLBACK = {2560, 1920, 1600, 1280, 1024, 800};
    private static final int MAX_CONSECUTIVE_ERRORS = 3;
    // After an error recovery the restarted codec owes an immediate config packet, so silence
    // past this deadline means it is wedged rather than idle (see encode()). A pinned encoder's
    // first output gets the same bound: a component that cannot run here — VA-API on a
    // guest-rendered container with no VA device — takes input and never answers (bd remora-hsdz).
    private static final int FIRST_OUTPUT_TIMEOUT_MS = 5000;
    // Halfway through a bounded wait the capture is made to compose once more, so a timeout
    // measures the encoder and never a display that simply had nothing new to show.
    private static final int FIRST_OUTPUT_NUDGE_MS = FIRST_OUTPUT_TIMEOUT_MS / 2;

    private final Session session;
    private final int sessionId;
    private final Capture capture;
    private final String codecName;      // "h264" | "h265" | "av1"
    private final String mimeType;
    private final int codecId;           // the u8 in START
    private final String encoderName;    // pinned component, or null for auto
    private final int videoBitRate;
    private final float maxFps;
    private final int requestedMaxSize;
    private final VideoStreamer streamer;

    private int maxSize;
    private boolean firstFrameSent;
    private int consecutiveErrors;
    private boolean recovering;
    private String activeEncoder;        // the component the current codec is
    private boolean codecProducedOutput; // the current codec has emitted any buffer at all
    private boolean fellBack;            // the pinned encoder was replaced by the default
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
                codecId = VideoStreamer.CODEC_H264;
                break;
            case "h265":
                mimeType = MediaFormat.MIMETYPE_VIDEO_HEVC;
                codecId = VideoStreamer.CODEC_H265;
                break;
            case "av1":
                mimeType = MediaFormat.MIMETYPE_VIDEO_AV1;
                codecId = VideoStreamer.CODEC_AV1;
                break;
            default:
                throw new ConfigurationException("unknown video codec: " + codecName);
        }
        final String enc = opts.get("video_encoder");
        this.encoderName = enc != null && !enc.isEmpty() ? enc : null;
        this.videoBitRate = intOpt(opts, "video_bit_rate", 8_000_000);
        this.requestedMaxSize = intOpt(opts, "max_size", 0);
        this.maxSize = requestedMaxSize;
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

    /** A bounded wait for output ran out: the codec took input and produced nothing. */
    private static final class NoOutputException extends IOException {
        NoOutputException(String message) {
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

    /**
     * Blocks until the stream ends (client disconnect, stop(), or a definitive encode failure).
     * Never throws: a runtime exception escaping here would reach the connection thread, and an
     * uncaught one there kills the agent and every other session with it (bd remora-hsdz).
     */
    public void run() {
        // Some devices deadlock if the encoding thread has no Looper.
        if (Looper.myLooper() == null) Looper.prepare();
        try {
            streamCapture();
        } catch (ConfigurationException e) {
            Ln.w("session " + sessionId + " video: " + e.getMessage());
            end(e.getMessage());
        } catch (IOException | RuntimeException e) {
            if (stopped || VideoStreamer.isBrokenPipe(e)) {
                // The normal ends: the client closed the socket, or the session closed under a
                // running codec — stop() can cancel a pending dequeue, which surfaces as an
                // IllegalStateException ("Pending dequeue output buffer request cancelled").
                Ln.i("session " + sessionId + " video stream ended: " + e);
            } else {
                Ln.e("session " + sessionId + " video failed", e);
                final String what = e.getMessage() != null ? e.getMessage()
                                                           : e.getClass().getSimpleName();
                end(activeEncoder != null ? "video encoder " + activeEncoder + ": " + what : what);
            }
        } finally {
            Ln.i("session " + sessionId + " video stopped");
        }
    }

    /** END reason 1: tell the client why, rather than leave it to guess from a closed socket. */
    private void end(String detail) {
        try {
            streamer.writeEnd(VideoStreamer.END_FAILED, detail);
        } catch (IOException io) {
            // the client already left
        }
    }

    private void streamCapture() throws IOException, ConfigurationException {
        MediaCodec codec = createMediaCodec();
        MediaFormat format = createFormat();
        streamer.writeStart(codecId);

        try {
            boolean alive;
            do {
                final MediaCodecInfo.VideoCapabilities caps =
                        codec.getCodecInfo().getCapabilitiesForType(mimeType).getVideoCapabilities();
                final int[] size = capture.prepare(alignment(caps), maxSize);
                clampToCodec(size, caps);
                format.setInteger(MediaFormat.KEY_WIDTH, size[0]);
                format.setInteger(MediaFormat.KEY_HEIGHT, size[1]);

                Surface surface = null;
                boolean codecStarted = false;
                boolean captureStarted = false;
                String fallBackBecause = null;
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
                        // FORMAT's resize bit tells the client "you asked for this size" apart
                        // from "the device changed size under you" — it resizes its window only
                        // for the latter.
                        final boolean wasClientResize = clientResized;
                        clientResized = false;
                        streamer.writeFormat(size[0], size[1], wasClientResize);
                        encode(codec);
                        alive = !stopped;
                    }
                } catch (IllegalStateException | IllegalArgumentException | IOException e) {
                    if (stopped) {
                        // stop() landed mid-call and cancelled it. The session is over; there is
                        // nothing to recover and nobody to tell.
                        alive = false;
                    } else {
                        // The client left; retrying would encode into a void.
                        if (VideoStreamer.isBrokenPipe(e)) throw e;
                        Ln.w("session " + sessionId + " capture/encoding error: " + e);
                        // A pinned encoder that has produced nothing at all cannot run here if
                        // it took input and stayed silent, and never could if it errored at
                        // every size the ladder tries. Either way the platform's default for the
                        // type gets one attempt before the stream is failed.
                        final boolean pinnedSilent = encoderName != null && !codecProducedOutput;
                        if (pinnedSilent && e instanceof NoOutputException) {
                            if (fellBack) {
                                throw new ConfigurationException(
                                        "video encoder " + encoderName + " produced no output, nor"
                                        + " did the default " + activeEncoder + " (" + e.getMessage()
                                        + ")");
                            }
                            fallBackBecause = "produced no output within "
                                              + FIRST_OUTPUT_TIMEOUT_MS + " ms";
                        } else if (prepareRetry(size[0], size[1])) {
                            recovering = true;
                        } else if (pinnedSilent && !fellBack) {
                            fallBackBecause = "failed at every size (" + e + ")";
                        } else {
                            throw e;
                        }
                        alive = true;
                    }
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
                if (fallBackBecause != null) {
                    final MediaCodec failed = codec;
                    codec = null;
                    failed.release();
                    codec = createFallbackCodec(fallBackBecause);
                }
            } while (alive);
        } finally {
            if (codec != null) codec.release();
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
     * the client freezes on its last frame, indistinguishable from working. A pinned encoder
     * that has not produced anything yet is held to the same deadline, for the same reason.
     */
    private void encode(MediaCodec codec) throws IOException {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean requireOutput = recovering || (encoderName != null && !codecProducedOutput);
        final long start = SystemClock.elapsedRealtime();
        boolean nudged = false;
        boolean eos;
        do {
            int id;
            if (requireOutput) {
                id = codec.dequeueOutputBuffer(info, 100_000);  // µs
                if (id == MediaCodec.INFO_TRY_AGAIN_LATER) {
                    if (stopped) return;
                    final long waited = SystemClock.elapsedRealtime() - start;
                    if (waited >= FIRST_OUTPUT_TIMEOUT_MS) {
                        throw new NoOutputException("no output within " + FIRST_OUTPUT_TIMEOUT_MS
                                                    + " ms" + (recovering ? " after recovery" : ""));
                    }
                    if (!nudged && waited >= FIRST_OUTPUT_NUDGE_MS) {
                        nudged = true;
                        capture.requestRecompose(true);
                    }
                }
            } else {
                id = codec.dequeueOutputBuffer(info, -1);
            }
            try {
                if (id >= 0) {
                    if (!codecProducedOutput) {
                        codecProducedOutput = true;
                        Ln.i("session " + sessionId + " video encoder in use: " + activeEncoder);
                    }
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
                activeEncoder = encoderName;
                return codec;
            } catch (IllegalArgumentException e) {
                throw new ConfigurationException("unknown encoder: " + encoderName);
            }
        }
        MediaCodec codec = MediaCodec.createEncoderByType(mimeType);
        activeEncoder = codec.getName();
        Ln.i("session " + sessionId + " video encoder (auto): " + activeEncoder);
        return codec;
    }

    /**
     * The pinned encoder cannot run here: try the platform's default for the same type, once.
     * Starts over as the first attempt did — full size, error budget reset — because nothing the
     * pinned encoder taught us about sizes applies to a different component.
     */
    private MediaCodec createFallbackCodec(String why) throws IOException, ConfigurationException {
        MediaCodec codec = MediaCodec.createEncoderByType(mimeType);
        final String name = codec.getName();
        if (name.equals(encoderName)) {
            codec.release();
            throw new ConfigurationException("video encoder " + encoderName + " " + why
                                             + ", and it is the default " + codecName
                                             + " encoder too");
        }
        Ln.w("session " + sessionId + " video encoder " + encoderName + " " + why
             + " — retrying once with the default " + codecName + " encoder, " + name);
        fellBack = true;
        activeEncoder = name;
        codecProducedOutput = false;
        recovering = false;
        consecutiveErrors = 0;
        maxSize = requestedMaxSize;
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
}
