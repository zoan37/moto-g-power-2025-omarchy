/*
 * Images: zero-copy DeriveImage on top of the surface's mmap()ed NV12
 * backing, plus copying GetImage/PutImage (from libva-v4l2_request 1.3,
 * reduced to NV12).
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "mtk_stateful.h"


/* Memory view of a surface's NV12 backing (allocated on demand). */
struct v4l2r_frame_view {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	unsigned int nb_planes;
	uint32_t plane_size[1];
	void *map[1];
};

static VAStatus surface_view(struct v4l2r_surface *surface,
			     struct v4l2r_frame_view *view)
{
	VAStatus status = v4l2r_surface_backing(surface);

	if (status != VA_STATUS_SUCCESS)
		return status;

	view->width = surface->backing->width;
	view->height = surface->backing->height;
	view->pitch = surface->backing->pitch;
	view->nb_planes = 1;
	view->plane_size[0] = surface->backing->size;
	view->map[0] = surface->backing->map;
	return VA_STATUS_SUCCESS;
}

static const VAImageFormat image_formats[] = {
	{
		.fourcc = VA_FOURCC_NV12,
		.byte_order = VA_LSB_FIRST,
		.bits_per_pixel = 12,
	},
};

VAStatus v4l2r_QueryImageFormats(VADriverContextP va_ctx, VAImageFormat *formats,
				 int *num_formats)
{
	(void)va_ctx;

	for (unsigned int i = 0; i < sizeof(image_formats) / sizeof(image_formats[0]); i++)
		formats[i] = image_formats[i];

	*num_formats = sizeof(image_formats) / sizeof(image_formats[0]);
	return VA_STATUS_SUCCESS;
}

static VAStatus image_setup_layout(VAImage *image, uint32_t fourcc,
				   unsigned int width, unsigned int height,
				   unsigned int pitch, unsigned int chroma_offset)
{
	switch (fourcc) {
	case VA_FOURCC_NV12:
		image->num_planes = 2;
		image->pitches[0] = pitch;
		image->offsets[0] = 0;
		image->pitches[1] = pitch;
		image->offsets[1] = chroma_offset;
		image->data_size = chroma_offset + pitch * ((height + 1) / 2);
		break;
	default:
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	image->width = width;
	image->height = height;
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_CreateImage(VADriverContextP va_ctx, VAImageFormat *format,
			   int width, int height, VAImage *image)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_image *image_object;
	unsigned int pitch, chroma_offset;
	VAImageID image_id;
	VABufferID buffer_id;
	VAStatus status;

	if (format->fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	pthread_mutex_lock(&drv->mutex);
	image_id = v4l2r_handles_alloc(&drv->images, sizeof(*image_object));
	image_object = V4L2R_IMAGE(drv, image_id);
	pthread_mutex_unlock(&drv->mutex);
	if (!image_object)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	memset(image, 0, sizeof(*image));
	image->image_id = image_id;
	image->format = *format;

	pitch = ((unsigned int)width + 63) & ~63u;
	chroma_offset = pitch * height;

	status = image_setup_layout(image, format->fourcc, width, height,
				    pitch, chroma_offset);
	if (status != VA_STATUS_SUCCESS)
		goto fail;

	status = v4l2r_CreateBuffer(va_ctx, VA_INVALID_ID, VAImageBufferType,
				    image->data_size, 1, NULL, &buffer_id);
	if (status != VA_STATUS_SUCCESS)
		goto fail;

	image->buf = buffer_id;
	image_object->image = *image;

	return VA_STATUS_SUCCESS;

fail:
	pthread_mutex_lock(&drv->mutex);
	v4l2r_handles_free(&drv->images, image_id);
	pthread_mutex_unlock(&drv->mutex);
	return status;
}

VAStatus v4l2r_DeriveImage(VADriverContextP va_ctx, VASurfaceID surface_id,
			   VAImage *image)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;
	struct v4l2r_image *image_object;
	struct v4l2r_buffer *buffer;
	struct v4l2r_frame_view view;
	VAImageID image_id;
	VABufferID buffer_id;
	VAStatus status;

	surface = V4L2R_SURFACE_GET(drv, surface_id);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/* Wait for a pending decode into the surface first. */
	status = v4l2r_surface_ready(surface);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = surface_view(surface, &view);
	if (status != VA_STATUS_SUCCESS)
		return status;


	/* Zero-copy is only possible when luma and chroma share one plane. */
	if (view.nb_planes != 1)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	pthread_mutex_lock(&drv->mutex);
	image_id = v4l2r_handles_alloc(&drv->images, sizeof(*image_object));
	image_object = V4L2R_IMAGE(drv, image_id);
	if (!image_object) {
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	buffer_id = v4l2r_handles_alloc(&drv->buffers, sizeof(*buffer));
	buffer = V4L2R_BUFFER(drv, buffer_id);
	if (!buffer) {
		v4l2r_handles_free(&drv->images, image_id);
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	pthread_mutex_unlock(&drv->mutex);

	memset(image, 0, sizeof(*image));
	image->image_id = image_id;
	image->format.fourcc = VA_FOURCC_NV12;
	image->format.byte_order = VA_LSB_FIRST;
	image->format.bits_per_pixel = 12;

	/*
	 * The backing may be larger than the surface (decoders align their
	 * CAPTURE resolution, and impose a minimum on standalone backing).
	 * Report the surface size, with the pitch and the chroma offset
	 * describing the real memory layout: FFmpeg rejects vaDeriveImage
	 * for all surfaces of a pool when the image size does not match the
	 * pool, and falls back to the much slower Get/PutImage path.
	 */
	status = image_setup_layout(image, VA_FOURCC_NV12,
				    surface->width < view.width ?
					surface->width : view.width,
				    surface->height < view.height ?
					surface->height : view.height,
				    view.pitch, view.pitch * view.height);
	if (status != VA_STATUS_SUCCESS) {
		pthread_mutex_lock(&drv->mutex);
		v4l2r_handles_free(&drv->buffers, buffer_id);
		v4l2r_handles_free(&drv->images, image_id);
		pthread_mutex_unlock(&drv->mutex);
		return status;
	}

	if (image->data_size > view.plane_size[0])
		image->data_size = view.plane_size[0];

	buffer->type = VAImageBufferType;
	buffer->element_size = image->data_size;
	buffer->nb_elements = 1;
	buffer->data = view.map[0];
	buffer->derived = true;

	image->buf = buffer_id;
	image_object->image = *image;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_DestroyImage(VADriverContextP va_ctx, VAImageID image_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_image *image_object;
	VABufferID buffer_id;

	pthread_mutex_lock(&drv->mutex);
	image_object = V4L2R_IMAGE(drv, image_id);
	if (!image_object) {
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_INVALID_IMAGE;
	}
	buffer_id = image_object->image.buf;
	v4l2r_handles_free(&drv->images, image_id);
	pthread_mutex_unlock(&drv->mutex);

	return v4l2r_DestroyBuffer(va_ctx, buffer_id);
}

VAStatus v4l2r_SetImagePalette(VADriverContextP va_ctx, VAImageID image,
			       unsigned char *palette)
{
	(void)va_ctx; (void)image; (void)palette;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_GetImage(VADriverContextP va_ctx, VASurfaceID surface_id,
			int x, int y, unsigned int width, unsigned int height,
			VAImageID image_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;
	struct v4l2r_image *image_object;
	struct v4l2r_buffer *buffer;
	struct v4l2r_frame_view view;
	VAImage *image;
	VAStatus status;
	const uint8_t *src_luma, *src_chroma;
	uint8_t *dst;
	unsigned int row_size;

	surface = V4L2R_SURFACE_GET(drv, surface_id);
	image_object = V4L2R_IMAGE_GET(drv, image_id);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!image_object)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	image = &image_object->image;

	if (x != 0 || y != 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Wait for a pending decode into the surface first. */
	status = v4l2r_surface_ready(surface);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = surface_view(surface, &view);
	if (status != VA_STATUS_SUCCESS)
		return status;


	if (image->format.fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Bound the copy by both the surface view and the destination image so
	 * a region larger than the image cannot overrun its buffer. */
	if (width > view.width || height > view.height ||
	    width > image->width || height > image->height)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	buffer = V4L2R_BUFFER_GET(drv, image->buf);
	if (!buffer)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	src_luma = view.map[0];
	if (view.nb_planes > 1)
		src_chroma = view.map[1];
	else
		src_chroma = src_luma + view.pitch * view.height;

	dst = buffer->data;

	row_size = width;
	if (row_size > image->pitches[0])
		row_size = image->pitches[0];
	if (row_size > view.pitch)
		row_size = view.pitch;

	/* Full rows with matching pitches: copy each plane in one go. */
	if (row_size == view.pitch && image->pitches[0] == view.pitch &&
	    image->pitches[1] == view.pitch) {
		memcpy(dst + image->offsets[0], src_luma,
		       (size_t)view.pitch * height);
		memcpy(dst + image->offsets[1], src_chroma,
		       (size_t)view.pitch * ((height + 1) / 2));
		return VA_STATUS_SUCCESS;
	}

	for (unsigned int i = 0; i < height; i++)
		memcpy(dst + image->offsets[0] + i * image->pitches[0],
		       src_luma + i * view.pitch, row_size);

	for (unsigned int i = 0; i < (height + 1) / 2; i++)
		memcpy(dst + image->offsets[1] + i * image->pitches[1],
		       src_chroma + i * view.pitch, row_size);

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_PutImage(VADriverContextP va_ctx, VASurfaceID surface_id,
			VAImageID image_id, int src_x, int src_y,
			unsigned int src_width, unsigned int src_height,
			int dest_x, int dest_y, unsigned int dest_width,
			unsigned int dest_height)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;
	struct v4l2r_image *image_object;
	struct v4l2r_buffer *buffer;
	struct v4l2r_frame_view view;
	VAImage *image;
	VAStatus status;
	uint8_t *dst_luma, *dst_chroma;
	const uint8_t *src;
	unsigned int width = dest_width;
	unsigned int height = dest_height;
	unsigned int row_size;

	surface = V4L2R_SURFACE_GET(drv, surface_id);
	image_object = V4L2R_IMAGE_GET(drv, image_id);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!image_object)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	image = &image_object->image;

	/* Plain full-frame uploads only: no offsets, no scaling. */
	if (src_x != 0 || src_y != 0 || dest_x != 0 || dest_y != 0 ||
	    src_width != dest_width || src_height != dest_height)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* A pending decode into this surface would overwrite the upload. */
	status = v4l2r_surface_ready(surface);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = surface_view(surface, &view);
	if (status != VA_STATUS_SUCCESS)
		return status;


	if (image->format.fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Bound the copy by both the surface view and the source image so an
	 * upload region larger than the image cannot overrun its buffer. */
	if (width > view.width || height > view.height ||
	    width > image->width || height > image->height)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	buffer = V4L2R_BUFFER_GET(drv, image->buf);
	if (!buffer)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	dst_luma = view.map[0];
	if (view.nb_planes > 1)
		dst_chroma = view.map[1];
	else
		dst_chroma = dst_luma + view.pitch * view.height;

	src = buffer->data;

	row_size = width;
	if (row_size > image->pitches[0])
		row_size = image->pitches[0];
	if (row_size > view.pitch)
		row_size = view.pitch;

	/* Full rows with matching pitches: copy each plane in one go. */
	if (row_size == view.pitch && image->pitches[0] == view.pitch &&
	    image->pitches[1] == view.pitch) {
		memcpy(dst_luma, src + image->offsets[0],
		       (size_t)view.pitch * height);
		memcpy(dst_chroma, src + image->offsets[1],
		       (size_t)view.pitch * ((height + 1) / 2));
		return VA_STATUS_SUCCESS;
	}

	for (unsigned int i = 0; i < height; i++)
		memcpy(dst_luma + i * view.pitch,
		       src + image->offsets[0] + i * image->pitches[0],
		       row_size);

	for (unsigned int i = 0; i < (height + 1) / 2; i++)
		memcpy(dst_chroma + i * view.pitch,
		       src + image->offsets[1] + i * image->pitches[1],
		       row_size);

	return VA_STATUS_SUCCESS;
}

/* --- subpictures are not supported --- */

VAStatus v4l2r_QuerySubpictureFormats(VADriverContextP va_ctx,
				      VAImageFormat *format_list,
				      unsigned int *flags,
				      unsigned int *num_formats)
{
	(void)va_ctx; (void)format_list; (void)flags;

	if (num_formats)
		*num_formats = 0;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_CreateSubpicture(VADriverContextP va_ctx, VAImageID image,
				VASubpictureID *subpicture)
{
	(void)va_ctx; (void)image; (void)subpicture;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_DestroySubpicture(VADriverContextP va_ctx,
				 VASubpictureID subpicture)
{
	(void)va_ctx; (void)subpicture;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_SetSubpictureImage(VADriverContextP va_ctx,
				  VASubpictureID subpicture, VAImageID image)
{
	(void)va_ctx; (void)subpicture; (void)image;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_SetSubpictureChromakey(VADriverContextP va_ctx,
				      VASubpictureID subpicture,
				      unsigned int chromakey_min,
				      unsigned int chromakey_max,
				      unsigned int chromakey_mask)
{
	(void)va_ctx; (void)subpicture; (void)chromakey_min;
	(void)chromakey_max; (void)chromakey_mask;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_SetSubpictureGlobalAlpha(VADriverContextP va_ctx,
					VASubpictureID subpicture,
					float global_alpha)
{
	(void)va_ctx; (void)subpicture; (void)global_alpha;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_AssociateSubpicture(VADriverContextP va_ctx,
				   VASubpictureID subpicture,
				   VASurfaceID *target_surfaces,
				   int num_surfaces, short src_x, short src_y,
				   unsigned short src_width,
				   unsigned short src_height, short dest_x,
				   short dest_y, unsigned short dest_width,
				   unsigned short dest_height,
				   unsigned int flags)
{
	(void)va_ctx; (void)subpicture; (void)target_surfaces; (void)num_surfaces;
	(void)src_x; (void)src_y; (void)src_width; (void)src_height;
	(void)dest_x; (void)dest_y; (void)dest_width; (void)dest_height;
	(void)flags;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_DeassociateSubpicture(VADriverContextP va_ctx,
				     VASubpictureID subpicture,
				     VASurfaceID *target_surfaces,
				     int num_surfaces)
{
	(void)va_ctx; (void)subpicture; (void)target_surfaces; (void)num_surfaces;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
