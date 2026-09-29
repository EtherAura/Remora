// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_STORE_VAAPI_COMPONENT_STORE_H
#define ANDROID_C2_VA_STORE_VAAPI_COMPONENT_STORE_H

#include <C2Component.h>

namespace android {

struct VaapiComponentStore {
    static std::shared_ptr<C2ComponentStore> Create();
};

}  // namespace android

#endif  // ANDROID_C2_VA_STORE_VAAPI_COMPONENT_STORE_H
