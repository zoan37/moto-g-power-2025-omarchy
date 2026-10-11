/*
 * Driver entry point, decoder probing and VAConfig handling.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> (libva-v4l2_request)
 * Stateful decoder probing: moto-g-power-2025-omarchy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "mtk_stateful.h"

void v4l2r_log(const char *fmt, ...)
{
	va_list args;

	fprintf(stderr, "mtk_stateful: ");
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

/* Per-picture tracing, off unless MTK_STATEFUL_TRACE is set. */
void v4l2r_trace(const char *fmt, ...)
{
	static int enabled = -1;
	struct timespec ts;
	char buf[512];
	va_list args;
	int n;

	if (enabled < 0)
		enabled = getenv("MTK_STATEFUL_TRACE") != NULL;
	if (!enabled)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	n = snprintf(buf, sizeof(buf), "[%lld.%06ld] mtk_stateful: ",
		     (long long)ts.tv_sec, ts.tv_nsec / 1000);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return;

	va_start(args, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
	va_end(args);

	fputs(buf, stderr);
}

/* --- profiles --- */

static enum sd_codec profile_codec(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		return SD_CODEC_H264;
	case VAProfileVP9Profile0:
		return SD_CODEC_VP9;
	default:
		return SD_CODEC_NONE;
	}
}

static bool driver_supports_profile(struct v4l2r_driver *drv, VAProfile profile)
{
	switch (profile_codec(profile)) {
	case SD_CODEC_H264:
		return drv->has_h264 && !getenv("MTK_STATEFUL_NO_H264");
	case SD_CODEC_VP9:
		return drv->has_vp9 && !getenv("MTK_STATEFUL_NO_VP9");
	default:
		return false;
	}
}

/* --- decoder probe --- */

/*
 * A usable decoder is a stateful mem2mem device (no request API) that takes
 * H.264 or VP9 bitstream and outputs MediaTek block NV12 (M21S/MM21).
 * Opening it starts a firmware instance, so this is done once, at init.
 */
static bool probe_decoder(struct v4l2r_driver *drv, const char *path)
{
	struct v4l2_capability cap = {0};
	struct v4l2_fmtdesc fmt = {0};
	struct v4l2_frmsizeenum fsize = {0};
	bool mm21 = false;
	int fd;

	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;

	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
		goto fail;
	if (!((cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps :
	       cap.capabilities) & V4L2_CAP_VIDEO_M2M_MPLANE))
		goto fail;

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++)
		if (fmt.pixelformat == SD_PIX_FMT_M21S ||
		    fmt.pixelformat == V4L2_PIX_FMT_MM21)
			mm21 = true;
	if (!mm21)
		goto fail;

	drv->has_h264 = drv->has_vp9 = drv->has_hevc = false;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++) {
		if (fmt.pixelformat == V4L2_PIX_FMT_H264)
			drv->has_h264 = true;
		else if (fmt.pixelformat == V4L2_PIX_FMT_VP9)
			drv->has_vp9 = true;
		else if (fmt.pixelformat == SD_PIX_FMT_H265 ||
			 fmt.pixelformat == V4L2_PIX_FMT_HEVC)
			drv->has_hevc = true;
	}
	if (!drv->has_h264 && !drv->has_vp9)
		goto fail;

	/* Largest frame size the firmware accepts for H.264 (the table is
	 * per codec; the vendor driver reports one stepwise range each). */
	drv->max_width = 1920;
	drv->max_height = 1088;
	fsize.pixel_format = drv->has_h264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_VP9;
	for (fsize.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0;
	     fsize.index++) {
		uint32_t w = fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ?
			     fsize.discrete.width : fsize.stepwise.max_width;
		uint32_t h = fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ?
			     fsize.discrete.height : fsize.stepwise.max_height;

		if (w > drv->max_width)
			drv->max_width = w;
		if (h > drv->max_height)
			drv->max_height = h;
	}

	snprintf(drv->card, sizeof(drv->card), "%s", cap.card);
	snprintf(drv->video_path, sizeof(drv->video_path), "%s", path);
	close(fd);
	return true;

fail:
	close(fd);
	return false;
}

static bool find_decoder(struct v4l2r_driver *drv)
{
	const char *env = getenv("MTK_STATEFUL_VIDEO");
	char path[32];

	if (env)
		return probe_decoder(drv, env);

	for (int i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		if (access(path, F_OK) < 0)
			continue;
		if (probe_decoder(drv, path))
			return true;
	}

	return false;
}

/* --- config --- */

VAStatus v4l2r_QueryConfigProfiles(VADriverContextP va_ctx, VAProfile *profiles,
				   int *num_profiles)
{
	static const VAProfile all[] = {
		VAProfileH264ConstrainedBaseline,
		VAProfileH264Main,
		VAProfileH264High,
		VAProfileVP9Profile0,
	};
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	int count = 0;

	for (unsigned int i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (driver_supports_profile(drv, all[i]))
			profiles[count++] = all[i];

	*num_profiles = count;
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_QueryConfigEntrypoints(VADriverContextP va_ctx, VAProfile profile,
				      VAEntrypoint *entrypoints,
				      int *num_entrypoints)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);

	*num_entrypoints = 0;
	if (driver_supports_profile(drv, profile))
		entrypoints[(*num_entrypoints)++] = VAEntrypointVLD;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_CreateConfig(VADriverContextP va_ctx, VAProfile profile,
			    VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
			    int num_attribs, VAConfigID *config_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_config *config;
	VAConfigID id;

	if (!driver_supports_profile(drv, profile))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

	for (int i = 0; i < num_attribs; i++)
		if (attrib_list[i].type == VAConfigAttribRTFormat &&
		    !(attrib_list[i].value & VA_RT_FORMAT_YUV420))
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	pthread_mutex_lock(&drv->mutex);
	id = v4l2r_handles_alloc(&drv->configs, sizeof(*config));
	config = V4L2R_CONFIG(drv, id);
	pthread_mutex_unlock(&drv->mutex);
	if (!config)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	config->profile = profile;
	config->entrypoint = entrypoint;
	config->codec = profile_codec(profile);
	config->rt_format = VA_RT_FORMAT_YUV420;
	config->attributes[0].type = VAConfigAttribRTFormat;
	config->attributes[0].value = config->rt_format;
	config->nb_attributes = 1;

	for (int i = 0; i < num_attribs; i++) {
		if (attrib_list[i].type == VAConfigAttribRTFormat)
			continue;
		if (config->nb_attributes < V4L2R_MAX_CONFIG_ATTRIBUTES)
			config->attributes[config->nb_attributes++] =
				attrib_list[i];
	}

	*config_id = id;
	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_DestroyConfig(VADriverContextP va_ctx, VAConfigID config_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	VAStatus status = VA_STATUS_SUCCESS;

	pthread_mutex_lock(&drv->mutex);
	if (!V4L2R_CONFIG(drv, config_id))
		status = VA_STATUS_ERROR_INVALID_CONFIG;
	else
		v4l2r_handles_free(&drv->configs, config_id);
	pthread_mutex_unlock(&drv->mutex);

	return status;
}

VAStatus v4l2r_QueryConfigAttributes(VADriverContextP va_ctx,
				     VAConfigID config_id, VAProfile *profile,
				     VAEntrypoint *entrypoint,
				     VAConfigAttrib *attrib_list,
				     int *num_attribs)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_config *config;

	pthread_mutex_lock(&drv->mutex);
	config = V4L2R_CONFIG(drv, config_id);
	if (!config) {
		pthread_mutex_unlock(&drv->mutex);
		return VA_STATUS_ERROR_INVALID_CONFIG;
	}

	if (profile)
		*profile = config->profile;
	if (entrypoint)
		*entrypoint = config->entrypoint;
	if (num_attribs)
		*num_attribs = config->nb_attributes;
	if (attrib_list)
		for (int i = 0; i < config->nb_attributes; i++)
			attrib_list[i] = config->attributes[i];
	pthread_mutex_unlock(&drv->mutex);

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_GetConfigAttributes(VADriverContextP va_ctx, VAProfile profile,
				   VAEntrypoint entrypoint,
				   VAConfigAttrib *attrib_list, int num_attribs)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);

	if (!driver_supports_profile(drv, profile))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

	for (int i = 0; i < num_attribs; i++) {
		switch (attrib_list[i].type) {
		case VAConfigAttribRTFormat:
			attrib_list[i].value = VA_RT_FORMAT_YUV420;
			break;
		case VAConfigAttribMaxPictureWidth:
			attrib_list[i].value = drv->max_width;
			break;
		case VAConfigAttribMaxPictureHeight:
			attrib_list[i].value = drv->max_height;
			break;
		default:
			attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
			break;
		}
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_QueryDisplayAttributes(VADriverContextP va_ctx,
				      VADisplayAttribute *attr_list,
				      int *num_attributes)
{
	(void)va_ctx;
	(void)attr_list;

	if (num_attributes)
		*num_attributes = 0;

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_GetDisplayAttributes(VADriverContextP va_ctx,
				    VADisplayAttribute *attr_list,
				    int num_attributes)
{
	(void)va_ctx; (void)attr_list; (void)num_attributes;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus v4l2r_SetDisplayAttributes(VADriverContextP va_ctx,
				    VADisplayAttribute *attr_list,
				    int num_attributes)
{
	(void)va_ctx; (void)attr_list; (void)num_attributes;
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* --- driver init/terminate --- */

VAStatus v4l2r_Terminate(VADriverContextP va_ctx)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	unsigned int iter;
	uint32_t id;

	iter = 0;
	while (v4l2r_handles_next(&drv->contexts, &iter, &id))
		v4l2r_DestroyContext(va_ctx, id);

	iter = 0;
	while (v4l2r_handles_next(&drv->images, &iter, &id))
		v4l2r_DestroyImage(va_ctx, id);

	iter = 0;
	while (v4l2r_handles_next(&drv->buffers, &iter, &id))
		v4l2r_DestroyBuffer(va_ctx, id);

	iter = 0;
	while (v4l2r_handles_next(&drv->surfaces, &iter, &id))
		v4l2r_DestroySurfaces(va_ctx, &id, 1);

	v4l2r_handles_destroy(&drv->configs);
	v4l2r_handles_destroy(&drv->contexts);
	v4l2r_handles_destroy(&drv->surfaces);
	v4l2r_handles_destroy(&drv->buffers);
	v4l2r_handles_destroy(&drv->images);

	pthread_mutex_destroy(&drv->mutex);

	free(drv);
	va_ctx->pDriverData = NULL;

	return VA_STATUS_SUCCESS;
}

#define V4L2R_INIT_NAME2(major, minor) __vaDriverInit_##major##_##minor
#define V4L2R_INIT_NAME1(major, minor) V4L2R_INIT_NAME2(major, minor)
#define V4L2R_DRIVER_INIT V4L2R_INIT_NAME1(VA_MAJOR_VERSION, VA_MINOR_VERSION)

VAStatus __attribute__((visibility("default")))
V4L2R_DRIVER_INIT(VADriverContextP va_ctx);

VAStatus V4L2R_DRIVER_INIT(VADriverContextP va_ctx)
{
	struct VADriverVTable *vtable = va_ctx->vtable;
	struct v4l2r_driver *drv;

	va_ctx->version_major = VA_MAJOR_VERSION;
	va_ctx->version_minor = VA_MINOR_VERSION;
	va_ctx->max_profiles = V4L2R_MAX_PROFILES;
	va_ctx->max_entrypoints = V4L2R_MAX_ENTRYPOINTS;
	va_ctx->max_attributes = V4L2R_MAX_CONFIG_ATTRIBUTES;
	va_ctx->max_image_formats = V4L2R_MAX_IMAGE_FORMATS;
	va_ctx->max_subpic_formats = V4L2R_MAX_SUBPIC_FORMATS;
	va_ctx->max_display_attributes = V4L2R_MAX_DISPLAY_ATTRIBUTES;
	va_ctx->str_vendor = V4L2R_STR_VENDOR;

	vtable->vaTerminate = v4l2r_Terminate;
	vtable->vaQueryConfigProfiles = v4l2r_QueryConfigProfiles;
	vtable->vaQueryConfigEntrypoints = v4l2r_QueryConfigEntrypoints;
	vtable->vaQueryConfigAttributes = v4l2r_QueryConfigAttributes;
	vtable->vaCreateConfig = v4l2r_CreateConfig;
	vtable->vaDestroyConfig = v4l2r_DestroyConfig;
	vtable->vaGetConfigAttributes = v4l2r_GetConfigAttributes;
	vtable->vaCreateSurfaces = v4l2r_CreateSurfaces;
	vtable->vaCreateSurfaces2 = v4l2r_CreateSurfaces2;
	vtable->vaDestroySurfaces = v4l2r_DestroySurfaces;
	vtable->vaExportSurfaceHandle = v4l2r_ExportSurfaceHandle;
	vtable->vaCreateContext = v4l2r_CreateContext;
	vtable->vaDestroyContext = v4l2r_DestroyContext;
	vtable->vaCreateBuffer = v4l2r_CreateBuffer;
	vtable->vaBufferSetNumElements = v4l2r_BufferSetNumElements;
	vtable->vaMapBuffer = v4l2r_MapBuffer;
	vtable->vaUnmapBuffer = v4l2r_UnmapBuffer;
	vtable->vaDestroyBuffer = v4l2r_DestroyBuffer;
	vtable->vaBufferInfo = v4l2r_BufferInfo;
	vtable->vaAcquireBufferHandle = v4l2r_AcquireBufferHandle;
	vtable->vaReleaseBufferHandle = v4l2r_ReleaseBufferHandle;
	vtable->vaBeginPicture = v4l2r_BeginPicture;
	vtable->vaRenderPicture = v4l2r_RenderPicture;
	vtable->vaEndPicture = v4l2r_EndPicture;
	vtable->vaSyncSurface = v4l2r_SyncSurface;
	vtable->vaQuerySurfaceAttributes = v4l2r_QuerySurfaceAttributes;
	vtable->vaQuerySurfaceStatus = v4l2r_QuerySurfaceStatus;
	vtable->vaPutSurface = v4l2r_PutSurface;
	vtable->vaLockSurface = v4l2r_LockSurface;
	vtable->vaUnlockSurface = v4l2r_UnlockSurface;
	vtable->vaQueryImageFormats = v4l2r_QueryImageFormats;
	vtable->vaCreateImage = v4l2r_CreateImage;
	vtable->vaDeriveImage = v4l2r_DeriveImage;
	vtable->vaDestroyImage = v4l2r_DestroyImage;
	vtable->vaSetImagePalette = v4l2r_SetImagePalette;
	vtable->vaGetImage = v4l2r_GetImage;
	vtable->vaPutImage = v4l2r_PutImage;
	vtable->vaQuerySubpictureFormats = v4l2r_QuerySubpictureFormats;
	vtable->vaCreateSubpicture = v4l2r_CreateSubpicture;
	vtable->vaDestroySubpicture = v4l2r_DestroySubpicture;
	vtable->vaSetSubpictureImage = v4l2r_SetSubpictureImage;
	vtable->vaSetSubpictureChromakey = v4l2r_SetSubpictureChromakey;
	vtable->vaSetSubpictureGlobalAlpha = v4l2r_SetSubpictureGlobalAlpha;
	vtable->vaAssociateSubpicture = v4l2r_AssociateSubpicture;
	vtable->vaDeassociateSubpicture = v4l2r_DeassociateSubpicture;
	vtable->vaQueryDisplayAttributes = v4l2r_QueryDisplayAttributes;
	vtable->vaGetDisplayAttributes = v4l2r_GetDisplayAttributes;
	vtable->vaSetDisplayAttributes = v4l2r_SetDisplayAttributes;

	drv = calloc(1, sizeof(*drv));
	if (!drv)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	if (v4l2r_handles_init(&drv->configs, V4L2R_ID_OFFSET_CONFIG) < 0 ||
	    v4l2r_handles_init(&drv->contexts, V4L2R_ID_OFFSET_CONTEXT) < 0 ||
	    v4l2r_handles_init(&drv->surfaces, V4L2R_ID_OFFSET_SURFACE) < 0 ||
	    v4l2r_handles_init(&drv->buffers, V4L2R_ID_OFFSET_BUFFER) < 0 ||
	    v4l2r_handles_init(&drv->images, V4L2R_ID_OFFSET_IMAGE) < 0) {
		v4l2r_handles_destroy(&drv->configs);
		v4l2r_handles_destroy(&drv->contexts);
		v4l2r_handles_destroy(&drv->surfaces);
		v4l2r_handles_destroy(&drv->buffers);
		v4l2r_handles_destroy(&drv->images);
		free(drv);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	pthread_mutex_init(&drv->mutex, NULL);
	va_ctx->pDriverData = drv;

	if (!find_decoder(drv)) {
		v4l2r_log("no stateful MediaTek decoder (M21S/MM21 output) found\n");
		v4l2r_Terminate(va_ctx);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	v4l2r_trace("decoder %s [%s]: h264 %d vp9 %d hevc %d, max %ux%u\n",
		    drv->video_path, drv->card, drv->has_h264, drv->has_vp9,
		    drv->has_hevc, drv->max_width, drv->max_height);

	return VA_STATUS_SUCCESS;
}
