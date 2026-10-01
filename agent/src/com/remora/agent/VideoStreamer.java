package com.remora.agent;

import android.media.MediaCodec;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/**
 * A media stream on a video or audio connection: protocol v3's stream records
 * (docs/MIRROR_PROTOCOL.md §2) — START with the codec, FORMAT with the frame size (video only),
 * CONFIG with the codec configuration, a FRAME per encoded packet, and END to say why the
 * stream stopped. The client's StreamDemuxer reads it and tests/test_mirrorproto.cpp pins it;
 * the host frame encoder writes the same records on its own socket.
 */
public final class VideoStreamer {
    // Stream record types.
    private static final int START = 0x01;
    private static final int FORMAT = 0x02;
    private static final int CONFIG = 0x03;
    private static final int FRAME = 0x04;
    private static final int END = 0x05;

    // The u8 codec in START. Defined here once; the encoders name them rather than restate them.
    public static final int CODEC_H264 = 0x01;
    public static final int CODEC_H265 = 0x02;
    public static final int CODEC_AV1 = 0x03;
    public static final int CODEC_OPUS = 0x81;
    public static final int CODEC_AAC = 0x82;
    public static final int CODEC_FLAC = 0x83;
    public static final int CODEC_PCM = 0x84;

    /** END reasons: carry on without this stream, or stop. */
    public static final int END_UNAVAILABLE = 0;
    public static final int END_FAILED = 1;

    private static final int FORMAT_FLAG_CLIENT_RESIZE = 1;
    private static final int FRAME_FLAG_KEY = 1;

    private final Records.Writer out;
    private final byte[] frameFields = new byte[8 + 1];  // u64 ptsUs, u8 flags
    private byte[] payload = new byte[256 * 1024];       // grown on demand

    public VideoStreamer(OutputStream raw) {
        this.out = new Records.Writer(raw, Records.STREAM_LIMIT);
    }

    /** START: the first record of every stream that is going to carry anything. */
    public void writeStart(int codec) throws IOException {
        out.write(START, new byte[]{(byte) codec});
    }

    /** FORMAT: the frame size, before the first CONFIG/FRAME and again whenever it changes. */
    public void writeFormat(int width, int height, boolean clientResized) throws IOException {
        out.write(FORMAT, ByteBuffer.allocate(4 + 4 + 1)
                                  .putInt(width)
                                  .putInt(height)
                                  .put((byte) (clientResized ? FORMAT_FLAG_CLIENT_RESIZE : 0))
                                  .array());
    }

    /**
     * END: the last record — the stream is over, and this says why. In place of START it fails a
     * connection the session reply has already accepted; after it, it says why an encoder that
     * was running stopped instead of just closing.
     */
    public void writeEnd(int reason, String detail) throws IOException {
        // Exception text is a common source and often ends in a newline, which would land in the
        // middle of the client's one-line error — so collapse the whitespace once, here.
        byte[] text = (detail != null ? detail.replaceAll("\\s+", " ").trim() : "")
                .getBytes(StandardCharsets.UTF_8);
        byte[] body = new byte[1 + text.length];
        body[0] = (byte) reason;
        System.arraycopy(text, 0, body, 1, text.length);
        out.write(END, body);
    }

    /** One MediaCodec output buffer: CONFIG when the codec flags it as configuration, else FRAME. */
    public void writePacket(ByteBuffer buffer, MediaCodec.BufferInfo info) throws IOException {
        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
            writeConfig(buffer);
        } else {
            writeFrame(buffer, info.presentationTimeUs,
                       (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0);
        }
    }

    public synchronized void writeConfig(ByteBuffer buffer) throws IOException {
        final int size = buffer.remaining();
        out.write(CONFIG, null, 0, take(buffer, size), 0, size);
    }

    public synchronized void writeFrame(ByteBuffer buffer, long ptsUs, boolean keyFrame)
            throws IOException {
        final int size = buffer.remaining();
        if (size == 0) return;  // a FRAME carries at least one byte; an empty buffer is no frame
        for (int i = 0; i < 8; ++i) frameFields[i] = (byte) (ptsUs >>> (56 - 8 * i));
        frameFields[8] = (byte) (keyFrame ? FRAME_FLAG_KEY : 0);
        out.write(FRAME, frameFields, frameFields.length, take(buffer, size), 0, size);
    }

    /**
     * The buffer's bytes in the reusable payload array. One bulk get and, in the writer, one write
     * for the payload: the output stream has no ByteBuffer path, and a per-byte loop at 30 Mb/s
     * is not free.
     */
    private byte[] take(ByteBuffer buffer, int size) {
        if (size > payload.length) payload = new byte[size];
        buffer.get(payload, 0, size);
        return payload;
    }

    /** True when this failure is the peer closing its end — the normal way a stream ends. */
    static boolean isBrokenPipe(Throwable e) {
        Throwable t = e;
        while (t != null) {
            String m = t.getMessage();
            if (m != null && (m.contains("Broken pipe") || m.contains("EPIPE"))) return true;
            t = t.getCause();
        }
        return false;
    }
}
