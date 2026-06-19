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

#include "util/gowl-wallpaper-scale.h"

void
gowl_wallpaper_cover_rect(gint iw, gint ih, gint mw, gint mh,
                          gint *scaled_w, gint *scaled_h,
                          gint *crop_x, gint *crop_y)
{
	gdouble sx, sy, s;
	gint sw, sh;

	if (iw <= 0 || ih <= 0 || mw <= 0 || mh <= 0) {
		*scaled_w = (mw > 0) ? mw : 0;
		*scaled_h = (mh > 0) ? mh : 0;
		*crop_x   = 0;
		*crop_y   = 0;
		return;
	}

	sx = (gdouble)mw / (gdouble)iw;
	sy = (gdouble)mh / (gdouble)ih;
	s  = (sx > sy) ? sx : sy;          /* cover: take the larger scale */

	sw = (gint)(iw * s + 0.5);
	sh = (gint)(ih * s + 0.5);

	/* Cover guarantees sw >= mw and sh >= mh in exact arithmetic;
	 * clamp against floating-point rounding so the crop never goes
	 * negative. */
	if (sw < mw)
		sw = mw;
	if (sh < mh)
		sh = mh;

	*scaled_w = sw;
	*scaled_h = sh;
	*crop_x   = (sw - mw) / 2;
	*crop_y   = (sh - mh) / 2;
}

void
gowl_wallpaper_fit_rect(gint iw, gint ih, gint mw, gint mh,
                        gint *scaled_w, gint *scaled_h,
                        gint *off_x, gint *off_y)
{
	gdouble sx, sy, s;
	gint sw, sh;

	if (iw <= 0 || ih <= 0 || mw <= 0 || mh <= 0) {
		*scaled_w = (mw > 0) ? mw : 0;
		*scaled_h = (mh > 0) ? mh : 0;
		*off_x    = 0;
		*off_y    = 0;
		return;
	}

	sx = (gdouble)mw / (gdouble)iw;
	sy = (gdouble)mh / (gdouble)ih;
	s  = (sx < sy) ? sx : sy;          /* fit: take the smaller scale */

	sw = (gint)(iw * s + 0.5);
	sh = (gint)(ih * s + 0.5);

	/* Contain guarantees sw <= mw and sh <= mh; clamp against rounding. */
	if (sw > mw)
		sw = mw;
	if (sh > mh)
		sh = mh;

	*scaled_w = sw;
	*scaled_h = sh;
	*off_x    = (mw - sw) / 2;
	*off_y    = (mh - sh) / 2;
}

void
gowl_wallpaper_center_rect(gint iw, gint ih, gint mw, gint mh,
                           gint *src_x, gint *src_y,
                           gint *dst_x, gint *dst_y,
                           gint *copy_w, gint *copy_h)
{
	if (iw <= 0 || ih <= 0 || mw <= 0 || mh <= 0) {
		*src_x  = 0;
		*src_y  = 0;
		*dst_x  = 0;
		*dst_y  = 0;
		*copy_w = 0;
		*copy_h = 0;
		return;
	}

	if (iw > mw) {
		*src_x  = (iw - mw) / 2;
		*dst_x  = 0;
		*copy_w = mw;
	} else {
		*src_x  = 0;
		*dst_x  = (mw - iw) / 2;
		*copy_w = iw;
	}

	if (ih > mh) {
		*src_y  = (ih - mh) / 2;
		*dst_y  = 0;
		*copy_h = mh;
	} else {
		*src_y  = 0;
		*dst_y  = (mh - ih) / 2;
		*copy_h = ih;
	}
}

void
gowl_wallpaper_tile_count(gint iw, gint ih, gint mw, gint mh,
                          gint *cols, gint *rows)
{
	if (iw <= 0 || ih <= 0 || mw <= 0 || mh <= 0) {
		*cols = 0;
		*rows = 0;
		return;
	}

	*cols = (mw + iw - 1) / iw;        /* ceil(mw / iw) */
	*rows = (mh + ih - 1) / ih;        /* ceil(mh / ih) */
}
