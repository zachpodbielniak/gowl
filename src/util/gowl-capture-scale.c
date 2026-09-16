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

#include "gowl-capture-scale.h"

#include <math.h>

gdouble
gowl_capture_scale_factor(
	gint    layout_w,
	gint    layout_h,
	gint    image_w,
	gint    image_h
){
	gdouble sx, sy;

	if (layout_w <= 0 || layout_h <= 0 || image_w <= 0 || image_h <= 0)
		return 1.0;

	sx = (gdouble)image_w / (gdouble)layout_w;
	sy = (gdouble)image_h / (gdouble)layout_h;

	/* The two axes agree on every unrotated output.  Taking the
	 * larger keeps a mismatch from silently shrinking the capture. */
	return sx > sy ? sx : sy;
}

gboolean
gowl_capture_scale_crop(
	gint    mon_x,
	gint    mon_y,
	gint    mon_w,
	gint    mon_h,
	gint    image_w,
	gint    image_h,
	gint    rx,
	gint    ry,
	gint    rw,
	gint    rh,
	gint   *crop_x,
	gint   *crop_y,
	gint   *crop_w,
	gint   *crop_h
){
	gdouble sx, sy;
	gint    lx0, ly0, lx1, ly1;
	gint    x0, y0, x1, y1;

	if (crop_x != NULL) *crop_x = 0;
	if (crop_y != NULL) *crop_y = 0;
	if (crop_w != NULL) *crop_w = 0;
	if (crop_h != NULL) *crop_h = 0;

	if (mon_w <= 0 || mon_h <= 0 || image_w <= 0 || image_h <= 0)
		return FALSE;
	if (rw <= 0 || rh <= 0)
		return FALSE;

	/* Step one: out of layout coordinates and into this monitor's
	 * own, which is where the region stops being about the desktop
	 * and starts being about one screen. */
	lx0 = rx - mon_x;
	ly0 = ry - mon_y;
	lx1 = lx0 + rw;
	ly1 = ly0 + rh;

	/* Step two: clamp while still logical, so a selection dragged off
	 * the edge of the screen keeps the part that was on it. */
	if (lx0 < 0)     lx0 = 0;
	if (ly0 < 0)     ly0 = 0;
	if (lx1 > mon_w) lx1 = mon_w;
	if (ly1 > mon_h) ly1 = mon_h;

	if (lx1 <= lx0 || ly1 <= ly0)
		return FALSE;

	/* Step three: into device pixels.  Each edge moves outwards, so
	 * the crop covers every pixel the selection touched. */
	sx = (gdouble)image_w / (gdouble)mon_w;
	sy = (gdouble)image_h / (gdouble)mon_h;

	x0 = (gint)floor((gdouble)lx0 * sx);
	y0 = (gint)floor((gdouble)ly0 * sy);
	x1 = (gint)ceil ((gdouble)lx1 * sx);
	y1 = (gint)ceil ((gdouble)ly1 * sy);

	/* Rounding outwards can step past the last pixel; the image is
	 * the final word on how big the image is. */
	if (x0 < 0)       x0 = 0;
	if (y0 < 0)       y0 = 0;
	if (x1 > image_w) x1 = image_w;
	if (y1 > image_h) y1 = image_h;

	if (x1 <= x0 || y1 <= y0)
		return FALSE;

	if (crop_x != NULL) *crop_x = x0;
	if (crop_y != NULL) *crop_y = y0;
	if (crop_w != NULL) *crop_w = x1 - x0;
	if (crop_h != NULL) *crop_h = y1 - y0;

	return TRUE;
}

gboolean
gowl_capture_scale_place(
	gint     mon_x,
	gint     mon_y,
	gint     mon_w,
	gint     mon_h,
	gint     origin_x,
	gint     origin_y,
	gdouble  scale,
	gint    *dest_x,
	gint    *dest_y,
	gint    *dest_w,
	gint    *dest_h
){
	gint x0, y0, x1, y1;

	if (dest_x != NULL) *dest_x = 0;
	if (dest_y != NULL) *dest_y = 0;
	if (dest_w != NULL) *dest_w = 0;
	if (dest_h != NULL) *dest_h = 0;

	if (mon_w <= 0 || mon_h <= 0 || scale <= 0.0)
		return FALSE;

	/* Both edges go through the same rounding, so abutting monitors
	 * abut on the canvas too: no seam, no overlap. */
	x0 = (gint)lround((gdouble)(mon_x - origin_x) * scale);
	y0 = (gint)lround((gdouble)(mon_y - origin_y) * scale);
	x1 = (gint)lround((gdouble)(mon_x - origin_x + mon_w) * scale);
	y1 = (gint)lround((gdouble)(mon_y - origin_y + mon_h) * scale);

	if (x1 <= x0 || y1 <= y0)
		return FALSE;

	if (dest_x != NULL) *dest_x = x0;
	if (dest_y != NULL) *dest_y = y0;
	if (dest_w != NULL) *dest_w = x1 - x0;
	if (dest_h != NULL) *dest_h = y1 - y0;

	return TRUE;
}
