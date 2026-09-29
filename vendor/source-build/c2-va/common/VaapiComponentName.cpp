// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiComponentName"

#include <c2_va/common/VaapiComponentName.h>

#include <log/log.h>
#include <map>
#include <set>

namespace android {

// Component names follow the c2.remora.vaapi.* convention (bd remora-28ix.4),
// one name per component — see the header. Rank (kept below the c2.android.* software decoders, ~512)
// is applied in the ComponentStore and in media_codecs_c2_va.xml, not in the name.
const std::string VaapiComponentName::kH264Decoder = "c2.remora.vaapi.avc.decoder";
const std::string VaapiComponentName::kHEVCDecoder = "c2.remora.vaapi.hevc.decoder";
const std::string VaapiComponentName::kVP9Decoder = "c2.remora.vaapi.vp9.decoder";
const std::string VaapiComponentName::kAV1Decoder = "c2.remora.vaapi.av1.decoder";

// Encoder names. HEVC is the priority (full-res H.265 mirroring); H.264
// shares the same VAAPI backend. Ranked below the c2.android.* software encoders
// in media_codecs_c2_va.xml so HW is preferred with SW as fallback.
const std::string VaapiComponentName::kHEVCEncoder = "c2.remora.vaapi.hevc.encoder";
const std::string VaapiComponentName::kH264Encoder = "c2.remora.vaapi.avc.encoder";


// static
bool VaapiComponentName::isValid(const std::string& name) {
    return name == kH264Decoder || name == kHEVCDecoder || name == kVP9Decoder ||
           name == kAV1Decoder || name == kHEVCEncoder || name == kH264Encoder;
}

// static
bool VaapiComponentName::isEncoder(const std::string& name) {
    ALOG_ASSERT(isValid(name));
    return name == kHEVCEncoder || name == kH264Encoder;
}

// static
bool VaapiComponentName::isDecoder(const std::string& name) {
    ALOG_ASSERT(isValid(name));
    static const std::set<std::string> kValidDecoders = {
            kH264Decoder,
            kHEVCDecoder,
            kVP9Decoder,
            kAV1Decoder,
    };

    return kValidDecoders.find(name) != kValidDecoders.end();
}

// static
std::optional<VideoCodec> VaapiComponentName::getCodec(const std::string& name) {
    ALOG_ASSERT(isValid(name));
    static const std::map<std::string, VideoCodec> kNameToCodecs = {
            {kH264Decoder, VideoCodec::H264},
            {kHEVCDecoder, VideoCodec::HEVC},
            {kVP9Decoder, VideoCodec::VP9},
            {kAV1Decoder, VideoCodec::AV1},
            {kHEVCEncoder, VideoCodec::HEVC},
            {kH264Encoder, VideoCodec::H264},
    };

    auto iter = kNameToCodecs.find(name);
    if (iter == kNameToCodecs.end()) {
        return std::nullopt;
    }
    return iter->second;
}

}  // namespace android
