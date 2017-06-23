/*
 * Copyright © 2017 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Author:
 *	Boris Brezillon <boris.brezillon@free-electrons.com>
 */

#ifndef __IGT_FRAMEBUFFER_H__
#define __IGT_FRAMEBUFFER_H__

#include <stdlib.h>
#include <xf86drm.h>

#include "igt_bo.h"
#include "igt_dev.h"

typedef struct igt_fb_format_info {
	uint32_t id;
	uint8_t nplanes;
	uint8_t cpp[3];
} igt_fb_format_info_t;

#define IGT_MAX_FB_PLANES	4

typedef struct igt_fb_plane {
	igt_bo_t *bo;
	uint32_t pitch;
	uint32_t offset;
} igt_fb_plane_t;

typedef struct igt_framebuffer igt_framebuffer_t;

typedef struct igt_framebuffer_ops {
	int (*map)(igt_framebuffer_t *fb, bool linear);
	int (*unmap)(igt_framebuffer_t *fb);
} igt_framebuffer_ops_t;

typedef struct igt_framebuffer {
	igt_dev_t *dev;
	const igt_framebuffer_ops_t *ops;
	int refcnt;
	uint32_t id;
	uint32_t format;
	int width;
	int height;
	uint64_t modifier;
	igt_fb_plane_t planes[IGT_MAX_FB_PLANES];
	void *planeptrs[IGT_MAX_FB_PLANES];
	void *priv;
} igt_framebuffer_t;

const igt_fb_format_info_t *igt_get_fb_format_info(uint32_t format);
igt_framebuffer_t *igt_framebuffer_create(igt_dev_t *dev, int width,
					  int height, uint32_t format,
					  uint64_t modifier,
					  const igt_fb_plane_t *planes,
					  const igt_framebuffer_ops_t *ops);
igt_framebuffer_t *igt_framebuffer_ref(igt_framebuffer_t *fb);
void igt_framebuffer_unref(igt_framebuffer_t *fb);
int igt_framebuffer_map(igt_framebuffer_t *fb, bool linear);
void *igt_framebuffer_get_ptr(igt_framebuffer_t *fb, int plane);
int igt_framebuffer_unmap(igt_framebuffer_t *fb);

igt_framebuffer_t *igt_dumb_new_framebuffer(igt_dev_t *dev, int width,
					    int height, uint32_t format,
					    uint64_t modifier);

#endif /* __IGT_FRAMEBUFFER_H__ */
