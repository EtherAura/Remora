// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// AV1 stateless decode core (remora-e5x.22).
//
// Unlike VP9, the bitstream layer is NOT hand-parsed: libgav1's ObuParser
// (external/libgav1, vendor_available, the same parser Android's software AV1
// decoder uses) digests each temporal unit into sequence/frame headers and
// tile spans, and this file only translates those into the VA-API AV1 decode
// structures. The reference model mirrors the VP9 core: eight slots holding
// non-owning DpbEntry pointers, refUsed recomputed per refresh so the shared
// reclaim/acquire machinery applies unchanged.
//
// Film grain: when a frame applies grain, the driver writes the pre-grain
// reconstruction (the REFERENCE image) into current_frame and the grainy
// DISPLAY image into current_display_picture. The recon picture lives in the
// DPB/slots; the display surface is only ever referenced by its PendingOutput
// and returns to the free pool as soon as the output copy completes.

#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <c2_va/vaapi/VaapiVideoDecoder.h>

#include <string.h>

#include <algorithm>

#include <log/log.h>
#include <utils/Trace.h>
#include <va/va.h>
#include <va/va_dec_av1.h>

#include "src/buffer_pool.h"
#include "src/decoder_state.h"
#include "src/obu_parser.h"

namespace android {

namespace {

constexpr int kAv1NumRefSlots = 8;

// libgav1 loop-restoration types -> VA restoration_type codes.
uint16_t restorationTypeToVa(libgav1::LoopRestorationType t) {
    switch (t) {
        case libgav1::kLoopRestorationTypeNone:
            return 0;
        case libgav1::kLoopRestorationTypeWiener:
            return 1;
        case libgav1::kLoopRestorationTypeSgrProj:
            return 2;
        case libgav1::kLoopRestorationTypeSwitchable:
            return 3;
        default:
            return 0;
    }
}

}  // namespace

// libgav1 parser state that persists across temporal units. The parser needs a
// BufferPool + DecoderState so inter frames can validate/resolve their
// references; UpdateReferenceFrames() keeps that state in sync with the slot
// refreshes we apply to our own DpbEntry map.
struct VaapiVideoDecoder::Av1Context {
    libgav1::BufferPool bufferPool{nullptr, nullptr, nullptr, nullptr};
    libgav1::DecoderState state;
    std::unique_ptr<libgav1::ObuSequenceHeader> seqHeader;
};

void VaapiVideoDecoder::av1ClearRefSlots() {
    mAv1Slots.fill(nullptr);
    if (mAv1Ctx) {
        for (int i = 0; i < kAv1NumRefSlots; ++i) {
            mAv1Ctx->state.reference_frame[i] = nullptr;
        }
    }
}

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeAV1Buffer(int32_t bitstreamId,
                                                                  const uint8_t* data,
                                                                  size_t size) {
    ATRACE_CALL();
    if (!mAv1Ctx) {
        mAv1Ctx = decltype(mAv1Ctx)(new Av1Context(), [](Av1Context* c) { delete c; });
    }

    // The csd buffer is an AV1CodecConfigurationRecord ("av1C"), not raw OBUs: MP4 and WebM both
    // carry the sequence header that way, and CCodec hands it to us as the first input buffer.
    // Its 4-byte header must come off or the parser rejects the whole buffer with
    // kStatusBitstreamError (-9) and the component dies before the first frame.
    // The test is unambiguous: byte 0 of the record is marker(1)=1 + version(7)=1 == 0x81, while
    // byte 0 of any real OBU has the forbidden bit clear, so the high bit can only be a record.
    constexpr size_t kAv1cHeaderSize = 4;
    if (size >= kAv1cHeaderSize && (data[0] & 0x80) != 0) {
        data += kAv1cHeaderSize;
        size -= kAv1cHeaderSize;
        if (size == 0) return FrameResult::kOk;  // header-only record: nothing to parse
    }

    libgav1::ObuParser parser(data, size, /*operating_point=*/0, &mAv1Ctx->bufferPool,
                              &mAv1Ctx->state);
    if (mAv1Ctx->seqHeader) parser.set_sequence_header(*mAv1Ctx->seqHeader);

    // A skipped frame (missing references) must not abort the rest of the
    // temporal unit, and libgav1's slot state still advances for it so parsing
    // stays consistent while our surfaces catch up at a key frame. The buffer
    // reports kSkipped only when nothing in it decoded or was shown, so its C2
    // work completes through the drop path instead of waiting for an output
    // that will never come.
    bool decodedAny = false;
    while (parser.HasData()) {
        libgav1::RefCountedBufferPtr currentFrame;
        const libgav1::StatusCode status = parser.ParseOneFrame(&currentFrame);
        if (status != libgav1::kStatusOk) {
            ALOGE("libgav1 ObuParser failed: %d", static_cast<int>(status));
            return FrameResult::kError;
        }
        if (parser.sequence_header_changed()) {
            mAv1Ctx->seqHeader =
                    std::make_unique<libgav1::ObuSequenceHeader>(parser.sequence_header());
        }
        if (!currentFrame) continue;  // metadata-only OBUs (temporal delimiter etc.)

        const libgav1::ObuFrameHeader& fh = parser.frame_header();

        if (fh.show_existing_frame) {
            const DpbEntry* e = mAv1Slots[fh.frame_to_show];
            if (!e) {
                // The frame this points at was skipped (or the stream was
                // joined mid-GOP): nothing to show, not a stream error. Keep
                // libgav1's state in step and move on.
                noteMissingRefSkip("AV1 show_existing_frame against an empty slot");
                mAv1Ctx->state.UpdateReferenceFrames(currentFrame,
                                                     static_cast<int>(fh.refresh_frame_flags));
                continue;
            }
            PendingOutput po;
            po.surface = e->surface;
            po.bitstreamId = bitstreamId;
            po.codedSize = e->codedSize;
            po.visibleRect = e->visibleRect;
            mPendingOutputs.push(po);
            pumpOutput();
            // A shown keyframe also refreshes all slots per spec; libgav1's
            // DecoderState took care of its side in ParseOneFrame.
            mAv1Ctx->state.UpdateReferenceFrames(currentFrame,
                                                 static_cast<int>(fh.refresh_frame_flags));
            decodedAny = true;
            continue;
        }

        const FrameResult r = decodeAv1Frame(bitstreamId, parser, data, size);
        if (r != FrameResult::kOk && r != FrameResult::kSkipped) return r;
        mAv1Ctx->state.UpdateReferenceFrames(currentFrame,
                                             static_cast<int>(fh.refresh_frame_flags));
        if (r == FrameResult::kOk) decodedAny = true;
    }
    return decodedAny ? FrameResult::kOk : FrameResult::kSkipped;
}

VaapiVideoDecoder::FrameResult VaapiVideoDecoder::decodeAv1Frame(int32_t bitstreamId,
                                                                 libgav1::ObuParser& parser,
                                                                 const uint8_t* data,
                                                                 size_t size) {
    ATRACE_CALL();
    if (!mAv1Ctx->seqHeader) {
        ALOGE("AV1 frame before any sequence header.");
        return FrameResult::kError;
    }
    const libgav1::ObuSequenceHeader& seq = *mAv1Ctx->seqHeader;
    const libgav1::ObuFrameHeader& fh = parser.frame_header();
    const libgav1::Vector<libgav1::TileBuffer>& tiles = parser.tile_buffers();

    if (seq.color_config.bitdepth != 8) {
        // 10-bit needs P010 decode surfaces; the internal pool is NV12-only
        // for now. Fail cleanly so the framework can fall back.
        ALOGE("AV1 bitdepth %d is not supported (8-bit only).", seq.color_config.bitdepth);
        return FrameResult::kError;
    }
    if (tiles.empty()) {
        ALOGE("AV1 frame carries no tiles.");
        return FrameResult::kError;
    }

    // An inter (or switch) frame predicting from empty slots (mid-GOP join, or
    // slots cleared by a flush) is skipped, not decoded from
    // VA_INVALID_SURFACE. libgav1's slot state still advances in the caller,
    // so parsing stays consistent while our surfaces catch up at a key frame.
    if (fh.frame_type != libgav1::kFrameKey && fh.frame_type != libgav1::kFrameIntraOnly) {
        for (int i = 0; i < 7; ++i) {
            if (!mAv1Slots[fh.reference_frame_index[i]]) {
                noteMissingRefSkip("AV1 inter frame referencing an empty slot");
                return FrameResult::kSkipped;
            }
        }
    }
    noteMissingRefRecovery("AV1");

    // Display geometry is the (possibly superres-upscaled) width.
    const uint32_t dispWidth = static_cast<uint32_t>(fh.upscaled_width);
    const uint32_t dispHeight = static_cast<uint32_t>(fh.height);
    const ui::Size alignedSize(static_cast<int>((dispWidth + 127) & ~127u),
                               static_cast<int>((dispHeight + 127) & ~127u));
    const ui::Size frameSize(static_cast<int>(dispWidth), static_cast<int>(dispHeight));
    const Rect visible(0, 0, static_cast<int>(dispWidth), static_cast<int>(dispHeight));
    if (!ensureVAContextForGeometry(alignedSize, visible, kAv1NumRefSlots)) {
        return FrameResult::kError;
    }

    const bool applyGrain = fh.film_grain_params.apply_grain;
    // Announce the grain path once per stream: it is the only path that needs a SECOND surface
    // (recon for the DPB, grainy image for display), so "did we take it?" is the first question
    // when a grainy stream misbehaves — and without this line the answer is unobservable.
    if (applyGrain && !mAv1LoggedGrain) {
        mAv1LoggedGrain = true;
        ALOGI("AV1 film grain active (seed=%u, %u luma points) — decoding into a separate display "
              "surface.",
              fh.film_grain_params.grain_seed, fh.film_grain_params.num_y_points);
    }
    const VASurfaceID target = acquireFreeSurface();
    if (target == VA_INVALID_SURFACE) return FrameResult::kNoSurface;
    VASurfaceID display = target;
    if (applyGrain) {
        // The grainy display image goes to its own surface; |target| keeps the
        // pre-grain reconstruction the DPB references. Reserve the display
        // surface by queueing nothing yet — acquire scans mDpb + pending, so
        // park it in a provisional pending entry only AFTER submission
        // succeeds; between the two acquires nothing can steal it because
        // this runs single-threaded on the decoder sequence.
        // A second surface must exist: park target in a temp DPB entry first.
        auto probe = std::make_unique<DpbEntry>();
        probe->surface = target;
        probe->refUsed = true;  // guard against re-acquisition
        mDpb.push_back(std::move(probe));
        display = acquireFreeSurface();
        mDpb.pop_back();
        if (display == VA_INVALID_SURFACE) return FrameResult::kNoSurface;
    }

    VADisplay dpy = mDevice->display();

    VADecPictureParameterBufferAV1 pic;
    memset(&pic, 0, sizeof(pic));
    pic.profile = static_cast<uint8_t>(seq.profile);
    pic.order_hint_bits_minus_1 =
            seq.enable_order_hint ? static_cast<uint8_t>(seq.order_hint_bits - 1) : 0;
    pic.bit_depth_idx = 0;  // 8-bit gated above
    pic.matrix_coefficients = static_cast<uint8_t>(seq.color_config.matrix_coefficients);

    pic.seq_info_fields.fields.still_picture = seq.still_picture;
    pic.seq_info_fields.fields.use_128x128_superblock = seq.use_128x128_superblock;
    pic.seq_info_fields.fields.enable_filter_intra = seq.enable_filter_intra;
    pic.seq_info_fields.fields.enable_intra_edge_filter = seq.enable_intra_edge_filter;
    pic.seq_info_fields.fields.enable_interintra_compound = seq.enable_interintra_compound;
    pic.seq_info_fields.fields.enable_masked_compound = seq.enable_masked_compound;
    pic.seq_info_fields.fields.enable_dual_filter = seq.enable_dual_filter;
    pic.seq_info_fields.fields.enable_order_hint = seq.enable_order_hint;
    pic.seq_info_fields.fields.enable_jnt_comp = seq.enable_jnt_comp;
    pic.seq_info_fields.fields.enable_cdef = seq.enable_cdef;
    pic.seq_info_fields.fields.mono_chrome = seq.color_config.is_monochrome;
    pic.seq_info_fields.fields.color_range = seq.color_config.color_range;
    pic.seq_info_fields.fields.subsampling_x = seq.color_config.subsampling_x;
    pic.seq_info_fields.fields.subsampling_y = seq.color_config.subsampling_y;
    pic.seq_info_fields.fields.film_grain_params_present = seq.film_grain_params_present;

    pic.current_frame = target;
    pic.current_display_picture = display;
    pic.anchor_frames_num = 0;
    pic.anchor_frames_list = nullptr;
    pic.frame_width_minus1 = static_cast<uint16_t>(dispWidth - 1);
    pic.frame_height_minus1 = static_cast<uint16_t>(dispHeight - 1);
    pic.output_frame_width_in_tiles_minus_1 = 0;
    pic.output_frame_height_in_tiles_minus_1 = 0;

    for (int i = 0; i < kAv1NumRefSlots; ++i) {
        pic.ref_frame_map[i] = mAv1Slots[i] ? mAv1Slots[i]->surface : VA_INVALID_SURFACE;
    }
    for (int i = 0; i < 7; ++i) {
        pic.ref_frame_idx[i] = static_cast<uint8_t>(fh.reference_frame_index[i]);
    }
    pic.primary_ref_frame = static_cast<uint8_t>(fh.primary_reference_frame);
    pic.order_hint = fh.order_hint;

    // Segmentation.
    const libgav1::Segmentation& seg = fh.segmentation;
    pic.seg_info.segment_info_fields.bits.enabled = seg.enabled;
    pic.seg_info.segment_info_fields.bits.update_map = seg.update_map;
    pic.seg_info.segment_info_fields.bits.temporal_update = seg.temporal_update;
    pic.seg_info.segment_info_fields.bits.update_data = seg.update_data;
    for (int s = 0; s < libgav1::kMaxSegments; ++s) {
        uint8_t mask = 0;
        for (int f = 0; f < libgav1::kSegmentFeatureMax; ++f) {
            pic.seg_info.feature_data[s][f] = seg.feature_data[s][f];
            if (seg.feature_enabled[s][f]) mask |= 1u << f;
        }
        pic.seg_info.feature_mask[s] = mask;
    }

    // Film grain.
    const libgav1::FilmGrainParams& fg = fh.film_grain_params;
    pic.film_grain_info.film_grain_info_fields.bits.apply_grain = fg.apply_grain;
    pic.film_grain_info.film_grain_info_fields.bits.chroma_scaling_from_luma =
            fg.chroma_scaling_from_luma;
    pic.film_grain_info.film_grain_info_fields.bits.grain_scaling_minus_8 =
            fg.chroma_scaling - 8;
    pic.film_grain_info.film_grain_info_fields.bits.ar_coeff_lag = fg.auto_regression_coeff_lag;
    pic.film_grain_info.film_grain_info_fields.bits.ar_coeff_shift_minus_6 =
            fg.auto_regression_shift - 6;
    pic.film_grain_info.film_grain_info_fields.bits.grain_scale_shift = fg.grain_scale_shift;
    pic.film_grain_info.film_grain_info_fields.bits.overlap_flag = fg.overlap_flag;
    pic.film_grain_info.film_grain_info_fields.bits.clip_to_restricted_range =
            fg.clip_to_restricted_range;
    pic.film_grain_info.grain_seed = fg.grain_seed;
    pic.film_grain_info.num_y_points = fg.num_y_points;
    for (int i = 0; i < fg.num_y_points; ++i) {
        pic.film_grain_info.point_y_value[i] = fg.point_y_value[i];
        pic.film_grain_info.point_y_scaling[i] = fg.point_y_scaling[i];
    }
    pic.film_grain_info.num_cb_points = fg.num_u_points;
    for (int i = 0; i < fg.num_u_points; ++i) {
        pic.film_grain_info.point_cb_value[i] = fg.point_u_value[i];
        pic.film_grain_info.point_cb_scaling[i] = fg.point_u_scaling[i];
    }
    pic.film_grain_info.num_cr_points = fg.num_v_points;
    for (int i = 0; i < fg.num_v_points; ++i) {
        pic.film_grain_info.point_cr_value[i] = fg.point_v_value[i];
        pic.film_grain_info.point_cr_scaling[i] = fg.point_v_scaling[i];
    }
    for (int i = 0; i < 24; ++i) {
        pic.film_grain_info.ar_coeffs_y[i] = fg.auto_regression_coeff_y[i];
    }
    for (int i = 0; i < 25; ++i) {
        pic.film_grain_info.ar_coeffs_cb[i] = fg.auto_regression_coeff_u[i];
        pic.film_grain_info.ar_coeffs_cr[i] = fg.auto_regression_coeff_v[i];
    }
    // libgav1 stores these biased (cb_mult - 128 etc.); VA wants raw values.
    pic.film_grain_info.cb_mult = static_cast<uint8_t>(fg.u_multiplier + 128);
    pic.film_grain_info.cb_luma_mult = static_cast<uint8_t>(fg.u_luma_multiplier + 128);
    pic.film_grain_info.cb_offset = static_cast<uint16_t>(fg.u_offset + 256);
    pic.film_grain_info.cr_mult = static_cast<uint8_t>(fg.v_multiplier + 128);
    pic.film_grain_info.cr_luma_mult = static_cast<uint8_t>(fg.v_luma_multiplier + 128);
    pic.film_grain_info.cr_offset = static_cast<uint16_t>(fg.v_offset + 256);

    // Tiles.
    const libgav1::TileInfo& ti = fh.tile_info;
    pic.tile_cols = static_cast<uint8_t>(ti.tile_columns);
    pic.tile_rows = static_cast<uint8_t>(ti.tile_rows);
    if (!ti.uniform_spacing) {
        for (int i = 0; i < ti.tile_columns && i < 63; ++i) {
            pic.width_in_sbs_minus_1[i] =
                    static_cast<uint16_t>(ti.tile_column_width_in_superblocks[i] - 1);
        }
        for (int i = 0; i < ti.tile_rows && i < 63; ++i) {
            pic.height_in_sbs_minus_1[i] =
                    static_cast<uint16_t>(ti.tile_row_height_in_superblocks[i] - 1);
        }
    }
    pic.context_update_tile_id = static_cast<uint16_t>(ti.context_update_id);

    pic.pic_info_fields.bits.frame_type = static_cast<uint32_t>(fh.frame_type);
    pic.pic_info_fields.bits.show_frame = fh.show_frame;
    pic.pic_info_fields.bits.showable_frame = fh.showable_frame;
    pic.pic_info_fields.bits.error_resilient_mode = fh.error_resilient_mode;
    pic.pic_info_fields.bits.disable_cdf_update = fh.enable_cdf_update ? 0 : 1;
    pic.pic_info_fields.bits.allow_screen_content_tools = fh.allow_screen_content_tools;
    pic.pic_info_fields.bits.force_integer_mv = fh.force_integer_mv;
    pic.pic_info_fields.bits.allow_intrabc = fh.allow_intrabc;
    pic.pic_info_fields.bits.use_superres = fh.use_superres;
    pic.pic_info_fields.bits.allow_high_precision_mv = fh.allow_high_precision_mv;
    pic.pic_info_fields.bits.is_motion_mode_switchable = fh.is_motion_mode_switchable;
    pic.pic_info_fields.bits.use_ref_frame_mvs = fh.use_ref_frame_mvs;
    pic.pic_info_fields.bits.disable_frame_end_update_cdf =
            fh.enable_frame_end_update_cdf ? 0 : 1;
    pic.pic_info_fields.bits.uniform_tile_spacing_flag = ti.uniform_spacing;
    pic.pic_info_fields.bits.allow_warped_motion = fh.allow_warped_motion;
    pic.pic_info_fields.bits.large_scale_tile = 0;

    pic.superres_scale_denominator = static_cast<uint8_t>(
            fh.use_superres ? fh.superres_scale_denominator : 8);
    pic.interp_filter = static_cast<uint8_t>(fh.interpolation_filter);

    // Loop filter.
    const libgav1::LoopFilter& lf = fh.loop_filter;
    pic.filter_level[0] = static_cast<uint8_t>(lf.level[0]);
    pic.filter_level[1] = static_cast<uint8_t>(lf.level[1]);
    pic.filter_level_u = static_cast<uint8_t>(lf.level[2]);
    pic.filter_level_v = static_cast<uint8_t>(lf.level[3]);
    pic.loop_filter_info_fields.bits.sharpness_level = lf.sharpness;
    pic.loop_filter_info_fields.bits.mode_ref_delta_enabled = lf.delta_enabled;
    pic.loop_filter_info_fields.bits.mode_ref_delta_update = lf.delta_update;
    for (int i = 0; i < 8; ++i) pic.ref_deltas[i] = lf.ref_deltas[i];
    for (int i = 0; i < 2; ++i) pic.mode_deltas[i] = lf.mode_deltas[i];

    // Quantization.
    const libgav1::QuantizerParameters& q = fh.quantizer;
    pic.base_qindex = q.base_index;
    pic.y_dc_delta_q = q.delta_dc[0];
    pic.u_dc_delta_q = q.delta_dc[1];
    pic.u_ac_delta_q = q.delta_ac[1];
    pic.v_dc_delta_q = q.delta_dc[2];
    pic.v_ac_delta_q = q.delta_ac[2];
    pic.qmatrix_fields.bits.using_qmatrix = q.use_matrix;
    if (q.use_matrix) {
        pic.qmatrix_fields.bits.qm_y = q.matrix_level[0];
        pic.qmatrix_fields.bits.qm_u = q.matrix_level[1];
        pic.qmatrix_fields.bits.qm_v = q.matrix_level[2];
    }

    pic.mode_control_fields.bits.delta_q_present_flag = fh.delta_q.present;
    pic.mode_control_fields.bits.log2_delta_q_res = fh.delta_q.scale;
    pic.mode_control_fields.bits.delta_lf_present_flag = fh.delta_lf.present;
    pic.mode_control_fields.bits.log2_delta_lf_res = fh.delta_lf.scale;
    pic.mode_control_fields.bits.delta_lf_multi = fh.delta_lf.multi;
    pic.mode_control_fields.bits.tx_mode = static_cast<uint32_t>(fh.tx_mode);
    pic.mode_control_fields.bits.reference_select = fh.reference_mode_select;
    pic.mode_control_fields.bits.reduced_tx_set_used = fh.reduced_tx_set;
    pic.mode_control_fields.bits.skip_mode_present = fh.skip_mode_present;

    // CDEF. Secondary strength 4 is coded as 3 per the packing contract.
    const libgav1::Cdef& cdef = fh.cdef;
    pic.cdef_damping_minus_3 = static_cast<uint8_t>(cdef.damping - 3);
    pic.cdef_bits = cdef.bits;
    for (int i = 0; i < 8; ++i) {
        uint8_t sec = cdef.y_secondary_strength[i];
        if (sec == 4) sec = 3;
        pic.cdef_y_strengths[i] =
                static_cast<uint8_t>((cdef.y_primary_strength[i] << 2) | (sec & 0x3));
        sec = cdef.uv_secondary_strength[i];
        if (sec == 4) sec = 3;
        pic.cdef_uv_strengths[i] =
                static_cast<uint8_t>((cdef.uv_primary_strength[i] << 2) | (sec & 0x3));
    }

    // Loop restoration.
    const libgav1::LoopRestoration& lr = fh.loop_restoration;
    pic.loop_restoration_fields.bits.yframe_restoration_type = restorationTypeToVa(lr.type[0]);
    pic.loop_restoration_fields.bits.cbframe_restoration_type = restorationTypeToVa(lr.type[1]);
    pic.loop_restoration_fields.bits.crframe_restoration_type = restorationTypeToVa(lr.type[2]);
    const bool anyLr = lr.type[0] != libgav1::kLoopRestorationTypeNone ||
                       lr.type[1] != libgav1::kLoopRestorationTypeNone ||
                       lr.type[2] != libgav1::kLoopRestorationTypeNone;
    if (anyLr) {
        pic.loop_restoration_fields.bits.lr_unit_shift = lr.unit_size_log2[0] - 6;
        pic.loop_restoration_fields.bits.lr_uv_shift =
                lr.unit_size_log2[0] - lr.unit_size_log2[1];
    }

    // Global motion, LAST..ALTREF.
    for (int i = 0; i < 7; ++i) {
        const libgav1::GlobalMotion& gm =
                fh.global_motion[libgav1::kReferenceFrameLast + i];
        pic.wm[i].wmtype = static_cast<VAAV1TransformationType>(gm.type);
        for (int p = 0; p < 6; ++p) pic.wm[i].wmmat[p] = gm.params[p];
        pic.wm[i].wmmat[6] = 0;
        pic.wm[i].wmmat[7] = 0;
        pic.wm[i].invalid = 0;
    }

    // Per-tile slice parameters; the slice data buffer carries the whole
    // temporal unit and each tile points into it by offset.
    const uint8_t* base = data;
    std::vector<VASliceParameterBufferAV1> sliceParams(tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i) {
        VASliceParameterBufferAV1* sp = &sliceParams[i];
        memset(sp, 0, sizeof(*sp));
        sp->slice_data_size = static_cast<uint32_t>(tiles[i].size);
        sp->slice_data_offset = static_cast<uint32_t>(tiles[i].data - base);
        sp->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        sp->tile_row = static_cast<uint16_t>(i / ti.tile_columns);
        sp->tile_column = static_cast<uint16_t>(i % ti.tile_columns);
        sp->tg_start = static_cast<uint16_t>(i);
        sp->tg_end = static_cast<uint16_t>(i);
    }

    std::vector<VABufferID> buffers;
    VAStatus status = VA_STATUS_SUCCESS;
    const auto makeBuffer = [&](VABufferType type, unsigned int sz, unsigned int count,
                                void* payload) {
        VABufferID id = VA_INVALID_ID;
        if (status != VA_STATUS_SUCCESS) return;
        status = vaCreateBuffer(dpy, mVAContext, type, sz, count, payload, &id);
        if (status == VA_STATUS_SUCCESS) buffers.push_back(id);
    };
    makeBuffer(VAPictureParameterBufferType, sizeof(pic), 1, &pic);
    makeBuffer(VASliceParameterBufferType, sizeof(VASliceParameterBufferAV1),
               static_cast<unsigned int>(sliceParams.size()), sliceParams.data());
    makeBuffer(VASliceDataBufferType, static_cast<unsigned int>(size), 1,
               const_cast<uint8_t*>(base));

    bool ok = status == VA_STATUS_SUCCESS;
    if (!ok) ALOGE("vaCreateBuffer(AV1) failed: %s", vaErrorStr(status));

    if (ok) {
        status = vaBeginPicture(dpy, mVAContext, target);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaBeginPicture(AV1) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    if (ok) {
        status = vaRenderPicture(dpy, mVAContext, buffers.data(),
                                 static_cast<int>(buffers.size()));
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaRenderPicture(AV1) failed: %s", vaErrorStr(status));
            ok = false;
        }
        status = vaEndPicture(dpy, mVAContext);
        if (status != VA_STATUS_SUCCESS) {
            ALOGE("vaEndPicture(AV1) failed: %s", vaErrorStr(status));
            ok = false;
        }
    }
    for (VABufferID b : buffers) vaDestroyBuffer(dpy, b);
    if (!ok) return FrameResult::kError;

    // Track the reconstruction and refresh the slots.
    auto entry = std::make_unique<DpbEntry>();
    entry->surface = target;
    entry->bitstreamId = bitstreamId;
    entry->codedSize = frameSize;
    entry->visibleRect = visible;
    // The recon of a grainy frame is never displayed itself; a plain hidden
    // frame is never displayed either. Both are born outputted.
    entry->outputted = !fh.show_frame || applyGrain;
    DpbEntry* raw = entry.get();
    mDpb.push_back(std::move(entry));

    for (int i = 0; i < kAv1NumRefSlots; ++i) {
        if (fh.refresh_frame_flags & (1u << i)) mAv1Slots[i] = raw;
    }
    for (auto& e : mDpb) {
        bool used = false;
        for (const DpbEntry* slot : mAv1Slots) {
            if (slot == e.get()) {
                used = true;
                break;
            }
        }
        // VP9 and AV1 are mutually exclusive per instance; only one slot array
        // is ever populated.
        for (const DpbEntry* slot : mVp9Slots) {
            if (slot == e.get()) {
                used = true;
                break;
            }
        }
        e->refUsed = used;
    }

    if (fh.show_frame) {
        if (applyGrain) {
            PendingOutput po;
            po.surface = display;
            po.bitstreamId = bitstreamId;
            po.codedSize = frameSize;
            po.visibleRect = visible;
            mPendingOutputs.push(po);
            pumpOutput();
        } else {
            raw->outputted = true;
            queueForOutput(*raw);
        }
    }

    reclaimDpbEntries();
    return FrameResult::kOk;
}

}  // namespace android
