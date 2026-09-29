// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_VAAPI_VAAPI_DEVICE_H
#define ANDROID_C2_VA_VAAPI_VAAPI_DEVICE_H

#include <memory>
#include <string>

#include <va/va.h>

namespace android {

// RAII wrapper around a libva VADisplay obtained from a DRM render node.
//
// VaapiDevice::Create() probes /dev/dri/renderD128 then /dev/dri/renderD129,
// opens the node, derives a VADisplay via vaGetDisplayDRM() and calls
// vaInitialize(). If no render node yields a usable, initialized VADisplay
// (e.g. bare/SwiftShader hosts with no VF GPU), Create() returns nullptr so the
// caller can self-disable instead of crash-looping.
class VaapiDevice {
public:
    // Probe the DRM render nodes and return an initialized device, or nullptr.
    static std::unique_ptr<VaapiDevice> Create();

    ~VaapiDevice();

    VaapiDevice(const VaapiDevice&) = delete;
    VaapiDevice& operator=(const VaapiDevice&) = delete;

    // The initialized VA display. Valid for the lifetime of this object.
    VADisplay display() const { return mDisplay; }

    // The render node that backed this display, e.g. "/dev/dri/renderD129".
    const std::string& renderNodePath() const { return mRenderNodePath; }

    // Returns true if |profile| supports the VLD (decode) entrypoint on this
    // device, as reported by vaQueryConfigProfiles() / vaQueryConfigEntrypoints().
    bool supportsProfileVLD(VAProfile profile) const;

private:
    VaapiDevice(int drmFd, VADisplay display, std::string renderNodePath);

    // Attempt to open |path|, create + initialize a VADisplay on it. On success
    // returns a populated VaapiDevice; on failure closes the fd and returns null.
    static std::unique_ptr<VaapiDevice> tryCreateOnNode(const char* path);

    int mDrmFd = -1;
    VADisplay mDisplay = nullptr;
    std::string mRenderNodePath;
};

}  // namespace android

#endif  // ANDROID_C2_VA_VAAPI_VAAPI_DEVICE_H
