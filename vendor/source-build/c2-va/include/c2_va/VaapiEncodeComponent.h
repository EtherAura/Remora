// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_VAAPI_ENCODE_COMPONENT_H
#define ANDROID_C2_VA_VAAPI_ENCODE_COMPONENT_H

#include <atomic>

#include <v4l2_codec2/components/EncodeComponent.h>

namespace android {

// The VAAPI-backed C2 encode component. It reuses the whole EncodeComponent
// machinery (queueing, output block pool, drain/flush, work reporting) from
// libv4l2_codec2_components and only overrides initializeEncoder() to plug in
// the VAAPI encoder core (VaapiVideoEncoder) instead of the V4L2 one. Mirrors
// V4L2EncodeComponent; the only pure-virtual EncodeComponent exposes is
// initializeEncoder(), so startTask() and the rest are inherited unchanged.
class VaapiEncodeComponent : public EncodeComponent {
public:
    static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               std::shared_ptr<EncodeInterface> intfImpl,
                                               C2ComponentFactory::ComponentDeleter deleter);

    VaapiEncodeComponent(const std::string& name, c2_node_id_t id,
                         std::shared_ptr<EncodeInterface> intfImpl);

    ~VaapiEncodeComponent() override;

protected:
    // Create mEncoder = VaapiVideoEncoder::create(...) on the encoder thread,
    // exactly like V4L2EncodeComponent::initializeEncoder().
    bool initializeEncoder() override;

private:
    // The number of concurrent encoder instances currently created.
    static std::atomic<int32_t> sConcurrentInstances;
};

}  // namespace android

#endif  // ANDROID_C2_VA_VAAPI_ENCODE_COMPONENT_H
