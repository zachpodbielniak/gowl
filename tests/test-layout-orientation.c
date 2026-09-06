/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
#include "interfaces/gowl-layout-provider.h"

/* Exercise the real plugins without a renderer. Only the final placement
 * and arrange scheduling are intercepted; selection and geometry are real. */
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	client->geom = (struct wlr_box){x, y, width, height};
}

void
gowl_compositor_arrange(GowlCompositor *self, GowlMonitor *monitor)
{
	gowl_layout_apply(self, monitor);
}

static void
test_orientation(void)
{
	const gchar *names[] = {"tile", "scrolling", "centeredmaster", "fibonacci", "monocle"};
	GowlCompositor *comp = gowl_compositor_new();
	GowlMonitor *landscape = g_object_new(GOWL_TYPE_MONITOR, NULL);
	GowlMonitor *portrait = g_object_new(GOWL_TYPE_MONITOR, NULL);
	GowlClient clients[6] = {0};
	guint i, layout, pass;

	comp->module_mgr = gowl_module_manager_new();
	comp->selmon = landscape; /* Portrait must not use the selected monitor. */
	landscape->compositor = portrait->compositor = comp;
	landscape->m = landscape->w = (struct wlr_box){-1200, -100, 1200, 800};
	portrait->m = portrait->w = (struct wlr_box){0, 50, 800, 900};
	landscape->tagset[0] = portrait->tagset[0] = 1;
	landscape->nmaster = portrait->nmaster = 1;
	landscape->mfact = portrait->mfact = 0.5;
	for (i = 0; i < G_N_ELEMENTS(clients); i++) {
		clients[i].mon = i < 3 ? landscape : portrait;
		clients[i].tags = 1;
		clients[i].compositor = comp;
		comp->clients = g_list_append(comp->clients, &clients[i]);
	}
	gowl_layout_registry_init(comp);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		gchar *path = g_strdup_printf("%s/%s.so", GOWL_TEST_LAYOUT_MODULE_DIR, names[i]);
		g_assert_true(gowl_module_manager_load_module(comp->module_mgr, path, NULL));
		g_free(path);
	}
	gowl_module_manager_activate_all(comp->module_mgr);
	gowl_layout_adopt_providers(comp);

	for (layout = 0; layout < G_N_ELEMENTS(names); layout++) {
		g_assert_true(gowl_layout_set(comp, landscape, names[layout]));
		g_assert_true(gowl_layout_set(comp, portrait, names[layout]));
		/* Repeat portrait -> square -> landscape -> portrait on the same
		 * monitor and provider, without reloading or restarting anything. */
		for (pass = 0; pass < 4; pass++) {
			gboolean vertical = pass == 0 || pass == 3;
			portrait->m.height = vertical ? 900 : pass == 1 ? 800 : 600;
			portrait->w = portrait->m;
			/* Usable area is deliberately tall on a square full output. */
			if (pass == 1) portrait->w.width -= 100;
			portrait->scroll_x = 0;
			gowl_layout_apply(comp, landscape);
			gowl_layout_apply(comp, portrait);
			if (layout == 0 || layout == 3) {
				g_assert_cmpint(clients[0].geom.height, ==, landscape->w.height);
				if (vertical) {
					g_assert_cmpint(clients[3].geom.width, ==, portrait->w.width);
					g_assert_cmpint(clients[4].geom.y, >, clients[3].geom.y);
				} else {
					g_assert_cmpint(clients[3].geom.height, ==, portrait->w.height);
					g_assert_cmpint(clients[4].geom.x, >, clients[3].geom.x);
				}
			} else if (layout == 1) {
				GowlLayoutProvider *provider = GOWL_LAYOUT_PROVIDER(
					gowl_module_manager_find_module(comp->module_mgr, "scrolling"));
				if (vertical) {
					g_assert_cmpint(clients[3].geom.width, ==, portrait->w.width);
					g_assert_cmpint(clients[5].geom.y, >, clients[4].geom.y);
				} else {
					g_assert_cmpint(clients[3].geom.height, ==, portrait->w.height);
					g_assert_cmpint(clients[5].geom.x, >, clients[4].geom.x);
				}
				GOWL_LAYOUT_PROVIDER_GET_IFACE(provider)->focus_client(provider, &clients[5]);
				g_assert_cmpint(portrait->scroll_x, >, 0);
				g_assert_cmpint(landscape->scroll_x, ==, 0);
				g_assert_cmpint(clients[5].geom.x, >=, portrait->w.x);
				g_assert_cmpint(clients[5].geom.y, >=, portrait->w.y);
				g_assert_cmpint(clients[5].geom.x + clients[5].geom.width, <=, portrait->w.x + portrait->w.width);
				g_assert_cmpint(clients[5].geom.y + clients[5].geom.height, <=, portrait->w.y + portrait->w.height);
			} else if (layout == 2) {
				if (vertical) {
					g_assert_cmpint(clients[4].geom.y, <, clients[3].geom.y);
					g_assert_cmpint(clients[5].geom.y, >, clients[3].geom.y);
					g_assert_cmpint(clients[3].geom.width, ==, portrait->w.width);
				} else {
					g_assert_cmpint(clients[4].geom.x, <, clients[3].geom.x);
					g_assert_cmpint(clients[5].geom.x, >, clients[3].geom.x);
				}
			} else {
				g_assert_true(wlr_box_equal(&clients[3].geom, &portrait->w));
			}
		}
	}
	g_list_free(comp->clients);
	comp->clients = NULL;
	gowl_layout_registry_finish(comp);
	g_clear_object(&comp->module_mgr);
	comp->selmon = NULL;
	g_object_unref(portrait);
	g_object_unref(landscape);
	g_object_unref(comp);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/layout/orientation/live-monitor-local", test_orientation);
	return g_test_run();
}
