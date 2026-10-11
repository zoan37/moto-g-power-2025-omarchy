/*
 * Surfaces: NV12 dma-buf backing from /dev/dma_heap/system, synchronization
 * and DRM PRIME export.
 *
 * The decoder's CAPTURE buffers are private to a context (the firmware keeps
 * references in them); every surface has its own linear NV12 dma-buf that a
 * decoded frame is detiled into. The backing is allocated once, on the first
 * export or decode, and never replaced: clients (Chrome) export surfaces
 * before the first decode and keep importing that buffer.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> (libva-v4l2_request)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-heap.h>

#include <drm_fourcc.h>
#include <va/va_drmcommon.h>

#include "mtk_stateful.h"

VAStatus v4l2r_CreateSurfaces2(VADriverContextP va_ctx, unsigned int format,
			       unsigned int width, unsigned int height,
			       VASurfaceID *surfaces, unsigned int num_surfaces,
			       VASurfaceAttrib *attrib_list,
			       unsigned int num_attribs)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	unsigned int i;

	if (format != VA_RT_FORMAT_YUV420)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	for (i = 0; i < num_attribs; i++) {
		if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
		    (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE) &&
		    attrib_list[i].value.value.i != VA_FOURCC_NV12)
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
		if (attrib_list[i].type == VASurfaceAttribMemoryType &&
		    (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE) &&
		    attrib_list[i].value.value.i != VA_SURFACE_ATTRIB_MEM_TYPE_VA)
			return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
	}

	for (i = 0; i < num_surfaces; i++) {
		struct v4l2r_surface *surface;
		VASurfaceID id;

		pthread_mutex_lock(&drv->mutex);
		id = v4l2r_handles_alloc(&drv->surfaces, sizeof(*surface));
		surface = V4L2R_SURFACE(drv, id);
		pthread_mutex_unlock(&drv->mutex);
		if (!surface)
			goto fail;

		surface->id = id;
		surface->width = width;
		surface->height = height;
		surface->rt_format = format;
		surfaces[i] = id;
	}

	return VA_STATUS_SUCCESS;

fail:
	pthread_mutex_lock(&drv->mutex);
	while (i--)
		v4l2r_handles_free(&drv->surfaces, surfaces[i]);
	pthread_mutex_unlock(&drv->mutex);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus v4l2r_CreateSurfaces(VADriverContextP va_ctx, int width, int height,
			      int format, int num_surfaces,
			      VASurfaceID *surfaces)
{
	return v4l2r_CreateSurfaces2(va_ctx, format, width, height, surfaces,
				     num_surfaces, NULL, 0);
}

VAStatus v4l2r_DestroySurfaces(VADriverContextP va_ctx, VASurfaceID *surface_list,
			       int num_surfaces)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);

	for (int i = 0; i < num_surfaces; i++) {
		struct v4l2r_surface *surface;

		surface = V4L2R_SURFACE_GET(drv, surface_list[i]);
		if (!surface)
			return VA_STATUS_ERROR_INVALID_SURFACE;

		if (surface->ctx)
			sd_forget_surface(surface->ctx, surface);
		v4l2r_surface_free_backing(surface);

		pthread_mutex_lock(&drv->mutex);
		v4l2r_handles_free(&drv->surfaces, surface_list[i]);
		pthread_mutex_unlock(&drv->mutex);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_surface_ready(struct v4l2r_surface *surface)
{
	if (!surface->ctx)
		return VA_STATUS_SUCCESS;

	return sd_wait_surface(surface->ctx, surface);
}

VAStatus v4l2r_SyncSurface(VADriverContextP va_ctx, VASurfaceID render_target)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;

	surface = V4L2R_SURFACE_GET(drv, render_target);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	return v4l2r_surface_ready(surface);
}

VAStatus v4l2r_QuerySurfaceStatus(VADriverContextP va_ctx,
				  VASurfaceID render_target,
				  VASurfaceStatus *status)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_surface *surface;

	surface = V4L2R_SURFACE_GET(drv, render_target);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	*status = VASurfaceReady;
	if (surface->ctx) {
		sd_poll(surface->ctx);
		pthread_mutex_lock(&surface->ctx->mutex);
		if (surface->pending_seq)
			*status = VASurfaceRendering;
		pthread_mutex_unlock(&surface->ctx->mutex);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_QuerySurfaceAttributes(VADriverContextP va_ctx, VAConfigID config,
				      VASurfaceAttrib *attrib_list,
				      unsigned int *num_attribs)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	unsigned int i = 0;

	if (!V4L2R_CONFIG_GET(drv, config))
		return VA_STATUS_ERROR_INVALID_CONFIG;

	if (!attrib_list) {
		*num_attribs = 6;
		return VA_STATUS_SUCCESS;
	}

	if (*num_attribs < 6)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	attrib_list[i].type = VASurfaceAttribPixelFormat;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = VA_FOURCC_NV12;
	i++;

	attrib_list[i].type = VASurfaceAttribMemoryType;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
				       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
	i++;

	attrib_list[i].type = VASurfaceAttribMinWidth;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = 16;
	i++;

	attrib_list[i].type = VASurfaceAttribMinHeight;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = 16;
	i++;

	/* The firmware's limit is 2560x1440 (an area); allow portrait too. */
	attrib_list[i].type = VASurfaceAttribMaxWidth;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = drv->max_width > drv->max_height ?
				       drv->max_width : drv->max_height;
	i++;

	attrib_list[i].type = VASurfaceAttribMaxHeight;
	attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attrib_list[i].value.type = VAGenericValueTypeInteger;
	attrib_list[i].value.value.i = drv->max_width > drv->max_height ?
				       drv->max_width : drv->max_height;
	i++;

	*num_attribs = i;
	return VA_STATUS_SUCCESS;
}

/* --- NV12 backing --- */

void v4l2r_surface_free_backing(struct v4l2r_surface *surface)
{
	struct v4l2r_surface_backing *backing = surface->backing;

	if (!backing)
		return;

	if (backing->map)
		munmap(backing->map, backing->size);
	if (backing->dmabuf_fd >= 0)
		close(backing->dmabuf_fd);

	free(backing);
	surface->backing = NULL;
}

VAStatus v4l2r_surface_backing(struct v4l2r_surface *surface)
{
	struct v4l2r_surface_backing *backing;
	struct dma_heap_allocation_data alloc = {0};
	uint32_t width = surface->width, height = (surface->height + 1) & ~1u;
	uint32_t pitch = (width + 63) & ~63u;
	void *map;
	int heap;

	if (surface->backing)
		return VA_STATUS_SUCCESS;

	heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
	if (heap < 0) {
		v4l2r_log("cannot open /dev/dma_heap/system: %s\n",
			  strerror(errno));
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	alloc.len = (uint64_t)pitch * height * 3 / 2;
	alloc.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		v4l2r_log("dma-heap allocation of %llu bytes failed: %s\n",
			  (unsigned long long)alloc.len, strerror(errno));
		close(heap);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	close(heap);

	map = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED,
		   alloc.fd, 0);
	if (map == MAP_FAILED) {
		v4l2r_log("cannot map NV12 backing: %s\n", strerror(errno));
		close(alloc.fd);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	backing = calloc(1, sizeof(*backing));
	if (!backing) {
		munmap(map, alloc.len);
		close(alloc.fd);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	backing->width = width;
	backing->height = height;
	backing->pitch = pitch;
	backing->size = alloc.len;
	backing->dmabuf_fd = alloc.fd;
	backing->map = map;

	surface->backing = backing;
	return VA_STATUS_SUCCESS;
}

/* --- export --- */

VAStatus v4l2r_ExportSurfaceHandle(VADriverContextP va_ctx, VASurfaceID surface_id,
				   uint32_t mem_type, uint32_t flags,
				   void *descriptor)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	VADRMPRIMESurfaceDescriptor *desc = descriptor;
	struct v4l2r_surface_backing *backing;
	struct v4l2r_surface *surface;
	VAStatus status;
	uint32_t offset1;
	int fd;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

	surface = V4L2R_SURFACE_GET(drv, surface_id);
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/* The exported dma-buf carries no fence: finish a pending decode
	 * first for consumers that do not vaSyncSurface() themselves. */
	v4l2r_surface_ready(surface);

	status = v4l2r_surface_backing(surface);
	if (status != VA_STATUS_SUCCESS)
		return status;
	backing = surface->backing;

	fd = dup(backing->dmabuf_fd);
	if (fd < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	offset1 = backing->pitch * backing->height;

	memset(desc, 0, sizeof(*desc));
	desc->fourcc = VA_FOURCC_NV12;
	desc->width = surface->width;
	desc->height = surface->height;
	desc->num_objects = 1;
	desc->objects[0].fd = fd;
	desc->objects[0].size = backing->size;
	desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

	if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
		desc->num_layers = 2;
		desc->layers[0].drm_format = DRM_FORMAT_R8;
		desc->layers[0].num_planes = 1;
		desc->layers[0].object_index[0] = 0;
		desc->layers[0].offset[0] = 0;
		desc->layers[0].pitch[0] = backing->pitch;
		desc->layers[1].drm_format = DRM_FORMAT_GR88;
		desc->layers[1].num_planes = 1;
		desc->layers[1].object_index[0] = 0;
		desc->layers[1].offset[0] = offset1;
		desc->layers[1].pitch[0] = backing->pitch;
	} else {
		desc->num_layers = 1;
		desc->layers[0].drm_format = DRM_FORMAT_NV12;
		desc->layers[0].num_planes = 2;
		desc->layers[0].object_index[0] = 0;
		desc->layers[0].offset[0] = 0;
		desc->layers[0].pitch[0] = backing->pitch;
		desc->layers[0].object_index[1] = 0;
		desc->layers[0].offset[1] = offset1;
		desc->layers[0].pitch[1] = backing->pitch;
	}

	v4l2r_trace("export surface 0x%08x: %ux%u pitch %u fd %d\n",
		    surface->id, surface->width, surface->height,
		    backing->pitch, fd);

	return VA_STATUS_SUCCESS;
}

/* --- unsupported display path --- */

VAStatus v4l2r_PutSurface(VADriverContextP va_ctx, VASurfaceID surface,
			  void *draw, short srcx, short srcy,
			  unsigned short srcw, unsigned short srch,
			  short destx, short desty,
			  unsigned short destw, unsigned short desth,
			  VARectangle *cliprects, unsigned int number_cliprects,
			  unsigned int flags)
{
	(void)va_ctx; (void)surface; (void)draw; (void)srcx; (void)srcy;
	(void)srcw; (void)srch; (void)destx; (void)desty; (void)destw;
	(void)desth; (void)cliprects; (void)number_cliprects; (void)flags;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_LockSurface(VADriverContextP va_ctx, VASurfaceID surface,
			   unsigned int *fourcc, unsigned int *luma_stride,
			   unsigned int *chroma_u_stride,
			   unsigned int *chroma_v_stride,
			   unsigned int *luma_offset,
			   unsigned int *chroma_u_offset,
			   unsigned int *chroma_v_offset,
			   unsigned int *buffer_name, void **buffer)
{
	(void)va_ctx; (void)surface; (void)fourcc; (void)luma_stride;
	(void)chroma_u_stride; (void)chroma_v_stride; (void)luma_offset;
	(void)chroma_u_offset; (void)chroma_v_offset; (void)buffer_name;
	(void)buffer;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_UnlockSurface(VADriverContextP va_ctx, VASurfaceID surface)
{
	(void)va_ctx; (void)surface;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
