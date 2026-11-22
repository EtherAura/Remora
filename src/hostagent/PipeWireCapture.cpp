#include "PipeWireCapture.h"

#include <QMetaObject>

#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace remora::hostagent {

namespace {

// remora-frame-encoder's wire format (struct frame_msg / fd_req there): native byte order and
// native padding, because both ends are on this machine by construction — they share an fd.
constexpr uint32_t kFrameMagic = 0x524d4632u;  // "RMF2"
constexpr uint32_t kFdReqMagic = 0x524d4651u;  // "RMFQ"
struct FrameMsg {
    uint32_t magic, resId, width, height, hasFd;
    uint32_t fourcc, offset, pitch;
    uint64_t modifier;
};
static_assert(sizeof(FrameMsg) == 40, "must match remora-frame-encoder's struct frame_msg");
struct FdReq {
    uint32_t magic, resId;
};

constexpr uint64_t kModLinear = 0;

// The formats the encoder takes (its fourcc_to_pixfmt), PipeWire's names on the left. SPA names
// bytes in memory order and DRM names a little-endian word, hence the reversal.
uint32_t drmFourcc(uint32_t spaFormat) {
    switch (spaFormat) {
        case SPA_VIDEO_FORMAT_BGRx: return 0x34325258;  // XR24
        case SPA_VIDEO_FORMAT_BGRA: return 0x34325241;  // AR24
        case SPA_VIDEO_FORMAT_RGBx: return 0x34324258;  // XB24
        case SPA_VIDEO_FORMAT_RGBA: return 0x34324241;  // AB24
        default: return 0;
    }
}
// In order of preference. The alpha formats first: they are what the Venus path already imports
// on NVIDIA, and Vulkan there refuses BGRx (bgr0) in KWin's tiled layout — measured.
const uint32_t kFormats[] = {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx,
                             SPA_VIDEO_FORMAT_RGBx};

// The layouts of KWin's offer the encoder can import. Two kinds are refused, both measured on an
// RTX 3070 Ti: NVIDIA's COMPRESSED block-linear layouts (the c field, bits 23-25 of the vendor
// code), which Vulkan will not map — VK_ERROR_FORMAT_NOT_SUPPORTED, and KWin picks one of them
// when offered — and DRM_FORMAT_MOD_INVALID, an implicit layout no importer can be told about.
// Everything else passes: other vendors' layouts are imported on their own GPUs as offered.
bool importable(quint64 modifier) {
    constexpr quint64 kModInvalid = 0x00ffffffffffffffull;
    constexpr quint64 kVendorNvidia = 0x03;
    if (modifier == kModInvalid) return false;
    if ((modifier >> 56) == kVendorNvidia && ((modifier >> 23) & 0x7) != 0) return false;
    return true;
}

// The values of a property that may or may not be a choice. For an enum choice the first value
// is the default and repeats among the alternatives, so callers dedupe.
template <typename T>
QList<T> propValues(const spa_pod *param, uint32_t key) {
    QList<T> out;
    const spa_pod_prop *prop = spa_pod_find_prop(param, nullptr, key);
    if (!prop) return out;
    uint32_t n = 0, choice = 0;
    const spa_pod *val = spa_pod_get_values(&prop->value, &n, &choice);
    if (!val || SPA_POD_BODY_SIZE(val) < sizeof(T)) return out;
    const T *vals = static_cast<const T *>(SPA_POD_BODY(val));
    for (uint32_t i = 0; i < n; ++i)
        if (!out.contains(vals[i])) out << vals[i];
    return out;
}

}  // namespace

// PipeWire's callback tables, with its own signatures, forwarding to the capture on its thread.
struct PwCallbacks {
    static void registryGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t,
                               const spa_dict *props) {
        auto *self = static_cast<PipeWireCapture *>(data);
        if (self->connected_ || self->node_ || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ||
            !props)
            return;
        const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
        if (!serial || QByteArray(serial).toULongLong() != self->serial_) return;
        static const pw_node_events nodeEvents = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = nullptr,
            .param = &nodeParam,
        };
        self->node_ = static_cast<pw_proxy *>(
            pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        pw_proxy_add_object_listener(self->node_, self->nodeHook_, &nodeEvents, self);
        pw_node_enum_params(reinterpret_cast<pw_node *>(self->node_), 0, SPA_PARAM_EnumFormat, 0,
                            UINT32_MAX, nullptr);
        // The answer to this sync arrives after every EnumFormat the node sends.
        self->enumSeq_ = pw_core_sync(self->core_, PW_ID_CORE, 0);
    }

    static void nodeParam(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
        auto *self = static_cast<PipeWireCapture *>(data);
        if (id != SPA_PARAM_EnumFormat || !param) return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
            mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
            return;
        const QList<int64_t> mods = propValues<int64_t>(param, SPA_FORMAT_VIDEO_modifier);
        if (mods.isEmpty()) return;  // the shared-memory offer
        for (const uint32_t f : propValues<uint32_t>(param, SPA_FORMAT_VIDEO_format)) {
            QList<quint64> &known = self->offer_[f];
            for (const int64_t m : mods)
                if (importable(quint64(m)) && !known.contains(quint64(m))) known << quint64(m);
        }
    }

    static void coreDone(void *data, uint32_t id, int seq) {
        auto *self = static_cast<PipeWireCapture *>(data);
        if (id != PW_ID_CORE || self->connected_) return;
        // Either the offer is in, or the registry's first sync found no such node — KWin may not
        // have announced it yet, and the stream connects by serial regardless.
        if (seq == self->enumSeq_ || (self->enumSeq_ < 0 && !self->node_)) self->connectStream();
    }

    static void coreError(void *data, uint32_t id, int, int res, const char *message) {
        if (id != PW_ID_CORE) return;
        static_cast<PipeWireCapture *>(data)->fail(
            QStringLiteral("PipeWire: %1 (%2)")
                .arg(QString::fromUtf8(message), QString::fromUtf8(spa_strerror(res))));
    }

    static void streamStateChanged(void *data, pw_stream_state, pw_stream_state state,
                                   const char *error) {
        auto *self = static_cast<PipeWireCapture *>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            self->fail(QStringLiteral("the stream failed: %1").arg(QString::fromUtf8(error)));
        } else if (state == PW_STREAM_STATE_UNCONNECTED && self->announced_) {
            self->fail(QStringLiteral("KWin closed the stream"));
        }
    }

    static void streamParamChanged(void *data, uint32_t id, const spa_pod *param) {
        auto *self = static_cast<PipeWireCapture *>(data);
        if (id != SPA_PARAM_Format || !param) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        self->dmabuf_ = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
        self->size_ = QSize(int(info.size.width), int(info.size.height));
        self->fourcc_ = drmFourcc(info.format);
        self->modifier_ = self->dmabuf_ ? info.modifier : kModLinear;
        if (!self->fourcc_) {
            self->fail(QStringLiteral("KWin chose a pixel format the encoder does not take (%1)")
                           .arg(info.format));
            return;
        }
        uint8_t buf[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        const spa_pod *bp = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(self->dmabuf_ ? (1 << SPA_DATA_DmaBuf)
                                                   : (1 << SPA_DATA_MemFd))));
        pw_stream_update_params(self->stream_, &bp, 1);
        if (!self->announced_.exchange(true)) {
            const QSize size = self->size_;
            const bool dmabuf = self->dmabuf_;
            QMetaObject::invokeMethod(self, [self, size, dmabuf] { emit self->streaming(size, dmabuf); },
                                      Qt::QueuedConnection);
        }
    }

    static void streamAddBuffer(void *data, pw_buffer *b) {
        auto *self = static_cast<PipeWireCapture *>(data);
        // Never reused: the encoder caches an import per res_id, and a recycled id would hand it
        // a stale mapping for a new buffer.
        self->buffers_.insert(b, {self->nextResId_++, false});
    }

    static void streamRemoveBuffer(void *data, pw_buffer *b) {
        auto *self = static_cast<PipeWireCapture *>(data);
        self->buffers_.remove(b);
        if (self->held_ == b) self->held_ = nullptr;
    }

    static void streamProcess(void *data) {
        auto *self = static_cast<PipeWireCapture *>(data);
        // Only the newest frame matters; older ones go straight back to KWin.
        pw_buffer *b = nullptr;
        while (pw_buffer *next = pw_stream_dequeue_buffer(self->stream_)) {
            if (b) pw_stream_queue_buffer(self->stream_, b);
            b = next;
        }
        if (!b) return;
        // KWin flags a buffer that carries no new picture (a cursor-only update) as corrupted:
        // it goes back, and the frame held stays the newest.
        const spa_chunk *chunk = b->buffer->n_datas > 0 ? b->buffer->datas[0].chunk : nullptr;
        const auto *header = static_cast<const spa_meta_header *>(
            spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof(spa_meta_header)));
        if (!chunk || (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
            (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED))) {
            pw_stream_queue_buffer(self->stream_, b);
            return;
        }
        self->readFdRequests();
        self->publish(b);
        if (self->held_) pw_stream_queue_buffer(self->stream_, self->held_);
        self->held_ = b;
    }

    static void socketIo(void *data, int, uint32_t mask) {
        static_cast<PipeWireCapture *>(data)->onSocket(mask);
    }
};

PipeWireCapture::PipeWireCapture(QObject *parent)
    : QObject(parent), coreHook_(new spa_hook{}), registryHook_(new spa_hook{}),
      nodeHook_(new spa_hook{}), streamHook_(new spa_hook{}) {}

PipeWireCapture::~PipeWireCapture() {
    stop();
    delete coreHook_;
    delete registryHook_;
    delete nodeHook_;
    delete streamHook_;
}

bool PipeWireCapture::start(quint64 serial, const QString &frameSocket, QString *error) {
    static const bool initialized = (pw_init(nullptr, nullptr), true);
    Q_UNUSED(initialized);
    serial_ = serial;

    sock_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray path = frameSocket.toLocal8Bit();
    if (sock_ < 0 || size_t(path.size()) >= sizeof addr.sun_path) {
        *error = QStringLiteral("could not open a socket for the encoder");
        return false;
    }
    std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
    if (::connect(sock_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
        *error = QStringLiteral("the encoder is not listening on %1: %2")
                     .arg(frameSocket, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    loop_ = pw_thread_loop_new("remora-capture", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!loop_ || !context_ || pw_thread_loop_start(loop_) < 0) {
        *error = QStringLiteral("could not start a PipeWire loop");
        return false;
    }
    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        *error = QStringLiteral("could not connect to PipeWire");
        return false;
    }
    static const pw_core_events coreEvents = {
        .version = PW_VERSION_CORE_EVENTS,
        .info = nullptr,
        .done = &PwCallbacks::coreDone,
        .ping = nullptr,
        .error = &PwCallbacks::coreError,
    };
    pw_core_add_listener(core_, coreHook_, &coreEvents, this);
    sockSource_ = pw_loop_add_io(pw_thread_loop_get_loop(loop_), sock_, SPA_IO_IN, false,
                                 &PwCallbacks::socketIo, this);
    static const pw_registry_events registryEvents = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = &PwCallbacks::registryGlobal,
        .global_remove = nullptr,
    };
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, registryHook_, &registryEvents, this);
    // The first sync: every global already known arrives before its done.
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_unlock(loop_);
    return true;
}

void PipeWireCapture::connectStream() {
    connected_ = true;
    QStringList offered;
    for (auto it = offer_.cbegin(); it != offer_.cend(); ++it) {
        QStringList mods;
        for (const quint64 m : *it) mods << QStringLiteral("0x%1").arg(m, 0, 16);
        offered << QStringLiteral("%1:[%2]").arg(it.key()).arg(mods.join(QLatin1Char(',')));
    }
    qInfo("capture: KWin offers DMA-BUF layouts per format (spa format:count) %s",
          qUtf8Printable(offered.join(QLatin1Char(' '))));
    uint8_t buf[4096];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    std::vector<const spa_pod *> params;
    // DMA-BUF first, one EnumFormat per format, each naming every layout KWin offered for it.
    for (const uint32_t f : kFormats) {
        const QList<quint64> mods = offer_.value(f);
        if (mods.isEmpty()) continue;
        spa_pod_frame obj, choice;
        spa_pod_builder_push_object(&b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_VIDEO_format, SPA_POD_Id(f), 0);
        spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier,
                             SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(&b, &choice, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(&b, int64_t(mods.first()));  // the default
        for (const quint64 m : mods) spa_pod_builder_long(&b, int64_t(m));
        spa_pod_builder_pop(&b, &choice);
        params.push_back(static_cast<const spa_pod *>(spa_pod_builder_pop(&b, &obj)));
    }
    // Shared memory last: no modifier, any of the formats.
    params.push_back(static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx,
                               SPA_VIDEO_FORMAT_RGBx))));

    const QByteArray target = QByteArray::number(serial_);
    stream_ = pw_stream_new(core_, "remora-host-agent",
                            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                              "Capture", PW_KEY_MEDIA_ROLE, "Screen",
                                              PW_KEY_TARGET_OBJECT, target.constData(), nullptr));
    static const pw_stream_events streamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = &PwCallbacks::streamStateChanged,
        .control_info = nullptr,
        .io_changed = nullptr,
        .param_changed = &PwCallbacks::streamParamChanged,
        .add_buffer = &PwCallbacks::streamAddBuffer,
        .remove_buffer = &PwCallbacks::streamRemoveBuffer,
        .process = &PwCallbacks::streamProcess,
    };
    pw_stream_add_listener(stream_, streamHook_, &streamEvents, this);
    // No MAP_BUFFERS: nothing here reads a pixel; the encoder maps what it needs.
    const int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                                      PW_STREAM_FLAG_AUTOCONNECT, params.data(),
                                      uint32_t(params.size()));
    if (res < 0)
        fail(QStringLiteral("could not connect to the stream: %1")
                 .arg(QString::fromUtf8(spa_strerror(res))));
}

void PipeWireCapture::publish(pw_buffer *b) {
    const spa_data &d = b->buffer->datas[0];
    if (d.fd < 0 || !d.chunk) return;
    auto it = buffers_.find(b);
    if (it == buffers_.end()) return;

    FrameMsg m{};
    m.magic = kFrameMagic;
    m.resId = it->resId;
    m.width = uint32_t(size_.width());
    m.height = uint32_t(size_.height());
    m.hasFd = it->fdSent ? 0 : 1;
    m.fourcc = fourcc_;
    m.offset = uint32_t(d.mapoffset + d.chunk->offset);
    m.pitch = uint32_t(d.chunk->stride);
    m.modifier = modifier_;

    iovec iov{&m, sizeof m};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    if (m.hasFd) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        const int fd = int(d.fd);
        std::memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    }
    // Never block KWin's stream on the encoder: a frame it is too busy to take is dropped, and
    // the next one supersedes it anyway.
    const ssize_t n = ::sendmsg(sock_, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == ssize_t(sizeof m)) {
        it->fdSent = true;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fail(QStringLiteral("the encoder went away: %1")
                 .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
}

void PipeWireCapture::readFdRequests() {
    // The encoder evicts its oldest imports, and drops those of another size on a resize, and
    // asks again for a buffer still in use.
    FdReq rq;
    while (::recv(sock_, &rq, sizeof rq, MSG_DONTWAIT) == ssize_t(sizeof rq)) {
        if (rq.magic != kFdReqMagic) continue;
        for (Buffer &buf : buffers_)
            if (buf.resId == rq.resId) buf.fdSent = false;
    }
}

void PipeWireCapture::onSocket(uint32_t mask) {
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(loop_), sockSource_);
        sockSource_ = nullptr;
        return fail(QStringLiteral("the encoder went away"));
    }
    readFdRequests();
    if (held_) {
        auto it = buffers_.constFind(held_);
        if (it != buffers_.cend() && !it->fdSent) publish(held_);
    }
}

void PipeWireCapture::fail(const QString &why) {
    QMetaObject::invokeMethod(this, [this, why] { emit failed(why); }, Qt::QueuedConnection);
}

void PipeWireCapture::stop() {
    if (loop_) {
        pw_thread_loop_lock(loop_);
        if (sockSource_) pw_loop_destroy_source(pw_thread_loop_get_loop(loop_), sockSource_);
        sockSource_ = nullptr;
        held_ = nullptr;  // the stream's, and it goes with it
        if (stream_) pw_stream_destroy(stream_);
        if (node_) pw_proxy_destroy(node_);
        if (registry_) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_));
        if (core_) pw_core_disconnect(core_);
        stream_ = nullptr;
        node_ = nullptr;
        registry_ = nullptr;
        core_ = nullptr;
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
    }
    if (context_) pw_context_destroy(context_);
    if (loop_) pw_thread_loop_destroy(loop_);
    context_ = nullptr;
    loop_ = nullptr;
    if (sock_ >= 0) ::close(sock_);
    sock_ = -1;
}

}  // namespace remora::hostagent
