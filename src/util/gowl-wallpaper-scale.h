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

#ifndef GOWL_WALLPAPER_SCALE_H
#define GOWL_WALLPAPER_SCALE_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Pure integer geometry for desktop-wallpaper scaling.  No gdk-pixbuf
 * or wlroots types, so the maths can be unit-tested directly (see
 * tests/test-wallpaper.c).  All functions are robust to non-positive
 * inputs: a zero or negative image/monitor dimension yields a safe,
 * crash-free zero/identity result.
 */

/**
 * gowl_wallpaper_cover_rect:
 * @iw: image width
 * @ih: image height
 * @mw: monitor width
 * @mh: monitor height
 * @scaled_w: (out): width the image is scaled to (>= @mw)
 * @scaled_h: (out): height the image is scaled to (>= @mh)
 * @crop_x: (out): left offset of the centered @mw x @mh crop window
 * @crop_y: (out): top offset of the centered @mw x @mh crop window
 *
 * "fill" / GNOME "zoom": scale the image (up or down) to fully cover
 * the monitor while preserving aspect ratio, then center-crop the
 * overflow.  Never stretches: the scaled aspect equals the source
 * aspect, and the @mw x @mh crop always lies within the scaled image.
 */
void gowl_wallpaper_cover_rect(gint iw, gint ih, gint mw, gint mh,
                               gint *scaled_w, gint *scaled_h,
                               gint *crop_x, gint *crop_y);

/**
 * gowl_wallpaper_fit_rect:
 * @iw: image width
 * @ih: image height
 * @mw: monitor width
 * @mh: monitor height
 * @scaled_w: (out): width the image is scaled to (<= @mw)
 * @scaled_h: (out): height the image is scaled to (<= @mh)
 * @off_x: (out): left offset to center the scaled image on the monitor
 * @off_y: (out): top offset to center the scaled image on the monitor
 *
 * "fit": scale the image to fit entirely within the monitor preserving
 * aspect ratio, centered, with letterbox padding around it.
 */
void gowl_wallpaper_fit_rect(gint iw, gint ih, gint mw, gint mh,
                             gint *scaled_w, gint *scaled_h,
                             gint *off_x, gint *off_y);

/**
 * gowl_wallpaper_center_rect:
 * @iw: image width
 * @ih: image height
 * @mw: monitor width
 * @mh: monitor height
 * @src_x: (out): left offset into the source to start copying from
 * @src_y: (out): top offset into the source to start copying from
 * @dst_x: (out): left offset on the monitor canvas to copy to
 * @dst_y: (out): top offset on the monitor canvas to copy to
 * @copy_w: (out): width of the copied region
 * @copy_h: (out): height of the copied region
 *
 * "center": place the image 1:1 (no scaling) at the monitor centre,
 * cropping it on axes where it is larger than the monitor and padding
 * on axes where it is smaller.
 */
void gowl_wallpaper_center_rect(gint iw, gint ih, gint mw, gint mh,
                                gint *src_x, gint *src_y,
                                gint *dst_x, gint *dst_y,
                                gint *copy_w, gint *copy_h);

/**
 * gowl_wallpaper_tile_count:
 * @iw: image width
 * @ih: image height
 * @mw: monitor width
 * @mh: monitor height
 * @cols: (out): number of tile columns needed to cover the monitor
 * @rows: (out): number of tile rows needed to cover the monitor
 *
 * "tile": number of 1:1 image repetitions across and down needed to
 * fill the monitor (ceil division).  Zero columns/rows for a
 * degenerate image.
 */
void gowl_wallpaper_tile_count(gint iw, gint ih, gint mw, gint mh,
                               gint *cols, gint *rows);

G_END_DECLS

#endif /* GOWL_WALLPAPER_SCALE_H */
