// Copyright 2024 The Android Open Source Project
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VA_VAAPI_VAAPI_VIDEO_DECODER_H
#define ANDROID_C2_VA_VAAPI_VAAPI_VIDEO_DECODER_H

#include <stdint.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <vector>

#include <base/memory/weak_ptr.h>
#include <base/sequenced_task_runner.h>
#include <ui/Rect.h>
#include <ui/Size.h>
#include <va/va.h>
#include <va/va_dec_vp9.h>

#include <v4l2_codec2/common/VideoTypes.h>
#include <v4l2_codec2/components/BitstreamBuffer.h>
#include <v4l2_codec2/components/VideoDecoder.h>
#include <v4l2_codec2/components/VideoFrame.h>
#include <v4l2_codec2/components/VideoFramePool.h>

#include <c2_va/vaapi/VaapiDevice.h>

namespace libgav1 {
class ObuParser;
}  // namespace libgav1

namespace android {

// A stateless VAAPI-backed VideoDecoder.
//
// The AVC/H.264 path is implemented end to end: SPS/PPS/slice NAL units are
// parsed with the v4l2_codec2 common NAL splitter plus a minimal in-file
// field extractor, translated into VAPictureParameterBufferH264 /
// VAIQMatrixBufferH264 / VASliceParameterBufferH264 + slice-data buffers, and
// submitted with vaBeginPicture/vaRenderPicture/vaEndPicture.
//
// The decoder owns an internal ring of VA output surfaces sized to the coded
// resolution; the DPB references those. When a picture reaches its POC-ordered
// output time a VideoFrame is fetched asynchronously from the VideoFramePool and
// the decoded NV12 surface is copied into the gralloc dma-buf via
// vaDeriveImage/vaGetImage before being emitted through OutputCB. (Zero-copy
// import of the pool buffer as the decode target is a future optimization.)
//
// HEVC/VP9/AV1: stateless decode cores are complete and enabled (bd remora-zbt),
// in VaapiVideoDecoder{HEVC,VP9,AV1}.cpp. 10-bit variants (VP9 profile 2, AV1
// Main 10) are deliberately not advertised — the surface pool is NV12-only.
//
// All public methods and internal callbacks run on |mTaskRunner|, mirroring
// V4L2Decoder's threading contract.
class VaapiVideoDecoder : public VideoDecoder {
public:
    // Mirror of V4L2Decoder::Create(). Returns nullptr (self-disable) if no
    // VAAPI device is available or the codec/profile is unsupported.
    static std::unique_ptr<VideoDecoder> Create(
            uint32_t debugStreamId, const VideoCodec& codec, const size_t inputBufferSize,
            const size_t minNumOutputBuffers, GetPoolCB getPoolCb, OutputCB outputCb,
            ErrorCB errorCb, scoped_refptr<::base::SequencedTaskRunner> taskRunner, bool isSecure);

    ~VaapiVideoDecoder() override;

    // VideoDecoder implementation.
    void decode(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB decodeCb) override;
    void drain(DecodeCB drainCb) override;
    void flush() override;

private:
    enum class State {
        Idle,
        Decoding,
        Draining,
        Error,
    };
    static const char* StateToString(State state);

    // ---- H.264 parsed parameter sets (minimal subset needed for VAAPI) ----

    struct H264SPS {
        bool valid = false;
        uint8_t profile_idc = 0;
        uint32_t seq_parameter_set_id = 0;
        uint32_t chroma_format_idc = 1;  // 4:2:0 default
        bool separate_colour_plane_flag = false;
        uint32_t bit_depth_luma_minus8 = 0;
        uint32_t bit_depth_chroma_minus8 = 0;
        uint32_t log2_max_frame_num_minus4 = 0;
        uint32_t pic_order_cnt_type = 0;
        uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
        bool delta_pic_order_always_zero_flag = false;
        int32_t offset_for_non_ref_pic = 0;
        int32_t offset_for_top_to_bottom_field = 0;
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
        int32_t offset_for_ref_frame[256] = {};
        uint32_t max_num_ref_frames = 0;
        bool gaps_in_frame_num_value_allowed_flag = false;
        uint32_t pic_width_in_mbs_minus1 = 0;
        uint32_t pic_height_in_map_units_minus1 = 0;
        bool frame_mbs_only_flag = true;
        bool mb_adaptive_frame_field_flag = false;
        bool direct_8x8_inference_flag = false;
        bool frame_cropping_flag = false;
        uint32_t frame_crop_left_offset = 0;
        uint32_t frame_crop_right_offset = 0;
        uint32_t frame_crop_top_offset = 0;
        uint32_t frame_crop_bottom_offset = 0;
        // Scaling lists (raster order). seq_scaling_matrix_present overrides flat.
        bool seq_scaling_matrix_present_flag = false;
        uint8_t scaling_list4x4[6][16] = {};
        uint8_t scaling_list8x8[6][64] = {};
    };

    struct H264PPS {
        bool valid = false;
        uint32_t pic_parameter_set_id = 0;
        uint32_t seq_parameter_set_id = 0;
        bool entropy_coding_mode_flag = false;
        bool bottom_field_pic_order_in_frame_present_flag = false;
        uint32_t num_ref_idx_l0_default_active_minus1 = 0;
        uint32_t num_ref_idx_l1_default_active_minus1 = 0;
        bool weighted_pred_flag = false;
        uint32_t weighted_bipred_idc = 0;
        int32_t pic_init_qp_minus26 = 0;
        int32_t pic_init_qs_minus26 = 0;
        int32_t chroma_qp_index_offset = 0;
        bool deblocking_filter_control_present_flag = false;
        bool constrained_intra_pred_flag = false;
        bool redundant_pic_cnt_present_flag = false;
        bool transform_8x8_mode_flag = false;
        int32_t second_chroma_qp_index_offset = 0;
        bool pic_scaling_matrix_present_flag = false;
        uint8_t scaling_list4x4[6][16] = {};
        uint8_t scaling_list8x8[6][64] = {};
    };

    // The subset of slice-header fields VAAPI needs.
    struct H264SliceHeader {
        bool valid = false;
        const uint8_t* nalData = nullptr;  // points into the input buffer (incl. NAL header byte)
        size_t nalSize = 0;
        uint8_t nalRefIdc = 0;
        uint8_t nalUnitType = 0;
        bool idrPicFlag = false;

        uint32_t first_mb_in_slice = 0;
        uint32_t slice_type = 0;  // 0..9
        uint32_t pic_parameter_set_id = 0;
        uint32_t frame_num = 0;
        bool field_pic_flag = false;
        bool bottom_field_flag = false;
        uint32_t idr_pic_id = 0;
        uint32_t pic_order_cnt_lsb = 0;
        int32_t delta_pic_order_cnt_bottom = 0;
        int32_t delta_pic_order_cnt[2] = {0, 0};
        bool num_ref_idx_active_override_flag = false;
        uint32_t num_ref_idx_l0_active_minus1 = 0;
        uint32_t num_ref_idx_l1_active_minus1 = 0;
        bool direct_spatial_mv_pred_flag = false;
        int32_t slice_qp_delta = 0;
        uint32_t cabac_init_idc = 0;
        uint32_t disable_deblocking_filter_idc = 0;
        int32_t slice_alpha_c0_offset_div2 = 0;
        int32_t slice_beta_offset_div2 = 0;

        // ref_pic_list_modification (7.3.3.1): ops 0/1 carry
        // abs_diff_pic_num_minus1, op 2 carries long_term_pic_num.
        struct RefListMod {
            uint32_t op = 0;
            uint32_t value = 0;
        };
        std::vector<RefListMod> refListMods[2];  // [l0, l1]

        // pred_weight_table (7.3.3.2). Entries without explicit weights get the
        // inferred defaults (1 << denom, offset 0) when filled into VA.
        bool hasWeightTable = false;
        uint32_t luma_log2_weight_denom = 0;
        uint32_t chroma_log2_weight_denom = 0;
        struct WeightEntry {
            bool lumaPresent = false;
            int32_t lumaWeight = 0;
            int32_t lumaOffset = 0;
            bool chromaPresent = false;
            int32_t chromaWeight[2] = {0, 0};
            int32_t chromaOffset[2] = {0, 0};
        };
        WeightEntry weights[2][32];  // [list][refIdx]

        // dec_ref_pic_marking (7.3.3.3).
        bool long_term_reference_flag = false;  // IDR only
        bool adaptiveRefPicMarking = false;
        struct MmcoOp {
            uint32_t op = 0;
            uint32_t arg1 = 0;  // difference_of_pic_nums_minus1 / long_term_pic_num /
                                // max_long_term_frame_idx_plus1
            uint32_t arg2 = 0;  // long_term_frame_idx
        };
        std::vector<MmcoOp> mmcoOps;

        // Bit offset (after emulation-prevention removal) from the start of the
        // NAL byte to the first byte of slice_data(). Needed for
        // slice_data_bit_offset.
        uint32_t header_bit_size = 0;
    };

    // ---- H.265 / HEVC parsed parameter sets (subset needed for VAAPI) ----
    //
    // Public only so the file-local parse helpers in VaapiVideoDecoderHEVC.cpp
    // (which are plain functions in an anonymous namespace, not members) can name
    // them. Nothing outside that translation unit uses these.
public:
    // scaling_list_data() (7.3.4), in the four sizeIds VA consumes directly.
    struct H265ScalingList {
        uint8_t list4x4[6][16] = {};
        uint8_t list8x8[6][64] = {};
        uint8_t list16x16[6][64] = {};
        uint8_t list32x32[2][64] = {};
        uint8_t dc16x16[6] = {};
        uint8_t dc32x32[2] = {};
    };

    // short_term_ref_pic_set() (7.3.7), already resolved: inter-RPS prediction is
    // applied at parse time so these are the derived DeltaPocS0/S1 +
    // UsedByCurrPicS0/S1 arrays of 7.4.8.
    struct H265ShortTermRPS {
        uint32_t num_negative_pics = 0;
        uint32_t num_positive_pics = 0;
        int32_t deltaPocS0[16] = {};
        int32_t deltaPocS1[16] = {};
        bool usedByCurrS0[16] = {};
        bool usedByCurrS1[16] = {};
        uint32_t numDeltaPocs() const { return num_negative_pics + num_positive_pics; }
    };

    struct H265SPS {
        bool valid = false;
        uint32_t sps_seq_parameter_set_id = 0;
        uint32_t chroma_format_idc = 1;
        bool separate_colour_plane_flag = false;
        uint32_t pic_width_in_luma_samples = 0;
        uint32_t pic_height_in_luma_samples = 0;
        uint32_t conf_win_left_offset = 0;
        uint32_t conf_win_right_offset = 0;
        uint32_t conf_win_top_offset = 0;
        uint32_t conf_win_bottom_offset = 0;
        uint32_t bit_depth_luma_minus8 = 0;
        uint32_t bit_depth_chroma_minus8 = 0;
        uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
        uint32_t sps_max_dec_pic_buffering_minus1 = 0;
        uint32_t sps_max_num_reorder_pics = 0;
        uint32_t log2_min_luma_coding_block_size_minus3 = 0;
        uint32_t log2_diff_max_min_luma_coding_block_size = 0;
        uint32_t log2_min_transform_block_size_minus2 = 0;
        uint32_t log2_diff_max_min_transform_block_size = 0;
        uint32_t max_transform_hierarchy_depth_inter = 0;
        uint32_t max_transform_hierarchy_depth_intra = 0;
        bool scaling_list_enabled_flag = false;
        bool amp_enabled_flag = false;
        bool sample_adaptive_offset_enabled_flag = false;
        bool pcm_enabled_flag = false;
        uint32_t pcm_sample_bit_depth_luma_minus1 = 0;
        uint32_t pcm_sample_bit_depth_chroma_minus1 = 0;
        uint32_t log2_min_pcm_luma_coding_block_size_minus3 = 0;
        uint32_t log2_diff_max_min_pcm_luma_coding_block_size = 0;
        bool pcm_loop_filter_disabled_flag = false;
        uint32_t num_short_term_ref_pic_sets = 0;
        // 7.4.3.2.1 caps num_short_term_ref_pic_sets at 64.
        H265ShortTermRPS st_rps[64];
        bool long_term_ref_pics_present_flag = false;
        uint32_t num_long_term_ref_pics_sps = 0;
        uint32_t lt_ref_pic_poc_lsb_sps[32] = {};
        bool used_by_curr_pic_lt_sps_flag[32] = {};
        bool sps_temporal_mvp_enabled_flag = false;
        bool strong_intra_smoothing_enabled_flag = false;
        H265ScalingList scaling_list;
    };

    struct H265PPS {
        bool valid = false;
        uint32_t pps_pic_parameter_set_id = 0;
        uint32_t pps_seq_parameter_set_id = 0;
        bool dependent_slice_segments_enabled_flag = false;
        bool output_flag_present_flag = false;
        uint32_t num_extra_slice_header_bits = 0;
        bool sign_data_hiding_enabled_flag = false;
        bool cabac_init_present_flag = false;
        uint32_t num_ref_idx_l0_default_active_minus1 = 0;
        uint32_t num_ref_idx_l1_default_active_minus1 = 0;
        int32_t init_qp_minus26 = 0;
        bool constrained_intra_pred_flag = false;
        bool transform_skip_enabled_flag = false;
        bool cu_qp_delta_enabled_flag = false;
        uint32_t diff_cu_qp_delta_depth = 0;
        int32_t pps_cb_qp_offset = 0;
        int32_t pps_cr_qp_offset = 0;
        bool pps_slice_chroma_qp_offsets_present_flag = false;
        bool weighted_pred_flag = false;
        bool weighted_bipred_flag = false;
        bool transquant_bypass_enabled_flag = false;
        bool tiles_enabled_flag = false;
        bool entropy_coding_sync_enabled_flag = false;
        uint32_t num_tile_columns_minus1 = 0;
        uint32_t num_tile_rows_minus1 = 0;
        bool uniform_spacing_flag = true;
        uint32_t column_width_minus1[19] = {};
        uint32_t row_height_minus1[21] = {};
        bool loop_filter_across_tiles_enabled_flag = true;
        bool pps_loop_filter_across_slices_enabled_flag = false;
        bool deblocking_filter_control_present_flag = false;
        bool deblocking_filter_override_enabled_flag = false;
        bool pps_deblocking_filter_disabled_flag = false;
        int32_t pps_beta_offset_div2 = 0;
        int32_t pps_tc_offset_div2 = 0;
        bool pps_scaling_list_data_present_flag = false;
        H265ScalingList scaling_list;
        bool lists_modification_present_flag = false;
        uint32_t log2_parallel_merge_level_minus2 = 0;
        bool slice_segment_header_extension_present_flag = false;
    };

    struct H265SliceHeader {
        bool valid = false;
        const uint8_t* nalData = nullptr;  // into the input buffer, incl. the 2-byte NAL header
        size_t nalSize = 0;
        uint8_t nalUnitType = 0;
        uint32_t temporalId = 0;

        bool first_slice_segment_in_pic_flag = false;
        bool dependent_slice_segment_flag = false;
        uint32_t slice_segment_address = 0;
        uint32_t slice_pic_parameter_set_id = 0;
        uint32_t slice_type = 2;  // 0 = B, 1 = P, 2 = I
        bool pic_output_flag = true;
        uint32_t colour_plane_id = 0;
        uint32_t slice_pic_order_cnt_lsb = 0;

        // Short-term RPS in force for this picture (resolved from the SPS list or
        // parsed inline). |st_rps_bits| is the coded size of an inline set — VA
        // wants it as st_rps_bits, and 0 when the set came from the SPS.
        H265ShortTermRPS st_rps;
        uint32_t st_rps_bits = 0;

        uint32_t num_long_term_sps = 0;
        uint32_t num_long_term_pics = 0;
        uint32_t poc_lsb_lt[32] = {};
        bool used_by_curr_pic_lt_flag[32] = {};
        bool delta_poc_msb_present_flag[32] = {};
        uint32_t delta_poc_msb_cycle_lt[32] = {};  // accumulated per 7.4.7.1

        bool slice_temporal_mvp_enabled_flag = false;
        bool slice_sao_luma_flag = false;
        bool slice_sao_chroma_flag = false;
        bool num_ref_idx_active_override_flag = false;
        uint32_t num_ref_idx_l0_active_minus1 = 0;
        uint32_t num_ref_idx_l1_active_minus1 = 0;
        bool ref_pic_list_modification_flag_l0 = false;
        bool ref_pic_list_modification_flag_l1 = false;
        uint32_t list_entry_l0[16] = {};
        uint32_t list_entry_l1[16] = {};
        bool mvd_l1_zero_flag = false;
        bool cabac_init_flag = false;
        bool collocated_from_l0_flag = true;
        uint32_t collocated_ref_idx = 0;

        // pred_weight_table (7.3.6.3); chroma offsets are the DERIVED
        // ChromaOffsetL0/L1 of 7.4.7.3, which is what VA consumes.
        uint32_t luma_log2_weight_denom = 0;
        int32_t delta_chroma_log2_weight_denom = 0;
        int32_t delta_luma_weight[2][16] = {};
        int32_t luma_offset[2][16] = {};
        int32_t delta_chroma_weight[2][16][2] = {};
        int32_t chroma_offset[2][16][2] = {};

        uint32_t five_minus_max_num_merge_cand = 0;
        int32_t slice_qp_delta = 0;
        int32_t slice_cb_qp_offset = 0;
        int32_t slice_cr_qp_offset = 0;
        bool slice_deblocking_filter_disabled_flag = false;
        int32_t slice_beta_offset_div2 = 0;
        int32_t slice_tc_offset_div2 = 0;
        bool slice_loop_filter_across_slices_enabled_flag = false;
        uint32_t num_entry_point_offsets = 0;

        // slice_data_byte_offset: bytes of slice_segment_header() measured in RBSP
        // space (emulation-prevention bytes removed) counting from and including
        // the 2-byte NAL header, plus the EP-byte count VA reports separately.
        uint32_t header_byte_size = 0;
        uint32_t num_emu_prevn_bytes = 0;

        // Derived NAL-type predicates (7.4.2.2).
        bool idrPicFlag = false;
        bool irapPicFlag = false;
    };

private:
    // An entry in the decoded picture buffer. Backed by an internal VA surface
    // (see mSurfacePool); the surface is returned to mFreeSurfaces once the
    // entry is neither a reference nor pending output.
    struct DpbEntry {
        VASurfaceID surface = VA_INVALID_SURFACE;
        int32_t bitstreamId = -1;

        uint32_t frameNum = 0;
        int32_t frameNumWrap = 0;
        int32_t picNum = 0;
        int32_t topFieldOrderCnt = 0;
        int32_t bottomFieldOrderCnt = 0;
        int32_t picOrderCnt = 0;
        bool refUsed = false;      // used as short-term reference
        bool longTerm = false;     // used as long-term reference
        uint32_t longTermFrameIdx = 0;
        bool outputted = false;    // already emitted via OutputCB

        // Geometry of the SPS this picture was decoded under. mCodedSize /
        // mVisibleRect flip as soon as a mid-stream resolution change activates,
        // while pictures from the previous SPS can still be awaiting output —
        // the copy/crop must use the picture's own geometry (remora-e97).
        ui::Size codedSize;
        Rect visibleRect;
    };

    VaapiVideoDecoder(uint32_t debugStreamId, VideoCodec codec,
                      scoped_refptr<::base::SequencedTaskRunner> taskRunner);

    // Second-phase init: opens the VAAPI device, resolves the VA profile, and
    // stores the callbacks. Returns false to make Create() self-disable.
    bool start(const VideoCodec& codec, size_t inputBufferSize, size_t minNumOutputBuffers,
               GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb, bool isSecure);

    // ---- decode pump ----
    struct DecodeRequest {
        DecodeRequest(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB cb)
              : buffer(std::move(buffer)), decodeCb(std::move(cb)) {}
        DecodeRequest(DecodeRequest&&) = default;
        DecodeRequest& operator=(DecodeRequest&&) = default;

        std::unique_ptr<ConstBitstreamBuffer> buffer;  // nullptr => drain marker
        DecodeCB decodeCb;
    };

    void pumpDecodeRequests();
    // Result of decoding one access unit. kNoSurface means "no free decode
    // surface right now" — the request stays queued (back-pressure) and is
    // retried when a pending output completes.
    // kSkipped: the frame was consumed but intentionally not decoded (missing
    // references). It completes its C2 work immediately via DecodeStatus::
    // kAborted — the component's no-output drop path. Returning kOk instead
    // deadlocks: the work would wait for an output that never comes, the
    // pipeline fills (~28 works), and input stalls before the next keyframe
    // can arrive to trigger the no-show reaper.
    enum class FrameResult { kOk, kSkipped, kNoSurface, kError };
    // Decode a single access unit (H.264). |bitstreamId| correlates the output
    // frame back to this input.
    FrameResult decodeH264Buffer(int32_t bitstreamId, const uint8_t* data, size_t size);
    // Decode a single access unit (HEVC). See VaapiVideoDecoderHEVC.cpp.
    FrameResult decodeHEVCBuffer(int32_t bitstreamId, const uint8_t* data, size_t size);

    // ---- H.264 parsing helpers ----
    bool parseSPS(const uint8_t* nal, size_t size, H264SPS* sps);
    bool parsePPS(const uint8_t* nal, size_t size, H264PPS* pps);
    bool parseSliceHeader(const uint8_t* nal, size_t size, H264SliceHeader* sh);

    // ---- HEVC parsing helpers (VaapiVideoDecoderHEVC.cpp) ----
    bool parseH265SPS(const uint8_t* nal, size_t size, H265SPS* sps);
    bool parseH265PPS(const uint8_t* nal, size_t size, H265PPS* pps);
    bool parseH265SliceHeader(const uint8_t* nal, size_t size, H265SliceHeader* sh);

    // ---- VP9 decode core (VaapiVideoDecoderVP9.cpp) ----
    // Decode one temporal unit: a single frame or a superframe. Superframes
    // are decoded atomically (surface preflight, then every sub-frame).
    FrameResult decodeVP9Buffer(int32_t bitstreamId, const uint8_t* data, size_t size);
    struct Vp9FrameHeader;  // uncompressed-header fields; defined in the .cpp
    FrameResult decodeVp9Frame(int32_t bitstreamId, const uint8_t* data, size_t size);
    bool parseVp9UncompressedHeader(const uint8_t* data, size_t size, Vp9FrameHeader* hdr);
    void fillVp9SegmentParams(const Vp9FrameHeader& hdr, VASliceParameterBufferVP9* slice);
    // spec setup_past_independence: loop-filter deltas to defaults,
    // segmentation features cleared. Runs on key/intra-only/error-resilient.
    void vp9SetupPastIndependence();
    void vp9ClearRefSlots();

    // ---- AV1 decode core (VaapiVideoDecoderAV1.cpp) ----
    // Decode one temporal unit; libgav1's ObuParser does the bitstream layer,
    // this class only fills the VA structures and tracks the slots.
    FrameResult decodeAV1Buffer(int32_t bitstreamId, const uint8_t* data, size_t size);
    FrameResult decodeAv1Frame(int32_t bitstreamId, libgav1::ObuParser& parser,
                               const uint8_t* data, size_t size);
    void av1ClearRefSlots();

    // ---- VA config/context lifecycle ----
    bool ensureVAContext(const H264SPS& sps);
    bool ensureVAContextHEVC(const H265SPS& sps);
    // Codec-agnostic core of the two above: (re)build the VA config/context and
    // the internal surface pool for |codedSize|, growing only.
    bool ensureVAContextForGeometry(const ui::Size& codedSize, const Rect& visible,
                                    size_t dpbMaxSize);
    void destroyVAContext();

    // ---- Per-frame VA submission ----
    // Decode a full frame made of |slices| (all sharing the same picture).
    FrameResult decodeFrameH264(int32_t bitstreamId, const H264SPS& sps, const H264PPS& pps,
                                const std::vector<H264SliceHeader>& slices);
    bool fillPictureParam(const H264SPS& sps, const H264PPS& pps, const H264SliceHeader& first,
                          VASurfaceID target, VAPictureParameterBufferH264* pic);
    void fillIQMatrix(const H264SPS& sps, const H264PPS& pps, VAIQMatrixBufferH264* iq);
    // |currPicOrderCnt| is the current picture's PicOrderCnt (needed for B ref
    // lists; the current picture is not in the DPB yet at this point).
    bool fillSliceParam(const H264SPS& sps, const H264PPS& pps, const H264SliceHeader& sh,
                        int32_t currPicOrderCnt, VASliceParameterBufferH264* sp);
    void fillVAPicture(const DpbEntry& e, uint32_t flags, VAPictureH264* out) const;

    // ---- POC / DPB / ref-list management ----
    void computePoc(const H264SPS& sps, const H264SliceHeader& sh, int32_t* topPoc,
                    int32_t* bottomPoc);
    void updatePicNums(const H264SPS& sps, uint32_t currFrameNum);
    void buildRefPicListP(const H264SPS& sps, const H264SliceHeader& sh,
                          std::vector<const DpbEntry*>* list);
    void buildRefPicListB(const H264SPS& sps, const H264SliceHeader& sh, int32_t currPicOrderCnt,
                          std::vector<const DpbEntry*>* list0,
                          std::vector<const DpbEntry*>* list1);
    // Apply ref_pic_list_modification (8.2.4.3) for list |listIdx| in place.
    void applyRefListModification(const H264SPS& sps, const H264SliceHeader& sh, int listIdx,
                                  std::vector<const DpbEntry*>* list);
    void referenceListMarking(const H264SPS& sps, const H264SliceHeader& sh, DpbEntry* current);
    // Adaptive reference marking: apply the slice's MMCO operations (8.2.5.4).
    void applyAdaptiveMarking(const H264SPS& sps, const H264SliceHeader& sh, DpbEntry* current);
    void bumpAndOutputAsNeeded(const H264SPS& sps);
    // Codec-agnostic bumping: emit in POC order while more than |reorderDepth|
    // pictures are buffered.
    void bumpAndOutputAsNeeded(size_t reorderDepth);
    void flushDpb();  // output everything pending, drop references

    // ---- HEVC picture-level decode (VaapiVideoDecoderHEVC.cpp) ----
    FrameResult decodeFrameHEVC(int32_t bitstreamId, const H265SPS& sps, const H265PPS& pps,
                                const std::vector<H265SliceHeader>& slices);
    // 8.3.1: derive PicOrderCntVal for the current picture into mH265CurrPoc.
    void computePocHEVC(const H265SPS& sps, const H265SliceHeader& sh);
    // 8.3.2: resolve the RPS against the DPB, re-marking every entry's
    // reference status and filling mRpsStCurrBefore/After/LtCurr. Pictures no
    // longer in any RPS lose their reference flags here (HEVC has no MMCO —
    // the RPS alone determines DPB state).
    void deriveRpsHEVC(const H265SPS& sps, const H265SliceHeader& sh);
    // 8.3.4: build RefPicList0/1 as indices into |refFrames|.
    void buildRefPicListsHEVC(const H265SliceHeader& sh,
                              const std::vector<DpbEntry*>& refFrames,
                              std::vector<uint8_t>* list0, std::vector<uint8_t>* list1);
    // Drop DPB entries that are neither reference nor pending output, returning
    // their internal surfaces to mFreeSurfaces.
    void reclaimDpbEntries();

    // ---- Internal decode-target surface pool ----
    bool allocateSurfacePool(const ui::Size& size, size_t count);
    void destroySurfacePool();
    VASurfaceID acquireFreeSurface();  // VA_INVALID_SURFACE if exhausted

    // ---- Output path (async, POC-ordered) ----
    // A picture decoded into |surface| and awaiting a pool buffer to copy into.
    struct PendingOutput {
        VASurfaceID surface = VA_INVALID_SURFACE;
        int32_t bitstreamId = -1;
        // Copied from the DpbEntry: the picture's own geometry, not the
        // current stream state (see DpbEntry::codedSize).
        ui::Size codedSize;
        Rect visibleRect;
    };
    void queueForOutput(const DpbEntry& e);
    void pumpOutput();
    void onVideoFrameReady(uint64_t fetchGeneration, int32_t bitstreamId, VASurfaceID surface,
                           std::optional<VideoFramePool::FrameWithBlockId> frameWithBlockId);
    // Layout + DRM format modifier of the pool's gralloc buffers, read straight
    // from the cros_gralloc handle (no CPU mapping / usage bits needed).
    struct OutputBufferInfo {
        uint32_t pitchY = 0;
        uint32_t pitchUV = 0;
        uint32_t offsetY = 0;
        uint32_t offsetUV = 0;
        uint32_t drmFormat = 0;  // DRM fourcc from the handle; 0 = assume NV12
        uint64_t modifier = 0;   // DRM_FORMAT_MOD_LINEAR == 0
        uint64_t totalSize = 0;  // whole-bo size; 0 = derive from fd/layout
        bool fromHandle = false;  // true when read from the cros_gralloc handle
    };
    OutputBufferInfo queryOutputBufferInfo(VideoFrame& frame, const ui::Size& codedSize);
    // Import a gralloc NV12 dma-buf VideoFrame as an external VA surface (used as
    // the destination of the output copy). Uses DRM_PRIME_2 with the buffer's
    // real modifier when known (minigbm may allocate X-/4-tiled); legacy linear
    // DRM_PRIME otherwise. Returns VA_INVALID_SURFACE on failure.
    VASurfaceID importFrameAsSurface(VideoFrame& frame, const ui::Size& codedSize);
    // GPU-copy |src| into |dst| via a cached VPP context (handles tiled
    // destinations that a CPU memcpy through vaDeriveImage would scramble).
    bool vppBlitSurface(VASurfaceID src, VASurfaceID dst, const ui::Size& codedSize,
                        const Rect& visibleRect);
    // Copy the decoded NV12 |srcSurface| into |frame|'s gralloc buffer, using
    // VA (import the frame as a surface + vaDeriveImage) to obtain a writable
    // mapping with a driver-reported layout.
    bool copySurfaceToFrame(VASurfaceID srcSurface, VideoFrame* frame, const ui::Size& codedSize,
                            const Rect& visibleRect);

    void setState(State newState);
    void onError();

    const uint32_t mDebugStreamId;
    VideoCodec mCodec;
    scoped_refptr<::base::SequencedTaskRunner> mTaskRunner;

    GetPoolCB mGetPoolCb;
    OutputCB mOutputCb;
    ErrorCB mErrorCb;
    size_t mMinNumOutputBuffers = 0;
    bool mIsSecure = false;

    std::unique_ptr<VaapiDevice> mDevice;
    VAProfile mVAProfile = VAProfileNone;
    VAConfigID mVAConfig = VA_INVALID_ID;
    VAContextID mVAContext = VA_INVALID_ID;
    ui::Size mContextSize{0, 0};  // size the VA context/surfaces were created for (grows only)

    // Parameter-set stores keyed by id.
    std::map<uint32_t, H264SPS> mSPSes;
    std::map<uint32_t, H264PPS> mPPSes;
    std::map<uint32_t, H265SPS> mH265SPSes;
    std::map<uint32_t, H265PPS> mH265PPSes;

    // ---- HEVC decode state ----
    // prevTid0Pic POC halves (8.3.1); updated after each TemporalId==0
    // non-RASL/RADL/SLNR picture.
    int32_t mH265PrevTid0PocLsb = 0;
    int32_t mH265PrevTid0PocMsb = 0;
    // PicOrderCntVal of the picture currently being decoded.
    int32_t mH265CurrPoc = 0;
    // NoRaslOutputFlag handling: the first picture of the sequence (and every
    // IDR/BLA) starts a new prediction chain.
    bool mH265FirstPicture = true;
    // Set at each IRAP to its NoRaslOutputFlag: the RASL pictures that follow a
    // broken link (stream opened at a CRA) reference pictures that were never
    // decoded, so they are dropped rather than decoded from a garbage DPB.
    bool mH265SkipRasl = false;
    // The current picture's RPS, resolved against the DPB by deriveRpsHEVC().
    // Non-owning pointers into mDpb.
    std::vector<DpbEntry*> mRpsStCurrBefore;
    std::vector<DpbEntry*> mRpsStCurrAfter;
    std::vector<DpbEntry*> mRpsLtCurr;

    // ---- VP9 decode state (VaapiVideoDecoderVP9.cpp) ----
    // The eight reference slots; non-owning pointers into mDpb. An entry's
    // refUsed flag mirrors "referenced by any slot" and is recomputed after
    // every refresh, so the shared reclaim path frees evicted pictures.
    std::array<DpbEntry*, 8> mVp9Slots{};
    // Persistent inter-frame state (survives frames, reset by
    // setup_past_independence; see the spec's uncompressed_header()).
    struct Vp9Segmentation {
        bool enabled = false;
        bool updateMap = false;
        bool temporalUpdate = false;
        bool absDelta = false;
        uint8_t treeProbs[7] = {255, 255, 255, 255, 255, 255, 255};
        uint8_t predProbs[3] = {255, 255, 255};
        bool featureEnabled[8][4] = {};
        int16_t featureData[8][4] = {};
    } mVp9Seg;
    struct Vp9LoopFilter {
        uint8_t level = 0;
        uint8_t sharpness = 0;
        bool deltaEnabled = false;
        int8_t refDeltas[4] = {1, 0, -1, -1};
        int8_t modeDeltas[2] = {};
    } mVp9Lf;

    // ---- AV1 decode state (VaapiVideoDecoderAV1.cpp) ----
    // Same slot model as VP9. The libgav1 parser state (BufferPool +
    // DecoderState + cached sequence header) hides behind a pimpl with a
    // function-pointer deleter so libgav1 headers stay out of this one.
    std::array<DpbEntry*, 8> mAv1Slots{};
    // One-shot latch for the film-grain log line (per decoder instance).
    bool mAv1LoggedGrain = false;
    struct Av1Context;
    std::unique_ptr<Av1Context, void (*)(Av1Context*)> mAv1Ctx{nullptr, nullptr};

    // ---- Missing-reference skip (all codecs) ----
    // A predicted frame whose references never entered the DPB/slots (the
    // stream was joined mid-GOP, or decode resumed on a non-key frame after a
    // flush) must not reach the GPU: its reference entries would all be
    // VA_INVALID_SURFACE and the driver motion-compensates from whatever VRAM
    // it lands on — gray smears and stale-tile checkerboards on screen. Such
    // frames are skipped (input consumed, no output; the component's no-show
    // path completes their work) until a decodable picture arrives.
    void noteMissingRefSkip(const char* what);
    void noteMissingRefRecovery(const char* what);
    uint32_t mMissingRefSkips = 0;

    // The decoded picture buffer.
    std::vector<std::unique_ptr<DpbEntry>> mDpb;
    size_t mDpbMaxSize = 16;
    // MaxLongTermFrameIdx (8.2.5.4.4); -1 means "no long-term frame indices".
    int32_t mMaxLongTermFrameIdx = -1;

    // POC state (pic_order_cnt_type 0).
    int32_t mPrevPicOrderCntMsb = 0;
    int32_t mPrevPicOrderCntLsb = 0;
    // POC state (pic_order_cnt_type 1/2) + frame_num gap detection.
    uint32_t mPrevFrameNum = 0;
    int32_t mPrevFrameNumOffset = 0;
    bool mHasPrevRefFrame = false;

    ui::Size mCodedSize;
    Rect mVisibleRect;
    // The size the output VideoFramePool's gralloc buffers are allocated at.
    // Grows monotonically: a resolution change reuses the existing (>=) pool
    // instead of recreating it, so adaptive-stream switches don't stall waiting
    // for the framework to return the old buffers. Frames smaller than this are
    // written into the top-left and cropped via mVisibleRect.
    ui::Size mOutputPoolSize{0, 0};

    // Internal decode-target surfaces. Owned here; the DPB and pending-output
    // queue reference (never own) them. A surface is "in use" iff it is the
    // target of any DPB entry or any queued PendingOutput (see
    // acquireFreeSurface()).
    std::vector<VASurfaceID> mSurfacePool;

    // The gralloc output pool. Created lazily when the first frame size is known.
    std::unique_ptr<VideoFramePool> mVideoFramePool;
    // Cached per pool generation (identical for every buffer of one pool);
    // reset when the pool is recreated.
    std::optional<OutputBufferInfo> mOutputInfo;
    // Lazy VPP config/context for tiled-output blits (created on first use,
    // destroyed with the VA context; sized to the coded resolution).
    VAConfigID mVppConfig = VA_INVALID_ID;
    VAContextID mVppContext = VA_INVALID_ID;
    // Pictures decoded and awaiting a pool buffer, in display (POC) order.
    std::queue<PendingOutput> mPendingOutputs;
    // Whether a getVideoFrame() callback is currently outstanding.
    bool mOutputFetchInFlight = false;
    // Bumped whenever in-flight fetches become invalid (flush; the pool reset on
    // a mid-stream resolution change). A getVideoFrame() callback carrying a
    // stale generation is ignored: acting on it would pop a pending output it
    // no longer owns — and when the pool it was fetched from was destroyed, the
    // callback never fires at all, so the generation ALSO lets the reset path
    // clear mOutputFetchInFlight without a stale late callback corrupting the
    // relaunched pump (the wedge behind "audio plays, picture frozen" on
    // adaptive-stream resolution switches).
    uint64_t mFetchGeneration = 0;

    std::queue<DecodeRequest> mDecodeRequests;
    DecodeCB mDrainCb;

    State mState = State::Idle;

    ::base::WeakPtr<VaapiVideoDecoder> mWeakThis;
    ::base::WeakPtrFactory<VaapiVideoDecoder> mWeakThisFactory{this};
};

}  // namespace android

#endif  // ANDROID_C2_VA_VAAPI_VAAPI_VIDEO_DECODER_H
