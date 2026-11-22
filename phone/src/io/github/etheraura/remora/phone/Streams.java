package io.github.etheraura.remora.phone;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.view.Surface;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * A session's media connections (docs/MIRROR_PROTOCOL.md §2), decoded by the phone's hardware:
 * START names the codec, FORMAT the size (again after a reflow), CONFIG the codec configuration,
 * FRAME the samples. Each runs on its own thread, blocking on its socket.
 */
final class Streams {
    private Streams() {}

    /** Whether this phone decodes H.265 in hardware — what the session asks for if so. */
    static boolean hasHevcDecoder() {
        for (android.media.MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS)
                .getCodecInfos()) {
            if (info.isEncoder() || !info.isHardwareAccelerated()) continue;
            for (String type : info.getSupportedTypes())
                if (type.equalsIgnoreCase(MediaFormat.MIMETYPE_VIDEO_HEVC)) return true;
        }
        return false;
    }

    interface Ended {
        void ended(String why);
    }

    /** The video connection into a surface, until it ends. */
    static void video(InputStream socket, Surface surface, Ended ended) {
        DataInputStream in = new DataInputStream(socket);
        MediaCodec codec = null;
        String mime = null;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            for (Proto.Record r; (r = Proto.readRecord(in, Proto.STREAM_LIMIT)) != null; ) {
                ByteBuffer b = ByteBuffer.wrap(r.body);
                switch (r.type) {
                    case Proto.START:
                        int c = b.get() & 0xff;
                        mime = c == Proto.CODEC_H265 ? MediaFormat.MIMETYPE_VIDEO_HEVC
                             : c == Proto.CODEC_AV1 ? MediaFormat.MIMETYPE_VIDEO_AV1
                             : MediaFormat.MIMETYPE_VIDEO_AVC;
                        break;
                    case Proto.FORMAT: {
                        // A new size (the first, or a reflow): a decoder configured for it.
                        int w = b.getInt(), h = b.getInt();
                        if (codec != null) {
                            codec.stop();
                            codec.release();
                        }
                        codec = MediaCodec.createDecoderByType(mime);
                        MediaFormat f = MediaFormat.createVideoFormat(mime, w, h);
                        f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
                        codec.configure(f, surface, null, 0);
                        codec.start();
                        break;
                    }
                    case Proto.CONFIG:
                        if (codec != null)
                            queue(codec, r.body, 0, r.body.length, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG, info);
                        break;
                    case Proto.FRAME: {
                        if (codec == null) break;
                        long pts = b.getLong();
                        int flags = (b.get() & 1) != 0 ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
                        queue(codec, r.body, 9, r.body.length - 9, pts, flags, info);
                        break;
                    }
                    case Proto.END:
                        ended.ended(r.body.length > 1 ? Proto.utf8(r.body, 1) : "the video ended");
                        return;
                    default:
                        break;  // a record from a newer agent: skipped
                }
            }
            ended.ended("the computer closed the video");
        } catch (IOException | RuntimeException e) {
            ended.ended("video: " + e.getMessage());
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (RuntimeException ignored) {
                }
                codec.release();
            }
        }
    }

    /** One sample in, and every finished frame out to the surface. */
    private static void queue(MediaCodec codec, byte[] data, int off, int len, long pts, int flags,
                              MediaCodec.BufferInfo info) {
        int idx;
        while ((idx = codec.dequeueInputBuffer(10_000)) < 0) drain(codec, info, true);
        ByteBuffer buf = codec.getInputBuffer(idx);
        buf.clear();
        buf.put(data, off, len);
        codec.queueInputBuffer(idx, 0, len, pts, flags);
        drain(codec, info, true);
    }

    private static void drain(MediaCodec codec, MediaCodec.BufferInfo info, boolean render) {
        int out;
        while ((out = codec.dequeueOutputBuffer(info, 0)) >= 0 || out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            if (out >= 0) codec.releaseOutputBuffer(out, render);
        }
    }

    /** The audio connection to the speaker, until it ends. Opus only: what the agents send. */
    static void audio(InputStream socket, Ended ended) {
        DataInputStream in = new DataInputStream(socket);
        MediaCodec codec = null;
        AudioTrack track = null;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            for (Proto.Record r; (r = Proto.readRecord(in, Proto.STREAM_LIMIT)) != null; ) {
                switch (r.type) {
                    case Proto.START:
                        if ((r.body[0] & 0xff) != Proto.CODEC_OPUS) {
                            ended.ended("audio in a codec this app does not play");
                            return;
                        }
                        break;
                    case Proto.CONFIG: {
                        // OpusHead (RFC 7845); MediaCodec also wants the pre-skip and the seek
                        // pre-roll as nanoseconds, little-endian.
                        int preSkip = (r.body[10] & 0xff) | ((r.body[11] & 0xff) << 8);
                        MediaFormat f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, 48000, 2);
                        f.setByteBuffer("csd-0", ByteBuffer.wrap(r.body));
                        f.setByteBuffer("csd-1", nanos(preSkip * 1_000_000_000L / 48000));
                        f.setByteBuffer("csd-2", nanos(80_000_000L));
                        codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
                        codec.configure(f, null, null, 0);
                        codec.start();
                        track = new AudioTrack.Builder()
                                .setAudioAttributes(new AudioAttributes.Builder()
                                        .setUsage(AudioAttributes.USAGE_MEDIA)
                                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                                .setAudioFormat(new AudioFormat.Builder()
                                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                        .setSampleRate(48000)
                                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                                .setBufferSizeInBytes(AudioTrack.getMinBufferSize(48000,
                                        AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT) * 2)
                                .setTransferMode(AudioTrack.MODE_STREAM)
                                .build();
                        track.play();
                        break;
                    }
                    case Proto.FRAME: {
                        if (codec == null) break;
                        long pts = ByteBuffer.wrap(r.body).getLong();
                        int idx;
                        while ((idx = codec.dequeueInputBuffer(10_000)) < 0) play(codec, track, info);
                        ByteBuffer buf = codec.getInputBuffer(idx);
                        buf.clear();
                        buf.put(r.body, 9, r.body.length - 9);
                        codec.queueInputBuffer(idx, 0, r.body.length - 9, pts, 0);
                        play(codec, track, info);
                        break;
                    }
                    case Proto.END:
                        return;  // no audio from this source: the session goes on without it
                    default:
                        break;
                }
            }
        } catch (IOException | RuntimeException e) {
            // Losing the sound never costs the picture.
        } finally {
            if (codec != null) codec.release();
            if (track != null) track.release();
        }
    }

    private static void play(MediaCodec codec, AudioTrack track, MediaCodec.BufferInfo info) {
        int out;
        while ((out = codec.dequeueOutputBuffer(info, 0)) >= 0 || out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            if (out < 0) continue;
            ByteBuffer pcm = codec.getOutputBuffer(out);
            if (pcm != null && info.size > 0) {
                pcm.position(info.offset).limit(info.offset + info.size);
                track.write(pcm, info.size, AudioTrack.WRITE_BLOCKING);
            }
            codec.releaseOutputBuffer(out, false);
        }
    }

    private static ByteBuffer nanos(long ns) {
        return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(0, ns);
    }
}
