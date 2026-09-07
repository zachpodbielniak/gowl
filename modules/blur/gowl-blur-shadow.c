/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "gowl-blur-shadow.h"

#include <math.h>

static gdouble
smoothstep(gdouble edge0, gdouble edge1, gdouble x)
{
	gdouble t;

	if (edge1 <= edge0)
		return x < edge0 ? 0.0 : 1.0;
	t = CLAMP((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

gdouble
gowl_blur_shadow_alpha(gdouble x, gdouble y,
                        gdouble rect_x, gdouble rect_y,
                        gdouble rect_w, gdouble rect_h,
                        gdouble radius, gdouble corner)
{
	gdouble half_w = rect_w * 0.5;
	gdouble half_h = rect_h * 0.5;
	gdouble cx = rect_x + half_w;
	gdouble cy = rect_y + half_h;
	gdouble r  = CLAMP(corner, 0.0, MIN(half_w, half_h));
	gdouble qx, qy, distance;

	if (rect_w <= 0.0 || rect_h <= 0.0)
		return 0.0;

	/*
	 * Signed distance to a rounded rectangle: positive outside, negative
	 * within.  Doing it this way rather than as two independent 1-D
	 * profiles is what makes the corners round rather than square with
	 * dark spots where the two falloffs overlap.
	 */
	qx = fabs(x - cx) - (half_w - r);
	qy = fabs(y - cy) - (half_h - r);
	distance = sqrt(MAX(qx, 0.0) * MAX(qx, 0.0) + MAX(qy, 0.0) * MAX(qy, 0.0))
	           + MIN(MAX(qx, qy), 0.0) - r;

	if (radius <= 0.0)
		return distance <= 0.0 ? 1.0 : 0.0;

	/* Full inside, gone by one radius out, smooth in between. */
	return 1.0 - smoothstep(-radius * 0.35, radius, distance);
}

guint8 *
gowl_blur_shadow_render(gint           width,
                         gint           height,
                         gdouble        rect_x,
                         gdouble        rect_y,
                         gdouble        rect_w,
                         gdouble        rect_h,
                         gdouble        radius,
                         gdouble        corner,
                         gdouble        opacity,
                         const gdouble *rgb)
{
	guint8 *pixels;
	gint    x, y;
	gdouble r = rgb != NULL ? CLAMP(rgb[0], 0.0, 1.0) : 0.0;
	gdouble g = rgb != NULL ? CLAMP(rgb[1], 0.0, 1.0) : 0.0;
	gdouble b = rgb != NULL ? CLAMP(rgb[2], 0.0, 1.0) : 0.0;

	if (width <= 0 || height <= 0)
		return NULL;

	opacity = CLAMP(opacity, 0.0, 1.0);
	pixels = g_malloc0((gsize)width * height * 4);

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			guint8 *p = pixels + ((gsize)y * width + x) * 4;
			gdouble a = gowl_blur_shadow_alpha((gdouble)x + 0.5,
			                                   (gdouble)y + 0.5,
			                                   rect_x, rect_y,
			                                   rect_w, rect_h,
			                                   radius, corner) * opacity;

			/*
			 * ARGB8888 little-endian is B, G, R, A in memory, and the
			 * colour channels are PREMULTIPLIED by the alpha.  wlroots
			 * composites premultiplied; storing straight colour here
			 * gives every shadow a bright halo along its soft edge,
			 * which looks like a rendering bug and is one.
			 */
			p[0] = (guint8)(b * a * 255.0 + 0.5);
			p[1] = (guint8)(g * a * 255.0 + 0.5);
			p[2] = (guint8)(r * a * 255.0 + 0.5);
			p[3] = (guint8)(a * 255.0 + 0.5);
		}
	}
	return pixels;
}
