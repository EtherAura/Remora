// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_VAAPI_DECODE_COMPONENT_H
#define ANDROID_C2_VA_VAAPI_DECODE_COMPONENT_H

#include <v4l2_codec2/components/DecodeComponent.h>

namespace android {

class VaapiDecodeComponent : public DecodeComponent {
public:
    static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               std::shared_ptr<DecodeInterface> intfImpl,
                                               C2ComponentFactory::ComponentDeleter deleter);

    VaapiDecodeComponent(uint32_t debugStreamId, const std::string& name, c2_node_id_t id,
                         std::shared_ptr<DecodeInterface> intfImpl);

    ~VaapiDecodeComponent() override;

    void startTask(c2_status_t* status, ::base::WaitableEvent* done) override;

    // Shadows DecodeComponent::getVideoFramePool (bound by name in startTask):
    // surface-bound sessions get RGB output buffers instead of NV12 — see the
    // implementation for why (remora-e5x.21).
    std::unique_ptr<VideoFramePool> getVideoFramePool(const ui::Size& size,
                                                      HalPixelFormat pixelFormat,
                                                      size_t numBuffers);

private:
    static std::atomic<int32_t> sConcurrentInstances;
    static std::atomic<uint32_t> sNextDebugStreamId;
};

};  // namespace android

#endif  // ANDROID_C2_VA_VAAPI_DECODE_COMPONENT_H
