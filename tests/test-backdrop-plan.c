/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Where a per-window backdrop is rendered, and what part of it shows.
 *
 * Shared by the liquid glass and the liquid water, which both render one
 * buffer per window rather than cropping one shared picture.
 *
 * Two different failures are guarded here.
 *
 * The one that ABORTS: wlr_scene_buffer_set_source_box() hands its box
 * to wlr_render_pass_add_texture(), which asserts the box lies inside
 * the texture -- on the compositor thread, during a page flip.  Under
 * `cmacs --gowl' that process is the whole desktop session, and Emacs
 * re-raises the fatal signal on its main thread, so the report comes
 * back with an idle pselect backtrace naming nothing to do with glass.
 * The blur module cost a core dump to find this once already.
 *
 * The one that merely looks wrong: these work in three coordinate
 * systems at once -- layout for where the window is, device pixels for
 * the wallpaper it bends, and the render buffer's own pixels, which are
 * the device ones divided by however far the render was scaled down
 * while the window was moving.  Each conversion is a chance to be off by
 * the output's scale, which on a HiDPI screen means refracting the
 * top-left quarter of the wallpaper.  That does not crash, so only a
 * test catches it.
 */

#include <glib.h>
#include <math.h>

#include "../src/util/gowl-backdrop-plan.c"

/* The exact condition wlroots asserts. */
static void
assert_inside_buffer(const GowlBackdropPlan *plan, const gchar *what)
{
	if (plan->src.x < 0.0 || plan->src.y < 0.0
	    || plan->src.x + plan->src.width  > (gdouble)plan->buf_width
	    || plan->src.y + plan->src.height > (gdouble)plan->buf_height) {
		g_error("%s: src {%g,%g %gx%g} outside a %dx%d buffer "
		        "-- wlroots would abort the session here",
		        what, plan->src.x, plan->src.y,
		        plan->src.width, plan->src.height,
		        plan->buf_width, plan->buf_height);
	}
}

/* A window sitting wholly on a 1:1 output: the buffer is the window, the
 * crop is all of it, and it refracts the wallpaper under itself. */
static void
test_whole_window_on_a_1x_output(void)
{
	struct wlr_box mon   = { 0, 0, 1920, 1080 };
	struct wlr_box frame = { 100, 200, 800, 600 };
	GowlBackdropPlan  plan;

	g_assert_true(gowl_backdrop_plan(&frame, &mon, 1920, 1080, 1, &plan));
	g_assert_cmpint(plan.buf_width, ==, 800);
	g_assert_cmpint(plan.buf_height, ==, 600);
	g_assert_cmpfloat(plan.origin_x, ==, 100.0);
	g_assert_cmpfloat(plan.origin_y, ==, 200.0);
	g_assert_cmpfloat(plan.src.x, ==, 0.0);
	g_assert_cmpfloat(plan.src.y, ==, 0.0);
	g_assert_cmpfloat(plan.src.width, ==, 800.0);
	g_assert_cmpfloat(plan.src.height, ==, 600.0);
	assert_inside_buffer(&plan, "whole window");
}

/*
 * The same window on a HiDPI output.
 *
 * Everything doubles except the numbers that are already in device
 * pixels.  Getting this wrong does not crash -- it renders a window-sized
 * buffer that bends the top-left quarter of the wallpaper, which reads as
 * "the glass is showing the wrong thing" long after the change that did
 * it.
 */
static void
test_hidpi_doubles_everything_once(void)
{
	struct wlr_box mon   = { 0, 0, 1440, 960 };   /* logical */
	struct wlr_box frame = { 100, 200, 800, 600 };
	GowlBackdropPlan  plan;

	/* 2880x1920 device pixels behind a 1440x960 logical output. */
	g_assert_true(gowl_backdrop_plan(&frame, &mon, 2880, 1920, 1, &plan));
	g_assert_cmpint(plan.buf_width, ==, 1600);
	g_assert_cmpint(plan.buf_height, ==, 1200);
	g_assert_cmpfloat(plan.scale_x, ==, 2.0);
	g_assert_cmpfloat(plan.scale_y, ==, 2.0);
	g_assert_cmpfloat(plan.origin_x, ==, 200.0);
	g_assert_cmpfloat(plan.origin_y, ==, 400.0);
	assert_inside_buffer(&plan, "hidpi");
}

/*
 * A window being moved is rendered at half resolution and stretched.
 *
 * The divisor belongs to the BUFFER, not to the wallpaper: the texture is
 * still full size, so the origin must not be halved with everything else.
 * Halving it too puts the glass a windowful up and to the left, and only
 * while something is moving -- which is exactly when nobody can see it
 * clearly enough to tell.
 */
static void
test_divisor_shrinks_the_buffer_not_the_wallpaper(void)
{
	struct wlr_box mon   = { 0, 0, 1440, 960 };
	struct wlr_box frame = { 100, 200, 800, 600 };
	GowlBackdropPlan  full, half;

	g_assert_true(gowl_backdrop_plan(&frame, &mon, 2880, 1920, 1, &full));
	g_assert_true(gowl_backdrop_plan(&frame, &mon, 2880, 1920, 2, &half));

	g_assert_cmpint(half.buf_width, ==, full.buf_width / 2);
	g_assert_cmpint(half.buf_height, ==, full.buf_height / 2);
	g_assert_cmpfloat(half.scale_x, ==, full.scale_x / 2.0);
	/*
	 * And the way BACK, which is the half of this that was missing and
	 * shipped: a half-resolution buffer is not looking at half the
	 * window, it is looking at all of it less finely.  A shader given
	 * only the buffer's own units walks the source at buffer pace and
	 * covers a quarter of the area, magnified fourfold.
	 */
	g_assert_cmpfloat(full.src_scale, ==, 1.0);
	g_assert_cmpfloat(half.src_scale, ==, 2.0);
	/* The two must compose back to the device ratio however they are
	 * split: buffer pixels per logical pixel, times source pixels per
	 * buffer pixel, is source pixels per logical pixel either way. */
	g_assert_cmpfloat(fabs(half.scale_x * half.src_scale
	                       - full.scale_x * full.src_scale), <, 1e-9);
	/* The wallpaper did not move. */
	g_assert_cmpfloat(half.origin_x, ==, full.origin_x);
	g_assert_cmpfloat(half.origin_y, ==, full.origin_y);
	/* And the window still appears at the same size. */
	g_assert_cmpint(half.vis.width, ==, full.vis.width);
	g_assert_cmpint(half.vis.height, ==, full.vis.height);
	assert_inside_buffer(&half, "half resolution");
}

/*
 * The blur module's crash, one layer up: a scrolling layout leaves a
 * window's geometry unclipped on purpose, so it describes a rectangle
 * mostly off the output.  The whole window is still rendered -- the
 * refraction depends on the distance to its own edge, so a window
 * rendered as though it ended at the screen edge would grow a second
 * bevel down the middle of the screen -- but the CROP must stay inside.
 */
static void
test_window_hanging_off_the_right_edge(void)
{
	struct wlr_box mon   = { 0, 0, 2880, 1920 };
	struct wlr_box frame = { 2848, 62, 1376, 1800 };
	GowlBackdropPlan  plan;

	g_assert_true(gowl_backdrop_plan(&frame, &mon, 2880, 1920, 1, &plan));
	/* The buffer is the whole window, not the visible sliver. */
	g_assert_cmpint(plan.buf_width, ==, 1376);
	/* Only 32 logical pixels of it are on screen. */
	g_assert_cmpint(plan.vis.width, ==, 32);
	g_assert_cmpfloat(plan.src.width, ==, 32.0);
	assert_inside_buffer(&plan, "overflowing window");
}

/* A window on the monitor to the left: its origin is negative, which is
 * not an error -- the shader clamps and the glass repeats the
 * wallpaper's edge there. */
static void
test_window_starting_before_the_output(void)
{
	struct wlr_box mon   = { 1920, 0, 1920, 1080 };
	struct wlr_box frame = { 1800, 100, 400, 300 };
	GowlBackdropPlan  plan;

	g_assert_true(gowl_backdrop_plan(&frame, &mon, 1920, 1080, 1, &plan));
	g_assert_cmpfloat(plan.origin_x, ==, -120.0);
	g_assert_cmpfloat(plan.src.x, ==, 120.0);
	g_assert_cmpfloat(plan.src.width, ==, 280.0);
	assert_inside_buffer(&plan, "window from the next output");
}

/* Nothing to draw, said clearly rather than by drawing an empty box. */
static void
test_refusals(void)
{
	struct wlr_box mon     = { 0, 0, 1920, 1080 };
	struct wlr_box off     = { 4000, 4000, 400, 300 };
	struct wlr_box tiny    = { 10, 10, 4, 4 };
	struct wlr_box frame   = { 10, 10, 400, 300 };
	struct wlr_box degen   = { 0, 0, 0, 0 };
	GowlBackdropPlan  plan;

	g_assert_false(gowl_backdrop_plan(&frame, &mon, 0, 0, 1, &plan));
	g_assert_false(gowl_backdrop_plan(&off, &mon, 1920, 1080, 1, &plan));
	g_assert_false(gowl_backdrop_plan(&tiny, &mon, 1920, 1080, 1, &plan));
	g_assert_false(gowl_backdrop_plan(&frame, &degen, 1920, 1080, 1, &plan));
	g_assert_false(gowl_backdrop_plan(NULL, &mon, 1920, 1080, 1, &plan));
	g_assert_false(gowl_backdrop_plan(&frame, &mon, 1920, 1080, 1, NULL));
}

/*
 * A moved window has to be drawn again.
 *
 * This is the whole difference from the blur, whose backdrop is a crop of
 * one shared picture and is correct wherever the window goes.  Glass
 * bends what is behind THIS window, so leaving position out of the check
 * leaves a dragged window refracting where it used to be.
 */
static void
test_staleness(void)
{
	struct wlr_box mon = { 0, 0, 1920, 1080 };
	struct wlr_box a   = { 100, 100, 400, 300 };
	struct wlr_box b   = { 140, 100, 400, 300 };
	struct wlr_box c   = { 100, 100, 500, 300 };
	GowlBackdropPlan  pa, pb, pc;

	g_assert_true(gowl_backdrop_plan(&a, &mon, 1920, 1080, 1, &pa));
	g_assert_true(gowl_backdrop_plan(&b, &mon, 1920, 1080, 1, &pb));
	g_assert_true(gowl_backdrop_plan(&c, &mon, 1920, 1080, 1, &pc));

	/* Nothing changed. */
	g_assert_false(gowl_backdrop_render_stale(TRUE, &pa, &pa, 7, 7, 3, 3));
	/* No buffer yet. */
	g_assert_true(gowl_backdrop_render_stale(FALSE, &pa, &pa, 7, 7, 3, 3));
	/* It moved. */
	g_assert_true(gowl_backdrop_render_stale(TRUE, &pa, &pb, 7, 7, 3, 3));
	/* It resized. */
	g_assert_true(gowl_backdrop_render_stale(TRUE, &pa, &pc, 7, 7, 3, 3));
	/* The wallpaper was captured again -- a tag switch. */
	g_assert_true(gowl_backdrop_render_stale(TRUE, &pa, &pa, 7, 8, 3, 3));
	/* The settings changed -- a config reload. */
	g_assert_true(gowl_backdrop_render_stale(TRUE, &pa, &pa, 7, 7, 3, 4));
}

/*
 * A drag reports fractional positions that round to the same texture
 * pixel several frames running, and re-rendering for each of them is a
 * full pass over the window for a picture that comes out identical.
 */
static void
test_a_subpixel_move_is_not_a_move(void)
{
	GowlBackdropPlan a, b;

	memset(&a, 0, sizeof(a));
	a.buf_width = 400;
	a.buf_height = 300;
	a.origin_x = 100.0;
	a.origin_y = 100.0;
	b = a;

	b.origin_x = 100.4;
	g_assert_false(gowl_backdrop_render_stale(TRUE, &a, &b, 1, 1, 1, 1));
	b.origin_x = 100.6;
	g_assert_true(gowl_backdrop_render_stale(TRUE, &a, &b, 1, 1, 1, 1));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/backdrop-plan/whole-window", test_whole_window_on_a_1x_output);
	g_test_add_func("/backdrop-plan/hidpi", test_hidpi_doubles_everything_once);
	g_test_add_func("/backdrop-plan/divisor",
	                test_divisor_shrinks_the_buffer_not_the_wallpaper);
	g_test_add_func("/backdrop-plan/overflow",
	                test_window_hanging_off_the_right_edge);
	g_test_add_func("/backdrop-plan/before-output",
	                test_window_starting_before_the_output);
	g_test_add_func("/backdrop-plan/refusals", test_refusals);
	g_test_add_func("/backdrop-plan/staleness", test_staleness);
	g_test_add_func("/backdrop-plan/subpixel",
	                test_a_subpixel_move_is_not_a_move);
	return g_test_run();
}
