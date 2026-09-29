/*
 * Host-side video decoder for in-guest playback (bd remora-e5x.3).
 *
 * The compose encoder run in reverse: c2-va's remote-decoder backend ships
 * access units over a bind-mounted unix socket, this helper decodes them on
 * the host's hardware (NVDEC via the NVIDIA VA driver, or any other libva
 * driver — the helper is driver-agnostic), and decoded NV12 frames travel
 * back as fds. NVIDIA cannot be reached from bionic — libcuda/libnvcuvid are
 * proprietary glibc binaries the VA driver dlopens — so the decode has to
 * live in a host process; the container shares the kernel, which makes
 * SCM_RIGHTS fd passing across the boundary ordinary (the compose publisher
 * has proven every piece of this plumbing in the other direction).
 *
 * Two frame-return modes, negotiated at hello:
 *   LINEAR — the helper downloads the frame and sends a memfd holding
 *     tightly-described NV12 (offsets/pitches in the message). Universally
 *     consumable: the guest mmaps and copies into its C2 block, the
 *     kv1-equivalent cost floor. The memfd is the client's to close; the
 *     helper forgets it on send.
 *   EXPORT — vaExportSurfaceHandle dma_bufs, zero-copy. The frame's memory
 *     stays alive while the client holds the fds, but the DECODER RECYCLES
 *     the surface for a later frame unless the helper keeps the AVFrame
 *     referenced — so the client must send RELEASE(res_id) when done, and
 *     the helper holds the ref until then. Called directly, not through
 *     ffmpeg's hwmap: that path returns ENOSYS on this host without ever
 *     reaching libva, for every driver (see host-prereqs/nvdec-vaapi/).
 *
 * Input framing is ACCESS UNITS, one per packet message — exactly what the
 * c2 component receives from CCodec — so there is no parser here and no
 * ambiguity about frame boundaries. Annex-B or length-prefixed per codec
 * convention; whatever the container hands c2 is what crosses the wire.
 *
 * Build: make host-decoder (opt-in, like host-encoder — links ffmpeg + libva).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include <va/va.h>
#include <va/va_drmcommon.h>

/* ------------------------------------------------------------------- wire */

#define DEC_HELLO_MAGIC   0x524d4448u /* "RMDH" client hello */
#define DEC_ACCEPT_MAGIC  0x524d4441u /* "RMDA" helper reply */
#define DEC_PACKET_MAGIC  0x524d4450u /* "RMDP" access unit in */
#define DEC_FRAME_MAGIC   0x524d4446u /* "RMDF" decoded frame out (fds ride it) */
#define DEC_RELEASE_MAGIC 0x524d4452u /* "RMDR" client done with an EXPORT frame */
#define DEC_DRAINED_MAGIC 0x524d4444u /* "RMDD" every frame for the EOS flush is out */
#define DEC_ERROR_MAGIC   0x524d4445u /* "RMDE" per-AU decode failure, stream continues */

#define DEC_MODE_LINEAR 1
#define DEC_MODE_EXPORT 2

#define DEC_PKT_EOS    (1u << 0) /* drain: every buffered frame comes out, then RMDD */
#define DEC_PKT_CONFIG (1u << 1) /* codec config (SPS/PPS...) — decoded, never answered */

/* Native byte order and padding: both ends are the same machine by
 * construction, same rule as the encoder's frame_msg. */
struct dec_hello {
   uint32_t magic, version, codec, mode; /* codec: "h264" "hevc" "vp9 " "av01" */
   uint32_t max_w, max_h;                /* 0 = unknown, sizes come from the stream */
};

struct dec_accept {
   uint32_t magic, version, mode, reserved;
};

struct dec_packet {
   uint32_t magic, size;
   uint64_t pts;
   uint32_t flags, reserved;
   /* size bytes of payload follow */
};

/* The frame's fds ride the SAME sendmsg as this struct, which means they are
 * delivered with its FIRST byte: a client that reads the magic with a plain
 * read() and only then recvmsg()s the rest has already discarded the
 * descriptors (the kernel closes them, silently — measured). Receive the
 * magic itself with a control buffer. */
struct dec_frame {
   uint32_t magic, res_id;
   uint32_t width, height;
   uint32_t fourcc, mode;   /* fourcc: "NV12" */
   uint64_t modifier;       /* EXPORT: from the driver; LINEAR: 0 */
   uint32_t n_planes;
   uint32_t offset[3], pitch[3];
   uint32_t plane_fd[3];    /* which of the passed fds each plane lives in */
   uint64_t pts;
   uint32_t n_fds, total_size;
   /* n_fds descriptors ride the same sendmsg as SCM_RIGHTS */
};

struct dec_release {
   uint32_t magic, res_id;
};

struct dec_simple {
   uint32_t magic, value; /* RMDD: 0; RMDE: negative AVERROR as u32 */
};

/* ------------------------------------------------------------------ state */

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{
   (void)sig;
   stop_requested = 1;
}

static void
die(const char *what, int err)
{
   if (err < 0) {
      char buf[128];
      av_strerror(err, buf, sizeof(buf));
      fprintf(stderr, "remora-frame-decoder: %s: %s\n", what, buf);
   } else {
      fprintf(stderr, "remora-frame-decoder: %s: %s\n", what, strerror(errno));
   }
   exit(1);
}

/* Same shape as the encoder's status file, same reason: stderr of a detached
 * helper goes to a per-spawn log, but `remora check` wants the latest state
 * without attaching to anything. A failing exit leaves the file behind on
 * purpose — it is the explanation; a clean exit removes it. */
static char g_status_path[256];

static void
status_write(const char *state, uint32_t codec, int w, int h, uint64_t frames)
{
   if (!g_status_path[0])
      return;
   char tmp[sizeof(g_status_path) + 4];
   snprintf(tmp, sizeof(tmp), "%s.tmp", g_status_path);
   FILE *f = fopen(tmp, "w");
   if (!f)
      return;
   fprintf(f, "state=%s codec=%.4s size=%dx%d frames=%" PRIu64 " pid=%d\n", state,
           (const char *)&codec, w, h, frames, (int)getpid());
   fclose(f);
   rename(tmp, g_status_path);
}

/* In-flight EXPORT frames: the AVFrame ref is what keeps the decoder from
 * recycling the surface while the client still reads the exported dma_bufs.
 * Sized above any sane C2 output-slot count; a client that never releases
 * hits the cap and the stream stops with a loud message rather than serving
 * torn frames. */
#define MAX_INFLIGHT 24
struct inflight {
   AVFrame *frame;
   uint32_t res_id;
};

struct session {
   int sock;
   uint32_t codec, mode;
   AVCodecContext *avctx;
   AVBufferRef *hw_dev;
   AVPacket *pkt;
   AVFrame *frame, *sw;
   struct inflight held[MAX_INFLIGHT];
   unsigned n_held;
   uint32_t next_res_id;
   uint64_t decoded, sent, released, errors;
   int width, height; /* first frame's, for the status file */
};

/* ------------------------------------------------------------------- io */

static bool
write_all(int fd, const void *data, size_t len)
{
   const uint8_t *p = data;
   while (len) {
      const ssize_t n = write(fd, p, len);
      if (n <= 0) {
         if (n < 0 && errno == EINTR)
            continue;
         return false;
      }
      p += n;
      len -= (size_t)n;
   }
   return true;
}

static bool
read_all(int fd, void *data, size_t len)
{
   uint8_t *p = data;
   while (len) {
      const ssize_t n = read(fd, p, len);
      if (n <= 0) {
         if (n < 0 && errno == EINTR)
            continue;
         return false;
      }
      p += n;
      len -= (size_t)n;
   }
   return true;
}

/* One frame message + its fds in one sendmsg, so the descriptors and the
 * layout that describes them cannot arrive separately. */
static bool
send_frame_msg(int sock, const struct dec_frame *fm, const int *fds, unsigned n_fds)
{
   char cbuf[CMSG_SPACE(sizeof(int) * 3)];
   struct iovec iov = { .iov_base = (void *)fm, .iov_len = sizeof(*fm) };
   struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
   if (n_fds) {
      mh.msg_control = cbuf;
      mh.msg_controllen = CMSG_SPACE(sizeof(int) * n_fds);
      struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type = SCM_RIGHTS;
      cm->cmsg_len = CMSG_LEN(sizeof(int) * n_fds);
      memcpy(CMSG_DATA(cm), fds, sizeof(int) * n_fds);
   }
   for (;;) {
      const ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
      if (n == (ssize_t)sizeof(*fm))
         return true;
      if (n < 0 && errno == EINTR)
         continue;
      return false; /* partial control-message sends do not happen on unix sockets */
   }
}

/* --------------------------------------------------------------- decode */

static enum AVPixelFormat
pick_vaapi(AVCodecContext *avctx, const enum AVPixelFormat *fmts)
{
   (void)avctx;
   for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
      if (*p == AV_PIX_FMT_VAAPI)
         return AV_PIX_FMT_VAAPI;
   /* The stream needs a profile this driver cannot decode. Refusing here
    * fails the whole session; the guest's local paths are the fallback. */
   fprintf(stderr, "[decoder] stream requires a profile the VA driver does not offer\n");
   return AV_PIX_FMT_NONE;
}

static const char *
codec_name_for(uint32_t fourcc)
{
   switch (fourcc) {
   case 0x34363268u: return "h264"; /* "h264" */
   case 0x63766568u: return "hevc"; /* "hevc" */
   case 0x20397076u: return "vp9";  /* "vp9 " */
   case 0x31307661u: return "av1";  /* "av01" */
   default: return NULL;
   }
}

static int
session_open_decoder(struct session *s, const char *drm_node)
{
   const char *name = codec_name_for(s->codec);
   if (!name) {
      fprintf(stderr, "[decoder] unknown codec fourcc 0x%08x\n", s->codec);
      return -1;
   }
   const AVCodec *codec = avcodec_find_decoder_by_name(name);
   if (!codec)
      return -1;
   int err = av_hwdevice_ctx_create(&s->hw_dev, AV_HWDEVICE_TYPE_VAAPI, drm_node, NULL, 0);
   if (err < 0) {
      char buf[128];
      av_strerror(err, buf, sizeof(buf));
      fprintf(stderr, "[decoder] no VAAPI device on %s: %s\n", drm_node, buf);
      return err;
   }
   s->avctx = avcodec_alloc_context3(codec);
   if (!s->avctx)
      return AVERROR(ENOMEM);
   s->avctx->hw_device_ctx = av_buffer_ref(s->hw_dev);
   s->avctx->get_format = pick_vaapi;
   /* Headroom for the EXPORT window: frames the client holds are frames the
    * decoder cannot recycle, same arithmetic as the mirror's zero-copy. */
   s->avctx->extra_hw_frames = MAX_INFLIGHT;
   if ((err = avcodec_open2(s->avctx, codec, NULL)) < 0)
      return err;
   fprintf(stderr, "[decoder] %s via VAAPI on %s (%s)\n", name, drm_node,
           s->mode == DEC_MODE_EXPORT ? "export dma_bufs" : "linear memfd");
   return 0;
}

/* EXPORT mode: hand out the surface's own dma_bufs and hold the frame ref
 * until the client releases it. */
static int
send_frame_export(struct session *s, AVFrame *frame)
{
   if (s->n_held >= MAX_INFLIGHT) {
      fprintf(stderr, "[decoder] BUG or stuck client: %u frames held without release — "
                      "refusing to serve a surface the decoder may recycle\n", s->n_held);
      return AVERROR(EBUSY);
   }
   AVHWDeviceContext *dev = (AVHWDeviceContext *)s->hw_dev->data;
   AVVAAPIDeviceContext *va = dev->hwctx;
   const VASurfaceID surface = (VASurfaceID)(uintptr_t)frame->data[3];

   VADRMPRIMESurfaceDescriptor d = { 0 };
   VAStatus vs = vaExportSurfaceHandle(va->display, surface,
                                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                       VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                                          VA_EXPORT_SURFACE_READ_ONLY,
                                       &d);
   if (vs != VA_STATUS_SUCCESS) {
      fprintf(stderr, "[decoder] vaExportSurfaceHandle: %s\n", vaErrorStr(vs));
      return AVERROR_EXTERNAL;
   }

   struct dec_frame fm = {
      .magic = DEC_FRAME_MAGIC,
      .res_id = s->next_res_id++,
      .width = d.width,
      .height = d.height,
      .fourcc = d.fourcc,
      .mode = DEC_MODE_EXPORT,
      .pts = (uint64_t)frame->pts,
      .n_fds = d.num_objects > 3 ? 3 : d.num_objects,
   };
   int fds[3] = { -1, -1, -1 };
   for (unsigned o = 0; o < fm.n_fds; o++) {
      fds[o] = d.objects[o].fd;
      fm.total_size += d.objects[o].size;
      /* One modifier per export in practice; keep the first. */
      if (!o)
         fm.modifier = d.objects[o].drm_format_modifier;
   }
   fm.n_planes = 0;
   for (unsigned l = 0; l < d.num_layers && fm.n_planes < 3; l++)
      for (unsigned p = 0; p < d.layers[l].num_planes && fm.n_planes < 3; p++) {
         fm.offset[fm.n_planes] = d.layers[l].offset[p];
         fm.pitch[fm.n_planes] = d.layers[l].pitch[p];
         fm.plane_fd[fm.n_planes] = d.layers[l].object_index[p];
         fm.n_planes++;
      }

   const bool ok = send_frame_msg(s->sock, &fm, fds, fm.n_fds);
   /* Our copies of the exported fds are sent (dup'd by the kernel) — close
    * ours either way. The surface's liveness rides the held AVFrame ref, not
    * these descriptors. */
   for (unsigned o = 0; o < fm.n_fds; o++)
      close(fds[o]);
   if (!ok)
      return AVERROR(EPIPE);

   s->held[s->n_held].frame = av_frame_alloc();
   if (!s->held[s->n_held].frame)
      return AVERROR(ENOMEM);
   av_frame_move_ref(s->held[s->n_held].frame, frame);
   s->held[s->n_held].res_id = fm.res_id;
   s->n_held++;
   return 0;
}

/* LINEAR mode: download NV12 to a fresh memfd. The extra copy is the price
 * of universal consumability — the guest mmaps whatever gralloc it has. */
static int
send_frame_linear(struct session *s, AVFrame *frame)
{
   int err = 0;
   av_frame_unref(s->sw);
   s->sw->format = AV_PIX_FMT_NV12;
   if ((err = av_hwframe_transfer_data(s->sw, frame, 0)) < 0)
      return err;

   const uint32_t pitch_y = (uint32_t)s->sw->linesize[0];
   const uint32_t pitch_uv = (uint32_t)s->sw->linesize[1];
   const uint32_t size_y = pitch_y * (uint32_t)s->sw->height;
   const uint32_t size_uv = pitch_uv * (uint32_t)((s->sw->height + 1) / 2);
   const uint32_t total = size_y + size_uv;

   const int mfd = memfd_create("remora-decoded-frame", MFD_CLOEXEC);
   if (mfd < 0)
      return AVERROR(errno);
   if (ftruncate(mfd, total) < 0) {
      close(mfd);
      return AVERROR(errno);
   }
   uint8_t *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
   if (map == MAP_FAILED) {
      close(mfd);
      return AVERROR(errno);
   }
   memcpy(map, s->sw->data[0], size_y);
   memcpy(map + size_y, s->sw->data[1], size_uv);
   munmap(map, total);

   struct dec_frame fm = {
      .magic = DEC_FRAME_MAGIC,
      .res_id = s->next_res_id++,
      .width = (uint32_t)s->sw->width,
      .height = (uint32_t)s->sw->height,
      .fourcc = 0x3231564eu, /* "NV12" */
      .mode = DEC_MODE_LINEAR,
      .n_planes = 2,
      .offset = { 0, size_y, 0 },
      .pitch = { pitch_y, pitch_uv, 0 },
      .plane_fd = { 0, 0, 0 },
      .pts = (uint64_t)frame->pts,
      .n_fds = 1,
      .total_size = total,
   };
   const bool ok = send_frame_msg(s->sock, &fm, &mfd, 1);
   close(mfd); /* the client's copy is the only owner now */
   return ok ? 0 : AVERROR(EPIPE);
}

/* Drain everything the decoder has ready. Returns 0, or a fatal error. */
static int
pump_frames(struct session *s)
{
   for (;;) {
      int err = avcodec_receive_frame(s->avctx, s->frame);
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
         return 0;
      if (err < 0)
         return err;
      s->decoded++;
      if (!s->width) {
         s->width = s->frame->width;
         s->height = s->frame->height;
         status_write("ok", s->codec, s->width, s->height, 0);
         fprintf(stderr, "[decoder] first frame: %dx%d\n", s->width, s->height);
      }
      err = s->mode == DEC_MODE_EXPORT ? send_frame_export(s, s->frame)
                                       : send_frame_linear(s, s->frame);
      av_frame_unref(s->frame);
      if (err < 0)
         return err;
      s->sent++;
   }
}

static void
session_close(struct session *s)
{
   for (unsigned i = 0; i < s->n_held; i++)
      av_frame_free(&s->held[i].frame);
   s->n_held = 0;
   avcodec_free_context(&s->avctx);
   av_buffer_unref(&s->hw_dev);
   av_packet_free(&s->pkt);
   av_frame_free(&s->frame);
   av_frame_free(&s->sw);
   if (s->sock >= 0)
      close(s->sock);
   fprintf(stderr, "[decoder] session end: %" PRIu64 " decoded, %" PRIu64 " sent, %" PRIu64
                   " released, %" PRIu64 " AU errors\n",
           s->decoded, s->sent, s->released, s->errors);
}

/* One client session, hello to hangup. Returns when the client is gone. */
static void
serve(int client, const char *drm_node, int max_frames)
{
   struct session s;
   memset(&s, 0, sizeof(s));
   s.sock = client;

   struct dec_hello hello;
   if (!read_all(client, &hello, sizeof(hello)) || hello.magic != DEC_HELLO_MAGIC ||
       hello.version != 1) {
      fprintf(stderr, "[decoder] bad hello — client and helper disagree on the wire format\n");
      close(client);
      return;
   }
   s.codec = hello.codec;
   s.mode = hello.mode == DEC_MODE_EXPORT ? DEC_MODE_EXPORT : DEC_MODE_LINEAR;

   s.pkt = av_packet_alloc();
   s.frame = av_frame_alloc();
   s.sw = av_frame_alloc();
   if (!s.pkt || !s.frame || !s.sw)
      die("allocating packet/frames", AVERROR(ENOMEM));

   if (session_open_decoder(&s, drm_node) < 0) {
      status_write("failed", s.codec, 0, 0, 0);
      session_close(&s);
      return;
   }
   const struct dec_accept acc = { DEC_ACCEPT_MAGIC, 1, s.mode, 0 };
   if (!write_all(client, &acc, sizeof(acc))) {
      session_close(&s);
      return;
   }

   while (!stop_requested) {
      uint32_t magic;
      if (!read_all(client, &magic, sizeof(magic)))
         break; /* hangup — the normal end of a session */

      if (magic == DEC_RELEASE_MAGIC) {
         struct dec_release rel;
         rel.magic = magic;
         if (!read_all(client, &rel.res_id, sizeof(rel.res_id)))
            break;
         bool found = false;
         for (unsigned i = 0; i < s.n_held; i++)
            if (s.held[i].res_id == rel.res_id) {
               av_frame_free(&s.held[i].frame);
               s.held[i] = s.held[--s.n_held];
               found = true;
               break;
            }
         if (!found)
            fprintf(stderr, "[decoder] release for unknown res_id %u\n", rel.res_id);
         s.released++;
         continue;
      }

      if (magic != DEC_PACKET_MAGIC) {
         fprintf(stderr, "[decoder] bad magic 0x%08x mid-stream\n", magic);
         break;
      }
      struct dec_packet ph;
      ph.magic = magic;
      if (!read_all(client, &ph.size, sizeof(ph) - sizeof(ph.magic)))
         break;
      if (ph.size > (64u << 20)) { /* no access unit is 64 MiB; refuse to allocate on a lie */
         fprintf(stderr, "[decoder] absurd AU size %u — protocol desync\n", ph.size);
         break;
      }

      int err = 0;
      if (ph.size) {
         if ((err = av_new_packet(s.pkt, (int)ph.size)) < 0)
            die("allocating an AU", err);
         if (!read_all(client, s.pkt->data, ph.size)) {
            av_packet_unref(s.pkt);
            break;
         }
         s.pkt->pts = (int64_t)ph.pts;
         err = avcodec_send_packet(s.avctx, s.pkt);
         if (err == AVERROR(EAGAIN)) {
            /* Decoder full because frames are pending: drain, then retry once
             * WITH THE PACKET STILL REFERENCED — an unreffed packet here would
             * read as an EOS flush. A second EAGAIN means the export window is
             * what is stuck. */
            if ((err = pump_frames(&s)) < 0) {
               av_packet_unref(s.pkt);
               break;
            }
            err = avcodec_send_packet(s.avctx, s.pkt);
         }
         av_packet_unref(s.pkt);
         if (err < 0 && err != AVERROR_EOF) {
            /* A corrupt AU is the stream's problem, not the session's: tell
             * the client which pts died and keep decoding. */
            const struct dec_simple e = { DEC_ERROR_MAGIC, (uint32_t)err };
            s.errors++;
            if (!write_all(client, &e, sizeof(e)))
               break;
         }
      }
      if ((err = pump_frames(&s)) < 0) {
         fprintf(stderr, "[decoder] fatal while draining: %d\n", err);
         break;
      }
      if (ph.flags & DEC_PKT_EOS) {
         avcodec_send_packet(s.avctx, NULL);
         if ((err = pump_frames(&s)) < 0)
            break;
         const struct dec_simple d = { DEC_DRAINED_MAGIC, 0 };
         if (!write_all(client, &d, sizeof(d)))
            break;
         /* The session survives EOS: a seek is a flush + new packets. */
         avcodec_flush_buffers(s.avctx);
      }
      if (max_frames && s.sent >= (uint64_t)max_frames) {
         fprintf(stderr, "[decoder] --frames %d reached\n", max_frames);
         stop_requested = 1;
      }
   }
   status_write(s.decoded ? "ok" : "failed", s.codec, s.width, s.height, s.sent);
   session_close(&s);
}

/* ------------------------------------------------------------------ main */

static int
listen_unix(const char *path)
{
   struct sockaddr_un addr;
   if (strlen(path) >= sizeof(addr.sun_path)) {
      fprintf(stderr, "socket path too long: %s\n", path);
      exit(1);
   }
   unlink(path);
   const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (s < 0)
      die("socket", 0);
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   strcpy(addr.sun_path, path);
   if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      die("bind", 0);
   /* connect() on a unix socket needs WRITE permission on the inode, and bind()
    * applies the umask — 022 leaves 0755, reachable only by the owning uid. The
    * container maps us to `system`, so the encoder's identical socket works purely
    * because ITS consumer is surfaceflinger, which also runs as `system`. Ours is
    * the codec2 HAL, which runs as `media`: it lands on the other-bits, finds no
    * `w`, and RemoteVideoDecoder reports "no decoder helper (Permission denied)"
    * and falls back to the local VA decoder — silently, exactly as designed, which
    * is why the whole path looked plumbed and decoded nothing (bd remora-e5x.37).
    * Widen it explicitly rather than trusting a uid coincidence. */
   if (chmod(path, 0666) < 0)
      die("chmod", 0);
   if (listen(s, 4) < 0)
      die("listen", 0);
   return s;
}

/* --probe: can this host decode-and-export at all? Exit 0 yes, 1 no. This is
 * the prereq detector Remora runs before offering the feature — the answer a
 * silent SwiftShader-style fallback would otherwise bury (bd remora-4ei.79). */
static int
probe(const char *drm_node)
{
   struct session s;
   memset(&s, 0, sizeof(s));
   s.sock = -1;
   s.codec = 0x34363268u; /* h264 — the least a decode host must offer */
   s.mode = DEC_MODE_EXPORT;
   if (session_open_decoder(&s, drm_node) < 0) {
      printf("no: VAAPI h264 decoder unavailable on %s\n", drm_node);
      return 1;
   }
   AVHWDeviceContext *dev = (AVHWDeviceContext *)s.hw_dev->data;
   AVVAAPIDeviceContext *va = dev->hwctx;
   printf("yes: %s (vendor: %s)\n", drm_node, vaQueryVendorString(va->display));
   session_close(&s);
   return 0;
}

static void
usage(void)
{
   fprintf(stderr,
           "usage: remora-frame-decoder --decode-socket PATH [--drm-node NODE]\n"
           "         [--driver NAME] [--idle-timeout SEC] [--frames N]\n"
           "       remora-frame-decoder --probe [--drm-node NODE] [--driver NAME]\n");
   exit(2);
}

int
main(int argc, char **argv)
{
   const char *sock_path = NULL, *drm_node = "/dev/dri/renderD128", *driver = NULL;
   int idle_timeout = 0, max_frames = 0;
   bool do_probe = false;

   static struct option opts[] = {
      { "decode-socket", required_argument, 0, 's' },
      { "drm-node", required_argument, 0, 'd' },
      { "driver", required_argument, 0, 'D' },
      { "idle-timeout", required_argument, 0, 'I' },
      { "frames", required_argument, 0, 'n' },
      { "probe", no_argument, 0, 'p' },
      { 0, 0, 0, 0 },
   };
   for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
      switch (c) {
      case 's': sock_path = optarg; break;
      case 'd': drm_node = optarg; break;
      case 'D': driver = optarg; break;
      case 'I': idle_timeout = atoi(optarg); break;
      case 'n': max_frames = atoi(optarg); break;
      case 'p': do_probe = true; break;
      default: usage();
      }
   }
   if (driver)
      setenv("LIBVA_DRIVER_NAME", driver, 1);
   if (do_probe)
      return probe(drm_node);
   if (!sock_path)
      usage();

   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);
   signal(SIGPIPE, SIG_IGN);

   snprintf(g_status_path, sizeof(g_status_path), "%s.status", sock_path);
   unlink(g_status_path);

   const int lsock = listen_unix(sock_path);
   fprintf(stderr, "[decoder] listening on %s\n", sock_path);

   const int64_t t_start = av_gettime_relative();
   int64_t idle_since = t_start;
   while (!stop_requested) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(lsock, &rfds);
      struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
      const int n = select(lsock + 1, &rfds, NULL, NULL, &tv);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         die("select", 0);
      }
      if (n > 0) {
         const int client = accept(lsock, NULL, NULL);
         if (client >= 0) {
            fprintf(stderr, "[decoder] client connected\n");
            serve(client, drm_node, max_frames);
            idle_since = av_gettime_relative();
         }
      } else if (idle_timeout > 0 &&
                 av_gettime_relative() - idle_since >= (int64_t)idle_timeout * 1000000) {
         fprintf(stderr, "[decoder] no client for %ds — exiting\n", idle_timeout);
         break;
      }
   }

   close(lsock);
   unlink(sock_path);
   if (g_status_path[0])
      unlink(g_status_path);
   return 0;
}
