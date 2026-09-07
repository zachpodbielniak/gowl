/* test-expo.c -- the overview's grid and its zoom
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The layout is where the overview can be wrong without failing.  A grid
 * that does not fit crops a tag off the screen; a zoom that does not land
 * exactly on the tile it came from puts a visible jump at both ends of
 * every overview; a hit test that is off by a cell opens the wrong tag on
 * a click.  None of those crash, and none are obvious from reading the
 * code, so they are pinned here.
 *
 * The implementation is included rather than linked: it is one pure
 * translation unit inside a module .so that has no business being loaded
 * by a test.
 */

#include <glib.h>
#include <math.h>

#include "../modules/expo/gowl-expo-layout.c"

#define W 1920.0
#define H 1080.0

/* Every tile is a picture of the output, so every tile has to be the
 * output's shape.  A cell shaped differently would either letterbox the
 * desktop inside it or stretch it. */
static void
test_cells_keep_the_output_aspect(void)
{
	GowlExpoLayout layout;
	gint n, i;

	for (n = 1; n <= GOWL_EXPO_MAX_TAGS; n++) {
		g_assert_true(gowl_expo_layout_build(&layout, n, 0, W, H, 0.06));
		g_assert_cmpint(layout.count, ==, n);

		for (i = 0; i < layout.count; i++) {
			gdouble aspect = layout.cell[i].width / layout.cell[i].height;

			g_assert_cmpfloat(fabs(aspect - (W / H)), <, 1e-9);
		}
	}
}

/* And the whole grid has to be on screen, at every count and both gaps. */
static void
test_grid_fits_the_output(void)
{
	GowlExpoLayout layout;
	gdouble gap;
	gint n, i;

	for (gap = 0.0; gap <= 0.4001; gap += 0.1) {
		for (n = 1; n <= GOWL_EXPO_MAX_TAGS; n++) {
			g_assert_true(gowl_expo_layout_build(&layout, n, 0, W, H, gap));

			for (i = 0; i < layout.count; i++) {
				g_assert_cmpfloat(layout.cell[i].x, >=, -0.001);
				g_assert_cmpfloat(layout.cell[i].y, >=, -0.001);
				g_assert_cmpfloat(layout.cell[i].x + layout.cell[i].width,
				                  <=, W + 0.001);
				g_assert_cmpfloat(layout.cell[i].y + layout.cell[i].height,
				                  <=, H + 0.001);
			}
		}
	}
}

/* Tiles must not overlap, or two tags would be showing in one place. */
static void
test_cells_do_not_overlap(void)
{
	GowlExpoLayout layout;
	gint i, j;

	g_assert_true(gowl_expo_layout_build(&layout, 9, 0, W, H, 0.05));

	for (i = 0; i < layout.count; i++) {
		for (j = i + 1; j < layout.count; j++) {
			gboolean apart =
				layout.cell[i].x + layout.cell[i].width <= layout.cell[j].x + 1e-6
				|| layout.cell[j].x + layout.cell[j].width <= layout.cell[i].x + 1e-6
				|| layout.cell[i].y + layout.cell[i].height <= layout.cell[j].y + 1e-6
				|| layout.cell[j].y + layout.cell[j].height <= layout.cell[i].y + 1e-6;

			g_assert_true(apart);
		}
	}
}

static void
test_near_square_by_default(void)
{
	GowlExpoLayout layout;

	g_assert_true(gowl_expo_layout_build(&layout, 4, 0, W, H, 0.05));
	g_assert_cmpint(layout.columns, ==, 2);
	g_assert_cmpint(layout.rows, ==, 2);

	g_assert_true(gowl_expo_layout_build(&layout, 9, 0, W, H, 0.05));
	g_assert_cmpint(layout.columns, ==, 3);
	g_assert_cmpint(layout.rows, ==, 3);

	/* An explicit column count wins. */
	g_assert_true(gowl_expo_layout_build(&layout, 6, 6, W, H, 0.05));
	g_assert_cmpint(layout.columns, ==, 6);
	g_assert_cmpint(layout.rows, ==, 1);
}

/*
 * THE SEAM TEST.  At progress 0 the anchor tile must map to precisely the
 * output rectangle -- that is what makes the overview open out of the
 * live desktop and close back into the tag you picked with no cut.  Off
 * by a fraction of a per cent and there is a visible flick at both ends
 * of every overview.
 */
static void
test_closed_zoom_lands_exactly_on_the_anchor(void)
{
	GowlExpoLayout layout;
	gint anchor;

	g_assert_true(gowl_expo_layout_build(&layout, 9, 0, W, H, 0.06));

	for (anchor = 0; anchor < layout.count; anchor++) {
		gdouble scale, ox, oy, x, y, w, h;

		gowl_expo_layout_transform(&layout, anchor, 0.0, W, H,
		                            &scale, &ox, &oy);
		x = layout.cell[anchor].x * scale + ox;
		y = layout.cell[anchor].y * scale + oy;
		w = layout.cell[anchor].width * scale;
		h = layout.cell[anchor].height * scale;

		g_assert_cmpfloat(fabs(x - 0.0), <, 1e-6);
		g_assert_cmpfloat(fabs(y - 0.0), <, 1e-6);
		g_assert_cmpfloat(fabs(w - W), <, 1e-6);
		g_assert_cmpfloat(fabs(h - H), <, 1e-6);
	}
}

/* And at progress 1 the transform is the identity, or the grid would not
 * be where the layout put it. */
static void
test_open_zoom_is_the_identity(void)
{
	GowlExpoLayout layout;
	gdouble scale, ox, oy;

	g_assert_true(gowl_expo_layout_build(&layout, 6, 0, W, H, 0.06));
	gowl_expo_layout_transform(&layout, 3, 1.0, W, H, &scale, &ox, &oy);

	g_assert_cmpfloat(fabs(scale - 1.0), <, 1e-9);
	g_assert_cmpfloat(fabs(ox), <, 1e-6);
	g_assert_cmpfloat(fabs(oy), <, 1e-6);
}

/* The zoom is monotonic: no bounce, no overshoot, whatever the anchor. */
static void
test_zoom_is_monotonic(void)
{
	GowlExpoLayout layout;
	gdouble previous = G_MAXDOUBLE;
	gdouble p;

	g_assert_true(gowl_expo_layout_build(&layout, 9, 0, W, H, 0.06));

	for (p = 0.0; p <= 1.0001; p += 0.05) {
		gdouble scale, ox, oy;

		gowl_expo_layout_transform(&layout, 4, p, W, H, &scale, &ox, &oy);
		g_assert_cmpfloat(scale, <=, previous + 1e-9);
		g_assert_cmpfloat(scale, >=, 1.0 - 1e-9);
		previous = scale;
	}
}

/* A click lands on the tile the pointer is actually over, at any zoom. */
static void
test_hit_test_follows_the_zoom(void)
{
	GowlExpoLayout layout;
	gdouble scale, ox, oy;
	gint i;

	g_assert_true(gowl_expo_layout_build(&layout, 9, 0, W, H, 0.06));
	gowl_expo_layout_transform(&layout, 0, 1.0, W, H, &scale, &ox, &oy);

	for (i = 0; i < layout.count; i++) {
		gdouble cx = layout.cell[i].x + layout.cell[i].width * 0.5;
		gdouble cy = layout.cell[i].y + layout.cell[i].height * 0.5;

		g_assert_cmpint(gowl_expo_layout_at(&layout, scale, ox, oy, cx, cy),
		                ==, i);
	}

	/* Fully closed, the anchor covers the screen and everything else is
	 * off it, so the middle of the screen is the anchor. */
	gowl_expo_layout_transform(&layout, 5, 0.0, W, H, &scale, &ox, &oy);
	g_assert_cmpint(gowl_expo_layout_at(&layout, scale, ox, oy,
	                                     W * 0.5, H * 0.5), ==, 5);

	/* Outside every tile is not a tile. */
	g_assert_cmpint(gowl_expo_layout_at(&layout, scale, ox, oy,
	                                     -50.0, -50.0), ==, -1);
}

/* Keyboard movement clamps rather than wrapping, and never leaves the
 * selection on a hole in a short last row. */
static void
test_stepping_stays_on_a_real_tile(void)
{
	GowlExpoLayout layout;
	gint n, from, dx, dy;

	for (n = 1; n <= GOWL_EXPO_MAX_TAGS; n++) {
		g_assert_true(gowl_expo_layout_build(&layout, n, 0, W, H, 0.05));

		for (from = 0; from < layout.count; from++) {
			for (dx = -1; dx <= 1; dx++) {
				for (dy = -1; dy <= 1; dy++) {
					gint to = gowl_expo_layout_step(&layout, from, dx, dy);

					g_assert_cmpint(to, >=, 0);
					g_assert_cmpint(to, <, layout.count);
				}
			}
		}
	}
}

/* Specifically: no wrap.  Left from the leftmost column stays put. */
static void
test_stepping_does_not_wrap(void)
{
	GowlExpoLayout layout;

	g_assert_true(gowl_expo_layout_build(&layout, 9, 3, W, H, 0.05));

	g_assert_cmpint(gowl_expo_layout_step(&layout, 0, -1, 0), ==, 0);
	g_assert_cmpint(gowl_expo_layout_step(&layout, 0, 0, -1), ==, 0);
	g_assert_cmpint(gowl_expo_layout_step(&layout, 8, 1, 0), ==, 8);
	g_assert_cmpint(gowl_expo_layout_step(&layout, 8, 0, 1), ==, 8);
	/* And moving right from the end of a row goes DOWN a row's worth,
	 * not onto the next row -- the grid is two-dimensional. */
	g_assert_cmpint(gowl_expo_layout_step(&layout, 2, 1, 0), ==, 2);
	g_assert_cmpint(gowl_expo_layout_step(&layout, 2, 0, 1), ==, 5);
}

static void
test_refusals(void)
{
	GowlExpoLayout layout;

	g_assert_false(gowl_expo_layout_build(&layout, 0, 0, W, H, 0.05));
	g_assert_false(gowl_expo_layout_build(&layout, 3, 0, 0.0, H, 0.05));
	g_assert_false(gowl_expo_layout_build(&layout, 3, 0, W, 0.0, 0.05));
	g_assert_false(gowl_expo_layout_build(NULL, 3, 0, W, H, 0.05));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/expo/aspect", test_cells_keep_the_output_aspect);
	g_test_add_func("/expo/fits", test_grid_fits_the_output);
	g_test_add_func("/expo/no-overlap", test_cells_do_not_overlap);
	g_test_add_func("/expo/near-square", test_near_square_by_default);
	g_test_add_func("/expo/closed-is-exact",
	                test_closed_zoom_lands_exactly_on_the_anchor);
	g_test_add_func("/expo/open-is-identity", test_open_zoom_is_the_identity);
	g_test_add_func("/expo/zoom-monotonic", test_zoom_is_monotonic);
	g_test_add_func("/expo/hit-test", test_hit_test_follows_the_zoom);
	g_test_add_func("/expo/step-in-range",
	                test_stepping_stays_on_a_real_tile);
	g_test_add_func("/expo/step-no-wrap", test_stepping_does_not_wrap);
	g_test_add_func("/expo/refusals", test_refusals);

	return g_test_run();
}
