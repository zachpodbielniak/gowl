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
 * test-overlay-layout.c - the geometry the dropdown and the scratchpad
 * share: where an overlay panel sits on an output, and how the
 * scratchpad splits its panel between windows.
 *
 * Pure arithmetic.  The two properties that matter most are asserted
 * outright: the scratchpad is the dropdown's size from the other edge,
 * and every column of a split panel shares its top edge and height,
 * which is what lets N windows slide up as one panel.
 */

#include <glib.h>

#include "core/gowl-overlay-layout.h"

/* A 1920x1080 output under a 30px bar: its usable area. */
#define AX (0)
#define AY (30)
#define AW (1920)
#define AH (1050)

static void
test_panel_bottom_default(void)
{
	gint x, y, w, h;

	gowl_overlay_panel_box(AX, AY, AW, AH, 0.0, 0.0, 0, 0, 1, &x, &y, &w, &h);
	g_assert_cmpint(w, ==, AW);
	g_assert_cmpint(h, ==, 700);            /* two thirds of 1050 */
	g_assert_cmpint(x, ==, AX);
	g_assert_cmpint(y, ==, AY + AH - 700);  /* flush with the bottom */
}

static void
test_panel_same_space_as_the_dropdown(void)
{
	gint tx, ty, tw, th;
	gint bx, by, bw, bh;

	/* The dropdown is the top-anchored panel and the scratchpad the
	 * bottom-anchored one, with cmacs's dropdown fractions: the same
	 * rectangle, mirrored. */
	gowl_overlay_panel_box(AX, AY, AW, AH, 1.0, 0.666667, 0, 0, 0,
	                       &tx, &ty, &tw, &th);
	gowl_overlay_panel_box(AX, AY, AW, AH, 1.0, 0.666667, 0, 0, 1,
	                       &bx, &by, &bw, &bh);
	g_assert_cmpint(tw, ==, bw);
	g_assert_cmpint(th, ==, bh);
	g_assert_cmpint(tx, ==, bx);
	g_assert_cmpint(ty, ==, AY);
	g_assert_cmpint(by + bh, ==, AY + AH);
}

static void
test_panel_fractions_and_centring(void)
{
	gint x, y, w, h;

	gowl_overlay_panel_box(AX, AY, AW, AH, 0.5, 0.25, 0, 0, 1, &x, &y, &w, &h);
	g_assert_cmpint(w, ==, 960);
	g_assert_cmpint(h, ==, 262);            /* 262.5 truncates */
	g_assert_cmpint(x, ==, AX + (AW - 960) / 2);
	g_assert_cmpint(y, ==, AY + AH - 262);
}

static void
test_panel_absolute_size_wins(void)
{
	gint x, y, w, h;

	gowl_overlay_panel_box(AX, AY, AW, AH, 0.1, 0.1, 800, 300, 1,
	                       &x, &y, &w, &h);
	g_assert_cmpint(w, ==, 800);
	g_assert_cmpint(h, ==, 300);
	g_assert_cmpint(y, ==, AY + AH - 300);
}

static void
test_panel_clamped_to_the_output(void)
{
	gint x, y, w, h;

	gowl_overlay_panel_box(AX, AY, AW, AH, 2.0, 3.0, 0, 0, 1, &x, &y, &w, &h);
	g_assert_cmpint(w, ==, AW);
	g_assert_cmpint(h, ==, AH);
	gowl_overlay_panel_box(AX, AY, AW, AH, 0.0, 0.0, 5000, 5000, 1,
	                       &x, &y, &w, &h);
	g_assert_cmpint(w, ==, AW);
	g_assert_cmpint(h, ==, AH);
	/* An output with no size yet still yields a real rectangle. */
	gowl_overlay_panel_box(0, 0, 0, 0, 0.0, 0.0, 0, 0, 1, &x, &y, &w, &h);
	g_assert_cmpint(w, ==, 1);
	g_assert_cmpint(h, ==, 1);
}

static void
test_panel_side_anchors(void)
{
	gint x, y, w, h;

	gowl_overlay_panel_box(AX, AY, AW, AH, 0.25, 0.5, 0, 0, 2, &x, &y, &w, &h);
	g_assert_cmpint(x, ==, AX);
	g_assert_cmpint(y, ==, AY + (AH - h) / 2);
	gowl_overlay_panel_box(AX, AY, AW, AH, 0.25, 0.5, 0, 0, 3, &x, &y, &w, &h);
	g_assert_cmpint(x + w, ==, AX + AW);
	g_assert_cmpint(y, ==, AY + (AH - h) / 2);
}

static void
test_columns_one_window_fills_the_panel(void)
{
	gint x, y, w, h;

	g_assert_true(gowl_overlay_tile_columns(0, 380, 1920, 700, 1, 0, 12,
	                                        &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 0);
	g_assert_cmpint(y, ==, 380);
	g_assert_cmpint(w, ==, 1920);
	g_assert_cmpint(h, ==, 700);
}

static void
test_columns_fill_the_panel_exactly(void)
{
	static const gint gaps[] = { 0, 7, 13 };
	guint g;
	guint n;
	guint i;

	/* For every split the columns abut at exactly the gap and the last
	 * one ends on the panel's right edge: no sliver, no overlap. */
	for (g = 0; g < G_N_ELEMENTS(gaps); g++) {
		for (n = 1; n <= 7; n++) {
			gint right = 0;

			for (i = 0; i < n; i++) {
				gint x, y, w, h;

				g_assert_true(gowl_overlay_tile_columns(
					0, 380, 1920, 700, n, i, gaps[g], &x, &y, &w, &h));
				g_assert_cmpint(w, >=, 1);
				if (i == 0)
					g_assert_cmpint(x, ==, 0);
				else
					g_assert_cmpint(x, ==, right + gaps[g]);
				right = x + w;
			}
			g_assert_cmpint(right, ==, 1920);
		}
	}
}

static void
test_columns_share_the_top_edge_and_height(void)
{
	guint n;
	guint i;

	/* What makes N windows slide as one panel: the animation moves each
	 * from the output's bottom edge to its own top edge, so an equal top
	 * edge and height mean equal motion. */
	for (n = 1; n <= 6; n++) {
		for (i = 0; i < n; i++) {
			gint x, y, w, h;

			g_assert_true(gowl_overlay_tile_columns(
				100, 380, 1720, 700, n, i, 10, &x, &y, &w, &h));
			g_assert_cmpint(y, ==, 380);
			g_assert_cmpint(h, ==, 700);
		}
	}
}

static void
test_columns_widths_differ_by_at_most_a_pixel(void)
{
	gint  x, y, w, h;
	gint  narrowest;
	gint  widest;
	guint i;

	/* 1920 less two 10px gaps is 1900 over three: 634, 633, 633. */
	narrowest = G_MAXINT;
	widest = 0;
	for (i = 0; i < 3; i++) {
		g_assert_true(gowl_overlay_tile_columns(0, 0, 1920, 700, 3, i, 10,
		                                        &x, &y, &w, &h));
		narrowest = MIN(narrowest, w);
		widest = MAX(widest, w);
	}
	g_assert_cmpint(narrowest, ==, 633);
	g_assert_cmpint(widest, ==, 634);
}

static void
test_columns_gap_gives_way(void)
{
	gint  x, y, w, h;
	guint i;

	/* Four windows in a 10px panel with a 50px gap: the gap shrinks so
	 * every column keeps a pixel and the last still ends inside. */
	for (i = 0; i < 4; i++) {
		g_assert_true(gowl_overlay_tile_columns(0, 0, 10, 10, 4, i, 50,
		                                        &x, &y, &w, &h));
		g_assert_cmpint(w, >=, 1);
		g_assert_cmpint(x + w, <=, 10);
	}
	/* A negative gap is no gap. */
	g_assert_true(gowl_overlay_tile_columns(0, 0, 100, 10, 2, 1, -5,
	                                        &x, &y, &w, &h));
	g_assert_cmpint(x, ==, 50);
	g_assert_cmpint(w, ==, 50);
}

static void
test_columns_out_of_range(void)
{
	gint x = -1;
	gint y = -1;
	gint w = -1;
	gint h = -1;

	g_assert_false(gowl_overlay_tile_columns(0, 0, 100, 100, 0, 0, 0,
	                                         &x, &y, &w, &h));
	g_assert_false(gowl_overlay_tile_columns(0, 0, 100, 100, 2, 2, 0,
	                                         &x, &y, &w, &h));
	g_assert_cmpint(x, ==, -1);
	g_assert_cmpint(y, ==, -1);
	g_assert_cmpint(w, ==, -1);
	g_assert_cmpint(h, ==, -1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/overlay-layout/panel/bottom-default",
	                test_panel_bottom_default);
	g_test_add_func("/overlay-layout/panel/same-space-as-the-dropdown",
	                test_panel_same_space_as_the_dropdown);
	g_test_add_func("/overlay-layout/panel/fractions-and-centring",
	                test_panel_fractions_and_centring);
	g_test_add_func("/overlay-layout/panel/absolute-size-wins",
	                test_panel_absolute_size_wins);
	g_test_add_func("/overlay-layout/panel/clamped-to-the-output",
	                test_panel_clamped_to_the_output);
	g_test_add_func("/overlay-layout/panel/side-anchors",
	                test_panel_side_anchors);
	g_test_add_func("/overlay-layout/columns/one-window-fills-the-panel",
	                test_columns_one_window_fills_the_panel);
	g_test_add_func("/overlay-layout/columns/fill-the-panel-exactly",
	                test_columns_fill_the_panel_exactly);
	g_test_add_func("/overlay-layout/columns/share-the-top-edge-and-height",
	                test_columns_share_the_top_edge_and_height);
	g_test_add_func("/overlay-layout/columns/widths-differ-by-at-most-a-pixel",
	                test_columns_widths_differ_by_at_most_a_pixel);
	g_test_add_func("/overlay-layout/columns/gap-gives-way",
	                test_columns_gap_gives_way);
	g_test_add_func("/overlay-layout/columns/out-of-range",
	                test_columns_out_of_range);

	return g_test_run();
}
