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

#ifndef __IGT_BO_H__
#define __IGT_BO_H__

#include <stdbool.h>

#include "igt_dev.h"

typedef struct igt_bo igt_bo_t;

typedef struct igt_bo_ops {
	void *(*map)(igt_bo_t *bo, int prot, int flags);
	int (*unmap)(igt_bo_t *bo, void *ptr);
	void (*destroy)(igt_bo_t *bo);
} igt_bo_ops_t;

struct igt_bo {
	igt_dev_t *dev;
	size_t size;
	unsigned handle;
	int refcnt;
	const igt_bo_ops_t *ops;
	void *priv;
};

igt_bo_t *igt_bo_create(igt_dev_t *dev, const igt_bo_ops_t *ops,
			uint32_t handle, size_t size);
igt_bo_t *igt_bo_ref(igt_bo_t *bo);
void igt_bo_unref(igt_bo_t *bo);
void *igt_bo_map(igt_bo_t *bo, int prot, int flags);
int igt_bo_unmap(igt_bo_t *bo, void *ptr);

igt_bo_t *igt_dumb_new_bo(igt_dev_t *dev, int width, int height,
			  uint32_t format, int plane, uint32_t *pitch);

#endif /* __IGT_BO_H__ */
