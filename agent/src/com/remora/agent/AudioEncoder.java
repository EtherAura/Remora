package com.remora.agent;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.os.SystemClock;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.Map;

/**
 * The audio half of a session (bd remora-28ix.3.6): REMOTE_SUBMIX capture, opus via MediaCodec,
 * written with the SAME framing as video (VideoStreamer) minus the session packet — audio has no
 * geometry, so the client's StreamDemuxer reads an audio stream as codec id then packets.
 *
 * It rides its own connection ROLE (2) rather than a positional third socket. v1 made audio a
 * socket-ORDERING convention — video, then audio, then control — which is exactly the fragility
 * the role byte exists to remove: with roles, "no video" no longer renumbers everything after it.
 *
 * Capture is REMOTE_SUBMIX rather than the fork's AudioPlaybackCapture: the latter builds an
 * AudioPolicy mix through three layers of reflection to capture per-app or per-usage, and Remora
 * mirrors the whole device — the submix IS the whole device's output. Compiled against the real
 * framework, so these are plain calls (docs/MIRROR_AGENT.md).
 */
public final class AudioEncoder {
    // The wire contract, fixed on both sides: 48 kHz stereo PCM in, opus out.
    private static final int SAMPLE_RATE = 48000;
    private static final int CHANNELS = 2;
    private static final int CHANNEL_CONFIG = AudioFormat.CHANNEL_IN_STEREO;
    private static final int ENCODING = AudioFormat.ENCODING_PCM_16BIT;
    private static final int CODEC_ID_OPUS = 0x6f_70_75_73;  // "opus"
    private static final int DEFAULT_BIT_RATE = 128_000;
    // One read of 1024 frames, the fork's size: small enough that a read is not a latency floor.
    private static final int MAX_READ_SIZE = 1024 * CHANNELS * 2;
    private static final int DEQUEUE_TIMEOUT_US = 100_000;
    private static final byte[] OPUS_HEADER_ID = {'A', 'O', 'P', 'U', 'S', 'H', 'D', 'R'};

    private final int sessionId;
    private final VideoStreamer streamer;
    private final int bitRate;

    private volatile boolean stopped;
    private volatile MediaCodec runningCodec;
    private volatile AudioRecord runningRecord;

    public AudioEncoder(int sessionId, Map<String, String> opts, VideoStreamer streamer) {
        this.sessionId = sessionId;
        this.streamer = streamer;
        this.bitRate = intOpt(opts, "audio_bit_rate", DEFAULT_BIT_RATE);
    }

    private static int intOpt(Map<String, String> opts, String key, int fallback) {
        try {
            String v = opts.get(key);
            return v != null && !v.isEmpty() ? Integer.parseInt(v) : fallback;
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    public void stop() {
        stopped = true;
        AudioRecord r = runningRecord;
        // Stopping the RECORD is what unblocks a thread parked in read(); the codec follows.
        if (r != null) {
            try {
                r.stop();
            } catch (IllegalStateException e) {
                // already stopped — the run loop is on its way out
            }
        }
    }

    private static AudioRecord createRecord() {
        AudioFormat format = new AudioFormat.Builder()
                                     .setEncoding(ENCODING)
                                     .setSampleRate(SAMPLE_RATE)
                                     .setChannelMask(CHANNEL_CONFIG)
                                     .build();
        AudioRecord.Builder builder = new AudioRecord.Builder()
                                              .setAudioSource(MediaRecorder.AudioSource.REMOTE_SUBMIX)
                                              .setAudioFormat(format);
        // Without an attributed context the builder throws "Cannot create AudioRecord": the
        // record is attributed to a package, and the agent is not an app. The shell context is
        // the one uid 2000 owns — the same reason Controller constructs its ClipboardManager
        // over it rather than through getSystemService().
        builder.setContext(Main.context());
        int min = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_CONFIG, ENCODING);
        // 8x the minimum, the fork's figure: headroom against a scheduling hiccup, and it does
        // not add latency — latency is set by how fast we drain, not by how deep the buffer is.
        if (min > 0) builder.setBufferSizeInBytes(8 * min);
        return builder.build();
    }

    private static MediaFormat createFormat(int bitRate) {
        MediaFormat format = new MediaFormat();
        format.setString(MediaFormat.KEY_MIME, MediaFormat.MIMETYPE_AUDIO_OPUS);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitRate);
        format.setInteger(MediaFormat.KEY_CHANNEL_COUNT, CHANNELS);
        format.setInteger(MediaFormat.KEY_SAMPLE_RATE, SAMPLE_RATE);
        return format;
    }

    /** Blocks until stop() or the peer disconnects. */
    public void run() {
        AudioRecord record = null;
        MediaCodec codec = null;
        boolean codecStarted = false;
        try {
            record = createRecord();
            runningRecord = record;
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
            codec.configure(createFormat(bitRate), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            // Starting the record is also what makes the submix an AVAILABLE output device, which
            // is the condition the submix_all_output source patch routes on: from here until the
            // finally block, every strategy AOSP would have sent to the container's stub speaker
            // — ring, alarm, notification, call — comes to us instead (bd remora-8g24).
            record.startRecording();
            codec.start();
            codecStarted = true;
            runningCodec = codec;
            Ln.i("session " + sessionId + " audio: opus " + SAMPLE_RATE + " Hz stereo via "
                 + codec.getName());
            streamer.writeCodecId(CODEC_ID_OPUS);
            pump(record, codec);
        } catch (IOException e) {
            // A broken pipe is the client leaving, which is not an error worth a stack trace.
            Ln.i("session " + sessionId + " audio stopped: " + e);
        } catch (Exception e) {
            Ln.e("session " + sessionId + " audio failed", e);
            try {
                // The stream has no other way to say "this failed after the session was accepted".
                if (!stopped) streamer.writeDisabled(true);
            } catch (IOException ignored) {
                // the peer is gone too; nothing left to tell
            }
        } finally {
            runningCodec = null;
            runningRecord = null;
            if (codec != null) {
                if (codecStarted) {
                    try {
                        codec.stop();
                    } catch (IllegalStateException e) {
                        // already dead — release() below is what matters
                    }
                }
                codec.release();
            }
            if (record != null) {
                try {
                    record.stop();
                } catch (IllegalStateException e) {
                    // never started, or already stopped
                }
                record.release();
            }
            Ln.i("session " + sessionId + " audio ended");
        }
    }

    /**
     * MediaCodec's opus config buffer is not extradata — it is three ID/length sections:
     *
     *   AOPUSHDR <u64 len> OpusHead…   <- the only part a decoder wants
     *   AOPUSDLY <u64 len> …           <- codec delay
     *   AOPUSPRL <u64 len> …           <- seek pre-roll
     *
     * Handing the whole 83-byte blob over as extradata makes avcodec_open2 fail, which is exactly
     * what "audio decoder open failed" was. Narrow the buffer to the OpusHead slice, the same fix
     * the fork's Streamer applies (docs: developer.android.com/reference/android/media/MediaCodec#CSD).
     */
    private static void trimOpusConfig(ByteBuffer buffer) throws IOException {
        if (buffer.remaining() < 16) throw new IOException("opus config packet too short");
        final byte[] id = new byte[8];
        buffer.get(id);
        if (!Arrays.equals(id, OPUS_HEADER_ID)) throw new IOException("no AOPUSHDR in opus config");
        // The section length is written in NATIVE byte order, not the wire's big-endian.
        final ByteOrder previous = buffer.order();
        buffer.order(ByteOrder.nativeOrder());
        final long size = buffer.getLong();
        buffer.order(previous);
        if (size < 0 || size > buffer.remaining()) {
            throw new IOException("bad OpusHead length in opus config: " + size);
        }
        buffer.limit(buffer.position() + (int) size);
    }

    /**
     * PCM in, packets out, on this one thread. The fork splits input and output across two
     * threads; here a single loop alternates, which is enough because the encoder's output is
     * tiny next to video and the read is the only blocking call.
     */
    private void pump(AudioRecord record, MediaCodec codec) throws IOException {
        final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (!stopped) {
            final int inIndex = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT_US);
            if (inIndex >= 0) {
                ByteBuffer in = codec.getInputBuffer(inIndex);
                int limit = Math.min(in.capacity(), MAX_READ_SIZE);
                in.position(0);
                in.limit(limit);
                final int read = record.read(in, limit, AudioRecord.READ_BLOCKING);
                if (read < 0) {
                    // stop() races us here; a negative read after it is the expected exit.
                    if (!stopped) Ln.w("session " + sessionId + " audio read failed: " + read);
                    codec.queueInputBuffer(inIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    return;
                }
                // The encoder wants a monotonic timestamp; the record has no clock of its own.
                codec.queueInputBuffer(inIndex, 0, read, SystemClock.elapsedRealtimeNanos() / 1000,
                                       0);
            }
            for (;;) {
                final int outIndex = codec.dequeueOutputBuffer(info, 0);
                if (outIndex < 0) break;  // TRY_AGAIN / format change — nothing to write
                try {
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return;
                    if (info.size > 0) {
                        ByteBuffer out = codec.getOutputBuffer(outIndex);
                        out.position(info.offset);
                        out.limit(info.offset + info.size);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            trimOpusConfig(out);
                            Ln.i("session " + sessionId + " audio config: OpusHead, "
                                 + out.remaining() + " bytes");
                        }
                        streamer.writePacket(out, info);
                    }
                } finally {
                    codec.releaseOutputBuffer(outIndex, false);
                }
            }
        }
    }
}
