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

#include "igt_i915.h"

typedef struct igt_i915_bo {
	uint64_t mod;
	uint32_t pitch;
	int width;
	int height;
	struct {
		igt_bo_t *bo;
		uint32_t pitch;
	} linear;
} igt_i915_bo_t;


static void *i915_bo_map(igt_bo_t *bo, bool linear)
{
	igt_i915_bo_t *i915bo = bo->priv;
	unsigned int tiling;

	if (!linear) {
		gem_set_domain(bo->dev->fd, bo->handle,
			       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
		return gem_mmap__gtt(bo->dev->fd, bo->handle, bo->size,
				     PROT_READ | PROT_WRITE);
	}

	tiling = igt_fb_mod_to_tiling(i915bo->mod);

	/* Copy non-linear BO content to linear BO */
	gem_set_domain(bo->dev->fd, i915bo->linear.bo->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	igt_blitter_fast_copy__raw(bo->dev->fd, bo->handle, i915bo->pitch,
				   tiling, 0, 0, /* src_x, src_y */
				   i915bo->width, i915bo->height,
				   i915bo->linear.bo->handle,
				   i915bo->linear.pitch,
				   I915_TILING_NONE, 0, 0 /* dst_x, dst_y */);

	gem_sync(bo->dev->fd, i915bo->linear.bo->handle);

	gem_set_domain(bo->dev->fd, i915bo->linear.bo->handle, I915_GEM_DOMAIN_CPU,
		       I915_GEM_DOMAIN_CPU);

	return gem_mmap__cpu(bo->dev->fd, i915bo->linear.bo->handle, 0,
			     i915bo->linear.bo->size, PROT_READ | PROT_WRITE);
}

static int i915_bo_unmap(igt_bo_t *bo)
{
	igt_i915_bo_t *i915bo = bo->priv;
	unsigned int tiling;

	if (!bo->linearmap)
		return gem_munmap(bo->map, bo->size);

	gem_munmap(bo->map, i915bo->linear.bo->size);

	gem_set_domain(bo->dev->fd, i915bo->linear.bo->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	tiling = igt_fb_mod_to_tiling(i915bo->mod);
	igt_blitter_fast_copy__raw(bo->dev->fd,
				   i915bo->linear.bo->handle,
				   i915bo->linear.pitch,
				   I915_TILING_NONE,
				   0, 0, /* src_x, src_y */
				   i915bo->width, i915bo->height,
				   bo->handle, i915bo->pitch,
				   tiling,
				   0, 0 /* dst_x, dst_y */);

	gem_sync(bo->dev->fd, i915bo->linear.bo->handle);

	return 0;
}

static void i915_bo_destroy(igt_bo_t *bo)
{
	igt_i915_bo_t *i915bo = bo->priv;

	gem_close(bo->dev->fd, bo->handle);
	if (i915bo->mod != LOCAL_DRM_FORMAT_MOD_NONE)
		igt_bo_unref(i915bo->linear.bo);

	free(i915bo);
}

static const igt_bo_ops_t i915_bo_ops = {
	.map = i915_bo_map,
	.unmap = i915_bo_unmap,
	.destroy = i915_bo_destroy,
};

igt_bo_t *igt_i915_new_bo(igt_dev_t *dev, int width, int height,
			  uint32_t format, uint64_t mod,
			  uint32_t *pitch)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	struct drm_i915_gem_create create = { };
	unsigned size, stride;
	igt_i915_bo_t *i915bo;
	igt_bo_t *bo;
	uint32_t *ptr;

	igt_calc_fb_size(dev->fd, width, height, finfo->cpp[0] * 8, mod,
			 &size, &stride);

	if (pitch)
		*pitch = stride;

	i915bo = calloc(sizeof(*i915bo), 1);
	if (!i915bo)
		return NULL;

	i915bo->pitch = stride;
	i915bo->height = height;
	i915bo->width = width;
	i915bo->mod = mod;

	if (i915bo->mod != LOCAL_DRM_FORMAT_MOD_NONE)
		i915bo->linear.bo = igt_dumb_new_bo(dev, width, height, format,
						    0, &i915bo->linear.pitch);

	create.size = size;
	do_ioctl(dev->fd, DRM_IOCTL_I915_GEM_CREATE, &create);
	igt_assert(create.handle);

	gem_set_tiling(dev->fd, create.handle, igt_fb_mod_to_tiling(mod),
		       stride);

	/* Ensure the framebuffer is preallocated */
	ptr = gem_mmap__gtt(dev->fd, create.handle, size, PROT_READ);
	igt_assert(*ptr == 0);
	gem_munmap(ptr, size);

	bo = igt_bo_create(dev, &i915_bo_ops, create.handle, size);
	if (!bo) {
		gem_close(dev->fd, create.handle);
		free(i915bo);
		return NULL;
	}

	bo->priv = i915bo;

	return bo;
}

igt_framebuffer_t *igt_i915_new_framebuffer(igt_dev_t *dev, int width,
					    int height, uint32_t format,
					    uint64_t modifier)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	igt_fb_plane_t fbplanes[IGT_MAX_FB_PLANES] = { };

	if (finfo->nplanes > 1)
		return NULL;

	fbplanes[0].bo = igt_i915_new_bo(dev, width, height, format,
					 modifier, &fbplanes[0].pitch);

	return igt_framebuffer_create(dev, width, height, format, modifier,
				      fbplanes);
}
