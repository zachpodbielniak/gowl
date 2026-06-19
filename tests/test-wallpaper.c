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

/* Unit tests for the pure wallpaper-scaling geometry. */

#include "util/gowl-wallpaper-scale.h"
#include <glib.h>
#include <math.h>

/* ---- fill / zoom (cover) ---------------------------------------- */

/* Assert the invariants of a cover layout: it fully covers the
 * monitor, the crop window lies inside the scaled image, and the
 * scaled aspect ratio equals the source aspect ratio (no stretch). */
static void
check_cover_invariants(gint iw, gint ih, gint mw, gint mh)
{
	gint sw, sh, cx, cy;
	gdouble src_aspect, scl_aspect;

	gowl_wallpaper_cover_rect(iw, ih, mw, mh, &sw, &sh, &cx, &cy);

	/* Fully covers the monitor. */
	g_assert_cmpint(sw, >=, mw);
	g_assert_cmpint(sh, >=, mh);

	/* Centered crop window lies within the scaled image. */
	g_assert_cmpint(cx, >=, 0);
	g_assert_cmpint(cy, >=, 0);
	g_assert_cmpint(cx + mw, <=, sw);
	g_assert_cmpint(cy + mh, <=, sh);

	/* No stretch: scaled aspect matches the source aspect within 1%. */
	src_aspect = (gdouble)iw / (gdouble)ih;
	scl_aspect = (gdouble)sw / (gdouble)sh;
	g_assert_cmpfloat(fabs(src_aspect - scl_aspect), <, 0.01);
}

static void
test_cover_invariants(void)
{
	check_cover_invariants(5760, 3240, 1920, 1080); /* same 16:9, down  */
	check_cover_invariants(5760, 3240, 3440, 1440); /* ultrawide 21:9   */
	check_cover_invariants(1920, 1080, 3840, 2160); /* smaller -> up    */
	check_cover_invariants(1080, 1920, 1920, 1080); /* portrait image   */
	check_cover_invariants(1920, 1080, 1080, 1920); /* portrait monitor */
	check_cover_invariants(1000, 1000, 1920, 1080); /* square on wide   */
	check_cover_invariants(1920, 1080, 1920, 1080); /* exact match      */
	check_cover_invariants(1, 1, 3840, 2160);       /* 1x1 on 4K        */
	check_cover_invariants(8000, 100, 1920, 1080);  /* extreme aspect   */
}

static void
test_cover_same_aspect_no_crop(void)
{
	gint sw, sh, cx, cy;

	/* 16:9 image into 16:9 monitor: whole image, no crop. */
	gowl_wallpaper_cover_rect(5760, 3240, 1920, 1080, &sw, &sh, &cx, &cy);
	g_assert_cmpint(cx, ==, 0);
	g_assert_cmpint(cy, ==, 0);
	g_assert_cmpint(sw, ==, 1920);
	g_assert_cmpint(sh, ==, 1080);
}

static void
test_cover_scales_up(void)
{
	gint sw, sh, cx, cy;

	/* Image smaller than the monitor must scale UP to cover (the GNOME
	 * "zoom" behaviour the user wants), never tile. */
	gowl_wallpaper_cover_rect(1920, 1080, 3840, 2160, &sw, &sh, &cx, &cy);
	g_assert_cmpint(sw, ==, 3840);
	g_assert_cmpint(sh, ==, 2160);
	g_assert_cmpint(cx, ==, 0);
	g_assert_cmpint(cy, ==, 0);
}

/* ---- fit (contain / letterbox) ---------------------------------- */

static void
check_fit_invariants(gint iw, gint ih, gint mw, gint mh)
{
	gint sw, sh, ox, oy;
	gdouble src_aspect, scl_aspect;

	gowl_wallpaper_fit_rect(iw, ih, mw, mh, &sw, &sh, &ox, &oy);

	/* Fits entirely within the monitor. */
	g_assert_cmpint(sw, <=, mw);
	g_assert_cmpint(sh, <=, mh);

	/* Centered, non-negative offsets. */
	g_assert_cmpint(ox, >=, 0);
	g_assert_cmpint(oy, >=, 0);
	g_assert_cmpint(ox, ==, (mw - sw) / 2);
	g_assert_cmpint(oy, ==, (mh - sh) / 2);

	/* No stretch. */
	src_aspect = (gdouble)iw / (gdouble)ih;
	scl_aspect = (gdouble)sw / (gdouble)sh;
	g_assert_cmpfloat(fabs(src_aspect - scl_aspect), <, 0.01);
}

static void
test_fit_invariants(void)
{
	check_fit_invariants(5760, 3240, 1920, 1080);
	check_fit_invariants(1920, 1080, 3440, 1440); /* pillarbox */
	check_fit_invariants(1080, 1920, 1920, 1080); /* tall image -> pillarbox */
	check_fit_invariants(1000, 1000, 1920, 1080);
	check_fit_invariants(1920, 1080, 1920, 1080);
}

/* ---- center (1:1) ----------------------------------------------- */

static void
test_center_smaller_image(void)
{
	gint sx, sy, dx, dy, cw, ch;

	/* Image smaller than the monitor: padded, copied whole. */
	gowl_wallpaper_center_rect(1000, 800, 1920, 1080,
	                           &sx, &sy, &dx, &dy, &cw, &ch);
	g_assert_cmpint(sx, ==, 0);
	g_assert_cmpint(sy, ==, 0);
	g_assert_cmpint(dx, ==, (1920 - 1000) / 2);
	g_assert_cmpint(dy, ==, (1080 - 800) / 2);
	g_assert_cmpint(cw, ==, 1000);
	g_assert_cmpint(ch, ==, 800);
}

static void
test_center_larger_image(void)
{
	gint sx, sy, dx, dy, cw, ch;

	/* Image larger than the monitor: center-cropped 1:1. */
	gowl_wallpaper_center_rect(5760, 3240, 1920, 1080,
	                           &sx, &sy, &dx, &dy, &cw, &ch);
	g_assert_cmpint(sx, ==, (5760 - 1920) / 2);
	g_assert_cmpint(sy, ==, (3240 - 1080) / 2);
	g_assert_cmpint(dx, ==, 0);
	g_assert_cmpint(dy, ==, 0);
	g_assert_cmpint(cw, ==, 1920);
	g_assert_cmpint(ch, ==, 1080);
}

static void
test_center_mixed_axes(void)
{
	gint sx, sy, dx, dy, cw, ch;

	/* Wider than the monitor but shorter: crop X, pad Y. */
	gowl_wallpaper_center_rect(3000, 600, 1920, 1080,
	                           &sx, &sy, &dx, &dy, &cw, &ch);
	g_assert_cmpint(sx, ==, (3000 - 1920) / 2);
	g_assert_cmpint(dx, ==, 0);
	g_assert_cmpint(cw, ==, 1920);
	g_assert_cmpint(sy, ==, 0);
	g_assert_cmpint(dy, ==, (1080 - 600) / 2);
	g_assert_cmpint(ch, ==, 600);
}

/* ---- tile ------------------------------------------------------- */

static void
test_tile_count(void)
{
	gint cols, rows;

	gowl_wallpaper_tile_count(1920, 1080, 1920, 1080, &cols, &rows);
	g_assert_cmpint(cols, ==, 1);
	g_assert_cmpint(rows, ==, 1);

	gowl_wallpaper_tile_count(1000, 1000, 1920, 1080, &cols, &rows);
	g_assert_cmpint(cols, ==, 2);
	g_assert_cmpint(rows, ==, 2);

	gowl_wallpaper_tile_count(640, 480, 1920, 1080, &cols, &rows);
	g_assert_cmpint(cols, ==, 3);   /* ceil(1920/640) */
	g_assert_cmpint(rows, ==, 3);   /* ceil(1080/480) */

	gowl_wallpaper_tile_count(960, 540, 1920, 1080, &cols, &rows);
	g_assert_cmpint(cols, ==, 2);   /* exact divisor */
	g_assert_cmpint(rows, ==, 2);
}

/* ---- degenerate / guard cases ----------------------------------- */

static void
test_degenerate_inputs(void)
{
	gint sw, sh, cx, cy, sx, sy, dx, dy, cw, ch, cols, rows;

	/* Zero image dimensions must not divide by zero or crash. */
	gowl_wallpaper_cover_rect(0, 1080, 1920, 1080, &sw, &sh, &cx, &cy);
	gowl_wallpaper_cover_rect(1920, 0, 1920, 1080, &sw, &sh, &cx, &cy);
	gowl_wallpaper_fit_rect(0, 0, 1920, 1080, &sw, &sh, &cx, &cy);

	/* Zero monitor dimensions (unconfigured output). */
	gowl_wallpaper_cover_rect(1920, 1080, 0, 0, &sw, &sh, &cx, &cy);

	/* center with a degenerate image: all zero, no crash. */
	gowl_wallpaper_center_rect(0, 0, 1920, 1080,
	                           &sx, &sy, &dx, &dy, &cw, &ch);
	g_assert_cmpint(cw, ==, 0);
	g_assert_cmpint(ch, ==, 0);

	/* tile with a degenerate image yields zero tiles. */
	gowl_wallpaper_tile_count(0, 0, 1920, 1080, &cols, &rows);
	g_assert_cmpint(cols, ==, 0);
	g_assert_cmpint(rows, ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/wallpaper/cover-invariants", test_cover_invariants);
	g_test_add_func("/wallpaper/cover-same-aspect-no-crop",
	                test_cover_same_aspect_no_crop);
	g_test_add_func("/wallpaper/cover-scales-up", test_cover_scales_up);
	g_test_add_func("/wallpaper/fit-invariants", test_fit_invariants);
	g_test_add_func("/wallpaper/center-smaller", test_center_smaller_image);
	g_test_add_func("/wallpaper/center-larger", test_center_larger_image);
	g_test_add_func("/wallpaper/center-mixed", test_center_mixed_axes);
	g_test_add_func("/wallpaper/tile-count", test_tile_count);
	g_test_add_func("/wallpaper/degenerate", test_degenerate_inputs);

	return g_test_run();
}
