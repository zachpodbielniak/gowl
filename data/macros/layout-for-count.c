/*
 * layout-for-count.c - pick the layout from how many windows are on
 * screen: one window full-size, a few tiled, many in a grid.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it on every window change -- in `modules: macro:':
 *     triggers:
 *       - "client-added: layout-for-count"
 *       - "client-removed: layout-for-count"
 * or by hand: gowl-msg macro-run layout-for-count
 *
 * The layouts named must be loaded modules (tile, monocle and grid
 * ship with gowl).
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "monocle for 1 window, tile for 2-4, grid for 5+";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlMonitor *mon;
	GList *all;
	GList *l;
	guint n;
	const gchar *layout;

	mon = gowl_compositor_get_selected_monitor(gowl_macro_get_compositor(ctx));
	n = 0;
	all = gowl_macro_list_clients(ctx, TRUE);
	for (l = all; l != NULL; l = l->next)
		if (gowl_client_get_monitor(l->data) == (gpointer)mon
		    && !gowl_client_get_floating(l->data))
			n++;
	g_list_free(all);

	if (n <= 1)
		layout = "monocle";
	else if (n <= 4)
		layout = "tile";
	else
		layout = "grid";

	gowl_macro_action(ctx, GOWL_ACTION_SET_LAYOUT, layout);
	gowl_macro_set_result(ctx, layout);
	return TRUE;
}
