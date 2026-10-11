/*
 * mtk_stateful - a VA-API driver on top of a V4L2 *stateful* decoder
 * (MediaTek mtk-vcodec-dec-v1: bitstream in, MM21 frames out, the VCP
 * firmware parses the stream).
 *
 * VA-API is a stateless interface: the client parses the stream and hands
 * the driver per-picture parameters plus slice data. This driver turns that
 * back into a bitstream: for H.264 it synthesizes SPS/PPS NAL units from the
 * VA picture parameters and prepends them to the slice NAL units; a VP9
 * picture already is a complete frame. Each VA picture becomes one OUTPUT
 * buffer whose timestamp carries a sequence number; the decoded CAPTURE
 * frame comes back with that timestamp (in whatever order the firmware
 * outputs it) and is detiled from MM21 into the target surface's NV12
 * dma-buf on the CPU. The decoder keeps its own reference frames, so the VA
 * reference lists are not needed.
 *
 * VA object management, surface export, images and the MM21 detiler come
 * from libva-v4l2_request 1.3 (sofus13) with the MediaTek MM21 patch of the
 * lenovo-slim3-chromebook-omarchy project.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> (libva-v4l2_request)
 * Stateful engine and bitstream synthesis: moto-g-power-2025-omarchy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MTK_STATEFUL_H
#define MTK_STATEFUL_H

#include <linux/videodev2.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <va/va.h>
#include <va/va_backend.h>

static inline uint64_t v4l2r_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define V4L2R_MAX_PROFILES		16
#define V4L2R_MAX_ENTRYPOINTS		2
#define V4L2R_MAX_CONFIG_ATTRIBUTES	16
#define V4L2R_MAX_IMAGE_FORMATS		2
#define V4L2R_MAX_SUBPIC_FORMATS	1
#define V4L2R_MAX_DISPLAY_ATTRIBUTES	1
#define V4L2R_STR_VENDOR		"mtk-stateful (V4L2 stateful MediaTek decoder)"

#define V4L2R_ID_OFFSET_CONFIG		0x01000000
#define V4L2R_ID_OFFSET_CONTEXT		0x02000000
#define V4L2R_ID_OFFSET_SURFACE		0x04000000
#define V4L2R_ID_OFFSET_BUFFER		0x08000000
#define V4L2R_ID_OFFSET_IMAGE		0x10000000

/* Bitstream (OUTPUT) buffers per context. */
#define SD_OUTPUT_BUFFERS		8
#define SD_MAX_CAPTURE			32
/* Pictures in flight that can still be matched to a surface. */
#define SD_PENDING_SLOTS		64

/* Vendor fourccs / controls (mtk-v4l2-vcodec.h). */
#ifndef V4L2_PIX_FMT_MM21
#define V4L2_PIX_FMT_MM21	v4l2_fourcc('M', 'M', '2', '1')
#endif
#define SD_PIX_FMT_M21S		v4l2_fourcc('M', '2', '1', 'S')
#define SD_PIX_FMT_H265		v4l2_fourcc('H', '2', '6', '5')
#define SD_CID_MTK_BASE		(V4L2_CTRL_CLASS_CODEC | 0x2000)
#define SD_CID_VDEC_NO_REORDER	(SD_CID_MTK_BASE + 61)
#define SD_EVENT_MTK_VDEC_ERROR	(V4L2_EVENT_PRIVATE_START + 0x2000 + 1)

struct v4l2r_driver;
struct v4l2r_context;

void v4l2r_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void v4l2r_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct v4l2r_handles {
	void **slots;
	unsigned int size;
	uint32_t id_offset;
};

int v4l2r_handles_init(struct v4l2r_handles *h, uint32_t id_offset);
void v4l2r_handles_destroy(struct v4l2r_handles *h);
uint32_t v4l2r_handles_alloc(struct v4l2r_handles *h, size_t object_size);
void *v4l2r_handles_lookup(struct v4l2r_handles *h, uint32_t id);
void v4l2r_handles_free(struct v4l2r_handles *h, uint32_t id);
void *v4l2r_handles_next(struct v4l2r_handles *h, unsigned int *iter,
			 uint32_t *id);

enum sd_codec {
	SD_CODEC_NONE = 0,
	SD_CODEC_H264,
	SD_CODEC_VP9,
};

struct v4l2r_config {
	VAProfile profile;
	VAEntrypoint entrypoint;
	enum sd_codec codec;
	unsigned int rt_format;
	VAConfigAttrib attributes[V4L2R_MAX_CONFIG_ATTRIBUTES];
	int nb_attributes;
};

/* NV12 surface memory: one dma-buf from /dev/dma_heap/system. */
struct v4l2r_surface_backing {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t size;
	int dmabuf_fd;
	void *map;
};

struct v4l2r_surface {
	struct v4l2r_context *ctx;	/* context of the last decode */
	VASurfaceID id;
	unsigned int width;
	unsigned int height;
	unsigned int rt_format;
	/* Sequence number of the decode whose frame this surface waits for,
	 * 0 when nothing is pending. Protected by ctx->mutex. */
	uint32_t pending_seq;
	/* Sequence number of the most recent decode into this surface. */
	uint32_t decode_seq;
	bool has_picture;		/* holds a decoded frame */
	struct v4l2r_surface_backing *backing;
};

struct v4l2r_buffer {
	VABufferType type;
	unsigned int element_size;
	unsigned int nb_elements;
	void *data;
	bool derived;			/* data points into a surface mapping */
};

struct v4l2r_image {
	VAImage image;
};

static inline size_t v4l2r_buffer_bytes(const struct v4l2r_buffer *buf)
{
	return (size_t)buf->element_size * buf->nb_elements;
}

struct sd_output {
	int fd;
	uint8_t *map;
	size_t size;
	bool queued;
};

struct sd_capture {
	int fd[2];
	void *map[2];
	size_t size[2];
	bool queued;
};

struct sd_pending {
	uint32_t seq;
	struct v4l2r_surface *surface;
	uint64_t submitted_ns;
	bool error;			/* OUTPUT buffer came back with ERROR */
};

/* Growable byte buffer for assembling one access unit. */
struct sd_bytes {
	uint8_t *data;
	size_t len;
	size_t alloc;
};

int sd_bytes_append(struct sd_bytes *b, const void *data, size_t len);
void sd_bytes_free(struct sd_bytes *b);

struct v4l2r_context {
	struct v4l2r_driver *drv;
	VAProfile profile;
	enum sd_codec codec;
	void *codec_priv;
	unsigned int picture_width;
	unsigned int picture_height;

	int video_fd;
	int heap_fd;			/* bitstream buffers */
	int capture_heap_fd;		/* frame buffers */
	pthread_mutex_t mutex;

	/* OUTPUT (bitstream) queue */
	struct v4l2_format output_format;
	struct sd_output output[SD_OUTPUT_BUFFERS];
	bool no_reorder_set;

	/* CAPTURE (frame) queue, set up on SOURCE_CHANGE */
	bool capture_ready;
	bool capture_eos;		/* LAST buffer seen, DQBUF gives EPIPE */
	struct v4l2_format capture_format;
	unsigned int capture_planes;
	unsigned int nb_captures;
	struct sd_capture capture[SD_MAX_CAPTURE];
	unsigned int source_changes;

	/* Picture being assembled between Begin/EndPicture. */
	bool in_picture;
	struct v4l2r_surface *target;
	struct sd_bytes au;		/* complete access unit to submit */
	bool expect_output;		/* the picture produces a frame */

	uint32_t seq;			/* last submitted sequence number */
	struct sd_pending pending[SD_PENDING_SLOTS];

	/* statistics */
	uint64_t frames_in, frames_out, frames_dropped;
	uint64_t detile_ns;
	unsigned int sync_timeouts;
	unsigned int decode_errors;	/* OUTPUT buffers returned with ERROR */
	unsigned int max_latency;	/* in pictures */
	bool failed;
};

struct v4l2r_driver {
	struct v4l2r_handles configs;
	struct v4l2r_handles contexts;
	struct v4l2r_handles surfaces;
	struct v4l2r_handles buffers;
	struct v4l2r_handles images;
	pthread_mutex_t mutex;

	char video_path[64];
	char card[32];
	bool has_h264, has_vp9, has_hevc;
	uint32_t max_width, max_height;
};

static inline struct v4l2r_driver *v4l2r_driver(VADriverContextP va_ctx)
{
	return va_ctx->pDriverData;
}

#define V4L2R_CONFIG(drv, id) \
	((struct v4l2r_config *)v4l2r_handles_lookup(&(drv)->configs, id))
#define V4L2R_CONTEXT(drv, id) \
	((struct v4l2r_context *)v4l2r_handles_lookup(&(drv)->contexts, id))
#define V4L2R_SURFACE(drv, id) \
	((struct v4l2r_surface *)v4l2r_handles_lookup(&(drv)->surfaces, id))
#define V4L2R_BUFFER(drv, id) \
	((struct v4l2r_buffer *)v4l2r_handles_lookup(&(drv)->buffers, id))
#define V4L2R_IMAGE(drv, id) \
	((struct v4l2r_image *)v4l2r_handles_lookup(&(drv)->images, id))

static inline void *v4l2r_handles_get(struct v4l2r_driver *drv,
				      struct v4l2r_handles *h, uint32_t id)
{
	void *object;

	pthread_mutex_lock(&drv->mutex);
	object = v4l2r_handles_lookup(h, id);
	pthread_mutex_unlock(&drv->mutex);

	return object;
}

#define V4L2R_CONFIG_GET(drv, id) \
	((struct v4l2r_config *)v4l2r_handles_get(drv, &(drv)->configs, id))
#define V4L2R_CONTEXT_GET(drv, id) \
	((struct v4l2r_context *)v4l2r_handles_get(drv, &(drv)->contexts, id))
#define V4L2R_SURFACE_GET(drv, id) \
	((struct v4l2r_surface *)v4l2r_handles_get(drv, &(drv)->surfaces, id))
#define V4L2R_BUFFER_GET(drv, id) \
	((struct v4l2r_buffer *)v4l2r_handles_get(drv, &(drv)->buffers, id))
#define V4L2R_IMAGE_GET(drv, id) \
	((struct v4l2r_image *)v4l2r_handles_get(drv, &(drv)->images, id))

/* --- surfaces (surface.c) --- */

/* Allocate the NV12 backing of a surface if it has none yet. */
VAStatus v4l2r_surface_backing(struct v4l2r_surface *surface);
void v4l2r_surface_free_backing(struct v4l2r_surface *surface);
/* Wait until the surface's pending decode (if any) landed in its backing. */
VAStatus v4l2r_surface_ready(struct v4l2r_surface *surface);

/* --- stateful engine (context.c) --- */

/* Wait for the frame of seq, servicing the decoder meanwhile. */
VAStatus sd_wait_surface(struct v4l2r_context *ctx,
			 struct v4l2r_surface *surface);
/* Non-blocking service pass (events, dequeues, detiling). */
void sd_poll(struct v4l2r_context *ctx);
/* Forget a surface that is being destroyed. */
void sd_forget_surface(struct v4l2r_context *ctx, struct v4l2r_surface *surface);

/* --- MM21 detiler (detile.c) --- */

void sd_detile_mm21(const uint8_t *y, uint32_t y_stride,
		    const uint8_t *uv, uint32_t uv_stride,
		    uint32_t src_width, uint32_t src_height,
		    uint8_t *dst, uint32_t dst_pitch,
		    uint32_t dst_width, uint32_t dst_height);

/* --- codecs (h264.c, vp9.c) --- */

struct sd_codec_ops {
	size_t priv_size;
	uint32_t pixelformat;
	VAStatus (*begin_picture)(struct v4l2r_context *ctx);
	VAStatus (*render_buffer)(struct v4l2r_context *ctx,
				  struct v4l2r_buffer *buf);
	/* Fill ctx->au and ctx->expect_output. */
	VAStatus (*end_picture)(struct v4l2r_context *ctx);
	void (*uninit)(struct v4l2r_context *ctx);
};

extern const struct sd_codec_ops sd_codec_h264;
extern const struct sd_codec_ops sd_codec_vp9;

/* --- VA entrypoints --- */

VAStatus v4l2r_Terminate(VADriverContextP va_ctx);
VAStatus v4l2r_QueryConfigProfiles(VADriverContextP va_ctx, VAProfile *profiles,
				   int *num_profiles);
VAStatus v4l2r_QueryConfigEntrypoints(VADriverContextP va_ctx, VAProfile profile,
				      VAEntrypoint *entrypoints,
				      int *num_entrypoints);
VAStatus v4l2r_QueryConfigAttributes(VADriverContextP va_ctx,
				     VAConfigID config_id, VAProfile *profile,
				     VAEntrypoint *entrypoint,
				     VAConfigAttrib *attrib_list,
				     int *num_attribs);
VAStatus v4l2r_CreateConfig(VADriverContextP va_ctx, VAProfile profile,
			    VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
			    int num_attribs, VAConfigID *config_id);
VAStatus v4l2r_DestroyConfig(VADriverContextP va_ctx, VAConfigID config_id);
VAStatus v4l2r_GetConfigAttributes(VADriverContextP va_ctx, VAProfile profile,
				   VAEntrypoint entrypoint,
				   VAConfigAttrib *attrib_list, int num_attribs);

VAStatus v4l2r_CreateSurfaces(VADriverContextP va_ctx, int width, int height,
			      int format, int num_surfaces,
			      VASurfaceID *surfaces);
VAStatus v4l2r_CreateSurfaces2(VADriverContextP va_ctx, unsigned int format,
			       unsigned int width, unsigned int height,
			       VASurfaceID *surfaces, unsigned int num_surfaces,
			       VASurfaceAttrib *attrib_list,
			       unsigned int num_attribs);
VAStatus v4l2r_DestroySurfaces(VADriverContextP va_ctx, VASurfaceID *surface_list,
			       int num_surfaces);
VAStatus v4l2r_SyncSurface(VADriverContextP va_ctx, VASurfaceID render_target);
VAStatus v4l2r_QuerySurfaceStatus(VADriverContextP va_ctx,
				  VASurfaceID render_target,
				  VASurfaceStatus *status);
VAStatus v4l2r_QuerySurfaceAttributes(VADriverContextP va_ctx, VAConfigID config,
				      VASurfaceAttrib *attrib_list,
				      unsigned int *num_attribs);
VAStatus v4l2r_ExportSurfaceHandle(VADriverContextP va_ctx, VASurfaceID surface_id,
				   uint32_t mem_type, uint32_t flags,
				   void *descriptor);
VAStatus v4l2r_PutSurface(VADriverContextP va_ctx, VASurfaceID surface,
			  void *draw, short srcx, short srcy,
			  unsigned short srcw, unsigned short srch,
			  short destx, short desty,
			  unsigned short destw, unsigned short desth,
			  VARectangle *cliprects, unsigned int number_cliprects,
			  unsigned int flags);
VAStatus v4l2r_LockSurface(VADriverContextP va_ctx, VASurfaceID surface,
			   unsigned int *fourcc, unsigned int *luma_stride,
			   unsigned int *chroma_u_stride,
			   unsigned int *chroma_v_stride,
			   unsigned int *luma_offset,
			   unsigned int *chroma_u_offset,
			   unsigned int *chroma_v_offset,
			   unsigned int *buffer_name, void **buffer);
VAStatus v4l2r_UnlockSurface(VADriverContextP va_ctx, VASurfaceID surface);

VAStatus v4l2r_CreateContext(VADriverContextP va_ctx, VAConfigID config_id,
			     int picture_width, int picture_height, int flag,
			     VASurfaceID *render_targets, int num_render_targets,
			     VAContextID *context_id);
VAStatus v4l2r_DestroyContext(VADriverContextP va_ctx, VAContextID context_id);

VAStatus v4l2r_CreateBuffer(VADriverContextP va_ctx, VAContextID context_id,
			    VABufferType type, unsigned int size,
			    unsigned int num_elements, void *data,
			    VABufferID *buf_id);
VAStatus v4l2r_BufferSetNumElements(VADriverContextP va_ctx, VABufferID buf_id,
				    unsigned int num_elements);
VAStatus v4l2r_MapBuffer(VADriverContextP va_ctx, VABufferID buf_id,
			 void **pbuf);
VAStatus v4l2r_UnmapBuffer(VADriverContextP va_ctx, VABufferID buf_id);
VAStatus v4l2r_DestroyBuffer(VADriverContextP va_ctx, VABufferID buf_id);
VAStatus v4l2r_BufferInfo(VADriverContextP va_ctx, VABufferID buf_id,
			  VABufferType *type, unsigned int *size,
			  unsigned int *num_elements);
VAStatus v4l2r_AcquireBufferHandle(VADriverContextP va_ctx, VABufferID buf_id,
				   VABufferInfo *buf_info);
VAStatus v4l2r_ReleaseBufferHandle(VADriverContextP va_ctx, VABufferID buf_id);

VAStatus v4l2r_BeginPicture(VADriverContextP va_ctx, VAContextID context_id,
			    VASurfaceID render_target);
VAStatus v4l2r_RenderPicture(VADriverContextP va_ctx, VAContextID context_id,
			     VABufferID *buffers, int num_buffers);
VAStatus v4l2r_EndPicture(VADriverContextP va_ctx, VAContextID context_id);

VAStatus v4l2r_QueryImageFormats(VADriverContextP va_ctx, VAImageFormat *formats,
				 int *num_formats);
VAStatus v4l2r_CreateImage(VADriverContextP va_ctx, VAImageFormat *format,
			   int width, int height, VAImage *image);
VAStatus v4l2r_DeriveImage(VADriverContextP va_ctx, VASurfaceID surface_id,
			   VAImage *image);
VAStatus v4l2r_DestroyImage(VADriverContextP va_ctx, VAImageID image_id);
VAStatus v4l2r_SetImagePalette(VADriverContextP va_ctx, VAImageID image,
			       unsigned char *palette);
VAStatus v4l2r_GetImage(VADriverContextP va_ctx, VASurfaceID surface_id,
			int x, int y, unsigned int width, unsigned int height,
			VAImageID image_id);
VAStatus v4l2r_PutImage(VADriverContextP va_ctx, VASurfaceID surface,
			VAImageID image, int src_x, int src_y,
			unsigned int src_width, unsigned int src_height,
			int dest_x, int dest_y, unsigned int dest_width,
			unsigned int dest_height);

VAStatus v4l2r_QuerySubpictureFormats(VADriverContextP va_ctx,
				      VAImageFormat *format_list,
				      unsigned int *flags,
				      unsigned int *num_formats);
VAStatus v4l2r_CreateSubpicture(VADriverContextP va_ctx, VAImageID image,
				VASubpictureID *subpicture);
VAStatus v4l2r_DestroySubpicture(VADriverContextP va_ctx,
				 VASubpictureID subpicture);
VAStatus v4l2r_SetSubpictureImage(VADriverContextP va_ctx,
				  VASubpictureID subpicture, VAImageID image);
VAStatus v4l2r_SetSubpictureChromakey(VADriverContextP va_ctx,
				      VASubpictureID subpicture,
				      unsigned int chromakey_min,
				      unsigned int chromakey_max,
				      unsigned int chromakey_mask);
VAStatus v4l2r_SetSubpictureGlobalAlpha(VADriverContextP va_ctx,
					VASubpictureID subpicture,
					float global_alpha);
VAStatus v4l2r_AssociateSubpicture(VADriverContextP va_ctx,
				   VASubpictureID subpicture,
				   VASurfaceID *target_surfaces,
				   int num_surfaces, short src_x, short src_y,
				   unsigned short src_width,
				   unsigned short src_height, short dest_x,
				   short dest_y, unsigned short dest_width,
				   unsigned short dest_height,
				   unsigned int flags);
VAStatus v4l2r_DeassociateSubpicture(VADriverContextP va_ctx,
				     VASubpictureID subpicture,
				     VASurfaceID *target_surfaces,
				     int num_surfaces);

VAStatus v4l2r_QueryDisplayAttributes(VADriverContextP va_ctx,
				      VADisplayAttribute *attr_list,
				      int *num_attributes);
VAStatus v4l2r_GetDisplayAttributes(VADriverContextP va_ctx,
				    VADisplayAttribute *attr_list,
				    int num_attributes);
VAStatus v4l2r_SetDisplayAttributes(VADriverContextP va_ctx,
				    VADisplayAttribute *attr_list,
				    int num_attributes);

#endif /* MTK_STATEFUL_H */
