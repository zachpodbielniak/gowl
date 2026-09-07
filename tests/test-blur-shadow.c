/* test-blur-shadow.c -- the analytic drop shadow
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The shadow is a blurred rectangle evaluated in closed form rather than
 * drawn and blurred.  That buys a shadow that costs nothing per frame,
 * and it costs a function that has to be right by argument rather than by
 * inspection -- so the shape of the falloff, the rounding of the corners
 * and (most of all) the premultiplied alpha are checked here.
 *
 * Premultiplication is the one worth the trouble: get it wrong and every
 * shadow grows a bright halo along its soft edge, which looks exactly
 * like a driver bug and is not one.
 */

#include <glib.h>
#include <math.h>

#include "../modules/blur/gowl-blur-shadow.c"

#define RECT_X 40.0
#define RECT_Y 40.0
#define RECT_W 200.0
#define RECT_H 120.0
#define RADIUS 20.0

/* Opaque well inside, gone well outside, and never out of range. */
static void
test_coverage_range(void)
{
	gdouble x, y;

	g_assert_cmpfloat(gowl_blur_shadow_alpha(RECT_X + RECT_W * 0.5,
	                                          RECT_Y + RECT_H * 0.5,
	                                          RECT_X, RECT_Y, RECT_W, RECT_H,
	                                          RADIUS, 8.0), >, 0.99);
	g_assert_cmpfloat(gowl_blur_shadow_alpha(RECT_X - RADIUS * 3.0,
	                                          RECT_Y + RECT_H * 0.5,
	                                          RECT_X, RECT_Y, RECT_W, RECT_H,
	                                          RADIUS, 8.0), <, 0.01);

	for (y = 0.0; y < 240.0; y += 7.0) {
		for (x = 0.0; x < 320.0; x += 7.0) {
			gdouble a = gowl_blur_shadow_alpha(x, y, RECT_X, RECT_Y,
			                                    RECT_W, RECT_H, RADIUS, 8.0);

			g_assert_cmpfloat(a, >=, 0.0);
			g_assert_cmpfloat(a, <=, 1.0);
		}
	}
}

/* Coverage falls off monotonically as you walk away from the edge --
 * a shadow that got darker further out would be a ring, not a shadow. */
static void
test_falloff_is_monotonic(void)
{
	gdouble previous = 2.0;
	gdouble x;

	for (x = RECT_X; x > RECT_X - RADIUS * 3.0; x -= 1.0) {
		gdouble a = gowl_blur_shadow_alpha(x, RECT_Y + RECT_H * 0.5,
		                                    RECT_X, RECT_Y, RECT_W, RECT_H,
		                                    RADIUS, 8.0);

		g_assert_cmpfloat(a, <=, previous + 1e-9);
		previous = a;
	}
	g_assert_cmpfloat(previous, <, 0.02);
}

/* The corners are rounded, not square: the diagonal has to be lighter
 * than the straight edge at the same distance out. */
static void
test_corners_are_rounded(void)
{
	gdouble edge, corner;

	edge = gowl_blur_shadow_alpha(RECT_X - 6.0, RECT_Y + RECT_H * 0.5,
	                               RECT_X, RECT_Y, RECT_W, RECT_H,
	                               RADIUS, 24.0);
	corner = gowl_blur_shadow_alpha(RECT_X - 6.0, RECT_Y - 6.0,
	                                 RECT_X, RECT_Y, RECT_W, RECT_H,
	                                 RADIUS, 24.0);
	g_assert_cmpfloat(corner, <, edge);
}

/* A zero radius is a hard edge rather than a division by zero. */
static void
test_zero_radius_is_a_hard_edge(void)
{
	g_assert_cmpfloat(gowl_blur_shadow_alpha(RECT_X + 10.0, RECT_Y + 10.0,
	                                          RECT_X, RECT_Y, RECT_W, RECT_H,
	                                          0.0, 0.0), ==, 1.0);
	g_assert_cmpfloat(gowl_blur_shadow_alpha(RECT_X - 10.0, RECT_Y + 10.0,
	                                          RECT_X, RECT_Y, RECT_W, RECT_H,
	                                          0.0, 0.0), ==, 0.0);
}

/* A degenerate rectangle casts nothing rather than dividing by zero. */
static void
test_empty_rect_casts_nothing(void)
{
	g_assert_cmpfloat(gowl_blur_shadow_alpha(10.0, 10.0, 0.0, 0.0,
	                                          0.0, 50.0, 10.0, 0.0), ==, 0.0);
	g_assert_cmpfloat(gowl_blur_shadow_alpha(10.0, 10.0, 0.0, 0.0,
	                                          50.0, 0.0, 10.0, 0.0), ==, 0.0);
}

/*
 * THE ONE THAT MATTERS.  wlroots composites premultiplied alpha, so every
 * colour channel must be <= the alpha at that pixel.  Storing straight
 * colour instead is not an error anywhere -- it just puts a bright fringe
 * around every shadow on the desktop.
 */
static void
test_output_is_premultiplied(void)
{
	const gdouble grey[3] = { 1.0, 1.0, 1.0 };
	guint8 *pixels;
	gint    width = 280, height = 200;
	gint    x, y;
	gboolean saw_partial = FALSE;

	pixels = gowl_blur_shadow_render(width, height, RECT_X, RECT_Y,
	                                  RECT_W, RECT_H, RADIUS, 8.0,
	                                  1.0, grey);
	g_assert_nonnull(pixels);

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			const guint8 *p = pixels + ((gsize)y * width + x) * 4;

			g_assert_cmpint(p[0], <=, p[3]);
			g_assert_cmpint(p[1], <=, p[3]);
			g_assert_cmpint(p[2], <=, p[3]);
			if (p[3] > 20 && p[3] < 235)
				saw_partial = TRUE;
		}
	}
	/* A soft edge exists at all -- otherwise the assertions above are
	 * trivially satisfied by a fully hard shadow. */
	g_assert_true(saw_partial);
	g_free(pixels);
}

/* Opacity scales the whole thing, and zero opacity draws nothing. */
static void
test_opacity_scales(void)
{
	const gdouble black[3] = { 0.0, 0.0, 0.0 };
	guint8 *full, *half, *none;
	gsize   centre;
	gint    width = 280, height = 200;

	full = gowl_blur_shadow_render(width, height, RECT_X, RECT_Y,
	                                RECT_W, RECT_H, RADIUS, 8.0, 1.0, black);
	half = gowl_blur_shadow_render(width, height, RECT_X, RECT_Y,
	                                RECT_W, RECT_H, RADIUS, 8.0, 0.5, black);
	none = gowl_blur_shadow_render(width, height, RECT_X, RECT_Y,
	                                RECT_W, RECT_H, RADIUS, 8.0, 0.0, black);
	g_assert_nonnull(full);
	g_assert_nonnull(half);
	g_assert_nonnull(none);

	centre = (((gsize)(RECT_Y + RECT_H * 0.5) * width)
	          + (gsize)(RECT_X + RECT_W * 0.5)) * 4;

	g_assert_cmpint(full[centre + 3], >, 240);
	g_assert_cmpint(half[centre + 3], >, 110);
	g_assert_cmpint(half[centre + 3], <, 140);
	g_assert_cmpint(none[centre + 3], ==, 0);

	g_free(full);
	g_free(half);
	g_free(none);
}

static void
test_render_refusals(void)
{
	const gdouble black[3] = { 0.0, 0.0, 0.0 };

	g_assert_null(gowl_blur_shadow_render(0, 10, 0, 0, 5, 5, 2, 0, 1.0, black));
	g_assert_null(gowl_blur_shadow_render(10, 0, 0, 0, 5, 5, 2, 0, 1.0, black));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/blur-shadow/coverage", test_coverage_range);
	g_test_add_func("/blur-shadow/monotonic", test_falloff_is_monotonic);
	g_test_add_func("/blur-shadow/rounded", test_corners_are_rounded);
	g_test_add_func("/blur-shadow/hard-edge", test_zero_radius_is_a_hard_edge);
	g_test_add_func("/blur-shadow/empty", test_empty_rect_casts_nothing);
	g_test_add_func("/blur-shadow/premultiplied",
	                test_output_is_premultiplied);
	g_test_add_func("/blur-shadow/opacity", test_opacity_scales);
	g_test_add_func("/blur-shadow/refusals", test_render_refusals);

	return g_test_run();
}
