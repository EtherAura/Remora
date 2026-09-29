/*
 * Host-side H.265 encoder for Remora's mirror (bd remora-e5x.6).
 *
 * Takes the dma_buf of the frame the Venus render server just composed and
 * encodes it on the same GPU it was rendered on, so the pixels never leave the
 * device. This is the whole point of Route B: NVIDIA's VA-API driver exposes
 * ZERO encode entrypoints (it is NVDEC, decode only), so the in-Android
 * MediaCodec path cannot reach NVENC — but the frames are already VkImages on a
 * device with a VIDEO_ENCODE queue, and Vulkan Video can encode them directly.
 *
 * Why this is a C program and not another line in the python: ffmpeg's CLI has
 * no way to accept a dma_buf. A bare fd is not a URL and not a file it can
 * open — the frame has to be described to libavutil as an AVDRMFrameDescriptor
 * and mapped into a Vulkan frames context by hand. That is the only part of
 * this pipeline that cannot be expressed as an ffmpeg invocation.
 *
 * Pipeline:
 *   publisher -> dma_buf fd + layout
 *             -> AVDRMFrameDescriptor (AV_PIX_FMT_DRM_PRIME)
 *             -> av_hwframe_map        (AV_PIX_FMT_VULKAN, zero copy)
 *             -> scale_vulkan=format=nv12   (encoders want YUV, the display is RGBA)
 *             -> hevc_vulkan
 *             -> the v1 mirror wire format (scrcpy 4.0 lineage)
 *
 * The Vulkan device is DERIVED FROM THE DRM NODE rather than chosen by index.
 * The frame must be imported by the same physical GPU that rendered it, and
 * "vulkan device 0" is not a stable way to say that on a two-GPU host.
 *
 * Build: see remora-frame-encoder.build.sh
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#define FRAME_MAGIC 0x524d4632u /* "RMF2" — must match vkr_remora_track.h */
#define FDREQ_MAGIC 0x524d4651u /* "RMFQ" — must match vkr_remora_track.h */

/* Wire message from the render server. Native byte order and native padding:
 * both ends are the same machine by construction (they share an fd). */
struct frame_msg {
   uint32_t magic, res_id, width, height, has_fd;
   uint32_t fourcc, offset, pitch;
   uint64_t modifier;
};

/* The one message that travels the OTHER way: ask the publisher to send a
 * buffer's fd again. It sends each fd once per res_id and never repeats it, so
 * without this any evicted cache entry would be a permanent per-buffer
 * blackout (bd remora-e5x.19.2). */
struct fd_req {
   uint32_t magic, res_id;
};

#define MAX_CACHED_FDS 16
/* One publisher_read parses at most this many messages, and each message can evict at most one
 * cached mapping — so a dead[] sized to it cannot overflow between the main loop's sweeps. The
 * two used to be separate literals whose equality nothing stated; if they ever diverge the
 * fallback frees a mapping the keepalive may still alias (bd remora-e5x.18.9's exact shape). */
#define PUB_READ_BATCH 8

/* One connected render server. Several connect — Venus forks one per client
 * connection — and only one of them is compositing the screen, so each is
 * tracked separately until there is enough evidence to choose. */
struct publisher {
   int sock;
   char rx[sizeof(struct frame_msg) * 4];
   size_t have;
   int pending_fd;

   uint32_t res_ids[MAX_CACHED_FDS];
   int fds[MAX_CACHED_FDS];
   /* The Vulkan mapping of each dma_buf, cached alongside its fd. A buffer maps
    * to the same VkImage every time, so mapping per FRAME instead of per BUFFER
    * leaks a device allocation per frame — measured: the encoder ran for a few
    * seconds and died with VK_ERROR_OUT_OF_DEVICE_MEMORY.
    * The cache is NOT append-only: entries are touched on every use, the
    * least-recently-used one is evicted when a 17th buffer arrives, and an
    * idle sweep releases entries the guest has plainly freed — each one pins
    * its dma_buf (~29 MiB at 4K), and "the pool only rotates through a handful
    * of buffers" was the same wrong live-set reasoning that let bd remora-d3t's
    * maps go deaf (bd remora-e5x.19.2). */
   AVFrame *mapped[MAX_CACHED_FDS];
   int64_t used_us[MAX_CACHED_FDS]; /* last put or lookup — the eviction key */
   unsigned n_fds;
   /* Evicted mappings park here instead of being freed: the main loop's
    * keepalive holds a bare pointer into this cache (last_frame), so only the
    * main loop may free them, after un-aliasing. One slot per message a single
    * publisher_read can produce (PUB_READ_BATCH), so it cannot overflow
    * between sweeps. */
   AVFrame *dead[PUB_READ_BATCH];
   unsigned n_dead;
   /* Rate limit for fd re-requests: one outstanding res_id, retried at most
    * every 500 ms, so a missing fd costs one tiny message per frame burst
    * rather than one per frame. */
   uint32_t req_res_id;
   int64_t req_us;

   uint32_t width, height;
   uint64_t frames;

   int64_t last_frame_us;  /* when this publisher last delivered anything; drives the keepalive */
   uint32_t last_res_id;   /* reported so a log line names the buffer, not just the connection */
};

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{
   (void)sig;
   stop_requested = 1;
}

/* Armed once teardown begins (bd remora-zz9k): the cleanup below main's loop freed everything for
 * months and then deadlocked once — futex_wait, 28 threads, the idle-timeout's own VRAM pinned
 * indefinitely and the sockets still bound, invisible to every liveness probe. Everything past
 * the loop is disposal, so a deadline beats a wait: past it, hard-exit. _exit skips the library
 * destructors deliberately — they are where the deadlock lives — and the chain already treats
 * leftover socket inodes as stale (it removes them before rebinding). */
static void
on_teardown_deadline(int sig)
{
   (void)sig;
   static const char msg[] = "[encoder] teardown stalled past its deadline — hard exit\n";
   if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) { /* dying anyway */ }
   _exit(0);
}

/* Out of GPU memory, in any of the shapes the libraries report it. hevc_vulkan needs a Vulkan
 * device, an NV12 filter pool and an HEVC DPB at the display's resolution, so a busy desktop on a
 * shared card is enough to sink it — and none of the library messages mention the GPU. */
static bool
is_gpu_oom(int err)
{
   return err == AVERROR(ENOMEM) || err == AVERROR_EXTERNAL || err == AVERROR_UNKNOWN;
}

static void
gpu_oom_note(int width, int height)
{
   fprintf(stderr,
           "remora-frame-encoder: the GPU appears to be out of memory. Encoding %dx%d needs a "
           "Vulkan device plus an NV12 pool and an HEVC reference buffer; a desktop session on a "
           "shared card can leave too little. Free VRAM, or let this degrade to a smaller size.\n",
           width, height);
}

static void
die(const char *what, int err)
{
   if (err < 0) {
      char buf[128];
      av_strerror(err, buf, sizeof(buf));
      fprintf(stderr, "remora-frame-encoder: %s: %s\n", what, buf);
   } else {
      fprintf(stderr, "remora-frame-encoder: %s: %s\n", what, strerror(errno));
   }
   exit(1);
}

/* Where the encoder's current shape is published for Remora to read: <frame socket>.status, one
 * line, rewritten atomically on every (re)build. stderr goes nowhere once this process is spawned
 * detached, and a 4x resolution drop that only stderr knew about cost a whole debugging session
 * (bd remora-e5x.18.3). Deliberately a file and not a socket: readers are `remora check` and the
 * connect step, which want the latest state without attaching to anything. A FAILING exit leaves
 * the file behind on purpose — it is the explanation; a clean exit removes it with the sockets. */
static char g_status_path[256];

/* The last-written line, kept whole so any one field can change without the others going blank.
 * The attach counts change independently of the encoder's shape — publishers=0 on a freshly
 * bound socket IS the whole bd remora-r6au story, and it happens precisely when no encoder has
 * been built yet and the shape fields have nothing to say. */
static char g_status_state[24] = "starting";
static int g_status_native_w, g_status_native_h, g_status_enc_w, g_status_enc_h;
static int64_t g_status_bitrate;
static int g_status_degrade;
static unsigned g_status_pubs, g_status_cons;
/* Suspicion, not a verdict: keyframes this small mean the encoder is being handed flat frames.
 * See black_note() (bd remora-pobz item 1). */
static int g_status_black;

static void
status_emit(void)
{
   if (!g_status_path[0])
      return;
   char tmp[sizeof(g_status_path) + 4];
   snprintf(tmp, sizeof(tmp), "%s.tmp", g_status_path);
   FILE *f = fopen(tmp, "w");
   if (!f)
      return; /* best-effort: never take the pipeline down over its own status report */
   fprintf(f,
           "state=%s native=%dx%d encode=%dx%d bitrate=%" PRId64 " degrade=%d pid=%d"
           " publishers=%u consumers=%u black_stream=%d\n",
           g_status_state, g_status_native_w, g_status_native_h, g_status_enc_w, g_status_enc_h,
           g_status_bitrate, g_status_degrade, (int)getpid(), g_status_pubs, g_status_cons,
           g_status_black);
   fclose(f);
   rename(tmp, g_status_path); /* atomic: a reader sees the old line or the new, never half */
}

static void
status_write(const char *state, int native_w, int native_h, int enc_w, int enc_h,
             int64_t bitrate, int degrade)
{
   snprintf(g_status_state, sizeof(g_status_state), "%s", state);
   g_status_native_w = native_w;
   g_status_native_h = native_h;
   g_status_enc_w = enc_w;
   g_status_enc_h = enc_h;
   g_status_bitrate = bitrate;
   g_status_degrade = degrade;
   status_emit();
}

/* Attach counts changed: re-publish under the existing shape. Event-driven from the select loop,
 * so the file is current the moment a publisher or consumer comes or goes — the connect step's
 * publisher gate reads this instead of inferring health from socket inodes (bd remora-r6au). */
static void
status_counts(unsigned publishers, unsigned consumers)
{
   g_status_pubs = publishers;
   g_status_cons = consumers;
   status_emit();
}

/* ------------------------------------------------------------------ input */

static int
listen_unix(const char *path)
{
   struct sockaddr_un addr;
   if (strlen(path) >= sizeof(addr.sun_path)) {
      fprintf(stderr, "socket path too long: %s\n", path);
      exit(1);
   }
   /* A leftover socket file makes bind() fail with EADDRINUSE even when nobody
    * is listening. */
   unlink(path);
   const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (s < 0)
      die("socket", 0);
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   strcpy(addr.sun_path, path);
   if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      die("bind", 0);
   if (listen(s, 64) < 0)
      die("listen", 0);
   return s;
}

static int
publisher_fd_for(struct publisher *p, uint32_t res_id)
{
   for (unsigned i = 0; i < p->n_fds; i++)
      if (p->res_ids[i] == res_id) {
         /* Touch on lookup — this is what makes the live set unevictable: the
          * compositor's buffers are looked up every frame, dead ones never
          * again (same rule as d3t's map_get). */
         p->used_us[i] = av_gettime_relative();
         return p->fds[i];
      }
   return -1;
}

/* Caller must have un-aliased last_frame first (or be on a path that already
 * did): every mapping freed here may be the keepalive's material. */
static void
publisher_drop_buffers(struct publisher *p)
{
   for (unsigned i = 0; i < p->n_fds; i++) {
      close(p->fds[i]);
      av_frame_free(&p->mapped[i]);
   }
   p->n_fds = 0;
   for (unsigned i = 0; i < p->n_dead; i++)
      av_frame_free(&p->dead[i]);
   p->n_dead = 0;
}

static int
publisher_slot(struct publisher *p, uint32_t res_id)
{
   for (unsigned i = 0; i < p->n_fds; i++)
      if (p->res_ids[i] == res_id) {
         p->used_us[i] = av_gettime_relative(); /* touch — see publisher_fd_for */
         return (int)i;
      }
   return -1;
}

static void
publisher_cache_fd(struct publisher *p, uint32_t res_id, int fd)
{
   for (unsigned i = 0; i < p->n_fds; i++) {
      if (p->res_ids[i] == res_id) {
         /* virgl RECYCLES resource ids, so a new fd for a known id replaces the
          * old one. Keeping the first served a 64 KiB icon in place of a frame
          * when this was measured. The old mapping described the old buffer and
          * must go with it — parked, not freed: it may be the keepalive's. */
         close(p->fds[i]);
         if (p->mapped[i] && p->n_dead < sizeof(p->dead) / sizeof(p->dead[0]))
            p->dead[p->n_dead++] = p->mapped[i];
         else if (p->mapped[i]) {
            /* Unreachable while dead[] is sized to PUB_READ_BATCH — if this ever prints, the
             * invariant broke and this free is a use-after-free candidate (the keepalive may
             * alias it). Evidence for bd remora-e5x.18.9, not routine noise. */
            fprintf(stderr, "[encoder] BUG: dead-frame list overflow on recycle of res_id %u — "
                            "freeing a possibly-aliased mapping\n", res_id);
            av_frame_free(&p->mapped[i]);
         }
         p->mapped[i] = NULL;
         p->fds[i] = fd;
         p->used_us[i] = av_gettime_relative();
         return;
      }
   }
   if (p->n_fds < MAX_CACHED_FDS) {
      p->res_ids[p->n_fds] = res_id;
      p->fds[p->n_fds] = fd;
      p->used_us[p->n_fds] = av_gettime_relative();
      p->n_fds++;
      return;
   }
   /* Full: evict the least recently USED entry instead of dropping the NEW fd.
    * Dropping went deaf per buffer — the publisher sends an fd once per res_id,
    * so every frame composed into the new buffer was silently unservable, and
    * the drop was not even counted. The stalest entry is almost always a buffer
    * the guest already freed (live ones are touched every frame); a wrong pick
    * costs one fd re-request round trip, not a blackout (bd remora-e5x.19.2). */
   unsigned v = 0;
   for (unsigned i = 1; i < p->n_fds; i++)
      if (p->used_us[i] < p->used_us[v])
         v = i;
   close(p->fds[v]);
   if (p->mapped[v] && p->n_dead < sizeof(p->dead) / sizeof(p->dead[0]))
      p->dead[p->n_dead++] = p->mapped[v];
   else if (p->mapped[v]) {
      /* Same invariant as the recycle branch above — see the comment there. */
      fprintf(stderr, "[encoder] BUG: dead-frame list overflow on LRU eviction of res_id %u — "
                      "freeing a possibly-aliased mapping\n", p->res_ids[v]);
      av_frame_free(&p->mapped[v]);
   }
   p->mapped[v] = NULL;
   p->res_ids[v] = res_id;
   p->fds[v] = fd;
   p->used_us[v] = av_gettime_relative();
}

/* Read whatever is available. Returns the number of complete messages parsed
 * into out, or -1 on EOF/error. */
static int
publisher_read(struct publisher *p, struct frame_msg *out, int max_out)
{
   char cbuf[CMSG_SPACE(sizeof(int)) * 4];
   struct iovec iov = { .iov_base = p->rx + p->have, .iov_len = sizeof(p->rx) - p->have };
   struct msghdr mh = {
      .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)
   };

   const ssize_t n = recvmsg(p->sock, &mh, 0);
   if (n <= 0)
      return -1;
   p->have += (size_t)n;

   for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
      if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
         continue;
      const int count = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < count; i++) {
         int fd;
         memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
         if (p->pending_fd >= 0)
            close(p->pending_fd);
         p->pending_fd = fd;
      }
   }

   int produced = 0;
   while (p->have >= sizeof(struct frame_msg) && produced < max_out) {
      struct frame_msg m;
      memcpy(&m, p->rx, sizeof(m));
      memmove(p->rx, p->rx + sizeof(m), p->have - sizeof(m));
      p->have -= sizeof(m);

      if (m.magic != FRAME_MAGIC) {
         fprintf(stderr, "bad magic 0x%08x — publisher and encoder disagree on the "
                         "wire format\n", m.magic);
         return -1;
      }
      if (m.has_fd) {
         if (p->pending_fd < 0) {
            fprintf(stderr, "res_id %u announced an fd but none arrived\n", m.res_id);
            return -1;
         }
         publisher_cache_fd(p, m.res_id, p->pending_fd);
         p->pending_fd = -1;
      }
      p->frames++;
      p->last_frame_us = av_gettime_relative();
      p->last_res_id = m.res_id;
      if ((uint64_t)m.width * m.height > (uint64_t)p->width * p->height) {
         p->width = m.width;
         p->height = m.height;
      }
      out[produced++] = m;
   }
   return produced;
}

/* Elapsed ms since the first call, for correlating the encoder's events with the client's
 * SCRCPY_TRACE timeline. The two logs are the only way to see which side of the socket a stall
 * lives on, and neither carried a timestamp (bd remora-90y). */
static double
enc_ms(void)
{
   static int64_t origin;
   const int64_t now = av_gettime_relative();
   if (!origin)
      origin = now;
   return (double)(now - origin) / 1000.0;
}

/* -------------------------------------------------------------- ffmpeg in */

struct importer {
   AVBufferRef *drm_dev;
   AVBufferRef *drm_frames;
   AVBufferRef *vk_dev;
   AVBufferRef *vk_frames;
   /* Upload frames context for the CPU-copy fallback, created on first use. Separate from
    * vk_frames, which is DERIVED from the DRM frames context for zero-copy mapping and has no
    * pool of its own to allocate from. The benchmark already proves a self-made context on the
    * same device feeds the filter graph. */
   AVBufferRef *up_frames;
   int width, height;
   enum AVPixelFormat sw_format;
};

/* Instrumentation for bd remora-7jh: a rebuild is meant to hand every device and pool back, and
 * measurably does not — ~145 MiB survives each one. av_buffer_unref() is silent about whether it
 * actually released anything: it drops OUR reference and says nothing about the others. So print
 * the count we are dropping. A handle leaving with more than one reference is still held by
 * somebody and outlives the rebuild, which names the owner instead of guessing at it.
 * Set REMORA_LEAK_TRACE=1 to enable; silent otherwise. */
static int
leak_trace(void)
{
   static int on = -1;
   if (on < 0) {
      const char *v = getenv("REMORA_LEAK_TRACE");
      on = (v && *v == '1') ? 1 : 0;
   }
   return on;
}

static int live_vk_dev, live_drm_dev, live_cuda_dev, live_pools;

/* The devices are PROCESS-LIFETIME, and must be, because ffmpeg will not let them be otherwise:
 * av_hwdevice_ctx_create_derived() caches the derived device ON THE SOURCE (so a second derive
 * returns the same one), which means drm_dev holds vk_dev holds cuda_dev. Our own reference is
 * never the last, so a rebuild that unrefs all three destroys none of them and the next one
 * allocates a fresh chain — measured at exactly one vk_dev, one drm_dev and one cuda_dev leaked
 * per rebuild, ~145 MiB a time, until device creation fails and every Vulkan client on the box
 * starts dying (bd remora-7jh).
 *
 * Nothing about a device depends on the display extent — only the frames contexts, the filter
 * graph and the encoder do. So they are created once and reused, and a rebuild tears down only
 * the things that actually describe the old size. */
static AVBufferRef *g_drm_dev, *g_vk_dev, *g_cuda_dev;

/* Our own resident size, in kB, or 0 if it cannot be read.
 *
 * This process shares the host's memory with the ANDROID CONTAINER, so unbounded growth here is
 * not a private problem: Android's low-memory killer starts killing the guest's apps seconds
 * after they launch, which presents as apps flickering open and shut with no visible cause. One
 * instance reached 27 GB and did exactly that (bd remora-e5x.18.11). Reporting our own size is
 * what turns the next occurrence into a diagnosis instead of an archaeology exercise. */
static long
rss_kb(void)
{
   FILE *f = fopen("/proc/self/status", "re");
   if (!f)
      return 0;
   char line[256];
   long kb = 0;
   while (fgets(line, sizeof(line), f))
      if (sscanf(line, "VmRSS: %ld kB", &kb) == 1)
         break;
   fclose(f);
   return kb;
}

static void
unref_noting(const char *what, AVBufferRef **b, int *live)
{
   if (!*b)
      return;
   const int n = av_buffer_get_ref_count(*b);
   if (leak_trace())
      fprintf(stderr, "[leak] unref %-11s refs=%d%s\n", what, n,
              n > 1 ? "  <-- SURVIVES: someone else still holds it" : "");
   if (n == 1 && live)
      (*live)--;
   av_buffer_unref(b);
}

/* DRM fourcc -> the ffmpeg pixel format describing the same bytes. DRM names
 * components from the high bits of a little-endian word down, ffmpeg names them
 * in memory order, so the two spellings are reversed: DRM_FORMAT_ABGR8888 is
 * byte R,G,B,A in memory, which ffmpeg calls RGBA. */
static enum AVPixelFormat
fourcc_to_pixfmt(uint32_t fourcc)
{
   switch (fourcc) {
   case 0x34324241: /* AB24, DRM_FORMAT_ABGR8888 */
      return AV_PIX_FMT_RGBA;
   case 0x34325241: /* AR24, DRM_FORMAT_ARGB8888 */
      return AV_PIX_FMT_BGRA;
   case 0x34325258: /* XR24, DRM_FORMAT_XRGB8888 */
      return AV_PIX_FMT_BGR0;
   case 0x34324258: /* XB24, DRM_FORMAT_XBGR8888 */
      return AV_PIX_FMT_RGB0;
   default:
      return AV_PIX_FMT_NONE;
   }
}

/* Derive the Vulkan device from the DRM node, ONE VULKAN IMAGE PER PLANE (bd remora-q5et).
 *
 * WHY disable_multiplane=1. By default ffmpeg backs a multi-plane software format with a single
 * multi-plane VkImage (NV12 = 2 planes in 1 image). CUDA's external-memory import cannot map that:
 * cuExternalMemoryGetMappedMipmappedArray wants one memory object per plane, so the import either
 * fails outright or — far worse, and this is what shipped — appears to SUCCEED and hands back the
 * planes with the wrong pitch. That is the "multiplane pitch bug" this bead chased: at 640x360 it
 * errored with CUDA_ERROR_INVALID_VALUE, and at 3760x1992 every transfer succeeded while the bytes
 * came back wrong from luma row 0, which the probe caught only because it is a CONTENT check.
 * ffmpeg 9 diagnoses it in as many words — "Cannot map a multiplane Vulkan image (1 image(s) for
 * 2 plane(s)) to CUDA; create the Vulkan device with the disable_multiplane=1 option" — and the
 * option is not new: it is present in the 8.1.2 we ship, which is why this is a fix here and not a
 * version bump.
 *
 * The cost is confined to how NV12 is allocated (two single-plane images instead of one two-plane
 * image). The RGBA frames imported from the guest are single-plane either way, so the dma_buf
 * import path is untouched.
 *
 * MUST BE THE OPTS FORM. av_hwdevice_ctx_create_derived() takes no dictionary, so the option cannot
 * be expressed through it — that call is why the encoder could never have had a working direct
 * transfer, whatever ffmpeg it was linked against. */
static int
vulkan_dev_from_drm(AVBufferRef **out, AVBufferRef *drm_dev)
{
   AVDictionary *opts = NULL;
   int err = av_dict_set(&opts, "disable_multiplane", "1", 0);
   if (err < 0)
      return err;
   err = av_hwdevice_ctx_create_derived_opts(out, AV_HWDEVICE_TYPE_VULKAN, drm_dev, opts, 0);
   av_dict_free(&opts);
   return err;
}

static void
importer_init(struct importer *im, const char *drm_node, int width, int height,
              enum AVPixelFormat sw_format)
{
   int err;

   im->width = width;
   im->height = height;
   im->sw_format = sw_format;

   if (!g_drm_dev) {
      err = av_hwdevice_ctx_create(&g_drm_dev, AV_HWDEVICE_TYPE_DRM, drm_node, NULL, 0);
      if (!err) live_drm_dev++;
   } else {
      err = 0;
   }
   im->drm_dev = g_drm_dev; /* BORROWED — importer_free must not unref it */
   if (err < 0)
      die("opening the DRM device", err);

   /* Derived, not selected by index: the frame must be imported by the GPU that
    * rendered it, and index order is not a stable way to name that on a host
    * with two GPUs. VK_EXT_physical_device_drm is what makes the match exact. */
   if (!g_vk_dev) {
      err = vulkan_dev_from_drm(&g_vk_dev, im->drm_dev);
      if (!err) live_vk_dev++;
   } else {
      err = 0;
   }
   im->vk_dev = g_vk_dev; /* BORROWED */
   if (err < 0) {
      /* Creating the device is the first thing that touches the GPU, and on a busy card it is
       * also the first thing to fail — as VK_ERROR_INITIALIZATION_FAILED, which the library
       * reports as "Generic error in an external library". Nothing about that says the GPU is
       * full, and unlike the encoder open there is nothing to shrink: with no device there is no
       * pipeline to degrade. So the least this can do is name the cause. */
      gpu_oom_note(width, height);
      die("deriving a Vulkan device from the DRM node", err);
   }

   im->drm_frames = av_hwframe_ctx_alloc(im->drm_dev);
   if (!im->drm_frames)
      die("allocating the DRM frames context", AVERROR(ENOMEM));
   AVHWFramesContext *fc = (AVHWFramesContext *)im->drm_frames->data;
   fc->format = AV_PIX_FMT_DRM_PRIME;
   fc->sw_format = sw_format;
   fc->width = width;
   fc->height = height;
   err = av_hwframe_ctx_init(im->drm_frames);
   if (err < 0)
      die("initialising the DRM frames context", err);

   err = av_hwframe_ctx_create_derived(&im->vk_frames, AV_PIX_FMT_VULKAN, im->vk_dev,
                                       im->drm_frames, AV_HWFRAME_MAP_READ);
   if (err < 0)
      die("deriving Vulkan frames from DRM frames", err);
}

static void
drm_desc_free(void *opaque, uint8_t *data)
{
   (void)opaque;
   /* The descriptor is ours; the fd inside it belongs to the publisher cache
    * and is deliberately NOT closed here. */
   av_free(data);
}

/* Wrap a dma_buf as an AVFrame and map it into Vulkan memory. Returns a frame
 * the encoder can consume, or NULL. */
static AVFrame *
import_frame(struct importer *im, const struct frame_msg *m, int fd)
{
   AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
   if (!desc)
      return NULL;

   desc->nb_objects = 1;
   desc->objects[0].fd = fd;
   desc->objects[0].size = 0; /* unknown to us, and unused by the importer */
   desc->objects[0].format_modifier = m->modifier;
   desc->nb_layers = 1;
   desc->layers[0].format = m->fourcc;
   desc->layers[0].nb_planes = 1;
   desc->layers[0].planes[0].object_index = 0;
   desc->layers[0].planes[0].offset = m->offset;
   desc->layers[0].planes[0].pitch = m->pitch;

   AVFrame *src = av_frame_alloc();
   if (!src) {
      av_free(desc);
      return NULL;
   }
   src->format = AV_PIX_FMT_DRM_PRIME;
   src->width = (int)m->width;
   src->height = (int)m->height;
   src->data[0] = (uint8_t *)desc;
   /* buf[0] must be set: the map path refcounts the source frame. */
   src->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc), drm_desc_free, NULL, 0);
   src->hw_frames_ctx = av_buffer_ref(im->drm_frames);
   if (!src->buf[0] || !src->hw_frames_ctx) {
      av_frame_free(&src);
      return NULL;
   }

   AVFrame *dst = av_frame_alloc();
   if (!dst) {
      av_frame_free(&src);
      return NULL;
   }
   dst->format = AV_PIX_FMT_VULKAN;
   dst->hw_frames_ctx = av_buffer_ref(im->vk_frames);

   const int err = av_hwframe_map(dst, src, AV_HWFRAME_MAP_READ);
   av_frame_free(&src);
   if (err < 0) {
      char buf[128];
      av_strerror(err, buf, sizeof(buf));
      /* Frames landing in an unmappable buffer are dropped, so name the buffer: a pool with one
       * such hole shows as a periodic stutter that this line is the only evidence for. */
      fprintf(stderr, "av_hwframe_map failed for res_id %u (%ux%u modifier=0x%" PRIx64
                      " pitch=%u): %s\n",
              m->res_id, m->width, m->height, m->modifier, m->pitch, buf);
      av_frame_free(&dst);
      return NULL;
   }
   dst->width = (int)m->width;
   dst->height = (int)m->height;
   /* scale_vulkan takes the RGB->YUV matrix from the INPUT frame's colorspace
    * and refuses to guess ("Unsupported colorspace"). An imported dma_buf
    * carries no such tag, so state it: BT.709 is what HD video and every
    * mirror-side decoder assume. The source is RGB, hence full range; the
    * filter converts to limited for the encoded stream, and the encoder is
    * tagged to match so a decoder reproduces the same levels. */
   dst->colorspace = AVCOL_SPC_BT709;
   dst->color_primaries = AVCOL_PRI_BT709;
   dst->color_trc = AVCOL_TRC_BT709;
   dst->color_range = AVCOL_RANGE_JPEG;
   return dst;
}

/* The copy fallback for a buffer Vulkan cannot import. A LINEAR dma_buf that offers no
 * device-local memory type is exactly a buffer sitting in system memory — which the CPU can read
 * with nothing more than mmap and the pitch the publisher already told us. So instead of dropping
 * every frame composed into it (which froze the mirror for 73 measured seconds after a cold
 * hand-off, bd remora-90y: the guest's whole 4-buffer ImageReader pool landed linear, so EVERY
 * frame was a hole), copy it: mmap -> software RGBA frame -> upload into a Vulkan frame the
 * filter graph accepts. ~30 MB per 4K frame, a few ms — a fallback, not the fast path, but a
 * mirror at copy speed beats a frozen one at zero.
 *
 * Returns an OWNED frame (nothing aliases the dma_buf — the content is a snapshot, which is also
 * why the caller must NOT cache it in the once-per-buffer map), or NULL when this buffer is not
 * copyable either (non-linear modifier: without the tiling layout the bytes are not ours to
 * interpret). */
static AVFrame *
import_frame_copy(struct importer *im, const struct frame_msg *m, int fd)
{
   if (m->modifier != 0) /* DRM_FORMAT_MOD_LINEAR — anything else has undecodable layout */
      return NULL;
   /* A pitch narrower than a row is a malformed message; copying it anyway would read past the
    * mapping on the last row. The publisher's bug must not become the encoder's SIGSEGV. */
   if ((size_t)m->pitch < (size_t)im->width * 4)
      return NULL;

   const size_t need = (size_t)m->offset + (size_t)m->pitch * m->height;
   uint8_t *base = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
   if (base == MAP_FAILED)
      return NULL;

   /* Bracket the read so writes from the render GPU/CPU are visible; best-effort — a missing
    * implementation must not turn a readable buffer back into a dropped frame. */
   struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
   ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

   AVFrame *sw = av_frame_alloc();
   AVFrame *vk = av_frame_alloc();
   if (!sw || !vk)
      goto fail;
   sw->format = im->sw_format;
   sw->width = im->width;
   sw->height = im->height;
   if (av_frame_get_buffer(sw, 0) < 0)
      goto fail;
   const size_t row = (size_t)im->width * 4; /* every supported fourcc is 32bpp */
   for (int y = 0; y < im->height; y++)
      memcpy(sw->data[0] + (ptrdiff_t)y * sw->linesize[0],
             base + m->offset + (size_t)y * m->pitch, row);

   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
   ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   munmap(base, need);
   base = NULL;

   if (!im->up_frames) {
      AVBufferRef *ref = av_hwframe_ctx_alloc(im->vk_dev);
      if (!ref)
         goto fail;
      AVHWFramesContext *fc = (AVHWFramesContext *)ref->data;
      fc->format = AV_PIX_FMT_VULKAN;
      fc->sw_format = im->sw_format;
      fc->width = im->width;
      fc->height = im->height;
      if (av_hwframe_ctx_init(ref) < 0) {
         av_buffer_unref(&ref);
         goto fail;
      }
      im->up_frames = ref;
   }
   if (av_hwframe_get_buffer(im->up_frames, vk, 0) < 0)
      goto fail;
   if (av_hwframe_transfer_data(vk, sw, 0) < 0)
      goto fail;
   av_frame_free(&sw);
   /* Same tags import_frame stamps, for the same scale_vulkan reason. */
   vk->colorspace = AVCOL_SPC_BT709;
   vk->color_primaries = AVCOL_PRI_BT709;
   vk->color_trc = AVCOL_TRC_BT709;
   vk->color_range = AVCOL_RANGE_JPEG;
   return vk;

fail:
   if (base && base != MAP_FAILED)
      munmap(base, need);
   av_frame_free(&sw);
   av_frame_free(&vk);
   return NULL;
}

static void
importer_free(struct importer *im)
{
   unref_noting("up_frames", &im->up_frames, &live_pools);
   unref_noting("vk_frames", &im->vk_frames, &live_pools);
   unref_noting("drm_frames", &im->drm_frames, &live_pools);
   /* devices are borrowed singletons — see g_drm_dev */
   if (leak_trace())
      fprintf(stderr, "[leak] after importer_free: vk_dev=%d drm_dev=%d cuda_dev=%d pools=%d\n",
              live_vk_dev, live_drm_dev, live_cuda_dev, live_pools);
   memset(im, 0, sizeof(*im));
}

/* ------------------------------------------------------------ filter/encode */

struct encoder {
   AVFilterGraph *graph;
   AVFilterContext *src_ctx, *sink_ctx;
   AVCodecContext *avctx;
   /* The NVENC path (bd remora-e5x.18.1). NVIDIA's Vulkan driver pins HEVC encode sessions to a
    * slow internal preset — 18.2 ms/frame at 4K whatever the quality level, rc mode or tuning
    * (all swept, none moved it), while the SAME ASIC via the NVENC SDK interface does the frame
    * in under 5 ms. So for HEVC the filtered NV12 frame is handed to hevc_nvenc through a CUDA
    * frames context instead of being encoded where it lies. h264 does not need this: the Vulkan
    * driver maps H.264 sessions to a fast preset (4.1 ms/frame measured). */
   AVBufferRef *cuda_dev, *cuda_frames;
   AVFrame *bounce;    /* CPU NV12 staging when the direct Vulkan->CUDA transfer is unsupported */
   int cuda_direct;    /* +1 direct transfer works, -1 bounce via system memory */
   int64_t pts;        /* synthetic pts cursor — used only by the benchmark */
   int64_t pts_origin; /* first frame's wall time — PTS are microseconds since then */
};

/* Nonzero only under --bench: encode_frame then stamps synthetic, evenly spaced pts instead of the
 * wall clock, because the bench pushes frames much faster than real time. */
static int64_t bench_pts_step_us;

static void encoder_free(struct encoder *e);

/* --enc-opt name=value, applied to the encoder's private options at open. */
static char *enc_opts[16];
static int n_enc_opts;

/* Build just the RGBA -> NV12 conversion graph. Split from encoder_init so the benchmark can run
 * (and time) the conversion with no encoder attached — that is the split bd remora-e5x.18.6 needs. */
static void
filter_init(struct encoder *e, struct importer *im, int fps, int out_w, int out_h)
{
   int err;

   e->graph = avfilter_graph_alloc();
   if (!e->graph)
      die("allocating the filter graph", AVERROR(ENOMEM));

   /* NO AUTO-INSERTED CONVERSION FILTERS. This graph is entirely on the GPU — a Vulkan source into
    * scale_vulkan — and the ONE conversion it performs (RGBA -> NV12, BT709) is precisely
    * scale_vulkan's job. libavfilter's auto-convert exists to paper over mismatched links by
    * inserting a SOFTWARE scale, which here is never the right answer: it would silently move the
    * hottest step of the pipeline onto the CPU, costing more than the encode.
    * ffmpeg 9 makes this mandatory rather than merely tidy. Its auto-conversion also considers
    * COLOUR properties, sees the RGBA source normalised to csp:gbr against scale_vulkan's bt709
    * output, decides a conversion is owed and inserts swscale — which then refuses the job:
    *   Failed initializing scaling graph (Operation not supported): fmt:rgba -> fmt:nv12
    * Turning auto-convert off states the intent: this graph's links must match exactly, and a
    * genuine mismatch is a configuration error we want to see, not something to route around the
    * GPU. Valid on 8.1.2 as well — nothing in this graph ever wanted a software converter. */
   avfilter_graph_set_auto_convert(e->graph, AVFILTER_AUTO_CONVERT_NONE);

   /* Allocated and configured BEFORE init, not created from an args string: a
    * hardware pix_fmt is rejected unless hw_frames_ctx is already set, and the
    * args form has no way to pass a frames context. */
   e->src_ctx = avfilter_graph_alloc_filter(e->graph, avfilter_get_by_name("buffer"), "in");
   if (!e->src_ctx)
      die("allocating the buffer source", AVERROR(ENOMEM));

   AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
   if (!par)
      die("allocating buffersrc parameters", AVERROR(ENOMEM));
   par->format = AV_PIX_FMT_VULKAN;
   par->width = im->width;
   par->height = im->height;
   par->time_base = (AVRational){ 1, 1000000 };
   par->frame_rate = (AVRational){ fps, 1 };
   par->sample_aspect_ratio = (AVRational){ 1, 1 };
   /* DECLARE THE COLOUR PROPERTIES THE FRAMES ACTUALLY CARRY. Every producer feeding this graph
    * stamps BT709 with FULL range — the imported dma_buf, the keepalive frame, and the benchmark
    * source alike (see the scale_vulkan note below; it reads the matrix off the input frame and
    * refuses to guess). Leaving them unset here declares "unknown" and makes the graph disagree
    * with its own input on the very first frame.
    * ffmpeg 8.1.2 tolerated that silently. ffmpeg 9 does not: it reports "Changing video frame
    * properties on the fly is not supported by all filters", reconfigures onto SOFTWARE swscale,
    * and swscale then refuses the conversion outright —
    *   Failed initializing scaling graph (Operation not supported): fmt:rgba -> fmt:nv12
    * which surfaces only as "Generic error in an external library" from the first frame pushed.
    * The fields exist in both versions, so stating them is not a version workaround; it is saying
    * what was always true. */
   par->color_space = AVCOL_SPC_BT709;
   par->color_range = AVCOL_RANGE_JPEG;
   par->hw_frames_ctx = im->vk_frames;
   err = av_buffersrc_parameters_set(e->src_ctx, par);
   av_free(par);
   if (err < 0)
      die("setting buffersrc parameters", err);
   if ((err = avfilter_init_str(e->src_ctx, NULL)) < 0)
      die("initialising the buffer source", err);

   AVFilterContext *scale_ctx;
   /* The display is RGBA; every video encoder wants YUV. scale_vulkan does the
    * conversion on the GPU, which keeps the zero-copy property — a CPU
    * conversion here would cost more than the encode. */
   char scale_args[128];
   snprintf(scale_args, sizeof(scale_args), "w=%d:h=%d:format=nv12:out_range=limited", out_w,
            out_h);
   err = avfilter_graph_create_filter(&scale_ctx, avfilter_get_by_name("scale_vulkan"),
                                      "scale", scale_args, NULL, e->graph);
   if (err < 0)
      die("creating scale_vulkan", err);

   err = avfilter_graph_create_filter(&e->sink_ctx, avfilter_get_by_name("buffersink"),
                                      "out", NULL, NULL, e->graph);
   if (err < 0)
      die("creating the buffer sink", err);

   if ((err = avfilter_link(e->src_ctx, 0, scale_ctx, 0)) < 0 ||
       (err = avfilter_link(scale_ctx, 0, e->sink_ctx, 0)) < 0)
      die("linking the filter graph", err);
   if ((err = avfilter_graph_config(e->graph, NULL)) < 0)
      die("configuring the filter graph", err);
}

/* Map a generic codec request to the backend that is actually fast here, MEASURED at 3760x1992 on
 * an RTX 3070 Ti (driver 610.43.03, ffmpeg 8.1.2):
 *   hevc_vulkan 54.8 fps — the NVIDIA driver offers only slow internal presets to Vulkan sessions
 *   hevc_nvenc  ~200+ fps — same silicon through the NVENC SDK interface
 *   h264_vulkan 243 fps — fast preset mapping, zero-copy, nothing to fix
 * So a generic "hevc" prefers NVENC when a CUDA device can be derived from the render GPU, and a
 * generic "h264" stays on Vulkan. Explicit encoder names pass through untouched, and on hosts with
 * no NVENC (Intel/AMD, or ffmpeg built without it) everything resolves back to Vulkan — the same
 * binary behaves right on any GPU. */
static const char *
resolve_codec(const char *requested, AVBufferRef *vk_dev)
{
   if (!strcmp(requested, "h264") || !strcmp(requested, "avc"))
      return "h264_vulkan";
   if (strcmp(requested, "hevc") && strcmp(requested, "h265"))
      return requested; /* explicit name — the caller knows best */
   if (avcodec_find_encoder_by_name("hevc_nvenc")) {
      AVBufferRef *cuda = NULL;
      if (av_hwdevice_ctx_create_derived(&cuda, AV_HWDEVICE_TYPE_CUDA, vk_dev, 0) >= 0) {
         av_buffer_unref(&cuda);
         return "hevc_nvenc";
      }
   }
   return "hevc_vulkan";
}

/* Decide whether a filtered Vulkan frame can move to CUDA without touching system memory.
 * ffmpeg's Vulkan->CUDA interop exists but is UNTRUSTWORTHY on current builds (the multiplane
 * mapping fix is still working through upstream): depending on configuration it either errors —
 * and its error path can corrupt state that crashes a later teardown (the CLI equivalent
 * segfaults on exit) — or, worse, SUCCEEDS while writing the planes with the wrong pitch.
 * Measured both: 640x360 failed with CUDA_ERROR_INVALID_VALUE, and 3760x1992 "worked" but
 * decoded to sheared garbage. So the probe is a round trip with a CONTENT CHECK, at the real
 * encode size (pitch bugs are size-dependent): known pattern -> Vulkan -> CUDA -> system memory,
 * byte-compared. Every hop except the one under test is the ordinary upload/download path.
 * The sacrificial objects are freed only on a verified pass; any other outcome leaks them on
 * purpose rather than run a teardown that may crash. */
/* WHICH STEP FAILED, not merely THAT one did (bd remora-q5et). Every failure used to collapse to
 * -1, so "failed verification" covered four unrelated situations and could not be attributed to
 * ffmpeg, the driver, or our own usage without re-deriving the probe by hand. The distinction that
 * matters is CAPABILITY vs CORRECTNESS: an import that returns an error is a path this build does
 * not support, and is reportable upstream with the error code; an import that SUCCEEDS and then
 * hands back the wrong bytes is the multiplane pitch bug, which is far more dangerous precisely
 * because nothing errors. The two want different responses, so they get different verdicts.
 * Only > 0 means usable; every consumer already tests it that way. */
enum {
   CUDA_DIRECT_OK        =  1,
   CUDA_DIRECT_SETUP     = -1, /* could not even build the probe — says nothing about interop */
   CUDA_DIRECT_UPLOAD    = -2, /* sysmem -> Vulkan: the ordinary path, not the one under test */
   CUDA_DIRECT_IMPORT    = -3, /* Vulkan -> CUDA returned an error: CAPABILITY */
   CUDA_DIRECT_READBACK  = -4, /* CUDA -> sysmem: the ordinary path again */
   CUDA_DIRECT_CONTENT   = -5, /* round-tripped clean and the bytes are WRONG: CORRECTNESS */
};

static int
cuda_probe_direct(AVBufferRef *vk_dev, AVBufferRef *cuda_dev, enum AVPixelFormat sw, int w, int h,
                  int *step_err)
{
   int verdict = CUDA_DIRECT_SETUP;
   int rc;
   if (step_err) *step_err = 0;
   AVBufferRef *vkf_ref = av_hwframe_ctx_alloc(vk_dev);
   AVBufferRef *cuf_ref = av_hwframe_ctx_alloc(cuda_dev);
   AVFrame *vf = av_frame_alloc();
   AVFrame *cf = av_frame_alloc();
   AVFrame *src = av_frame_alloc();
   AVFrame *back = av_frame_alloc();
   bool clean = true; /* nothing half-imported yet: freeing is safe */
   if (!vkf_ref || !cuf_ref || !vf || !cf || !src || !back)
      goto out;
   AVHWFramesContext *fc = (AVHWFramesContext *)vkf_ref->data;
   fc->format = AV_PIX_FMT_VULKAN;
   fc->sw_format = sw;
   fc->width = w;
   fc->height = h;
   fc = (AVHWFramesContext *)cuf_ref->data;
   fc->format = AV_PIX_FMT_CUDA;
   fc->sw_format = sw;
   fc->width = w;
   fc->height = h;
   if (av_hwframe_ctx_init(vkf_ref) < 0 || av_hwframe_ctx_init(cuf_ref) < 0)
      goto out;
   if (av_hwframe_get_buffer(vkf_ref, vf, 0) < 0 || av_hwframe_get_buffer(cuf_ref, cf, 0) < 0)
      goto out;

   src->format = sw;
   src->width = w;
   src->height = h;
   if (av_frame_get_buffer(src, 0) < 0)
      goto out;
   for (int p = 0; p < 2; p++) { /* NV12: full-height luma, half-height chroma, both w wide */
      const int rows = p ? h / 2 : h;
      for (int y = 0; y < rows; y++) {
         uint8_t *row = src->data[p] + (ptrdiff_t)y * src->linesize[p];
         for (int x = 0; x < w; x++)
            row[x] = (uint8_t)(x * 5 + y * 3 + p * 17);
      }
   }
   if ((rc = av_hwframe_transfer_data(vf, src, 0)) < 0) {
      verdict = CUDA_DIRECT_UPLOAD;
      if (step_err) *step_err = rc;
      goto out;
   }

   clean = false; /* from here on a failure may leave poisoned interop state */
   /* THE STEP UNDER TEST. Its error code is the one worth reporting upstream. */
   if ((rc = av_hwframe_transfer_data(cf, vf, 0)) < 0) {
      verdict = CUDA_DIRECT_IMPORT;
      if (step_err) *step_err = rc;
      goto out;
   }
   if ((rc = av_hwframe_transfer_data(back, cf, 0)) < 0) {
      verdict = CUDA_DIRECT_READBACK;
      if (step_err) *step_err = rc;
      goto out;
   }
   for (int p = 0; p < 2; p++) {
      const int rows = p ? h / 2 : h;
      for (int y = 0; y < rows; y++)
         if (memcmp(src->data[p] + (ptrdiff_t)y * src->linesize[p],
                    back->data[p] + (ptrdiff_t)y * back->linesize[p], (size_t)w)) {
            /* Every transfer reported success and the bytes still differ — the pitch bug. Record
             * WHERE it first diverged: a mismatch that starts at row 0 of the chroma plane reads
             * very differently from one that starts partway down luma. */
            verdict = CUDA_DIRECT_CONTENT;
            if (step_err) *step_err = p * 100000 + y;
            goto out;
         }
   }
   verdict = CUDA_DIRECT_OK;
   clean = true;

out:
   av_frame_free(&src);
   av_frame_free(&back);
   if (clean) {
      av_frame_free(&vf);
      av_frame_free(&cf);
      av_buffer_unref(&vkf_ref);
      av_buffer_unref(&cuf_ref);
   }
   return verdict;
}

/* Returns 0, or a negative error the caller can degrade on. Split from the fatal path because an
 * allocation failure here is recoverable by encoding smaller, and a half-size mirror beats a black
 * one — which is what a hard exit produces once host encode is driving the display. */
static int
encoder_init(struct encoder *e, struct importer *im, const char *codec_name,
             int64_t bitrate, int fps, int out_w, int out_h, int quality)
{
   int err;

   filter_init(e, im, fps, out_w, out_h);

   const char *backend = resolve_codec(codec_name, im->vk_dev);
   const bool nvenc = strstr(backend, "_nvenc") != NULL;
   const AVCodec *codec = avcodec_find_encoder_by_name(backend);
   if (!codec) {
      fprintf(stderr, "encoder %s not available in this ffmpeg\n", backend);
      exit(1);
   }
   e->avctx = avcodec_alloc_context3(codec);
   if (!e->avctx)
      die("allocating the encoder", AVERROR(ENOMEM));

   AVBufferRef *sink_frames = av_buffersink_get_hw_frames_ctx(e->sink_ctx);
   if (!sink_frames) {
      fprintf(stderr, "filter sink produced no hardware frames context\n");
      exit(1);
   }
   if (nvenc) {
      if (!g_cuda_dev) {
         err = av_hwdevice_ctx_create_derived(&g_cuda_dev, AV_HWDEVICE_TYPE_CUDA, im->vk_dev, 0);
         if (!err) live_cuda_dev++;
      } else {
         err = 0;
      }
      e->cuda_dev = g_cuda_dev; /* BORROWED */
      if (err < 0)
         die("deriving a CUDA device for NVENC", err);
      e->cuda_frames = av_hwframe_ctx_alloc(e->cuda_dev);
      if (!e->cuda_frames)
         die("allocating the CUDA frames context", AVERROR(ENOMEM));
      AVHWFramesContext *cfc = (AVHWFramesContext *)e->cuda_frames->data;
      cfc->format = AV_PIX_FMT_CUDA;
      cfc->sw_format = ((AVHWFramesContext *)sink_frames->data)->sw_format;
      cfc->width = av_buffersink_get_w(e->sink_ctx);
      cfc->height = av_buffersink_get_h(e->sink_ctx);
      err = av_hwframe_ctx_init(e->cuda_frames);
      if (err < 0) {
         if (is_gpu_oom(err)) {
            encoder_free(e);
            return err;
         }
         die("initialising the CUDA frames context", err);
      }
      int probe_err = 0;
      e->cuda_direct = cuda_probe_direct(im->vk_dev, e->cuda_dev,
                                         ((AVHWFramesContext *)sink_frames->data)->sw_format,
                                         cfc->width, cfc->height, &probe_err);
      if (e->cuda_direct > 0) {
         fprintf(stderr, "[encoder] Vulkan->CUDA direct transfer verified — zero-copy handoff\n");
      } else {
         /* Name the failing STEP. The bounce costs the same either way, but what to DO about it
          * differs: an import error is a capability gap to report with its code, a content
          * mismatch is the silent pitch bug, and an upload/readback failure is not about interop
          * at all — it means the ordinary path is broken and the verdict says nothing (bd
          * remora-q5et). */
         /* Sized for the longest branch below WITH an ffmpeg error string spliced in. 160 silently
          * truncated the import case mid-word ("— CAPABILITY: th."), which is a poor way for a
          * diagnostic whose entire job is attribution to fail. */
         char detail[320];
         switch (e->cuda_direct) {
         case CUDA_DIRECT_IMPORT: {
            char eb[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(probe_err, eb, sizeof eb);
            /* ffmpeg collapses the driver's reason into AVERROR_EXTERNAL ("Generic error in an
             * external library"), so the return code alone cannot attribute this. The real cause
             * is in ffmpeg's own AV_LOG_ERROR line immediately above — measured here as
             * cuExternalMemoryGetMappedMipmappedArray -> CUDA_ERROR_INVALID_VALUE. Say so, or the
             * next reader repeats the hour it took to notice the two lines belong together. */
            snprintf(detail, sizeof detail,
                     "the Vulkan->CUDA import itself returned an error (%s; the driver-level "
                     "reason is in the [CUDA] line above) — CAPABILITY: this ffmpeg/driver "
                     "pair cannot do it", eb);
            break;
         }
         case CUDA_DIRECT_CONTENT:
            snprintf(detail, sizeof detail,
                     "every transfer SUCCEEDED and the bytes came back wrong, first differing at "
                     "%s row %d — CORRECTNESS: the multiplane pitch bug",
                     probe_err >= 100000 ? "chroma" : "luma", probe_err % 100000);
            break;
         case CUDA_DIRECT_UPLOAD:
         case CUDA_DIRECT_READBACK:
            snprintf(detail, sizeof detail,
                     "the probe's own %s hop failed, so this says nothing about Vulkan->CUDA — the "
                     "ordinary transfer path is broken",
                     e->cuda_direct == CUDA_DIRECT_UPLOAD ? "sysmem->Vulkan" : "CUDA->sysmem");
            break;
         default:
            snprintf(detail, sizeof detail,
                     "the probe could not be set up, so the interop was never exercised");
            break;
         }
         /* The cost figure is MEASURED, not estimated, and it has been revised UPWARDS twice — a
          * reminder that this line is worth keeping honest. --bench 3760x1992 on this host, with
          * and without the bounce:
          *     bounced   encode 17.77 ms/frame ( 56.3 fps)   full 24.06 ms/frame ( 41.6 fps)
          *     zero-copy encode  4.31 ms/frame (232.3 fps)   full  4.86 ms/frame (205.7 fps)
          * so the bounce costs ~13.5 ms/frame, not the "~6 ms" this line claimed (itself a
          * correction of an earlier "~2 ms"). The earlier 6 ms was derived as full-minus-parts and
          * missed that the transfer also sits INSIDE the encode phase, via encoder_submit.
          * REACHING THIS BRANCH IS NOW UNEXPECTED on a device that supports the interop at all:
          * bd remora-q5et found the cause — the Vulkan device must be created with
          * disable_multiplane=1, which vulkan_dev_from_drm() now does — so a failure here means
          * something OTHER than the multiplane layout, and the verdict above says which. */
         fprintf(stderr,
                 "[encoder] Vulkan->CUDA direct transfer unavailable — %s. Bouncing frames "
                 "through system memory (~13.5 ms/frame measured at 3760x1992, which is what "
                 "drops the full path from 206 to 42 fps; a fixed ffmpeg makes this zero-copy "
                 "automatically)\n", detail);
      }
      e->avctx->hw_frames_ctx = av_buffer_ref(e->cuda_frames);
      e->avctx->pix_fmt = AV_PIX_FMT_CUDA;
   } else {
      e->avctx->hw_frames_ctx = av_buffer_ref(sink_frames);
      e->avctx->pix_fmt = AV_PIX_FMT_VULKAN;
   }
   e->avctx->width = av_buffersink_get_w(e->sink_ctx);
   e->avctx->height = av_buffersink_get_h(e->sink_ctx);
   e->avctx->time_base = (AVRational){ 1, 1000000 }; /* wire timestamps are microseconds */
   e->avctx->framerate = (AVRational){ fps, 1 };
   e->avctx->bit_rate = bitrate;
   e->avctx->gop_size = fps * 2;
   e->avctx->max_b_frames = 0; /* a mirror is latency-bound; B-frames add delay */
   e->avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
   /* Signal what the filter actually produced, so the decoder does not assume
    * a different matrix and shift every colour. */
   e->avctx->colorspace = AVCOL_SPC_BT709;
   e->avctx->color_primaries = AVCOL_PRI_BT709;
   e->avctx->color_trc = AVCOL_TRC_BT709;
   e->avctx->color_range = AVCOL_RANGE_MPEG;

   if (nvenc) {
      /* Latency posture for a mirror: low-latency tuning and delay=0, so a packet comes back for
       * the very frame that went in instead of trailing the pipeline. delay>0 would raise
       * throughput (delay=2 measured +40%) but each unit holds packets a full frame interval at
       * 60 fps input — pure added latency, the one thing a mirror cannot spend. */
      av_opt_set(e->avctx->priv_data, "tune", "ll", 0);
      av_opt_set_int(e->avctx->priv_data, "delay", 0, 0);
      /* p1 by choice, MEASURED at 3760x1992 with the system-memory bounce in the path (per-frame
       * total / sustained): p1 7.9 ms/126 fps, p2 11.8/85, p3 13.0/77, p4 14.6/69. All clear
       * 60 fps, but p1 is the only one with real headroom against a busy desktop, and it is also
       * the lowest-latency choice. At 30 Mbps for UI content the preset quality delta is small,
       * and HEVC keeps its per-bit advantage over H.264 at any preset. Override with
       * --enc-opt preset=pN to trade speed back for quality. */
      av_opt_set(e->avctx->priv_data, "preset", "p1", 0);
      /* Make a REQUESTED keyframe an actual IDR. NVENC's forced-idr defaults to 0, and with it off
       * an AV_PICTURE_TYPE_I request produces at best a non-IDR I-frame: not something a decoder
       * can start on, and not flagged AV_PKT_FLAG_KEY, so a newly attached consumer keeps
       * discarding packets. Measured (bd remora-90y): a consumer attached at 24.4 s got its first
       * decodable packet at 72.9 s — 48 SECONDS of discarded packets — because it was waiting for
       * NVENC's natural 250-frame GOP boundary, and a still screen only feeds the keepalive's
       * ~3 fps. That is the frozen grey mirror, and the whole keyframe-on-attach mechanism above
       * it was inert without this one option. */
      av_opt_set_int(e->avctx->priv_data, "forced-idr", 1, 0);
      /* One reference frame. A mirror's P-frames reference the previous frame and nothing else,
       * so the default DPB only spends VRAM: fresh footprint measured 665 -> 629 MiB at
       * 3760x1992 with dpb_size=1 (surfaces=3 measured NO change against the
       * delay=0 default, so it is not set). 36 MiB is real margin on a desktop-shared 8 GiB
       * card, and --enc-opt dpb_size=N still overrides (bd remora-e5x.18.2.2). */
      av_opt_set_int(e->avctx->priv_data, "dpb_size", 1, 0);
   } else {
      /* Let frames overlap in the Vulkan encoder: at 4K its per-frame cost exceeds the frame
       * interval, so without depth the loop's ingest rate is pinned to encode latency. Measured:
       * depth changes throughput little (the engine is the bottleneck) but it keeps the loop
       * responsive while a frame drains. */
      av_opt_set_int(e->avctx->priv_data, "async_depth", 4, 0);
      /* CBR, from measurement (bd remora-e5x.12): identical throughput, but ~13.4 Mb/s against
       * auto's ~9.0 on the same animation — more bits for free. Vulkan-only: the same CBR on
       * NVENC costs ~30% throughput (126.9 -> ~85 fps at 4K), so that backend keeps its default
       * rate control. This is why the choice lives here and not in the caller's argv. */
      av_opt_set(e->avctx->priv_data, "rc_mode", "cbr", 0);
      /* MEASURED, and it contradicts the option's own documentation. ffmpeg describes quality as
       * "higher is faster", but on hevc_vulkan raising it is SLOWER: default(0) encodes 3760x1992
       * in 18.41 ms for 54.3 fps, quality=4 in 23.01 ms for 43.4 fps. On NVIDIA every level is
       * slow — that is why the NVENC path above exists (bd remora-e5x.18.1). */
      if (quality > 0)
         av_opt_set_int(e->avctx->priv_data, "quality", quality, 0);
   }
   /* Encoder-private options straight from the command line, applied by NAME so ffmpeg's own
    * enum parsing handles values like tune=ull or usage=stream. This is the tuning surface —
    * measured results live on bd remora-e5x.12, including the two knobs that made things WORSE
    * (quality>0, async_depth), so experiments go through here rather than into hardcoded
    * defaults. */
   for (int i = 0; i < n_enc_opts; i++) {
      char *eq = strchr(enc_opts[i], '=');
      if (!eq)
         continue;
      *eq = '\0';
      const char *name = enc_opts[i];
      /* The backends spell rate control differently (vulkan: rc_mode, nvenc: rc). The config
       * surface uses the Vulkan spelling and the NVENC path translates, so the same stored
       * options work whichever backend resolve_codec lands on. */
      if (nvenc && !strcmp(name, "rc_mode"))
         name = "rc";
      if (av_opt_set(e->avctx->priv_data, name, eq + 1, 0) < 0)
         fprintf(stderr, "[encoder] option '%s=%s' rejected\n", name, eq + 1);
      *eq = '=';
   }

   err = avcodec_open2(e->avctx, codec, NULL);
   if (err < 0) {
      if (is_gpu_oom(err)) {
         encoder_free(e);
         return err; /* recoverable: the caller retries smaller */
      }
      die("opening the encoder", err);
   }

   fprintf(stderr, "[encoder] %s %dx%d @%d fps, %" PRId64 " bps\n", backend,
           e->avctx->width, e->avctx->height, fps, bitrate);
   return 0;
}

/* Deliver one filtered frame to the open encoder, moving it Vulkan -> CUDA first when the NVENC
 * path is active. BORROWS `filtered` — the caller unrefs it. Returns 0 or a negative AVERROR the
 * caller may treat as a GPU allocation failure and degrade on. */
static int
encoder_submit(struct encoder *e, AVFrame *filtered)
{
   if (!e->cuda_frames)
      return avcodec_send_frame(e->avctx, filtered);

   int err;
   AVFrame *cf = av_frame_alloc();
   if (!cf)
      return AVERROR(ENOMEM);
   if ((err = av_hwframe_get_buffer(e->cuda_frames, cf, 0)) < 0) {
      av_frame_free(&cf);
      return err;
   }
   if (e->cuda_direct > 0) {
      err = av_hwframe_transfer_data(cf, filtered, 0);
   } else {
      /* Two hops through system memory. ~2 ms/frame at 4K, measured — an order of magnitude
       * cheaper than what the NVENC path saves, so it is a good trade until the direct transfer
       * works. The staging frame is allocated once and reused. */
      if (!e->bounce && !(e->bounce = av_frame_alloc())) {
         av_frame_free(&cf);
         return AVERROR(ENOMEM);
      }
      err = av_hwframe_transfer_data(e->bounce, filtered, 0);
      if (err >= 0)
         err = av_hwframe_transfer_data(cf, e->bounce, 0);
   }
   if (err < 0) {
      av_frame_free(&cf);
      return err;
   }
   av_frame_copy_props(cf, filtered);
   err = avcodec_send_frame(e->avctx, cf);
   av_frame_free(&cf);
   return err;
}

/* Open the encoder, halving the output on an allocation failure. A half-size mirror beats a black
 * one, which is what exiting produces once host encode is driving the display. Two halvings turn
 * a 4K-ish display into something a nearly-full card can still take; past that the picture is not
 * worth serving and the honest answer is to say why. */
static void
encoder_init_degrading(struct encoder *e, struct importer *im, const char *codec_name,
                       int64_t bitrate, int fps, int start_shift, int max_size, int quality)
{
   int w = im->width, h = im->height;
   /* Cap the ENCODED size up front. Recovering from an allocation failure afterwards is not
    * possible here: ffmpeg's Vulkan code SEGFAULTS on VK_ERROR_OUT_OF_DEVICE_MEMORY rather than
    * returning it (measured — signal 11 immediately after "Failed to allocate memory"), so there
    * is no error for a retry to catch. Sizing the pipeline to fit is the only defence on a card
    * that is already busy. */
   if (max_size > 0 && (w > max_size || h > max_size)) {
      const double scale = (double)max_size / (w > h ? w : h);
      w = ((int)(w * scale) + 7) & ~7;
      h = ((int)(h * scale) + 7) & ~7;
      fprintf(stderr, "[encoder] capping encode to %dx%d (--max-size %d)\n", w, h, max_size);
   }
   /* Start already reduced when a previous pipeline was killed by an allocation failure — retrying
    * at full size would just fail again and thrash. */
   for (int i = 0; i < start_shift; i++) {
      w = ((w / 2) + 7) & ~7;
      h = ((h / 2) + 7) & ~7;
      bitrate = bitrate / 2 > 500000 ? bitrate / 2 : 500000;
   }
   for (int attempt = 0;; attempt++) {
      /* The floor the comment above promises, now enforced. TOTAL shift from the (capped) source,
       * so runtime degradations and open-time retries count against the same budget: 3760x1992
       * was measured degrading to 944x504 with nothing said, and that mirror was diagnosed as a
       * broken Remora rather than a full GPU (bd remora-e5x.18.3). Below a quarter, failing
       * loudly IS the feature — the status file stays behind saying why. */
      const int shift = start_shift + attempt;
      if (shift > 2) {
         status_write("failed", im->width, im->height, w, h, bitrate, shift);
         fprintf(stderr,
                 "remora-frame-encoder: giving up: the GPU cannot sustain an encode at or above "
                 "a quarter of %dx%d, and a smaller mirror reads as broken rather than degraded. "
                 "Free VRAM (a desktop session on a shared card is the usual eater), then "
                 "reconnect.\n",
                 im->width, im->height);
         exit(1);
      }
      if (encoder_init(e, im, codec_name, bitrate, fps, w, h, quality) >= 0) {
         status_write(shift ? "degraded" : "ok", im->width, im->height, w, h, bitrate, shift);
         if (shift)
            fprintf(stderr, "[encoder] serving DEGRADED %dx%d (%d halving(s) of %dx%d)\n", w, h,
                    shift, im->width, im->height);
         return;
      }
      gpu_oom_note(w, h);
      /* Kept even and 8-aligned: chroma subsampling wants it, and so does the encoder. */
      w = ((w / 2) + 7) & ~7;
      h = ((h / 2) + 7) & ~7;
      bitrate = bitrate / 2 > 500000 ? bitrate / 2 : 500000;
      fprintf(stderr, "[encoder] retrying at %dx%d, %" PRId64 " bps\n", w, h, bitrate);
   }
}

static void
encoder_free(struct encoder *e)
{
   avcodec_free_context(&e->avctx);
   avfilter_graph_free(&e->graph);
   av_frame_free(&e->bounce);
   unref_noting("cuda_frames", &e->cuda_frames, &live_pools);
   e->cuda_dev = NULL; /* borrowed singleton — see g_drm_dev */
   if (leak_trace())
      fprintf(stderr, "[leak] after encoder_free:  vk_dev=%d drm_dev=%d cuda_dev=%d pools=%d\n",
              live_vk_dev, live_drm_dev, live_cuda_dev, live_pools);
   memset(e, 0, sizeof(*e));
}

/* ------------------------------------------------------------------ output */

/* Rolling encode statistics (bd remora-e5x.12). The point of Route B is that NVIDIA cannot encode
 * through MediaCodec, so it falls back to software in the guest — but nobody has compared the two.
 * These are the numbers that comparison needs, gathered on the live path so it is one run rather
 * than a build. */
struct stats {
   bool on;
   uint64_t frames, bytes;
   uint64_t received;  /* frames the compositor published, encoded or not */
   int64_t window_start_us;
   int64_t encode_us_total, encode_us_max;   /* frame in -> packet out */
   /* Stage split of that same interval (bd remora-e5x.18.6). CPU-side wall time is the right lens:
    * the loop is single-threaded, so however much the GPU overlaps internally, the loop ingests
    * only as fast as these calls return. `wait` is avcodec_receive_packet — the only call that
    * waits on the GPU — so it is where the 18.4 ms has to be hiding if the encode is the cost. */
   int64_t feed_us, send_us, recv_us, write_us;
   /* Pipeline evidence, the discriminator this bead asks for. depth = frames in flight when a
    * packet emerged; lag = submitted frame's pts minus that packet's pts. depth ~1 and lag ~0 mean
    * every frame waited for ITS OWN packet — fully serialized, async_depth doing nothing, and the
    * fix is pipelining. depth ~async_depth means frames genuinely overlap and the wait is real GPU
    * throughput — then only cheaper GPU work (conversion or codec) can raise the ceiling. */
   uint64_t sends, recvs;  /* running counters, never reset: only their difference matters */
   uint64_t depth_total;   /* in-flight depth summed at each packet */
   int64_t lag_us_total;   /* (submitted pts - packet pts) summed */
};

static void
stats_report(struct stats *st, int width, int height)
{
   const int64_t now = av_gettime_relative();
   const double secs = (now - st->window_start_us) / 1e6;
   if (secs <= 0 || !st->frames)
      return;
   /* in vs out is the whole diagnosis: if they match, the compositor sets the rate; if in is
    * far higher, the encoder is the bottleneck and frames are being coalesced upstream. */
   fprintf(stderr,
           "[stats] %dx%d  in %.1f fps  out %.1f fps  %.2f Mb/s  encode avg %.2f ms  max %.2f ms\n",
           width, height, st->received / secs, st->frames / secs,
           (st->bytes * 8.0) / secs / 1e6, st->encode_us_total / 1000.0 / st->frames,
           st->encode_us_max / 1000.0);
   fprintf(stderr,
           "[stats] stages/frame: feed %.2f ms  send %.2f  wait %.2f  write %.2f  |  "
           "pipeline depth %.2f  pkt lag %.2f ms\n",
           st->feed_us / 1000.0 / st->frames, st->send_us / 1000.0 / st->frames,
           st->recv_us / 1000.0 / st->frames, st->write_us / 1000.0 / st->frames,
           (double)st->depth_total / st->frames, st->lag_us_total / 1000.0 / st->frames);
   st->frames = st->bytes = st->received = 0;
   st->encode_us_total = st->encode_us_max = 0;
   st->feed_us = st->send_us = st->recv_us = st->write_us = 0;
   st->depth_total = 0;
   st->lag_us_total = 0;
   st->window_start_us = now;
}

struct output {
   int fd;          /* mirror connection, or a plain file */
   bool framed;     /* emit v1 mirror framing (scrcpy 4.0 lineage) rather than raw Annex-B */
   bool sent_config;
   bool sent_header;  /* codec id + first session packet are sent exactly once */
   bool need_keyframe; /* a consumer just attached: it cannot decode mid-GOP */
   /* Hold packets back until the first KEYFRAME actually passes. Requesting an IDR on attach is
    * not enough: the encoder pipeline is async (async_depth=4), so P-frames submitted BEFORE the
    * attach can pop out of avcodec_receive_packet AFTER it — and a consumer that receives them
    * decodes against references it never saw. Measured on a clean boot: the splash-adopted mirror
    * died at REMORA_ATTACH under a stream of 'Could not find ref with POC …' while a manual
    * reconnect (fresh consumer, IDR served idle via keepalive) worked. Gating on the key flag
    * makes an attach clean no matter what the pipeline held (bd remora-e5x.16). */
   bool waiting_key;
   /* Set when a write to a mirror consumer failed, i.e. the consumer went away. The main loop
    * turns this into a real disconnect (close, fd = -1). Before it existed, write_all swallowed
    * the error and claimed "the caller notices on the next frame" — nothing ever did, so out.fd
    * stayed set forever. Two consequences, both observed: the encoder kept encoding into a dead
    * fd for HOURS holding ~2.9 GB of VRAM, and because the accept path is gated on fd < 0 it
    * could never take a replacement consumer — so doConnect's mirror retries all attached to a
    * socket that would never answer, and reported "the mirror exited immediately after 5 attempts"
    * (bd remora-e5x.13). */
   bool gone;
   int64_t t0;
};

/* False when the peer went away mid-write. Callers MUST propagate that: this returning void is
 * what let a dead consumer go unnoticed indefinitely. */
static bool
write_all(int fd, const void *data, size_t len)
{
   const uint8_t *p = data;
   while (len) {
      const ssize_t n = write(fd, p, len);
      if (n <= 0) {
         if (errno == EINTR)
            continue;
         return false;
      }
      p += n;
      len -= (size_t)n;
   }
   return true;
}

static void
put_be32(uint8_t *p, uint32_t v)
{
   p[0] = v >> 24;
   p[1] = v >> 16;
   p[2] = v >> 8;
   p[3] = v;
}

static void
put_be64(uint8_t *p, uint64_t v)
{
   put_be32(p, (uint32_t)(v >> 32));
   put_be32(p + 4, (uint32_t)v);
}

/* The v1 stream header (scrcpy 4.0 lineage): a 4-byte codec id, then a 12-byte session packet
 * carrying the frame size. The mirror client's demuxer reads it (src/mirror), and
 * tests/test_mirrorproto.cpp pins the byte layout. */
static void
output_header(struct output *o, const char *codec_name, int width, int height)
{
   if (o->fd < 0 || !o->framed)
      return;
   const uint32_t codec_id = strstr(codec_name, "hevc") ? 0x68323635u : 0x68323634u;
   uint8_t hdr[4 + 12];
   put_be32(hdr, codec_id);
   put_be32(hdr + 4, 0x80000000u);
   put_be32(hdr + 8, (uint32_t)width);
   put_be32(hdr + 12, (uint32_t)height);
   if (!write_all(o->fd, hdr, sizeof(hdr)))
      o->gone = true;
}

/* The v1 session packet: sent after the codec id at startup, and AGAIN on its
 * own whenever the frame size changes mid-stream. That is how a device-side
 * resize is signalled, so a host-side one uses the same path. */
static void
output_session(struct output *o, int width, int height)
{
   if (o->fd < 0 || !o->framed)
      return;
   uint8_t pkt[12];
   put_be32(pkt, 0x80000000u);
   put_be32(pkt + 4, (uint32_t)width);
   put_be32(pkt + 8, (uint32_t)height);
   if (!write_all(o->fd, pkt, sizeof(pkt)))
      o->gone = true;
}

#define FLAG_CONFIG (1ull << 62)
#define FLAG_KEY    (1ull << 61)

static void
output_packet(struct output *o, const AVPacket *pkt)
{
   if (o->fd < 0)
      return; /* nobody attached yet — encode anyway, so the stream is live on connect */
   if (!o->framed) {
      /* A plain --out file: a failed write is a disk problem, not a departed consumer, so it does
       * not set `gone` — there is nothing to reconnect and nothing to reap. */
      write_all(o->fd, pkt->data, (size_t)pkt->size);
      return;
   }

   uint64_t flags = 0;
   if (pkt->flags & AV_PKT_FLAG_KEY)
      flags |= FLAG_KEY;
   const uint64_t pts = (uint64_t)(pkt->pts > 0 ? pkt->pts : 0) & ((1ull << 61) - 1);

   uint8_t hdr[12];
   put_be64(hdr, pts | flags);
   put_be32(hdr + 8, (uint32_t)pkt->size);
   if (!write_all(o->fd, hdr, sizeof(hdr)) ||
       !write_all(o->fd, pkt->data, (size_t)pkt->size))
      o->gone = true;
}

static void
output_config(struct output *o, const AVCodecContext *avctx)
{
   if (o->fd < 0 || o->sent_config || !avctx->extradata_size)
      return;
   if (!o->framed) {
      /* A raw Annex-B file needs the parameter sets IN the stream to be decodable. The Vulkan
       * encoders happen to repeat them in-band despite GLOBAL_HEADER; NVENC honours the flag and
       * strips them, which made --out files unparseable. Written once, up front, for all. */
      o->sent_config = true;
      write_all(o->fd, avctx->extradata, (size_t)avctx->extradata_size);
      return;
   }
   o->sent_config = true;
   /* Parameter sets go once, before any frame, flagged CONFIG. */
   uint8_t hdr[12];
   put_be64(hdr, FLAG_CONFIG);
   put_be32(hdr + 8, (uint32_t)avctx->extradata_size);
   if (!write_all(o->fd, hdr, sizeof(hdr)) ||
       !write_all(o->fd, avctx->extradata, (size_t)avctx->extradata_size))
      o->gone = true;
}

/* --------------------------------------------------------------- consumers */

/* Remora attaches MORE THAN ONE mirror to the same encoder — the main window, the PiP and each
 * standalone app window are separate clients — so the encoded stream has to fan out. Serving only
 * the first was invisible from the outside, which is what made it expensive: the second client's
 * connect() SUCCEEDS (the kernel completes it into the listen backlog), it simply never gets
 * accept()ed, never receives a header and never opens a window — while Remora's liveness check saw
 * a perfectly alive process and reported the connect as ok (bd remora-e5x.14). */
#define MAX_CONSUMERS 8

/* Remove the consumers marked gone, preserving order. Returns how many went. */
static unsigned
outputs_drop_gone(struct output *v, unsigned *n)
{
   unsigned w = 0, dropped = 0;
   for (unsigned i = 0; i < *n; i++) {
      if (v[i].gone) {
         close(v[i].fd);
         dropped++;
         continue;
      }
      v[w++] = v[i];
   }
   *n = w;
   return dropped;
}

/* True when ANY consumer still owes a keyframe, clearing the debt as it goes: one IDR satisfies
 * every one of them, so a newcomer costs at most one regardless of how many are attached. */
static bool
outputs_take_keyframe_request(struct output *v, unsigned n)
{
   bool want = false;
   for (unsigned i = 0; i < n; i++) {
      if (v[i].need_keyframe) {
         want = true;
         v[i].need_keyframe = false;
      }
   }
   return want;
}

/* Black-stream suspicion from keyframe size alone (bd remora-pobz item 1).
 *
 * The failure this exists for: SurfaceFlinger loses its Vulkan device, keeps PRESENTING client
 * targets it no longer renders into, and the composer faithfully publishes them. Every signal in
 * the pipeline stays green — publishers=1, consumers=1, packets at rate — and the picture is
 * black. It cost a live debugging session to find by hand, twice.
 *
 * Why keyframe size and not luma: the pixels are already encoded here, so this is free. An IDR of
 * a flat frame is a couple of KB where real content is hundreds; the gap is orders of magnitude,
 * which is why a crude threshold is enough and a per-frame luma sample would be paying for
 * precision the decision does not need.
 *
 * SUSPICION, deliberately: a legitimately dark scene (a game's black loading screen — the very
 * thing being mirrored when this was first hit) also encodes small. So it is scaled by encoded
 * pixels, requires several CONSECUTIVE flat keyframes to latch, and is surfaced as a warning with
 * a remedy rather than as an error. gop_size is fps*2, so the latch is ~6 s of flat output. Any
 * fat keyframe clears it immediately — recovery needs no separate signal. */
#define BLACK_KEYFRAMES 3

static void
black_note(const AVPacket *pkt)
{
   /* pixels/1000 puts the line at ~7.5 KB for 3760x1992: far above a flat IDR, far below content.
    * The floor covers small encodes, where pixels/1000 would be under a packet header. */
   int64_t px = (int64_t)g_status_enc_w * g_status_enc_h;
   int64_t flat_max = px / 1000;
   if (flat_max < 4096)
      flat_max = 4096;

   static unsigned run;
   if (pkt->size <= flat_max) {
      if (++run < BLACK_KEYFRAMES || g_status_black)
         return;
      g_status_black = 1;
      fprintf(stderr,
              "[encoder] %.0f ms: %u consecutive keyframes under %" PRId64 " bytes at %dx%d — the "
              "stream looks flat/black while frames flow; if the picture is black, restart the "
              "guest framework (docker exec <container> sh -c 'stop && start')\n",
              enc_ms(), run, flat_max, g_status_enc_w, g_status_enc_h);
   } else {
      run = 0;
      if (!g_status_black)
         return;
      g_status_black = 0;
      fprintf(stderr, "[encoder] %.0f ms: keyframes carry content again (%d bytes)\n", enc_ms(),
              pkt->size);
   }
   status_emit(); /* only on a transition — this must not rewrite the file per keyframe */
}

static void
outputs_packet(struct output *v, unsigned n, const AVPacket *pkt)
{
   const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
   if (key)
      black_note(pkt);
   for (unsigned i = 0; i < n; i++) {
      if (v[i].waiting_key) {
         if (!key)
            continue; /* pre-attach pipeline residue — this consumer cannot use it */
         v[i].waiting_key = false;
         fprintf(stderr, "[encoder] %.0f ms: consumer %u got its first decodable packet\n",
                 enc_ms(), i);
      }
      output_packet(&v[i], pkt);
   }
}

static void
outputs_config(struct output *v, unsigned n, const AVCodecContext *avctx)
{
   for (unsigned i = 0; i < n; i++)
      output_config(&v[i], avctx);
}

/* Only socket consumers count for the idle timer: a --out file is not something that can attach or
 * leave, so an encoder writing one must not reap itself for want of company. */
static unsigned
outputs_framed_count(const struct output *v, unsigned n)
{
   unsigned c = 0;
   for (unsigned i = 0; i < n; i++)
      if (v[i].framed)
         c++;
   return c;
}

/* -------------------------------------------------------------------- main */


/* Encode one frame into every consumer. vk is BORROWED (KEEP_REF feed; the caller owns or caches
 * it). Returns 0, or 1 when the GPU refused an allocation as an error — the caller degrades and
 * rebuilds. Crashing OOMs never get here (ffmpeg segfaults on those; see --max-size).
 *
 * PTS is the WALL CLOCK, microseconds since the first frame — not a synthetic 1/fps increment.
 * The mirror paces display and syncs AUDIO against video PTS: synthetic 120fps stamps on frames that
 * actually arrive at ~54fps ran the video clock at twice wall speed, and the player's compensation
 * showed up exactly as reported — a laggy mirror and clipped audio. Real timestamps are what the
 * device encoder sends, so the client's clock logic sees what it expects. */
static int
encode_frame(struct encoder *e, struct output *outs, unsigned n_outs, AVFrame *vk, bool force_key,
             struct stats *st, uint64_t *encoded, int max_frames, AVPacket *pkt,
             AVFrame *filtered)
{
   const int64_t now_us = av_gettime_relative();
   if (bench_pts_step_us) {
      /* Benchmark runs faster than real time; wall-clock pts would compress the timeline and
       * confuse rate control, so the bench steps pts as if frames arrived on schedule. */
      vk->pts = e->pts;
      e->pts += bench_pts_step_us;
   } else {
      if (!e->pts_origin)
         e->pts_origin = now_us;
      vk->pts = now_us - e->pts_origin;
   }
   if (force_key) {
      vk->pict_type = AV_PICTURE_TYPE_I;
      vk->flags |= AV_FRAME_FLAG_KEY;
   } else {
      vk->pict_type = AV_PICTURE_TYPE_NONE;
   }

   int err = av_buffersrc_add_frame_flags(e->src_ctx, vk, AV_BUFFERSRC_FLAG_KEEP_REF);
   if (err < 0) {
      if (is_gpu_oom(err))
         return 1;
      die("feeding the filter graph", err);
   }
   if (st->on)
      st->feed_us += av_gettime_relative() - now_us;

   for (;;) {
      int64_t t = av_gettime_relative();
      err = av_buffersink_get_frame(e->sink_ctx, filtered);
      if (st->on)
         st->feed_us += av_gettime_relative() - t;
      if (err < 0)
         break;

      /* The request rides on the frame fed to the FILTER; carry it onto what comes out, because
       * only the filtered frame is ever handed to the encoder. Cheap, and it removes any
       * dependence on which props a given filter chooses to propagate. */
      if (force_key) {
         filtered->pict_type = AV_PICTURE_TYPE_I;
         filtered->flags |= AV_FRAME_FLAG_KEY;
      }
      t = av_gettime_relative();
      err = encoder_submit(e, filtered);
      av_frame_unref(filtered);
      if (err < 0) {
         if (is_gpu_oom(err))
            return 1; /* a CUDA pool that cannot grow degrades like any other allocation */
         die("sending a frame to the encoder", err);
      }
      if (st->on) {
         st->send_us += av_gettime_relative() - t;
         st->sends++;
      }

      for (;;) {
         t = av_gettime_relative();
         err = avcodec_receive_packet(e->avctx, pkt);
         if (st->on)
            st->recv_us += av_gettime_relative() - t;
         if (err < 0)
            break;
         if (st->on) {
            /* Sampled BEFORE counting this packet: with everything serialized the frame just sent
             * is the one popping out, so depth reads 1; at async_depth=4 it reads 4. */
            st->depth_total += st->sends - st->recvs;
            st->recvs++;
            st->lag_us_total += vk->pts - pkt->pts;
         }
         t = av_gettime_relative();
         outputs_config(outs, n_outs, e->avctx);
         outputs_packet(outs, n_outs, pkt);
         if (st->on) {
            const int64_t done = av_gettime_relative();
            const int64_t dt = done - now_us;
            st->write_us += done - t;
            st->frames++;
            st->bytes += (uint64_t)pkt->size;
            st->encode_us_total += dt;
            if (dt > st->encode_us_max)
               st->encode_us_max = dt;
            if (!st->window_start_us)
               st->window_start_us = now_us;
            else if (done - st->window_start_us > 5000000)
               stats_report(st, e->avctx->width, e->avctx->height);
         }
         av_packet_unref(pkt);
         (*encoded)++;
         if (*encoded == 1 || *encoded % 60 == 0)
            fprintf(stderr, "[encoder] %.0f ms: %" PRIu64 " packets encoded\n", enc_ms(), *encoded);
      }
      if (err != AVERROR(EAGAIN) && err != AVERROR_EOF)
         die("receiving a packet", err);
   }
   if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
      if (is_gpu_oom(err))
         return 1;
      die("draining the filter graph", err);
   }

   if (max_frames && (int)*encoded >= max_frames)
      stop_requested = 1;
   return 0;
}

/* --------------------------------------------------------------- benchmark */

/* Standalone measurement mode (bd remora-e5x.18.6): no publisher, no guest, no sockets. The live
 * number to explain is 18.4 ms/frame at 3760x1992 — but the live path cannot say how much of that
 * is the RGBA->NV12 conversion pass and how much is the HEVC encode, because both hide inside one
 * blocking avcodec_receive_packet. So the bench runs the same building blocks in isolation on a
 * synthetic frame:
 *   convert   the scale_vulkan graph alone, GPU-synced at the end
 *   encode    hevc_vulkan alone, fed one pre-converted NV12 frame repeatedly
 *   full      the exact live path (encode_frame), with the per-stage stats on
 * If convert + encode ~= full, the stages do not overlap and the bigger one is what to attack.
 * If full ~= max(convert, encode), pipelining already works and only the slower stage matters.
 *
 * The synthetic frame is UI-like on purpose — gradient wash, flat cards, one band of noise —
 * because encode cost tracks content entropy and the thing being mirrored is a UI, not static.
 * Live buffers import with an NVIDIA block-linear modifier, so the default optimal tiling is
 * representative; --bench-linear exists to measure how much worse a linear source would be. */

static void
bench_fill_pattern(AVFrame *f)
{
   uint32_t lcg = 0x2545f491;
   for (int y = 0; y < f->height; y++) {
      uint8_t *row = f->data[0] + (ptrdiff_t)y * f->linesize[0];
      const uint8_t wash = (uint8_t)(32 + 160 * y / f->height);
      const bool noise_band = y >= f->height * 3 / 4 && y < f->height * 3 / 4 + f->height / 10;
      for (int x = 0; x < f->width; x++) {
         uint8_t r = wash / 2, g = wash, b = 255 - wash;
         if (((x / 240) + (y / 240)) & 1) { /* flat "cards" over the wash */
            r = 235;
            g = 238;
            b = 243;
         }
         if (noise_band) { /* text-ish entropy so the encoder works for its bits */
            lcg = lcg * 1664525u + 1013904223u;
            r = g = b = (lcg >> 16) & 0xff;
         }
         row[x * 4 + 0] = r;
         row[x * 4 + 1] = g;
         row[x * 4 + 2] = b;
         row[x * 4 + 3] = 0xff;
      }
   }
}

/* Populate `im` with a Vulkan device derived from the DRM node (same selection rule as the live
 * import path) plus an RGBA frames context, and return one filled source frame from it. */
static AVFrame *
bench_source(struct importer *im, const char *drm_node, int w, int h, bool linear)
{
   int err;

   im->width = w;
   im->height = h;
   im->sw_format = AV_PIX_FMT_RGBA;
   if (!g_drm_dev) {
      err = av_hwdevice_ctx_create(&g_drm_dev, AV_HWDEVICE_TYPE_DRM, drm_node, NULL, 0);
      if (!err) live_drm_dev++;
   } else {
      err = 0;
   }
   im->drm_dev = g_drm_dev; /* BORROWED — importer_free must not unref it */
   if (err < 0)
      die("opening the DRM device", err);
   if (!g_vk_dev) {
      err = vulkan_dev_from_drm(&g_vk_dev, im->drm_dev);
      if (!err) live_vk_dev++;
   } else {
      err = 0;
   }
   im->vk_dev = g_vk_dev; /* BORROWED */
   if (err < 0) {
      gpu_oom_note(w, h);
      die("deriving a Vulkan device from the DRM node", err);
   }

   im->vk_frames = av_hwframe_ctx_alloc(im->vk_dev);
   if (!im->vk_frames)
      die("allocating the Vulkan frames context", AVERROR(ENOMEM));
   AVHWFramesContext *fc = (AVHWFramesContext *)im->vk_frames->data;
   fc->format = AV_PIX_FMT_VULKAN;
   fc->sw_format = AV_PIX_FMT_RGBA;
   fc->width = w;
   fc->height = h;
   if (linear) {
      AVVulkanFramesContext *vkfc = fc->hwctx;
      vkfc->tiling = VK_IMAGE_TILING_LINEAR;
   }
   err = av_hwframe_ctx_init(im->vk_frames);
   if (err < 0)
      die("initialising the Vulkan frames context", err);

   AVFrame *cpu = av_frame_alloc();
   if (!cpu)
      die("allocating the pattern frame", AVERROR(ENOMEM));
   cpu->format = AV_PIX_FMT_RGBA;
   cpu->width = w;
   cpu->height = h;
   if ((err = av_frame_get_buffer(cpu, 0)) < 0)
      die("allocating pattern pixels", err);
   bench_fill_pattern(cpu);

   AVFrame *hw = av_frame_alloc();
   if (!hw)
      die("allocating the source frame", AVERROR(ENOMEM));
   if ((err = av_hwframe_get_buffer(im->vk_frames, hw, 0)) < 0)
      die("allocating the GPU source frame", err);
   if ((err = av_hwframe_transfer_data(hw, cpu, 0)) < 0)
      die("uploading the pattern", err);
   av_frame_free(&cpu);
   /* Same tags import_frame stamps on a live dma_buf, for the same reason: scale_vulkan refuses
    * to guess the RGB->YUV matrix. */
   hw->colorspace = AVCOL_SPC_BT709;
   hw->color_primaries = AVCOL_PRI_BT709;
   hw->color_trc = AVCOL_TRC_BT709;
   hw->color_range = AVCOL_RANGE_JPEG;
   return hw;
}

/* Force completion of everything submitted so far on the frame's queue, so a wall-clock stop is a
 * GPU measurement rather than a submission measurement. A readback of the newest frame is enough:
 * same-queue submissions retire in order. */
static void
bench_sync(const AVFrame *hw)
{
   AVFrame *cpu = av_frame_alloc();
   if (!cpu)
      die("allocating the sync frame", AVERROR(ENOMEM));
   const int err = av_hwframe_transfer_data(cpu, hw, 0);
   if (err < 0)
      die("syncing with the GPU", err);
   av_frame_free(&cpu);
}

static void
bench_report(const char *phase, int frames, int64_t wall_us)
{
   fprintf(stderr, "[bench] %-8s %d frames  %8.1f ms  %6.2f ms/frame  %6.1f fps\n", phase, frames,
           wall_us / 1000.0, wall_us / 1000.0 / frames, frames / (wall_us / 1e6));
}

#define BENCH_WARMUP 16 /* first submissions pay pipeline/shader setup; keep them out of the clock */

static void
bench_run(const char *drm_node, const char *codec_name, int64_t bitrate, int fps, int quality,
          int w, int h, int nframes, bool linear, const char *phase, const char *out_file)
{
   struct importer im;
   memset(&im, 0, sizeof(im));
   AVFrame *src = bench_source(&im, drm_node, w, h, linear);
   AVFrame *filtered = av_frame_alloc();
   AVPacket *pkt = av_packet_alloc();
   if (!filtered || !pkt)
      die("allocating packet/frame", AVERROR(ENOMEM));
   bench_pts_step_us = 1000000 / fps;
   const bool all = !strcmp(phase, "all");
   fprintf(stderr, "[bench] %dx%d %s tiling, %s, %d frames per phase\n", w, h,
           linear ? "LINEAR" : "optimal", codec_name, nframes);

   if (all || !strcmp(phase, "convert")) {
      struct encoder f;
      memset(&f, 0, sizeof(f));
      filter_init(&f, &im, fps, w, h);
      for (int i = 0; i < BENCH_WARMUP && !stop_requested; i++) {
         src->pts = i;
         if (av_buffersrc_add_frame_flags(f.src_ctx, src, AV_BUFFERSRC_FLAG_KEEP_REF) < 0 ||
             av_buffersink_get_frame(f.sink_ctx, filtered) < 0)
            die("bench convert warmup", AVERROR_EXTERNAL);
         bench_sync(filtered);
         av_frame_unref(filtered);
      }
      const int64_t t0 = av_gettime_relative();
      int done = 0;
      for (; done < nframes && !stop_requested; done++) {
         src->pts = BENCH_WARMUP + done;
         int err = av_buffersrc_add_frame_flags(f.src_ctx, src, AV_BUFFERSRC_FLAG_KEEP_REF);
         if (err < 0)
            die("bench convert feed", err);
         err = av_buffersink_get_frame(f.sink_ctx, filtered);
         if (err < 0)
            die("bench convert sink", err);
         if (done == nframes - 1)
            bench_sync(filtered); /* the stop matters only once — syncing each frame would
                                   * serialize the very pipelining being measured */
         av_frame_unref(filtered);
      }
      if (done)
         bench_report("convert", done, av_gettime_relative() - t0);
      encoder_free(&f);
   }

   if (all || !strcmp(phase, "encode")) {
      struct encoder e;
      memset(&e, 0, sizeof(e));
      if (encoder_init(&e, &im, codec_name, bitrate, fps, w, h, quality) < 0) {
         gpu_oom_note(w, h);
         die("bench encoder init", AVERROR(ENOMEM));
      }
      /* One conversion, then the encoder alone: the same NV12 frame goes in over and over. The
       * encoder only reads its input, so sharing one frame across in-flight slots is fine. */
      src->pts = 0;
      if (av_buffersrc_add_frame_flags(e.src_ctx, src, AV_BUFFERSRC_FLAG_KEEP_REF) < 0 ||
          av_buffersink_get_frame(e.sink_ctx, filtered) < 0)
         die("bench encode: converting the input", AVERROR_EXTERNAL);

      int64_t t0 = 0, t_last = 0;
      int sent = 0, got = 0, got0 = 0;
      for (; sent < nframes + BENCH_WARMUP && !stop_requested; sent++) {
         filtered->pts = (int64_t)sent * bench_pts_step_us;
         filtered->pict_type = sent ? AV_PICTURE_TYPE_NONE : AV_PICTURE_TYPE_I;
         int err = encoder_submit(&e, filtered);
         if (err < 0)
            die("bench encode send", err);
         while ((err = avcodec_receive_packet(e.avctx, pkt)) >= 0) {
            got++;
            t_last = av_gettime_relative();
            av_packet_unref(pkt);
         }
         if (err != AVERROR(EAGAIN))
            die("bench encode receive", err);
         if (sent == BENCH_WARMUP - 1) {
            t0 = av_gettime_relative(); /* clock starts after the warmup frames went in — and only
                                         * packets that arrive after this instant count against it */
            got0 = got;
         }
      }
      avcodec_send_frame(e.avctx, NULL);
      while (avcodec_receive_packet(e.avctx, pkt) >= 0) {
         got++;
         t_last = av_gettime_relative();
         av_packet_unref(pkt);
      }
      av_frame_unref(filtered);
      if (t0 && got > got0)
         bench_report("encode", got - got0, t_last - t0);
      encoder_free(&e);
   }

   if (all || !strcmp(phase, "full")) {
      struct encoder e;
      memset(&e, 0, sizeof(e));
      if (encoder_init(&e, &im, codec_name, bitrate, fps, w, h, quality) < 0) {
         gpu_oom_note(w, h);
         die("bench encoder init", AVERROR(ENOMEM));
      }
      /* An --out file also works under --bench, so the produced bitstream can be decode-verified —
       * a throughput number from an encoder emitting garbage would be worse than no number. */
      struct output outs[1];
      unsigned n_outs = 0;
      memset(outs, 0, sizeof(outs));
      if (out_file) {
         const int ffd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
         if (ffd < 0)
            die(out_file, 0);
         outs[0].fd = ffd; /* framed=false: raw Annex-B */
         n_outs = 1;
      }
      struct stats st;
      memset(&st, 0, sizeof(st));
      st.on = true; /* on for the warmup too: sends/recvs must span the whole stream, or frames
                     * left in flight at the reset make the depth metric read low forever */
      uint64_t encoded = 0;
      for (int i = 0; i < BENCH_WARMUP && !stop_requested; i++)
         encode_frame(&e, outs, n_outs, src, i == 0, &st, &encoded, 0, pkt, filtered);
      const uint64_t warm_sends = st.sends, warm_recvs = st.recvs;
      memset(&st, 0, sizeof(st)); /* the timed window starts clean… */
      st.on = true;
      st.sends = warm_sends; /* …except the stream-lifetime counters */
      st.recvs = warm_recvs;
      const uint64_t warm = encoded;
      const int64_t t0 = av_gettime_relative();
      int64_t t_last = t0;
      for (int i = 0; i < nframes && !stop_requested; i++) {
         if (encode_frame(&e, outs, n_outs, src, false, &st, &encoded, 0, pkt, filtered))
            die("bench full: encoder lost its memory mid-run", AVERROR(ENOMEM));
         t_last = av_gettime_relative();
      }
      avcodec_send_frame(e.avctx, NULL);
      while (avcodec_receive_packet(e.avctx, pkt) >= 0) {
         outputs_packet(outs, n_outs, pkt);
         encoded++;
         t_last = av_gettime_relative();
         av_packet_unref(pkt);
      }
      if (encoded > warm)
         bench_report("full", (int)(encoded - warm), t_last - t0);
      if (st.frames)
         stats_report(&st, e.avctx->width, e.avctx->height);
      if (n_outs)
         close(outs[0].fd);
      encoder_free(&e);
   }

   av_packet_free(&pkt);
   av_frame_free(&filtered);
   av_frame_free(&src);
   importer_free(&im);
}

static void
usage(void)
{
   fprintf(stderr,
           "usage: remora-frame-encoder [options]\n"
           "  --frame-socket PATH   listen here for the render server (default /tmp/remora-frames.sock)\n"
           "  --video-socket PATH   listen here for the mirror's --external-video-socket\n"
           "  --out FILE            write raw Annex-B here instead (for verification)\n"
           "  --drm-node PATH       GPU that rendered the frames (default /dev/dri/renderD128)\n"
           "  --codec NAME          hevc (default) or h264 — the fastest backend for this GPU is\n"
           "                        picked automatically (NVENC on NVIDIA for hevc, Vulkan Video\n"
           "                        otherwise). An explicit ffmpeg encoder name (hevc_vulkan,\n"
           "                        hevc_nvenc, h264_vulkan, ...) forces that backend\n"
           "  --bit-rate BPS        default 30000000\n"
           "  --fps N               default 60\n"
           "  --frames N            stop after N encoded frames (0 = forever)\n"
           "  --stats               report fps, bitrate and encode latency every few seconds\n"
           "  --max-size N          cap the ENCODED size to N on its longest side (0 = display)\n"
           "  --quality N           hevc_vulkan quality (default 0 = fastest here, measured)\n"
           "  --enc-opt NAME=VALUE  encoder-private option (repeatable), e.g. tune=ull\n"
           "  --probe               import only: prove the dma_buf maps, encode nothing\n"
           "  --timeout SEC         give up after SEC seconds whatever happens (0 = never;\n"
           "                        --probe defaults to 15 so a probe ALWAYS terminates)\n"
           "  --idle-timeout SEC    exit after SEC seconds with no video consumer attached\n"
           "                        (0 = never). This is what stops an abandoned encoder\n"
           "                        holding GPU memory forever\n"
           "  --bench WxH           standalone GPU benchmark at this size: no sockets, no guest.\n"
           "                        Times conversion, encode and the full path separately\n"
           "  --bench-frames N      frames per benchmark phase (default 600)\n"
           "  --bench-phase P       convert, encode, full, or all (default all)\n"
           "  --bench-linear        allocate the bench source LINEAR instead of optimal tiling\n");
   exit(2);
}

int
main(int argc, char **argv)
{
   /* The NVIDIA driver retains ~1 device fd per dma_buf import for the life of the Vulkan
    * device, and imports now continue for the whole session (the fd cache evicts instead of
    * going deaf — bd remora-e5x.19.2, which ironically was what capped this before). Measured
    * ~+4 fds per app-churn burst against the default soft limit of 1024; raise to the hard
    * limit so a long session cannot exhaust the table. */
   struct rlimit rl;
   if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
      rl.rlim_cur = rl.rlim_max;
      setrlimit(RLIMIT_NOFILE, &rl);
   }

   const char *frame_socket = "/tmp/remora-frames.sock";
   const char *video_socket = NULL;
   const char *out_file = NULL;
   const char *drm_node = "/dev/dri/renderD128";
   const char *codec_name = "hevc";
   int64_t bitrate = 30000000;
   int fps = 60, max_frames = 0, max_size = 0, quality = 0;
   int timeout_sec = 0, idle_timeout_sec = 0;
   bool probe = false;
   int bench_w = 0, bench_h = 0, bench_frames = 600;
   bool bench_linear = false;
   const char *bench_phase = "all";

   struct stats stats;
   memset(&stats, 0, sizeof(stats));

   static struct option opts[] = {
      { "frame-socket", required_argument, 0, 'f' },
      { "video-socket", required_argument, 0, 's' },
      { "out", required_argument, 0, 'o' },
      { "drm-node", required_argument, 0, 'd' },
      { "codec", required_argument, 0, 'c' },
      { "bit-rate", required_argument, 0, 'b' },
      { "fps", required_argument, 0, 'r' },
      { "frames", required_argument, 0, 'n' },
      { "probe", no_argument, 0, 'p' },
      { "stats", no_argument, 0, 'S' },
      { "max-size", required_argument, 0, 'm' },
      { "quality", required_argument, 0, 'q' },
      { "enc-opt", required_argument, 0, 'E' },
      { "timeout", required_argument, 0, 'T' },
      { "idle-timeout", required_argument, 0, 'I' },
      { "bench", required_argument, 0, 'B' },
      { "bench-frames", required_argument, 0, 'F' },
      { "bench-phase", required_argument, 0, 'P' },
      { "bench-linear", no_argument, 0, 'L' },
      { 0, 0, 0, 0 },
   };
   for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
      switch (c) {
      case 'f': frame_socket = optarg; break;
      case 's': video_socket = optarg; break;
      case 'o': out_file = optarg; break;
      case 'd': drm_node = optarg; break;
      case 'c': codec_name = optarg; break;
      case 'b': bitrate = atoll(optarg); break;
      case 'r': fps = atoi(optarg); break;
      case 'n': max_frames = atoi(optarg); break;
      case 'p': probe = true; break;
      case 'S': stats.on = true; break;
      case 'm': max_size = atoi(optarg); break;
      case 'q': quality = atoi(optarg); break;
      case 'E':
         if (n_enc_opts < 16)
            enc_opts[n_enc_opts++] = optarg;
         break;
      case 'T': timeout_sec = atoi(optarg); break;
      case 'I': idle_timeout_sec = atoi(optarg); break;
      case 'B':
         if (sscanf(optarg, "%dx%d", &bench_w, &bench_h) != 2 || bench_w < 16 || bench_h < 16) {
            fprintf(stderr, "--bench wants WxH, e.g. 3760x1992\n");
            usage();
         }
         break;
      case 'F': bench_frames = atoi(optarg); break;
      case 'P': bench_phase = optarg; break;
      case 'L': bench_linear = true; break;
      default: usage();
      }
   }
   if (bench_w) {
      signal(SIGINT, on_signal);
      signal(SIGTERM, on_signal);
      bench_run(drm_node, codec_name, bitrate, fps, quality, bench_w, bench_h, bench_frames,
                bench_linear, bench_phase, out_file);
      return 0;
   }
   if (!probe && !video_socket && !out_file) {
      fprintf(stderr, "need --video-socket, --out, or --probe\n");
      usage();
   }
   /* A PROBE MUST ALWAYS TERMINATE. Its only previous exit was `--frames N` frames actually
    * imported, so a bare --probe ran forever, and even --probe --frames 60 ran forever whenever
    * nothing was driving the display — which is the normal case when you are probing to find out
    * whether anything is. Two such probes were found alive after 1h22m and 7h04m, holding GPU
    * memory the whole time (bd remora-e5x.13). Timing out IS a result here: it means nothing
    * published, which is exactly what the probe was asked to determine. */
   if (probe && timeout_sec <= 0)
      timeout_sec = 15;

   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);
   signal(SIGPIPE, SIG_IGN);

   const int lsock = listen_unix(frame_socket);
   fprintf(stderr, "[encoder] listening for the render server on %s\n", frame_socket);
   /* Not for probes: a probe binding the frame socket must not clobber the story a real encoder
    * left behind. The unlink clears any PREDECESSOR's record — until this process builds its
    * first encoder there is genuinely nothing to report, and a stale "ok" from a dead encoder
    * reading as current is exactly the class of lie this file exists to end. */
   if (!probe) {
      snprintf(g_status_path, sizeof(g_status_path), "%s.status", frame_socket);
      unlink(g_status_path);
      /* "starting" from bind time, not silence until the first build: the connect step's
       * publisher gate needs publishers=0 to be READABLE before any frame exists — that window
       * is the whole r6au orphan state. The stale-"ok" lie this file's silence used to guard
       * against is handled by the unlink above; "starting" cannot claim health it lacks. */
      status_emit();
   }

   /* Bind the consumer socket NOW, before a single frame exists. The consumer is the mirror, and
    * its session is what makes the device compose the display in the first place — so the
    * frames this encoder needs only start flowing AFTER the mirror has started and connected. Creating
    * this socket lazily (after the first import) deadlocks that: the mirror connects at startup, finds
    * nothing, and exits. */
   int video_lsock = -1;
   if (video_socket) {
      video_lsock = listen_unix(video_socket);
      fprintf(stderr, "[encoder] listening for the video consumer on %s\n", video_socket);
   }

   struct publisher pubs[32];
   memset(pubs, 0, sizeof(pubs));
   unsigned n_pubs = 0;

   struct importer im;
   memset(&im, 0, sizeof(im));
   struct encoder enc;
   memset(&enc, 0, sizeof(enc));
   struct output outs[MAX_CONSUMERS];
   memset(outs, 0, sizeof(outs));
   unsigned n_outs = 0;

   bool ready = false;       /* importer + encoder configured */
   /* Set at resize teardown: the extent just abandoned, and how long its frames stay suspect.
    * Frames for the old buffer are still in flight when the pipeline comes down, and whichever
    * message the re-init picked up first used to win — measured as a rebuild AT THE OLD EXTENT
    * followed immediately by the real one, two full pipeline constructions per display change
    * (bd remora-8u0). Deadline-bounded, not absolute: a display that flip-flops straight back to
    * the old extent is a real display again, and skipping it forever would starve the init. */
   int stale_w = 0, stale_h = 0;
   int64_t stale_until_us = 0;
   /* How far the output has been reduced to fit in GPU memory. Allocation failures happen at
    * RUNTIME as much as at open: the filter graph allocates its NV12 pool as frames flow, so a
    * card that had room to open the encoder can still run out one packet later — measured
    * exactly that. Degrading has to be reachable from the encode loop, not just from init. */
   int degrade = 0;
   /* Keepalive material: the newest cached (mapped) frame, borrowed from the mapping cache. A
    * still screen composites nothing, so without re-sending this the mirror shows GREY from
    * connect until the first touch — measured exactly that. Cleared whenever a publisher
    * disconnects, because the cache it points into is freed with the publisher. */
   AVFrame *last_frame = NULL;
   int64_t last_encode_us = 0;
   uint64_t dropped_unimportable = 0;
   uint64_t copied_frames = 0;
   int64_t last_copy_note_us = 0;
   /* The most recent COPIED frame, which we own outright (a mapping is owned by the publisher
    * cache instead). Kept so the keepalive has material on a still screen even when every buffer
    * is linear — the exact case bd remora-90y froze in. */
   AVFrame *last_copy = NULL;
   int64_t last_drop_note_us = 0;
   uint64_t dropped_no_fd = 0;
   int64_t last_nofd_note_us = 0;
   int64_t last_sweep_us = 0;
   int64_t last_pin_note_us = 0; /* rate limit for the pinned-buffer report (bd remora-1dz) */
#define KEEPALIVE_US 300000 /* device encoders repeat after ~100ms; 300 is enough for a mirror */
#define CACHE_IDLE_US 60000000 /* a mapping untouched this long is a freed buffer's pin */
   int chosen = -1;          /* index into pubs of the display publisher */
   uint64_t encoded = 0, imported = 0;
   AVPacket *pkt = av_packet_alloc();
   AVFrame *filtered = av_frame_alloc();
   if (!pkt || !filtered)
      die("allocating packet/frame", AVERROR(ENOMEM));

   /* Lifetime bookkeeping. Monotonic (av_gettime_relative), so a wall-clock jump cannot make an
    * encoder immortal or kill a healthy one. `idle_since` tracks how long there has been NO video
    * consumer; it is reset whenever one is attached, so the timeout measures abandonment rather
    * than uptime. */
   const int64_t t_start = av_gettime_relative();
   int64_t idle_since = t_start;
   bool timed_out = false;
   int64_t last_mem_check_us = t_start;
   long last_mem_report_kb = 0;
   /* 4 GB: an order of magnitude above the ~375 MB steady state, so a healthy encoder never
    * approaches it and a runaway is stopped long before the host notices. */
   long rss_cap_kb = 4L * 1024 * 1024;
   {
      const char *v = getenv("REMORA_ENCODER_RSS_CAP_MB");
      if (v && *v)
         rss_cap_kb = atol(v) * 1024;
   }

   while (!stop_requested) {
      const int64_t now = av_gettime_relative();
      if (timeout_sec > 0 && now - t_start >= (int64_t)timeout_sec * 1000000) {
         fprintf(stderr, "[encoder] --timeout %ds reached (imported %" PRIu64 " frames)\n",
                 timeout_sec, imported);
         timed_out = true;
         break;
      }
      /* Resident-size watch. Steady state measures ~375 MB and does not drift (four
       * configurations, bd remora-e5x.18.11), so growth past the cap is a leak, not a workload —
       * and this process cannot be allowed to take the guest's apps down with it. Report the
       * counters that would name the cause, then leave: the engine starts a new encoder on
       * demand, so exiting costs a reconnect and staying costs the machine.
       * REMORA_ENCODER_RSS_CAP_MB=0 disables the cap (the report stays). */
      if (now - last_mem_check_us >= 30 * 1000000) {
         last_mem_check_us = now;
         const long rss = rss_kb();
         if (rss > 0) {
            if (rss - last_mem_report_kb > 256 * 1024 || rss_cap_kb > 0) {
               fprintf(stderr,
                       "[mem] rss %ld MB  imported %" PRIu64 "  copied %" PRIu64
                       "  publishers %u  consumers %u  pools %d\n",
                       rss / 1024, imported, copied_frames, n_pubs,
                       outputs_framed_count(outs, n_outs), live_pools);
               last_mem_report_kb = rss;
            }
            if (rss_cap_kb > 0 && rss > rss_cap_kb) {
               fprintf(stderr,
                       "[encoder] rss %ld MB exceeds the %ld MB cap — exiting before the guest's "
                       "low-memory killer starts reaping apps (bd remora-e5x.18.11). copied=%"
                       PRIu64 " imported=%" PRIu64 "\n",
                       rss / 1024, rss_cap_kb / 1024, copied_frames, imported);
               break;
            }
         }
      }
      /* Only meaningful when there is a consumer socket to be idle ON: a --out run has no
       * consumer by design and must not reap itself. */
      if (video_lsock >= 0 && idle_timeout_sec > 0) {
         if (outputs_framed_count(outs, n_outs) > 0)
            idle_since = now;
         else if (now - idle_since >= (int64_t)idle_timeout_sec * 1000000) {
            fprintf(stderr,
                    "[encoder] no video consumer for %ds — exiting so the GPU memory goes back\n",
                    idle_timeout_sec);
            break;
         }
      }

      /* Turn failed writes into real disconnects BEFORE the FD_SETs below, so a freed slot can take
       * a replacement consumer in this very iteration. Beyond tidiness: while the accept path was
       * gated on a single slot, the first mirror to die left the encoder unable to accept another
       * at all, and doConnect's retries each attached to a socket that would never answer. Keep
       * encoding meanwhile — the stream stays live for whoever attaches next, which is the same
       * reason output_packet tolerates having no consumers. */
      const unsigned went = outputs_drop_gone(outs, &n_outs);
      if (went) {
         if (outputs_framed_count(outs, n_outs) == 0)
            idle_since = now;
         fprintf(stderr, "[encoder] %u video consumer(s) disconnected, %u still attached\n", went,
                 outputs_framed_count(outs, n_outs));
      }

      /* Publish attach-count changes the moment they happen (bd remora-r6au). Loop-top is
       * enough: every attach and detach wakes select, so the iteration after the change lands
       * here microseconds later — no event goes unpublished for longer than one pass. */
      {
         const unsigned cons_now = outputs_framed_count(outs, n_outs);
         if (n_pubs != g_status_pubs || cons_now != g_status_cons)
            status_counts(n_pubs, cons_now);
      }

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(lsock, &rfds);
      int maxfd = lsock;
      if (video_lsock >= 0 && n_outs < MAX_CONSUMERS) {
         FD_SET(video_lsock, &rfds);
         if (video_lsock > maxfd)
            maxfd = video_lsock;
      }
      /* Watch every CONNECTED consumer as well, so a departure is noticed even while no frames are
       * flowing. A failed write is otherwise the only disconnect signal, and it never arrives if
       * nothing is being published — which would leave the slot occupied, the idle timer frozen,
       * and the encoder immortal in precisely the situation it is supposed to reap itself in. The
       * video socket is one-way, so readable here means EOF in practice; anything a consumer does
       * send is peeked and ignored rather than mistaken for a hangup. */
      for (unsigned i = 0; i < n_outs; i++) {
         if (!outs[i].framed)
            continue;
         FD_SET(outs[i].fd, &rfds);
         if (outs[i].fd > maxfd)
            maxfd = outs[i].fd;
      }
      for (unsigned i = 0; i < n_pubs; i++) {
         FD_SET(pubs[i].sock, &rfds);
         if (pubs[i].sock > maxfd)
            maxfd = pubs[i].sock;
      }
      /* Short enough that the keepalive below fires promptly on a still screen. */
      struct timeval tv = { .tv_sec = 0, .tv_usec = 150000 };
      const int nsel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
      if (nsel < 0) {
         if (errno == EINTR)
            continue;
         die("select", 0);
      }

      for (unsigned i = 0; i < n_outs; i++) {
         if (!outs[i].framed || !FD_ISSET(outs[i].fd, &rfds))
            continue;
         char b;
         const ssize_t n = recv(outs[i].fd, &b, 1, MSG_PEEK);
         if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            outs[i].gone = true;  /* reaped at the top of the next iteration */
      }

      if (video_lsock >= 0 && n_outs < MAX_CONSUMERS && FD_ISSET(video_lsock, &rfds)) {
         const int c = accept(video_lsock, NULL, NULL);
         if (c >= 0) {
            /* ONE SLOW CONSUMER MUST NOT STALL THE OTHERS. With fan-out a blocking write would hold
             * every other window's frames hostage behind the worst client. A send timeout bounds
             * that, and a consumer that cannot take a packet in half a second is broken rather than
             * slow — a partial write has already desynchronised its stream, so it is dropped rather
             * than nursed. */
            const struct timeval sndto = { .tv_sec = 0, .tv_usec = 500000 };
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

            /* ...but the half second above was never the budget a consumer actually got, and that
             * is what made healthy mirrors die (bd remora-10bt). On AF_UNIX the SENDER charges its
             * own SO_SNDBUF, so a write blocks once that much is queued unread — at the default
             * ~208 KB that is ~55 ms of a 30 Mbit stream. A mirror decoding and presenting on its
             * GUI thread stops reading for longer than that routinely (one slow CUDA decode, one
             * compositor stall), so the clock started almost immediately and 500 ms later a
             * perfectly healthy client was evicted as "broken". Sizing this to seconds of stream
             * rather than milliseconds is what separates a stalled consumer from a dead one, which
             * is the distinction the timeout was always meant to draw.
             *
             * Not the timeout instead: nursing a genuinely dead consumer is what it exists to
             * refuse, and lengthening it would hold every other window's frames behind the worst
             * client for that much longer. Buffer, not patience.
             *
             * Best-effort and unchecked on purpose: the kernel clamps to wmem_max, a smaller cap
             * is still an improvement over the default, and a consumer is not worth refusing over
             * a socket option. */
            const int sndbuf = 8 << 20;
            setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

            struct output *o = &outs[n_outs++];
            memset(o, 0, sizeof(*o));
            o->fd = c;
            o->framed = true;
            /* Encoding starts with the first frame, long before anything connects, and those
             * packets are dropped — so a consumer attaching mid-GOP receives P-frames whose
             * references it never saw. Measured, from the mirror's own decoder: "Could not find ref
             * with POC 5", repeating. Force an IDR so the stream it sees starts at a keyframe.
             * With fan-out this also covers the LATE joiner: the PiP or app window that attaches
             * to an encoder already streaming to the mirror gets its own IDR, and because the
             * request is pooled, one IDR serves however many arrive together. */
            o->need_keyframe = true;
            o->waiting_key = true;
            fprintf(stderr, "[encoder] %.0f ms: video consumer connected (%u attached)\n",
                    enc_ms(), outputs_framed_count(outs, n_outs));
            /* If frames were already flowing, the stream header is owed immediately; otherwise it
             * goes out when the encoder opens, which is the usual order. */
            if (ready && !probe) {
               output_header(o, codec_name, enc.avctx->width, enc.avctx->height);
               o->sent_header = true;
               output_config(o, enc.avctx);
            }
         }
      }

      if (FD_ISSET(lsock, &rfds) && n_pubs < sizeof(pubs) / sizeof(pubs[0])) {
         const int c = accept(lsock, NULL, NULL);
         if (c >= 0) {
            memset(&pubs[n_pubs], 0, sizeof(pubs[0]));
            pubs[n_pubs].sock = c;
            pubs[n_pubs].pending_fd = -1;
            n_pubs++;
            fprintf(stderr, "[encoder] publisher %u connected\n", n_pubs);
         }
      }

      for (unsigned i = 0; i < n_pubs; i++) {
         if (!FD_ISSET(pubs[i].sock, &rfds))
            continue;

         struct frame_msg msgs[PUB_READ_BATCH];
         const int got = publisher_read(&pubs[i], msgs, PUB_READ_BATCH);
         if (got < 0) {
            fprintf(stderr, "[encoder] publisher %u gone (%" PRIu64 " frames, %ux%u)\n",
                    i + 1, pubs[i].frames, pubs[i].width, pubs[i].height);
            close(pubs[i].sock);
            last_frame = NULL; /* it may point into the cache about to be freed */
            publisher_drop_buffers(&pubs[i]);
            av_frame_free(&last_copy);
            if (pubs[i].pending_fd >= 0)
               close(pubs[i].pending_fd);
            pubs[i] = pubs[--n_pubs];
            if (chosen == (int)i)
               chosen = -1;
            else if (chosen == (int)n_pubs)
               chosen = (int)i;
            i--;
            continue;
         }

         /* Free what the cache evicted during that read, un-aliasing the
          * keepalive first — only this loop knows what last_frame points at. */
         for (unsigned k = 0; k < pubs[i].n_dead; k++) {
            if (last_frame == pubs[i].dead[k])
               last_frame = NULL;
            av_frame_free(&pubs[i].dead[k]);
         }
         pubs[i].n_dead = 0;

         /* Encode only the NEWEST frame that arrived. Encoding every queued message tied the
          * loop's ingest rate to the encoder's throughput: a compositor composing faster than the
          * encoder encodes backs this socket up and the publisher then stalls — starving
          * SurfaceFlinger of buffers, which shows on the mirror as black/glitched frames and laggy
          * animation (observed live at fps=120 compose against ~90 fps encode; the 54 fps
          * hevc_vulkan encoder sat below even 60 Hz compose, same mechanism). A mirror wants the
          * newest frame; stale ones are dropped, not paid for.
          *
          * THE PUBLISHER IS THE DISPLAY — there is no longer anything to choose between (bd
          * remora-emoe). This called pick_display(), which ranked connections by extent and
          * liveness because the Venus render-server interception published every composited
          * surface and the screen had to be guessed out of them. The Remora hwcomposer publishes
          * the client target and nothing else, so this socket has exactly one writer by
          * construction, and a guess can only be wrong. Every heuristic that lived here was a bug
          * being managed: ranking on extent alone followed a dead publisher still advertising a
          * bigger display, and ranking on recent activity handed the mirror to an app's surface
          * whenever the real screen sat idle — a static home screen turned the mirror black. */
         if ((int)i != chosen) {
            fprintf(stderr, "[encoder] display = publisher %d (%ux%u, res_id=%u)\n", (int)i + 1,
                    pubs[i].width, pubs[i].height, pubs[i].last_res_id);
            chosen = (int)i;
         }
         /* Two writers means a publisher outlived its connection; the composer opens exactly one.
          * Said once, because from here the frames would interleave. */
         if (n_pubs > 1) {
            static int warned_multi;
            if (!warned_multi) {
               warned_multi = 1;
               fprintf(stderr, "[encoder] %u publishers on a socket that should have one — frames "
                               "may interleave\n", n_pubs);
            }
         }
         int newest = -1;
         for (int k = 0; k < got; k++) {
            if (stats.on)
               stats.received++; /* arrivals, not encodes: the in-vs-out gap is the drop rate */
            newest = k;
         }
         if (newest >= 0) {
            const struct frame_msg *m = &msgs[newest];

            const int fd = publisher_fd_for(&pubs[i], m->res_id);
            if (fd < 0) {
               /* No fd for this resource: it predates this connection, or the
                * cache evicted it. Ask for it again — the publisher clears its
                * sent-flag and the fd rides this buffer's next frame. Counted:
                * this drop used to be silent, which is precisely how going deaf
                * per buffer stayed invisible (bd remora-e5x.19.2). */
               const int64_t rnow = av_gettime_relative();
               if (pubs[i].req_res_id != m->res_id || rnow - pubs[i].req_us > 500000) {
                  const struct fd_req rq = { FDREQ_MAGIC, m->res_id };
                  (void)send(pubs[i].sock, &rq, sizeof(rq), MSG_DONTWAIT | MSG_NOSIGNAL);
                  pubs[i].req_res_id = m->res_id;
                  pubs[i].req_us = rnow;
               }
               dropped_no_fd++;
               if (rnow - last_nofd_note_us > 5000000) {
                  fprintf(stderr,
                          "[encoder] %" PRIu64 " frames dropped awaiting a buffer fd "
                          "(re-requested from the publisher)\n", dropped_no_fd);
                  last_nofd_note_us = rnow;
               }
               continue;
            }

            /* A frame with no extent is malformed, not a resize, and the difference is
             * expensive: the branch below trusts the extent enough to tear the whole pipeline
             * down and rebuild it, so one bad message costs the mirror. A publisher bug did
             * exactly that (an fd_req answered with a zeroed message), and the encoder dutifully
             * "resized" to 0x0 and then could not rebuild. No display is 0 pixels wide, so
             * refusing here costs one dropped frame and cannot cost anything else. */
            if (m->width == 0 || m->height == 0) {
               static int warned_zero_extent = 0;
               if (!warned_zero_extent) {
                  warned_zero_extent = 1;
                  fprintf(stderr,
                          "[encoder] publisher sent a 0x0 frame for res_id=%u — dropped; a frame "
                          "with no extent is malformed, not a resize\n", m->res_id);
               }
               continue;
            }

            /* The display resized. Everything downstream was built for the old
             * extent — the frames context, the filter graph, the encoder — and
             * every cached mapping describes a buffer nobody writes any more, so
             * the whole pipeline is rebuilt rather than fed mismatched frames.
             * The render server relabels when a display shrinks, so this is a
             * normal event, not an error. */
            if (ready && ((int)m->width != im.width || (int)m->height != im.height)) {
               fprintf(stderr, "[encoder] display resized %dx%d -> %ux%u, rebuilding\n",
                       im.width, im.height, m->width, m->height);
               /* Both keepalive handles belong to the pipeline being torn down, and the publisher
                * -gone path above already clears them for exactly these reasons — this path did
                * not, and that asymmetry was the leak.
                *
                * last_frame points into the mapping cache dropped on the next line, so leaving it
                * set is a use-after-free the keepalive can re-send.
                *
                * last_copy is worse, because it is silent: it holds a frame drawn from the
                * importer's OWN Vulkan pool, and av_buffer_unref cannot destroy a pool that a
                * live frame still references — so importer_free() below returns nothing and the
                * whole old pool, every 4K image in it, survives the rebuild. It leaks per
                * REBUILD, not per frame, which is why the encoder sat flat for minutes and then
                * stepped up: 665 -> 1311 MiB over ~20 minutes of a display label flip-flopping
                * between the mirror and an app window (bd remora-ljm), and 0 growth once that
                * stopped. And last_copy is non-NULL whenever the compositor is handing out
                * linear buffers, which is the case this fallback exists for. */
               last_frame = NULL;
               av_frame_free(&last_copy);
               for (unsigned k = 0; k < n_pubs; k++)
                  publisher_drop_buffers(&pubs[k]);
               if (!probe)
                  encoder_free(&enc);
               stale_w = im.width;
               stale_h = im.height;
               stale_until_us = av_gettime_relative() + 500000;
               importer_free(&im);
               ready = false;
               continue; /* the fd for this resource is re-sent by the publisher */
            }

            if (!ready) {
               /* A frame at the extent a resize just abandoned: known stale, not a target to
                * rebuild for. Within the deadline it is dropped rather than initialised on;
                * past the deadline whatever arrives is the newest truth (bd remora-8u0). */
               if ((int)m->width == stale_w && (int)m->height == stale_h &&
                   av_gettime_relative() < stale_until_us)
                  continue;
               const enum AVPixelFormat sw = fourcc_to_pixfmt(m->fourcc);
               if (sw == AV_PIX_FMT_NONE) {
                  fprintf(stderr, "unsupported fourcc 0x%08x — refusing to guess\n",
                          m->fourcc);
                  exit(1);
               }
               /* Only when the importer is actually down (first frame, or a resize freed it).
                * A runtime DEGRADE lands here too — encoder_free + ready=false with the extent
                * unchanged — and re-initialising then did two bad things: the live
                * drm_frames/vk_frames refs were overwritten unfreed (one pool pair leaked per
                * degrade), and every cached mapping kept pointing at the OLD vk_frames while the
                * rebuilt graph's buffersrc was parameterised with the NEW one, so each encoded
                * frame crossed frames-context pools from then on. The importer describes the
                * display, not the encoder; it only rebuilds when the display does. */
               if (!im.drm_frames) {
                  fprintf(stderr,
                          "[encoder] importing %ux%u %s modifier=0x%" PRIx64
                          " offset=%u pitch=%u res_id=%u\n",
                          m->width, m->height, av_get_pix_fmt_name(sw), m->modifier,
                          m->offset, m->pitch, m->res_id);
                  importer_init(&im, drm_node, (int)m->width, (int)m->height, sw);
               }
               if (!probe) {
                  encoder_init_degrading(&enc, &im, codec_name, bitrate, fps, degrade, max_size, quality);
                  if (out_file && n_outs == 0) {
                     const int ffd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                     if (ffd < 0)
                        die(out_file, 0);
                     struct output *o = &outs[n_outs++];
                     memset(o, 0, sizeof(*o));
                     o->fd = ffd;  /* framed=false: raw Annex-B, and never reaped for idleness */
                  }
                  /* Consumers that connected before any frame existed are owed the header now.
                   * After a resize they have already had one, and re-sending the codec id would
                   * desynchronise the stream — they get a session packet instead. Per consumer,
                   * because with fan-out they do not all arrive at the same point in the stream:
                   * one may predate the first frame while another joined after a resize. */
                  for (unsigned oi = 0; oi < n_outs; oi++) {
                     struct output *o = &outs[oi];
                     if (o->sent_header) {
                        output_session(o, enc.avctx->width, enc.avctx->height);
                        o->sent_config = false;
                     } else {
                        output_header(o, codec_name, enc.avctx->width, enc.avctx->height);
                        o->sent_header = true;
                     }
                     output_config(o, enc.avctx);
                  }
               }
               ready = true;
               stale_w = stale_h = 0; /* the rebuild landed; nothing is suspect any more */
            }

            /* Map ONCE PER BUFFER, not per frame. The compositor writes into the
             * same dma_buf again and again; re-mapping it each time allocates a
             * fresh VkImage every frame and exhausts device memory in seconds. */
            const int slot = publisher_slot(&pubs[i], m->res_id);
            AVFrame *vk = (slot >= 0) ? pubs[i].mapped[slot] : NULL;
            /* A copied frame is a SNAPSHOT of the buffer, not a mapping of it, so it must never be
             * cached the way a real mapping is — the compositor writes the same dma_buf again and
             * a stale copy would freeze the mirror on one image. Re-copied per frame, and freed
             * below by the same `slot < 0` rule that frees an uncached mapping. */
            bool copied = false;
            if (!vk) {
               vk = import_frame(&im, m, fd);
               if (!vk) {
                  /* Not importable — but a LINEAR buffer is still READABLE. Copy it rather than
                   * drop it (bd remora-90y). */
                  vk = import_frame_copy(&im, m, fd);
                  copied = vk != NULL;
               }
               if (!vk) {
                  /* A buffer that cannot be imported drops every frame composed into it, and
                   * MEASURED LIVE that is what a full GPU looks like: under VRAM pressure the
                   * driver places new compose buffers in system memory, whose dma_bufs offer no
                   * device-local memory type, and the mirror flickers between the buffers it can
                   * see and holes where it cannot. Import errors print once per attempt above;
                   * this line keeps a running total and names the likely cause, because the
                   * in-vs-out stats gap alone reads as mere slowness (bd remora-e5x.18.2/.18.3). */
                  dropped_unimportable++;
                  if (av_gettime_relative() - last_drop_note_us > 5000000) {
                     /* Say what was actually wrong with the buffer. This used to assert "GPU
                      * memory is full — free VRAM", which sent three separate debugging sessions
                      * at the wrong thing while VRAM sat at 5.5/8.2 GB: the buffers were LINEAR,
                      * and a linear dma_buf offers no device-local memory type however much VRAM
                      * is free (bd remora-90y). A non-zero modifier is a tiled buffer whose layout
                      * we cannot interpret, which is a genuinely different failure. */
                     fprintf(stderr,
                             "[encoder] %" PRIu64 " display frames dropped: buffer modifier=0x%"
                             PRIx64 " could be neither imported nor copied%s\n",
                             dropped_unimportable, m->modifier,
                             m->modifier ? " (tiled layout — only linear buffers can be copied)"
                                         : " (linear, but the CPU copy failed)");
                     last_drop_note_us = av_gettime_relative();
                  }
                  continue;
               }
               if (copied) {
                  /* Loud, once per stretch: the mirror is alive but paying a full CPU copy per
                   * frame. That is worth fixing at the source (bd remora-e5x.18.5, the gralloc
                   * allocation path) and worth never mistaking for the fast path. */
                  copied_frames++;
                  if (av_gettime_relative() - last_copy_note_us > 5000000) {
                     fprintf(stderr,
                             "[encoder] %" PRIu64 " frames COPIED through system memory: the "
                             "compositor allocated linear buffers that Vulkan cannot import. The "
                             "mirror works but every frame costs a full copy\n",
                             copied_frames);
                     last_copy_note_us = av_gettime_relative();
                  }
               } else if (slot >= 0) {
                  pubs[i].mapped[slot] = vk;
               }
            }
            imported++;

            if (probe) {
               if (imported == 1 || imported % 30 == 0)
                  fprintf(stderr, "[encoder] imported %" PRIu64 " frames (res_id=%u)\n",
                          imported, m->res_id);
               if (slot < 0)
                  av_frame_free(&vk); /* uncached: nothing else owns it */
               if (max_frames && (int)imported >= max_frames)
                  stop_requested = 1;
               continue;
            }

            const bool force_key = outputs_take_keyframe_request(outs, n_outs);
            const int enc_rc =
               encode_frame(&enc, outs, n_outs, vk, force_key, &stats, &encoded, max_frames,
                            pkt, filtered);
            if (copied) {
               /* We own this one. Hand it to the keepalive and release the previous copy — doing
                * it in this order matters, because last_frame may still point at that copy. */
               if (last_copy && last_copy != vk)
                  av_frame_free(&last_copy);
               last_copy = vk;
               last_frame = vk;
            } else if (slot < 0) {
               av_frame_free(&vk); /* uncached: nothing else owns it */
            } else {
               /* The newest cached frame is the keepalive's material: a still screen composites
                * nothing, so this is the only thing that can be re-sent. */
               last_frame = vk;
            }
            last_encode_us = av_gettime_relative();
            if (enc_rc) {
               gpu_oom_note(enc.avctx->width, enc.avctx->height);
               /* Published before the context is freed — the sizes live in it. "rebuilding" and
                * not "degraded": on a still screen the rebuild waits for the next composed frame,
                * and a reader should see the truth of that window (bd remora-e5x.18.3). */
               status_write("rebuilding", im.width, im.height, enc.avctx->width,
                            enc.avctx->height, enc.avctx->bit_rate, degrade + 1);
               encoder_free(&enc);
               degrade++;
               ready = false;
               /* The rebuilt encoder is a NEW stream: new parameter sets, no shared references.
                * Every attached consumer must re-receive config and wait for the new stream's
                * first keyframe, or it decodes garbage against the old one. */
               for (unsigned oi = 0; oi < n_outs; oi++) {
                  outs[oi].sent_config = false;
                  outs[oi].need_keyframe = true;
                  outs[oi].waiting_key = true;
               }
               continue;
            }
         }
      }

      /* Idle sweep: an entry untouched for CACHE_IDLE_US is a buffer the guest freed — live
       * ones are touched every frame — and each pins its dma_buf (~29 MiB at 4K) until this
       * process exits; measured as the +118 MiB churn plateau that idle never returned
       * (bd remora-e5x.19.2). Two deliberate exceptions: the keepalive's frame is spared,
       * because a consumer attaching to a STILL screen is served from it and a still screen
       * touches nothing (bd remora-e5x.18.4); and a wrongly swept live-but-idle buffer costs
       * one fd re-request when it wakes, not a blackout.
       *
       * A shorter grace for publishers that are NOT the display was considered and REJECTED on
       * measurement (bd remora-1dz): only the chosen publisher's entries are ever touched, so a
       * loser does hold its buffers for the full minute — but a live 4K session was measured
       * pinning exactly ONE dma_buf, so the term is noise, and this path carries two beads' worth
       * of warnings about eager dropping causing blackouts. The report below is what would make
       * that case visible if it ever arrives. */
      {
         const int64_t now = av_gettime_relative();
         if (now - last_sweep_us > 5000000) {
            last_sweep_us = now;
            unsigned swept = 0;
            for (unsigned i = 0; i < n_pubs; i++) {
               for (unsigned k = 0; k < pubs[i].n_fds;) {
                  if (now - pubs[i].used_us[k] < CACHE_IDLE_US ||
                      (last_frame && pubs[i].mapped[k] == last_frame)) {
                     k++;
                     continue;
                  }
                  close(pubs[i].fds[k]);
                  av_frame_free(&pubs[i].mapped[k]); /* not last_frame — just checked */
                  pubs[i].n_fds--;
                  pubs[i].res_ids[k] = pubs[i].res_ids[pubs[i].n_fds];
                  pubs[i].fds[k] = pubs[i].fds[pubs[i].n_fds];
                  pubs[i].mapped[k] = pubs[i].mapped[pubs[i].n_fds];
                  pubs[i].used_us[k] = pubs[i].used_us[pubs[i].n_fds];
                  swept++;
               }
            }

            /* What the cache is holding, so the next session MEASURES this instead of inferring
             * it from total VRAM (bd remora-1dz). Every cached fd pins its dma_buf, so the pin is
             * the encoder's real retained footprint — the frames themselves are the guest's
             * allocations, which is why it does not show up as anything this process allocated.
             * The MiB figure is an ESTIMATE: each publisher's largest seen extent at 4 bytes a
             * pixel, since the per-entry size is not something the publisher tells us. Printed
             * only when it is worth reading — a sweep freed something, or the estimate is large
             * — and at most every 30 s, so a healthy single-display session stays quiet. */
            unsigned held = 0;
            uint64_t est_bytes = 0;
            for (unsigned i = 0; i < n_pubs; i++) {
               held += pubs[i].n_fds;
               est_bytes += (uint64_t)pubs[i].n_fds * pubs[i].width * pubs[i].height * 4;
            }
            const unsigned est_mib = (unsigned)(est_bytes >> 20);
            if ((swept || est_mib >= 256) && now - last_pin_note_us > 30000000) {
               last_pin_note_us = now;
               fprintf(stderr, "[encoder] pinned %u buffer%s across %u publisher%s, ~%u MiB est"
                               "%s\n", held, held == 1 ? "" : "s", n_pubs,
                       n_pubs == 1 ? "" : "s", est_mib,
                       swept ? " (swept this pass)" : "");
               for (unsigned i = 0; i < n_pubs; i++)
                  if (pubs[i].n_fds)
                     fprintf(stderr, "[encoder]   publisher %u: %u pinned at %ux%u%s\n", i + 1,
                             pubs[i].n_fds, pubs[i].width, pubs[i].height,
                             chosen == (int)i ? "  <-- the display" : "");
            }
         }
      }

      /* Keepalive: re-encode the last composed frame when nothing new arrives, and immediately
       * when a newcomer is owed a keyframe. Two symptoms, one cause — the compositor only submits
       * when something changes, but a consumer needs frames regardless: a mirror attaching to a
       * still screen showed GREY until the first swipe, and the device encoder this path replaces
       * repeats its last frame for the same reason (REPEAT_FRAME_DELAY). The re-encode reads the
       * cached dma_buf mapping, which still holds the last composited image. */
      if (ready && !probe && last_frame) {
         const int64_t now = av_gettime_relative();
         bool owed = false;
         for (unsigned i = 0; i < n_outs; i++)
            if (outs[i].fd >= 0 && outs[i].framed && outs[i].need_keyframe)
               owed = true;
         /* The keepalive's material is only honest while NOTHING NEWER has been published. When
          * newer frames arrived but could not be encoded (unimportable buffers, above), re-sending
          * the older cached frame flashes the past between the frames that do get through —
          * observed live as heavy glitching during VRAM-pressure episodes. Freezing on the last
          * delivered frame is the calm failure mode; only a keyframe debt (a consumer that has
          * NOTHING yet) still gets served stale, because stale beats black. */
         const bool current = chosen < 0 || pubs[chosen].last_frame_us <= last_encode_us;
         if (owed || (current && now - last_encode_us > KEEPALIVE_US)) {
            const bool key = outputs_take_keyframe_request(outs, n_outs);
            const int rc = encode_frame(&enc, outs, n_outs, last_frame, key, &stats, &encoded,
                                        max_frames, pkt, filtered);
            last_encode_us = now;
            if (rc) {
               gpu_oom_note(enc.avctx->width, enc.avctx->height);
               status_write("rebuilding", im.width, im.height, enc.avctx->width,
                            enc.avctx->height, enc.avctx->bit_rate, degrade + 1);
               encoder_free(&enc);
               degrade++;
               ready = false;
               for (unsigned oi = 0; oi < n_outs; oi++) {
                  outs[oi].sent_config = false;
                  outs[oi].need_keyframe = true;
                  outs[oi].waiting_key = true;
               }
            }
         }
      }
   }

   fprintf(stderr, "[encoder] stopping: %" PRIu64 " imported, %" PRIu64 " encoded\n",
           imported, encoded);

   /* Everything from here is disposal — deadline it (bd remora-zz9k, rationale at the handler).
    * Ten seconds is an order of magnitude above a measured clean teardown. */
   signal(SIGALRM, on_teardown_deadline);
   alarm(10);

   if (ready && !probe) {
      avcodec_send_frame(enc.avctx, NULL);
      while (avcodec_receive_packet(enc.avctx, pkt) >= 0) {
         outputs_packet(outs, n_outs, pkt);
         av_packet_unref(pkt);
      }
   }
   for (unsigned i = 0; i < n_outs; i++)
      close(outs[i].fd);
   av_packet_free(&pkt);
   av_frame_free(&filtered);
   for (unsigned i = 0; i < n_pubs; i++) {
      for (unsigned k = 0; k < pubs[i].n_fds; k++) {
         close(pubs[i].fds[k]);
         av_frame_free(&pubs[i].mapped[k]);
      }
      close(pubs[i].sock);
   }
   close(lsock);
   unlink(frame_socket);
   if (g_status_path[0])
      unlink(g_status_path); /* a clean exit leaves no stale story; failing exits keep theirs */
   /* Also remove the CONSUMER socket. A leftover inode makes a dead encoder look present to
    * anything that probes with [ -S … ], which is exactly how Remora's ensureHostEncoder decided
    * an orphan was healthy and reused it (bd remora-e5x.13). */
   if (video_lsock >= 0) {
      close(video_lsock);
      unlink(video_socket);
   }
   /* A probe that ran out of time having imported NOTHING is a failed probe: nothing was
    * publishing frames. Say so in the exit status — it is the whole question a probe asks. One
    * that timed out after importing has already proved what it was asked to prove. */
   return (timed_out && probe && imported == 0) ? 1 : 0;
}
