// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiDevice"

#include <c2_va/vaapi/VaapiDevice.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <vector>

#include <log/log.h>
#include <va/va_drm.h>

namespace android {
namespace {

// DRM render nodes probed in order. renderD128 is usually the primary/host GPU;
// on the KVM guest the Intel xe SR-IOV VF decode engine is exposed on
// renderD129.
constexpr const char* kRenderNodes[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
};

// LIBVA_DRIVER_NAME has to be set HERE, in the process, and not by the service .rc.
//
// The .rc used to carry `setenv LIBVA_DRIVER_NAME ${ro.boot.va_driver:-iHD}`, and Android init
// DOES NOT EXPAND ${prop:-default} in a service setenv — the HAL received that 26-character string
// verbatim, libva dutifully tried to dlopen "${ro.boot.va_driver:-iHD}_drv_video.so", and every
// VAAPI component died at vaInitialize. Measured with the property both unset AND set, so it is not
// a missing-property problem: init simply never expands it (bd remora-e5x.2).
//
// The name is ALSO LOAD-BEARING AS A NODE FILTER, which is why the fix is not "let libva
// auto-detect". kRenderNodes is probed in order and the FIRST node that initializes wins. On a
// hybrid NVIDIA+Intel host renderD128 is NVIDIA, whose VA driver is NVDEC — decode-only, with zero
// VAEntrypointEnc* — so an auto-detecting probe would bind there and silently lose hardware encode
// (the scrcpy mirror). Naming iHD makes vaInitialize FAIL on the NVIDIA node and fall through to
// the Intel one, which has the encode entrypoints. Keep that behaviour deliberate, not accidental.
//
// androidboot.va_driver -> ro.boot.va_driver is the per-host override (Remora's va_driver= key);
// iHD is the default because it is the only VA driver this image ships.
constexpr const char* kDefaultVaDriver = "iHD";

void applyVaDriverFromProperty() {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get("ro.boot.va_driver", value);
    const char* driver = (len > 0) ? value : kDefaultVaDriver;
    // Overwrite unconditionally: whatever init left in the environment is either the unexpanded
    // literal above or a stale value, and neither should win over the property.
    setenv("LIBVA_DRIVER_NAME", driver, 1);
    ALOGI("LIBVA_DRIVER_NAME=%s (%s)", driver,
          (len > 0) ? "from ro.boot.va_driver" : "default; ro.boot.va_driver unset");
}

}  // namespace

// static
std::unique_ptr<VaapiDevice> VaapiDevice::Create() {
    applyVaDriverFromProperty();

    for (const char* path : kRenderNodes) {
        std::unique_ptr<VaapiDevice> device = tryCreateOnNode(path);
        if (device) {
            ALOGI("Initialized VAAPI on %s", path);
            return device;
        }
    }

    ALOGW("No usable DRM render node found for VAAPI; self-disabling.");
    return nullptr;
}

// static
std::unique_ptr<VaapiDevice> VaapiDevice::tryCreateOnNode(const char* path) {
    int drmFd = open(path, O_RDWR | O_CLOEXEC);
    if (drmFd < 0) {
        ALOGV("Failed to open %s: %s", path, strerror(errno));
        return nullptr;
    }

    VADisplay display = vaGetDisplayDRM(drmFd);
    if (!vaDisplayIsValid(display)) {
        ALOGV("vaGetDisplayDRM() failed for %s", path);
        close(drmFd);
        return nullptr;
    }

    int major = 0;
    int minor = 0;
    VAStatus status = vaInitialize(display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        ALOGV("vaInitialize() failed for %s: %s", path, vaErrorStr(status));
        // vaGetDisplayDRM() may have taken a reference on the fd; terminate the
        // (partially created) display before closing to avoid leaks.
        vaTerminate(display);
        close(drmFd);
        return nullptr;
    }

    ALOGI("vaInitialize() OK on %s (VA-API %d.%d)", path, major, minor);
    return std::unique_ptr<VaapiDevice>(new VaapiDevice(drmFd, display, std::string(path)));
}

VaapiDevice::VaapiDevice(int drmFd, VADisplay display, std::string renderNodePath)
      : mDrmFd(drmFd), mDisplay(display), mRenderNodePath(std::move(renderNodePath)) {}

VaapiDevice::~VaapiDevice() {
    if (mDisplay) {
        vaTerminate(mDisplay);
        mDisplay = nullptr;
    }
    if (mDrmFd >= 0) {
        close(mDrmFd);
        mDrmFd = -1;
    }
}

bool VaapiDevice::supportsProfileVLD(VAProfile profile) const {
    if (!mDisplay) return false;

    // Enumerate supported profiles.
    int maxProfiles = vaMaxNumProfiles(mDisplay);
    if (maxProfiles <= 0) return false;

    std::vector<VAProfile> profiles(maxProfiles);
    int numProfiles = 0;
    VAStatus status = vaQueryConfigProfiles(mDisplay, profiles.data(), &numProfiles);
    if (status != VA_STATUS_SUCCESS) {
        ALOGW("vaQueryConfigProfiles() failed: %s", vaErrorStr(status));
        return false;
    }

    bool found = false;
    for (int i = 0; i < numProfiles; ++i) {
        if (profiles[i] == profile) {
            found = true;
            break;
        }
    }
    if (!found) {
        ALOGV("Profile %d not advertised by driver.", static_cast<int>(profile));
        return false;
    }

    // Enumerate entrypoints for that profile and look for VLD (decode).
    int maxEntrypoints = vaMaxNumEntrypoints(mDisplay);
    if (maxEntrypoints <= 0) return false;

    std::vector<VAEntrypoint> entrypoints(maxEntrypoints);
    int numEntrypoints = 0;
    status = vaQueryConfigEntrypoints(mDisplay, profile, entrypoints.data(), &numEntrypoints);
    if (status != VA_STATUS_SUCCESS) {
        ALOGW("vaQueryConfigEntrypoints() failed for profile %d: %s", static_cast<int>(profile),
              vaErrorStr(status));
        return false;
    }

    for (int i = 0; i < numEntrypoints; ++i) {
        if (entrypoints[i] == VAEntrypointVLD) {
            return true;
        }
    }

    ALOGV("Profile %d has no VLD entrypoint.", static_cast<int>(profile));
    return false;
}

}  // namespace android
