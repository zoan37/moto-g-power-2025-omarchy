/*
 * The stateful decode engine: one V4L2 mem2mem instance per VA context.
 *
 *  - OUTPUT: SD_OUTPUT_BUFFERS dma-bufs from /dev/dma_heap/mtk_mm (the
 *    vendor driver gives the VCP no address for plain MMAP buffers), one
 *    complete access unit / frame per buffer. The buffer timestamp carries
 *    the picture's sequence number.
 *  - CAPTURE: set up on V4L2_EVENT_SOURCE_CHANGE (G_FMT, MIN_BUFFERS + 4
 *    dma-bufs, also from mtk_mm), torn down and rebuilt on later source
 *    changes. Every dequeued frame is matched to its surface by timestamp,
 *    detiled into the surface's NV12 backing and requeued at once; the
 *    firmware tracks which buffers it still references.
 *
 * There is no worker thread: the queues are serviced from EndPicture
 * (non-blocking), SyncSurface (until the surface's frame arrived, bounded
 * by a timeout) and QuerySurfaceStatus. All of it runs under ctx->mutex.
 *
 * Copyright (C) 2026 moto-g-power-2025-omarchy; entrypoint skeleton from
 * libva-v4l2_request 1.3, Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>

#include "mtk_stateful.h"

#define SD_OUT_TYPE	V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define SD_CAP_TYPE	V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);

	return v && *v ? atoi(v) : def;
}

static int sync_timeout_ms(void)
{
	static int ms = -1;

	if (ms < 0)
		ms = env_int("MTK_STATEFUL_TIMEOUT_MS", 1000);
	return ms;
}

/* --- small helpers --- */

int sd_bytes_append(struct sd_bytes *b, const void *data, size_t len)
{
	if (b->len + len > b->alloc) {
		size_t alloc = b->alloc ? b->alloc : 64 * 1024;
		uint8_t *grown;

		while (alloc < b->len + len)
			alloc *= 2;
		grown = realloc(b->data, alloc);
		if (!grown)
			return -1;
		b->data = grown;
		b->alloc = alloc;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
	return 0;
}

void sd_bytes_free(struct sd_bytes *b)
{
	free(b->data);
	memset(b, 0, sizeof(*b));
}

static void dmabuf_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags };

	if (fd >= 0)
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int heap_open(const char *var)
{
	const char *env = getenv(var);
	const char *names[] = { env ? env : "mtk_mm", "system" };
	char path[64];

	for (unsigned int i = 0; i < 2; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/dma_heap/%s", names[i]);
		/* Allocation is an ioctl; the vendor heap node is 0644 root,
		 * so a read-only open is what an unprivileged process gets. */
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			if (i)
				v4l2r_log("using %s (decode is slower than from mtk_mm)\n",
					  path);
			return fd;
		}
		v4l2r_log("cannot open %s: %s\n", path, strerror(errno));
	}
	return -1;
}

static int heap_alloc(int heap_fd, size_t len)
{
	struct dma_heap_allocation_data a = {
		.len = len,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
		v4l2r_log("dma-heap allocation of %zu bytes failed: %s\n", len,
			  strerror(errno));
		return -1;
	}
	return a.fd;
}

static inline struct timeval seq_timeval(uint32_t seq)
{
	return (struct timeval) { .tv_sec = seq / 1000000,
				  .tv_usec = seq % 1000000 };
}

static inline uint32_t timeval_seq(const struct timeval *tv)
{
	return (uint32_t)(tv->tv_sec * 1000000 + tv->tv_usec);
}

/* --- OUTPUT buffers --- */

static void output_release(struct sd_output *out)
{
	if (out->map)
		munmap(out->map, out->size);
	if (out->fd >= 0)
		close(out->fd);
	out->map = NULL;
	out->fd = -1;
	out->size = 0;
}

static int output_ensure(struct v4l2r_context *ctx, struct sd_output *out,
			 size_t len)
{
	size_t size = ctx->output_format.fmt.pix_mp.plane_fmt[0].sizeimage;

	if (out->fd >= 0 && out->size >= len)
		return 0;

	while (size < len)
		size *= 2;
	output_release(out);

	out->fd = heap_alloc(ctx->heap_fd, size);
	if (out->fd < 0)
		return -1;
	out->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			out->fd, 0);
	if (out->map == MAP_FAILED) {
		v4l2r_log("cannot map bitstream buffer: %s\n", strerror(errno));
		out->map = NULL;
		output_release(out);
		return -1;
	}
	out->size = size;
	return 0;
}

/* --- CAPTURE buffers --- */

static int capture_queue(struct v4l2r_context *ctx, unsigned int index)
{
	struct sd_capture *cap = &ctx->capture[index];
	struct v4l2_plane planes[2] = {0};
	struct v4l2_buffer b = {
		.type = SD_CAP_TYPE,
		.memory = V4L2_MEMORY_DMABUF,
		.index = index,
		.length = ctx->capture_planes,
		.m.planes = planes,
	};

	/* The vendor driver reads this field as an optional "general buffer"
	 * dma-buf fd; -1 means none (0 would be taken as fd 0). */
	b.request_fd = -1;
	for (unsigned int p = 0; p < ctx->capture_planes; p++) {
		planes[p].m.fd = cap->fd[p];
		planes[p].length = cap->size[p];
	}
	if (ioctl(ctx->video_fd, VIDIOC_QBUF, &b) < 0) {
		v4l2r_log("CAPTURE QBUF %u failed: %s\n", index, strerror(errno));
		return -1;
	}
	cap->queued = true;
	return 0;
}

static void capture_teardown(struct v4l2r_context *ctx)
{
	enum v4l2_buf_type type = SD_CAP_TYPE;
	struct v4l2_requestbuffers r = {
		.count = 0,
		.type = SD_CAP_TYPE,
		.memory = V4L2_MEMORY_DMABUF,
	};

	if (!ctx->capture_ready && !ctx->nb_captures)
		return;

	ioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type);
	ioctl(ctx->video_fd, VIDIOC_REQBUFS, &r);

	for (unsigned int i = 0; i < ctx->nb_captures; i++) {
		struct sd_capture *cap = &ctx->capture[i];

		for (unsigned int p = 0; p < 2; p++) {
			if (cap->map[p])
				munmap(cap->map[p], cap->size[p]);
			if (cap->fd[p] >= 0)
				close(cap->fd[p]);
			cap->map[p] = NULL;
			cap->fd[p] = -1;
		}
		cap->queued = false;
	}
	ctx->nb_captures = 0;
	ctx->capture_ready = false;
}

static int capture_setup(struct v4l2r_context *ctx)
{
	struct v4l2_pix_format_mplane *pix = &ctx->capture_format.fmt.pix_mp;
	struct v4l2_control min = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
	struct v4l2_requestbuffers r = {
		.type = SD_CAP_TYPE,
		.memory = V4L2_MEMORY_DMABUF,
	};
	enum v4l2_buf_type type = SD_CAP_TYPE;
	unsigned int count;

	ctx->capture_format = (struct v4l2_format) { .type = SD_CAP_TYPE };
	if (ioctl(ctx->video_fd, VIDIOC_G_FMT, &ctx->capture_format) < 0) {
		v4l2r_log("CAPTURE G_FMT failed: %s\n", strerror(errno));
		return -1;
	}
	if ((pix->pixelformat != SD_PIX_FMT_M21S &&
	     pix->pixelformat != V4L2_PIX_FMT_MM21) ||
	    pix->num_planes < 1 || pix->num_planes > 2) {
		v4l2r_log("unexpected CAPTURE format %.4s with %u planes\n",
			  (const char *)&pix->pixelformat, pix->num_planes);
		return -1;
	}
	ctx->capture_planes = pix->num_planes;

	count = ioctl(ctx->video_fd, VIDIOC_G_CTRL, &min) == 0 ? min.value : 8;
	count += env_int("MTK_STATEFUL_EXTRA_CAPTURE", 4);
	if (count > SD_MAX_CAPTURE)
		count = SD_MAX_CAPTURE;

	r.count = count;
	if (ioctl(ctx->video_fd, VIDIOC_REQBUFS, &r) < 0) {
		v4l2r_log("CAPTURE REQBUFS %u failed: %s\n", count, strerror(errno));
		return -1;
	}
	if (r.count > SD_MAX_CAPTURE)
		r.count = SD_MAX_CAPTURE;
	ctx->nb_captures = r.count;

	for (unsigned int i = 0; i < ctx->nb_captures; i++) {
		struct sd_capture *cap = &ctx->capture[i];

		for (unsigned int p = 0; p < ctx->capture_planes; p++) {
			cap->size[p] = pix->plane_fmt[p].sizeimage;
			cap->fd[p] = heap_alloc(ctx->capture_heap_fd, cap->size[p]);
			if (cap->fd[p] < 0)
				goto fail;
			cap->map[p] = mmap(NULL, cap->size[p], PROT_READ,
					   MAP_SHARED, cap->fd[p], 0);
			if (cap->map[p] == MAP_FAILED) {
				cap->map[p] = NULL;
				v4l2r_log("cannot map CAPTURE buffer: %s\n",
					  strerror(errno));
				goto fail;
			}
		}
		if (capture_queue(ctx, i) < 0)
			goto fail;
	}

	if (ioctl(ctx->video_fd, VIDIOC_STREAMON, &type) < 0) {
		v4l2r_log("CAPTURE STREAMON failed: %s\n", strerror(errno));
		goto fail;
	}

	ctx->capture_ready = true;
	ctx->capture_eos = false;
	v4l2r_log("decoding %.4s %ux%u (bytesperline %u, %u plane%s, %u buffers)\n",
		  (const char *)&pix->pixelformat, pix->width, pix->height,
		  pix->plane_fmt[0].bytesperline, pix->num_planes,
		  pix->num_planes > 1 ? "s" : "", ctx->nb_captures);
	return 0;

fail:
	ctx->capture_ready = true;	/* let teardown release everything */
	capture_teardown(ctx);
	return -1;
}

/* --- frame delivery --- */

static void detile_into(struct v4l2r_context *ctx, struct sd_capture *cap,
			struct v4l2r_surface *surface)
{
	const struct v4l2_pix_format_mplane *pix = &ctx->capture_format.fmt.pix_mp;
	uint32_t y_stride = pix->plane_fmt[0].bytesperline;
	uint32_t uv_stride = ctx->capture_planes > 1 ?
			     pix->plane_fmt[1].bytesperline : y_stride;
	struct v4l2r_surface_backing *dst;
	const uint8_t *y, *uv;
	uint64_t t0 = v4l2r_now_ns();

	if (v4l2r_surface_backing(surface) != VA_STATUS_SUCCESS)
		return;
	dst = surface->backing;

	y = cap->map[0];
	uv = ctx->capture_planes > 1 ? cap->map[1] :
	     y + (size_t)y_stride * pix->height;

	for (unsigned int p = 0; p < ctx->capture_planes; p++)
		dmabuf_sync(cap->fd[p], DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	dmabuf_sync(dst->dmabuf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);

	sd_detile_mm21(y, y_stride, uv, uv_stride, pix->width, pix->height,
		       dst->map, dst->pitch, dst->width, dst->height);

	dmabuf_sync(dst->dmabuf_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	for (unsigned int p = 0; p < ctx->capture_planes; p++)
		dmabuf_sync(cap->fd[p], DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);

	ctx->detile_ns += v4l2r_now_ns() - t0;
}

/* A frame came back: detile it into the surface still waiting for it. */
static void deliver(struct v4l2r_context *ctx, const struct v4l2_buffer *b)
{
	uint32_t seq = timeval_seq(&b->timestamp);
	struct sd_pending *slot = &ctx->pending[seq % SD_PENDING_SLOTS];
	struct v4l2r_surface *surface = NULL;
	uint32_t bytes = b->m.planes[0].bytesused;

	if (slot->seq == seq && slot->surface &&
	    slot->surface->ctx == ctx)
		surface = slot->surface;

	v4l2r_trace("frame seq %u buf %u bytes %u flags 0x%x -> surface 0x%08x "
		    "(latency %u pictures, %.1f ms)\n", seq, b->index, bytes,
		    b->flags, surface ? surface->id : VA_INVALID_ID,
		    ctx->seq - seq, slot->seq == seq ?
		    (v4l2r_now_ns() - slot->submitted_ns) / 1e6 : -1.0);

	if (!bytes) {
		/* No picture for this one (e.g. a decode-only frame): don't let
		 * a sync wait for it. */
		if (surface && surface->decode_seq == seq &&
		    surface->pending_seq == seq)
			surface->pending_seq = 0;
		return;
	}

	ctx->frames_out++;
	if (ctx->seq - seq > ctx->max_latency)
		ctx->max_latency = ctx->seq - seq;
	if (b->flags & V4L2_BUF_FLAG_ERROR)
		v4l2r_trace("frame seq %u has the ERROR flag\n", seq);

	/* Frames come out in decode order: an errored picture older than
	 * this one won't produce a frame any more. */
	for (unsigned int i = 0; i < SD_PENDING_SLOTS; i++) {
		struct sd_pending *old = &ctx->pending[i];

		if (old->error && old->surface && (int32_t)(seq - old->seq) > 0 &&
		    old->surface->pending_seq == old->seq) {
			v4l2r_trace("seq %u failed to decode, not waiting for it\n",
				    old->seq);
			old->surface->pending_seq = 0;
			old->surface = NULL;
			ctx->frames_dropped++;
		}
	}

	if (!surface)
		return;

	slot->surface = NULL;
	if (surface->decode_seq != seq)
		return;	/* surface was reused for a newer decode */

	detile_into(ctx, &ctx->capture[b->index], surface);
	surface->has_picture = true;
	if (surface->pending_seq == seq)
		surface->pending_seq = 0;
	ctx->sync_timeouts = 0;
}

/* --- servicing --- */

static void handle_source_change(struct v4l2r_context *ctx);

/* One pass over events and both queues; poll()s up to timeout_ms first.
 * Called with ctx->mutex held. */
static void service(struct v4l2r_context *ctx, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = ctx->video_fd,
		.events = POLLIN | POLLOUT | POLLPRI,
	};
	struct v4l2_plane planes[2];
	struct v4l2_buffer b;
	bool work = false;
	int ret;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret <= 0)
		return;

	if (pfd.revents & POLLPRI) {
		struct v4l2_event ev;

		while (ioctl(ctx->video_fd, VIDIOC_DQEVENT, &ev) == 0) {
			work = true;
			if (ev.type == V4L2_EVENT_SOURCE_CHANGE)
				handle_source_change(ctx);
			else if (ev.type == V4L2_EVENT_EOS)
				v4l2r_trace("EOS event\n");
			else if (ev.type == SD_EVENT_MTK_VDEC_ERROR) {
				/* Fatal for this instance (e.g. "vcodec not
				 * support the source"): fail fast, the client
				 * falls back or reports the error. */
				v4l2r_log("decoder reported an unrecoverable stream "
					  "error (0x%x)\n", ev.u.data[0]);
				ctx->failed = true;
			}
			else
				v4l2r_log("decoder event 0x%x\n", ev.type);
		}
	}

	for (;;) {
		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = SD_OUT_TYPE;
		b.memory = V4L2_MEMORY_DMABUF;
		b.length = 1;
		b.m.planes = planes;
		if (ioctl(ctx->video_fd, VIDIOC_DQBUF, &b) < 0)
			break;
		if (b.index < SD_OUTPUT_BUFFERS)
			ctx->output[b.index].queued = false;
		work = true;
		/* The firmware reported an error for this picture. A frame
		 * (concealed) usually still follows; if a later picture's
		 * frame shows up first, deliver() stops waiting for it. */
		if (b.flags & V4L2_BUF_FLAG_ERROR) {
			uint32_t seq = timeval_seq(&b.timestamp);
			struct sd_pending *slot = &ctx->pending[seq % SD_PENDING_SLOTS];

			v4l2r_trace("decode error on seq %u\n", seq);
			ctx->decode_errors++;
			if (slot->seq == seq)
				slot->error = true;
		}
	}

	while (ctx->capture_ready) {
		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = SD_CAP_TYPE;
		b.memory = V4L2_MEMORY_DMABUF;
		b.length = ctx->capture_planes;
		b.m.planes = planes;
		if (ioctl(ctx->video_fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EPIPE && !ctx->capture_eos) {
				v4l2r_trace("CAPTURE queue drained (EPIPE)\n");
				ctx->capture_eos = true;
			}
			break;
		}
		work = true;
		if (b.index >= ctx->nb_captures)
			continue;
		ctx->capture[b.index].queued = false;
		deliver(ctx, &b);
		if (b.flags & V4L2_BUF_FLAG_LAST) {
			v4l2r_trace("CAPTURE buffer with LAST flag\n");
			break;
		}
		capture_queue(ctx, b.index);
	}

	/* An m2m fd polls ready at once without queued buffers on both sides
	 * (POLLERR) and after the LAST buffer (POLLIN, DQBUF fails with EPIPE);
	 * don't spin on that. */
	if (!work && timeout_ms > 0)
		usleep(1000);
}

static void handle_source_change(struct v4l2r_context *ctx)
{
	ctx->source_changes++;

	if (ctx->capture_ready) {
		/* Resolution change: frames decoded at the old size still
		 * come out first, up to a buffer flagged LAST. */
		uint64_t deadline = v4l2r_now_ns() + 500000000ull;

		v4l2r_log("resolution change, draining CAPTURE\n");
		while (v4l2r_now_ns() < deadline) {
			struct v4l2_plane planes[2] = {0};
			struct v4l2_buffer b = {
				.type = SD_CAP_TYPE,
				.memory = V4L2_MEMORY_DMABUF,
				.length = ctx->capture_planes,
				.m.planes = planes,
			};
			struct pollfd pfd = { .fd = ctx->video_fd, .events = POLLIN };

			if (ioctl(ctx->video_fd, VIDIOC_DQBUF, &b) < 0) {
				if (errno == EPIPE)
					break;
				poll(&pfd, 1, 20);
				continue;
			}
			deliver(ctx, &b);
			if (b.flags & V4L2_BUF_FLAG_LAST)
				break;
		}
		capture_teardown(ctx);
	}

	if (capture_setup(ctx) < 0) {
		v4l2r_log("CAPTURE setup failed, giving up on this context\n");
		ctx->failed = true;
	}
}

void sd_poll(struct v4l2r_context *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	service(ctx, 0);
	pthread_mutex_unlock(&ctx->mutex);
}

VAStatus sd_wait_surface(struct v4l2r_context *ctx,
			 struct v4l2r_surface *surface)
{
	VAStatus status = VA_STATUS_SUCCESS;
	uint64_t t0, deadline;

	pthread_mutex_lock(&ctx->mutex);
	t0 = v4l2r_now_ns();
	deadline = t0 + (uint64_t)sync_timeout_ms() * 1000000ull;

	while (surface->pending_seq && !ctx->failed) {
		uint64_t now = v4l2r_now_ns();
		int left;

		if (now >= deadline)
			break;
		left = (deadline - now) / 1000000 + 1;
		service(ctx, left < 50 ? left : 50);
	}

	if (surface->pending_seq) {
		v4l2r_log("timeout waiting for seq %u (surface 0x%08x; submitted %u, "
			  "frames in %llu out %llu, capture %s)\n",
			  surface->pending_seq, surface->id, ctx->seq,
			  (unsigned long long)ctx->frames_in,
			  (unsigned long long)ctx->frames_out,
			  ctx->capture_ready ? "ready" : "not set up");
		surface->pending_seq = 0;
		/* A dropped frame shouldn't end playback, a dead decoder must. */
		if (++ctx->sync_timeouts >= 3 || ctx->failed)
			status = VA_STATUS_ERROR_DECODING_ERROR;
	} else if (ctx->failed) {
		status = VA_STATUS_ERROR_DECODING_ERROR;
	}

	v4l2r_trace("sync surface 0x%08x: %.2f ms\n", surface->id,
		    (v4l2r_now_ns() - t0) / 1e6);
	pthread_mutex_unlock(&ctx->mutex);
	return status;
}

void sd_forget_surface(struct v4l2r_context *ctx, struct v4l2r_surface *surface)
{
	pthread_mutex_lock(&ctx->mutex);
	for (unsigned int i = 0; i < SD_PENDING_SLOTS; i++)
		if (ctx->pending[i].surface == surface)
			ctx->pending[i].surface = NULL;
	surface->pending_seq = 0;
	surface->ctx = NULL;
	pthread_mutex_unlock(&ctx->mutex);
}

/* --- submission --- */

static VAStatus submit(struct v4l2r_context *ctx)
{
	struct v4l2r_surface *target = ctx->target;
	struct v4l2_plane plane = {0};
	struct v4l2_buffer b = {
		.type = SD_OUT_TYPE,
		.memory = V4L2_MEMORY_DMABUF,
		.length = 1,
		.m.planes = &plane,
	};
	struct sd_output *out = NULL;
	struct sd_pending *slot;
	uint64_t deadline;
	unsigned int i;
	uint32_t seq;

	if (!ctx->au.len)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	/* A free bitstream buffer; wait for the decoder to return one. */
	service(ctx, 0);
	deadline = v4l2r_now_ns() + (uint64_t)sync_timeout_ms() * 1000000ull;
	for (;;) {
		for (i = 0; i < SD_OUTPUT_BUFFERS; i++)
			if (!ctx->output[i].queued) {
				out = &ctx->output[i];
				break;
			}
		if (out || ctx->failed || v4l2r_now_ns() >= deadline)
			break;
		service(ctx, 50);
	}
	if (!out) {
		v4l2r_log("no bitstream buffer came back from the decoder\n");
		return VA_STATUS_ERROR_DECODING_ERROR;
	}
	if (ctx->failed)
		return VA_STATUS_ERROR_DECODING_ERROR;

	if (output_ensure(ctx, out, ctx->au.len) < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	dmabuf_sync(out->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	memcpy(out->map, ctx->au.data, ctx->au.len);
	dmabuf_sync(out->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

	seq = ++ctx->seq;
	if (!seq)
		seq = ++ctx->seq;

	b.index = i;
	b.timestamp = seq_timeval(seq);
	plane.m.fd = out->fd;
	plane.length = out->size;
	plane.bytesused = ctx->au.len;
	if (ioctl(ctx->video_fd, VIDIOC_QBUF, &b) < 0) {
		v4l2r_log("OUTPUT QBUF (%zu bytes) failed: %s\n", ctx->au.len,
			  strerror(errno));
		ctx->seq--;
		return VA_STATUS_ERROR_DECODING_ERROR;
	}
	out->queued = true;
	ctx->frames_in++;

	/* The vendor control only reaches the firmware once the instance
	 * exists, i.e. after the first OUTPUT buffer. */
	if (!ctx->no_reorder_set) {
		struct v4l2_control c = { .id = SD_CID_VDEC_NO_REORDER, .value = 1 };
		int ret = ioctl(ctx->video_fd, VIDIOC_S_CTRL, &c);

		ctx->no_reorder_set = true;
		v4l2r_trace("V4L2_CID_VDEC_NO_REORDER: %s\n",
			    ret < 0 ? strerror(errno) : "set");
	}

	slot = &ctx->pending[seq % SD_PENDING_SLOTS];
	if (slot->surface && slot->surface->pending_seq == slot->seq) {
		v4l2r_log("frame seq %u never came back\n", slot->seq);
		slot->surface->pending_seq = 0;
		ctx->frames_dropped++;
	}
	slot->seq = seq;
	slot->error = false;
	slot->surface = target;
	slot->submitted_ns = v4l2r_now_ns();

	target->ctx = ctx;
	target->decode_seq = seq;
	target->pending_seq = ctx->expect_output ? seq : 0;

	v4l2r_trace("submit seq %u -> surface 0x%08x, %zu bytes, buf %u%s\n",
		    seq, target->id, ctx->au.len, i,
		    ctx->expect_output ? "" : " (no output expected)");

	service(ctx, 0);
	return VA_STATUS_SUCCESS;
}

/* --- VA entrypoints --- */

VAStatus v4l2r_CreateContext(VADriverContextP va_ctx, VAConfigID config_id,
			     int picture_width, int picture_height, int flag,
			     VASurfaceID *render_targets, int num_render_targets,
			     VAContextID *context_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2_event_subscription sub = {0};
	struct v4l2_requestbuffers r = {0};
	enum v4l2_buf_type type = SD_OUT_TYPE;
	struct v4l2r_config *config;
	struct v4l2r_context *ctx;
	const struct sd_codec_ops *ops;
	uint32_t sizeimage;
	VAContextID id;

	(void)flag;
	(void)render_targets;
	(void)num_render_targets;

	config = V4L2R_CONFIG_GET(drv, config_id);
	if (!config)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	if (picture_width <= 0 || picture_height <= 0 ||
	    (uint32_t)picture_width * picture_height >
	    drv->max_width * drv->max_height)
		return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

	ops = config->codec == SD_CODEC_H264 ? &sd_codec_h264 :
	      config->codec == SD_CODEC_VP9 ? &sd_codec_vp9 : NULL;
	if (!ops)
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

	pthread_mutex_lock(&drv->mutex);
	id = v4l2r_handles_alloc(&drv->contexts, sizeof(*ctx));
	ctx = V4L2R_CONTEXT(drv, id);
	pthread_mutex_unlock(&drv->mutex);
	if (!ctx)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	ctx->drv = drv;
	ctx->profile = config->profile;
	ctx->codec = config->codec;
	ctx->picture_width = picture_width;
	ctx->picture_height = picture_height;
	ctx->video_fd = -1;
	ctx->heap_fd = -1;
	ctx->capture_heap_fd = -1;
	pthread_mutex_init(&ctx->mutex, NULL);
	for (unsigned int i = 0; i < SD_OUTPUT_BUFFERS; i++)
		ctx->output[i].fd = -1;
	for (unsigned int i = 0; i < SD_MAX_CAPTURE; i++)
		ctx->capture[i].fd[0] = ctx->capture[i].fd[1] = -1;

	ctx->codec_priv = calloc(1, ops->priv_size);
	if (!ctx->codec_priv)
		goto fail;

	ctx->heap_fd = heap_open("MTK_STATEFUL_HEAP");
	if (ctx->heap_fd < 0)
		goto fail;
	ctx->capture_heap_fd = heap_open(getenv("MTK_STATEFUL_CAPTURE_HEAP") ?
					 "MTK_STATEFUL_CAPTURE_HEAP" :
					 "MTK_STATEFUL_HEAP");
	if (ctx->capture_heap_fd < 0)
		goto fail;

	ctx->video_fd = open(drv->video_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctx->video_fd < 0) {
		v4l2r_log("cannot open %s: %s\n", drv->video_path, strerror(errno));
		goto fail;
	}

	sub.type = V4L2_EVENT_SOURCE_CHANGE;
	if (ioctl(ctx->video_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		v4l2r_log("cannot subscribe to SOURCE_CHANGE: %s\n", strerror(errno));
		goto fail;
	}
	sub.type = V4L2_EVENT_EOS;
	ioctl(ctx->video_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
	sub.type = SD_EVENT_MTK_VDEC_ERROR;
	ioctl(ctx->video_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

	/* Bitstream buffers: room for a large intra frame, they grow on
	 * demand (output_ensure). */
	sizeimage = picture_width * picture_height / 2;
	if (sizeimage < 1024 * 1024)
		sizeimage = 1024 * 1024;
	ctx->output_format.type = SD_OUT_TYPE;
	ctx->output_format.fmt.pix_mp.pixelformat = ops->pixelformat;
	ctx->output_format.fmt.pix_mp.width = picture_width;
	ctx->output_format.fmt.pix_mp.height = picture_height;
	ctx->output_format.fmt.pix_mp.num_planes = 1;
	ctx->output_format.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
	if (ioctl(ctx->video_fd, VIDIOC_S_FMT, &ctx->output_format) < 0) {
		v4l2r_log("OUTPUT S_FMT %.4s %dx%d failed: %s\n",
			  (const char *)&ops->pixelformat, picture_width,
			  picture_height, strerror(errno));
		goto fail;
	}
	if (ctx->output_format.fmt.pix_mp.plane_fmt[0].sizeimage < 64 * 1024)
		ctx->output_format.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;

	r.count = SD_OUTPUT_BUFFERS;
	r.type = SD_OUT_TYPE;
	r.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(ctx->video_fd, VIDIOC_REQBUFS, &r) < 0 ||
	    r.count < SD_OUTPUT_BUFFERS) {
		v4l2r_log("OUTPUT REQBUFS failed (%u): %s\n", r.count,
			  strerror(errno));
		goto fail;
	}

	if (ioctl(ctx->video_fd, VIDIOC_STREAMON, &type) < 0) {
		v4l2r_log("OUTPUT STREAMON failed: %s\n", strerror(errno));
		goto fail;
	}

	v4l2r_trace("context %dx%d %.4s on %s\n", picture_width, picture_height,
		    (const char *)&ops->pixelformat, drv->video_path);

	*context_id = id;
	return VA_STATUS_SUCCESS;

fail:
	v4l2r_DestroyContext(va_ctx, id);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

/* Let the firmware finish what it has (decoded frames still land in their
 * surfaces), bounded: a stop command drains up to a LAST buffer. */
static void drain(struct v4l2r_context *ctx)
{
	struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
	uint64_t deadline = v4l2r_now_ns() + 300000000ull;
	bool pending = false;

	for (unsigned int i = 0; i < SD_PENDING_SLOTS; i++)
		if (ctx->pending[i].surface &&
		    ctx->pending[i].surface->pending_seq == ctx->pending[i].seq)
			pending = true;
	if (!pending || !ctx->capture_ready || ctx->failed)
		return;

	if (ioctl(ctx->video_fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
		v4l2r_trace("DEC_CMD_STOP: %s\n", strerror(errno));
		return;
	}

	while (v4l2r_now_ns() < deadline) {
		struct v4l2_plane planes[2] = {0};
		struct v4l2_buffer b = {
			.type = SD_CAP_TYPE,
			.memory = V4L2_MEMORY_DMABUF,
			.length = ctx->capture_planes,
			.m.planes = planes,
		};
		struct pollfd pfd = { .fd = ctx->video_fd, .events = POLLIN };

		if (ioctl(ctx->video_fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EPIPE)
				break;
			poll(&pfd, 1, 20);
			continue;
		}
		if (b.index < ctx->nb_captures)
			deliver(ctx, &b);
		if (b.flags & V4L2_BUF_FLAG_LAST)
			break;
	}
}

VAStatus v4l2r_DestroyContext(VADriverContextP va_ctx, VAContextID context_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	struct v4l2r_surface *surface;
	unsigned int iter = 0;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->video_fd >= 0) {
		enum v4l2_buf_type type = SD_OUT_TYPE;
		struct v4l2_requestbuffers r = {
			.count = 0,
			.type = SD_OUT_TYPE,
			.memory = V4L2_MEMORY_DMABUF,
		};

		drain(ctx);
		capture_teardown(ctx);
		ioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type);
		ioctl(ctx->video_fd, VIDIOC_REQBUFS, &r);
		close(ctx->video_fd);
		ctx->video_fd = -1;
	}
	for (unsigned int i = 0; i < SD_OUTPUT_BUFFERS; i++)
		output_release(&ctx->output[i]);
	if (ctx->heap_fd >= 0)
		close(ctx->heap_fd);
	if (ctx->capture_heap_fd >= 0)
		close(ctx->capture_heap_fd);
	pthread_mutex_unlock(&ctx->mutex);

	if (ctx->frames_in)
		v4l2r_log("context done: %llu pictures in, %llu frames out, "
			  "%llu lost, %u decode errors, max latency %u, "
			  "detile %.2f ms/frame, %u source changes\n",
			  (unsigned long long)ctx->frames_in,
			  (unsigned long long)ctx->frames_out,
			  (unsigned long long)ctx->frames_dropped,
			  ctx->decode_errors,
			  ctx->max_latency,
			  ctx->frames_out ? ctx->detile_ns / 1e6 / ctx->frames_out : 0.0,
			  ctx->source_changes);

	pthread_mutex_lock(&drv->mutex);
	while ((surface = v4l2r_handles_next(&drv->surfaces, &iter, NULL))) {
		if (surface->ctx == ctx) {
			surface->ctx = NULL;
			surface->pending_seq = 0;
		}
	}
	pthread_mutex_unlock(&drv->mutex);

	if (ctx->codec_priv) {
		const struct sd_codec_ops *ops = ctx->codec == SD_CODEC_H264 ?
			&sd_codec_h264 : &sd_codec_vp9;

		if (ops->uninit)
			ops->uninit(ctx);
		free(ctx->codec_priv);
	}
	sd_bytes_free(&ctx->au);
	pthread_mutex_destroy(&ctx->mutex);

	pthread_mutex_lock(&drv->mutex);
	v4l2r_handles_free(&drv->contexts, context_id);
	pthread_mutex_unlock(&drv->mutex);

	return VA_STATUS_SUCCESS;
}

static const struct sd_codec_ops *ctx_ops(struct v4l2r_context *ctx)
{
	return ctx->codec == SD_CODEC_H264 ? &sd_codec_h264 : &sd_codec_vp9;
}

VAStatus v4l2r_BeginPicture(VADriverContextP va_ctx, VAContextID context_id,
			    VASurfaceID render_target)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	struct v4l2r_surface *surface;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	surface = V4L2R_SURFACE_GET(drv, render_target);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!surface)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (ctx->failed)
		return VA_STATUS_ERROR_DECODING_ERROR;

	/* Last decoded by another (live) context: detach it there. */
	if (surface->ctx && surface->ctx != ctx)
		sd_forget_surface(surface->ctx, surface);

	ctx->target = surface;
	ctx->au.len = 0;
	ctx->expect_output = true;
	ctx->in_picture = true;

	return ctx_ops(ctx)->begin_picture(ctx);
}

VAStatus v4l2r_RenderPicture(VADriverContextP va_ctx, VAContextID context_id,
			     VABufferID *buffers, int num_buffers)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	VAStatus status;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!ctx->in_picture)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	for (int i = 0; i < num_buffers; i++) {
		struct v4l2r_buffer *buffer = V4L2R_BUFFER_GET(drv, buffers[i]);

		if (!buffer)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		status = ctx_ops(ctx)->render_buffer(ctx, buffer);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus v4l2r_EndPicture(VADriverContextP va_ctx, VAContextID context_id)
{
	struct v4l2r_driver *drv = v4l2r_driver(va_ctx);
	struct v4l2r_context *ctx;
	VAStatus status;

	ctx = V4L2R_CONTEXT_GET(drv, context_id);
	if (!ctx)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!ctx->in_picture)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	ctx->in_picture = false;

	status = ctx_ops(ctx)->end_picture(ctx);
	if (status != VA_STATUS_SUCCESS)
		return status;

	pthread_mutex_lock(&ctx->mutex);
	status = submit(ctx);
	pthread_mutex_unlock(&ctx->mutex);

	ctx->target = NULL;
	return status;
}
