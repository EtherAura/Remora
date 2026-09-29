// Remora's HWC2 composer HAL — the replacement for the closed prebuilt (bd remora-emoe, the
// R4 half of bd remora-28ix.4). This is the last closed binary in a Remora image, and it is far
// smaller than "display HAL" suggests.
//
// WHAT THE PREBUILT ACTUALLY DOES, measured from the live A17 image rather than assumed: 49 KB,
// exports exactly one symbol (HMI), and links only libcutils/libsync/liblog/libc++/libdl/libc —
// ZERO GPU libraries, no EGL, no GL, no DRM, no GBM, no gralloc, no Vulkan. It therefore performs
// NO COMPOSITION AT ALL. SurfaceFlinger does 100% CLIENT composition on the GPU and hands the HAL
// a finished client-target buffer. The HAL's entire job is to be a plausible display: advertise
// one, pace it with vsync, and take delivery of the composed frame.
//
// So this file deliberately does not composite either. Every layer is forced to CLIENT, which is
// what the prebuilt effectively achieves and what Remora's frame path already assumes — frames are
// published from the Venus render server, not from here. Doing anything cleverer would change how
// the image renders at the same moment as replacing the HAL, and those are two different changes.
//
// PHASE 1 IS BEHAVIOURAL PARITY. Frame export (owning the point where the composed client target
// lands, so pick_display in remora-frame-encoder.c can stop guessing which publisher is the
// screen) is phase 2 and is deliberately NOT started here — see the bead.
//
// NOT DEFAULT-ON. gpu_config.sh keeps selecting the prebuilt until this is proven, because
// ro.hardware.hwcomposer is a RUNTIME property: both HALs ship in one image and switching is a
// property flip plus a container restart, not a rebuild. SurfaceFlinger is unforgiving about the
// vsync and fence protocol, and a display HAL that gets it wrong shows nothing at all, so the A/B
// has to be cheap enough to run repeatedly.
#define LOG_TAG "remora_hwc"

#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <sync/sync.h>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frame_export.h"

namespace {

constexpr hwc2_display_t kPrimaryDisplay = 1;
constexpr hwc2_config_t kPrimaryConfig = 1;

// ONE NAMESPACE (bd remora-28ix.4 step 4). This took two names and read Remora's
// first, falling back to the pre-migration spelling — which existed only because the closed
// binary this HAL replaced read the old names. That binary is gone from the image, the boot
// property adapter that fed it has been reduced to the two setprops that were never mapping,
// and the host has emitted ro.boot.remora_* since the flip. A second lookup that can no longer
// match anything is not tolerance, it is a second lookup.
int propInt(const char *name, int fallback) {
    char buf[PROPERTY_VALUE_MAX] = {0};
    if (property_get(name, buf, nullptr) > 0) return atoi(buf);
    return fallback;
}

struct Layer {
    int32_t compositionType = HWC2_COMPOSITION_DEVICE;
};

class Display {
public:
    Display() {
        // Defaults match the prebuilt's own: 30 fps if nobody said otherwise, and a phone-ish
        // density. Width/height have no sane default — a display HAL that invents a size hides a
        // misconfigured container behind a picture that merely looks wrong — but returning zero
        // makes SurfaceFlinger abort, so fall back to a plain 720p and
        // say so loudly.
        width_ = propInt("ro.boot.remora_width", 0);
        height_ = propInt("ro.boot.remora_height", 0);
        if (width_ <= 0 || height_ <= 0) {
            ALOGE("no display size in ro.boot.remora_width/height — "
                  "defaulting to 1280x720; the container was started without a resolution");
            width_ = 1280;
            height_ = 720;
        }
        fps_ = propInt("ro.boot.remora_fps", 30);
        if (fps_ <= 0 || fps_ > 240) {
            ALOGE("implausible fps %d — using 30", fps_);
            fps_ = 30;
        }
        density_ = propInt("ro.sf.lcd_density", 320);
        if (density_ <= 0) density_ = 320;
        ALOGI("remora hwcomposer: %dx%d @ %d fps, density %d", width_, height_, fps_, density_);
    }

    ~Display() { stopVsync(); }

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    int32_t fps() const { return fps_; }
    int32_t density() const { return density_; }
    int64_t vsyncPeriodNs() const { return 1000000000LL / fps_; }

    hwc2_layer_t createLayer() {
        std::lock_guard<std::mutex> lk(mutex_);
        const hwc2_layer_t id = nextLayer_++;
        layers_[id] = Layer{};
        return id;
    }

    bool destroyLayer(hwc2_layer_t id) {
        std::lock_guard<std::mutex> lk(mutex_);
        return layers_.erase(id) > 0;
    }

    bool hasLayer(hwc2_layer_t id) {
        std::lock_guard<std::mutex> lk(mutex_);
        return layers_.count(id) > 0;
    }

    void setLayerType(hwc2_layer_t id, int32_t type) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = layers_.find(id);
        if (it != layers_.end()) it->second.compositionType = type;
    }

    // Everything becomes CLIENT. The changed set is only those layers that were not already
    // CLIENT, because SurfaceFlinger validates the count it gets here against what
    // getChangedCompositionTypes then returns, and disagreeing is a protocol error rather than a
    // cosmetic one.
    void validate(std::vector<hwc2_layer_t> *changedLayers) {
        std::lock_guard<std::mutex> lk(mutex_);
        pendingChanges_.clear();
        for (const auto &kv : layers_)
            if (kv.second.compositionType != HWC2_COMPOSITION_CLIENT)
                pendingChanges_.push_back(kv.first);
        if (changedLayers) *changedLayers = pendingChanges_;
    }

    std::vector<hwc2_layer_t> pendingChanges() {
        std::lock_guard<std::mutex> lk(mutex_);
        return pendingChanges_;
    }

    void acceptChanges() {
        std::lock_guard<std::mutex> lk(mutex_);
        for (hwc2_layer_t id : pendingChanges_) {
            auto it = layers_.find(id);
            if (it != layers_.end()) it->second.compositionType = HWC2_COMPOSITION_CLIENT;
        }
        pendingChanges_.clear();
    }

    // The client target's acquire fence is the one fence that matters here: it signals when the
    // GPU has finished composing the frame we are being handed. The prebuilt waits on it too
    // ("hwcomposer waited on fence %d for %d ms" is its own log line). We take ownership of the fd.
    // The HANDLE is kept too, for frame export (bd remora-emoe phase 2). Only borrowed for the
    // duration of present(): SurfaceFlinger owns it, and the export path dups the dma_buf fd it
    // needs rather than retaining the handle past the call.
    void setClientTarget(buffer_handle_t target, int32_t acquireFence) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (clientTargetFence_ >= 0) close(clientTargetFence_);
        clientTargetFence_ = acquireFence;
        clientTarget_ = target;
    }

    // Returns the fd to hand back as the PRESENT FENCE, or -1 when there is none to give.
    // Ownership transfers to the caller, which passes it to SurfaceFlinger; SF closes it.
    //
    // WHY WE RETURN A FENCE AT ALL (bd remora-o0mb). hwcomposer2.h is unconditional: "If this call
    // succeeds, outPresentFence will be populated with a file descriptor referring to a present
    // sync fence object." There is no -1 case. SurfaceFlinger holds us to it — Fence(-1) reports
    // SIGNAL_TIME_INVALID, and PresentLatencyTracker::trackPendingFrame logs "Invalid present
    // fence" ONCE PER PRESENTED FRAME, which is where the AMD hosts' log volume was coming from.
    //
    // The fence we return is the CLIENT TARGET ACQUIRE FENCE, and the reason that is sound is the
    // sync_wait below: by the time we hand it back it has ALREADY SIGNALLED, so it can never wedge
    // the compositor waiting on us — which is the risk the old -1 was chosen to avoid, and the one
    // constraint this change must not trade away. It is also the honest answer to what the spec
    // asks for: composition completing is the moment this frame "starts to appear", because
    // publishing to the export path is synchronous and happens immediately after.
    int present() {
        int fence;
        buffer_handle_t target;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            fence = clientTargetFence_;
            clientTargetFence_ = -1;
            target = clientTarget_;
        }
        // Export AFTER the wait below, never before: the acquire fence is what says the GPU has
        // finished writing this buffer, so publishing early would hand the encoder a half-drawn
        // frame. A frame with no fence is already complete and can go straight out.
        if (fence < 0) {
            remora::frameExportPublish(target, width_, height_);
            return -1;  // nothing to give; SF logs for this frame, and that is the honest report
        }
        // A bounded wait. An unbounded one turns a wedged GPU into a wedged SurfaceFlinger, and
        // the whole device stops rather than the frame; 1000 ms is far beyond any healthy frame
        // and short enough to keep the compositor alive to log it.
        const auto start = std::chrono::steady_clock::now();
        const int rc = sync_wait(fence, 1000);
        if (rc < 0) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start).count();
            ALOGW("client target fence %d did not signal within %lld ms", fence,
                  static_cast<long long>(ms));
            // NOT handed back. This is the one path where the fence is NOT known to be signalled,
            // and an unsignalled present fence is precisely what stalls the compositor. A wedged
            // GPU should cost us a log line, never the device.
            close(fence);
            remora::frameExportPublish(target, width_, height_);
            return -1;
        }
        remora::frameExportPublish(target, width_, height_);
        return fence;
    }

    void setVsyncCallback(hwc2_callback_data_t data, HWC2_PFN_VSYNC fn) {
        std::lock_guard<std::mutex> lk(cbMutex_);
        vsyncData_ = data;
        vsyncFn_ = fn;
    }

    void setVsyncEnabled(bool enabled) {
        if (enabled == vsyncEnabled_) return;
        vsyncEnabled_ = enabled;
        if (enabled)
            startVsync();
        else
            stopVsync();
    }

private:
    void startVsync() {
        stopVsync();
        vsyncRunning_ = true;
        vsyncThread_ = std::thread([this] { vsyncLoop(); });
    }

    void stopVsync() {
        {
            std::lock_guard<std::mutex> lk(vsyncMutex_);
            vsyncRunning_ = false;
        }
        vsyncCv_.notify_all();
        if (vsyncThread_.joinable()) vsyncThread_.join();
    }

    // Paced against a monotonic deadline rather than by sleeping for one period each time: the
    // latter accumulates every scheduling delay, so the reported rate drifts below the configured
    // one and SurfaceFlinger's own vsync model — which predicts future vsyncs from these
    // timestamps — spends its life resynchronising.
    void vsyncLoop() {
        const auto period = std::chrono::nanoseconds(vsyncPeriodNs());
        auto next = std::chrono::steady_clock::now() + period;
        int64_t sent = 0;
        auto reportAt = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (true) {
            std::unique_lock<std::mutex> lk(vsyncMutex_);
            if (vsyncCv_.wait_until(lk, next, [this] { return !vsyncRunning_; })) return;
            lk.unlock();

            const auto now = std::chrono::steady_clock::now();
            next += period;
            // A long stall (a suspended container, a stopped host) leaves the deadline far in the
            // past; catching up would fire a burst of vsyncs with backdated timestamps, which is
            // worse than the gap. Re-base instead.
            if (next < now) next = now + period;

            HWC2_PFN_VSYNC fn = nullptr;
            hwc2_callback_data_t data = nullptr;
            {
                std::lock_guard<std::mutex> cb(cbMutex_);
                fn = vsyncFn_;
                data = vsyncData_;
            }
            if (fn) {
                const int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       now.time_since_epoch()).count();
                fn(data, kPrimaryDisplay, ts);
                ++sent;
            }
            if (now >= reportAt) {
                ALOGI("hw_composer sent %" PRId64 " syncs in 60s", sent);
                sent = 0;
                reportAt = now + std::chrono::seconds(60);
            }
        }
    }

    int32_t width_ = 0, height_ = 0, fps_ = 30, density_ = 320;

    std::mutex mutex_;
    std::unordered_map<hwc2_layer_t, Layer> layers_;
    std::vector<hwc2_layer_t> pendingChanges_;
    hwc2_layer_t nextLayer_ = 1;
    int clientTargetFence_ = -1;
    // BORROWED, never owned: SurfaceFlinger's client target handle, valid only for the present()
    // that follows. The export path dups the dma_buf fd it needs, so nothing here outlives the call
    // (bd remora-emoe phase 2).
    buffer_handle_t clientTarget_ = nullptr;

    std::mutex cbMutex_;
    hwc2_callback_data_t vsyncData_ = nullptr;
    HWC2_PFN_VSYNC vsyncFn_ = nullptr;

    bool vsyncEnabled_ = false;
    bool vsyncRunning_ = false;
    std::thread vsyncThread_;
    std::mutex vsyncMutex_;
    std::condition_variable vsyncCv_;
};

struct RemoraDevice {
    hwc2_device_t base;
    Display display;
};

RemoraDevice *self(hwc2_device_t *d) { return reinterpret_cast<RemoraDevice *>(d); }

// ---------------------------------------------------------------------------------------------
// HWC2 entry points. The set implemented here is what the composer@2.1 passthrough service asks
// getFunction for; the service treats a null pointer for any of its mandatory descriptors as a
// fatal error at load, so a missing one costs the display rather than the feature.
// ---------------------------------------------------------------------------------------------

int32_t acceptDisplayChanges(hwc2_device_t *dev, hwc2_display_t display) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    self(dev)->display.acceptChanges();
    return HWC2_ERROR_NONE;
}

int32_t createLayer(hwc2_device_t *dev, hwc2_display_t display, hwc2_layer_t *outLayer) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    *outLayer = self(dev)->display.createLayer();
    return HWC2_ERROR_NONE;
}

int32_t destroyLayer(hwc2_device_t *dev, hwc2_display_t display, hwc2_layer_t layer) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    return self(dev)->display.destroyLayer(layer) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_LAYER;
}

// No virtual display support, exactly as the prebuilt: it advertises one display and nothing else.
int32_t createVirtualDisplay(hwc2_device_t *, uint32_t, uint32_t, int32_t *, hwc2_display_t *) {
    return HWC2_ERROR_NO_RESOURCES;
}

int32_t destroyVirtualDisplay(hwc2_device_t *, hwc2_display_t) { return HWC2_ERROR_BAD_DISPLAY; }

uint32_t getMaxVirtualDisplayCount(hwc2_device_t *) { return 0; }

void dump(hwc2_device_t *dev, uint32_t *outSize, char *outBuffer) {
    const Display &d = self(dev)->display;
    char buf[256];
    const int n = snprintf(buf, sizeof(buf),
                           "Remora hwcomposer: %dx%d @%d fps, density %d, all layers CLIENT\n",
                           d.width(), d.height(), d.fps(), d.density());
    if (!outBuffer) {
        *outSize = static_cast<uint32_t>(n);
        return;
    }
    const uint32_t copy = std::min<uint32_t>(*outSize, static_cast<uint32_t>(n));
    memcpy(outBuffer, buf, copy);
    *outSize = copy;
}

int32_t getActiveConfig(hwc2_device_t *, hwc2_display_t display, hwc2_config_t *outConfig) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    *outConfig = kPrimaryConfig;
    return HWC2_ERROR_NONE;
}

int32_t setActiveConfig(hwc2_device_t *, hwc2_display_t display, hwc2_config_t config) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    return config == kPrimaryConfig ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_CONFIG;
}

int32_t getDisplayConfigs(hwc2_device_t *, hwc2_display_t display, uint32_t *outNumConfigs,
                          hwc2_config_t *outConfigs) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    if (!outConfigs) {
        *outNumConfigs = 1;
        return HWC2_ERROR_NONE;
    }
    if (*outNumConfigs >= 1) outConfigs[0] = kPrimaryConfig;
    *outNumConfigs = 1;
    return HWC2_ERROR_NONE;
}

int32_t getDisplayAttribute(hwc2_device_t *dev, hwc2_display_t display, hwc2_config_t config,
                            int32_t attribute, int32_t *outValue) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    if (config != kPrimaryConfig) return HWC2_ERROR_BAD_CONFIG;
    const Display &d = self(dev)->display;
    switch (attribute) {
        case HWC2_ATTRIBUTE_WIDTH: *outValue = d.width(); return HWC2_ERROR_NONE;
        case HWC2_ATTRIBUTE_HEIGHT: *outValue = d.height(); return HWC2_ERROR_NONE;
        case HWC2_ATTRIBUTE_VSYNC_PERIOD:
            *outValue = static_cast<int32_t>(d.vsyncPeriodNs());
            return HWC2_ERROR_NONE;
        // DPI is in THOUSANDTHS of an inch. Returning the raw density here is a classic off-by-1000
        // that makes every app lay out as if on a giant screen.
        case HWC2_ATTRIBUTE_DPI_X:
        case HWC2_ATTRIBUTE_DPI_Y: *outValue = d.density() * 1000; return HWC2_ERROR_NONE;
        default:
            // The prebuilt logs "unknown display attribute %u" here and so do we — it is a normal
            // probe for attributes a newer framework knows about, not an error.
            ALOGV("unknown display attribute %d", attribute);
            *outValue = -1;
            return HWC2_ERROR_BAD_PARAMETER;
    }
}

int32_t getDisplayName(hwc2_device_t *, hwc2_display_t display, uint32_t *outSize,
                       char *outName) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    static const char kName[] = "Remora Display";
    const uint32_t len = sizeof(kName) - 1;
    if (!outName) {
        *outSize = len;
        return HWC2_ERROR_NONE;
    }
    const uint32_t copy = std::min(*outSize, len);
    memcpy(outName, kName, copy);
    *outSize = copy;
    return HWC2_ERROR_NONE;
}

int32_t getDisplayType(hwc2_device_t *, hwc2_display_t display, int32_t *outType) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    *outType = HWC2_DISPLAY_TYPE_PHYSICAL;
    return HWC2_ERROR_NONE;
}

int32_t getDozeSupport(hwc2_device_t *, hwc2_display_t display, int32_t *outSupport) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    *outSupport = 0;
    return HWC2_ERROR_NONE;
}

int32_t getHdrCapabilities(hwc2_device_t *, hwc2_display_t display, uint32_t *outNumTypes,
                           int32_t *, float *, float *, float *) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    *outNumTypes = 0;
    return HWC2_ERROR_NONE;
}

int32_t getColorModes(hwc2_device_t *, hwc2_display_t display, uint32_t *outNumModes,
                      int32_t *outModes) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    if (!outModes) {
        *outNumModes = 1;
        return HWC2_ERROR_NONE;
    }
    if (*outNumModes >= 1) outModes[0] = HAL_COLOR_MODE_NATIVE;
    *outNumModes = 1;
    return HWC2_ERROR_NONE;
}

int32_t setColorMode(hwc2_device_t *, hwc2_display_t display, int32_t mode) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    return mode == HAL_COLOR_MODE_NATIVE ? HWC2_ERROR_NONE : HWC2_ERROR_UNSUPPORTED;
}

// Accepted and ignored on purpose. We advertise no SKIP_CLIENT_COLOR_TRANSFORM capability, so
// SurfaceFlinger applies the transform itself while composing the client target — by the time the
// frame reaches us it is already in it.
int32_t setColorTransform(hwc2_device_t *, hwc2_display_t display, const float *, int32_t) {
    return display == kPrimaryDisplay ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}

int32_t getClientTargetSupport(hwc2_device_t *dev, hwc2_display_t display, uint32_t width,
                               uint32_t height, int32_t /*format*/, int32_t /*dataspace*/) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    const Display &d = self(dev)->display;
    return (width == static_cast<uint32_t>(d.width()) &&
            height == static_cast<uint32_t>(d.height()))
               ? HWC2_ERROR_NONE
               : HWC2_ERROR_UNSUPPORTED;
}

int32_t setClientTarget(hwc2_device_t *dev, hwc2_display_t display, buffer_handle_t target,
                        int32_t acquireFence, int32_t /*dataspace*/, hwc_region_t /*damage*/) {
    if (display != kPrimaryDisplay) {
        if (acquireFence >= 0) close(acquireFence);
        return HWC2_ERROR_BAD_DISPLAY;
    }
    self(dev)->display.setClientTarget(target, acquireFence);
    return HWC2_ERROR_NONE;
}

int32_t getChangedCompositionTypes(hwc2_device_t *dev, hwc2_display_t display,
                                   uint32_t *outNumElements, hwc2_layer_t *outLayers,
                                   int32_t *outTypes) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    const std::vector<hwc2_layer_t> changed = self(dev)->display.pendingChanges();
    if (!outLayers || !outTypes) {
        *outNumElements = static_cast<uint32_t>(changed.size());
        return HWC2_ERROR_NONE;
    }
    const uint32_t n = std::min<uint32_t>(*outNumElements, changed.size());
    for (uint32_t i = 0; i < n; ++i) {
        outLayers[i] = changed[i];
        outTypes[i] = HWC2_COMPOSITION_CLIENT;
    }
    *outNumElements = n;
    return HWC2_ERROR_NONE;
}

int32_t getDisplayRequests(hwc2_device_t *, hwc2_display_t display, int32_t *outDisplayRequests,
                           uint32_t *outNumElements, hwc2_layer_t *, int32_t *) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    if (outDisplayRequests) *outDisplayRequests = 0;
    *outNumElements = 0;
    return HWC2_ERROR_NONE;
}

int32_t validateDisplay(hwc2_device_t *dev, hwc2_display_t display, uint32_t *outNumTypes,
                        uint32_t *outNumRequests) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    std::vector<hwc2_layer_t> changed;
    self(dev)->display.validate(&changed);
    *outNumTypes = static_cast<uint32_t>(changed.size());
    *outNumRequests = 0;
    // HAS_CHANGES, not NONE, whenever anything moved to CLIENT: returning NONE tells
    // SurfaceFlinger it may present without calling acceptDisplayChanges, and the two sides then
    // disagree about every layer's composition type.
    return changed.empty() ? HWC2_ERROR_NONE : HWC2_ERROR_HAS_CHANGES;
}

int32_t presentDisplay(hwc2_device_t *dev, hwc2_display_t display, int32_t *outPresentFence) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    // The already-signalled client target acquire fence, or -1 when there is none to give. See
    // Display::present() for why handing one back is both required and safe (bd remora-o0mb).
    *outPresentFence = self(dev)->display.present();
    return HWC2_ERROR_NONE;
}

int32_t getReleaseFences(hwc2_device_t *, hwc2_display_t display, uint32_t *outNumElements,
                         hwc2_layer_t *, int32_t *) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    // None: we never hold a layer buffer past present, because we never read one.
    *outNumElements = 0;
    return HWC2_ERROR_NONE;
}

int32_t setOutputBuffer(hwc2_device_t *, hwc2_display_t, buffer_handle_t, int32_t releaseFence) {
    if (releaseFence >= 0) close(releaseFence);
    return HWC2_ERROR_UNSUPPORTED;  // virtual displays only, and we have none
}

int32_t setPowerMode(hwc2_device_t *, hwc2_display_t display, int32_t mode) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    switch (mode) {
        case HWC2_POWER_MODE_OFF:
        case HWC2_POWER_MODE_ON: return HWC2_ERROR_NONE;
        default: return HWC2_ERROR_UNSUPPORTED;  // no doze, as getDozeSupport says
    }
}

int32_t setVsyncEnabled(hwc2_device_t *dev, hwc2_display_t display, int32_t enabled) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    if (enabled != HWC2_VSYNC_ENABLE && enabled != HWC2_VSYNC_DISABLE)
        return HWC2_ERROR_BAD_PARAMETER;
    self(dev)->display.setVsyncEnabled(enabled == HWC2_VSYNC_ENABLE);
    return HWC2_ERROR_NONE;
}

// Layer state. All of it is accepted and none of it is acted on: every layer is CLIENT, so
// SurfaceFlinger has already applied blending, cropping, transform and z-order itself while
// producing the client target. Accepting is not the same as ignoring — refusing any of these
// would make SurfaceFlinger treat the layer as unsupported and thrash trying to renegotiate.
int32_t setLayerBuffer(hwc2_device_t *dev, hwc2_display_t display, hwc2_layer_t layer,
                       buffer_handle_t, int32_t acquireFence) {
    // The fence is ours once we are handed it, and we never read the buffer, so close it now
    // rather than leaking an fd per layer per frame — which at 60 fps exhausts the process's fd
    // table in minutes.
    if (acquireFence >= 0) close(acquireFence);
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    return self(dev)->display.hasLayer(layer) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_LAYER;
}

int32_t setLayerCompositionType(hwc2_device_t *dev, hwc2_display_t display, hwc2_layer_t layer,
                                int32_t type) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    if (!self(dev)->display.hasLayer(layer)) return HWC2_ERROR_BAD_LAYER;
    self(dev)->display.setLayerType(layer, type);
    return HWC2_ERROR_NONE;
}

template <typename... Args>
int32_t layerAccept(hwc2_device_t *dev, hwc2_display_t display, hwc2_layer_t layer, Args...) {
    if (display != kPrimaryDisplay) return HWC2_ERROR_BAD_DISPLAY;
    return self(dev)->display.hasLayer(layer) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_LAYER;
}

int32_t setLayerSidebandStream(hwc2_device_t *, hwc2_display_t, hwc2_layer_t,
                               const native_handle_t *) {
    // We advertise no SIDEBAND_STREAM capability, so this should never be called; say so rather
    // than pretending it worked.
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t setCursorPosition(hwc2_device_t *, hwc2_display_t display, hwc2_layer_t, int32_t,
                          int32_t) {
    // No CURSOR composition is ever handed out by validateDisplay, so this is a no-op that must
    // still succeed for the layers SurfaceFlinger speculatively marks.
    return display == kPrimaryDisplay ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}

void registerCallback(hwc2_device_t *dev, int32_t descriptor, hwc2_callback_data_t callbackData,
                      hwc2_function_pointer_t pointer) {
    RemoraDevice *d = self(dev);
    switch (descriptor) {
        case HWC2_CALLBACK_HOTPLUG: {
            auto fn = reinterpret_cast<HWC2_PFN_HOTPLUG>(pointer);
            // Report the primary display the moment SurfaceFlinger asks to hear about hotplug.
            // There is nothing to wait for — the display is a property set, it exists at load —
            // and SurfaceFlinger blocks on the primary appearing before it will start.
            if (fn) fn(callbackData, kPrimaryDisplay, HWC2_CONNECTION_CONNECTED);
            break;
        }
        case HWC2_CALLBACK_VSYNC:
            d->display.setVsyncCallback(callbackData, reinterpret_cast<HWC2_PFN_VSYNC>(pointer));
            break;
        case HWC2_CALLBACK_REFRESH:
            // Never invoked: we have no reason to ask for a redraw, since we do not composite and
            // cannot notice content going stale.
            break;
        default: ALOGV("unknown callback descriptor %d", descriptor); break;
    }
}

void getCapabilities(hwc2_device_t *, uint32_t *outCount, int32_t *) {
    *outCount = 0;  // no SIDEBAND_STREAM, no SKIP_CLIENT_COLOR_TRANSFORM
}

hwc2_function_pointer_t getFunction(hwc2_device_t *, int32_t descriptor) {
    switch (descriptor) {
        case HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES:
            return reinterpret_cast<hwc2_function_pointer_t>(acceptDisplayChanges);
        case HWC2_FUNCTION_CREATE_LAYER:
            return reinterpret_cast<hwc2_function_pointer_t>(createLayer);
        case HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(createVirtualDisplay);
        case HWC2_FUNCTION_DESTROY_LAYER:
            return reinterpret_cast<hwc2_function_pointer_t>(destroyLayer);
        case HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(destroyVirtualDisplay);
        case HWC2_FUNCTION_DUMP: return reinterpret_cast<hwc2_function_pointer_t>(dump);
        case HWC2_FUNCTION_GET_ACTIVE_CONFIG:
            return reinterpret_cast<hwc2_function_pointer_t>(getActiveConfig);
        case HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES:
            return reinterpret_cast<hwc2_function_pointer_t>(getChangedCompositionTypes);
        case HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT:
            return reinterpret_cast<hwc2_function_pointer_t>(getClientTargetSupport);
        case HWC2_FUNCTION_GET_COLOR_MODES:
            return reinterpret_cast<hwc2_function_pointer_t>(getColorModes);
        case HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE:
            return reinterpret_cast<hwc2_function_pointer_t>(getDisplayAttribute);
        case HWC2_FUNCTION_GET_DISPLAY_CONFIGS:
            return reinterpret_cast<hwc2_function_pointer_t>(getDisplayConfigs);
        case HWC2_FUNCTION_GET_DISPLAY_NAME:
            return reinterpret_cast<hwc2_function_pointer_t>(getDisplayName);
        case HWC2_FUNCTION_GET_DISPLAY_REQUESTS:
            return reinterpret_cast<hwc2_function_pointer_t>(getDisplayRequests);
        case HWC2_FUNCTION_GET_DISPLAY_TYPE:
            return reinterpret_cast<hwc2_function_pointer_t>(getDisplayType);
        case HWC2_FUNCTION_GET_DOZE_SUPPORT:
            return reinterpret_cast<hwc2_function_pointer_t>(getDozeSupport);
        case HWC2_FUNCTION_GET_HDR_CAPABILITIES:
            return reinterpret_cast<hwc2_function_pointer_t>(getHdrCapabilities);
        case HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT:
            return reinterpret_cast<hwc2_function_pointer_t>(getMaxVirtualDisplayCount);
        case HWC2_FUNCTION_GET_RELEASE_FENCES:
            return reinterpret_cast<hwc2_function_pointer_t>(getReleaseFences);
        case HWC2_FUNCTION_PRESENT_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(presentDisplay);
        case HWC2_FUNCTION_REGISTER_CALLBACK:
            return reinterpret_cast<hwc2_function_pointer_t>(registerCallback);
        case HWC2_FUNCTION_SET_ACTIVE_CONFIG:
            return reinterpret_cast<hwc2_function_pointer_t>(setActiveConfig);
        case HWC2_FUNCTION_SET_CLIENT_TARGET:
            return reinterpret_cast<hwc2_function_pointer_t>(setClientTarget);
        case HWC2_FUNCTION_SET_COLOR_MODE:
            return reinterpret_cast<hwc2_function_pointer_t>(setColorMode);
        case HWC2_FUNCTION_SET_COLOR_TRANSFORM:
            return reinterpret_cast<hwc2_function_pointer_t>(setColorTransform);
        case HWC2_FUNCTION_SET_CURSOR_POSITION:
            return reinterpret_cast<hwc2_function_pointer_t>(setCursorPosition);
        case HWC2_FUNCTION_SET_LAYER_BLEND_MODE:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<int32_t>);
        case HWC2_FUNCTION_SET_LAYER_BUFFER:
            return reinterpret_cast<hwc2_function_pointer_t>(setLayerBuffer);
        case HWC2_FUNCTION_SET_LAYER_COLOR:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<hwc_color_t>);
        case HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE:
            return reinterpret_cast<hwc2_function_pointer_t>(setLayerCompositionType);
        case HWC2_FUNCTION_SET_LAYER_DATASPACE:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<int32_t>);
        case HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<hwc_rect_t>);
        case HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<float>);
        case HWC2_FUNCTION_SET_LAYER_SIDEBAND_STREAM:
            return reinterpret_cast<hwc2_function_pointer_t>(setLayerSidebandStream);
        case HWC2_FUNCTION_SET_LAYER_SOURCE_CROP:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<hwc_frect_t>);
        case HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<hwc_region_t>);
        case HWC2_FUNCTION_SET_LAYER_TRANSFORM:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<int32_t>);
        case HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<hwc_region_t>);
        case HWC2_FUNCTION_SET_LAYER_Z_ORDER:
            return reinterpret_cast<hwc2_function_pointer_t>(layerAccept<uint32_t>);
        case HWC2_FUNCTION_SET_OUTPUT_BUFFER:
            return reinterpret_cast<hwc2_function_pointer_t>(setOutputBuffer);
        case HWC2_FUNCTION_SET_POWER_MODE:
            return reinterpret_cast<hwc2_function_pointer_t>(setPowerMode);
        case HWC2_FUNCTION_SET_VSYNC_ENABLED:
            return reinterpret_cast<hwc2_function_pointer_t>(setVsyncEnabled);
        case HWC2_FUNCTION_VALIDATE_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(validateDisplay);
        default:
            // Everything else is a 2.2+/2.3+ descriptor this HAL does not implement. Returning
            // null is the documented way to say so, and the service falls back accordingly.
            return nullptr;
    }
}

int closeDevice(hw_device_t *device) {
    delete reinterpret_cast<RemoraDevice *>(device);
    return 0;
}

int openDevice(const hw_module_t *module, const char *name, hw_device_t **device) {
    if (!name || strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
        ALOGE("unknown device name '%s'", name ? name : "(null)");
        return -EINVAL;
    }
    auto *dev = new RemoraDevice();
    dev->base.common.tag = HARDWARE_DEVICE_TAG;
    dev->base.common.version = HWC_DEVICE_API_VERSION_2_0;
    dev->base.common.module = const_cast<hw_module_t *>(module);
    dev->base.common.close = closeDevice;
    dev->base.getCapabilities = getCapabilities;
    dev->base.getFunction = getFunction;
    // Arm frame export (bd remora-emoe phase 2). A no-op unless the host named a
    // socket, so an image carrying this behaves exactly as before wherever compose
    // mode is not in use.
    remora::frameExportInit();
    *device = &dev->base.common;
    ALOGI("remora hwcomposer opened (HWC2 passthrough, client composition only)");
    return 0;
}

hw_module_methods_t kModuleMethods = {.open = openDevice};

}  // namespace

// The one exported symbol, exactly as the prebuilt exports exactly one. hw_get_module resolves
// this library from ro.hardware.hwcomposer (gpu_config.sh sets it), so the installed name
// hwcomposer.remora.so and that property must agree.
hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HWC_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "Remora hwcomposer",
    .author = "Remora",
    .methods = &kModuleMethods,
};
