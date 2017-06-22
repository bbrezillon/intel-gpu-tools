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

#include <drm_fourcc.h>

#include "drmtest.h"
#include "igt_framebuffer.h"
#include "ioctl_wrappers.h"

static const igt_fb_format_info_t formats[] = {
	{ .id = DRM_FORMAT_RGB565, .nplanes = 1, .cpp = { 16 } },
	{ .id = DRM_FORMAT_XRGB8888, .nplanes = 1, .cpp = { 32 } },
	{ .id = DRM_FORMAT_XRGB2101010,	.nplanes = 1, .cpp = { 32 } },
	{ .id = DRM_FORMAT_ARGB8888,	.nplanes = 1, .cpp = { 32 } },
};

const igt_fb_format_info_t *igt_get_fb_format_info(uint32_t format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if (formats[i].id == format)
			return &formats[i];
	}

	return NULL;
}

igt_framebuffer_t *igt_framebuffer_create(igt_dev_t *dev, int width,
					 int height, uint32_t format,
					 uint64_t modifier,
					 const igt_fb_plane_t *planes)
{
	struct drm_mode_fb_cmd2 addfb = { };
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	igt_framebuffer_t *fb;
	int i;

	igt_require_fb_modifiers(dev->fd);

	fb = calloc(sizeof(*fb), 1);
	if (!fb)
		return NULL;

	addfb.width  = width;
	addfb.height = height;
	addfb.pixel_format = format;

	if (modifier != LOCAL_DRM_FORMAT_MOD_NONE)
		addfb.flags = LOCAL_DRM_MODE_FB_MODIFIERS;

	fb->dev = dev;
	fb->height = height;
	fb->width = width;
	fb->format = format;
	fb->modifier = modifier;
	fb->refcnt = 1;

	for (i = 0; i < finfo->nplanes; i++) {
		fb->planes[i] = planes[i];
		fb->planes[i].bo = igt_bo_ref(fb->planes[i].bo);
		addfb.handles[i] = fb->planes[i].bo->handle;
		addfb.pitches[i] = fb->planes[i].pitch;
		addfb.modifier[i] = modifier;
	}

	do_ioctl(dev->fd, DRM_IOCTL_MODE_ADDFB, &addfb);
	fb->id = addfb.fb_id;

	return fb;
}

igt_framebuffer_t *igt_framebuffer_ref(igt_framebuffer_t *fb)
{
	if (fb)
		fb->refcnt++;

	return fb;
}

void igt_framebuffer_unref(igt_framebuffer_t *fb)
{
	int i;

	if (--fb->refcnt)
		return;

	do_ioctl(fb->dev->fd, DRM_IOCTL_MODE_RMFB, &fb->id);

	for (i = 0; i < ARRAY_SIZE(fb->planes); i++) {
		if (!fb->planes[i].bo)
			break;

		igt_bo_unref(fb->planes[i].bo);
	}
}

int igt_framebuffer_map(igt_framebuffer_t *fb, bool linear)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fb->planes) && fb->planes[i].bo; i++) {
		if (!igt_bo_map(fb->planes[i].bo, linear))
			goto err_unmap;
	}

	return 0;

err_unmap:
	for (i--; i >= 0; i--)
		igt_bo_unmap(fb->planes[i].bo);

	return -EINVAL;
}

void *igt_framebuffer_get_ptr(igt_framebuffer_t *fb, int plane)
{

	if (plane > ARRAY_SIZE(fb->planes) || !fb->planes[plane].bo ||
	    !fb->planes[plane].bo->map)
		return NULL;

	return (uint8_t *)fb->planes[plane].bo->map + fb->planes[plane].offset;
}

int igt_framebuffer_unmap(igt_framebuffer_t *fb)
{
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(fb->planes) && fb->planes[i].bo; i--) {
		ret = igt_bo_unmap(fb->planes[i].bo);
		if (ret)
			return ret;
	}

	return 0;
}

igt_framebuffer_t *igt_dumb_new_framebuffer(igt_dev_t *dev, int width,
					    int height, uint32_t format,
					    uint64_t modifier)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	igt_fb_plane_t fbplanes[IGT_MAX_FB_PLANES] = { };
	int i;

	if (!finfo || modifier != LOCAL_DRM_FORMAT_MOD_NONE)
		return NULL;

	for (i = 0; i < finfo->nplanes; i++)
		fbplanes[i].bo = igt_dumb_new_bo(dev, width, height, format, i,
						 &fbplanes[i].pitch);

	return igt_framebuffer_create(dev, width, height, format, modifier,
				      fbplanes);
}
