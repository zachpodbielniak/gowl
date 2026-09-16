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

/*
 * The arithmetic that puts a screenshot where it was asked for.
 *
 * A selection is dragged in layout coordinates and cropped out of a
 * framebuffer in device pixels.  On an unscaled monitor those are the
 * same numbers, which is why the conversion was missing for so long
 * without anybody noticing; on a scaled one, leaving it out takes a
 * picture of somewhere else.  These tests are written against that
 * failure: most of them are constructed so that the answer WOULD be
 * the un-converted input if the conversion were dropped.
 */

#include <glib.h>

#include "util/gowl-capture-scale.h"

/* ------------------------------------------------------------------ *
 * gowl_capture_scale_factor
 * ------------------------------------------------------------------ */

static void
test_factor_unscaled(void)
{
	g_assert_cmpfloat(gowl_capture_scale_factor(1920, 1080, 1920, 1080),
	                  ==, 1.0);
}

static void
test_factor_hidpi(void)
{
	g_assert_cmpfloat(gowl_capture_scale_factor(1920, 1080, 3840, 2160),
	                  ==, 2.0);
}

static void
test_factor_fractional(void)
{
	gdouble s;

	/* 2560 at scale 1.5 -> a layout width of 1707, rounded.  The
	 * factor is measured back off the image, so it is what the
	 * pixels say and not the 1.5 that was configured. */
	s = gowl_capture_scale_factor(1707, 960, 2560, 1440);
	g_assert_cmpfloat(s, >, 1.49);
	g_assert_cmpfloat(s, <, 1.51);
}

static void
test_factor_degenerate(void)
{
	/* Nothing to divide by: 1.0, not a crash and not an infinity. */
	g_assert_cmpfloat(gowl_capture_scale_factor(0, 0, 100, 100),
	                  ==, 1.0);
	g_assert_cmpfloat(gowl_capture_scale_factor(100, 100, 0, 0),
	                  ==, 1.0);
}

/* ------------------------------------------------------------------ *
 * gowl_capture_scale_crop -- the reported bug
 * ------------------------------------------------------------------ */

static void
test_crop_unscaled_is_identity(void)
{
	gint x, y, w, h;

	/* Scale 1, origin 0: the region comes back untouched.  This is
	 * the case the old code got right, and it must stay right. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                      1920, 1080,
	                                      100, 200, 300, 400,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 100);
	g_assert_cmpint(y, ==, 200);
	g_assert_cmpint(w, ==, 300);
	g_assert_cmpint(h, ==, 400);
}

static void
test_crop_scale_two_doubles_everything(void)
{
	gint x, y, w, h;

	/* A 3840x2160 panel at scale 2 is 1920x1080 of layout.  A
	 * selection at (100,200) is at (200,400) in the framebuffer --
	 * not at (100,200), which is what it used to crop, and which is
	 * why the picture came out of the wrong place. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                      3840, 2160,
	                                      100, 200, 300, 400,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 200);
	g_assert_cmpint(y, ==, 400);
	g_assert_cmpint(w, ==, 600);
	g_assert_cmpint(h, ==, 800);
}

static void
test_crop_offset_monitor(void)
{
	gint x, y, w, h;

	/* The second monitor starts at layout x=1920.  A selection at
	 * layout 2000 is 80 into THAT screen, so 80 device pixels in at
	 * scale 1 -- not 2000, which is off the end of the image. */
	g_assert_true(gowl_capture_scale_crop(1920, 0, 1920, 1080,
	                                      1920, 1080,
	                                      2000, 100, 200, 100,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 80);
	g_assert_cmpint(y, ==, 100);
	g_assert_cmpint(w, ==, 200);
	g_assert_cmpint(h, ==, 100);
}

static void
test_crop_offset_monitor_scaled(void)
{
	gint x, y, w, h;

	/* Both corrections at once: a scaled second monitor.  80 layout
	 * units in, at scale 2, is 160 device pixels in. */
	g_assert_true(gowl_capture_scale_crop(1920, 0, 1920, 1080,
	                                      3840, 2160,
	                                      2000, 100, 200, 100,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 160);
	g_assert_cmpint(y, ==, 200);
	g_assert_cmpint(w, ==, 400);
	g_assert_cmpint(h, ==, 200);
}

static void
test_crop_clamps_to_monitor(void)
{
	gint x, y, w, h;

	/* Dragged off the right edge.  The part that was on the screen
	 * survives, and the crop stops at the last pixel. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                      3840, 2160,
	                                      1800, 1000, 400, 400,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 3600);
	g_assert_cmpint(y, ==, 2000);
	g_assert_cmpint(x + w, ==, 3840);
	g_assert_cmpint(y + h, ==, 2160);
}

static void
test_crop_clamps_negative_origin(void)
{
	gint x, y, w, h;

	/* Dragged off the top-left.  The crop starts at the image's own
	 * corner rather than reading from before the buffer. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                      3840, 2160,
	                                      -100, -50, 300, 200,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 0);
	g_assert_cmpint(y, ==, 0);
	g_assert_cmpint(w, ==, 400);  /* the 200 layout units on-screen */
	g_assert_cmpint(h, ==, 300);  /* the 150 layout units on-screen */
}

static void
test_crop_whole_monitor(void)
{
	gint x, y, w, h;

	/* Selecting the entire screen gets the entire framebuffer, with
	 * nothing lost to rounding at either edge. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                      3840, 2160,
	                                      0, 0, 1920, 1080,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 0);
	g_assert_cmpint(y, ==, 0);
	g_assert_cmpint(w, ==, 3840);
	g_assert_cmpint(h, ==, 2160);
}

static void
test_crop_fractional_scale_stays_inside(void)
{
	gint x, y, w, h;

	/* 1.5x, where the layout size is a rounded quotient.  Rounding
	 * the edges outwards must never walk off the image. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1707, 960,
	                                      2560, 1440,
	                                      1600, 900, 107, 60,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, >=, 0);
	g_assert_cmpint(y, >=, 0);
	g_assert_cmpint(x + w, <=, 2560);
	g_assert_cmpint(y + h, <=, 1440);
	g_assert_cmpint(w, >, 0);
	g_assert_cmpint(h, >, 0);
}

static void
test_crop_rounds_outwards(void)
{
	gint x, y, w, h;

	/*
	 * At 1.5x a selection from 1 to 3 covers device pixels 1.5 to
	 * 4.5.  Both edges move away from the selection -- the near one
	 * down to 1, the far one up to 5 -- so every pixel the rubber
	 * band was drawn around is in the picture.  Rounding either edge
	 * the other way, or both the same way, gives a different answer
	 * than this.
	 */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1000, 1000,
	                                      1500, 1500,
	                                      1, 1, 2, 2,
	                                      &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 1);
	g_assert_cmpint(y, ==, 1);
	g_assert_cmpint(w, ==, 4);
	g_assert_cmpint(h, ==, 4);
}

static void
test_crop_misses_monitor(void)
{
	gint x, y, w, h;

	/* Entirely on the other screen: nothing to crop, and it says so
	 * rather than handing back a rectangle of somebody else's
	 * pixels. */
	g_assert_false(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                       3840, 2160,
	                                       2000, 100, 200, 100,
	                                       &x, &y, &w, &h));
	g_assert_cmpint(w, ==, 0);
	g_assert_cmpint(h, ==, 0);
}

static void
test_crop_degenerate_inputs(void)
{
	gint x, y, w, h;

	/* Zero-sized region, zero-sized monitor, zero-sized image. */
	g_assert_false(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                       3840, 2160,
	                                       10, 10, 0, 0,
	                                       &x, &y, &w, &h));
	g_assert_false(gowl_capture_scale_crop(0, 0, 0, 0,
	                                       3840, 2160,
	                                       10, 10, 100, 100,
	                                       &x, &y, &w, &h));
	g_assert_false(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                       0, 0,
	                                       10, 10, 100, 100,
	                                       &x, &y, &w, &h));
}

static void
test_crop_null_outs(void)
{
	/* Asking only whether it fits must not require somewhere to put
	 * the answer. */
	g_assert_true(gowl_capture_scale_crop(0, 0, 1920, 1080,
	                                      3840, 2160,
	                                      10, 10, 100, 100,
	                                      NULL, NULL, NULL, NULL));
}

/* ------------------------------------------------------------------ *
 * gowl_capture_scale_place -- the all-monitors stitch
 * ------------------------------------------------------------------ */

static void
test_place_single_unscaled(void)
{
	gint x, y, w, h;

	g_assert_true(gowl_capture_scale_place(0, 0, 1920, 1080,
	                                       0, 0, 1.0,
	                                       &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 0);
	g_assert_cmpint(y, ==, 0);
	g_assert_cmpint(w, ==, 1920);
	g_assert_cmpint(h, ==, 1080);
}

static void
test_place_single_scaled(void)
{
	gint x, y, w, h;

	/* At canvas scale 2 the monitor occupies its framebuffer's worth
	 * of canvas, which is what lets the blit be a straight copy. */
	g_assert_true(gowl_capture_scale_place(0, 0, 1920, 1080,
	                                       0, 0, 2.0,
	                                       &x, &y, &w, &h));
	g_assert_cmpint(w, ==, 3840);
	g_assert_cmpint(h, ==, 2160);
}

static void
test_place_side_by_side_abuts(void)
{
	gint ax, ay, aw, ah;
	gint bx, by, bw, bh;

	/* Two screens touching in the layout touch on the canvas: the
	 * second starts exactly where the first ends.  A seam of blank
	 * pixels or a one-pixel overlap would both show. */
	g_assert_true(gowl_capture_scale_place(0, 0, 1920, 1080,
	                                       0, 0, 2.0,
	                                       &ax, &ay, &aw, &ah));
	g_assert_true(gowl_capture_scale_place(1920, 0, 1920, 1080,
	                                       0, 0, 2.0,
	                                       &bx, &by, &bw, &bh));
	g_assert_cmpint(ax + aw, ==, bx);
}

static void
test_place_negative_origin(void)
{
	gint x, y, w, h;

	/* A monitor to the left of the primary has a negative layout x;
	 * the canvas origin moves it back to zero. */
	g_assert_true(gowl_capture_scale_place(-1920, 0, 1920, 1080,
	                                       -1920, 0, 1.0,
	                                       &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 0);
	g_assert_cmpint(w, ==, 1920);
}

static void
test_place_mixed_scale_keeps_apparent_size(void)
{
	gint ax, ay, aw, ah;
	gint bx, by, bw, bh;

	/* A scale-1 screen next to a scale-2 one, on a scale-2 canvas.
	 * Both are 1920 layout units wide, so both are 3840 canvas
	 * pixels wide -- the scale-1 one gets stretched, which is what
	 * keeps its windows the size they look on the desktop. */
	g_assert_true(gowl_capture_scale_place(0, 0, 1920, 1080,
	                                       0, 0, 2.0,
	                                       &ax, &ay, &aw, &ah));
	g_assert_true(gowl_capture_scale_place(1920, 0, 1920, 1080,
	                                       0, 0, 2.0,
	                                       &bx, &by, &bw, &bh));
	g_assert_cmpint(aw, ==, bw);
	g_assert_cmpint(aw, ==, 3840);
}

static void
test_place_degenerate(void)
{
	gint x, y, w, h;

	g_assert_false(gowl_capture_scale_place(0, 0, 0, 0,
	                                        0, 0, 1.0,
	                                        &x, &y, &w, &h));
	g_assert_false(gowl_capture_scale_place(0, 0, 1920, 1080,
	                                        0, 0, 0.0,
	                                        &x, &y, &w, &h));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/capture-scale/factor/unscaled",
	                test_factor_unscaled);
	g_test_add_func("/capture-scale/factor/hidpi",
	                test_factor_hidpi);
	g_test_add_func("/capture-scale/factor/fractional",
	                test_factor_fractional);
	g_test_add_func("/capture-scale/factor/degenerate",
	                test_factor_degenerate);

	g_test_add_func("/capture-scale/crop/unscaled-is-identity",
	                test_crop_unscaled_is_identity);
	g_test_add_func("/capture-scale/crop/scale-two-doubles-everything",
	                test_crop_scale_two_doubles_everything);
	g_test_add_func("/capture-scale/crop/offset-monitor",
	                test_crop_offset_monitor);
	g_test_add_func("/capture-scale/crop/offset-monitor-scaled",
	                test_crop_offset_monitor_scaled);
	g_test_add_func("/capture-scale/crop/clamps-to-monitor",
	                test_crop_clamps_to_monitor);
	g_test_add_func("/capture-scale/crop/clamps-negative-origin",
	                test_crop_clamps_negative_origin);
	g_test_add_func("/capture-scale/crop/whole-monitor",
	                test_crop_whole_monitor);
	g_test_add_func("/capture-scale/crop/fractional-stays-inside",
	                test_crop_fractional_scale_stays_inside);
	g_test_add_func("/capture-scale/crop/rounds-outwards",
	                test_crop_rounds_outwards);
	g_test_add_func("/capture-scale/crop/misses-monitor",
	                test_crop_misses_monitor);
	g_test_add_func("/capture-scale/crop/degenerate-inputs",
	                test_crop_degenerate_inputs);
	g_test_add_func("/capture-scale/crop/null-outs",
	                test_crop_null_outs);

	g_test_add_func("/capture-scale/place/single-unscaled",
	                test_place_single_unscaled);
	g_test_add_func("/capture-scale/place/single-scaled",
	                test_place_single_scaled);
	g_test_add_func("/capture-scale/place/side-by-side-abuts",
	                test_place_side_by_side_abuts);
	g_test_add_func("/capture-scale/place/negative-origin",
	                test_place_negative_origin);
	g_test_add_func("/capture-scale/place/mixed-scale-apparent-size",
	                test_place_mixed_scale_keeps_apparent_size);
	g_test_add_func("/capture-scale/place/degenerate",
	                test_place_degenerate);

	return g_test_run();
}
