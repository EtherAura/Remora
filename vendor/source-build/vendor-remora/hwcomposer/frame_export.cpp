// Frame export: publish the composed client target to Remora's host encoder (bd remora-emoe R4
// phase 2). See frame_export.h for why this exists and why it is opt-in.

#define LOG_TAG "hwcomposer.remora"

#include "frame_export.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <mutex>
#include <thread>

#include <cutils/properties.h>
#include <log/log.h>

namespace remora {
namespace {

// THE WIRE FORMAT IS remora-frame-encoder.c's, copied verbatim and not negotiated. Both magics and
// both structs must match that file (which in turn matches vkr_remora_track.h on the Venus side) —
// three copies of one contract, which is a wart, but the encoder is host-side C and this is an
// in-image HAL, so there is no header they can share.
constexpr uint32_t kFrameMagic = 0x524d4632u;  // "RMF2"
constexpr uint32_t kFdReqMagic = 0x524d4651u;  // "RMFQ"

struct FrameMsg {
    uint32_t magic, res_id, width, height, has_fd;
    uint32_t fourcc, offset, pitch;
    uint64_t modifier;
};
struct FdReq {
    uint32_t magic, res_id;
};

// minigbm's gralloc0 perform() contract. Private to
// external/minigbm/cros_gralloc/gralloc0/gralloc0.cc — exported in no header — so it is restated
// here and we are coupled to that layout. It is the only route to the layout on this image: there
// is NO gralloc4 (only mapper@2.0-impl-2.1), so IMapper's metadata getters answer UNAVAILABLE.
// The enum there is anonymous and starts at 0, which makes GET_BUFFER_INFO 4 POSITIONALLY — an op
// inserted before it upstream would silently repoint this at a different question, which is why
// every reply is sanity-checked below rather than trusted.
struct CrosBufferInfo {
    uint32_t drm_fourcc;
    int num_fds;
    int fds[4];
    uint64_t modifier;
    uint32_t offset[4];
    uint32_t stride[4];
};
constexpr int kGetBufferInfo = 4;

// One entry per distinct buffer we have published. The encoder caches an fd per res_id and never
// asks again unless it evicts (fd_req), so the fd is sent once per buffer PER CONNECTION — once it
// has actually landed, which is not the same as once we have tried (see fd_sent) — and the id must
// be stable for that buffer's lifetime. Identity is the dma_buf's INODE, not the handle pointer:
// handles are freed and their addresses reused, and a recycled address would silently alias two
// buffers.
// Sized to the encoder's own MAX_CACHED_FDS; SurfaceFlinger cycles two framebuffer targets here, so
// this never comes close to full.
constexpr int kMaxBuffers = 16;
struct Slot {
    ino_t ino;
    uint32_t res_id;
    int fd;  // our dup, kept so an fd_req can be answered later
    // Whether the encoder has actually RECEIVED the fd — not whether we tried to send it. See
    // sendFrame(): a send lost to a full socket must be retried on the next frame for this buffer,
    // otherwise the encoder is left holding an unresolvable res_id.
    bool fd_sent;
    // The last description we published for this buffer, replayed verbatim to answer an fd_req.
    // See drainRequests() for why a bare res_id is not enough.
    FrameMsg last;
};

Slot g_slots[kMaxBuffers];
int g_nslots = 0;
uint32_t g_next_res_id = 1;

int g_sock = -1;
bool g_enabled = false;
bool g_dropped_once = false;  // the "encoder went away" notice logs once, not once per frame
int g_retry_countdown = 0;    // frames to wait before the next connect attempt
char g_sock_path[PROPERTY_VALUE_MAX];
const gralloc_module_t *g_gralloc = nullptr;
bool g_warned_layout = false;
// The buffer most recently published — what the reconnect watchdog replays so a restarted encoder
// has material without waiting for the compositor to feel like composing. By res_id, not slot
// index: eviction memmoves the table and an index would silently point at a different buffer.
uint32_t g_last_res_id = 0;
// Everything above is shared between the composer's present() path and the watchdog thread below.
// Held only for microseconds on either side (every send is already non-blocking), so the composer
// never actually waits on it.
std::mutex g_lock;

// A composer must never block. Every send is non-blocking and a full socket drops the frame:
// falling behind the encoder is a dropped frame, waiting for it is a frozen display.

// A DEAD ENCODER IS TRANSIENT, NOT TERMINAL, and treating it as terminal was a real bug: the first
// version set g_enabled=false here, so the first time the encoder restarted — which it does on
// every reconnect and every deploy — the composer stopped exporting for the life of the container
// and only a recreate brought it back. Observed exactly that: "frame export disabled: encoder
// socket closed", then silence. Drop the connection and let the next frame reconnect, which is the
// same treatment a refused initial connect already got; only genuine unavailability (no socket
// property, no gralloc perform) disables export for good.
void dropConnection(const char *why) {
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    // Every fd we handed over went to the socket we just closed. A reconnected encoder is a NEW
    // process holding none of them, so the whole table is undelivered again — keeping the flags
    // set would make each buffer wait for an fd_req that only fires after frames are already
    // being dropped.
    for (int i = 0; i < g_nslots; ++i) g_slots[i].fd_sent = false;
    if (!g_dropped_once) {
        g_dropped_once = true;
        ALOGI("frame export: %s — will reconnect when the encoder returns", why);
    }
}

bool connectEncoder() {
    if (g_sock >= 0) return true;
    // Compose mode is usually NOT running, so this path is the common one. A connect() to an absent
    // unix socket fails instantly, but sixty of them a second is still pointless work in the
    // composer's own present() path — back off to roughly one attempt a second.
    if (g_retry_countdown > 0) {
        --g_retry_countdown;
        return false;
    }
    g_retry_countdown = 60;
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return false;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
    // The encoder LISTENS and publishers connect, so this is either accepted at once or refused —
    // no waiting either way. A refusal just means compose mode is not running right now, which is
    // normal and must stay silent rather than logging per frame.
    if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(s);
        return false;
    }
    g_sock = s;
    g_dropped_once = false;
    g_retry_countdown = 0;
    ALOGI("frame export: connected to %s", g_sock_path);
    return true;
}

Slot *slotFor(ino_t ino) {
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].ino == ino) return &g_slots[i];
    return nullptr;
}

// DROPPING A FRAME AND DROPPING AN FD ARE NOT THE SAME EVENT, and collapsing them stalled the
// mirror. A frame lost to a full socket costs one frame; the fd rides the FIRST message for a
// buffer, so losing that one message means the encoder holds a res_id it can never resolve and
// every subsequent frame for that buffer is undecodable. Observed as a picture that froze while
// the encoder kept emitting keepalive packets: `imported` stopped at 1808 and stayed there.
// So the caller is told which happened and only marks the fd delivered when it really went out.
enum class Send { Ok, Dropped, Failed };

// Send one message, with the fd attached when the encoder has not seen this buffer before.
Send sendFrame(const FrameMsg &m, int fd) {
    iovec iov {const_cast<FrameMsg *>(&m), sizeof(m)};
    msghdr mh {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        cmsghdr align;
    } cmsg_u;
    if (fd >= 0) {
        memset(&cmsg_u, 0, sizeof(cmsg_u));
        mh.msg_control = cmsg_u.buf;
        mh.msg_controllen = sizeof(cmsg_u.buf);
        cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
    }
    const ssize_t n = sendmsg(g_sock, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == ssize_t(sizeof(m))) return Send::Ok;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Send::Dropped;  // encoder behind
    dropConnection("encoder socket closed");
    return Send::Failed;
}

// The one message that travels back: re-send a buffer's fd after the encoder evicted it. Without
// answering this, an evicted buffer would be a permanent blackout for that res_id.
//
// THE REPLY IS A FULL FRAME, NOT A BARE res_id, and getting that wrong took the mirror down in the
// first soak. There is one message type on this socket, so the encoder does not read a reply as
// "here is your fd" — it reads it as the next frame and runs the whole pipeline on it: width and
// height against the current extent, then fourcc. A reply zeroed except magic/res_id/has_fd
// therefore announces a resize to 0x0 and an unknown format, and the observed result was exactly
// that, three lines apart:
//   [encoder] 1 frames dropped awaiting a buffer fd (re-requested from the publisher)
//   [encoder] display resized 3760x1992 -> 0x0, rebuilding
//   unsupported fourcc 0x00000000 — refusing to guess
// So the fd_req path tore down a working pipeline and could not rebuild it. Replaying the buffer's
// last published description makes the reply indistinguishable from a normal frame, which is what
// the encoder is entitled to assume.
void drainRequests() {
    FdReq r;
    for (;;) {
        const ssize_t n = recv(g_sock, &r, sizeof(r), MSG_DONTWAIT);
        if (n <= 0) {
            if (n == 0) dropConnection("encoder hung up");
            return;
        }
        if (n != ssize_t(sizeof(r)) || r.magic != kFdReqMagic) continue;
        for (int i = 0; i < g_nslots; ++i) {
            if (g_slots[i].res_id != r.res_id || g_slots[i].fd < 0) continue;
            // Never answer for a slot we have not published yet: a zero-width `last` would be the
            // very bug described above, arriving by a different road.
            if (g_slots[i].last.width == 0) break;
            FrameMsg m = g_slots[i].last;
            m.has_fd = 1;
            if (sendFrame(m, g_slots[i].fd) == Send::Ok) g_slots[i].fd_sent = true;
            break;
        }
    }
}

// Replay the last published buffer on a fresh connection, fd attached: the new encoder process
// has cached nothing, and dropConnection already cleared every fd_sent. Same replay drainRequests
// does, for the same reason — the reply must be indistinguishable from a normal frame.
void republishLastLocked() {
    if (g_last_res_id == 0) return;
    for (int i = 0; i < g_nslots; ++i) {
        if (g_slots[i].res_id != g_last_res_id || g_slots[i].fd < 0) continue;
        if (g_slots[i].last.width == 0) return;
        FrameMsg m = g_slots[i].last;
        m.has_fd = 1;
        if (sendFrame(m, g_slots[i].fd) == Send::Ok) g_slots[i].fd_sent = true;
        return;
    }
}

// THE RECONNECT WATCHDOG — retries on TIME, not frames. Reconnection used to ride entirely on
// frameExportPublish, which deadlocks with a still screen: an encoder that restarts while nothing
// composes is never reconnected to, because the retry that would reconnect only fires when a
// frame arrives — and the 60-frame backoff stretched that to minutes even on a screen that
// composed occasionally. Measured live: a fresh deploy's encoder sat at publishers=0
// for 81 s until a swipe composed enough frames to burn the countdown, and every mirror died at
// its 45 s first-frame timeout before that — "keeps crashing on connect". The Venus render server
// had this exact bug and this exact fix (venus-nvidia patch 0014) before publishing moved here;
// the frame-riding retry pattern must not come back a third time. On success the last composed
// frame is republished immediately, so a restarted encoder has material within ~500 ms regardless
// of composition.
void reconnectThread() {
    for (;;) {
        usleep(500 * 1000);
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_sock >= 0) continue;
        g_retry_countdown = 0;  // bypass the per-frame backoff: this loop IS the rate limit
        if (connectEncoder()) republishLastLocked();
    }
}

}  // namespace

void frameExportInit() {
    if (property_get("ro.boot.remora_frame_socket", g_sock_path, "") <= 0) return;

    const hw_module_t *mod = nullptr;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mod) != 0 || mod == nullptr) {
        ALOGW("frame export: no gralloc module, cannot learn buffer layout — staying off");
        return;
    }
    g_gralloc = reinterpret_cast<const gralloc_module_t *>(mod);
    if (g_gralloc->perform == nullptr) {
        ALOGW("frame export: gralloc implements no perform(), cannot learn buffer layout");
        g_gralloc = nullptr;
        return;
    }
    g_enabled = true;
    std::thread(reconnectThread).detach();
    ALOGI("frame export armed for %s", g_sock_path);
}

void frameExportPublish(buffer_handle_t target, int width, int height) {
    if (!g_enabled || target == nullptr) return;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!connectEncoder()) return;
    drainRequests();
    if (!g_enabled) return;

    CrosBufferInfo info;
    memset(&info, 0, sizeof(info));
    if (g_gralloc->perform(g_gralloc, kGetBufferInfo, target, &info) != 0 || info.num_fds < 1 ||
        info.fds[0] < 0) {
        if (!g_warned_layout) {
            g_warned_layout = true;
            ALOGW("frame export: gralloc could not describe the client target — not exporting");
        }
        return;
    }
    // SANITY-CHECK THE REPLY rather than trust it, because the op number is positional (see
    // CrosBufferInfo): if upstream ever inserts an op, this call returns a DIFFERENT struct and
    // every field would be plausible garbage. A pitch that cannot hold one row of pixels is the
    // cheapest thing that cannot be true, and catching it here turns a scrambled picture into a
    // refusal — which is the whole reason the layout is queried instead of assumed.
    if (info.stride[0] < uint32_t(width) * 4u || info.drm_fourcc == 0) {
        if (!g_warned_layout) {
            g_warned_layout = true;
            ALOGW("frame export: implausible layout (fourcc=0x%08x stride=%u for width %d) — "
                  "refusing to publish rather than encode garbage",
                  info.drm_fourcc, info.stride[0], width);
        }
        return;
    }

    struct stat st;
    if (fstat(info.fds[0], &st) != 0) return;

    Slot *slot = slotFor(st.st_ino);
    int fd_to_send = -1;
    if (slot == nullptr) {
        if (g_nslots == kMaxBuffers) {
            // Full. SurfaceFlinger cycles two targets, so reaching this means something changed
            // shape repeatedly; drop the oldest rather than grow without bound.
            close(g_slots[0].fd);
            memmove(&g_slots[0], &g_slots[1], sizeof(Slot) * (kMaxBuffers - 1));
            --g_nslots;
        }
        slot = &g_slots[g_nslots++];
        slot->last = FrameMsg {};  // an evicted index carries the previous buffer's description
        slot->fd_sent = false;
        slot->ino = st.st_ino;
        slot->res_id = g_next_res_id++;
        // Our OWN dup: the handle's fd belongs to SurfaceFlinger and may be closed the moment the
        // buffer is released, but an fd_req can arrive long afterwards.
        slot->fd = fcntl(info.fds[0], F_DUPFD_CLOEXEC, 0);
    }
    // Attach the fd on every frame until one actually lands. Re-attaching costs a cmsg on a frame
    // we are sending regardless, and it makes delivery self-healing: the encoder's fd_req is then
    // a backstop rather than the only route out of a stalled buffer.
    if (!slot->fd_sent) fd_to_send = slot->fd;

    FrameMsg m {};
    m.magic = kFrameMagic;
    m.res_id = slot->res_id;
    m.width = uint32_t(width);
    m.height = uint32_t(height);
    m.has_fd = fd_to_send >= 0 ? 1u : 0u;
    m.fourcc = info.drm_fourcc;
    m.offset = info.offset[0];
    m.pitch = info.stride[0];
    // NOT assumed linear. Measured on this host: the client target's modifier is
    // 0x0300000000606014 — NVIDIA block-linear — even though stride == width and the allocation is
    // exactly w*h*4. Tiling changes byte ORDER, not size, so "no padding" is not evidence of
    // linearity, and an importer told the wrong modifier produces a scrambled frame with no error.
    m.modifier = info.modifier;
    // Recorded before the send so an fd_req or the watchdog's republish can replay it; has_fd is
    // per-message, not per-buffer, and the replayers set it for themselves.
    slot->last = m;
    g_last_res_id = slot->res_id;
    if (sendFrame(m, fd_to_send) == Send::Ok && fd_to_send >= 0) slot->fd_sent = true;
}

}  // namespace remora
