/*
 * Small bitstream reader (from libva-v4l2_request 1.3) used to parse slice
 * header fields VA-API does not expose, and a writer for the SPS/PPS NAL
 * units this driver synthesizes.
 *
 * The RBSP variant transparently strips H.264/HEVC emulation prevention
 * bytes; bit positions are counted in RBSP bits, matching the semantics
 * of the V4L2 *_bit_size control fields.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef V4L2R_BITS_H
#define V4L2R_BITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct v4l2r_bits {
	const uint8_t *data;
	size_t size;		/* in bytes */
	size_t byte;		/* next byte to load */
	uint8_t cur;		/* current byte */
	unsigned int left;	/* bits left in cur */
	size_t pos;		/* bits consumed (RBSP domain when stripping) */
	unsigned int zeros;	/* consecutive zero bytes seen */
	bool rbsp;		/* strip emulation prevention bytes */
	bool error;		/* ran past the end */
};

static inline void v4l2r_bits_init(struct v4l2r_bits *b, const uint8_t *data,
				   size_t size, bool rbsp)
{
	b->data = data;
	b->size = size;
	b->byte = 0;
	b->cur = 0;
	b->left = 0;
	b->pos = 0;
	b->zeros = 0;
	b->rbsp = rbsp;
	b->error = false;
}

static inline uint32_t v4l2r_bits_bit(struct v4l2r_bits *b)
{
	if (!b->left) {
		if (b->byte >= b->size) {
			b->error = true;
			return 0;
		}

		if (b->rbsp && b->zeros >= 2 && b->data[b->byte] == 0x03) {
			b->byte++;
			b->zeros = 0;
			if (b->byte >= b->size) {
				b->error = true;
				return 0;
			}
		}

		b->cur = b->data[b->byte++];
		b->zeros = b->cur ? 0 : b->zeros + 1;
		b->left = 8;
	}

	b->left--;
	b->pos++;
	return (b->cur >> b->left) & 1;
}

static inline uint32_t v4l2r_bits_read(struct v4l2r_bits *b, unsigned int count)
{
	uint32_t value = 0;

	while (count--)
		value = (value << 1) | v4l2r_bits_bit(b);

	return value;
}

static inline void v4l2r_bits_skip(struct v4l2r_bits *b, size_t count)
{
	while (count--)
		v4l2r_bits_bit(b);
}

/* Exp-Golomb ue(v) */
static inline uint32_t v4l2r_bits_ue(struct v4l2r_bits *b)
{
	unsigned int zeros = 0;

	while (!v4l2r_bits_bit(b)) {
		if (b->error)
			return 0;
		if (zeros >= 32) {
			/* Malformed code: flag the error so callers relying on
			 * b->error (and info->valid) treat the parse as failed
			 * instead of silently trusting a desynced result. */
			b->error = true;
			return 0;
		}
		zeros++;
	}

	return (1u << zeros) - 1 + v4l2r_bits_read(b, zeros);
}

/* Exp-Golomb se(v) */
static inline int32_t v4l2r_bits_se(struct v4l2r_bits *b)
{
	uint32_t value = v4l2r_bits_ue(b);

	if (value & 1)
		return (int32_t)((value + 1) / 2);
	return -(int32_t)(value / 2);
}

/* --- writer (RBSP; emulation prevention is applied by sd_nal_escape) --- */

struct v4l2r_bitw {
	uint8_t *data;
	size_t size;		/* capacity in bytes */
	size_t pos;		/* bits written */
	bool error;		/* overflow */
};

static inline void v4l2r_bitw_init(struct v4l2r_bitw *w, uint8_t *data,
				   size_t size)
{
	w->data = data;
	w->size = size;
	w->pos = 0;
	w->error = false;
}

static inline void v4l2r_bitw_bit(struct v4l2r_bitw *w, uint32_t bit)
{
	size_t byte = w->pos >> 3;
	unsigned int shift = 7 - (w->pos & 7);

	if (byte >= w->size) {
		w->error = true;
		return;
	}
	if (!(w->pos & 7))
		w->data[byte] = 0;
	if (bit & 1)
		w->data[byte] |= 1u << shift;
	w->pos++;
}

static inline void v4l2r_bitw_write(struct v4l2r_bitw *w, uint32_t value,
				    unsigned int count)
{
	while (count--)
		v4l2r_bitw_bit(w, value >> count);
}

static inline void v4l2r_bitw_ue(struct v4l2r_bitw *w, uint32_t value)
{
	uint64_t v = (uint64_t)value + 1;
	unsigned int bits = 0;

	while ((v >> bits) > 1)
		bits++;
	v4l2r_bitw_write(w, 0, bits);
	v4l2r_bitw_write(w, 1, 1);
	v4l2r_bitw_write(w, (uint32_t)(v & ((1ull << bits) - 1)), bits);
}

static inline void v4l2r_bitw_se(struct v4l2r_bitw *w, int32_t value)
{
	v4l2r_bitw_ue(w, value > 0 ? 2 * (uint32_t)value - 1 :
				     2 * (uint32_t)(-(int64_t)value));
}

/* rbsp_trailing_bits(); returns the RBSP size in bytes. */
static inline size_t v4l2r_bitw_trailing(struct v4l2r_bitw *w)
{
	v4l2r_bitw_bit(w, 1);
	while (w->pos & 7)
		v4l2r_bitw_bit(w, 0);
	return w->pos >> 3;
}

#endif /* V4L2R_BITS_H */
