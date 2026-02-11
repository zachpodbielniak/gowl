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

#include "layout/gowl-layout-tile.h"
#include "layout/gowl-layout-monocle.h"
#include "layout/gowl-layout-float.h"

/* ---- Tile layout tests ---- */

static void
test_tile_single_client(void)
{
	gint pos[4];

	gowl_layout_tile(1, 1, 0.55, 0, 0, 1920, 1080, pos);

	/* Single client in master takes full width */
	g_assert_cmpint(pos[0], ==, 0);
	g_assert_cmpint(pos[1], ==, 0);
	g_assert_cmpint(pos[2], ==, 1920);
	g_assert_cmpint(pos[3], ==, 1080);
}

static void
test_tile_two_clients(void)
{
	gint pos[8];

	gowl_layout_tile(2, 1, 0.55, 0, 0, 1000, 1000, pos);

	/* Master takes 55% width */
	g_assert_cmpint(pos[0], ==, 0);    /* master x */
	g_assert_cmpint(pos[1], ==, 0);    /* master y */
	g_assert_cmpint(pos[2], ==, 550);  /* master w = 1000 * 0.55 */
	g_assert_cmpint(pos[3], ==, 1000); /* master h */

	/* Stack takes remaining 45% */
	g_assert_cmpint(pos[4], ==, 550);  /* stack x */
	g_assert_cmpint(pos[5], ==, 0);    /* stack y */
	g_assert_cmpint(pos[6], ==, 450);  /* stack w */
	g_assert_cmpint(pos[7], ==, 1000); /* stack h */
}

static void
test_tile_three_clients(void)
{
	gint pos[12];

	gowl_layout_tile(3, 1, 0.5, 0, 0, 1000, 1000, pos);

	/* Master: full height, 50% width */
	g_assert_cmpint(pos[0], ==, 0);
	g_assert_cmpint(pos[2], ==, 500);
	g_assert_cmpint(pos[3], ==, 1000);

	/* Stack client 1: top half of right side */
	g_assert_cmpint(pos[4], ==, 500);
	g_assert_cmpint(pos[5], ==, 0);
	g_assert_cmpint(pos[6], ==, 500);
	g_assert_cmpint(pos[7], ==, 500);

	/* Stack client 2: bottom half of right side */
	g_assert_cmpint(pos[8], ==, 500);
	g_assert_cmpint(pos[9], ==, 500);
	g_assert_cmpint(pos[10], ==, 500);
	g_assert_cmpint(pos[11], ==, 500);
}

static void
test_tile_two_masters(void)
{
	gint pos[12];

	gowl_layout_tile(3, 2, 0.5, 0, 0, 1000, 1000, pos);

	/* Master 1: top half of left side */
	g_assert_cmpint(pos[0], ==, 0);
	g_assert_cmpint(pos[1], ==, 0);
	g_assert_cmpint(pos[2], ==, 500);
	g_assert_cmpint(pos[3], ==, 500);

	/* Master 2: bottom half of left side */
	g_assert_cmpint(pos[4], ==, 0);
	g_assert_cmpint(pos[5], ==, 500);
	g_assert_cmpint(pos[6], ==, 500);
	g_assert_cmpint(pos[7], ==, 500);

	/* Stack: full height of right side */
	g_assert_cmpint(pos[8], ==, 500);
	g_assert_cmpint(pos[9], ==, 0);
	g_assert_cmpint(pos[10], ==, 500);
	g_assert_cmpint(pos[11], ==, 1000);
}

static void
test_tile_with_offset(void)
{
	gint pos[4];

	gowl_layout_tile(1, 1, 0.55, 100, 50, 800, 600, pos);

	g_assert_cmpint(pos[0], ==, 100);
	g_assert_cmpint(pos[1], ==, 50);
	g_assert_cmpint(pos[2], ==, 800);
	g_assert_cmpint(pos[3], ==, 600);
}

static void
test_tile_zero_clients(void)
{
	/* Should not crash */
	gowl_layout_tile(0, 1, 0.55, 0, 0, 1920, 1080, NULL);
}

static void
test_tile_symbol(void)
{
	g_assert_cmpstr(gowl_layout_tile_symbol(), ==, "[]=");
}

/* ---- Monocle layout tests ---- */

static void
test_monocle_single(void)
{
	gint pos[4];

	gowl_layout_monocle(1, 0, 0, 1920, 1080, pos);

	g_assert_cmpint(pos[0], ==, 0);
	g_assert_cmpint(pos[1], ==, 0);
	g_assert_cmpint(pos[2], ==, 1920);
	g_assert_cmpint(pos[3], ==, 1080);
}

static void
test_monocle_multiple(void)
{
	gint pos[12];

	gowl_layout_monocle(3, 10, 20, 800, 600, pos);

	/* All clients get the same geometry */
	g_assert_cmpint(pos[0], ==, 10);
	g_assert_cmpint(pos[1], ==, 20);
	g_assert_cmpint(pos[2], ==, 800);
	g_assert_cmpint(pos[3], ==, 600);

	g_assert_cmpint(pos[4], ==, 10);
	g_assert_cmpint(pos[5], ==, 20);
	g_assert_cmpint(pos[6], ==, 800);
	g_assert_cmpint(pos[7], ==, 600);

	g_assert_cmpint(pos[8], ==, 10);
	g_assert_cmpint(pos[9], ==, 20);
	g_assert_cmpint(pos[10], ==, 800);
	g_assert_cmpint(pos[11], ==, 600);
}

static void
test_monocle_symbol(void)
{
	g_assert_cmpstr(gowl_layout_monocle_symbol(), ==, "[M]");
}

/* ---- Float layout tests ---- */

static void
test_float_symbol(void)
{
	g_assert_cmpstr(gowl_layout_float_symbol(), ==, "><>");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Tile */
	g_test_add_func("/layout/tile/single-client", test_tile_single_client);
	g_test_add_func("/layout/tile/two-clients", test_tile_two_clients);
	g_test_add_func("/layout/tile/three-clients", test_tile_three_clients);
	g_test_add_func("/layout/tile/two-masters", test_tile_two_masters);
	g_test_add_func("/layout/tile/with-offset", test_tile_with_offset);
	g_test_add_func("/layout/tile/zero-clients", test_tile_zero_clients);
	g_test_add_func("/layout/tile/symbol", test_tile_symbol);

	/* Monocle */
	g_test_add_func("/layout/monocle/single", test_monocle_single);
	g_test_add_func("/layout/monocle/multiple", test_monocle_multiple);
	g_test_add_func("/layout/monocle/symbol", test_monocle_symbol);

	/* Float */
	g_test_add_func("/layout/float/symbol", test_float_symbol);

	return g_test_run();
}
