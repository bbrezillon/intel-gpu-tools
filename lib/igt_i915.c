/*
 * Copyright © 2013,2014 Intel Corporation
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
 * Authors:
 * 	Daniel Vetter <daniel.vetter@ffwll.ch>
 * 	Damien Lespiau <damien.lespiau@intel.com>
 *	Boris Brezillon <boris.brezillon@free-electrons.com>
 */

#include "drmtest.h"
#include "igt_fb.h"
#include "igt_i915.h"
#include "ioctl_wrappers.h"

static void *i915_bo_map(igt_bo_t *bo, int prot, int flags)
{
	gem_set_domain(bo->dev->fd, bo->handle, I915_GEM_DOMAIN_CPU,
		       I915_GEM_DOMAIN_CPU);
	return gem_mmap__gtt(bo->dev->fd, bo->handle, bo->size, prot);
}

static int i915_bo_unmap(igt_bo_t *bo, void *ptr)
{
	return gem_munmap(ptr, bo->size);
}

static void i915_bo_destroy(igt_bo_t *bo)
{
	gem_close(bo->dev->fd, bo->handle);
}

static const igt_bo_ops_t i915_bo_ops = {
	.map = i915_bo_map,
	.unmap = i915_bo_unmap,
	.destroy = i915_bo_destroy,
};

igt_bo_t *igt_i915_new_bo(igt_dev_t *dev, size_t size)
{
	struct drm_i915_gem_create create = { };
	igt_bo_t *bo;

	create.size = size;
	do_ioctl(dev->fd, DRM_IOCTL_I915_GEM_CREATE, &create);
	igt_assert(create.handle);

	bo = igt_bo_create(dev, &i915_bo_ops, create.handle, size);
	if (!bo) {
		gem_close(dev->fd, create.handle);
		return NULL;
	}

	return bo;
}

typedef struct igt_i915_fb {
	igt_fb_plane_t linearplane;
} igt_i915_fb_t;

static int i915_fb_map(igt_framebuffer_t *fb, bool linear)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(fb->format);
	igt_i915_fb_t *i915fb = fb->priv;
	unsigned int tiling;
	int i;

	/* Copy non-linear BO content to linear BO */
	gem_set_domain(fb->dev->fd, i915fb->linearplane.bo->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	if (!linear || fb->modifier == LOCAL_DRM_FORMAT_MOD_NONE) {
		for (i = 0; i < finfo->nplanes; i++) {
			uint8_t *ptr;

			ptr = igt_bo_map(fb->planes[i].bo,
					 PROT_READ | PROT_WRITE, MAP_SHARED);
			fb->planeptrs[i] = ptr + fb->planes[i].offset;
		}

		return 0;
	}

	i915fb->linearplane.bo = igt_dumb_new_bo(fb->dev, fb->width,
						 fb->height, fb->format,
						 LOCAL_DRM_FORMAT_MOD_NONE,
						 &i915fb->linearplane.pitch);
	i915fb->linearplane.offset = 0;

	tiling = igt_fb_mod_to_tiling(fb->modifier);

	/* Copy non-linear BO content to linear BO */
	gem_set_domain(fb->dev->fd, i915fb->linearplane.bo->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	igt_blitter_fast_copy__raw(fb->dev->fd, fb->planes[0].bo->handle,
				   fb->planes[0].pitch,
				   tiling, 0, 0, /* src_x, src_y */
				   fb->width, fb->height,
				   i915fb->linearplane.bo->handle,
				   i915fb->linearplane.pitch,
				   I915_TILING_NONE, 0, 0 /* dst_x, dst_y */);

	gem_sync(fb->dev->fd, i915fb->linearplane.bo->handle);

	gem_set_domain(fb->dev->fd, i915fb->linearplane.bo->handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	fb->planeptrs[i] = gem_mmap__cpu(fb->dev->fd,
					 i915fb->linearplane.bo->handle, 0,
					 i915fb->linearplane.bo->size,
					 PROT_READ | PROT_WRITE);

	return 0;
}

static int i915_fb_unmap(igt_framebuffer_t *fb)
{
	igt_i915_fb_t *i915fb = fb->priv;
	unsigned int tiling;
	uint8_t *ptr;
	int i;

	if (!i915fb->linearplane.bo) {
		for (i = 0; i < ARRAY_SIZE(fb->planes); i++) {
			ptr = fb->planeptrs[i];
			if (!ptr)
				break;

			igt_bo_unmap(fb->planes[i].bo,
				     ptr - fb->planes[i].offset);
			fb->planeptrs[i] = NULL;
		}

		return 0;
	}

	ptr = fb->planeptrs[0];
	gem_munmap(ptr - fb->planes[0].offset, i915fb->linearplane.bo->handle);

	gem_set_domain(fb->dev->fd, i915fb->linearplane.bo->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	tiling = igt_fb_mod_to_tiling(fb->modifier);
	igt_blitter_fast_copy__raw(fb->dev->fd, i915fb->linearplane.bo->handle,
				   i915fb->linearplane.pitch, I915_TILING_NONE,
				   0, 0, /* src_x, src_y */
				   fb->width, fb->height,
				   fb->planes[0].bo->handle, fb->planes[0].pitch,
				   tiling, 0, 0 /* dst_x, dst_y */);

	gem_sync(fb->dev->fd, i915fb->linearplane.bo->handle);
	igt_bo_unref(i915fb->linearplane.bo);
	memset(&i915fb->linearplane, 0, sizeof(i915fb->linearplane));

	return 0;
}

static const igt_framebuffer_ops_t i915_fb_ops = {
	.map = i915_fb_map,
	.unmap = i915_fb_unmap,
};

igt_framebuffer_t *igt_i915_new_framebuffer(igt_dev_t *dev, int width,
					    int height, uint32_t format,
					    uint64_t modifier)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	igt_fb_plane_t fbplanes[IGT_MAX_FB_PLANES] = { };
	unsigned int size, stride;
	igt_framebuffer_t *fb;
	igt_i915_fb_t *i915fb;
	uint32_t *ptr;

	if (finfo->nplanes > 1)
		return NULL;

	i915fb = calloc(1, sizeof(*i915fb));
	if (!i915fb)
		return NULL;

	igt_calc_fb_size(dev->fd, width, height, finfo->cpp[0] * 8, modifier,
			 &size, &stride);

	fbplanes[0].bo = igt_i915_new_bo(dev, size);
	fbplanes[0].offset = 0;
	fbplanes[0].pitch = stride;

	gem_set_tiling(dev->fd, fbplanes[0].bo->handle,
		       igt_fb_mod_to_tiling(modifier), stride);

	/* Ensure the framebuffer is preallocated */
	ptr = gem_mmap__gtt(dev->fd, fbplanes[0].bo->handle, size, PROT_READ);
	igt_assert(*ptr == 0);
	gem_munmap(ptr, size);

	fb = igt_framebuffer_create(dev, width, height, format, modifier,
				    fbplanes, &i915_fb_ops);
	fb->priv = i915fb;

	return fb;
}
