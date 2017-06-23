/*
 * Copyright © 2016 Broadcom
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
 */

#include <assert.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_vc4.h"
#include "ioctl_wrappers.h"
#include "intel_reg.h"
#include "intel_chipset.h"
#include "vc4_drm.h"
#include "vc4_packet.h"

#if NEW_CONTEXT_PARAM_NO_ERROR_CAPTURE_API
#define LOCAL_CONTEXT_PARAM_NO_ERROR_CAPTURE 0x4
#endif

/**
 * SECTION:igt_vc4
 * @short_description: VC4 support library
 * @title: VC4
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing VC4
 * tests.
 */

/**
 * igt_vc4_get_cleared_bo:
 * @fd: device file descriptor
 * @size: size of the BO in bytes
 * @clearval: u32 value that the buffer should be completely cleared with
 *
 * This helper returns a new BO with the given size, which has just been
 * cleared using the render engine.
 */
uint32_t igt_vc4_get_cleared_bo(int fd, size_t size, uint32_t clearval)
{
	/* A single row will be a page. */
	uint32_t width = 1024;
	uint32_t height = size / (width * 4);
	uint32_t handle = igt_vc4_create_bo(fd, size);
	struct drm_vc4_submit_cl submit = {
		.color_write = {
			.hindex = 0,
			.bits = VC4_SET_FIELD(VC4_RENDER_CONFIG_FORMAT_RGBA8888,
					      VC4_RENDER_CONFIG_FORMAT),
		},

		.color_read = { .hindex = ~0 },
		.zs_read = { .hindex = ~0 },
		.zs_write = { .hindex = ~0 },
		.msaa_color_write = { .hindex = ~0 },
		.msaa_zs_write = { .hindex = ~0 },

		.bo_handles = to_user_pointer(&handle),
		.bo_handle_count = 1,
		.width = width,
		.height = height,
		.max_x_tile = ALIGN(width, 64) / 64 - 1,
		.max_y_tile = ALIGN(height, 64) / 64 - 1,
		.clear_color = { clearval, clearval },
		.flags = VC4_SUBMIT_CL_USE_CLEAR_COLOR,
	};

	igt_assert_eq_u32(width * height * 4, size);

	do_ioctl(fd, DRM_IOCTL_VC4_SUBMIT_CL, &submit);

	return handle;
}

int
igt_vc4_create_bo(int fd, size_t size)
{
	struct drm_vc4_create_bo create = {
		.size = size,
	};

	do_ioctl(fd, DRM_IOCTL_VC4_CREATE_BO, &create);

	return create.handle;
}

void *
igt_vc4_mmap_bo(int fd, uint32_t handle, uint32_t size, unsigned prot)
{
	struct drm_vc4_mmap_bo mmap_bo = {
		.handle = handle,
	};
	void *ptr;

	do_ioctl(fd, DRM_IOCTL_VC4_MMAP_BO, &mmap_bo);

	ptr = mmap(0, size, prot, MAP_SHARED, fd, mmap_bo.offset);
	if (ptr == MAP_FAILED)
		return NULL;
	else
		return ptr;
}

typedef struct igt_vc4_fb {
	igt_fb_plane_t linearplanes[IGT_MAX_FB_PLANES];
} igt_vc4_fb_t;

#define VC4_T_TILE_CPP			4
#define VC4_T_UTILE_LINE_SIZE		16
#define VC4_T_UTILE_SIZE		64
#define VC4_T_SUBTILE_LINE_SIZE		256
#define VC4_T_SUBTILE_SIZE		1024
#define VC4_T_TILE_SIZE			4096

/*
* Broadcom VC4 "T" format
*
* This is the primary layout that the V3D GPU can texture from (it
* can't do linear).  The T format has:
*
* - 64b utiles of pixels in a raster-order grid according to cpp.  It's 4x4
*   pixels at 32 bit depth.
*
* - 1k subtiles made of a 4x4 raster-order grid of 64b utiles (so usually
*   16x16 pixels).
*
* - 4k tiles made of a 2x2 grid of 1k subtiles (so usually 32x32 pixels).  On
*   even 4k tile rows, they're arranged as (BL, TL, TR, BR), and on odd rows
*   they're (TR, BR, BL, TL), where bottom left is start of memory.
*
* - an image made of 4k tiles in rows either left-to-right (even rows of 4k
*   tiles) or right-to-left (odd rows of 4k tiles).
*/
static void vc4_get_t_utile_pos(int xpos, int ypos,
				int tiles_per_row, int tiles_per_column,
				int linear_pitch,
				unsigned int *linearpos, unsigned int *tilepos)
{
	int xtile, ytile, xsubtile, ysubtile, xutile, yutile, xrem, yrem;

	/* A UTILE is 4 x 4 pixels, so xpos and ypos should be aligned on 4. */
	igt_assert(!(xpos % 4) || !(ypos % 4));

	/*
	 * Position of the first line of the tile in linear format is pretty
	 * easy to calculate:
	 *  (YPOS * LINE_PITCH) + (XPOS * NUM_BYTES_PER_PIXEL)
	 */
	*linearpos = (ypos * linear_pitch) + (xpos * VC4_T_TILE_CPP);

	/*
	 * It's a bit more complicated for the T-tile mode.
	 * First we need to identify the tile, subtile and utile attached to
	 * the xpos+ypos pair.
	 */
	xtile = xpos / 32;
	ytile = ypos / 32;
	xrem = xpos % 32;
	yrem = ypos % 32;
	xsubtile = xrem / 16;
	ysubtile = yrem / 16;
	xrem %= 16;
	yrem %= 16;
	xutile = xrem / 4;
	yutile = yrem / 4;
	xrem %= 4;
	yrem %= 4;

	/* Start at the beginning of the tile row containing this utile. */
	*tilepos = ytile *  tiles_per_row * VC4_T_TILE_SIZE;

	if (ytile % 2) {
		/*
		 * On odd rows, tiles are stored from right to left, and
		 * subtiles are arranged as (TR, BR, BL, TL) within each
		 * tile.
		 */
		*tilepos += (tiles_per_row - xtile) * VC4_T_TILE_SIZE;
		*tilepos += (((1 - xsubtile) * 2) + ysubtile) * VC4_T_TILE_SIZE;
	} else {
		/*
		 * On even rows, tiles are stored from left to right, and
		 * subtiles are arranged as (BL, TL, TR, BR) within each
		 * tile.
		 */
		*tilepos += xtile * VC4_T_TILE_SIZE;
		*tilepos += ((xsubtile * 2) + (1 - ysubtile)) * VC4_T_TILE_SIZE;
	}

	/*
	 * Finally reach the relevant utile within the subtile, which is easily
	 * extracted since utiles are stored in raster-order.
	 */
	*tilepos += ((yutile * 4) + xutile) * VC4_T_UTILE_SIZE;
}

static void vc4_bo_destroy(igt_bo_t *bo)
{
	struct drm_gem_close close = { .handle = bo->handle };

	do_ioctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &close);
}

static void *vc4_bo_map(igt_bo_t *bo, int prot, int flags)
{
	void *ptr;

	ptr = igt_vc4_mmap_bo(bo->dev->fd, bo->handle, bo->size, prot);
	igt_assert(ptr);

	return ptr;
}

static int vc4_bo_unmap(igt_bo_t *bo, void *ptr)
{
	munmap(ptr, bo->size);
	return 0;
}

static const igt_bo_ops_t vc4_bo_ops = {
	.map = vc4_bo_map,
	.unmap = vc4_bo_unmap,
	.destroy = vc4_bo_destroy,
};

igt_bo_t *igt_vc4_new_bo(igt_dev_t *dev, size_t size)
{
	return igt_bo_create(dev, &vc4_bo_ops,
			     igt_vc4_create_bo(dev->fd, size), size);
}

static void vc4_t_utile_from_linear(int xpos, int ypos, int tiles_per_row,
				    int tiles_per_column, int linear_pitch,
				    const uint8_t *linearbuf, uint8_t *tilebuf)
{
	unsigned int linearpos, tilepos, i;

	vc4_get_t_utile_pos(xpos, ypos, tiles_per_row, tiles_per_column,
			    linear_pitch, &linearpos, &tilepos);

	/* Copy the 4 lines of 4 pixels contained in the UTILE. */
	for (i = 0; i < 4; i++) {
		memcpy(tilebuf + tilepos, linearbuf + linearpos,
		       VC4_T_UTILE_LINE_SIZE);
		linearpos += linear_pitch;
		tilepos += VC4_T_UTILE_LINE_SIZE;
	}
}

static void vc4_t_utile_to_linear(int xpos, int ypos, int tiles_per_row,
				  int tiles_per_column, int linear_pitch,
				  uint8_t *linearbuf, const uint8_t *tilebuf)
{
	unsigned int linearpos, tilepos, i;

	vc4_get_t_utile_pos(xpos, ypos, tiles_per_row, tiles_per_column,
			    linear_pitch, &linearpos, &tilepos);

	/* Copy the 4 lines of 4 pixels contained in the UTILE. */
	for (i = 0; i < 4; i++) {
		memcpy(linearbuf + linearpos, tilebuf + tilepos,
		       VC4_T_UTILE_LINE_SIZE);
		linearpos += linear_pitch;
		tilepos += VC4_T_UTILE_LINE_SIZE;
	}
}

static int vc4_t_tile_fb_map_linear(igt_framebuffer_t *fb)
{
	int tiles_per_row, tiles_per_column, linear_pitch, xpos, ypos;
	igt_vc4_fb_t *vc4fb = fb->priv;
	uint8_t *tilemap, *linearmap;

	vc4fb->linearplanes[0].bo = igt_dumb_new_bo(fb->dev, fb->width,
						    fb->height, fb->format, 0,
						    &vc4fb->linearplanes[0].pitch);
	linearmap = igt_bo_map(vc4fb->linearplanes[0].bo,
			       PROT_READ | PROT_WRITE, MAP_SHARED);
	tilemap = igt_bo_map(fb->planes[0].bo, PROT_READ,
			     MAP_SHARED);
	tilemap += fb->planes[0].offset;

	tiles_per_row = fb->width / 32;
	tiles_per_column = fb->height / 32;
	linear_pitch = vc4fb->linearplanes[0].pitch;

	for (ypos = 0; ypos < tiles_per_column; ypos += 4) {
		for (xpos = 0; xpos < tiles_per_row; xpos += 4) {
			vc4_t_utile_to_linear(xpos, ypos, tiles_per_row,
					      tiles_per_column, linear_pitch,
					      linearmap, tilemap);
		}
	}

	fb->planeptrs[0] = linearmap;
	igt_bo_unmap(fb->planes[0].bo, tilemap);

	return 0;
}

static int vc4_t_tile_fb_unmap_linear(igt_framebuffer_t *fb)
{
	int tiles_per_row, tiles_per_column, linear_pitch, xpos, ypos;
	igt_vc4_fb_t *vc4fb = fb->priv;
	uint8_t *linearmap, *tilemap;

	linearmap = fb->planeptrs[0];
	tilemap = igt_bo_map(fb->planes[0].bo, PROT_READ | PROT_WRITE,
			     MAP_SHARED);
	tilemap += fb->planes[0].offset;

	tiles_per_row = fb->width / 32;
	tiles_per_column = fb->height / 32;
	linear_pitch = vc4fb->linearplanes[0].pitch;

	for (ypos = 0; ypos < tiles_per_column; ypos += 4) {
		for (xpos = 0; xpos < tiles_per_row; xpos += 4) {
			vc4_t_utile_from_linear(xpos, ypos, tiles_per_row,
						   tiles_per_column,
						   linear_pitch, linearmap,
						   tilemap);
		}
	}

	igt_bo_unmap(fb->planes[0].bo, tilemap - fb->planes[0].offset);
	igt_bo_unmap(vc4fb->linearplanes[0].bo, linearmap);
	igt_bo_unref(vc4fb->linearplanes[0].bo);
	memset(vc4fb->linearplanes, 0, sizeof(vc4fb->linearplanes));
	fb->planeptrs[0] = NULL;

	return 0;
}

static int vc4_fb_map(igt_framebuffer_t *fb, bool linear)
{
	int i;

	if (fb->modifier == LOCAL_BROADCOM_FORMAT_MOD_VC4_T_TILED &&
	    linear)
		return vc4_t_tile_fb_map_linear(fb);

	for (i = 0; i < ARRAY_SIZE(fb->planes) && fb->planes[i].bo; i++) {
		uint8_t *ptr;

		 ptr = igt_bo_map(fb->planes[i].bo, PROT_READ | PROT_WRITE,
				  MAP_SHARED);
		if (!ptr)
			goto err_unmap;

		fb->planeptrs[i] = ptr + fb->planes[i].offset;
	}

	return 0;

err_unmap:
	for (i--; i >= 0; i--) {
		igt_bo_unmap(fb->planes[i].bo, fb->planeptrs[i]);
		fb->planeptrs[i] = NULL;
	}

	return -EINVAL;
}

static int vc4_fb_unmap(igt_framebuffer_t *fb)
{
	igt_vc4_fb_t *vc4fb = fb->priv;
	int i;

	if (vc4fb->linearplanes[0].bo) {
		if (fb->modifier == LOCAL_BROADCOM_FORMAT_MOD_VC4_T_TILED)
			return vc4_t_tile_fb_unmap_linear(fb);

		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(fb->planes) && fb->planes[i].bo; i++) {
		igt_bo_unmap(fb->planes[i].bo, fb->planeptrs[i]);
		fb->planeptrs[i] = NULL;
	}

	return 0;
}

static const igt_framebuffer_ops_t vc4_fb_ops = {
	.map = vc4_fb_map,
	.unmap = vc4_fb_unmap,
};

igt_framebuffer_t *igt_vc4_new_framebuffer(igt_dev_t *dev, int width,
					   int height, uint32_t format,
					   uint64_t modifier)
{
	const igt_fb_format_info_t *finfo = igt_get_fb_format_info(format);
	igt_fb_plane_t fbplanes[IGT_MAX_FB_PLANES] = { };
	igt_framebuffer_t *fb;
	igt_vc4_fb_t *vc4fb;
	int i;

	if (!finfo ||
	    (modifier != LOCAL_DRM_FORMAT_MOD_NONE &&
	     modifier != LOCAL_BROADCOM_FORMAT_MOD_VC4_T_TILED))
		return NULL;

	vc4fb = calloc(1, sizeof(*vc4fb));
	if (!vc4fb)
		return NULL;

	for (i = 0; i < finfo->nplanes; i++) {
		size_t size;

		fbplanes[i].pitch = finfo->cpp[i] * width;
		size = fbplanes[i].pitch * height;
		fbplanes[i].bo = igt_vc4_new_bo(dev, size);
	}

	fb = igt_framebuffer_create(dev, width, height, format, modifier,
				    fbplanes, &vc4_fb_ops);
	fb->priv = vc4fb;

	return fb;
}
