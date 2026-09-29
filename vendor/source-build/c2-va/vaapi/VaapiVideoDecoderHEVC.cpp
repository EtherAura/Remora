// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The HEVC/H.265 half of VaapiVideoDecoder: VPS/SPS/PPS/slice parsing, POC and
// reference-picture-set derivation, and VA buffer submission. Split out of
// VaapiVideoDecoder.cpp (which keeps the H.264 core plus everything
// codec-agnostic: the surface pool, the DPB bumping/reclaim and the async
// output path, all reused verbatim here).
//
// Structure mirrors the H.264 path deliberately: each NAL payload is unescaped
// into its RBSP and parsed with a plain ABitReader so consumed-bit counts stay
// exact, and reads use getBitsWithFallback (raw getBits aborts the process on
// over-read) with a final overRead() check catching truncation.

// Must precede <utils/Trace.h>: without it ATRACE_TAG defaults to
// ATRACE_TAG_NEVER and every ATRACE_CALL in this file compiles to a no-op.
#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <c2_va/vaapi/VaapiVideoDecoder.h>

#include <string.h>

#include <algorithm>
#include <vector>

#include <log/log.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <utils/Trace.h>
#include <va/va.h>
#include <va/va_dec_hevc.h>

#include <v4l2_codec2/common/HEVCNalParser.h>
#include <v4l2_codec2/common/NalParser.h>

namespace android {
namespace {

// slice_type (Table 7-7). Note these differ from H.264's numbering.
constexpr uint32_t kSliceTypeB = 0;
constexpr uint32_t kSliceTypeP = 1;
constexpr uint32_t kSliceTypeI = 2;

// NAL unit types (Table 7-1).
constexpr uint8_t kNalRadlN = 6;
constexpr uint8_t kNalRaslN = 8;
constexpr uint8_t kNalRaslR = 9;
constexpr uint8_t kNalBlaWLp = 16;
constexpr uint8_t kNalBlaNLp = 18;
constexpr uint8_t kNalIdrWRadl = 19;
constexpr uint8_t kNalIdrNLp = 20;
constexpr uint8_t kNalIrapEnd = 23;
constexpr uint8_t kNalVps = 32;
constexpr uint8_t kNalSps = 33;
constexpr uint8_t kNalPps = 34;

bool isIrapNal(uint8_t t) { return t >= kNalBlaWLp && t <= kNalIrapEnd; }
bool isIdrNal(uint8_t t) { return t == kNalIdrWRadl || t == kNalIdrNLp; }
bool isBlaNal(uint8_t t) { return t >= kNalBlaWLp && t <= kNalBlaNLp; }
bool isRaslNal(uint8_t t) { return t == kNalRaslN || t == kNalRaslR; }
bool isRadlNal(uint8_t t) { return t == kNalRadlN || t == kNalRadlN + 1; }
// Sub-layer non-reference pictures are the even-numbered types below 16.
bool isSlnrNal(uint8_t t) { return t < kNalBlaWLp && (t % 2) == 0; }
// VCL NALs carrying a slice segment (types 0..21 in version 1 streams).
bool isSliceNal(uint8_t t) { return t <= kNalIrapEnd; }

int32_t clip3(int32_t lo, int32_t hi, int32_t v) { return v < lo ? lo : (v > hi ? hi : v); }

// Number of bits needed to represent values 0..n-1, i.e. Ceil(Log2(n)).
uint32_t ceilLog2(uint32_t n) {
    uint32_t bits = 0;
    while (n > (1u << bits)) ++bits;
    return bits;
}

// Unescape a NAL payload into its RBSP, recording for each RBSP byte the index
// it came from in the escaped stream. The mapping lets us report VA's
// slice_data_num_emu_prevn_bytes: the emulation-prevention bytes removed before
// RBSP offset p is exactly origPos[p] - p.
std::vector<uint8_t> unescapeRbspTracked(const uint8_t* data, size_t size,
                                         std::vector<uint32_t>* origPos) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    origPos->clear();
    origPos->reserve(size);
    size_t zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(b);
        origPos->push_back(static_cast<uint32_t>(i));
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return rbsp;
}

// profile_tier_level() (7.3.3). Nothing in it is needed for VA decode, but it
// is variable-length so it must be walked exactly.
void skipProfileTierLevel(ABitReader* br, bool profilePresentFlag,
                          uint32_t maxNumSubLayersMinus1) {
    if (profilePresentFlag) {
        // general_profile_space/tier/idc (8) + compatibility flags (32) +
        // progressive/interlaced/non_packed/frame_only (4) + 44 reserved.
        br->skipBits(8);
        br->skipBits(32);
        br->skipBits(4);
        br->skipBits(32);
        br->skipBits(12);
    }
    br->skipBits(8);  // general_level_idc

    bool subLayerProfilePresent[8] = {};
    bool subLayerLevelPresent[8] = {};
    const uint32_t n = std::min<uint32_t>(maxNumSubLayersMinus1, 8);
    for (uint32_t i = 0; i < n; ++i) {
        subLayerProfilePresent[i] = br->getBitsWithFallback(1, 0);
        subLayerLevelPresent[i] = br->getBitsWithFallback(1, 0);
    }
    if (maxNumSubLayersMinus1 > 0) {
        for (uint32_t i = maxNumSubLayersMinus1; i < 8; ++i) br->skipBits(2);
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (subLayerProfilePresent[i]) {
            br->skipBits(32);
            br->skipBits(32);
            br->skipBits(24);
        }
        if (subLayerLevelPresent[i]) br->skipBits(8);
    }
}

// Default scaling lists, Table 7-5 (intra) and 7-6 (inter), in the up-right
// diagonal coefficient order the syntax uses — which is also the order VA wants
// (its ScalingListNxN mirror the spec's ScalingList[i][MatrixID][j] directly).
const uint8_t kDefaultScaling8x8Intra[64] = {
        16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25,
        16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
        17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65,
        21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115};
const uint8_t kDefaultScaling8x8Inter[64] = {
        16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25,
        16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
        17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54,
        20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91};

// Flat 16 everywhere: what a stream with scaling_list_enabled_flag == 0 uses.
void setFlatScalingList(VaapiVideoDecoder::H265ScalingList* sl) {
    memset(sl->list4x4, 16, sizeof(sl->list4x4));
    memset(sl->list8x8, 16, sizeof(sl->list8x8));
    memset(sl->list16x16, 16, sizeof(sl->list16x16));
    memset(sl->list32x32, 16, sizeof(sl->list32x32));
    memset(sl->dc16x16, 16, sizeof(sl->dc16x16));
    memset(sl->dc32x32, 16, sizeof(sl->dc32x32));
}

void setDefaultScalingList(VaapiVideoDecoder::H265ScalingList* sl) {
    memset(sl->list4x4, 16, sizeof(sl->list4x4));
    for (int m = 0; m < 6; ++m) {
        memcpy(sl->list8x8[m], m < 3 ? kDefaultScaling8x8Intra : kDefaultScaling8x8Inter, 64);
        memcpy(sl->list16x16[m], m < 3 ? kDefaultScaling8x8Intra : kDefaultScaling8x8Inter, 64);
        sl->dc16x16[m] = 16;
    }
    for (int m = 0; m < 2; ++m) {
        memcpy(sl->list32x32[m], m == 0 ? kDefaultScaling8x8Intra : kDefaultScaling8x8Inter, 64);
        sl->dc32x32[m] = 16;
    }
}

// scaling_list_data() (7.3.4).
bool parseScalingListData(ABitReader* br, VaapiVideoDecoder::H265ScalingList* sl) {
    setDefaultScalingList(sl);
    for (int sizeId = 0; sizeId < 4; ++sizeId) {
        const int numMatrix = (sizeId == 3) ? 2 : 6;
        for (int matrixId = 0; matrixId < numMatrix; ++matrixId) {
            const bool predMode = br->getBitsWithFallback(1, 0);  // ..._pred_mode_flag
            if (!predMode) {
                uint32_t delta = 0;
                if (!NalParser::parseUE(br, &delta)) return false;
                // refMatrixId = matrixId - delta * (sizeId == 3 ? 3 : 1); delta 0
                // means "use the default list", which is already loaded.
                if (delta != 0) {
                    const int step = (sizeId == 3) ? 3 : 1;
                    const int refId = matrixId - static_cast<int>(delta) * step;
                    if (refId < 0 || refId >= numMatrix) return false;
                    switch (sizeId) {
                    case 0: memcpy(sl->list4x4[matrixId], sl->list4x4[refId], 16); break;
                    case 1: memcpy(sl->list8x8[matrixId], sl->list8x8[refId], 64); break;
                    case 2:
                        memcpy(sl->list16x16[matrixId], sl->list16x16[refId], 64);
                        sl->dc16x16[matrixId] = sl->dc16x16[refId];
                        break;
                    default:
                        memcpy(sl->list32x32[matrixId], sl->list32x32[refId], 64);
                        sl->dc32x32[matrixId] = sl->dc32x32[refId];
                        break;
                    }
                }
                continue;
            }
            const int coefNum = std::min(64, 1 << (4 + (sizeId << 1)));
            int32_t nextCoef = 8;
            if (sizeId > 1) {
                int32_t dcMinus8 = 0;
                if (!NalParser::parseSE(br, &dcMinus8)) return false;
                nextCoef = dcMinus8 + 8;
                if (sizeId == 2) {
                    sl->dc16x16[matrixId] = static_cast<uint8_t>(nextCoef);
                } else {
                    sl->dc32x32[matrixId] = static_cast<uint8_t>(nextCoef);
                }
            }
            for (int i = 0; i < coefNum; ++i) {
                int32_t deltaCoef = 0;
                if (!NalParser::parseSE(br, &deltaCoef)) return false;
                nextCoef = (nextCoef + deltaCoef + 256) % 256;
                const uint8_t v = static_cast<uint8_t>(nextCoef);
                switch (sizeId) {
                case 0: sl->list4x4[matrixId][i] = v; break;
                case 1: sl->list8x8[matrixId][i] = v; break;
                case 2: sl->list16x16[matrixId][i] = v; break;
                default: sl->list32x32[matrixId][i] = v; break;
                }
            }
        }
    }
    return true;
}

// st_ref_pic_set() (7.3.7), resolved into the derived arrays of 7.4.8.
// |sets| is the SPS list (for inter-RPS prediction), |idx| the set being parsed.
bool parseShortTermRefPicSet(ABitReader* br, uint32_t idx, uint32_t numShortTermSets,
                             const VaapiVideoDecoder::H265ShortTermRPS* sets,
                             VaapiVideoDecoder::H265ShortTermRPS* out) {
    *out = VaapiVideoDecoder::H265ShortTermRPS();

    bool interPred = false;
    if (idx != 0) interPred = br->getBitsWithFallback(1, 0);

    if (interPred) {
        uint32_t deltaIdxMinus1 = 0;
        if (idx == numShortTermSets) {
            if (!NalParser::parseUE(br, &deltaIdxMinus1)) return false;
        }
        if (deltaIdxMinus1 + 1 > idx) return false;
        const uint32_t refIdx = idx - (deltaIdxMinus1 + 1);
        const int32_t deltaRpsSign = br->getBitsWithFallback(1, 0);
        uint32_t absDeltaRpsMinus1 = 0;
        if (!NalParser::parseUE(br, &absDeltaRpsMinus1)) return false;
        const int32_t deltaRps =
                (1 - 2 * deltaRpsSign) * static_cast<int32_t>(absDeltaRpsMinus1 + 1);

        const VaapiVideoDecoder::H265ShortTermRPS& ref = sets[refIdx];
        const uint32_t refNumDelta = ref.numDeltaPocs();
        if (refNumDelta + 1 > 17) return false;
        bool usedByCurr[17] = {};
        bool useDelta[17] = {};
        for (uint32_t j = 0; j <= refNumDelta; ++j) {
            usedByCurr[j] = br->getBitsWithFallback(1, 0);
            useDelta[j] = usedByCurr[j] ? true : br->getBitsWithFallback(1, 0);
        }

        // 7-59 / 7-60: splice the reference set's deltas around deltaRps,
        // keeping each side sorted by increasing distance from the current pic.
        int i = 0;
        for (int j = static_cast<int>(ref.num_positive_pics) - 1; j >= 0; --j) {
            const int32_t dPoc = ref.deltaPocS1[j] + deltaRps;
            if (dPoc < 0 && useDelta[ref.num_negative_pics + j]) {
                out->deltaPocS0[i] = dPoc;
                out->usedByCurrS0[i++] = usedByCurr[ref.num_negative_pics + j];
            }
        }
        if (deltaRps < 0 && useDelta[refNumDelta]) {
            out->deltaPocS0[i] = deltaRps;
            out->usedByCurrS0[i++] = usedByCurr[refNumDelta];
        }
        for (uint32_t j = 0; j < ref.num_negative_pics && i < 16; ++j) {
            const int32_t dPoc = ref.deltaPocS0[j] + deltaRps;
            if (dPoc < 0 && useDelta[j]) {
                out->deltaPocS0[i] = dPoc;
                out->usedByCurrS0[i++] = usedByCurr[j];
            }
        }
        out->num_negative_pics = static_cast<uint32_t>(i);

        i = 0;
        for (int j = static_cast<int>(ref.num_negative_pics) - 1; j >= 0; --j) {
            const int32_t dPoc = ref.deltaPocS0[j] + deltaRps;
            if (dPoc > 0 && useDelta[j]) {
                out->deltaPocS1[i] = dPoc;
                out->usedByCurrS1[i++] = usedByCurr[j];
            }
        }
        if (deltaRps > 0 && useDelta[refNumDelta]) {
            out->deltaPocS1[i] = deltaRps;
            out->usedByCurrS1[i++] = usedByCurr[refNumDelta];
        }
        for (uint32_t j = 0; j < ref.num_positive_pics && i < 16; ++j) {
            const int32_t dPoc = ref.deltaPocS1[j] + deltaRps;
            if (dPoc > 0 && useDelta[ref.num_negative_pics + j]) {
                out->deltaPocS1[i] = dPoc;
                out->usedByCurrS1[i++] = usedByCurr[ref.num_negative_pics + j];
            }
        }
        out->num_positive_pics = static_cast<uint32_t>(i);
        return true;
    }

    uint32_t numNegative = 0;
    uint32_t numPositive = 0;
    if (!NalParser::parseUE(br, &numNegative)) return false;
    if (!NalParser::parseUE(br, &numPositive)) return false;
    if (numNegative > 16 || numPositive > 16) return false;
    out->num_negative_pics = numNegative;
    out->num_positive_pics = numPositive;

    int32_t prev = 0;
    for (uint32_t i = 0; i < numNegative; ++i) {
        uint32_t deltaMinus1 = 0;
        if (!NalParser::parseUE(br, &deltaMinus1)) return false;
        prev -= static_cast<int32_t>(deltaMinus1 + 1);
        out->deltaPocS0[i] = prev;
        out->usedByCurrS0[i] = br->getBitsWithFallback(1, 0);
    }
    prev = 0;
    for (uint32_t i = 0; i < numPositive; ++i) {
        uint32_t deltaMinus1 = 0;
        if (!NalParser::parseUE(br, &deltaMinus1)) return false;
        prev += static_cast<int32_t>(deltaMinus1 + 1);
        out->deltaPocS1[i] = prev;
        out->usedByCurrS1[i] = br->getBitsWithFallback(1, 0);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parameter-set parsing
// ---------------------------------------------------------------------------

bool VaapiVideoDecoder::parseH265SPS(const uint8_t* nal, size_t size, H265SPS* sps) {
    *sps = H265SPS();
    if (size < 3) return false;
    // Skip the 2-byte NAL header.
    std::vector<uint32_t> origPos;
    const std::vector<uint8_t> rbsp = unescapeRbspTracked(nal + 2, size - 2, &origPos);
    ABitReader br(rbsp.data(), rbsp.size());

    br.skipBits(4);  // sps_video_parameter_set_id
    const uint32_t maxSubLayersMinus1 = br.getBitsWithFallback(3, 0);
    br.skipBits(1);  // sps_temporal_id_nesting_flag
    skipProfileTierLevel(&br, true, maxSubLayersMinus1);

    if (!NalParser::parseUE(&br, &sps->sps_seq_parameter_set_id)) return false;
    if (!NalParser::parseUE(&br, &sps->chroma_format_idc)) return false;
    if (sps->chroma_format_idc == 3) {
        sps->separate_colour_plane_flag = br.getBitsWithFallback(1, 0);
    }
    if (!NalParser::parseUE(&br, &sps->pic_width_in_luma_samples)) return false;
    if (!NalParser::parseUE(&br, &sps->pic_height_in_luma_samples)) return false;
    if (br.getBitsWithFallback(1, 0)) {  // conformance_window_flag
        if (!NalParser::parseUE(&br, &sps->conf_win_left_offset)) return false;
        if (!NalParser::parseUE(&br, &sps->conf_win_right_offset)) return false;
        if (!NalParser::parseUE(&br, &sps->conf_win_top_offset)) return false;
        if (!NalParser::parseUE(&br, &sps->conf_win_bottom_offset)) return false;
    }
    if (!NalParser::parseUE(&br, &sps->bit_depth_luma_minus8)) return false;
    if (!NalParser::parseUE(&br, &sps->bit_depth_chroma_minus8)) return false;
    if (!NalParser::parseUE(&br, &sps->log2_max_pic_order_cnt_lsb_minus4)) return false;

    const bool subLayerOrdering = br.getBitsWithFallback(1, 0);
    for (uint32_t i = (subLayerOrdering ? 0 : maxSubLayersMinus1); i <= maxSubLayersMinus1; ++i) {
        uint32_t maxDecPicBufferingMinus1 = 0;
        uint32_t maxNumReorderPics = 0;
        uint32_t maxLatencyIncreasePlus1 = 0;
        if (!NalParser::parseUE(&br, &maxDecPicBufferingMinus1)) return false;
        if (!NalParser::parseUE(&br, &maxNumReorderPics)) return false;
        if (!NalParser::parseUE(&br, &maxLatencyIncreasePlus1)) return false;
        // Only the values for the highest sub-layer (the one we decode) matter.
        sps->sps_max_dec_pic_buffering_minus1 = maxDecPicBufferingMinus1;
        sps->sps_max_num_reorder_pics = maxNumReorderPics;
    }

    if (!NalParser::parseUE(&br, &sps->log2_min_luma_coding_block_size_minus3)) return false;
    if (!NalParser::parseUE(&br, &sps->log2_diff_max_min_luma_coding_block_size)) return false;
    if (!NalParser::parseUE(&br, &sps->log2_min_transform_block_size_minus2)) return false;
    if (!NalParser::parseUE(&br, &sps->log2_diff_max_min_transform_block_size)) return false;
    if (!NalParser::parseUE(&br, &sps->max_transform_hierarchy_depth_inter)) return false;
    if (!NalParser::parseUE(&br, &sps->max_transform_hierarchy_depth_intra)) return false;

    sps->scaling_list_enabled_flag = br.getBitsWithFallback(1, 0);
    if (sps->scaling_list_enabled_flag) {
        if (br.getBitsWithFallback(1, 0)) {  // sps_scaling_list_data_present_flag
            if (!parseScalingListData(&br, &sps->scaling_list)) return false;
        } else {
            setDefaultScalingList(&sps->scaling_list);
        }
    } else {
        setFlatScalingList(&sps->scaling_list);
    }

    sps->amp_enabled_flag = br.getBitsWithFallback(1, 0);
    sps->sample_adaptive_offset_enabled_flag = br.getBitsWithFallback(1, 0);
    sps->pcm_enabled_flag = br.getBitsWithFallback(1, 0);
    if (sps->pcm_enabled_flag) {
        sps->pcm_sample_bit_depth_luma_minus1 = br.getBitsWithFallback(4, 0);
        sps->pcm_sample_bit_depth_chroma_minus1 = br.getBitsWithFallback(4, 0);
        if (!NalParser::parseUE(&br, &sps->log2_min_pcm_luma_coding_block_size_minus3)) return false;
        if (!NalParser::parseUE(&br, &sps->log2_diff_max_min_pcm_luma_coding_block_size)) {
            return false;
        }
        sps->pcm_loop_filter_disabled_flag = br.getBitsWithFallback(1, 0);
    }

    if (!NalParser::parseUE(&br, &sps->num_short_term_ref_pic_sets)) return false;
    if (sps->num_short_term_ref_pic_sets > 64) return false;
    for (uint32_t i = 0; i < sps->num_short_term_ref_pic_sets; ++i) {
        if (!parseShortTermRefPicSet(&br, i, sps->num_short_term_ref_pic_sets, sps->st_rps,
                                     &sps->st_rps[i])) {
            ALOGE("Failed to parse SPS short_term_ref_pic_set %u.", i);
            return false;
        }
    }

    sps->long_term_ref_pics_present_flag = br.getBitsWithFallback(1, 0);
    if (sps->long_term_ref_pics_present_flag) {
        if (!NalParser::parseUE(&br, &sps->num_long_term_ref_pics_sps)) return false;
        if (sps->num_long_term_ref_pics_sps > 32) return false;
        const uint32_t pocLsbBits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
        for (uint32_t i = 0; i < sps->num_long_term_ref_pics_sps; ++i) {
            sps->lt_ref_pic_poc_lsb_sps[i] = br.getBitsWithFallback(pocLsbBits, 0);
            sps->used_by_curr_pic_lt_sps_flag[i] = br.getBitsWithFallback(1, 0);
        }
    }

    sps->sps_temporal_mvp_enabled_flag = br.getBitsWithFallback(1, 0);
    sps->strong_intra_smoothing_enabled_flag = br.getBitsWithFallback(1, 0);
    // vui_parameters() and sps_extension follow; nothing past here is needed.

    if (br.overRead()) {
        ALOGE("Truncated HEVC SPS.");
        return false;
    }
    if (sps->pic_width_in_luma_samples == 0 || sps->pic_height_in_luma_samples == 0) {
        ALOGE("HEVC SPS has zero picture dimensions.");
        return false;
    }
    sps->valid = true;
    return true;
}

bool VaapiVideoDecoder::parseH265PPS(const uint8_t* nal, size_t size, H265PPS* pps) {
    *pps = H265PPS();
    if (size < 3) return false;
    std::vector<uint32_t> origPos;
    const std::vector<uint8_t> rbsp = unescapeRbspTracked(nal + 2, size - 2, &origPos);
    ABitReader br(rbsp.data(), rbsp.size());

    if (!NalParser::parseUE(&br, &pps->pps_pic_parameter_set_id)) return false;
    if (!NalParser::parseUE(&br, &pps->pps_seq_parameter_set_id)) return false;
    pps->dependent_slice_segments_enabled_flag = br.getBitsWithFallback(1, 0);
    pps->output_flag_present_flag = br.getBitsWithFallback(1, 0);
    pps->num_extra_slice_header_bits = br.getBitsWithFallback(3, 0);
    pps->sign_data_hiding_enabled_flag = br.getBitsWithFallback(1, 0);
    pps->cabac_init_present_flag = br.getBitsWithFallback(1, 0);
    if (!NalParser::parseUE(&br, &pps->num_ref_idx_l0_default_active_minus1)) return false;
    if (!NalParser::parseUE(&br, &pps->num_ref_idx_l1_default_active_minus1)) return false;
    if (!NalParser::parseSE(&br, &pps->init_qp_minus26)) return false;
    pps->constrained_intra_pred_flag = br.getBitsWithFallback(1, 0);
    pps->transform_skip_enabled_flag = br.getBitsWithFallback(1, 0);
    pps->cu_qp_delta_enabled_flag = br.getBitsWithFallback(1, 0);
    if (pps->cu_qp_delta_enabled_flag) {
        if (!NalParser::parseUE(&br, &pps->diff_cu_qp_delta_depth)) return false;
    }
    if (!NalParser::parseSE(&br, &pps->pps_cb_qp_offset)) return false;
    if (!NalParser::parseSE(&br, &pps->pps_cr_qp_offset)) return false;
    pps->pps_slice_chroma_qp_offsets_present_flag = br.getBitsWithFallback(1, 0);
    pps->weighted_pred_flag = br.getBitsWithFallback(1, 0);
    pps->weighted_bipred_flag = br.getBitsWithFallback(1, 0);
    pps->transquant_bypass_enabled_flag = br.getBitsWithFallback(1, 0);
    pps->tiles_enabled_flag = br.getBitsWithFallback(1, 0);
    pps->entropy_coding_sync_enabled_flag = br.getBitsWithFallback(1, 0);
    if (pps->tiles_enabled_flag) {
        if (!NalParser::parseUE(&br, &pps->num_tile_columns_minus1)) return false;
        if (!NalParser::parseUE(&br, &pps->num_tile_rows_minus1)) return false;
        if (pps->num_tile_columns_minus1 >= 19 || pps->num_tile_rows_minus1 >= 21) return false;
        pps->uniform_spacing_flag = br.getBitsWithFallback(1, 0);
        if (!pps->uniform_spacing_flag) {
            for (uint32_t i = 0; i < pps->num_tile_columns_minus1; ++i) {
                if (!NalParser::parseUE(&br, &pps->column_width_minus1[i])) return false;
            }
            for (uint32_t i = 0; i < pps->num_tile_rows_minus1; ++i) {
                if (!NalParser::parseUE(&br, &pps->row_height_minus1[i])) return false;
            }
        }
        pps->loop_filter_across_tiles_enabled_flag = br.getBitsWithFallback(1, 1);
    }
    pps->pps_loop_filter_across_slices_enabled_flag = br.getBitsWithFallback(1, 0);
    pps->deblocking_filter_control_present_flag = br.getBitsWithFallback(1, 0);
    if (pps->deblocking_filter_control_present_flag) {
        pps->deblocking_filter_override_enabled_flag = br.getBitsWithFallback(1, 0);
        pps->pps_deblocking_filter_disabled_flag = br.getBitsWithFallback(1, 0);
        if (!pps->pps_deblocking_filter_disabled_flag) {
            if (!NalParser::parseSE(&br, &pps->pps_beta_offset_div2)) return false;
            if (!NalParser::parseSE(&br, &pps->pps_tc_offset_div2)) return false;
        }
    }
    pps->pps_scaling_list_data_present_flag = br.getBitsWithFallback(1, 0);
    if (pps->pps_scaling_list_data_present_flag) {
        if (!parseScalingListData(&br, &pps->scaling_list)) return false;
    }
    pps->lists_modification_present_flag = br.getBitsWithFallback(1, 0);
    if (!NalParser::parseUE(&br, &pps->log2_parallel_merge_level_minus2)) return false;
    pps->slice_segment_header_extension_present_flag = br.getBitsWithFallback(1, 0);

    if (br.overRead()) {
        ALOGE("Truncated HEVC PPS.");
        return false;
    }
    pps->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Slice-segment header parsing (7.3.6)
// ---------------------------------------------------------------------------

bool VaapiVideoDecoder::parseH265SliceHeader(const uint8_t* nal, size_t size, H265SliceHeader* sh) {
    *sh = H265SliceHeader();
    if (size < 3) return false;

    // nal_unit_header(): forbidden_zero(1) type(6) layer_id(6) temporal_id_plus1(3).
    sh->nalUnitType = static_cast<uint8_t>((nal[0] >> 1) & 0x3f);
    sh->temporalId = static_cast<uint32_t>(nal[1] & 0x07);
    if (sh->temporalId == 0) return false;  // temporal_id_plus1 == 0 is illegal
    sh->temporalId -= 1;
    sh->idrPicFlag = isIdrNal(sh->nalUnitType);
    sh->irapPicFlag = isIrapNal(sh->nalUnitType);
    sh->nalData = nal;
    sh->nalSize = size;

    std::vector<uint32_t> origPos;
    const std::vector<uint8_t> rbsp = unescapeRbspTracked(nal + 2, size - 2, &origPos);
    ABitReader br(rbsp.data(), rbsp.size());
    const size_t rbspTotalBits = rbsp.size() * 8;

    sh->first_slice_segment_in_pic_flag = br.getBitsWithFallback(1, 0);
    if (sh->irapPicFlag) br.skipBits(1);  // no_output_of_prior_pics_flag
    if (!NalParser::parseUE(&br, &sh->slice_pic_parameter_set_id)) return false;

    auto ppsIt = mH265PPSes.find(sh->slice_pic_parameter_set_id);
    if (ppsIt == mH265PPSes.end()) {
        ALOGE("HEVC slice refers to unknown PPS %u.", sh->slice_pic_parameter_set_id);
        return false;
    }
    const H265PPS& pps = ppsIt->second;
    auto spsIt = mH265SPSes.find(pps.pps_seq_parameter_set_id);
    if (spsIt == mH265SPSes.end()) {
        ALOGE("HEVC PPS refers to unknown SPS %u.", pps.pps_seq_parameter_set_id);
        return false;
    }
    const H265SPS& sps = spsIt->second;
    const uint32_t chromaArrayType = sps.separate_colour_plane_flag ? 0 : sps.chroma_format_idc;

    if (!sh->first_slice_segment_in_pic_flag) {
        if (pps.dependent_slice_segments_enabled_flag) {
            sh->dependent_slice_segment_flag = br.getBitsWithFallback(1, 0);
        }
        const uint32_t ctbLog2SizeY = sps.log2_min_luma_coding_block_size_minus3 + 3 +
                                      sps.log2_diff_max_min_luma_coding_block_size;
        const uint32_t ctbSizeY = 1u << ctbLog2SizeY;
        const uint32_t picWidthInCtbsY =
                (sps.pic_width_in_luma_samples + ctbSizeY - 1) / ctbSizeY;
        const uint32_t picHeightInCtbsY =
                (sps.pic_height_in_luma_samples + ctbSizeY - 1) / ctbSizeY;
        const uint32_t addrBits = ceilLog2(picWidthInCtbsY * picHeightInCtbsY);
        sh->slice_segment_address = br.getBitsWithFallback(addrBits, 0);
    }

    // Everything below is signalled only for an independent slice segment; a
    // dependent one inherits it from the preceding independent segment (done by
    // the caller, which has the access unit's slice list).
    if (!sh->dependent_slice_segment_flag) {
        for (uint32_t i = 0; i < pps.num_extra_slice_header_bits; ++i) br.skipBits(1);
        if (!NalParser::parseUE(&br, &sh->slice_type)) return false;
        if (pps.output_flag_present_flag) sh->pic_output_flag = br.getBitsWithFallback(1, 1);
        if (sps.separate_colour_plane_flag) sh->colour_plane_id = br.getBitsWithFallback(2, 0);

        if (!sh->idrPicFlag) {
            const uint32_t pocLsbBits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
            sh->slice_pic_order_cnt_lsb = br.getBitsWithFallback(pocLsbBits, 0);
            const bool stRpsSpsFlag = br.getBitsWithFallback(1, 0);
            if (!stRpsSpsFlag) {
                const size_t before = br.numBitsLeft();
                if (!parseShortTermRefPicSet(&br, sps.num_short_term_ref_pic_sets,
                                             sps.num_short_term_ref_pic_sets, sps.st_rps,
                                             &sh->st_rps)) {
                    ALOGE("Failed to parse inline short_term_ref_pic_set.");
                    return false;
                }
                sh->st_rps_bits = static_cast<uint32_t>(before - br.numBitsLeft());
            } else if (sps.num_short_term_ref_pic_sets > 1) {
                const uint32_t idxBits = ceilLog2(sps.num_short_term_ref_pic_sets);
                const uint32_t idx = br.getBitsWithFallback(idxBits, 0);
                if (idx >= sps.num_short_term_ref_pic_sets) return false;
                sh->st_rps = sps.st_rps[idx];
            } else if (sps.num_short_term_ref_pic_sets == 1) {
                sh->st_rps = sps.st_rps[0];
            }

            if (sps.long_term_ref_pics_present_flag) {
                if (sps.num_long_term_ref_pics_sps > 0) {
                    if (!NalParser::parseUE(&br, &sh->num_long_term_sps)) return false;
                }
                if (!NalParser::parseUE(&br, &sh->num_long_term_pics)) return false;
                const uint32_t total = sh->num_long_term_sps + sh->num_long_term_pics;
                if (total > 32 || sh->num_long_term_sps > sps.num_long_term_ref_pics_sps) {
                    return false;
                }
                const uint32_t pocLsbLtBits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
                for (uint32_t i = 0; i < total; ++i) {
                    if (i < sh->num_long_term_sps) {
                        uint32_t ltIdxSps = 0;
                        if (sps.num_long_term_ref_pics_sps > 1) {
                            ltIdxSps = br.getBitsWithFallback(
                                    ceilLog2(sps.num_long_term_ref_pics_sps), 0);
                        }
                        if (ltIdxSps >= sps.num_long_term_ref_pics_sps) return false;
                        sh->poc_lsb_lt[i] = sps.lt_ref_pic_poc_lsb_sps[ltIdxSps];
                        sh->used_by_curr_pic_lt_flag[i] =
                                sps.used_by_curr_pic_lt_sps_flag[ltIdxSps];
                    } else {
                        sh->poc_lsb_lt[i] = br.getBitsWithFallback(pocLsbLtBits, 0);
                        sh->used_by_curr_pic_lt_flag[i] = br.getBitsWithFallback(1, 0);
                    }
                    sh->delta_poc_msb_present_flag[i] = br.getBitsWithFallback(1, 0);
                    if (sh->delta_poc_msb_present_flag[i]) {
                        uint32_t cycle = 0;
                        if (!NalParser::parseUE(&br, &cycle)) return false;
                        // 7-52: the cycle is cumulative within each of the two
                        // groups (SPS-derived entries, then slice-signalled).
                        sh->delta_poc_msb_cycle_lt[i] =
                                (i == 0 || i == sh->num_long_term_sps)
                                        ? cycle
                                        : cycle + sh->delta_poc_msb_cycle_lt[i - 1];
                    } else if (i != 0 && i != sh->num_long_term_sps) {
                        sh->delta_poc_msb_cycle_lt[i] = sh->delta_poc_msb_cycle_lt[i - 1];
                    }
                }
            }
            if (sps.sps_temporal_mvp_enabled_flag) {
                sh->slice_temporal_mvp_enabled_flag = br.getBitsWithFallback(1, 0);
            }
        }

        if (sps.sample_adaptive_offset_enabled_flag) {
            sh->slice_sao_luma_flag = br.getBitsWithFallback(1, 0);
            if (chromaArrayType != 0) sh->slice_sao_chroma_flag = br.getBitsWithFallback(1, 0);
        }

        sh->num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
        sh->num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;

        if (sh->slice_type == kSliceTypeP || sh->slice_type == kSliceTypeB) {
            sh->num_ref_idx_active_override_flag = br.getBitsWithFallback(1, 0);
            if (sh->num_ref_idx_active_override_flag) {
                if (!NalParser::parseUE(&br, &sh->num_ref_idx_l0_active_minus1)) return false;
                if (sh->slice_type == kSliceTypeB) {
                    if (!NalParser::parseUE(&br, &sh->num_ref_idx_l1_active_minus1)) return false;
                }
            }
            if (sh->num_ref_idx_l0_active_minus1 > 14 || sh->num_ref_idx_l1_active_minus1 > 14) {
                ALOGE("HEVC slice num_ref_idx out of range.");
                return false;
            }

            // NumPicTotalCurr (7-57): pictures in the RPS used by this picture.
            uint32_t numPicTotalCurr = 0;
            for (uint32_t i = 0; i < sh->st_rps.num_negative_pics; ++i) {
                if (sh->st_rps.usedByCurrS0[i]) ++numPicTotalCurr;
            }
            for (uint32_t i = 0; i < sh->st_rps.num_positive_pics; ++i) {
                if (sh->st_rps.usedByCurrS1[i]) ++numPicTotalCurr;
            }
            for (uint32_t i = 0; i < sh->num_long_term_sps + sh->num_long_term_pics; ++i) {
                if (sh->used_by_curr_pic_lt_flag[i]) ++numPicTotalCurr;
            }

            if (pps.lists_modification_present_flag && numPicTotalCurr > 1) {
                const uint32_t entryBits = ceilLog2(numPicTotalCurr);
                sh->ref_pic_list_modification_flag_l0 = br.getBitsWithFallback(1, 0);
                if (sh->ref_pic_list_modification_flag_l0) {
                    for (uint32_t i = 0; i <= sh->num_ref_idx_l0_active_minus1; ++i) {
                        sh->list_entry_l0[i] = br.getBitsWithFallback(entryBits, 0);
                    }
                }
                if (sh->slice_type == kSliceTypeB) {
                    sh->ref_pic_list_modification_flag_l1 = br.getBitsWithFallback(1, 0);
                    if (sh->ref_pic_list_modification_flag_l1) {
                        for (uint32_t i = 0; i <= sh->num_ref_idx_l1_active_minus1; ++i) {
                            sh->list_entry_l1[i] = br.getBitsWithFallback(entryBits, 0);
                        }
                    }
                }
            }

            if (sh->slice_type == kSliceTypeB) sh->mvd_l1_zero_flag = br.getBitsWithFallback(1, 0);
            if (pps.cabac_init_present_flag) sh->cabac_init_flag = br.getBitsWithFallback(1, 0);
            if (sh->slice_temporal_mvp_enabled_flag) {
                if (sh->slice_type == kSliceTypeB) {
                    sh->collocated_from_l0_flag = br.getBitsWithFallback(1, 1);
                }
                if ((sh->collocated_from_l0_flag && sh->num_ref_idx_l0_active_minus1 > 0) ||
                    (!sh->collocated_from_l0_flag && sh->num_ref_idx_l1_active_minus1 > 0)) {
                    if (!NalParser::parseUE(&br, &sh->collocated_ref_idx)) return false;
                }
            }

            if ((pps.weighted_pred_flag && sh->slice_type == kSliceTypeP) ||
                (pps.weighted_bipred_flag && sh->slice_type == kSliceTypeB)) {
                // pred_weight_table() (7.3.6.3).
                if (!NalParser::parseUE(&br, &sh->luma_log2_weight_denom)) return false;
                if (chromaArrayType != 0) {
                    if (!NalParser::parseSE(&br, &sh->delta_chroma_log2_weight_denom)) return false;
                }
                const int32_t chromaLog2Denom =
                        static_cast<int32_t>(sh->luma_log2_weight_denom) +
                        sh->delta_chroma_log2_weight_denom;
                const int numLists = (sh->slice_type == kSliceTypeB) ? 2 : 1;
                for (int l = 0; l < numLists; ++l) {
                    const uint32_t count = (l == 0 ? sh->num_ref_idx_l0_active_minus1
                                                   : sh->num_ref_idx_l1_active_minus1) + 1;
                    bool lumaFlag[16] = {};
                    bool chromaFlag[16] = {};
                    for (uint32_t i = 0; i < count; ++i) lumaFlag[i] = br.getBitsWithFallback(1, 0);
                    if (chromaArrayType != 0) {
                        for (uint32_t i = 0; i < count; ++i) {
                            chromaFlag[i] = br.getBitsWithFallback(1, 0);
                        }
                    }
                    for (uint32_t i = 0; i < count; ++i) {
                        if (lumaFlag[i]) {
                            if (!NalParser::parseSE(&br, &sh->delta_luma_weight[l][i])) return false;
                            if (!NalParser::parseSE(&br, &sh->luma_offset[l][i])) return false;
                        }
                        if (chromaFlag[i]) {
                            for (int j = 0; j < 2; ++j) {
                                int32_t deltaWeight = 0;
                                int32_t deltaOffset = 0;
                                if (!NalParser::parseSE(&br, &deltaWeight)) return false;
                                if (!NalParser::parseSE(&br, &deltaOffset)) return false;
                                sh->delta_chroma_weight[l][i][j] = deltaWeight;
                                // 7-56: VA wants the DERIVED ChromaOffsetLx.
                                const int32_t chromaWeight =
                                        (1 << chromaLog2Denom) + deltaWeight;
                                const int32_t shifted =
                                        chromaLog2Denom >= 0
                                                ? ((128 * chromaWeight) >> chromaLog2Denom)
                                                : 0;
                                sh->chroma_offset[l][i][j] =
                                        clip3(-128, 127, 128 + deltaOffset - shifted);
                            }
                        }
                    }
                }
            }
            if (!NalParser::parseUE(&br, &sh->five_minus_max_num_merge_cand)) return false;
        }

        if (!NalParser::parseSE(&br, &sh->slice_qp_delta)) return false;
        if (pps.pps_slice_chroma_qp_offsets_present_flag) {
            if (!NalParser::parseSE(&br, &sh->slice_cb_qp_offset)) return false;
            if (!NalParser::parseSE(&br, &sh->slice_cr_qp_offset)) return false;
        }

        sh->slice_deblocking_filter_disabled_flag = pps.pps_deblocking_filter_disabled_flag;
        sh->slice_beta_offset_div2 = pps.pps_beta_offset_div2;
        sh->slice_tc_offset_div2 = pps.pps_tc_offset_div2;
        bool deblockingOverride = false;
        if (pps.deblocking_filter_override_enabled_flag) {
            deblockingOverride = br.getBitsWithFallback(1, 0);
        }
        if (deblockingOverride) {
            sh->slice_deblocking_filter_disabled_flag = br.getBitsWithFallback(1, 0);
            if (!sh->slice_deblocking_filter_disabled_flag) {
                if (!NalParser::parseSE(&br, &sh->slice_beta_offset_div2)) return false;
                if (!NalParser::parseSE(&br, &sh->slice_tc_offset_div2)) return false;
            }
        }

        sh->slice_loop_filter_across_slices_enabled_flag =
                pps.pps_loop_filter_across_slices_enabled_flag;
        if (pps.pps_loop_filter_across_slices_enabled_flag &&
            (sh->slice_sao_luma_flag || sh->slice_sao_chroma_flag ||
             !sh->slice_deblocking_filter_disabled_flag)) {
            sh->slice_loop_filter_across_slices_enabled_flag = br.getBitsWithFallback(1, 0);
        }
    }

    if (pps.tiles_enabled_flag || pps.entropy_coding_sync_enabled_flag) {
        if (!NalParser::parseUE(&br, &sh->num_entry_point_offsets)) return false;
        if (sh->num_entry_point_offsets > 0) {
            uint32_t offsetLenMinus1 = 0;
            if (!NalParser::parseUE(&br, &offsetLenMinus1)) return false;
            if (offsetLenMinus1 > 31) return false;
            for (uint32_t i = 0; i < sh->num_entry_point_offsets; ++i) {
                br.skipBits(offsetLenMinus1 + 1);
            }
        }
    }
    if (pps.slice_segment_header_extension_present_flag) {
        uint32_t extLen = 0;
        if (!NalParser::parseUE(&br, &extLen)) return false;
        for (uint32_t i = 0; i < extLen; ++i) br.skipBits(8);
    }

    // byte_alignment(): alignment_bit_equal_to_one then zero bits to the byte
    // boundary. slice_data() starts at the next byte.
    br.skipBits(1);
    const size_t consumed = rbspTotalBits - br.numBitsLeft();
    const size_t headerBytesInRbsp = (consumed + 7) / 8;

    if (br.overRead()) {
        ALOGE("Truncated HEVC slice header.");
        return false;
    }

    // VA wants the offset measured in RBSP space (emulation-prevention bytes
    // removed) counting from and including the 2-byte NAL header, and separately
    // the number of EP bytes that were inside the header. origPos maps an RBSP
    // index back to its escaped index, so the EP bytes dropped before offset p
    // are exactly origPos[p] - p.
    sh->header_byte_size = static_cast<uint32_t>(2 + headerBytesInRbsp);
    if (headerBytesInRbsp < origPos.size()) {
        sh->num_emu_prevn_bytes =
                origPos[headerBytesInRbsp] - static_cast<uint32_t>(headerBytesInRbsp);
    } else if (!origPos.empty()) {
        sh->num_emu_prevn_bytes = static_cast<uint32_t>((size - 2) - origPos.size());
    }

    sh->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// POC / RPS / reference lists
// ---------------------------------------------------------------------------

void VaapiVideoDecoder::computePocHEVC(const H265SPS& sps, const H265SliceHeader& sh) {
    const int32_t maxPocLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    const bool noRaslOutputFlag =
            sh.idrPicFlag || isBlaNal(sh.nalUnitType) || mH265FirstPicture;

    int32_t pocMsb = 0;
    if (sh.irapPicFlag && noRaslOutputFlag) {
        pocMsb = 0;
    } else {
        const int32_t lsb = static_cast<int32_t>(sh.slice_pic_order_cnt_lsb);
        const int32_t prevLsb = mH265PrevTid0PocLsb;
        const int32_t prevMsb = mH265PrevTid0PocMsb;
        if (lsb < prevLsb && (prevLsb - lsb) >= maxPocLsb / 2) {
            pocMsb = prevMsb + maxPocLsb;
        } else if (lsb > prevLsb && (lsb - prevLsb) > maxPocLsb / 2) {
            pocMsb = prevMsb - maxPocLsb;
        } else {
            pocMsb = prevMsb;
        }
    }
    mH265CurrPoc = pocMsb + static_cast<int32_t>(sh.slice_pic_order_cnt_lsb);

    // prevTid0Pic: the most recent TemporalId 0 picture that is not a RASL, RADL
    // or sub-layer non-reference picture.
    if (sh.temporalId == 0 && !isRaslNal(sh.nalUnitType) && !isRadlNal(sh.nalUnitType) &&
        !isSlnrNal(sh.nalUnitType)) {
        mH265PrevTid0PocLsb = static_cast<int32_t>(sh.slice_pic_order_cnt_lsb);
        mH265PrevTid0PocMsb = pocMsb;
    }
}

void VaapiVideoDecoder::deriveRpsHEVC(const H265SPS& sps, const H265SliceHeader& sh) {
    mRpsStCurrBefore.clear();
    mRpsStCurrAfter.clear();
    mRpsLtCurr.clear();

    // HEVC has no MMCO: the RPS signalled by this slice is the complete
    // description of what stays a reference. Clear every mark and rebuild.
    for (const auto& e : mDpb) {
        e->refUsed = false;
        e->longTerm = false;
    }
    if (sh.idrPicFlag) return;  // an IDR has an empty RPS

    const int32_t maxPocLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    auto findByPoc = [this](int32_t poc) -> DpbEntry* {
        for (const auto& e : mDpb) {
            if (e->picOrderCnt == poc) return e.get();
        }
        return nullptr;
    };
    auto findByPocLsb = [this, maxPocLsb](int32_t pocLsb) -> DpbEntry* {
        for (const auto& e : mDpb) {
            if ((e->picOrderCnt & (maxPocLsb - 1)) == pocLsb) return e.get();
        }
        return nullptr;
    };

    const H265ShortTermRPS& rps = sh.st_rps;
    for (uint32_t i = 0; i < rps.num_negative_pics; ++i) {
        DpbEntry* e = findByPoc(mH265CurrPoc + rps.deltaPocS0[i]);
        if (!e) continue;
        e->refUsed = true;
        if (rps.usedByCurrS0[i]) mRpsStCurrBefore.push_back(e);
    }
    for (uint32_t i = 0; i < rps.num_positive_pics; ++i) {
        DpbEntry* e = findByPoc(mH265CurrPoc + rps.deltaPocS1[i]);
        if (!e) continue;
        e->refUsed = true;
        if (rps.usedByCurrS1[i]) mRpsStCurrAfter.push_back(e);
    }

    for (uint32_t i = 0; i < sh.num_long_term_sps + sh.num_long_term_pics; ++i) {
        DpbEntry* e = nullptr;
        if (sh.delta_poc_msb_present_flag[i]) {
            // 7-53: the full POC is recoverable, so match on it exactly.
            const int32_t poc = mH265CurrPoc -
                                static_cast<int32_t>(sh.delta_poc_msb_cycle_lt[i]) * maxPocLsb -
                                (mH265CurrPoc & (maxPocLsb - 1)) +
                                static_cast<int32_t>(sh.poc_lsb_lt[i]);
            e = findByPoc(poc);
        } else {
            e = findByPocLsb(static_cast<int32_t>(sh.poc_lsb_lt[i]));
        }
        if (!e) continue;
        e->longTerm = true;
        e->refUsed = false;
        if (sh.used_by_curr_pic_lt_flag[i]) mRpsLtCurr.push_back(e);
    }
}

void VaapiVideoDecoder::buildRefPicListsHEVC(const H265SliceHeader& sh,
                                             const std::vector<DpbEntry*>& refFrames,
                                             std::vector<uint8_t>* list0,
                                             std::vector<uint8_t>* list1) {
    list0->clear();
    list1->clear();
    if (sh.slice_type == kSliceTypeI) return;

    const size_t numPicTotalCurr =
            mRpsStCurrBefore.size() + mRpsStCurrAfter.size() + mRpsLtCurr.size();
    if (numPicTotalCurr == 0) {
        ALOGW("HEVC P/B slice with an empty RPS; skipping reference lists.");
        return;
    }

    auto indexOf = [&refFrames](const DpbEntry* e) -> uint8_t {
        for (size_t i = 0; i < refFrames.size(); ++i) {
            if (refFrames[i] == e) return static_cast<uint8_t>(i);
        }
        return 0;
    };

    // 8.3.4: cycle the three RPS groups until the temp list is long enough, then
    // either take it directly or permute it with the signalled list_entry.
    auto build = [&](bool afterFirst, size_t numActive, bool modFlag, const uint32_t* listEntry,
                     std::vector<uint8_t>* out) {
        std::vector<DpbEntry*> temp;
        const size_t want = std::max(numActive, numPicTotalCurr);
        while (temp.size() < want) {
            const std::vector<DpbEntry*>& first =
                    afterFirst ? mRpsStCurrAfter : mRpsStCurrBefore;
            const std::vector<DpbEntry*>& second =
                    afterFirst ? mRpsStCurrBefore : mRpsStCurrAfter;
            for (DpbEntry* e : first) temp.push_back(e);
            for (DpbEntry* e : second) temp.push_back(e);
            for (DpbEntry* e : mRpsLtCurr) temp.push_back(e);
        }
        for (size_t i = 0; i < numActive && i < 15; ++i) {
            size_t idx = i;
            if (modFlag) {
                idx = listEntry[i];
                if (idx >= temp.size()) idx = 0;
            }
            out->push_back(indexOf(temp[idx]));
        }
    };

    build(false, sh.num_ref_idx_l0_active_minus1 + 1, sh.ref_pic_list_modification_flag_l0,
          sh.list_entry_l0, list0);
    if (sh.slice_type == kSliceTypeB) {
        build(true, sh.num_ref_idx_l1_active_minus1 + 1, sh.ref_pic_list_modification_flag_l1,
              sh.list_entry_l1, list1);
    }
}

// ---------------------------------------------------------------------------
// VA context + picture submission
// ---------------------------------------------------------------------------

bool VaapiVideoDecoder::ensureVAContextHEVC(const H265SPS& sps) {
    const int codedW = static_cast<int>(sps.pic_width_in_luma_samples);
    const int codedH = static_cast<int>(sps.pic_height_in_luma_samples);

    // Conformance-window offsets are in chroma units (Table 6-1).
    int subW = 1;
    int subH = 1;
    if (sps.chroma_format_idc == 1) {
        subW = 2;
        subH = 2;
    } else if (sps.chroma_format_idc == 2) {
        subW = 2;
    }
    const Rect visible(static_cast<int>(sps.conf_win_left_offset) * subW,
                       static_cast<int>(sps.conf_win_top_offset) * subH,
                       codedW - static_cast<int>(sps.conf_win_right_offset) * subW,
                       codedH - static_cast<int>(sps.conf_win_bottom_offset) * subH);

    const size_t dpbMaxSize =
            std::min<size_t>(16, static_cast<size_t>(sps.sps_max_dec_pic_buffering_minus1) + 2);
    return ensureVAContextForGeometry(ui::Size(codedW, codedH), visible, dpbMaxSize);
}

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeFrameHEVC(
        int32_t bitstreamId, const H265SPS& sps, const H265PPS& pps,
        const std::vector<H265SliceHeader>& slices) {
    ATRACE_CALL();

    if (!ensureVAContextHEVC(sps)) return FrameResult::kError;

    const H265SliceHeader& first = slices.front();

    // P/B slices with a reference-free DPB (stream joined mid-GOP, or resumed
    // after a flush on a non-IRAP picture) are skipped rather than decoded
    // from VA_INVALID_SURFACE references — the same guard the RASL drop below
    // applies to broken-link pictures the stream itself marks.
    if (!first.irapPicFlag) {
        bool needsRefs = false;
        for (const H265SliceHeader& sh : slices) {
            if (sh.slice_type != kSliceTypeI) {
                needsRefs = true;
                break;
            }
        }
        if (needsRefs) {
            bool haveRefs = false;
            for (const auto& e : mDpb) {
                if (e->refUsed || e->longTerm) {
                    haveRefs = true;
                    break;
                }
            }
            if (!haveRefs) {
                noteMissingRefSkip("HEVC P/B frame with an empty DPB");
                return FrameResult::kSkipped;
            }
        }
    }
    noteMissingRefRecovery("HEVC");

    const bool noRaslOutputFlag =
            first.idrPicFlag || isBlaNal(first.nalUnitType) || mH265FirstPicture;

    if (first.irapPicFlag) {
        if (noRaslOutputFlag) {
            // A new prediction chain starts here: emit everything buffered and
            // drop all references before decoding.
            flushDpb();
        }
        mH265SkipRasl = noRaslOutputFlag;
    }

    computePocHEVC(sps, first);

    VASurfaceID target = acquireFreeSurface();
    if (target == VA_INVALID_SURFACE) {
        // Back-pressure. Everything above is idempotent, so the access unit is
        // simply retried once an output completes.
        return FrameResult::kNoSurface;
    }

    deriveRpsHEVC(sps, first);

    // The VA reference array: every picture still marked as a reference, plus
    // the RPS category flags the driver needs.
    std::vector<DpbEntry*> refFrames;
    for (const auto& e : mDpb) {
        if (e->refUsed || e->longTerm) refFrames.push_back(e.get());
        if (refFrames.size() >= 15) break;
    }

    VADisplay dpy = mDevice->display();
    VAStatus status = vaBeginPicture(dpy, mVAContext, target);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaBeginPicture failed: %s", vaErrorStr(status));
        return FrameResult::kError;
    }

    VAPictureParameterBufferHEVC pic;
    memset(&pic, 0, sizeof(pic));
    pic.CurrPic.picture_id = target;
    pic.CurrPic.pic_order_cnt = mH265CurrPoc;
    pic.CurrPic.flags = 0;

    for (size_t i = 0; i < 15; ++i) {
        if (i < refFrames.size()) {
            const DpbEntry* e = refFrames[i];
            pic.ReferenceFrames[i].picture_id = e->surface;
            pic.ReferenceFrames[i].pic_order_cnt = e->picOrderCnt;
            uint32_t flags = 0;
            if (e->longTerm) flags |= VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
            for (const DpbEntry* r : mRpsStCurrBefore) {
                if (r == e) flags |= VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
            }
            for (const DpbEntry* r : mRpsStCurrAfter) {
                if (r == e) flags |= VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
            }
            for (const DpbEntry* r : mRpsLtCurr) {
                if (r == e) flags |= VA_PICTURE_HEVC_RPS_LT_CURR;
            }
            pic.ReferenceFrames[i].flags = flags;
        } else {
            pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            pic.ReferenceFrames[i].pic_order_cnt = 0;
            pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        }
    }

    pic.pic_width_in_luma_samples = static_cast<uint16_t>(sps.pic_width_in_luma_samples);
    pic.pic_height_in_luma_samples = static_cast<uint16_t>(sps.pic_height_in_luma_samples);

    pic.pic_fields.bits.chroma_format_idc = sps.chroma_format_idc;
    pic.pic_fields.bits.separate_colour_plane_flag = sps.separate_colour_plane_flag;
    pic.pic_fields.bits.pcm_enabled_flag = sps.pcm_enabled_flag;
    pic.pic_fields.bits.scaling_list_enabled_flag = sps.scaling_list_enabled_flag;
    pic.pic_fields.bits.transform_skip_enabled_flag = pps.transform_skip_enabled_flag;
    pic.pic_fields.bits.amp_enabled_flag = sps.amp_enabled_flag;
    pic.pic_fields.bits.strong_intra_smoothing_enabled_flag =
            sps.strong_intra_smoothing_enabled_flag;
    pic.pic_fields.bits.sign_data_hiding_enabled_flag = pps.sign_data_hiding_enabled_flag;
    pic.pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra_pred_flag;
    pic.pic_fields.bits.cu_qp_delta_enabled_flag = pps.cu_qp_delta_enabled_flag;
    pic.pic_fields.bits.weighted_pred_flag = pps.weighted_pred_flag;
    pic.pic_fields.bits.weighted_bipred_flag = pps.weighted_bipred_flag;
    pic.pic_fields.bits.transquant_bypass_enabled_flag = pps.transquant_bypass_enabled_flag;
    pic.pic_fields.bits.tiles_enabled_flag = pps.tiles_enabled_flag;
    pic.pic_fields.bits.entropy_coding_sync_enabled_flag = pps.entropy_coding_sync_enabled_flag;
    pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag =
            pps.pps_loop_filter_across_slices_enabled_flag;
    pic.pic_fields.bits.loop_filter_across_tiles_enabled_flag =
            pps.loop_filter_across_tiles_enabled_flag;
    pic.pic_fields.bits.pcm_loop_filter_disabled_flag = sps.pcm_loop_filter_disabled_flag;
    pic.pic_fields.bits.NoPicReorderingFlag = (sps.sps_max_num_reorder_pics == 0) ? 1 : 0;

    pic.sps_max_dec_pic_buffering_minus1 =
            static_cast<uint8_t>(sps.sps_max_dec_pic_buffering_minus1);
    pic.bit_depth_luma_minus8 = static_cast<uint8_t>(sps.bit_depth_luma_minus8);
    pic.bit_depth_chroma_minus8 = static_cast<uint8_t>(sps.bit_depth_chroma_minus8);
    pic.pcm_sample_bit_depth_luma_minus1 =
            static_cast<uint8_t>(sps.pcm_sample_bit_depth_luma_minus1);
    pic.pcm_sample_bit_depth_chroma_minus1 =
            static_cast<uint8_t>(sps.pcm_sample_bit_depth_chroma_minus1);
    pic.log2_min_luma_coding_block_size_minus3 =
            static_cast<uint8_t>(sps.log2_min_luma_coding_block_size_minus3);
    pic.log2_diff_max_min_luma_coding_block_size =
            static_cast<uint8_t>(sps.log2_diff_max_min_luma_coding_block_size);
    pic.log2_min_transform_block_size_minus2 =
            static_cast<uint8_t>(sps.log2_min_transform_block_size_minus2);
    pic.log2_diff_max_min_transform_block_size =
            static_cast<uint8_t>(sps.log2_diff_max_min_transform_block_size);
    pic.log2_min_pcm_luma_coding_block_size_minus3 =
            static_cast<uint8_t>(sps.log2_min_pcm_luma_coding_block_size_minus3);
    pic.log2_diff_max_min_pcm_luma_coding_block_size =
            static_cast<uint8_t>(sps.log2_diff_max_min_pcm_luma_coding_block_size);
    pic.max_transform_hierarchy_depth_intra =
            static_cast<uint8_t>(sps.max_transform_hierarchy_depth_intra);
    pic.max_transform_hierarchy_depth_inter =
            static_cast<uint8_t>(sps.max_transform_hierarchy_depth_inter);
    pic.init_qp_minus26 = static_cast<int8_t>(pps.init_qp_minus26);
    pic.diff_cu_qp_delta_depth = static_cast<uint8_t>(pps.diff_cu_qp_delta_depth);
    pic.pps_cb_qp_offset = static_cast<int8_t>(pps.pps_cb_qp_offset);
    pic.pps_cr_qp_offset = static_cast<int8_t>(pps.pps_cr_qp_offset);
    pic.log2_parallel_merge_level_minus2 =
            static_cast<uint8_t>(pps.log2_parallel_merge_level_minus2);
    pic.num_tile_columns_minus1 = static_cast<uint8_t>(pps.num_tile_columns_minus1);
    pic.num_tile_rows_minus1 = static_cast<uint8_t>(pps.num_tile_rows_minus1);
    if (pps.tiles_enabled_flag && !pps.uniform_spacing_flag) {
        for (uint32_t i = 0; i < pps.num_tile_columns_minus1 && i < 19; ++i) {
            pic.column_width_minus1[i] = static_cast<uint16_t>(pps.column_width_minus1[i]);
        }
        for (uint32_t i = 0; i < pps.num_tile_rows_minus1 && i < 21; ++i) {
            pic.row_height_minus1[i] = static_cast<uint16_t>(pps.row_height_minus1[i]);
        }
    }

    pic.slice_parsing_fields.bits.lists_modification_present_flag =
            pps.lists_modification_present_flag;
    pic.slice_parsing_fields.bits.long_term_ref_pics_present_flag =
            sps.long_term_ref_pics_present_flag;
    pic.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag =
            sps.sps_temporal_mvp_enabled_flag;
    pic.slice_parsing_fields.bits.cabac_init_present_flag = pps.cabac_init_present_flag;
    pic.slice_parsing_fields.bits.output_flag_present_flag = pps.output_flag_present_flag;
    pic.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag =
            pps.dependent_slice_segments_enabled_flag;
    pic.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag =
            pps.pps_slice_chroma_qp_offsets_present_flag;
    pic.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag =
            sps.sample_adaptive_offset_enabled_flag;
    pic.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag =
            pps.deblocking_filter_override_enabled_flag;
    pic.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag =
            pps.pps_deblocking_filter_disabled_flag;
    pic.slice_parsing_fields.bits.slice_segment_header_extension_present_flag =
            pps.slice_segment_header_extension_present_flag;
    pic.slice_parsing_fields.bits.RapPicFlag = first.irapPicFlag ? 1 : 0;
    pic.slice_parsing_fields.bits.IdrPicFlag = first.idrPicFlag ? 1 : 0;
    pic.slice_parsing_fields.bits.IntraPicFlag = first.irapPicFlag ? 1 : 0;

    pic.log2_max_pic_order_cnt_lsb_minus4 =
            static_cast<uint8_t>(sps.log2_max_pic_order_cnt_lsb_minus4);
    pic.num_short_term_ref_pic_sets = static_cast<uint8_t>(sps.num_short_term_ref_pic_sets);
    pic.num_long_term_ref_pic_sps = static_cast<uint8_t>(sps.num_long_term_ref_pics_sps);
    pic.num_ref_idx_l0_default_active_minus1 =
            static_cast<uint8_t>(pps.num_ref_idx_l0_default_active_minus1);
    pic.num_ref_idx_l1_default_active_minus1 =
            static_cast<uint8_t>(pps.num_ref_idx_l1_default_active_minus1);
    pic.pps_beta_offset_div2 = static_cast<int8_t>(pps.pps_beta_offset_div2);
    pic.pps_tc_offset_div2 = static_cast<int8_t>(pps.pps_tc_offset_div2);
    pic.num_extra_slice_header_bits = static_cast<uint8_t>(pps.num_extra_slice_header_bits);
    pic.st_rps_bits = first.st_rps_bits;

    std::vector<VABufferID> vaBuffers;
    auto createBuffer = [&](VABufferType type, unsigned int sz, void* payload) -> bool {
        VABufferID id = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(dpy, mVAContext, type, sz, 1, payload, &id);
        if (s != VA_STATUS_SUCCESS) {
            ALOGE("vaCreateBuffer(type=%d) failed: %s", type, vaErrorStr(s));
            return false;
        }
        vaBuffers.push_back(id);
        return true;
    };
    auto bail = [&]() {
        for (VABufferID b : vaBuffers) vaDestroyBuffer(dpy, b);
        return FrameResult::kError;
    };

    if (!createBuffer(VAPictureParameterBufferType, sizeof(pic), &pic)) return bail();

    // Scaling lists: the PPS overrides the SPS when it carries its own.
    if (sps.scaling_list_enabled_flag) {
        const H265ScalingList& sl =
                pps.pps_scaling_list_data_present_flag ? pps.scaling_list : sps.scaling_list;
        VAIQMatrixBufferHEVC iq;
        memset(&iq, 0, sizeof(iq));
        memcpy(iq.ScalingList4x4, sl.list4x4, sizeof(iq.ScalingList4x4));
        memcpy(iq.ScalingList8x8, sl.list8x8, sizeof(iq.ScalingList8x8));
        memcpy(iq.ScalingList16x16, sl.list16x16, sizeof(iq.ScalingList16x16));
        memcpy(iq.ScalingList32x32, sl.list32x32, sizeof(iq.ScalingList32x32));
        memcpy(iq.ScalingListDC16x16, sl.dc16x16, sizeof(iq.ScalingListDC16x16));
        memcpy(iq.ScalingListDC32x32, sl.dc32x32, sizeof(iq.ScalingListDC32x32));
        if (!createBuffer(VAIQMatrixBufferType, sizeof(iq), &iq)) return bail();
    }

    for (size_t s = 0; s < slices.size(); ++s) {
        const H265SliceHeader& sh = slices[s];
        std::vector<uint8_t> list0;
        std::vector<uint8_t> list1;
        buildRefPicListsHEVC(sh, refFrames, &list0, &list1);

        VASliceParameterBufferHEVC sp;
        memset(&sp, 0, sizeof(sp));
        sp.slice_data_size = static_cast<uint32_t>(sh.nalSize);
        sp.slice_data_offset = 0;
        sp.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        sp.slice_data_byte_offset = sh.header_byte_size;
        sp.slice_data_num_emu_prevn_bytes = static_cast<uint16_t>(sh.num_emu_prevn_bytes);
        sp.slice_segment_address = sh.slice_segment_address;

        memset(sp.RefPicList, 0xff, sizeof(sp.RefPicList));
        for (size_t i = 0; i < list0.size() && i < 15; ++i) sp.RefPicList[0][i] = list0[i];
        for (size_t i = 0; i < list1.size() && i < 15; ++i) sp.RefPicList[1][i] = list1[i];

        sp.LongSliceFlags.fields.LastSliceOfPic = (s + 1 == slices.size()) ? 1 : 0;
        sp.LongSliceFlags.fields.dependent_slice_segment_flag = sh.dependent_slice_segment_flag;
        sp.LongSliceFlags.fields.slice_type = sh.slice_type;
        sp.LongSliceFlags.fields.color_plane_id = sh.colour_plane_id;
        sp.LongSliceFlags.fields.slice_sao_luma_flag = sh.slice_sao_luma_flag;
        sp.LongSliceFlags.fields.slice_sao_chroma_flag = sh.slice_sao_chroma_flag;
        sp.LongSliceFlags.fields.mvd_l1_zero_flag = sh.mvd_l1_zero_flag;
        sp.LongSliceFlags.fields.cabac_init_flag = sh.cabac_init_flag;
        sp.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag =
                sh.slice_temporal_mvp_enabled_flag;
        sp.LongSliceFlags.fields.slice_deblocking_filter_disabled_flag =
                sh.slice_deblocking_filter_disabled_flag;
        sp.LongSliceFlags.fields.collocated_from_l0_flag = sh.collocated_from_l0_flag;
        sp.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag =
                sh.slice_loop_filter_across_slices_enabled_flag;

        sp.collocated_ref_idx =
                sh.slice_temporal_mvp_enabled_flag ? static_cast<uint8_t>(sh.collocated_ref_idx)
                                                   : 0xff;
        sp.num_ref_idx_l0_active_minus1 = static_cast<uint8_t>(sh.num_ref_idx_l0_active_minus1);
        sp.num_ref_idx_l1_active_minus1 = static_cast<uint8_t>(sh.num_ref_idx_l1_active_minus1);
        sp.slice_qp_delta = static_cast<int8_t>(sh.slice_qp_delta);
        sp.slice_cb_qp_offset = static_cast<int8_t>(sh.slice_cb_qp_offset);
        sp.slice_cr_qp_offset = static_cast<int8_t>(sh.slice_cr_qp_offset);
        sp.slice_beta_offset_div2 = static_cast<int8_t>(sh.slice_beta_offset_div2);
        sp.slice_tc_offset_div2 = static_cast<int8_t>(sh.slice_tc_offset_div2);
        sp.luma_log2_weight_denom = static_cast<uint8_t>(sh.luma_log2_weight_denom);
        sp.delta_chroma_log2_weight_denom =
                static_cast<int8_t>(sh.delta_chroma_log2_weight_denom);
        for (int i = 0; i < 15; ++i) {
            sp.delta_luma_weight_l0[i] = static_cast<int8_t>(sh.delta_luma_weight[0][i]);
            sp.luma_offset_l0[i] = static_cast<int8_t>(sh.luma_offset[0][i]);
            sp.delta_luma_weight_l1[i] = static_cast<int8_t>(sh.delta_luma_weight[1][i]);
            sp.luma_offset_l1[i] = static_cast<int8_t>(sh.luma_offset[1][i]);
            for (int j = 0; j < 2; ++j) {
                sp.delta_chroma_weight_l0[i][j] =
                        static_cast<int8_t>(sh.delta_chroma_weight[0][i][j]);
                sp.ChromaOffsetL0[i][j] = static_cast<int8_t>(sh.chroma_offset[0][i][j]);
                sp.delta_chroma_weight_l1[i][j] =
                        static_cast<int8_t>(sh.delta_chroma_weight[1][i][j]);
                sp.ChromaOffsetL1[i][j] = static_cast<int8_t>(sh.chroma_offset[1][i][j]);
            }
        }
        sp.five_minus_max_num_merge_cand =
                static_cast<uint8_t>(sh.five_minus_max_num_merge_cand);
        sp.num_entry_point_offsets = static_cast<uint16_t>(sh.num_entry_point_offsets);
        sp.entry_offset_to_subset_array = 0;

        if (!createBuffer(VASliceParameterBufferType, sizeof(sp), &sp)) return bail();
        if (!createBuffer(VASliceDataBufferType, static_cast<unsigned int>(sh.nalSize),
                          const_cast<uint8_t*>(sh.nalData))) {
            return bail();
        }
    }

    status = vaRenderPicture(dpy, mVAContext, vaBuffers.data(),
                             static_cast<int>(vaBuffers.size()));
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaRenderPicture failed: %s", vaErrorStr(status));
        return bail();
    }
    status = vaEndPicture(dpy, mVAContext);
    for (VABufferID b : vaBuffers) vaDestroyBuffer(dpy, b);
    if (status != VA_STATUS_SUCCESS) {
        ALOGE("vaEndPicture failed: %s", vaErrorStr(status));
        return FrameResult::kError;
    }

    status = vaSyncSurface(dpy, target);
    if (status != VA_STATUS_SUCCESS) {
        ALOGW("vaSyncSurface failed: %s", vaErrorStr(status));
    }

    // Insert into the DPB. Every decoded picture is a potential reference until
    // a later RPS says otherwise (deriveRpsHEVC re-marks the whole DPB), except
    // sub-layer non-reference pictures, which nothing may reference.
    auto entry = std::make_unique<DpbEntry>();
    entry->surface = target;
    entry->bitstreamId = bitstreamId;
    entry->picOrderCnt = mH265CurrPoc;
    entry->topFieldOrderCnt = mH265CurrPoc;
    entry->bottomFieldOrderCnt = mH265CurrPoc;
    entry->refUsed = !isSlnrNal(first.nalUnitType);
    entry->longTerm = false;
    entry->codedSize = mCodedSize;
    entry->visibleRect = mVisibleRect;
    // pic_output_flag == 0 means "decode as a reference but never display".
    entry->outputted = !first.pic_output_flag;
    mDpb.push_back(std::move(entry));

    mH265FirstPicture = false;

    bumpAndOutputAsNeeded(std::max<size_t>(sps.sps_max_num_reorder_pics, 1));
    reclaimDpbEntries();
    return FrameResult::kOk;
}

// ---------------------------------------------------------------------------
// HEVC access-unit decode
// ---------------------------------------------------------------------------

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeHEVCBuffer(int32_t bitstreamId,
                                                                    const uint8_t* data,
                                                                    size_t size) {
    ATRACE_CALL();

    // One access unit (one picture) per input buffer, same contract as the
    // H.264 path: parameter sets update our stores, slice NALs accumulate.
    HEVCNalParser parser(data, size);

    std::vector<H265SliceHeader> slices;
    const H265SPS* activeSps = nullptr;
    const H265PPS* activePps = nullptr;
    bool droppedRasl = false;

    while (parser.locateNextNal()) {
        const uint8_t* nal = parser.data();
        const size_t nalSize = parser.length();
        if (nalSize < 3) continue;
        const uint8_t nalType = static_cast<uint8_t>((nal[0] >> 1) & 0x3f);

        if (nalType == kNalVps) continue;  // nothing in the VPS is needed for VA

        if (nalType == kNalSps) {
            H265SPS sps;
            if (!parseH265SPS(nal, nalSize, &sps)) {
                ALOGE("Failed to parse HEVC SPS.");
                return FrameResult::kError;
            }
            mH265SPSes[sps.sps_seq_parameter_set_id] = sps;
            continue;
        }
        if (nalType == kNalPps) {
            H265PPS pps;
            if (!parseH265PPS(nal, nalSize, &pps)) {
                ALOGE("Failed to parse HEVC PPS.");
                return FrameResult::kError;
            }
            mH265PPSes[pps.pps_pic_parameter_set_id] = pps;
            continue;
        }
        if (!isSliceNal(nalType)) continue;  // AUD / SEI / EOS / filler

        // Drop the RASL pictures that follow a broken link — their references
        // were never decoded.
        if (isRaslNal(nalType) && mH265SkipRasl) {
            ALOGV("Skipping RASL picture after a broken link.");
            droppedRasl = true;
            continue;
        }

        H265SliceHeader sh;
        if (!parseH265SliceHeader(nal, nalSize, &sh)) {
            ALOGE("Failed to parse HEVC slice header.");
            return FrameResult::kError;
        }
        if (sh.dependent_slice_segment_flag && !slices.empty()) {
            // A dependent slice segment inherits the whole header from the
            // preceding independent one; only its own addressing and payload
            // geometry are its own.
            const H265SliceHeader prev = slices.back();
            const uint32_t addr = sh.slice_segment_address;
            const uint32_t hdrBytes = sh.header_byte_size;
            const uint32_t emu = sh.num_emu_prevn_bytes;
            const uint32_t entryPoints = sh.num_entry_point_offsets;
            const uint8_t* nalPtr = sh.nalData;
            const size_t nalLen = sh.nalSize;
            sh = prev;
            sh.dependent_slice_segment_flag = true;
            sh.first_slice_segment_in_pic_flag = false;
            sh.slice_segment_address = addr;
            sh.header_byte_size = hdrBytes;
            sh.num_emu_prevn_bytes = emu;
            sh.num_entry_point_offsets = entryPoints;
            sh.nalData = nalPtr;
            sh.nalSize = nalLen;
        }

        auto ppsIt = mH265PPSes.find(sh.slice_pic_parameter_set_id);
        if (ppsIt == mH265PPSes.end()) return FrameResult::kError;
        auto spsIt = mH265SPSes.find(ppsIt->second.pps_seq_parameter_set_id);
        if (spsIt == mH265SPSes.end()) return FrameResult::kError;
        activePps = &ppsIt->second;
        activeSps = &spsIt->second;
        slices.push_back(sh);
    }

    if (slices.empty()) {
        // A buffer that was all dropped RASL pictures completes through the
        // drop path (kOk would leave its work waiting for an output forever);
        // a genuine CSD-only (VPS/SPS/PPS) buffer completes via the
        // component's CODEC_CONFIG branch.
        return droppedRasl ? FrameResult::kSkipped : FrameResult::kOk;
    }
    return decodeFrameHEVC(bitstreamId, *activeSps, *activePps, slices);
}

}  // namespace android
