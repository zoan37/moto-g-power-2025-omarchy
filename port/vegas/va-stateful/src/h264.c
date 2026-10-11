/*
 * H.264: VA-API picture -> Annex-B access unit for a stateful decoder.
 *
 * VA-API passes the slice NAL units (NAL header included) but not the
 * SPS/PPS. Both are synthesized here from VAPictureParameterBufferH264, the
 * IQ matrix and the slice parameters, and sent in front of a picture's
 * slices whenever their content changes:
 *  - SPS id 0, profile from the VA profile, level 5.1 (VA has no level), a
 *    VUI whose bitstream_restriction says max_num_reorder_frames = 0 so the
 *    firmware outputs every picture as soon as it is decoded (frames are
 *    matched to surfaces by timestamp, so the order doesn't matter, but the
 *    latency does: VA clients sync a surface right after decoding it);
 *  - PPS with the id parsed from the slice header; scaling lists are written
 *    explicitly; num_ref_idx_lX_default_active_minus1, which VA doesn't
 *    carry, is learned from slices that do not override it (parsed from the
 *    slice header, like libva-v4l2_request does).
 * Unsupported: interlaced (frame_mbs_only_flag = 0), POC type 1 (its offset
 * tables aren't in VA), 4:4:4 and > 8-bit (rejected at EndPicture); FMO
 * (slice groups, Baseline only, which VA clients don't decode) isn't
 * carried into the PPS.
 *
 * The slice header parser is libva-v4l2_request 1.3's, cut down to the
 * fields needed here. Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "mtk_stateful.h"
#include "bits.h"

enum {
	SLICE_P = 0, SLICE_B, SLICE_I, SLICE_SP, SLICE_SI,
};

#define MAX_PARAM_SET 512	/* RBSP bytes; a PPS with 8 scaling lists stays below */

struct h264_priv {
	VAPictureParameterBufferH264 pic;
	bool have_pic;
	VAIQMatrixBufferH264 iq;
	bool have_iq;

	VASliceParameterBufferH264 *slices;
	unsigned int nb_slices, alloc_slices, consumed;

	struct sd_bytes nals;		/* the picture's slice NAL units */
	unsigned int nb_nals;
	int pps_id;			/* from the first slice, -1 = none */
	uint8_t slice_l0, slice_l1;	/* first P/B slice's active counts */
	bool have_slice_counts;

	/* PPS default reference counts learned per PPS id. */
	struct {
		bool l0_known, l1_known;
		uint8_t l0, l1;
	} defaults[256];

	/* Last SPS/PPS sent (escaped NAL payload), to resend on change. */
	uint8_t sps[MAX_PARAM_SET * 2];
	size_t sps_len;
	uint8_t *pps[256];
	size_t pps_len[256];

	bool warned_offset;
	bool scaling_raster;
};

/* zigzag (coded) index -> raster position */
static const uint8_t zigzag4x4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

static const uint8_t zigzag8x8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* --- NAL helpers --- */

/* Append start code + NAL header + RBSP with emulation prevention. */
static int append_nal(struct sd_bytes *out, uint8_t header,
		      const uint8_t *rbsp, size_t len)
{
	static const uint8_t start[4] = { 0, 0, 0, 1 };
	uint8_t buf[MAX_PARAM_SET * 2 + 8];
	size_t n = 0;
	unsigned int zeros = 0;

	buf[n++] = header;
	for (size_t i = 0; i < len; i++) {
		if (zeros >= 2 && rbsp[i] <= 3) {
			buf[n++] = 3;
			zeros = 0;
		}
		buf[n++] = rbsp[i];
		zeros = rbsp[i] ? 0 : zeros + 1;
		if (n + 2 >= sizeof(buf))
			return -1;
	}

	if (sd_bytes_append(out, start, 4) < 0 ||
	    sd_bytes_append(out, buf, n) < 0)
		return -1;
	return 0;
}

static bool profile_is_high(VAProfile profile)
{
	return profile == VAProfileH264High;
}

/* --- SPS --- */

static size_t write_sps(struct v4l2r_context *ctx, uint8_t *rbsp, size_t size)
{
	struct h264_priv *h = ctx->codec_priv;
	const VAPictureParameterBufferH264 *pic = &h->pic;
	struct v4l2r_bitw w;
	unsigned int profile_idc, flags;

	switch (ctx->profile) {
	case VAProfileH264ConstrainedBaseline:
		profile_idc = 66;
		flags = 0xc0;	/* constraint_set0 + constraint_set1 */
		break;
	case VAProfileH264Main:
		profile_idc = 77;
		flags = 0x40;	/* constraint_set1 */
		break;
	default:
		profile_idc = 100;
		flags = 0;
		break;
	}

	v4l2r_bitw_init(&w, rbsp, size);
	v4l2r_bitw_write(&w, profile_idc, 8);
	v4l2r_bitw_write(&w, flags, 8);
	v4l2r_bitw_write(&w, 51, 8);			/* level_idc */
	v4l2r_bitw_ue(&w, 0);				/* seq_parameter_set_id */
	if (profile_idc == 100) {
		v4l2r_bitw_ue(&w, pic->seq_fields.bits.chroma_format_idc);
		v4l2r_bitw_ue(&w, pic->bit_depth_luma_minus8);
		v4l2r_bitw_ue(&w, pic->bit_depth_chroma_minus8);
		v4l2r_bitw_bit(&w, 0);	/* qpprime_y_zero_transform_bypass */
		v4l2r_bitw_bit(&w, 0);	/* seq_scaling_matrix_present (PPS has it) */
	}
	v4l2r_bitw_ue(&w, pic->seq_fields.bits.log2_max_frame_num_minus4);
	v4l2r_bitw_ue(&w, pic->seq_fields.bits.pic_order_cnt_type);
	if (pic->seq_fields.bits.pic_order_cnt_type == 0)
		v4l2r_bitw_ue(&w, pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
	v4l2r_bitw_ue(&w, pic->num_ref_frames);
	v4l2r_bitw_bit(&w, pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
	v4l2r_bitw_ue(&w, pic->picture_width_in_mbs_minus1);
	v4l2r_bitw_ue(&w, pic->picture_height_in_mbs_minus1);
	v4l2r_bitw_bit(&w, 1);				/* frame_mbs_only_flag */
	v4l2r_bitw_bit(&w, pic->seq_fields.bits.direct_8x8_inference_flag);
	v4l2r_bitw_bit(&w, 0);				/* frame_cropping_flag */

	v4l2r_bitw_bit(&w, 1);				/* vui_parameters_present */
	v4l2r_bitw_bit(&w, 0);	/* aspect_ratio_info_present */
	v4l2r_bitw_bit(&w, 0);	/* overscan_info_present */
	v4l2r_bitw_bit(&w, 0);	/* video_signal_type_present */
	v4l2r_bitw_bit(&w, 0);	/* chroma_loc_info_present */
	v4l2r_bitw_bit(&w, 0);	/* timing_info_present */
	v4l2r_bitw_bit(&w, 0);	/* nal_hrd_parameters_present */
	v4l2r_bitw_bit(&w, 0);	/* vcl_hrd_parameters_present */
	v4l2r_bitw_bit(&w, 0);	/* pic_struct_present */
	v4l2r_bitw_bit(&w, 1);	/* bitstream_restriction_flag */
	v4l2r_bitw_bit(&w, 1);	/* motion_vectors_over_pic_boundaries */
	v4l2r_bitw_ue(&w, 2);	/* max_bytes_per_pic_denom */
	v4l2r_bitw_ue(&w, 1);	/* max_bits_per_mb_denom */
	v4l2r_bitw_ue(&w, 16);	/* log2_max_mv_length_horizontal */
	v4l2r_bitw_ue(&w, 16);	/* log2_max_mv_length_vertical */
	v4l2r_bitw_ue(&w, 0);	/* max_num_reorder_frames */
	v4l2r_bitw_ue(&w, pic->num_ref_frames ? pic->num_ref_frames : 1);

	return w.error ? 0 : v4l2r_bitw_trailing(&w);
}

/* --- PPS --- */

static bool iq_is_flat(const VAIQMatrixBufferH264 *iq, bool with_8x8)
{
	for (unsigned int i = 0; i < 6; i++)
		for (unsigned int j = 0; j < 16; j++)
			if (iq->ScalingList4x4[i][j] != 16)
				return false;
	if (with_8x8)
		for (unsigned int i = 0; i < 2; i++)
			for (unsigned int j = 0; j < 64; j++)
				if (iq->ScalingList8x8[i][j] != 16)
					return false;
	return true;
}

static void write_scaling_list(struct v4l2r_bitw *w, const uint8_t *list,
			       const uint8_t *zigzag, unsigned int size,
			       bool raster)
{
	int last = 8;

	v4l2r_bitw_bit(w, 1);			/* *_scaling_list_present_flag */
	for (unsigned int j = 0; j < size; j++) {
		int value = raster ? list[zigzag[j]] : list[j];
		int delta = value - last;

		if (delta > 127)
			delta -= 256;
		if (delta < -128)
			delta += 256;
		v4l2r_bitw_se(w, delta);
		last = value;
	}
}

static size_t write_pps(struct v4l2r_context *ctx, uint8_t *rbsp, size_t size)
{
	struct h264_priv *h = ctx->codec_priv;
	const VAPictureParameterBufferH264 *pic = &h->pic;
	unsigned int id = h->pps_id;
	unsigned int l0 = h->defaults[id].l0_known ? h->defaults[id].l0 :
			  h->have_slice_counts ? h->slice_l0 : 0;
	unsigned int l1 = h->defaults[id].l1_known ? h->defaults[id].l1 :
			  h->have_slice_counts ? h->slice_l1 : 0;
	bool t8x8 = pic->pic_fields.bits.transform_8x8_mode_flag;
	bool matrix = h->have_iq && !iq_is_flat(&h->iq, t8x8);
	struct v4l2r_bitw w;

	v4l2r_bitw_init(&w, rbsp, size);
	v4l2r_bitw_ue(&w, id);
	v4l2r_bitw_ue(&w, 0);				/* seq_parameter_set_id */
	v4l2r_bitw_bit(&w, pic->pic_fields.bits.entropy_coding_mode_flag);
	v4l2r_bitw_bit(&w, pic->pic_fields.bits.pic_order_present_flag);
	v4l2r_bitw_ue(&w, 0);				/* num_slice_groups_minus1 */
	v4l2r_bitw_ue(&w, l0);
	v4l2r_bitw_ue(&w, l1);
	v4l2r_bitw_bit(&w, pic->pic_fields.bits.weighted_pred_flag);
	v4l2r_bitw_write(&w, pic->pic_fields.bits.weighted_bipred_idc, 2);
	v4l2r_bitw_se(&w, pic->pic_init_qp_minus26);
	v4l2r_bitw_se(&w, pic->pic_init_qs_minus26);
	v4l2r_bitw_se(&w, pic->chroma_qp_index_offset);
	v4l2r_bitw_bit(&w, pic->pic_fields.bits.deblocking_filter_control_present_flag);
	v4l2r_bitw_bit(&w, pic->pic_fields.bits.constrained_intra_pred_flag);
	v4l2r_bitw_bit(&w, pic->pic_fields.bits.redundant_pic_cnt_present_flag);

	/* The extension only exists in High profile PPSs. */
	if (profile_is_high(ctx->profile) &&
	    (t8x8 || matrix ||
	     pic->second_chroma_qp_index_offset != pic->chroma_qp_index_offset)) {
		v4l2r_bitw_bit(&w, t8x8);
		v4l2r_bitw_bit(&w, matrix);	/* pic_scaling_matrix_present */
		if (matrix) {
			for (unsigned int i = 0; i < 6; i++)
				write_scaling_list(&w, h->iq.ScalingList4x4[i],
						   zigzag4x4, 16, h->scaling_raster);
			if (t8x8)
				for (unsigned int i = 0; i < 2; i++)
					write_scaling_list(&w, h->iq.ScalingList8x8[i],
							   zigzag8x8, 64,
							   h->scaling_raster);
		}
		v4l2r_bitw_se(&w, pic->second_chroma_qp_index_offset);
	}

	return w.error ? 0 : v4l2r_bitw_trailing(&w);
}

/* --- slices --- */

struct slice_info {
	unsigned int nal_unit_type;
	unsigned int slice_type;
	unsigned int pps_id;
	bool override;
	bool valid;
};

/* Parse up to num_ref_idx_active_override_flag. */
static void parse_slice_header(const VAPictureParameterBufferH264 *pic,
			       const uint8_t *data, size_t size,
			       struct slice_info *info)
{
	struct v4l2r_bits b;

	memset(info, 0, sizeof(*info));
	v4l2r_bits_init(&b, data, size, true);

	v4l2r_bits_bit(&b);				/* forbidden_zero_bit */
	v4l2r_bits_read(&b, 2);				/* nal_ref_idc */
	info->nal_unit_type = v4l2r_bits_read(&b, 5);
	v4l2r_bits_ue(&b);				/* first_mb_in_slice */
	info->slice_type = v4l2r_bits_ue(&b) % 5;
	info->pps_id = v4l2r_bits_ue(&b);
	v4l2r_bits_read(&b, pic->seq_fields.bits.log2_max_frame_num_minus4 + 4);
	if (info->nal_unit_type == 5)
		v4l2r_bits_ue(&b);			/* idr_pic_id */
	if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
		v4l2r_bits_read(&b,
			pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pic->pic_fields.bits.pic_order_present_flag)
			v4l2r_bits_se(&b);	/* delta_pic_order_cnt_bottom */
	}
	if (pic->pic_fields.bits.redundant_pic_cnt_present_flag)
		v4l2r_bits_ue(&b);
	if (info->slice_type == SLICE_B)
		v4l2r_bits_bit(&b);		/* direct_spatial_mv_pred_flag */
	if (info->slice_type == SLICE_P || info->slice_type == SLICE_SP ||
	    info->slice_type == SLICE_B)
		info->override = v4l2r_bits_bit(&b);

	info->valid = !b.error && info->pps_id < 256;
}

static VAStatus process_slice(struct v4l2r_context *ctx,
			      const VASliceParameterBufferH264 *va_slice,
			      const uint8_t *data, size_t data_size)
{
	static const uint8_t start[3] = { 0, 0, 1 };
	struct h264_priv *h = ctx->codec_priv;
	const uint8_t *nal;
	size_t len;
	struct slice_info info;

	if (va_slice->slice_data_offset > data_size ||
	    va_slice->slice_data_size > data_size - va_slice->slice_data_offset)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	nal = data + va_slice->slice_data_offset;
	len = va_slice->slice_data_size;

	/* Some clients include the start code. */
	if (len >= 4 && !nal[0] && !nal[1] && !nal[2] && nal[3] == 1) {
		nal += 4;
		len -= 4;
	} else if (len >= 3 && !nal[0] && !nal[1] && nal[2] == 1) {
		nal += 3;
		len -= 3;
	}
	if (!len)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (!h->warned_offset) {
		unsigned int type = nal[0] & 0x1f;

		h->warned_offset = true;
		v4l2r_trace("first slice: NAL header 0x%02x (type %u), "
			    "slice_data_bit_offset %u, %zu bytes\n", nal[0], type,
			    va_slice->slice_data_bit_offset, len);
		if (type != 1 && type != 5)
			v4l2r_log("slice data does not start with a slice NAL "
				  "header (0x%02x)\n", nal[0]);
	}

	parse_slice_header(&h->pic, nal, len, &info);
	if (!info.valid) {
		v4l2r_log("cannot parse slice header\n");
		return VA_STATUS_ERROR_INVALID_BUFFER;
	}

	if (h->pps_id < 0)
		h->pps_id = info.pps_id;
	else if ((int)info.pps_id != h->pps_id)
		v4l2r_trace("slices of one picture use PPS %d and %u\n",
			    h->pps_id, info.pps_id);

	if (info.slice_type == SLICE_P || info.slice_type == SLICE_SP ||
	    info.slice_type == SLICE_B) {
		if (!h->have_slice_counts) {
			h->slice_l0 = va_slice->num_ref_idx_l0_active_minus1;
			h->slice_l1 = va_slice->num_ref_idx_l1_active_minus1;
			h->have_slice_counts = true;
		}
		/* A slice that doesn't override uses the PPS defaults. */
		if (!info.override) {
			h->defaults[info.pps_id].l0 =
				va_slice->num_ref_idx_l0_active_minus1;
			h->defaults[info.pps_id].l0_known = true;
			if (info.slice_type == SLICE_B) {
				h->defaults[info.pps_id].l1 =
					va_slice->num_ref_idx_l1_active_minus1;
				h->defaults[info.pps_id].l1_known = true;
			}
		}
	}

	if (sd_bytes_append(&h->nals, start, 3) < 0 ||
	    sd_bytes_append(&h->nals, nal, len) < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	h->nb_nals++;

	return VA_STATUS_SUCCESS;
}

/* --- codec ops --- */

static VAStatus h264_begin_picture(struct v4l2r_context *ctx)
{
	struct h264_priv *h = ctx->codec_priv;

	h->have_pic = false;
	h->nb_slices = 0;
	h->consumed = 0;
	h->nals.len = 0;
	h->nb_nals = 0;
	h->pps_id = -1;
	h->have_slice_counts = false;
	h->scaling_raster = !getenv("MTK_STATEFUL_SCALING_ZIGZAG");
	/* The IQ matrix persists: clients may send it only when it changes. */

	return VA_STATUS_SUCCESS;
}

static VAStatus h264_render_buffer(struct v4l2r_context *ctx,
				   struct v4l2r_buffer *buf)
{
	struct h264_priv *h = ctx->codec_priv;
	VAStatus status;

	switch (buf->type) {
	case VAPictureParameterBufferType:
		if (v4l2r_buffer_bytes(buf) < sizeof(h->pic))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&h->pic, buf->data, sizeof(h->pic));
		h->have_pic = true;
		return VA_STATUS_SUCCESS;

	case VAIQMatrixBufferType:
		if (v4l2r_buffer_bytes(buf) < sizeof(h->iq))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&h->iq, buf->data, sizeof(h->iq));
		h->have_iq = true;
		return VA_STATUS_SUCCESS;

	case VASliceParameterBufferType: {
		unsigned int count = buf->nb_elements;

		if (buf->element_size < sizeof(VASliceParameterBufferH264))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		if (h->nb_slices + count > h->alloc_slices) {
			unsigned int alloc = h->alloc_slices ? h->alloc_slices : 8;
			void *grown;

			while (alloc < h->nb_slices + count)
				alloc *= 2;
			grown = realloc(h->slices, alloc * sizeof(*h->slices));
			if (!grown)
				return VA_STATUS_ERROR_ALLOCATION_FAILED;
			h->slices = grown;
			h->alloc_slices = alloc;
		}
		for (unsigned int i = 0; i < count; i++)
			memcpy(&h->slices[h->nb_slices + i],
			       (uint8_t *)buf->data + (size_t)i * buf->element_size,
			       sizeof(*h->slices));
		h->nb_slices += count;
		return VA_STATUS_SUCCESS;
	}

	case VASliceDataBufferType:
		if (!h->have_pic)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		while (h->consumed < h->nb_slices) {
			status = process_slice(ctx, &h->slices[h->consumed],
					       buf->data, v4l2r_buffer_bytes(buf));
			if (status != VA_STATUS_SUCCESS)
				return status;
			h->consumed++;
		}
		return VA_STATUS_SUCCESS;

	default:
		return VA_STATUS_SUCCESS;	/* e.g. VAHuffmanTable: ignore */
	}
}

static VAStatus h264_end_picture(struct v4l2r_context *ctx)
{
	struct h264_priv *h = ctx->codec_priv;
	const VAPictureParameterBufferH264 *pic = &h->pic;
	uint8_t rbsp[MAX_PARAM_SET * 2];
	struct sd_bytes nal = {0};
	size_t len;

	if (!h->have_pic || !h->nb_nals || h->pps_id < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (!pic->seq_fields.bits.frame_mbs_only_flag ||
	    pic->seq_fields.bits.pic_order_cnt_type == 1 ||
	    pic->seq_fields.bits.chroma_format_idc != 1 ||
	    pic->bit_depth_luma_minus8 || pic->bit_depth_chroma_minus8) {
		v4l2r_log("unsupported H.264 stream (interlaced, POC type 1, "
			  "not 4:2:0 or not 8-bit)\n");
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	ctx->au.len = 0;
	ctx->expect_output = true;

	/* SPS: resend on change; a new SPS invalidates the PPS cache. */
	len = write_sps(ctx, rbsp, sizeof(rbsp));
	if (!len)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	if (append_nal(&nal, 0x67, rbsp, len) < 0)
		goto oom;
	if (nal.len != h->sps_len || memcmp(nal.data, h->sps, nal.len)) {
		if (nal.len > sizeof(h->sps))
			goto oom;
		memcpy(h->sps, nal.data, nal.len);
		h->sps_len = nal.len;
		for (unsigned int i = 0; i < 256; i++)
			h->pps_len[i] = 0;
		if (sd_bytes_append(&ctx->au, nal.data, nal.len) < 0)
			goto oom;
		v4l2r_trace("sending SPS (%zu bytes)\n", nal.len);
	}

	nal.len = 0;
	len = write_pps(ctx, rbsp, sizeof(rbsp));
	if (!len)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	if (append_nal(&nal, 0x68, rbsp, len) < 0)
		goto oom;
	if (nal.len != h->pps_len[h->pps_id] ||
	    memcmp(nal.data, h->pps[h->pps_id], nal.len)) {
		uint8_t *copy = realloc(h->pps[h->pps_id], nal.len);

		if (!copy)
			goto oom;
		memcpy(copy, nal.data, nal.len);
		h->pps[h->pps_id] = copy;
		h->pps_len[h->pps_id] = nal.len;
		if (sd_bytes_append(&ctx->au, nal.data, nal.len) < 0)
			goto oom;
		v4l2r_trace("sending PPS %d (%zu bytes)\n", h->pps_id, nal.len);
	}
	sd_bytes_free(&nal);

	if (sd_bytes_append(&ctx->au, h->nals.data, h->nals.len) < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	return VA_STATUS_SUCCESS;

oom:
	sd_bytes_free(&nal);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static void h264_uninit(struct v4l2r_context *ctx)
{
	struct h264_priv *h = ctx->codec_priv;

	free(h->slices);
	sd_bytes_free(&h->nals);
	for (unsigned int i = 0; i < 256; i++)
		free(h->pps[i]);
}

const struct sd_codec_ops sd_codec_h264 = {
	.priv_size = sizeof(struct h264_priv),
	.pixelformat = V4L2_PIX_FMT_H264,
	.begin_picture = h264_begin_picture,
	.render_buffer = h264_render_buffer,
	.end_picture = h264_end_picture,
	.uninit = h264_uninit,
};
