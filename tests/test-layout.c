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

#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
#include "../modules/tile/gowl-layout-tile.h"
#include "../modules/monocle/gowl-layout-monocle.h"
#include "../modules/float/gowl-layout-float.h"

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

static void
layout_changed_count(GowlMonitor *monitor, gpointer data)
{
 guint *count = data;
 (*count)++;
}

static void
test_layout_plugins_and_tags(void)
{
 GowlCompositor *comp = gowl_compositor_new();
 GowlMonitor *mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
 const gchar *names[] = { "tile", "monocle", "float", "scrolling" };
 guint i, notifications = 0;
 comp->module_mgr = gowl_module_manager_new();
 comp->selmon = mon;
 mon->tagset[mon->seltags] = 1;
 gowl_layout_registry_init(comp);
 g_assert_null(gowl_layout_get(comp, mon));
 for (i = 0; i < G_N_ELEMENTS(names); i++) {
  gchar *path = g_strdup_printf("%s/%s.so", GOWL_TEST_LAYOUT_MODULE_DIR, names[i]);
  g_assert_true(gowl_module_manager_load_module(comp->module_mgr, path, NULL));
  g_free(path);
 }
 gowl_module_manager_activate_all(comp->module_mgr);
 gowl_layout_adopt_providers(comp);
 g_assert_cmpstr(gowl_layout_get(comp, mon)->name, ==, "tile");
 g_signal_connect(mon, "layout-changed", G_CALLBACK(layout_changed_count), &notifications);
 gowl_layout_apply(comp, mon);
 g_assert_cmpuint(notifications, ==, 0);
 g_assert_true(gowl_layout_set(comp, mon, "scrolling"));
 gowl_layout_apply(comp, mon);
 g_assert_cmpuint(notifications, ==, 1);
 gowl_layout_apply(comp, mon);
 g_assert_cmpuint(notifications, ==, 1);
 mon->scroll_x = 450;
 mon->tagset[mon->seltags] = 2;
 g_assert_cmpstr(gowl_layout_get(comp, mon)->name, ==, "tile");
 g_assert_cmpint(mon->scroll_x, ==, 0);
 g_assert_cmpstr(gowl_layout_cycle(comp, mon, 1), ==, "monocle");
 mon->tagset[mon->seltags] = 1;
 g_assert_cmpstr(gowl_layout_get(comp, mon)->name, ==, "scrolling");
 g_assert_cmpint(mon->scroll_x, ==, 450);
 gowl_module_deactivate(gowl_module_manager_find_module(comp->module_mgr, "scrolling"));
 g_assert_cmpstr(gowl_layout_get(comp, mon)->name, ==, "tile");
 gowl_module_activate(gowl_module_manager_find_module(comp->module_mgr, "scrolling"));
 g_assert_cmpstr(gowl_layout_get(comp, mon)->name, ==, "scrolling");
 g_assert_cmpstr(gowl_layout_cycle(comp, mon, 1), ==, "tile");
 g_assert_cmpstr(gowl_layout_cycle(comp, mon, -1), ==, "scrolling");
 mon->tagset[mon->seltags] = 2;
 g_assert_cmpstr(gowl_layout_get(comp, mon)->name, ==, "monocle");
 gowl_layout_registry_finish(comp);
 g_clear_object(&comp->module_mgr);
 comp->selmon = NULL;
 g_object_unref(mon);
 g_object_unref(comp);
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

	g_test_add_func("/layout/plugins-and-tags", test_layout_plugins_and_tags);
	return g_test_run();
}
