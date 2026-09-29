package com.remora.agent;

import android.media.MediaCodec;

import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;

/**
 * The v1 stream framing on a video connection, byte for byte (docs/MIRROR_PROTOCOL.md): a u32
 * codec id, then a 12-byte session packet, then a 12-byte header per media packet. The client's
 * StreamDemuxer and the host frame encoder already speak exactly this, and
 * tests/test_mirrorproto.cpp pins it — it is Remora's format now, not scrcpy's, so it carries
 * over to protocol v2 unchanged. What v2 drops is only what the hello supersedes: no 64-byte
 * device-name preamble, no dummy byte.
 */
public final class VideoStreamer {
    private static final long PACKET_FLAG_SESSION = 1L << 63;
    private static final long PACKET_FLAG_CONFIG = 1L << 62;
    private static final long PACKET_FLAG_KEY_FRAME = 1L << 61;

    private final DataOutputStream out;
    private final byte[] payload = new byte[256 * 1024];  // grown on demand

    public VideoStreamer(OutputStream raw) {
        this.out = new DataOutputStream(raw);
    }

    /** u32 codec id — the ASCII codec name, left-zero-padded ("h264", "h265", " av1"). */
    public void writeCodecId(int codecId) throws IOException {
        out.writeInt(codecId);
        out.flush();
    }

    /**
     * The stream sentinels that replace the codec id: 0 = stream disabled (client continues
     * without it), 1 = configuration error (client stops). Sent instead of a codec id, so this
     * is the only way to fail a video connection after the session reply has already said ok.
     */
    public void writeDisabled(boolean error) throws IOException {
        out.writeInt(error ? 1 : 0);
        out.flush();
    }

    /** The session packet: the top flag bit set, bit 0 = "this size came from a client resize". */
    public void writeSessionMeta(int width, int height, boolean clientResized) throws IOException {
        int flags = (int) (PACKET_FLAG_SESSION >> 32);
        if (clientResized) flags |= 1;
        out.writeInt(flags);
        out.writeInt(width);
        out.writeInt(height);
        out.flush();
    }

    public void writePacket(ByteBuffer buffer, MediaCodec.BufferInfo info) throws IOException {
        final boolean config = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
        final boolean keyFrame = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
        // A config packet's header is exactly the config flag — its PTS is discarded, which is
        // what lets the client tell one from a media packet with a very large PTS.
        long ptsAndFlags = config ? PACKET_FLAG_CONFIG
                                  : info.presentationTimeUs | (keyFrame ? PACKET_FLAG_KEY_FRAME : 0);
        final int size = buffer.remaining();
        out.writeLong(ptsAndFlags);
        out.writeInt(size);
        // One write for the payload: DataOutputStream has no ByteBuffer path, and a per-byte
        // loop at 30 Mb/s is not free.
        byte[] buf = size <= payload.length ? payload : new byte[size];
        buffer.get(buf, 0, size);
        out.write(buf, 0, size);
        out.flush();
    }
}
