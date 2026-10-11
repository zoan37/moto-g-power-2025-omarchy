/*
 * Object handle table (from libva-v4l2_request 1.3).
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "mtk_stateful.h"

#define V4L2R_HANDLES_INITIAL	32

int v4l2r_handles_init(struct v4l2r_handles *h, uint32_t id_offset)
{
	h->slots = calloc(V4L2R_HANDLES_INITIAL, sizeof(*h->slots));
	if (!h->slots)
		return -1;

	h->size = V4L2R_HANDLES_INITIAL;
	h->id_offset = id_offset;
	return 0;
}

void v4l2r_handles_destroy(struct v4l2r_handles *h)
{
	for (unsigned int i = 0; i < h->size; i++)
		free(h->slots[i]);
	free(h->slots);
	h->slots = NULL;
	h->size = 0;
}

uint32_t v4l2r_handles_alloc(struct v4l2r_handles *h, size_t object_size)
{
	unsigned int i;
	void *object;

	for (i = 0; i < h->size; i++) {
		if (!h->slots[i])
			break;
	}

	if (i == h->size) {
		unsigned int size = h->size * 2;
		void **slots = realloc(h->slots, size * sizeof(*slots));
		if (!slots)
			return VA_INVALID_ID;
		memset(slots + h->size, 0, h->size * sizeof(*slots));
		h->slots = slots;
		h->size = size;
	}

	object = calloc(1, object_size);
	if (!object)
		return VA_INVALID_ID;

	h->slots[i] = object;
	return h->id_offset + i;
}

void *v4l2r_handles_lookup(struct v4l2r_handles *h, uint32_t id)
{
	uint32_t index = id - h->id_offset;

	if (id < h->id_offset || index >= h->size)
		return NULL;

	return h->slots[index];
}

void v4l2r_handles_free(struct v4l2r_handles *h, uint32_t id)
{
	uint32_t index = id - h->id_offset;

	if (id < h->id_offset || index >= h->size)
		return;

	free(h->slots[index]);
	h->slots[index] = NULL;
}

void *v4l2r_handles_next(struct v4l2r_handles *h, unsigned int *iter,
			 uint32_t *id)
{
	for (unsigned int i = *iter; i < h->size; i++) {
		if (h->slots[i]) {
			*iter = i + 1;
			if (id)
				*id = h->id_offset + i;
			return h->slots[i];
		}
	}

	*iter = h->size;
	return NULL;
}
