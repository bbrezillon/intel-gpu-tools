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

#include <stdlib.h>

#include "drmtest.h"
#include "igt_bo.h"
#include "igt_framebuffer.h"
#include "ioctl_wrappers.h"

igt_bo_t *igt_bo_create(igt_dev_t *dev, const igt_bo_ops_t *ops,
			uint32_t handle, size_t size)
{
	igt_bo_t *bo;

	bo = calloc(sizeof(*bo), 1);
	if (!bo)
		return NULL;

	bo->dev = dev;
	bo->size = size;
	bo->handle = handle;
	bo->refcnt = 1;

	return bo;
}

igt_bo_t *igt_bo_ref(igt_bo_t *bo)
{
	if (bo)
		bo->refcnt++;

	return bo;
}

void igt_bo_unref(igt_bo_t *bo)
{
	if (--bo->refcnt)
		return;

	bo->ops->destroy(bo);
	free(bo);
}

void *igt_bo_map(igt_bo_t *bo, bool linear)
{
	if (bo->map) {
		if (linear != bo->linearmap)
			return NULL;

		bo->mapcnt++;
		return bo->map;
	}

	bo->map =  bo->ops->map(bo, linear);
	if (bo->map) {
		bo->mapcnt = 1;
		bo->linearmap = linear;
	}

	return bo->map;
}

int igt_bo_unmap(igt_bo_t *bo)
{
	int ret;

	if (bo->mapcnt > 1)
		return --(bo->mapcnt);

	ret = bo->ops->unmap(bo);
	if (!ret) {
		bo->mapcnt = 0;
		bo->map = NULL;
	}

	return ret;
}

static void dumb_bo_destroy(igt_bo_t *bo)
{
	struct drm_gem_close close = { .handle = bo->handle };

	do_ioctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &close);
}

static void *dumb_bo_map(igt_bo_t *bo, bool linear)
{
	struct drm_mode_map_dumb arg = { .handle = bo->handle };
	void *ptr;

	do_ioctl(bo->dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
	ptr = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   bo->dev->fd, arg.offset);
	igt_assert(ptr != MAP_FAILED);

	return ptr;
}

static int dumb_bo_unmap(igt_bo_t *bo)
{
	munmap(bo->map, bo->size);
	return 0;
}

static const igt_bo_ops_t dumb_bo_ops = {
	.map = dumb_bo_map,
	.unmap = dumb_bo_unmap,
	.destroy = dumb_bo_destroy,
};

igt_bo_t *igt_dumb_new_bo(igt_dev_t *dev, int width, int height,
			  uint32_t format, int plane, uint32_t *pitch)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	struct drm_mode_create_dumb create = { };
	igt_bo_t *bo;

	if (plane < 0 || plane >= finfo->nplanes)
		return NULL;

	create.width = width;
	create.height = height;
	create.bpp = finfo->cpp[plane] * 8;

	do_ioctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	igt_assert(create.handle);
	igt_assert(create.size >= width * height * finfo->cpp[plane]);
	bo = igt_bo_create(dev, &dumb_bo_ops, create.handle, create.size);
	if (!bo) {
		struct drm_mode_destroy_dumb destroy;

		destroy.handle = create.handle;
		do_ioctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		return NULL;
	}

	if (pitch)
		*pitch = create.pitch;

	return bo;
}
