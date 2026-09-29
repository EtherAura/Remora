// On-device MediaCodec decode probe. Runs in app_process, decodes a video file with a NAMED
// codec component, and prints a machine-readable verdict — the harness for proving a hardware
// decoder actually produces frames (bd remora-e5x.27).
//
// It exists because no app on the image can be made to exercise an arbitrary decoder: Chromium
// never routes AV1 to MediaCodec (it uses its own software decoder — decodingInfo reports
// powerEfficient=false even at 4K), Glimpse rejects file:// URIs, and MediaStore cannot be
// driven from the A17 shell. This asks for the component BY NAME, so there is no negotiation to
// lose.
//
// It decodes to an ImageReader Surface rather than ByteBuffer mode ON PURPOSE: c2-va only takes
// the RGBX_8888 output path for surface-bound sessions (VaapiDecodeComponent::getVideoFramePool
// switches on the block-pool allocator), so a ByteBuffer run would exercise NV12 and prove
// nothing about the path real apps take.
//
// Usage:
//   CLASSPATH=/data/local/tmp/codecprobe.dex app_process / com.remora.CodecProbe \
//       <file> [component-name] [seconds]
//   component-name omitted or "-" => let MediaCodec pick for the track's mime.
//
// Output is one KEY=VALUE per line, plus a final VERDICT=PASS|FAIL line.
//
// The prebuilt classes.dex (codecprobe.dex) is committed next to this file. To rebuild after
// editing (needs the AOSP tree's framework.jar + d8):
//   FW=<tree>/out/soong/.intermediates/frameworks/base/framework/android_common/turbine-combined/framework.jar
//   javac -cp "$FW" -d /tmp/cp CodecProbe.java
//   <tree>/out/host/linux-x86/bin/d8 --min-api 26 --lib "$FW" --output . /tmp/cp/com/remora/CodecProbe.class
//   mv classes.dex codecprobe.dex
package com.remora;

import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Looper;
import android.view.Surface;

import java.nio.ByteBuffer;

public class CodecProbe {

    // A frame is "real" if it is not a flat colour: decoders that fail silently tend to emit
    // black or a constant fill, which a mean alone would not catch.
    private static long framesRendered = 0;
    private static long framesNonBlank = 0;
    private static long firstFrameMs = -1;
    // Mean absolute difference between horizontally adjacent pixels, averaged over frames.
    // Film grain is high-frequency noise, so a grainy stream reads far rougher than the same
    // source encoded without it — which is how "did the GRAINY image reach the output, or did we
    // emit the clean reconstruction by mistake?" gets answered by measurement instead of by
    // assuming the log line means the pixels followed.
    private static double roughnessSum = 0;
    private static long roughnessFrames = 0;

    public static void main(String[] args) {
        int exit = 1;
        try {
            if (args.length < 1) {
                System.out.println("ERROR=usage: <file> [component|-] [seconds] [skipSamples]");
                System.out.println("VERDICT=FAIL");
                System.exit(2);
            }
            final String path = args[0];
            final String want = (args.length > 1 && !"-".equals(args[1])) ? args[1] : null;
            final int seconds = args.length > 2 ? Integer.parseInt(args[2]) : 15;
            // Don't feed the first N samples: the decoder then sees a stream that
            // starts mid-GOP (P frames whose references it never decoded), which is
            // what a codec gets on a bad flush/reuse restart. Container-level
            // surgery can't produce this — every demuxer drops to a sync sample.
            final int skipSamples = args.length > 3 ? Integer.parseInt(args[3]) : 0;

            if (Looper.myLooper() == null) Looper.prepareMainLooper();

            MediaExtractor ex = new MediaExtractor();
            ex.setDataSource(path);
            int track = -1;
            MediaFormat fmt = null;
            for (int i = 0; i < ex.getTrackCount(); i++) {
                MediaFormat f = ex.getTrackFormat(i);
                String mime = f.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("video/")) { track = i; fmt = f; break; }
            }
            if (track < 0) {
                System.out.println("ERROR=no video track in " + path);
                System.out.println("VERDICT=FAIL");
                System.exit(2);
            }
            ex.selectTrack(track);

            final int width = fmt.getInteger(MediaFormat.KEY_WIDTH);
            final int height = fmt.getInteger(MediaFormat.KEY_HEIGHT);
            System.out.println("FILE=" + path);
            System.out.println("MIME=" + fmt.getString(MediaFormat.KEY_MIME));
            System.out.println("SIZE=" + width + "x" + height);

            // RGBA_8888 matches what the surface-bound c2-va path produces, so the buffers come
            // back readable without an extra conversion. maxImages 4: enough to keep the decoder
            // from starving while we read one at a time.
            ImageReader reader = ImageReader.newInstance(width, height, android.graphics.PixelFormat.RGBA_8888, 4);
            Surface surface = reader.getSurface();
            // The callback needs a looper that is actually RUNNING. Handing it null would bind it
            // to this thread's looper, which never loops (the decode drives the main thread), so
            // no image would ever be delivered and a perfectly good decode would read as zero
            // frames rendered.
            android.os.HandlerThread readerThread = new android.os.HandlerThread("probe-reader");
            readerThread.start();
            android.os.Handler readerHandler = new android.os.Handler(readerThread.getLooper());
            reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                @Override public void onImageAvailable(ImageReader r) {
                    Image img = null;
                    try {
                        // acquireLatestImage COALESCES: if several frames queued up while this
                        // callback was busy, the older ones are dropped and RENDERED can land a
                        // frame or two under DEQUEUED at high resolutions. That is this reader
                        // pacing itself, not the decoder dropping output — compare DEQUEUED,
                        // which counts every buffer the codec actually produced.
                        img = r.acquireLatestImage();
                        if (img == null) return;
                        framesRendered++;
                        if (firstFrameMs < 0) firstFrameMs = System.currentTimeMillis();
                        if (!isBlank(img)) framesNonBlank++;
                        double rough = roughness(img);
                        if (rough >= 0) { roughnessSum += rough; roughnessFrames++; }
                    } catch (Throwable t) {
                        // A read failure is not a decode failure; keep counting frames.
                    } finally {
                        if (img != null) img.close();
                    }
                }
            }, readerHandler);

            MediaCodec codec = (want != null) ? MediaCodec.createByCodecName(want)
                                              : MediaCodec.createDecoderByType(fmt.getString(MediaFormat.KEY_MIME));
            System.out.println("COMPONENT=" + codec.getName());
            codec.configure(fmt, surface, null, 0);
            codec.start();

            for (int s = 0; s < skipSamples; s++) {
                if (!ex.advance()) break;
            }
            if (skipSamples > 0) System.out.println("SKIPPED_INPUT_SAMPLES=" + skipSamples);

            final long deadline = System.currentTimeMillis() + seconds * 1000L;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean sawInputEos = false, sawOutputEos = false;
            long queued = 0, dequeued = 0;

            while (!sawOutputEos && System.currentTimeMillis() < deadline) {
                if (!sawInputEos) {
                    int in = codec.dequeueInputBuffer(10000);
                    if (in >= 0) {
                        ByteBuffer buf = codec.getInputBuffer(in);
                        int sz = ex.readSampleData(buf, 0);
                        if (sz < 0) {
                            codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            sawInputEos = true;
                        } else {
                            codec.queueInputBuffer(in, 0, sz, ex.getSampleTime(), 0);
                            queued++;
                            ex.advance();
                        }
                    }
                }
                int out = codec.dequeueOutputBuffer(info, 10000);
                if (out >= 0) {
                    dequeued++;
                    // render=true pushes the frame to the ImageReader surface.
                    codec.releaseOutputBuffer(out, true);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) sawOutputEos = true;
                } else if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    System.out.println("OUTPUT_FORMAT=" + codec.getOutputFormat());
                }
            }
            // The listener runs on the caller's looper thread; give the last frames a moment.
            try { Thread.sleep(300); } catch (InterruptedException ie) { }

            System.out.println("QUEUED=" + queued);
            System.out.println("DEQUEUED=" + dequeued);
            System.out.println("RENDERED=" + framesRendered);
            System.out.println("NONBLANK=" + framesNonBlank);
            System.out.println("ROUGHNESS=" + (roughnessFrames > 0
                    ? String.format("%.3f", roughnessSum / roughnessFrames) : "n/a"));
            System.out.println("EOS=" + sawOutputEos);

            codec.stop();
            codec.release();
            reader.close();
            readerThread.quitSafely();
            ex.release();

            // PASS needs real decoded output, not merely a component that started: frames must
            // have come out AND at least one must carry actual picture content.
            boolean pass = dequeued > 0 && framesRendered > 0 && framesNonBlank > 0;
            System.out.println("VERDICT=" + (pass ? "PASS" : "FAIL"));
            exit = pass ? 0 : 1;
        } catch (Throwable t) {
            System.out.println("ERROR=" + t.getClass().getName() + ": " + t.getMessage());
            for (StackTraceElement e : t.getStackTrace()) System.out.println("  at " + e);
            System.out.println("VERDICT=FAIL");
            exit = 1;
        }
        System.exit(exit);
    }

    // Mean |R(x) - R(x+1)| over a sample of rows. Cheap, resolution-independent, and it does not
    // care what the picture is — only how noisy it is.
    private static double roughness(Image img) {
        Image.Plane p = img.getPlanes()[0];
        ByteBuffer b = p.getBuffer();
        int rowStride = p.getRowStride();
        int pixStride = p.getPixelStride();
        int w = img.getWidth(), h = img.getHeight();
        long acc = 0, n = 0;
        int rowStep = Math.max(1, h / 32);
        for (int y = 0; y < h; y += rowStep) {
            int base = y * rowStride;
            for (int x = 0; x + 1 < w; x++) {
                int o1 = base + x * pixStride, o2 = base + (x + 1) * pixStride;
                if (o2 + 1 >= b.limit()) break;
                acc += Math.abs((b.get(o1) & 0xff) - (b.get(o2) & 0xff));
                n++;
            }
        }
        return n > 0 ? (double) acc / n : -1;
    }

    // Sample a grid of pixels; blank = every sample identical. Cheap and catches the two silent
    // failure modes that matter (all-black, constant fill) without decoding semantics.
    private static boolean isBlank(Image img) {
        Image.Plane p = img.getPlanes()[0];
        ByteBuffer b = p.getBuffer();
        int rowStride = p.getRowStride();
        int pixStride = p.getPixelStride();
        int w = img.getWidth(), h = img.getHeight();
        int first = Integer.MIN_VALUE;
        for (int y = 0; y < h; y += Math.max(1, h / 16)) {
            for (int x = 0; x < w; x += Math.max(1, w / 16)) {
                int off = y * rowStride + x * pixStride;
                if (off + 3 >= b.limit()) continue;
                int px = (b.get(off) & 0xff) | ((b.get(off + 1) & 0xff) << 8)
                         | ((b.get(off + 2) & 0xff) << 16);
                if (first == Integer.MIN_VALUE) first = px;
                else if (px != first) return false;
            }
        }
        return true;
    }
}
