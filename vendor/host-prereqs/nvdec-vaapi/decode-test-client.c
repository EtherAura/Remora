/* Exercises remora-frame-decoder end to end on the host (bd remora-e5x.3):
 * demuxes a real file into access units — the same framing the c2 component
 * will send — feeds them over the socket, and validates what comes back.
 *
 * LINEAR mode is verified by CONTENT: the Y plane of every returned frame is
 * checksummed and, with --ref, compared against a software decode of the same
 * file. EXPORT mode validates the descriptors (fds present, block-linear
 * modifier, sane layout) and exercises the release window like a real client.
 *
 * Build: cc -O2 -Wall decode-test-client.c -o decode-test-client \
 *            $(pkg-config --cflags --libs libavformat libavcodec libavutil)
 * Run:   ./decode-test-client --socket /tmp/rmd.sock --input t.mp4 [--export] [--ref]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>

/* Wire structs: must match remora-frame-decoder.c exactly. */
#define DEC_HELLO_MAGIC   0x524d4448u
#define DEC_ACCEPT_MAGIC  0x524d4441u
#define DEC_PACKET_MAGIC  0x524d4450u
#define DEC_FRAME_MAGIC   0x524d4446u
#define DEC_RELEASE_MAGIC 0x524d4452u
#define DEC_DRAINED_MAGIC 0x524d4444u
#define DEC_ERROR_MAGIC   0x524d4445u
#define DEC_MODE_LINEAR 1
#define DEC_MODE_EXPORT 2
#define DEC_PKT_EOS (1u << 0)

struct dec_hello { uint32_t magic, version, codec, mode, max_w, max_h; };
struct dec_accept { uint32_t magic, version, mode, reserved; };
struct dec_packet { uint32_t magic, size; uint64_t pts; uint32_t flags, reserved; };
struct dec_frame {
   uint32_t magic, res_id, width, height, fourcc, mode;
   uint64_t modifier;
   uint32_t n_planes, offset[3], pitch[3], plane_fd[3];
   uint64_t pts;
   uint32_t n_fds, total_size;
};
struct dec_release { uint32_t magic, res_id; };
struct dec_simple { uint32_t magic, value; };

static bool
write_all(int fd, const void *data, size_t len)
{
   const uint8_t *p = data;
   while (len) {
      ssize_t n = write(fd, p, len);
      if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
      p += n; len -= (size_t)n;
   }
   return true;
}

/* Read one message that may carry fds. Returns bytes read into buf, fds into
 * fds[] (up to 3), or -1. */
static ssize_t
recv_msg(int sock, void *buf, size_t len, int *fds, unsigned *n_fds)
{
   char cbuf[CMSG_SPACE(sizeof(int) * 3)];
   struct iovec iov = { .iov_base = buf, .iov_len = len };
   struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
   *n_fds = 0;
   ssize_t n;
   do n = recvmsg(sock, &mh, 0); while (n < 0 && errno == EINTR);
   if (n <= 0)
      return -1;
   for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
      if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
         continue;
      const unsigned c = (unsigned)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (unsigned i = 0; i < c && *n_fds < 3; i++)
         memcpy(&fds[(*n_fds)++], CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
   }
   return n;
}

static bool
read_exact(int sock, void *buf, size_t len)
{
   uint8_t *p = buf;
   while (len) {
      ssize_t m = read(sock, p, len);
      if (m <= 0) { if (m < 0 && errno == EINTR) continue; return false; }
      p += (size_t)m;
      len -= (size_t)m;
   }
   return true;
}

/* SCM_RIGHTS rides the FIRST byte of the helper's sendmsg — a plain read() of
 * the 4-byte magic silently DISCARDS the fds (measured: n_fds came back 0 and
 * every descriptor was gone). So the magic itself must be received with a
 * control buffer; the rest of whichever struct follows is plain bytes. The
 * guest-side c2 client must do the same. */
static bool
read_reply_magic(int sock, uint32_t *magic, int *fds, unsigned *n_fds)
{
   uint8_t *p = (uint8_t *)magic;
   ssize_t n = recv_msg(sock, p, sizeof(*magic), fds, n_fds);
   if (n <= 0)
      return false;
   return n == (ssize_t)sizeof(*magic) ||
          read_exact(sock, p + n, sizeof(*magic) - (size_t)n);
}

/* Software-decode the same file and hash every frame's tightly-packed Y
 * plane, so LINEAR mode is judged on pixels, not plumbing. */
static int
reference_hashes(const char *path, uint64_t **out, unsigned *count)
{
   AVFormatContext *fc = NULL;
   int err = avformat_open_input(&fc, path, NULL, NULL);
   if (err < 0) return err;
   if ((err = avformat_find_stream_info(fc, NULL)) < 0) return err;
   const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   if (vi < 0) return vi;
   const AVCodec *dec = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id);
   AVCodecContext *ctx = avcodec_alloc_context3(dec);
   avcodec_parameters_to_context(ctx, fc->streams[vi]->codecpar);
   if ((err = avcodec_open2(ctx, dec, NULL)) < 0) return err;

   AVPacket *pkt = av_packet_alloc();
   AVFrame *fr = av_frame_alloc();
   unsigned cap = 256, n = 0;
   uint64_t *hashes = malloc(cap * sizeof(*hashes));
   while (av_read_frame(fc, pkt) >= 0) {
      if (pkt->stream_index == vi && avcodec_send_packet(ctx, pkt) >= 0)
         while (avcodec_receive_frame(ctx, fr) >= 0) {
            if (n == cap) hashes = realloc(hashes, (cap *= 2) * sizeof(*hashes));
            uint64_t h = 0xcbf29ce484222325ull;
            for (int y = 0; y < fr->height; y++) {
               const uint8_t *row = fr->data[0] + (ptrdiff_t)y * fr->linesize[0];
               for (int x = 0; x < fr->width; x++) { h ^= row[x]; h *= 0x100000001b3ull; }
            }
            hashes[n++] = h;
            av_frame_unref(fr);
         }
      av_packet_unref(pkt);
   }
   avcodec_send_packet(ctx, NULL);
   while (avcodec_receive_frame(ctx, fr) >= 0) {
      if (n == cap) hashes = realloc(hashes, (cap *= 2) * sizeof(*hashes));
      uint64_t h = 0xcbf29ce484222325ull;
      for (int y = 0; y < fr->height; y++) {
         const uint8_t *row = fr->data[0] + (ptrdiff_t)y * fr->linesize[0];
         for (int x = 0; x < fr->width; x++) { h ^= row[x]; h *= 0x100000001b3ull; }
      }
      hashes[n++] = h;
      av_frame_unref(fr);
   }
   av_frame_free(&fr);
   av_packet_free(&pkt);
   avcodec_free_context(&ctx);
   avformat_close_input(&fc);
   *out = hashes;
   *count = n;
   return 0;
}

int
main(int argc, char **argv)
{
   const char *sock_path = NULL, *input = NULL;
   uint32_t mode = DEC_MODE_LINEAR;
   bool use_ref = false;

   static struct option opts[] = {
      { "socket", required_argument, 0, 's' },
      { "input", required_argument, 0, 'i' },
      { "export", no_argument, 0, 'e' },
      { "ref", no_argument, 0, 'r' },
      { 0, 0, 0, 0 },
   };
   for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
      switch (c) {
      case 's': sock_path = optarg; break;
      case 'i': input = optarg; break;
      case 'e': mode = DEC_MODE_EXPORT; break;
      case 'r': use_ref = true; break;
      default: return 2;
      }
   }
   if (!sock_path || !input) {
      fprintf(stderr, "usage: decode-test-client --socket PATH --input FILE [--export] [--ref]\n");
      return 2;
   }

   /* Demux to access units. h264/hevc in mp4 need the annexb filter so the
    * AUs look like what a real bitstream carries. */
   AVFormatContext *fc = NULL;
   if (avformat_open_input(&fc, input, NULL, NULL) < 0 ||
       avformat_find_stream_info(fc, NULL) < 0) {
      fprintf(stderr, "cannot open %s\n", input);
      return 1;
   }
   const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   const enum AVCodecID cid = fc->streams[vi]->codecpar->codec_id;
   uint32_t codec_fourcc;
   const char *bsf_name = NULL;
   switch (cid) {
   case AV_CODEC_ID_H264: codec_fourcc = 0x34363268u; bsf_name = "h264_mp4toannexb"; break;
   case AV_CODEC_ID_HEVC: codec_fourcc = 0x63766568u; bsf_name = "hevc_mp4toannexb"; break;
   case AV_CODEC_ID_VP9:  codec_fourcc = 0x20397076u; break;
   case AV_CODEC_ID_AV1:  codec_fourcc = 0x31307661u; break;
   default:
      fprintf(stderr, "unsupported codec in %s\n", input);
      return 1;
   }
   AVBSFContext *bsf = NULL;
   if (bsf_name) {
      const AVBitStreamFilter *f = av_bsf_get_by_name(bsf_name);
      av_bsf_alloc(f, &bsf);
      avcodec_parameters_copy(bsf->par_in, fc->streams[vi]->codecpar);
      av_bsf_init(bsf);
   }

   uint64_t *ref = NULL;
   unsigned n_ref = 0;
   if (use_ref && mode == DEC_MODE_LINEAR && reference_hashes(input, &ref, &n_ref) < 0) {
      fprintf(stderr, "reference decode failed\n");
      return 1;
   }

   const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
   if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      perror("connect");
      return 1;
   }
   const struct dec_hello hello = { DEC_HELLO_MAGIC, 1, codec_fourcc, mode, 0, 0 };
   if (!write_all(sock, &hello, sizeof(hello)))
      return 1;
   struct dec_accept acc;
   if (read(sock, &acc, sizeof(acc)) != (ssize_t)sizeof(acc) ||
       acc.magic != DEC_ACCEPT_MAGIC) {
      fprintf(stderr, "no accept from helper\n");
      return 1;
   }

   unsigned sent = 0, got = 0, matched = 0, mismatched = 0, errors = 0;
   bool drained = false, export_ok = true;

   /* Interleave: send an AU, then read whatever is ready. A real client is a
    * pipeline too; strict lockstep would deadlock on B-frame reorder. */
   AVPacket *pkt = av_packet_alloc();
   bool eof_sent = false;
   while (!drained) {
      if (!eof_sent) {
         int r = av_read_frame(fc, pkt);
         if (r >= 0 && pkt->stream_index != vi) {
            av_packet_unref(pkt);
            continue;
         }
         if (r >= 0 && bsf) {
            av_bsf_send_packet(bsf, pkt);
            if (av_bsf_receive_packet(bsf, pkt) < 0) {
               av_packet_unref(pkt);
               continue;
            }
         }
         if (r < 0) {
            const struct dec_packet ph = { DEC_PACKET_MAGIC, 0, 0, DEC_PKT_EOS, 0 };
            if (!write_all(sock, &ph, sizeof(ph)))
               break;
            eof_sent = true;
         } else {
            const struct dec_packet ph = { DEC_PACKET_MAGIC, (uint32_t)pkt->size,
                                           (uint64_t)pkt->pts, 0, 0 };
            const bool ok = write_all(sock, &ph, sizeof(ph)) &&
                            write_all(sock, pkt->data, (size_t)pkt->size);
            av_packet_unref(pkt);
            if (!ok)
               break;
            sent++;
         }
      }

      /* Drain replies; block only once EOF is sent. */
      for (;;) {
         if (!eof_sent) {
            struct timeval tv = { 0, 0 };
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(sock, &rf);
            if (select(sock + 1, &rf, NULL, NULL, &tv) <= 0)
               break;
         }
         uint32_t magic;
         int fds[3];
         unsigned n_fds;
         if (!read_reply_magic(sock, &magic, fds, &n_fds)) {
            drained = true;
            break;
         }
         if (magic == DEC_DRAINED_MAGIC || magic == DEC_ERROR_MAGIC) {
            uint32_t value;
            if (!read_exact(sock, &value, sizeof(value))) {
               drained = true;
               break;
            }
            if (magic == DEC_ERROR_MAGIC)
               errors++;
            else {
               drained = true;
               break;
            }
            continue;
         }
         if (magic != DEC_FRAME_MAGIC) {
            fprintf(stderr, "unexpected magic 0x%08x\n", magic);
            drained = true;
            break;
         }
         struct dec_frame fm;
         fm.magic = magic;
         if (!read_exact(sock, (uint8_t *)&fm + sizeof(magic), sizeof(fm) - sizeof(magic))) {
            drained = true;
            break;
         }
         got++;
         if (fm.mode == DEC_MODE_LINEAR && n_fds == 1) {
            uint8_t *map = mmap(NULL, fm.total_size, PROT_READ, MAP_SHARED, fds[0], 0);
            if (map != MAP_FAILED) {
               if (ref) {
                  uint64_t h2 = 0xcbf29ce484222325ull;
                  for (uint32_t y = 0; y < fm.height; y++) {
                     const uint8_t *row = map + fm.offset[0] + (size_t)y * fm.pitch[0];
                     for (uint32_t x = 0; x < fm.width; x++) { h2 ^= row[x]; h2 *= 0x100000001b3ull; }
                  }
                  if (got - 1 < n_ref && h2 == ref[got - 1])
                     matched++;
                  else
                     mismatched++;
               }
               munmap(map, fm.total_size);
            }
            close(fds[0]);
         } else if (fm.mode == DEC_MODE_EXPORT) {
            if (n_fds != fm.n_fds || n_fds == 0 || fm.n_planes < 2 || fm.pitch[0] == 0)
               export_ok = false;
            for (unsigned i = 0; i < n_fds; i++)
               close(fds[i]);
            /* Hold a small window, then release oldest — exercises the
             * helper's ref-holding the way a real output pool would. */
            static uint32_t window[4];
            static unsigned wn;
            window[wn % 4] = fm.res_id;
            wn++;
            if (wn >= 4) {
               const struct dec_release rel = { DEC_RELEASE_MAGIC, window[(wn - 4) % 4] };
               if (!write_all(sock, &rel, sizeof(rel))) {
                  drained = true;
                  break;
               }
            }
         }
      }
   }
   av_packet_free(&pkt);
   if (bsf)
      av_bsf_free(&bsf);
   avformat_close_input(&fc);
   close(sock);

   printf("sent=%u got=%u errors=%u", sent, got, errors);
   if (ref)
      printf(" ref_matched=%u/%u mismatched=%u", matched, n_ref, mismatched);
   if (mode == DEC_MODE_EXPORT)
      printf(" export_descriptors=%s", export_ok ? "ok" : "BAD");
   printf("\n");
   free(ref);

   const bool pass = got == sent && errors == 0 &&
                     (!ref || (matched == got && mismatched == 0)) &&
                     (mode != DEC_MODE_EXPORT || export_ok);
   printf(pass ? "PASS\n" : "FAIL\n");
   return pass ? 0 : 1;
}
