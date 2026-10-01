// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_COMMON_VAAPI_COMPONENT_NAME_H
#define ANDROID_C2_VA_COMMON_VAAPI_COMPONENT_NAME_H

#include <v4l2_codec2/common/VideoTypes.h>
#include <optional>
#include <string>

namespace android {

// Defines the names of all supported components.
//
// One name per component: the c2.remora.vaapi.* spelling (bd remora-28ix.4).
//
// The component name crosses the host-image boundary on the mirror's CLI
// (--video-encoder=), so it is a contract with the host rather than an internal
// detail: Builders emits c2.remora.vaapi.hevc.encoder. A host that names a
// component this image does not register gets no component, not a silent
// fallback.
struct VaapiComponentName {
    static const std::string kH264Decoder;
    static const std::string kHEVCDecoder;
    static const std::string kVP9Decoder;
    static const std::string kAV1Decoder;

    // Encoder names. Ranked below the c2.android.* software encoders (in
    // media_codecs_c2_va.xml) so the mirror prefers the HW VAAPI encoder, which is
    // not hard-capped to 512x512 like the software HEVC encoder.
    static const std::string kHEVCEncoder;
    static const std::string kH264Encoder;

    // Return true if |name| is a valid component name.
    static bool isValid(const std::string& name);

    // Return true if |name| is an encoder name.
    // Note that |name| should be a valid component name.
    static bool isEncoder(const std::string& name);

    // Return true if |name| is a decoder name.
    // Note that |name| should be a valid component name.
    static bool isDecoder(const std::string& name);

    // Returns VideoCodec for |name| component.
    static std::optional<VideoCodec> getCodec(const std::string& name);
};

}  // namespace android

#endif  // ANDROID_C2_VA_COMMON_VAAPI_COMPONENT_NAME_H
