/*
 * MM21 to NV12 on the CPU (from the libva-v4l2_request MediaTek MM21 patch,
 * lenovo-slim3-chromebook-omarchy convert.c).
 *
 * MM21 stores each plane as a raster of tiles 16 bytes wide and 32 rows
 * tall for luma, 16 rows for the interleaved chroma; each tile is one
 * contiguous block (16 * tile_height bytes), and a row of tiles takes
 * bytesperline * tile_height bytes. The source is read strictly
 * sequentially, a whole tile at a time in 64-byte loads, and the rows are
 * scattered into the destination; bands of tile rows run on several
 * threads.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> (libva-v4l2_request)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "mtk_stateful.h"

static void detile_plane(const uint8_t *src, uint32_t src_stride,
			 uint8_t *dst, uint32_t dst_stride,
			 uint32_t width, uint32_t height, uint32_t tile_height)
{
	const size_t tile_size = 16 * tile_height;

	for (uint32_t y0 = 0; y0 < height; y0 += tile_height) {
		const uint8_t *tile = src + (size_t)(y0 / tile_height) * src_stride * tile_height;
		uint32_t rows = height - y0 < tile_height ? height - y0 : tile_height;
		uint8_t *drow = dst + (size_t)y0 * dst_stride;

		for (uint32_t x = 0; x < width; x += 16, tile += tile_size) {
			const uint8_t *s = tile;
			uint8_t *d = drow + x;
			uint32_t r = 0;

#if defined(__aarch64__)
			for (; r + 4 <= rows; r += 4, s += 64, d += 4 * (size_t)dst_stride) {
				uint8x16x4_t v = vld1q_u8_x4(s);

				vst1q_u8(d, v.val[0]);
				vst1q_u8(d + dst_stride, v.val[1]);
				vst1q_u8(d + 2 * (size_t)dst_stride, v.val[2]);
				vst1q_u8(d + 3 * (size_t)dst_stride, v.val[3]);
			}
#endif
			for (; r < rows; r++, s += 16, d += dst_stride)
				memcpy(d, s, 16);
		}
	}
}

#define DETILE_THREADS 4

struct detile_band {
	const uint8_t *src;
	uint32_t src_stride;
	uint8_t *dst;
	uint32_t dst_stride;
	uint32_t width, height, tile_height;
};

static void *detile_band_run(void *arg)
{
	struct detile_band *b = arg;

	detile_plane(b->src, b->src_stride, b->dst, b->dst_stride, b->width,
		     b->height, b->tile_height);
	return NULL;
}

/* Luma and chroma bands of the same rows go to the same thread, so a frame
 * costs one thread start per helper rather than one per plane. */
struct detile_job {
	struct detile_band y, uv;
};

static void *detile_job_run(void *arg)
{
	struct detile_job *j = arg;

	if (j->y.height)
		detile_band_run(&j->y);
	if (j->uv.height)
		detile_band_run(&j->uv);
	return NULL;
}

static unsigned int detile_threads(void)
{
	static int n = -1;

	if (n < 0) {
		const char *env = getenv("MTK_STATEFUL_DETILE_THREADS");

		n = env ? atoi(env) : DETILE_THREADS;
		if (n < 1)
			n = 1;
		if (n > DETILE_THREADS)
			n = DETILE_THREADS;
	}
	return n;
}

void sd_detile_mm21(const uint8_t *y, uint32_t y_stride,
		    const uint8_t *uv, uint32_t uv_stride,
		    uint32_t src_width, uint32_t src_height,
		    uint8_t *dst, uint32_t dst_pitch,
		    uint32_t dst_width, uint32_t dst_height)
{
	struct detile_job jobs[DETILE_THREADS];
	pthread_t threads[DETILE_THREADS];
	bool started[DETILE_THREADS] = { false };
	unsigned int n = detile_threads();
	uint32_t width, height, luma_rows, per;
	uint8_t *dst_uv;

	/* Clamp to the destination: the decoder works on aligned frames
	 * (1920x1088) while the surface may be 1920x1080. Whole 16-byte tile
	 * columns are copied as long as they fit the 64-aligned pitch. */
	width = (dst_width + 15) & ~15u;
	if (width > src_width)
		width = src_width;
	if (width > dst_pitch)
		width = dst_pitch & ~15u;
	height = dst_height < src_height ? dst_height : src_height;
	dst_uv = dst + (size_t)dst_pitch * dst_height;

	/* Split in bands of whole luma tile rows (32 luma rows = 16 chroma). */
	luma_rows = (height + 31) / 32;
	if (luma_rows < 2 * n)
		n = 1;
	per = (luma_rows + n - 1) / n;

	for (unsigned int i = 0; i < n; i++) {
		uint32_t r0 = i * per, r1 = (i + 1) * per;
		uint32_t y0 = r0 * 32, y1 = r1 * 32;

		if (y1 > height)
			y1 = height;
		if (y0 > y1)
			y0 = y1;
		jobs[i].y = (struct detile_band) {
			.src = y + (size_t)r0 * y_stride * 32,
			.src_stride = y_stride,
			.dst = dst + (size_t)y0 * dst_pitch,
			.dst_stride = dst_pitch,
			.width = width,
			.height = y1 - y0,
			.tile_height = 32,
		};
		jobs[i].uv = (struct detile_band) {
			.src = uv + (size_t)r0 * uv_stride * 16,
			.src_stride = uv_stride,
			.dst = dst_uv + (size_t)(y0 / 2) * dst_pitch,
			.dst_stride = dst_pitch,
			.width = width,
			.height = y1 / 2 - y0 / 2,
			.tile_height = 16,
		};
	}

	for (unsigned int i = 1; i < n; i++)
		started[i] = pthread_create(&threads[i], NULL, detile_job_run,
					    &jobs[i]) == 0;
	detile_job_run(&jobs[0]);
	for (unsigned int i = 1; i < n; i++) {
		if (started[i])
			pthread_join(threads[i], NULL);
		else
			detile_job_run(&jobs[i]);
	}
}
