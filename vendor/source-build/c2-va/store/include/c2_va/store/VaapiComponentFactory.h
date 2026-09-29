// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_STORE_VAAPI_COMPONENT_FACTORY_H
#define ANDROID_C2_VA_STORE_VAAPI_COMPONENT_FACTORY_H

#include <memory>
#include <string>

#include <C2ComponentFactory.h>
#include <util/C2InterfaceHelper.h>
#include <v4l2_codec2/common/Common.h>

namespace android {

struct SupportedCapabilities;
class DecodeInterface;
class EncodeInterface;

class VaapiComponentFactory : public C2ComponentFactory {
public:
    static std::unique_ptr<VaapiComponentFactory> create(
            const std::string& componentName, std::shared_ptr<C2ReflectorHelper> reflector);
    VaapiComponentFactory(const std::string& componentName, bool isEncoder,
                          std::shared_ptr<C2ReflectorHelper> reflector);
    ~VaapiComponentFactory() override = default;

    // Implementation of C2ComponentFactory.
    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override;
    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override;

private:
    c2_status_t createDecodeInterface(std::shared_ptr<DecodeInterface>* intfImpl);
    c2_status_t createEncodeInterface(std::shared_ptr<EncodeInterface>* intfImpl);

    const std::string mComponentName;
    const bool mIsEncoder;
    std::shared_ptr<C2ReflectorHelper> mReflector;
    std::unique_ptr<SupportedCapabilities> mCapabilites;
};

}  // namespace android

#endif  // ANDROID_C2_VA_STORE_VAAPI_COMPONENT_FACTORY_H
