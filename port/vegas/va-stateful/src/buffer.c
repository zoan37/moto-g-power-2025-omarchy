/*
 * VA buffer objects.
 *
 * Parameter and slice data buffers are plain heap allocations (from
 * libva-v4l2_request 1.3); the codec copies slice data into the access unit
 * it assembles at RenderPicture time.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "mtk_stateful.h"

VAStatus v4l2r_CreateBuffer(VADriverContextP va_ctx, VAContextID context_id,
			    VABufferType type, unsigned int size,
			    unsigned int num_elements, void *data,
			    VABufferID *buf_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_buffer *buffer;
	VABufferID id;

	switch (type) {
	case VAPictureParameterBufferType:
	case VAIQMatrixBufferType:
	case VASliceParameterBufferType:
	case VASliceDataBufferType:
	case VAProbabilityBufferType:
	case VAImageBufferType:
		break;
	default:
		return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
	}

	(void)context_id;

	pthread_mutex_lock(&drv->mutex);
	id = v4l2r_handles_alloc(&drv->buffers, sizeof(*buffer));
	buffer = V4L2R_BUFFER(drv, id);
	pthread_mutex_unlock(&drv->mutex);
	if (!buffer)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	buffer->type = type;
	buffer->element_size = size;
	buffer->nb_elements = num_elements;

	buffer->data = malloc((size_t)size * num_elements);
	if (!buffer->data) {
		pthread_mutex_lock(&drv->mutex);
		v4l2r_handles_free(&drv->buffers, id);
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	if (data)
		memcpy(buffer->data, data, (size_t)size * num_elements);

	*buf_id = id;
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_BufferSetNumElements(VADriverContextP va_ctx, VABufferID buf_id,
				    unsigned int num_elements)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_buffer *buffer;

	buffer = V4L2R_BUFFER_GET(drv, buf_id);
	if (!buffer)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (buffer->derived)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (num_elements != buffer->nb_elements) {
		void *data = realloc(buffer->data,
				     (size_t)buffer->element_size * num_elements);
		if (!data)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		buffer->data = data;
		buffer->nb_elements = num_elements;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_MapBuffer(VADriverContextP va_ctx, VABufferID buf_id, void **pbuf)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_buffer *buffer;

	buffer = V4L2R_BUFFER_GET(drv, buf_id);
	if (!buffer)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	*pbuf = buffer->data;
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_UnmapBuffer(VADriverContextP va_ctx, VABufferID buf_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_buffer *buffer;

	buffer = V4L2R_BUFFER_GET(drv, buf_id);
	if (!buffer)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_DestroyBuffer(VADriverContextP va_ctx, VABufferID buf_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_buffer *buffer;

	pthread_mutex_lock(&drv->mutex);
	buffer = V4L2R_BUFFER(drv, buf_id);
	if (!buffer) {
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_INVALID_BUFFER;
	}

	/* Derived image buffers point into the surface mapping. */
	if (!buffer->derived)
		free(buffer->data);

	v4l2r_handles_free(&drv->buffers, buf_id);
	pthread_mutex_unlock(&drv->mutex);

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_BufferInfo(VADriverContextP va_ctx, VABufferID buf_id,
			  VABufferType *type, unsigned int *size,
			  unsigned int *num_elements)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_buffer *buffer;

	buffer = V4L2R_BUFFER_GET(drv, buf_id);
	if (!buffer)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (type)
		*type = buffer->type;
	if (size)
		*size = buffer->element_size;
	if (num_elements)
		*num_elements = buffer->nb_elements;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_AcquireBufferHandle(VADriverContextP va_ctx, VABufferID buf_id,
				   VABufferInfo *buf_info)
{
	(void)va_ctx; (void)buf_id; (void)buf_info;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_ReleaseBufferHandle(VADriverContextP va_ctx, VABufferID buf_id)
{
	(void)va_ctx; (void)buf_id;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
