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
 * test-tabbed.c - The tabbed layout's geometry.
 *
 * Every tiled window on the tag gets the area below the strip, the
 * strip's height comes from the module's setting, the symbol counts
 * the tabs, floating and other-tag windows are left alone, and a
 * monitor that is all strip is not possible (the strip is capped at
 * half the area).  Run on a compositor without a scene, like the
 * orientation test: the strip is not drawn, the arithmetic is.
 */

#include <glib.h>
#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
#include "interfaces/gowl-layout-provider.h"
#include "module/gowl-module-manager.h"

#ifndef GOWL_TEST_LAYOUT_MODULE_DIR
#define GOWL_TEST_LAYOUT_MODULE_DIR "build/release/modules"
#endif

/* As the orientation test does: the real plugin, without a renderer.
 * Only the final placement and arrange scheduling are intercepted. */
void gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                                  gint x, gint y, gint width, gint height);
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	(void)self;
	client->geom = (struct wlr_box){x, y, width, height};
}

void gowl_compositor_arrange(GowlCompositor *self, GowlMonitor *monitor);
void
gowl_compositor_arrange(GowlCompositor *self, GowlMonitor *monitor)
{
	gowl_layout_apply(self, monitor);
}

static void
test_tabbed_geometry(void)
{
	GowlCompositor *comp = gowl_compositor_new();
	GowlMonitor *mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	GowlClient clients[4] = {0};
	GowlModule *tabbed;
	GHashTable *settings;
	gchar *path;
	guint i;

	comp->module_mgr = gowl_module_manager_new();
	comp->selmon = mon;
	mon->compositor = comp;
	mon->m = mon->w = (struct wlr_box){100, 50, 1200, 800};
	mon->tagset[0] = 1;
	for (i = 0; i < G_N_ELEMENTS(clients); i++) {
		clients[i].mon = mon;
		clients[i].tags = 1;
		clients[i].compositor = comp;
		comp->clients = g_list_append(comp->clients, &clients[i]);
	}
	clients[2].isfloating = TRUE;  /* left where it is */
	clients[3].tags = 2;           /* another tag: untouched */

	gowl_layout_registry_init(comp);
	path = g_strdup_printf("%s/tabbed.so", GOWL_TEST_LAYOUT_MODULE_DIR);
	g_assert_true(gowl_module_manager_load_module(comp->module_mgr, path, NULL));
	g_free(path);
	gowl_module_manager_activate_all(comp->module_mgr);
	gowl_layout_adopt_providers(comp);
	tabbed = gowl_module_manager_find_module(comp->module_mgr, "tabbed");
	g_assert_nonnull(tabbed);

	g_assert_true(gowl_layout_set(comp, mon, "tabbed"));
	gowl_layout_apply(comp, mon);

	/* Two tabs; both windows fill the area under a 26px strip. */
	g_assert_cmpstr(mon->layout_symbol, ==, "[T2]");
	for (i = 0; i < 2; i++) {
		g_assert_cmpint(clients[i].geom.x, ==, 100);
		g_assert_cmpint(clients[i].geom.y, ==, 50 + 26);
		g_assert_cmpint(clients[i].geom.width, ==, 1200);
		g_assert_cmpint(clients[i].geom.height, ==, 800 - 26);
	}
	g_assert_cmpint(clients[2].geom.width, ==, 0);
	g_assert_cmpint(clients[3].geom.width, ==, 0);

	/* A taller strip from the setting, capped at half the area. */
	settings = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(settings, (gpointer)"height", (gpointer)"40");
	gowl_module_configure(tabbed, settings);
	gowl_layout_apply(comp, mon);
	g_assert_cmpint(clients[0].geom.y, ==, 50 + 40);
	g_assert_cmpint(clients[0].geom.height, ==, 800 - 40);
	mon->w.height = 60;
	gowl_layout_apply(comp, mon);
	g_assert_cmpint(clients[0].geom.y, ==, 50 + 30);
	g_assert_cmpint(clients[0].geom.height, ==, 30);
	g_hash_table_unref(settings);

	/* The symbol is the module's. */
	g_assert_cmpstr(gowl_layout_provider_get_symbol(
		GOWL_LAYOUT_PROVIDER(tabbed)), ==, "[T]");

	g_list_free(comp->clients);
	comp->clients = NULL;
	comp->selmon = NULL;
	g_object_unref(comp);
	g_object_unref(mon);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tabbed/geometry", test_tabbed_geometry);
	return g_test_run();
}
