/*
 * Copyright © 2013,2014 Intel Corporation
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
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>

#include "drmtest.h"
#include "igt_fb.h"
#include "igt_kms.h"
#include "ioctl_wrappers.h"
#include "intel_chipset.h"

/**
 * SECTION:igt_fb
 * @short_description: Framebuffer handling and drawing library
 * @title: Framebuffer
 * @include: igt.h
 *
 * This library contains helper functions for handling kms framebuffer objects
 * using #igt_fb structures to track all the metadata.  igt_create_fb() creates
 * a basic framebuffer and igt_remove_fb() cleans everything up again.
 *
 * It also supports drawing using the cairo library and provides some simplified
 * helper functions to easily draw test patterns. The main function to create a
 * cairo drawing context for a framebuffer object is igt_get_cairo_ctx().
 *
 * Finally it also pulls in the drm fourcc headers and provides some helper
 * functions to work with these pixel format codes.
 */

/* drm fourcc/cairo format maps */
#define DF(did, cid, _bpp, _depth)	\
	{ DRM_FORMAT_##did, CAIRO_FORMAT_##cid, # did, _bpp, _depth }
static struct format_desc_struct {
	uint32_t drm_id;
	cairo_format_t cairo_id;
	const char *name;
	int bpp;
	int depth;
} format_desc[] = {
	DF(RGB565,	RGB16_565,	16, 16),
	//DF(RGB888,	INVALID,	24, 24),
	DF(XRGB8888,	RGB24,		32, 24),
	DF(XRGB2101010,	RGB30,		32, 30),
	DF(ARGB8888,	ARGB32,		32, 32),
};
#undef DF

#define for_each_format(f)	\
	for (f = format_desc; f - format_desc < ARRAY_SIZE(format_desc); f++)

static void igt_get_fb_tile_size(int fd, uint64_t tiling, int fb_bpp,
				 unsigned *width_ret, unsigned *height_ret)
{
	switch (tiling) {
	case LOCAL_DRM_FORMAT_MOD_NONE:
		*width_ret = 64;
		*height_ret = 1;
		break;
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		igt_require_intel(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 2) {
			*width_ret = 128;
			*height_ret = 16;
		} else {
			*width_ret = 512;
			*height_ret = 8;
		}
		break;
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		igt_require_intel(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 2) {
			*width_ret = 128;
			*height_ret = 16;
		} else if (IS_915(intel_get_drm_devid(fd))) {
			*width_ret = 512;
			*height_ret = 8;
		} else {
			*width_ret = 128;
			*height_ret = 32;
		}
		break;
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		igt_require_intel(fd);
		switch (fb_bpp) {
		case 8:
			*width_ret = 64;
			*height_ret = 64;
			break;
		case 16:
		case 32:
			*width_ret = 128;
			*height_ret = 32;
			break;
		case 64:
		case 128:
			*width_ret = 256;
			*height_ret = 16;
			break;
		default:
			igt_assert(false);
		}
		break;
	default:
		igt_assert(false);
	}
}

/**
 * igt_calc_fb_size:
 * @fd: the DRM file descriptor
 * @width: width of the framebuffer in pixels
 * @height: height of the framebuffer in pixels
 * @bpp: bytes per pixel of the framebuffer
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @size_ret: returned size for the framebuffer
 * @stride_ret: returned stride for the framebuffer
 *
 * This function returns valid stride and size values for a framebuffer with the
 * specified parameters.
 */
void igt_calc_fb_size(int fd, int width, int height, int bpp, uint64_t tiling,
		      unsigned *size_ret, unsigned *stride_ret)
{
	unsigned int tile_width, tile_height, stride, size;
	int byte_width = width * (bpp / 8);

	igt_get_fb_tile_size(fd, tiling, bpp, &tile_width, &tile_height);

	if (tiling != LOCAL_DRM_FORMAT_MOD_NONE &&
	    intel_gen(intel_get_drm_devid(fd)) <= 3) {
		int v;

		/* Round the tiling up to the next power-of-two and the region
		 * up to the next pot fence size so that this works on all
		 * generations.
		 *
		 * This can still fail if the framebuffer is too large to be
		 * tiled. But then that failure is expected.
		 */

		v = width * bpp / 8;
		for (stride = 512; stride < v; stride *= 2)
			;

		v = stride * height;
		for (size = 1024*1024; size < v; size *= 2)
			;
	} else {
		stride = ALIGN(byte_width, tile_width);
		size = stride * ALIGN(height, tile_height);
	}

	*stride_ret = stride;
	*size_ret = size;
}

/**
 * igt_fb_mod_to_tiling:
 * @modifier: DRM framebuffer modifier
 *
 * This function converts a DRM framebuffer modifier to its corresponding
 * tiling constant.
 *
 * Returns:
 * A tiling constant
 */
uint64_t igt_fb_mod_to_tiling(uint64_t modifier)
{
	switch (modifier) {
	case LOCAL_DRM_FORMAT_MOD_NONE:
		return I915_TILING_NONE;
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		return I915_TILING_X;
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		return I915_TILING_Y;
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		return I915_TILING_Yf;
	default:
		igt_assert(0);
	}
}

/* helpers to create nice-looking framebuffers */
static int create_bo_for_fb(int fd, int width, int height, uint32_t format,
			    uint64_t tiling, unsigned size, unsigned stride,
			    unsigned *size_ret, unsigned *stride_ret,
			    bool *is_dumb)
{
	int bpp = igt_drm_format_to_bpp(format);
	int bo;

	if (tiling || size || stride) {
		unsigned calculated_size, calculated_stride;

		igt_calc_fb_size(fd, width, height, bpp, tiling,
				 &calculated_size, &calculated_stride);
		if (stride == 0)
			stride = calculated_stride;
		if (size == 0)
			size = calculated_size;

		if (is_dumb)
			*is_dumb = false;

		if (is_i915_device(fd)) {
			uint32_t *ptr;

			bo = gem_create(fd, size);
			gem_set_tiling(fd, bo, igt_fb_mod_to_tiling(tiling), stride);

			/* Ensure the framebuffer is preallocated */
			ptr = gem_mmap__gtt(fd, bo, size, PROT_READ);
			igt_assert(*ptr == 0);
			gem_munmap(ptr, size);

			if (size_ret)
				*size_ret = size;

			if (stride_ret)
				*stride_ret = stride;

			return bo;
		} else {
			bool driver_has_gem_api = false;

			igt_require(driver_has_gem_api);
			return -EINVAL;
		}
	} else {
		if (is_dumb)
			*is_dumb = true;

		return kmstest_dumb_create(fd, width, height, bpp, stride_ret,
					   size_ret);
	}
}

/**
 * igt_create_bo_with_dimensions:
 * @fd: open drm file descriptor
 * @width: width of the buffer object in pixels
 * @height: height of the buffer object in pixels
 * @format: drm fourcc pixel format code
 * @modifier: modifier corresponding to the tiling layout of the buffer object
 * @stride: stride of the buffer object in bytes (0 for automatic stride)
 * @size_ret: size of the buffer object as created by the kernel
 * @stride_ret: stride of the buffer object as created by the kernel
 * @is_dumb: whether the created buffer object is a dumb buffer or not
 *
 * This function allocates a gem buffer object matching the requested
 * properties.
 *
 * Returns:
 * The kms id of the created buffer object.
 */
int igt_create_bo_with_dimensions(int fd, int width, int height,
				  uint32_t format, uint64_t modifier,
				  unsigned stride, unsigned *size_ret,
				  unsigned *stride_ret, bool *is_dumb)
{
	return create_bo_for_fb(fd, width, height, format, modifier, 0, stride,
			     size_ret, stride_ret, is_dumb);
}

/**
 * igt_create_fb_with_bo_size:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @fb: pointer to an #igt_fb structure
 * @bo_size: size of the backing bo (0 for automatic size)
 * @bo_stride: stride of the backing bo (0 for automatic stride)
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object of the requested size. All metadata is stored in @fb.
 *
 * The backing storage of the framebuffer is filled with all zeros, i.e. black
 * for rgb pixel formats.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int
igt_create_fb_with_bo_size(int fd, int width, int height,
			   uint32_t format, uint64_t tiling,
			   struct igt_fb *fb, unsigned bo_size,
			   unsigned bo_stride)
{
	uint32_t fb_id;

	memset(fb, 0, sizeof(*fb));

	igt_debug("%s(width=%d, height=%d, format=0x%x, tiling=0x%"PRIx64", size=%d)\n",
		  __func__, width, height, format, tiling, bo_size);
	fb->gem_handle = create_bo_for_fb(fd, width, height, format, tiling,
					  bo_size, bo_stride, &fb->size,
					  &fb->stride, &fb->is_dumb);
	igt_assert(fb->gem_handle > 0);

	igt_debug("%s(handle=%d, pitch=%d)\n",
		  __func__, fb->gem_handle, fb->stride);

	if (tiling != LOCAL_DRM_FORMAT_MOD_NONE &&
	    tiling != LOCAL_I915_FORMAT_MOD_X_TILED) {
		do_or_die(__kms_addfb(fd, fb->gem_handle, width, height,
				      fb->stride, format, tiling,
				      LOCAL_DRM_MODE_FB_MODIFIERS, &fb_id));
	} else {
		uint32_t handles[4];
		uint32_t pitches[4];
		uint32_t offsets[4];

		memset(handles, 0, sizeof(handles));
		memset(pitches, 0, sizeof(pitches));
		memset(offsets, 0, sizeof(offsets));

		handles[0] = fb->gem_handle;
		pitches[0] = fb->stride;

		do_or_die(drmModeAddFB2(fd, width, height, format,
					handles, pitches, offsets,
					&fb_id, 0));
	}

	fb->width = width;
	fb->height = height;
	fb->tiling = tiling;
	fb->drm_format = format;
	fb->fb_id = fb_id;
	fb->fd = fd;

	return fb_id;
}

/**
 * igt_create_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * The backing storage of the framebuffer is filled with all zeros, i.e. black
 * for rgb pixel formats.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int igt_create_fb(int fd, int width, int height, uint32_t format,
			   uint64_t tiling, struct igt_fb *fb)
{
	return igt_create_fb_with_bo_size(fd, width, height, format, tiling, fb,
					  0, 0);
}

/**
 * igt_create_color_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also fills the entire framebuffer
 * with the given color, which is useful for some simple pipe crc based tests.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_color_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 double r, double g, double b,
				 struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_color(cr, 0, 0, width, height, r, g, b);
	igt_assert(cairo_status(cr) == 0);
	cairo_destroy(cr);

	return fb_id;
}

/**
 * igt_create_pattern_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also draws the standard test pattern
 * into the framebuffer.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_pattern_fb(int fd, int width, int height,
				   uint32_t format, uint64_t tiling,
				   struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_test_pattern(cr, width, height);
	igt_assert(cairo_status(cr) == 0);
	cairo_destroy(cr);

	return fb_id;
}

/**
 * igt_create_color_pattern_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also fills the entire framebuffer
 * with the given color, and then draws the standard test pattern into the
 * framebuffer.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_color_pattern_fb(int fd, int width, int height,
					 uint32_t format, uint64_t tiling,
					 double r, double g, double b,
					 struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_color(cr, 0, 0, width, height, r, g, b);
	igt_paint_test_pattern(cr, width, height);
	igt_assert(cairo_status(cr) == 0);
	cairo_destroy(cr);

	return fb_id;
}

/**
 * igt_create_image_fb:
 * @drm_fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel or 0
 * @height: height of the framebuffer in pixel or 0
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @filename: filename of the png image to draw
 * @fb: pointer to an #igt_fb structure
 *
 * Create a framebuffer with the specified image. If @width is zero the
 * image width will be used. If @height is zero the image height will be used.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_image_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 const char *filename,
				 struct igt_fb *fb /* out */)
{
	cairo_surface_t *image;
	uint32_t fb_id;
	cairo_t *cr;

	image = cairo_image_surface_create_from_png(filename);
	igt_assert(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
	if (width == 0)
		width = cairo_image_surface_get_width(image);
	if (height == 0)
		height = cairo_image_surface_get_height(image);
	cairo_surface_destroy(image);

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_image(cr, filename, 0, 0, width, height);
	igt_assert(cairo_status(cr) == 0);
	cairo_destroy(cr);

	return fb_id;
}

struct box {
	int x, y, width, height;
};

struct stereo_fb_layout {
	int fb_width, fb_height;
	struct box left, right;
};

static void box_init(struct box *box, int x, int y, int bwidth, int bheight)
{
	box->x = x;
	box->y = y;
	box->width = bwidth;
	box->height = bheight;
}


static void stereo_fb_layout_from_mode(struct stereo_fb_layout *layout,
				       drmModeModeInfo *mode)
{
	unsigned int format = mode->flags & DRM_MODE_FLAG_3D_MASK;
	const int hdisplay = mode->hdisplay, vdisplay = mode->vdisplay;
	int middle;

	switch (format) {
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
		layout->fb_width = hdisplay;
		layout->fb_height = vdisplay;

		middle = vdisplay / 2;
		box_init(&layout->left, 0, 0, hdisplay, middle);
		box_init(&layout->right,
			 0, middle, hdisplay, vdisplay - middle);
		break;
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
		layout->fb_width = hdisplay;
		layout->fb_height = vdisplay;

		middle = hdisplay / 2;
		box_init(&layout->left, 0, 0, middle, vdisplay);
		box_init(&layout->right,
			 middle, 0, hdisplay - middle, vdisplay);
		break;
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
	{
		int vactive_space = mode->vtotal - vdisplay;

		layout->fb_width = hdisplay;
		layout->fb_height = 2 * vdisplay + vactive_space;

		box_init(&layout->left,
			 0, 0, hdisplay, vdisplay);
		box_init(&layout->right,
			 0, vdisplay + vactive_space, hdisplay, vdisplay);
		break;
	}
	default:
		igt_assert(0);
	}
}

/**
 * igt_create_stereo_fb:
 * @drm_fd: open i915 drm file descriptor
 * @mode: A stereo 3D mode.
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 *
 * Create a framebuffer for use with the stereo 3D mode specified by @mode.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_stereo_fb(int drm_fd, drmModeModeInfo *mode,
				  uint32_t format, uint64_t tiling)
{
	struct stereo_fb_layout layout;
	cairo_t *cr;
	uint32_t fb_id;
	struct igt_fb fb;

	stereo_fb_layout_from_mode(&layout, mode);
	fb_id = igt_create_fb(drm_fd, layout.fb_width, layout.fb_height, format,
			      tiling, &fb);
	cr = igt_get_cairo_ctx(drm_fd, &fb);

	igt_paint_image(cr, "1080p-left.png",
			layout.left.x, layout.left.y,
			layout.left.width, layout.left.height);
	igt_paint_image(cr, "1080p-right.png",
			layout.right.x, layout.right.y,
			layout.right.width, layout.right.height);

	cairo_destroy(cr);

	return fb_id;
}

static cairo_format_t drm_format_to_cairo(uint32_t drm_format)
{
	struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return f->cairo_id;

	igt_assert_f(0, "can't find a cairo format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));
}

struct fb_blit_upload {
	int fd;
	struct igt_fb *fb;
	struct {
		uint32_t handle;
		unsigned size, stride;
		uint8_t *map;
		bool is_dumb;
	} linear;
};

static void destroy_cairo_surface__blit(void *arg)
{
	struct fb_blit_upload *blit = arg;
	struct igt_fb *fb = blit->fb;
	unsigned int obj_tiling = igt_fb_mod_to_tiling(fb->tiling);

	gem_munmap(blit->linear.map, blit->linear.size);
	fb->cairo_surface = NULL;

	gem_set_domain(blit->fd, blit->linear.handle,
			I915_GEM_DOMAIN_GTT, 0);

	igt_blitter_fast_copy__raw(blit->fd,
				   blit->linear.handle,
				   blit->linear.stride,
				   I915_TILING_NONE,
				   0, 0, /* src_x, src_y */
				   fb->width, fb->height,
				   fb->gem_handle,
				   fb->stride,
				   obj_tiling,
				   0, 0 /* dst_x, dst_y */);

	gem_sync(blit->fd, blit->linear.handle);
	gem_close(blit->fd, blit->linear.handle);

	free(blit);
}

static void create_cairo_surface__blit(int fd, struct igt_fb *fb)
{
	struct fb_blit_upload *blit;
	cairo_format_t cairo_format;
	unsigned int obj_tiling = igt_fb_mod_to_tiling(fb->tiling);

	blit = malloc(sizeof(*blit));
	igt_assert(blit);

	/*
	 * We create a linear BO that we'll map for the CPU to write to (using
	 * cairo). This linear bo will be then blitted to its final
	 * destination, tiling it at the same time.
	 */
	blit->linear.handle = create_bo_for_fb(fd, fb->width, fb->height,
					       fb->drm_format,
					       LOCAL_DRM_FORMAT_MOD_NONE, 0,
					       0, &blit->linear.size,
					       &blit->linear.stride,
					       &blit->linear.is_dumb);

	igt_assert(blit->linear.handle > 0);

	blit->fd = fd;
	blit->fb = fb;

	/* Copy fb content to linear BO */
	gem_set_domain(fd, blit->linear.handle,
			I915_GEM_DOMAIN_GTT, 0);

	igt_blitter_fast_copy__raw(fd,
				   fb->gem_handle,
				   fb->stride,
				   obj_tiling,
				   0, 0, /* src_x, src_y */
				   fb->width, fb->height,
				   blit->linear.handle,
				   blit->linear.stride,
				   I915_TILING_NONE,
				   0, 0 /* dst_x, dst_y */);

	gem_sync(fd, blit->linear.handle);

	gem_set_domain(fd, blit->linear.handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	/* Setup cairo context */
	blit->linear.map = gem_mmap__cpu(fd,
					 blit->linear.handle,
					 0,
					 blit->linear.size,
					 PROT_READ | PROT_WRITE);

	cairo_format = drm_format_to_cairo(fb->drm_format);
	fb->cairo_surface =
		cairo_image_surface_create_for_data(blit->linear.map,
						    cairo_format,
						    fb->width, fb->height,
						    blit->linear.stride);
	fb->domain = I915_GEM_DOMAIN_GTT;

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__blit,
				    blit, destroy_cairo_surface__blit);
}

/**
 * igt_dirty_fb:
 * @fd: open drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * Flushes out the whole framebuffer.
 *
 * Returns: 0 upon success.
 */
int igt_dirty_fb(int fd, struct igt_fb *fb)
{
	return drmModeDirtyFB(fb->fd, fb->fb_id, NULL, 0);
}

static void destroy_cairo_surface__gtt(void *arg)
{
	struct igt_fb *fb = arg;

	gem_munmap(cairo_image_surface_get_data(fb->cairo_surface), fb->size);
	fb->cairo_surface = NULL;

	if (fb->is_dumb)
		igt_dirty_fb(fb->fd, fb);
}

static void create_cairo_surface__gtt(int fd, struct igt_fb *fb)
{
	void *ptr;

	if (fb->is_dumb)
		ptr = kmstest_dumb_map_buffer(fd, fb->gem_handle, fb->size,
					      PROT_READ | PROT_WRITE);
	else
		ptr = gem_mmap__gtt(fd, fb->gem_handle, fb->size,
				    PROT_READ | PROT_WRITE);

	fb->cairo_surface =
		cairo_image_surface_create_for_data(ptr,
						    drm_format_to_cairo(fb->drm_format),
						    fb->width, fb->height, fb->stride);
	fb->domain = I915_GEM_DOMAIN_GTT;

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__gtt,
				    fb, destroy_cairo_surface__gtt);
}

static cairo_surface_t *get_cairo_surface(int fd, struct igt_fb *fb)
{
	if (fb->cairo_surface == NULL) {
		if (fb->tiling == LOCAL_I915_FORMAT_MOD_Y_TILED ||
		    fb->tiling == LOCAL_I915_FORMAT_MOD_Yf_TILED)
			create_cairo_surface__blit(fd, fb);
		else
			create_cairo_surface__gtt(fd, fb);
	}

	if (!fb->is_dumb)
		gem_set_domain(fd, fb->gem_handle, I915_GEM_DOMAIN_CPU,
			       I915_GEM_DOMAIN_CPU);

	igt_assert(cairo_surface_status(fb->cairo_surface) == CAIRO_STATUS_SUCCESS);
	return fb->cairo_surface;
}

/**
 * igt_get_cairo_ctx:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This initializes a cairo surface for @fb and then allocates a drawing context
 * for it. The return cairo drawing context should be released by calling
 * cairo_destroy(). This also sets a default font for drawing text on
 * framebuffers.
 *
 * Returns:
 * The created cairo drawing context.
 */
cairo_t *igt_get_cairo_ctx(int fd, struct igt_fb *fb)
{
	cairo_surface_t *surface;
	cairo_t *cr;

	surface = get_cairo_surface(fd, fb);
	cr = cairo_create(surface);
	cairo_surface_destroy(surface);
	igt_assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	cairo_select_font_face(cr, "Helvetica", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	igt_assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	return cr;
}

/**
 * igt_write_fb_to_png:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 * @filename: target name for the png image
 *
 * This function stores the contents of the supplied framebuffer into a png
 * image stored at @filename.
 */
void igt_write_fb_to_png(int fd, struct igt_fb *fb, const char *filename)
{
	cairo_surface_t *surface;
	cairo_status_t status;

	surface = get_cairo_surface(fd, fb);
	status = cairo_surface_write_to_png(surface, filename);
	cairo_surface_destroy(surface);

	igt_assert(status == CAIRO_STATUS_SUCCESS);
}

/**
 * igt_remove_fb:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This function releases all resources allocated in igt_create_fb() for @fb.
 * Note that if this framebuffer is still in use on a primary plane the kernel
 * will disable the corresponding crtc.
 */
void igt_remove_fb(int fd, struct igt_fb *fb)
{
	cairo_surface_destroy(fb->cairo_surface);
	do_or_die(drmModeRmFB(fd, fb->fb_id));
	gem_close(fd, fb->gem_handle);
}

/**
 * igt_bpp_depth_to_drm_format:
 * @bpp: desired bits per pixel
 * @depth: desired depth
 *
 * Returns:
 * The rgb drm fourcc pixel format code corresponding to the given @bpp and
 * @depth values.  Fails hard if no match was found.
 */
uint32_t igt_bpp_depth_to_drm_format(int bpp, int depth)
{
	struct format_desc_struct *f;

	for_each_format(f)
		if (f->bpp == bpp && f->depth == depth)
			return f->drm_id;


	igt_assert_f(0, "can't find drm format with bpp=%d, depth=%d\n", bpp,
		     depth);
}

/**
 * igt_drm_format_to_bpp:
 * @drm_format: drm fourcc pixel format code
 *
 * Returns:
 * The bits per pixel for the given drm fourcc pixel format code. Fails hard if
 * no match was found.
 */
uint32_t igt_drm_format_to_bpp(uint32_t drm_format)
{
	struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return f->bpp;

	igt_assert_f(0, "can't find a bpp format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));
}

/**
 * igt_format_str:
 * @drm_format: drm fourcc pixel format code
 *
 * Returns:
 * Human-readable fourcc pixel format code for @drm_format or "invalid" no match
 * was found.
 */
const char *igt_format_str(uint32_t drm_format)
{
	struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return f->name;

	return "invalid";
}

/**
 * igt_get_all_cairo_formats:
 * @formats: pointer to pointer to store the allocated formats array
 * @format_count: pointer to integer to store the size of the allocated array
 *
 * This functions returns an array of all the drm fourcc codes supported by
 * cairo and this library.
 */
void igt_get_all_cairo_formats(const uint32_t **formats, int *format_count)
{
	static uint32_t *drm_formats;
	static int n_formats;

	if (!drm_formats) {
		struct format_desc_struct *f;
		uint32_t *format;

		n_formats = 0;
		for_each_format(f)
			if (f->cairo_id != CAIRO_FORMAT_INVALID)
				n_formats++;

		drm_formats = calloc(n_formats, sizeof(*drm_formats));
		format = &drm_formats[0];
		for_each_format(f)
			if (f->cairo_id != CAIRO_FORMAT_INVALID)
				*format++ = f->drm_id;
	}

	*formats = drm_formats;
	*format_count = n_formats;
}
