// bd remora-e5x.37 item 4: decode a real stream through a NAMED codec2 component and write the
// decoded frames out, so host-side decode can be verified by frame CONTENT rather than by the
// absence of an error. The host half's standard is pixel-exactness against a software reference
// decode; this is what lets the guest half be held to the same one.
//
// WHY THIS EXISTS AT ALL, given AOSP ships two command-line decoders: neither is usable on A17.
// `stagefright -N <component>` names a component correctly (SimpleDecodingSource ->
// MediaCodec::CreateByComponentName, which bypasses the codec list) but its decode loop never
// retrieves output — for c2.android.avc.decoder just as much as for ours, so it is the tool that
// is broken, not the component. `codec -v -R` selects by rank (ours wins at 260) but hangs before
// rendering on both the remote and the local decoder. And `stagefright -t` works but goes through
// MediaMetadataRetriever, which hardcodes a SOFTWARE preference for frame extraction and so never
// touches a hardware component at all.
//
// The loop below is deliberately explicit — every dequeue status is reported — because the failure
// this fixture exists to catch is "frames are produced but never surface", which looks exactly like
// a hang from the outside.
//
// Build:  do-build.sh --module remora-decode-test    (not in PRODUCT_PACKAGES; never ships)
// Usage:  remora-decode-test [-N component] [-o out.yuv] [-m frames] <input.mp4>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <binder/ProcessState.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

// Long enough that a real stall is distinguishable from ordinary codec latency, short enough that
// a wedged component is reported rather than waited on forever.
#define DEQUEUE_TIMEOUT_US 200000
#define STALL_LIMIT_US (10 * 1000000LL)

static int
usage(const char *me)
{
   fprintf(stderr,
           "usage: %s [-N component] [-o out.yuv] [-m frames] <input.mp4>\n"
           "  -N  create this component BY NAME (skips codec-list selection)\n"
           "  -o  write decoded frames as planar I420, cropped to the visible rect\n"
           "  -m  stop after this many frames\n",
           me);
   return 2;
}

// Copy one plane out of the codec's output buffer, dropping the alignment padding: the component
// reports its own stride and slice height, and a flat copy of the whole buffer would shear the
// image and make a byte-exact comparison against a reference decode impossible.
static void
write_plane(FILE *out, const uint8_t *src, int32_t stride, int32_t width, int32_t height)
{
   for (int32_t y = 0; y < height; ++y)
      fwrite(src + (size_t)y * stride, 1, (size_t)width, out);
}

int
main(int argc, char **argv)
{
   // MANDATORY, and the reason this is C++ rather than a plain NDK C file. codec2's buffer pool
   // returns blocks to the component over binder; with no thread pool in this process there is
   // nobody to service those transactions, so blocks are fetched and never recycled and output
   // stops dead once the pool is drained — at 25 frames here, on the local and remote decoders
   // alike, which is exactly the shape of a component bug and is not one (bd remora-e5x.38). An app
   // gets this for free from its runtime; a bare CLI does not.
   android::ProcessState::self()->startThreadPool();

   const char *component = NULL;
   const char *out_path = NULL;
   int max_frames = 0;
   int opt;

   while ((opt = getopt(argc, argv, "N:o:m:")) != -1) {
      switch (opt) {
      case 'N': component = optarg; break;
      case 'o': out_path = optarg; break;
      case 'm': max_frames = atoi(optarg); break;
      default: return usage(argv[0]);
      }
   }
   if (optind >= argc)
      return usage(argv[0]);
   const char *path = argv[optind];

   const int fd = open(path, O_RDONLY);
   if (fd < 0) {
      fprintf(stderr, "open %s: %s\n", path, strerror(errno));
      return 1;
   }
   off_t sz = lseek(fd, 0, SEEK_END);
   lseek(fd, 0, SEEK_SET);

   AMediaExtractor *ex = AMediaExtractor_new();
   media_status_t st = AMediaExtractor_setDataSourceFd(ex, fd, 0, sz);
   if (st != AMEDIA_OK) {
      fprintf(stderr, "setDataSourceFd: %d\n", st);
      return 1;
   }

   // First video track wins; this fixture is about video and an mp4 here has at most one.
   const size_t ntracks = AMediaExtractor_getTrackCount(ex);
   AMediaFormat *fmt = NULL;
   const char *mime = NULL;
   size_t track = ntracks;
   for (size_t i = 0; i < ntracks; ++i) {
      AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
      const char *m = NULL;
      if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && strncmp(m, "video/", 6) == 0) {
         fmt = f;
         mime = m;
         track = i;
         break;
      }
      AMediaFormat_delete(f);
   }
   if (track == ntracks) {
      fprintf(stderr, "no video track in %s\n", path);
      return 1;
   }
   AMediaExtractor_selectTrack(ex, track);
   printf("track %zu: %s\n", track, mime);

   AMediaCodec *codec = component ? AMediaCodec_createCodecByName(component)
                                  : AMediaCodec_createDecoderByType(mime);
   if (!codec) {
      fprintf(stderr, "could not create %s\n", component ? component : mime);
      return 1;
   }
   printf("component: %s\n", component ? component : "(by type, codec-list rank)");

   // NULL surface: ByteBuffer mode, which is the only way to get at the pixels for comparison.
   st = AMediaCodec_configure(codec, fmt, NULL, NULL, 0);
   if (st != AMEDIA_OK) {
      fprintf(stderr, "configure: %d\n", st);
      return 1;
   }
   if ((st = AMediaCodec_start(codec)) != AMEDIA_OK) {
      fprintf(stderr, "start: %d\n", st);
      return 1;
   }

   FILE *out = NULL;
   if (out_path && !(out = fopen(out_path, "wb"))) {
      fprintf(stderr, "fopen %s: %s\n", out_path, strerror(errno));
      return 1;
   }

   int32_t width = 0, height = 0, stride = 0, slice_height = 0, color_format = 0;
   int frames = 0, in_bufs = 0;
   bool input_done = false, output_done = false;
   int64_t stalled_us = 0;

   while (!output_done) {
      if (!input_done) {
         const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, DEQUEUE_TIMEOUT_US);
         if (idx >= 0) {
            size_t cap = 0;
            uint8_t *buf = AMediaCodec_getInputBuffer(codec, (size_t)idx, &cap);
            const ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
            if (n < 0) {
               AMediaCodec_queueInputBuffer(codec, (size_t)idx, 0, 0, 0,
                                            AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
               input_done = true;
            } else {
               AMediaCodec_queueInputBuffer(codec, (size_t)idx, 0, (size_t)n,
                                            AMediaExtractor_getSampleTime(ex), 0);
               AMediaExtractor_advance(ex);
               ++in_bufs;
            }
         }
      }

      AMediaCodecBufferInfo info;
      const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, DEQUEUE_TIMEOUT_US);
      if (idx >= 0) {
         stalled_us = 0;
         if (info.size > 0 && out && width > 0) {
            size_t cap = 0;
            const uint8_t *buf = AMediaCodec_getOutputBuffer(codec, (size_t)idx, &cap);
            if (buf) {
               const uint8_t *y = buf + info.offset;
               write_plane(out, y, stride, width, height);
               // COLOR_FormatYUV420Planar: U then V, each at half pitch and half height, starting
               // one slice-height down. Anything else is reported rather than written wrong.
               if (color_format == 19) {
                  const uint8_t *u = y + (size_t)stride * slice_height;
                  const uint8_t *v = u + (size_t)(stride / 2) * (slice_height / 2);
                  write_plane(out, u, stride / 2, width / 2, height / 2);
                  write_plane(out, v, stride / 2, width / 2, height / 2);
               }
            }
         }
         if (info.size > 0)
            ++frames;
         AMediaCodec_releaseOutputBuffer(codec, (size_t)idx, false);
         if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            output_done = true;
         if (max_frames && frames >= max_frames)
            output_done = true;
      } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
         stalled_us = 0;
         AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
         AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &width);
         AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &height);
         AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format);
         if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride) || stride < width)
            stride = width;
         if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice_height) ||
             slice_height < height)
            slice_height = height;
         printf("output format: %dx%d stride=%d slice-height=%d color-format=%d\n", width, height,
                stride, slice_height, color_format);
         if (color_format != 19)
            fprintf(stderr, "NOTE: color-format %d is not COLOR_FormatYUV420Planar; only the Y "
                            "plane is written\n",
                    color_format);
         AMediaFormat_delete(of);
      } else if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
         // The whole point of the fixture: say so, rather than hanging the way the stock tools do.
         stalled_us += DEQUEUE_TIMEOUT_US;
         if (stalled_us >= STALL_LIMIT_US) {
            fprintf(stderr,
                    "STALLED: no output for %" PRId64 "s after %d input buffers and %d frames\n",
                    stalled_us / 1000000, in_bufs, frames);
            break;
         }
      }
   }

   if (out)
      fclose(out);
   AMediaCodec_stop(codec);
   AMediaCodec_delete(codec);
   AMediaFormat_delete(fmt);
   AMediaExtractor_delete(ex);
   close(fd);

   printf("decoded %d frame(s) from %d input buffer(s)\n", frames, in_bufs);
   return frames > 0 ? 0 : 1;
}
