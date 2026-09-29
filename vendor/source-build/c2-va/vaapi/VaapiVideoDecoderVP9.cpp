// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// VP9 stateless decode core (remora-e5x.22).
//
// VP9 has no NAL layer: each input buffer is one temporal unit — either a
// single frame or a superframe (several frames back to back with a trailing
// index; typically an invisible alt-ref plus the shown frame). Only the
// UNCOMPRESSED header is parsed here; the compressed header and tile data go
// to the driver verbatim (the VA slice buffer spans the whole frame, with
// frame_header_length_in_bytes/first_partition_size telling it the split).
//
// Reference model: eight slots, refreshed per frame by refresh_frame_flags.
// Slots are non-owning pointers into the shared mDpb; an entry's refUsed flag
// is recomputed after every refresh so the generic reclaim/acquire machinery
// (acquireFreeSurface scans the DPB + pending outputs) needs no VP9 cases.
// There is no reordering: shown frames are queued for output immediately,
// invisible ones are marked outputted at birth so reclaim can free them the
// moment no slot references them.

#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <c2_va/vaapi/VaapiVideoDecoder.h>

#include <string.h>

#include <algorithm>

#include <log/log.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <utils/Trace.h>
#include <va/va.h>
#include <va/va_dec_vp9.h>

namespace android {

namespace {

constexpr uint32_t kVp9FrameMarker = 2;
constexpr uint32_t kVp9SyncCode = 0x498342;
constexpr int kVp9NumRefSlots = 8;
constexpr int kVp9RefsPerFrame = 3;
constexpr int kVp9MaxSegments = 8;
constexpr int kVp9SegLvlAltQ = 0;
constexpr int kVp9SegLvlAltL = 1;
constexpr int kVp9SegLvlRefFrame = 2;
constexpr int kVp9SegLvlSkip = 3;
constexpr int kVp9MaxLoopFilter = 63;

// Per-feature payload bits and signedness (spec 6.2.4: seg_feature_data).
constexpr int kSegFeatureBits[4] = {8, 6, 2, 0};
constexpr bool kSegFeatureSigned[4] = {true, true, false, false};

// 8-bit dequant lookup tables, verbatim from libvpx vp9/common/vp9_quant_common.c.
constexpr int16_t kDcQLookup[256] = {
    4,    8,    8,    9,    10,  11,  12,  12,  13,  14,  15,   16,   17,   18,
    19,   19,   20,   21,   22,  23,  24,  25,  26,  26,  27,   28,   29,   30,
    31,   32,   32,   33,   34,  35,  36,  37,  38,  38,  39,   40,   41,   42,
    43,   43,   44,   45,   46,  47,  48,  48,  49,  50,  51,   52,   53,   53,
    54,   55,   56,   57,   57,  58,  59,  60,  61,  62,  62,   63,   64,   65,
    66,   66,   67,   68,   69,  70,  70,  71,  72,  73,  74,   74,   75,   76,
    77,   78,   78,   79,   80,  81,  81,  82,  83,  84,  85,   85,   87,   88,
    90,   92,   93,   95,   96,  98,  99,  101, 102, 104, 105,  107,  108,  110,
    111,  113,  114,  116,  117, 118, 120, 121, 123, 125, 127,  129,  131,  134,
    136,  138,  140,  142,  144, 146, 148, 150, 152, 154, 156,  158,  161,  164,
    166,  169,  172,  174,  177, 180, 182, 185, 187, 190, 192,  195,  199,  202,
    205,  208,  211,  214,  217, 220, 223, 226, 230, 233, 237,  240,  243,  247,
    250,  253,  257,  261,  265, 269, 272, 276, 280, 284, 288,  292,  296,  300,
    304,  309,  313,  317,  322, 326, 330, 335, 340, 344, 349,  354,  359,  364,
    369,  374,  379,  384,  389, 395, 400, 406, 411, 417, 423,  429,  435,  441,
    447,  454,  461,  467,  475, 482, 489, 497, 505, 513, 522,  530,  539,  549,
    559,  569,  579,  590,  602, 614, 626, 640, 654, 668, 684,  700,  717,  736,
    755,  775,  796,  819,  843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139,
    1184, 1232, 1282, 1336,
};
constexpr int16_t kAcQLookup[256] = {
    4,    8,    9,    10,   11,   12,   13,   14,   15,   16,   17,   18,   19,
    20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,
    33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,   45,
    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,
    59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   69,   70,   71,
    72,   73,   74,   75,   76,   77,   78,   79,   80,   81,   82,   83,   84,
    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
    98,   99,   100,  101,  102,  104,  106,  108,  110,  112,  114,  116,  118,
    120,  122,  124,  126,  128,  130,  132,  134,  136,  138,  140,  142,  144,
    146,  148,  150,  152,  155,  158,  161,  164,  167,  170,  173,  176,  179,
    182,  185,  188,  191,  194,  197,  200,  203,  207,  211,  215,  219,  223,
    227,  231,  235,  239,  243,  247,  251,  255,  260,  265,  270,  275,  280,
    285,  290,  295,  300,  305,  311,  317,  323,  329,  335,  341,  347,  353,
    359,  366,  373,  380,  387,  394,  401,  408,  416,  424,  432,  440,  448,
    456,  465,  474,  483,  492,  501,  510,  520,  530,  540,  550,  560,  571,
    582,  593,  604,  615,  627,  639,  651,  663,  676,  689,  702,  715,  729,
    743,  757,  771,  786,  801,  816,  832,  848,  864,  881,  898,  915,  933,
    951,  969,  988,  1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196,
    1219, 1243, 1267, 1292, 1317, 1343, 1369, 1396, 1423, 1451, 1479, 1508, 1537,
    1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828,
};

// All reads go through the graceful API: plain ABitReader::getBits CHECK-
// aborts the whole HAL process on overread, and this parses whatever bytes an
// app queued. A truncated header reads as zeros and trips overRead(), checked
// once at the end.
uint32_t rd(ABitReader* br, size_t n) {
    return br->getBitsWithFallback(n, 0);
}

// f(n) then a sign bit (spec su(n)): sign-magnitude, NOT two's complement.
int32_t readSignedLiteral(ABitReader* br, int bits) {
    const int32_t value = static_cast<int32_t>(rd(br, bits));
    return rd(br, 1) ? -value : value;
}

int32_t clampInt(int32_t v, int32_t lo, int32_t hi) {
    return std::min(std::max(v, lo), hi);
}

// literal-to-filter map (spec 6.2.3 read_interpolation_filter).
constexpr uint8_t kLiteralToFilter[4] = {1 /*SMOOTH*/, 0 /*EIGHTTAP*/, 2 /*SHARP*/,
                                         3 /*BILINEAR*/};

// Trailing superframe index: marker byte 0b110 in the top bits, repeated at
// both ends of the index (spec Annex B). Returns each frame's [offset, size).
struct SuperframeSpan {
    size_t offset;
    size_t size;
};
bool splitSuperframe(const uint8_t* data, size_t size, std::vector<SuperframeSpan>* out) {
    out->clear();
    if (size == 0) return false;
    const uint8_t marker = data[size - 1];
    if ((marker & 0xe0) == 0xc0) {
        const int bytesPerSize = ((marker >> 3) & 0x3) + 1;
        const int frameCount = (marker & 0x7) + 1;
        const size_t indexSize = 2 + static_cast<size_t>(bytesPerSize) * frameCount;
        if (size >= indexSize && data[size - indexSize] == marker) {
            const uint8_t* idx = data + size - indexSize + 1;
            size_t offset = 0;
            for (int f = 0; f < frameCount; ++f) {
                uint32_t frameSize = 0;
                for (int b = 0; b < bytesPerSize; ++b) {
                    frameSize |= static_cast<uint32_t>(*idx++) << (8 * b);  // little-endian
                }
                if (offset + frameSize > size - indexSize) {
                    ALOGE("Superframe index overruns the buffer (frame %d).", f);
                    return false;
                }
                out->push_back({offset, frameSize});
                offset += frameSize;
            }
            return true;
        }
    }
    out->push_back({0, size});
    return true;
}

}  // namespace

// One frame's worth of uncompressed-header state, plus the split points the
// VA buffers need.
struct VaapiVideoDecoder::Vp9FrameHeader {
    uint8_t profile = 0;
    bool showExistingFrame = false;
    uint8_t frameToShowMapIdx = 0;

    bool isKeyFrame = false;
    bool showFrame = false;
    bool errorResilient = false;
    bool intraOnly = false;
    uint8_t resetFrameContext = 0;
    uint8_t refreshFrameFlags = 0;
    uint8_t refFrameIdx[kVp9RefsPerFrame] = {0, 0, 0};
    uint8_t refFrameSignBias[kVp9RefsPerFrame] = {0, 0, 0};
    bool allowHighPrecisionMv = false;
    uint8_t interpFilter = 0;  // VP9-code enum: 0 EIGHTTAP .. 4 SWITCHABLE
    bool refreshFrameContext = false;
    bool frameParallelDecoding = true;
    uint8_t frameContextIdx = 0;

    uint32_t width = 0;
    uint32_t height = 0;

    // quantization_params
    uint8_t baseQIdx = 0;
    int32_t deltaQYDc = 0;
    int32_t deltaQUvDc = 0;
    int32_t deltaQUvAc = 0;

    // tile_info
    uint8_t tileColsLog2 = 0;
    uint8_t tileRowsLog2 = 0;

    uint32_t uncompressedHeaderBytes = 0;  // frame_header_length_in_bytes
    uint16_t compressedHeaderBytes = 0;    // first_partition_size

    // Set when the header could not be fully parsed because it derives state
    // (frame_size_with_refs) from a reference slot that holds nothing — the
    // caller skips such frames instead of treating them as stream errors.
    bool refMissing = false;

    bool frameIsIntra() const { return isKeyFrame || intraOnly; }
};


// Reset the persistent inter-frame state (spec setup_past_independence):
// loop-filter deltas to their defaults, segmentation features cleared.
void VaapiVideoDecoder::vp9SetupPastIndependence() {
    mVp9Lf.deltaEnabled = false;
    mVp9Lf.refDeltas[0] = 1;
    mVp9Lf.refDeltas[1] = 0;
    mVp9Lf.refDeltas[2] = -1;
    mVp9Lf.refDeltas[3] = -1;
    mVp9Lf.modeDeltas[0] = 0;
    mVp9Lf.modeDeltas[1] = 0;
    memset(mVp9Seg.featureEnabled, 0, sizeof(mVp9Seg.featureEnabled));
    memset(mVp9Seg.featureData, 0, sizeof(mVp9Seg.featureData));
    memset(mVp9Seg.treeProbs, 255, sizeof(mVp9Seg.treeProbs));
    memset(mVp9Seg.predProbs, 255, sizeof(mVp9Seg.predProbs));
}

void VaapiVideoDecoder::vp9ClearRefSlots() {
    mVp9Slots.fill(nullptr);
}

// Parse one frame's uncompressed header. Persistent state (loop-filter deltas,
// segmentation) is applied and updated in place per the spec's ordering:
// setup_past_independence runs BEFORE loop_filter/segmentation are read.
bool VaapiVideoDecoder::parseVp9UncompressedHeader(const uint8_t* data, size_t size,
                                                  Vp9FrameHeader* hdr) {
    ABitReader br(data, size);
    if (rd(&br, 2) != kVp9FrameMarker) {
        ALOGE("Bad VP9 frame marker.");
        return false;
    }
    const uint32_t profileLow = rd(&br, 1);
    const uint32_t profileHigh = rd(&br, 1);
    hdr->profile = static_cast<uint8_t>((profileHigh << 1) | profileLow);
    if (hdr->profile == 3) rd(&br, 1);  // reserved_zero

    hdr->showExistingFrame = rd(&br, 1);
    if (hdr->showExistingFrame) {
        hdr->frameToShowMapIdx = static_cast<uint8_t>(rd(&br, 3));
        return true;  // nothing else in the header; no decode happens
    }

    hdr->isKeyFrame = rd(&br, 1) == 0;
    hdr->showFrame = rd(&br, 1);
    hdr->errorResilient = rd(&br, 1);

    // color_config() — profile 0/1 are 8-bit; only subsampling/range matter to
    // the container and none of it reaches the VA params beyond 4:2:0 checks.
    const auto parseColorConfig = [&]() {
        if (hdr->profile >= 2) rd(&br, 1);  // ten_or_twelve_bit
        const uint32_t colorSpace = rd(&br, 3);
        if (colorSpace != 7 /*CS_RGB*/) {
            rd(&br, 1);  // color_range
            if (hdr->profile == 1 || hdr->profile == 3) {
                rd(&br, 3);  // subsampling_x, subsampling_y, reserved
            }
        } else if (hdr->profile == 1 || hdr->profile == 3) {
            rd(&br, 1);  // reserved
        }
    };
    const auto parseFrameSize = [&]() {
        hdr->width = rd(&br, 16) + 1;
        hdr->height = rd(&br, 16) + 1;
    };
    const auto parseRenderSize = [&]() {
        if (rd(&br, 1)) rd(&br, 32);  // render W-1, H-1 — display-only
    };

    if (hdr->isKeyFrame) {
        if (rd(&br, 24) != kVp9SyncCode) {
            ALOGE("Bad VP9 sync code (key frame).");
            return false;
        }
        parseColorConfig();
        parseFrameSize();
        parseRenderSize();
        hdr->refreshFrameFlags = 0xff;
    } else {
        hdr->intraOnly = hdr->showFrame ? false : rd(&br, 1);
        hdr->resetFrameContext = hdr->errorResilient ? 0 : static_cast<uint8_t>(rd(&br, 2));
        if (hdr->intraOnly) {
            if (rd(&br, 24) != kVp9SyncCode) {
                ALOGE("Bad VP9 sync code (intra-only frame).");
                return false;
            }
            if (hdr->profile > 0) {
                parseColorConfig();
            }
            hdr->refreshFrameFlags = static_cast<uint8_t>(rd(&br, 8));
            parseFrameSize();
            parseRenderSize();
        } else {
            hdr->refreshFrameFlags = static_cast<uint8_t>(rd(&br, 8));
            for (int i = 0; i < kVp9RefsPerFrame; ++i) {
                hdr->refFrameIdx[i] = static_cast<uint8_t>(rd(&br, 3));
                hdr->refFrameSignBias[i] = static_cast<uint8_t>(rd(&br, 1));
            }
            // frame_size_with_refs: inherit the first ref whose stored size is
            // used, else read an explicit size.
            bool foundRef = false;
            for (int i = 0; i < kVp9RefsPerFrame; ++i) {
                if (rd(&br, 1)) {
                    const DpbEntry* ref = mVp9Slots[hdr->refFrameIdx[i]];
                    if (!ref) {
                        hdr->refMissing = true;
                        return false;
                    }
                    hdr->width = static_cast<uint32_t>(ref->codedSize.width);
                    hdr->height = static_cast<uint32_t>(ref->codedSize.height);
                    foundRef = true;
                    break;
                }
            }
            if (!foundRef) parseFrameSize();
            parseRenderSize();
            hdr->allowHighPrecisionMv = rd(&br, 1);
            hdr->interpFilter =
                    rd(&br, 1) ? 4 /*SWITCHABLE*/ : kLiteralToFilter[rd(&br, 2)];
        }
    }

    if (!hdr->errorResilient) {
        hdr->refreshFrameContext = rd(&br, 1);
        hdr->frameParallelDecoding = rd(&br, 1);
    } else {
        hdr->refreshFrameContext = false;
        hdr->frameParallelDecoding = true;
    }
    hdr->frameContextIdx = static_cast<uint8_t>(rd(&br, 2));

    if (hdr->frameIsIntra() || hdr->errorResilient) {
        vp9SetupPastIndependence();
        hdr->frameContextIdx = 0;
    }

    // loop_filter_params — the deltas persist across frames.
    mVp9Lf.level = static_cast<uint8_t>(rd(&br, 6));
    mVp9Lf.sharpness = static_cast<uint8_t>(rd(&br, 3));
    mVp9Lf.deltaEnabled = rd(&br, 1);
    if (mVp9Lf.deltaEnabled && rd(&br, 1) /*delta_update*/) {
        for (int i = 0; i < 4; ++i) {
            if (rd(&br, 1)) mVp9Lf.refDeltas[i] = static_cast<int8_t>(readSignedLiteral(&br, 6));
        }
        for (int i = 0; i < 2; ++i) {
            if (rd(&br, 1)) {
                mVp9Lf.modeDeltas[i] = static_cast<int8_t>(readSignedLiteral(&br, 6));
            }
        }
    }

    // quantization_params
    hdr->baseQIdx = static_cast<uint8_t>(rd(&br, 8));
    const auto readDeltaQ = [&]() -> int32_t {
        return rd(&br, 1) ? readSignedLiteral(&br, 4) : 0;
    };
    hdr->deltaQYDc = readDeltaQ();
    hdr->deltaQUvDc = readDeltaQ();
    hdr->deltaQUvAc = readDeltaQ();

    // segmentation_params — probs and feature data persist across frames.
    mVp9Seg.enabled = rd(&br, 1);
    mVp9Seg.updateMap = false;
    mVp9Seg.temporalUpdate = false;
    if (mVp9Seg.enabled) {
        mVp9Seg.updateMap = rd(&br, 1);
        if (mVp9Seg.updateMap) {
            for (int i = 0; i < 7; ++i) {
                mVp9Seg.treeProbs[i] =
                        rd(&br, 1) ? static_cast<uint8_t>(rd(&br, 8)) : 255;
            }
            mVp9Seg.temporalUpdate = rd(&br, 1);
            for (int i = 0; i < 3; ++i) {
                uint8_t prob = 255;
                if (mVp9Seg.temporalUpdate && rd(&br, 1)) {
                    prob = static_cast<uint8_t>(rd(&br, 8));
                }
                mVp9Seg.predProbs[i] = prob;
            }
        }
        if (rd(&br, 1) /*update_data*/) {
            mVp9Seg.absDelta = rd(&br, 1);
            for (int seg = 0; seg < kVp9MaxSegments; ++seg) {
                for (int f = 0; f < 4; ++f) {
                    mVp9Seg.featureEnabled[seg][f] = rd(&br, 1);
                    int32_t value = 0;
                    if (mVp9Seg.featureEnabled[seg][f]) {
                        if (kSegFeatureBits[f] > 0) {
                            value = static_cast<int32_t>(rd(&br, kSegFeatureBits[f]));
                        }
                        if (kSegFeatureSigned[f] && rd(&br, 1)) value = -value;
                    }
                    mVp9Seg.featureData[seg][f] = static_cast<int16_t>(value);
                }
            }
        }
    }

    // tile_info — bounds per spec 6.2.14: MAX_TILE_WIDTH_B64 = 64,
    // MIN_TILE_WIDTH_B64 = 4, in units of 64-wide superblocks.
    const uint32_t sb64Cols = (hdr->width + 63) / 64;
    uint32_t minLog2 = 0;
    while ((64u << minLog2) < sb64Cols) ++minLog2;
    uint32_t maxLog2 = 1;
    while ((sb64Cols >> (maxLog2 + 1)) >= 4) ++maxLog2;
    uint32_t tileColsLog2 = minLog2;
    while (tileColsLog2 < maxLog2) {
        if (rd(&br, 1)) {
            ++tileColsLog2;
        } else {
            break;
        }
    }
    hdr->tileColsLog2 = static_cast<uint8_t>(tileColsLog2);
    uint32_t tileRowsLog2 = rd(&br, 1);
    if (tileRowsLog2) tileRowsLog2 += rd(&br, 1);
    hdr->tileRowsLog2 = static_cast<uint8_t>(tileRowsLog2);

    hdr->compressedHeaderBytes = static_cast<uint16_t>(rd(&br, 16));

    if (br.overRead()) {
        ALOGE("VP9 uncompressed header overran the frame (%zu bytes).", size);
        return false;
    }
    const size_t consumedBits = size * 8 - br.numBitsLeft();
    hdr->uncompressedHeaderBytes = static_cast<uint32_t>((consumedBits + 7) / 8);
    if (hdr->uncompressedHeaderBytes + hdr->compressedHeaderBytes > size) {
        ALOGE("VP9 header split (%u + %u) exceeds the frame size (%zu).",
              hdr->uncompressedHeaderBytes, hdr->compressedHeaderBytes, size);
        return false;
    }
    return true;
}

// Fill the eight VASegmentParameterVP9 entries: per-segment dequant scales from
// the lookup tables and per-[ref][mode] filter levels, both mirroring libvpx's
// vp9_init_dequantizer / vp9_loop_filter_frame_init derivations.
void VaapiVideoDecoder::fillVp9SegmentParams(const Vp9FrameHeader& hdr,
                                             VASliceParameterBufferVP9* slice) {
    const int scale = 1 << (mVp9Lf.level >> 5);
    for (int seg = 0; seg < kVp9MaxSegments; ++seg) {
        VASegmentParameterVP9* p = &slice->seg_param[seg];
        memset(p, 0, sizeof(*p));

        int32_t qIdx = hdr.baseQIdx;
        if (mVp9Seg.enabled && mVp9Seg.featureEnabled[seg][kVp9SegLvlAltQ]) {
            const int32_t data = mVp9Seg.featureData[seg][kVp9SegLvlAltQ];
            qIdx = mVp9Seg.absDelta ? data : qIdx + data;
        }
        qIdx = clampInt(qIdx, 0, 255);
        p->luma_ac_quant_scale = kAcQLookup[qIdx];
        p->luma_dc_quant_scale = kDcQLookup[clampInt(qIdx + hdr.deltaQYDc, 0, 255)];
        p->chroma_ac_quant_scale = kAcQLookup[clampInt(qIdx + hdr.deltaQUvAc, 0, 255)];
        p->chroma_dc_quant_scale = kDcQLookup[clampInt(qIdx + hdr.deltaQUvDc, 0, 255)];

        int32_t lvlSeg = mVp9Lf.level;
        if (mVp9Seg.enabled && mVp9Seg.featureEnabled[seg][kVp9SegLvlAltL]) {
            const int32_t data = mVp9Seg.featureData[seg][kVp9SegLvlAltL];
            lvlSeg = clampInt(mVp9Seg.absDelta ? data : lvlSeg + data, 0, kVp9MaxLoopFilter);
        }
        if (!mVp9Lf.deltaEnabled) {
            for (int r = 0; r < 4; ++r) {
                p->filter_level[r][0] = static_cast<uint8_t>(lvlSeg);
                p->filter_level[r][1] = static_cast<uint8_t>(lvlSeg);
            }
        } else {
            const int32_t intraLvl = lvlSeg + mVp9Lf.refDeltas[0] * scale;
            p->filter_level[0][0] =
                    static_cast<uint8_t>(clampInt(intraLvl, 0, kVp9MaxLoopFilter));
            p->filter_level[0][1] = p->filter_level[0][0];
            for (int r = 1; r < 4; ++r) {
                for (int m = 0; m < 2; ++m) {
                    const int32_t lvl =
                            lvlSeg + mVp9Lf.refDeltas[r] * scale + mVp9Lf.modeDeltas[m] * scale;
                    p->filter_level[r][m] =
                            static_cast<uint8_t>(clampInt(lvl, 0, kVp9MaxLoopFilter));
                }
            }
        }

        if (mVp9Seg.enabled && mVp9Seg.featureEnabled[seg][kVp9SegLvlRefFrame]) {
            p->segment_flags.fields.segment_reference_enabled = 1;
            p->segment_flags.fields.segment_reference =
                    static_cast<uint16_t>(mVp9Seg.featureData[seg][kVp9SegLvlRefFrame]) & 0x3;
        }
        if (mVp9Seg.enabled && mVp9Seg.featureEnabled[seg][kVp9SegLvlSkip]) {
            p->segment_flags.fields.segment_reference_skipped = 1;
        }
    }
}

// Decode one frame of a temporal unit into a fresh surface and update the
// reference slots. |data|/|size| span exactly this frame's bytes.
VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeVp9Frame(int32_t bitstreamId,
                                                                 const uint8_t* data,
                                                                 size_t size) {
    ATRACE_CALL();
    Vp9FrameHeader hdr;
    if (!parseVp9UncompressedHeader(data, size, &hdr)) {
        if (hdr.refMissing) {
            noteMissingRefSkip("VP9 frame sized from an empty ref slot");
            return FrameResult::kSkipped;
        }
        return FrameResult::kError;
    }

    if (hdr.showExistingFrame) {
        const DpbEntry* e = mVp9Slots[hdr.frameToShowMapIdx];
        if (!e) {
            // The frame this points at was skipped (or the stream was joined
            // mid-GOP): nothing to show, not a stream error.
            noteMissingRefSkip("VP9 show_existing_frame against an empty slot");
            return FrameResult::kSkipped;
        }
        PendingOutput po;
        po.surface = e->surface;
        po.bitstreamId = bitstreamId;
        po.codedSize = e->codedSize;
        po.visibleRect = e->visibleRect;
        mPendingOutputs.push(po);
        pumpOutput();
        return FrameResult::kOk;
    }

    if (hdr.profile != 0) {
        // Profile 2 (10-bit) needs P010 decode surfaces and a different VA
        // profile; not wired up yet. Fail cleanly so the framework can fall
        // back rather than decode garbage.
        ALOGE("VP9 profile %u is not supported (profile 0 only).", hdr.profile);
        return FrameResult::kError;
    }
    if (hdr.width == 0 || hdr.height == 0) {
        ALOGE("VP9 frame has no size (inherit from an empty slot?).");
        return FrameResult::kError;
    }

    // An inter frame predicting from empty slots (mid-GOP join, or slots
    // cleared by a flush) is skipped, not decoded from VA_INVALID_SURFACE.
    if (!hdr.frameIsIntra()) {
        for (int i = 0; i < kVp9RefsPerFrame; ++i) {
            if (!mVp9Slots[hdr.refFrameIdx[i]]) {
                noteMissingRefSkip("VP9 inter frame referencing an empty slot");
                return FrameResult::kSkipped;
            }
        }
    }
    noteMissingRefRecovery("VP9");

    // Superblocks are 64x64; size the context/surfaces aligned, keep the
    // picture's exact geometry for output.
    const ui::Size alignedSize(static_cast<int>((hdr.width + 63) & ~63u),
                               static_cast<int>((hdr.height + 63) & ~63u));
    const ui::Size frameSize(static_cast<int>(hdr.width), static_cast<int>(hdr.height));
    const Rect visible(0, 0, static_cast<int>(hdr.width), static_cast<int>(hdr.height));
    if (!ensureVAContextForGeometry(alignedSize, visible, kVp9NumRefSlots)) {
        return FrameResult::kError;
    }

    const VASurfaceID target = acquireFreeSurface();
    if (target == VA_INVALID_SURFACE) return FrameResult::kNoSurface;

    VADisplay dpy = mDevice->display();

    VADecPictureParameterBufferVP9 pic;
    memset(&pic, 0, sizeof(pic));
    pic.frame_width = static_cast<uint16_t>(hdr.width);
    pic.frame_height = static_cast<uint16_t>(hdr.height);
    for (int i = 0; i < kVp9NumRefSlots; ++i) {
        pic.reference_frames[i] = mVp9Slots[i] ? mVp9Slots[i]->surface : VA_INVALID_SURFACE;
    }
    pic.pic_fields.bits.subsampling_x = 1;
    pic.pic_fields.bits.subsampling_y = 1;
    pic.pic_fields.bits.frame_type = hdr.isKeyFrame ? 0 : 1;
    pic.pic_fields.bits.show_frame = hdr.showFrame;
    pic.pic_fields.bits.error_resilient_mode = hdr.errorResilient;
    pic.pic_fields.bits.intra_only = hdr.intraOnly;
    pic.pic_fields.bits.allow_high_precision_mv = hdr.allowHighPrecisionMv;
    pic.pic_fields.bits.mcomp_filter_type = hdr.interpFilter;
    pic.pic_fields.bits.frame_parallel_decoding_mode = hdr.frameParallelDecoding;
    pic.pic_fields.bits.reset_frame_context = hdr.resetFrameContext;
    pic.pic_fields.bits.refresh_frame_context = hdr.refreshFrameContext;
    pic.pic_fields.bits.frame_context_idx = hdr.frameContextIdx;
    pic.pic_fields.bits.segmentation_enabled = mVp9Seg.enabled;
    pic.pic_fields.bits.segmentation_temporal_update = mVp9Seg.temporalUpdate;
    pic.pic_fields.bits.segmentation_update_map = mVp9Seg.updateMap;
    pic.pic_fields.bits.last_ref_frame = hdr.refFrameIdx[0];
    pic.pic_fields.bits.last_ref_frame_sign_bias = hdr.refFrameSignBias[0];
    pic.pic_fields.bits.golden_ref_frame = hdr.refFrameIdx[1];
    pic.pic_fields.bits.golden_ref_frame_sign_bias = hdr.refFrameSignBias[1];
    pic.pic_fields.bits.alt_ref_frame = hdr.refFrameIdx[2];
    pic.pic_fields.bits.alt_ref_frame_sign_bias = hdr.refFrameSignBias[2];
    pic.pic_fields.bits.lossless_flag = hdr.baseQIdx == 0 && hdr.deltaQYDc == 0 &&
                                        hdr.deltaQUvDc == 0 && hdr.deltaQUvAc == 0;
    pic.filter_level = mVp9Lf.level;
    pic.sharpness_level = mVp9Lf.sharpness;
    pic.log2_tile_rows = hdr.tileRowsLog2;
    pic.log2_tile_columns = hdr.tileColsLog2;
    pic.frame_header_length_in_bytes = static_cast<uint8_t>(hdr.uncompressedHeaderBytes);
    pic.first_partition_size = hdr.compressedHeaderBytes;
    memcpy(pic.mb_segment_tree_probs, mVp9Seg.treeProbs, sizeof(pic.mb_segment_tree_probs));
    memcpy(pic.segment_pred_probs, mVp9Seg.predProbs, sizeof(pic.segment_pred_probs));
    pic.profile = hdr.profile;
    pic.bit_depth = 8;

    VASliceParameterBufferVP9 slice;
    memset(&slice, 0, sizeof(slice));
    slice.slice_data_size = static_cast<uint32_t>(size);
    slice.slice_data_offset = 0;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    fillVp9SegmentParams(hdr, &slice);

    VABufferID buffers[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    VAStatus status = vaCreateBuffer(dpy, mVAContext, VAPictureParameterBufferType, sizeof(pic),
                                     1, &pic, &buffers[0]);
    if (status == VA_STATUS_SUCCESS) {
        status = vaCreateBuffer(dpy, mVAContext, VASliceParameterBufferType, sizeof(slice), 1,
                                &slice, &buffers[1]);
    }
    if (status == VA_STATUS_SUCCESS) {
        status = vaCreateBuffer(dpy, mVAContext, VASliceDataBufferType,
                                static_cast<unsigned int>(size), 1, const_cast<uint8_t*>(data),
                                &buffers[2]);
    }
    bool ok = status == VA_STATUS_SUCCESS;
    if (!ok) ALOGE("vaCreateBuffer(VP9) failed: %s", vaErrorStr(status));

    if (ok) {
        status = vaBeginPicture(dpy, mVAContext, target);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaBeginPicture(VP9) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    if (ok) {
        status = vaRenderPicture(dpy, mVAContext, buffers, 3);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(VP9) failed: %s", vaErrorStr(status));
            ok = false;
        }
        // vaEndPicture must pair with a successful vaBeginPicture even if
        // render failed, or the context wedges.
        status = vaEndPicture(dpy, mVAContext);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaEndPicture(VP9) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    for (VABufferID b : buffers) {
        if (b != VA_INVALID_ID) vaDestroyBuffer(dpy, b);
    }
    if (!ok) return FrameResult::kError;

    // Track the decoded picture and refresh the reference slots.
    auto entry = std::make_unique<DpbEntry>();
    entry->surface = target;
    entry->bitstreamId = bitstreamId;
    entry->codedSize = frameSize;
    entry->visibleRect = visible;
    // Invisible frames are never displayed: born outputted so the reclaimer
    // can free them as soon as no slot references them.
    entry->outputted = !hdr.showFrame;
    DpbEntry* raw = entry.get();
    mDpb.push_back(std::move(entry));

    for (int i = 0; i < kVp9NumRefSlots; ++i) {
        if (hdr.refreshFrameFlags & (1u << i)) mVp9Slots[i] = raw;
    }
    // refUsed = referenced by any slot; recomputed wholesale so evicted
    // pictures free up through the shared reclaim path.
    for (auto& e : mDpb) {
        bool used = false;
        for (const DpbEntry* slot : mVp9Slots) {
            if (slot == e.get()) {
                used = true;
                break;
            }
        }
        e->refUsed = used;
    }

    if (hdr.showFrame) {
        raw->outputted = true;
        queueForOutput(*raw);
    }
    reclaimDpbEntries();
    return FrameResult::kOk;
}

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeVP9Buffer(int32_t bitstreamId,
                                                                  const uint8_t* data,
                                                                  size_t size) {
    ATRACE_CALL();
    std::vector<SuperframeSpan> frames;
    if (!splitSuperframe(data, size, &frames)) return FrameResult::kError;

    // Back-pressure is per input buffer: a superframe is decoded atomically, so
    // every sub-frame that needs a surface must have one BEFORE the first
    // decode — retrying halfway through would decode the leading frames twice
    // against slots they already refreshed.
    size_t surfacesNeeded = 0;
    for (const SuperframeSpan& f : frames) {
        if (f.size == 0) continue;
        // A show-existing frame starts with marker+profile+1: cheap peek.
        ABitReader peek(data + f.offset, std::min<size_t>(f.size, 2));
        rd(&peek, 2);
        uint32_t prof = rd(&peek, 1) | (rd(&peek, 1) << 1);
        if (prof == 3) rd(&peek, 1);
        if (!rd(&peek, 1) /*!show_existing*/) ++surfacesNeeded;
    }
    reclaimDpbEntries();
    size_t freeCount = 0;
    for (VASurfaceID candidate : mSurfacePool) {
        bool inUse = false;
        for (const auto& e : mDpb) {
            if (e->surface == candidate) {
                inUse = true;
                break;
            }
        }
        if (!inUse) {
            std::queue<PendingOutput> scan = mPendingOutputs;
            while (!scan.empty()) {
                if (scan.front().surface == candidate) {
                    inUse = true;
                    break;
                }
                scan.pop();
            }
        }
        if (!inUse) ++freeCount;
    }
    // First frame(s) before the pool exists: mSurfacePool is empty until
    // ensureVAContextForGeometry runs; let the per-frame acquire handle that.
    if (!mSurfacePool.empty() && freeCount < surfacesNeeded) return FrameResult::kNoSurface;

    // A skipped sub-frame (missing references) must not abort the rest of the
    // superframe — a later sub-frame may be decodable (or a keyframe). The
    // buffer as a whole reports kSkipped only when nothing in it decoded, so
    // its C2 work completes through the drop path instead of waiting for an
    // output that will never come.
    bool decodedAny = false;
    for (const SuperframeSpan& f : frames) {
        if (f.size == 0) continue;
        const FrameResult r = decodeVp9Frame(bitstreamId, data + f.offset, f.size);
        if (r == FrameResult::kSkipped) continue;
        if (r != FrameResult::kOk) return r;
        decodedAny = true;
    }
    return decodedAny ? FrameResult::kOk : FrameResult::kSkipped;
}

}  // namespace android
