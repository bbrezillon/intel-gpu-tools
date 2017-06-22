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

#ifndef __IGT_CAIRO_H__
#define __IGT_CAIRO_H__

/* cairo is assumed available on linux. On Android we check for ANDROID_HAS_CAIRO */
#if (!defined(ANDROID)) || (defined(ANDROID) && ANDROID_HAS_CAIRO)
#include <cairo.h>
#else
typedef struct _cairo_surface cairo_surface_t;
typedef struct _cairo cairo_t;
#endif

#include <stddef.h>
#include <stdbool.h>

/**
 * igt_text_align:
 * @align_left: align left
 * @align_right: align right
 * @align_bottom: align bottom
 * @align_top: align top
 * @align_vcenter: align vcenter
 * @align_hcenter: align hcenter
 *
 * Alignment mode for text drawing using igt_cairo_printf_line().
 */
enum igt_text_align {
	align_left,
	align_bottom	= align_left,
	align_right	= 0x01,
	align_top	= 0x02,
	align_vcenter	= 0x04,
	align_hcenter	= 0x08,
};

void igt_paint_color(cairo_t *cr, int x, int y, int w, int h,
		     double r, double g, double b);
void igt_paint_color_alpha(cairo_t *cr, int x, int y, int w, int h,
			   double r, double g, double b, double a);
void igt_paint_color_gradient(cairo_t *cr, int x, int y, int w, int h,
			      int r, int g, int b);
void igt_paint_color_gradient_range(cairo_t *cr, int x, int y, int w, int h,
				    double sr, double sg, double sb,
				    double er, double eg, double eb);
void igt_paint_test_pattern(cairo_t *cr, int width, int height);
void igt_paint_image(cairo_t *cr, const char *filename, int dst_x, int dst_y,
		     int dst_width, int dst_height);
int igt_cairo_printf_line(cairo_t *cr, enum igt_text_align align,
			  double yspacing, const char *fmt, ...)
			       __attribute__((format (printf, 4, 5)));

#endif /* __IGT_CAIRO_H__ */
