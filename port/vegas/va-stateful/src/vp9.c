/*
 * VP9 profile 0: VA-API clients (Chrome, FFmpeg) pass each frame whole,
 * uncompressed header included, as slice data, so it goes to the decoder
 * unchanged, one frame per OUTPUT buffer. Superframes are split by the
 * client; show_existing_frame is handled by the client without a decode.
 * A hidden frame (show_frame = 0, e.g. an alt-ref) produces no output frame
 * on a stateful decoder: its surface is not waited for (its content stays
 * undefined unless the firmware returns the frame anyway).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "mtk_stateful.h"

struct vp9_priv {
	VADecPictureParameterBufferVP9 pic;
	bool have_pic;
	VASliceParameterBufferVP9 slice;
	bool have_slice;
};

static VAStatus vp9_begin_picture(struct v4l2r_context *ctx)
{
	struct vp9_priv *p = ctx->codec_priv;

	p->have_pic = false;
	p->have_slice = false;
	return VA_STATUS_SUCCESS;
}

static VAStatus vp9_render_buffer(struct v4l2r_context *ctx,
				  struct v4l2r_buffer *buf)
{
	struct vp9_priv *p = ctx->codec_priv;

	switch (buf->type) {
	case VAPictureParameterBufferType:
		if (v4l2r_buffer_bytes(buf) < sizeof(p->pic))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&p->pic, buf->data, sizeof(p->pic));
		p->have_pic = true;
		if (p->pic.profile != 0 || p->pic.bit_depth > 8) {
			v4l2r_log("VP9 profile %u, %u-bit is not supported\n",
				  p->pic.profile, p->pic.bit_depth);
			return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
		}
		return VA_STATUS_SUCCESS;

	case VASliceParameterBufferType:
		if (buf->element_size < sizeof(p->slice) || !buf->nb_elements)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&p->slice, buf->data, sizeof(p->slice));
		p->have_slice = true;
		return VA_STATUS_SUCCESS;

	case VASliceDataBufferType: {
		size_t size = v4l2r_buffer_bytes(buf);
		uint32_t offset = p->have_slice ? p->slice.slice_data_offset : 0;
		uint32_t len = p->have_slice ? p->slice.slice_data_size :
					      (uint32_t)size;

		if (offset > size || len > size - offset || !len)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		if (sd_bytes_append(&ctx->au, (uint8_t *)buf->data + offset,
				    len) < 0)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		return VA_STATUS_SUCCESS;
	}

	default:
		return VA_STATUS_SUCCESS;
	}
}

static VAStatus vp9_end_picture(struct v4l2r_context *ctx)
{
	struct vp9_priv *p = ctx->codec_priv;

	if (!p->have_pic || !ctx->au.len)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	ctx->expect_output = p->pic.pic_fields.bits.show_frame;
	return VA_STATUS_SUCCESS;
}

const struct sd_codec_ops sd_codec_vp9 = {
	.priv_size = sizeof(struct vp9_priv),
	.pixelformat = V4L2_PIX_FMT_VP9,
	.begin_picture = vp9_begin_picture,
	.render_buffer = vp9_render_buffer,
	.end_picture = vp9_end_picture,
	.uninit = NULL,
};
