/*
 * swap-monitors.c - swap the visible windows of the first two outputs.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run swap-monitors
 * With one output it says so and does nothing.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Swap the visible windows of the first two outputs";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GList *monitors;
	GowlMonitor *a;
	GowlMonitor *b;
	GList *on_a;
	GList *on_b;
	GList *all;
	GList *l;

	monitors = gowl_compositor_get_monitors(gowl_macro_get_compositor(ctx));
	if (g_list_length(monitors) < 2) {
		gowl_macro_set_result(ctx, "only one output");
		return TRUE;
	}
	a = monitors->data;
	b = monitors->next->data;

	/* Decide first, move after: moving changes what is visible. */
	on_a = NULL;
	on_b = NULL;
	all = gowl_macro_list_clients(ctx, TRUE);
	for (l = all; l != NULL; l = l->next) {
		if (gowl_client_get_monitor(l->data) == (gpointer)a)
			on_a = g_list_prepend(on_a, l->data);
		else if (gowl_client_get_monitor(l->data) == (gpointer)b)
			on_b = g_list_prepend(on_b, l->data);
	}
	g_list_free(all);

	for (l = on_a; l != NULL; l = l->next)
		gowl_macro_move_client(ctx, l->data, b, 0);
	for (l = on_b; l != NULL; l = l->next)
		gowl_macro_move_client(ctx, l->data, a, 0);
	g_list_free(on_a);
	g_list_free(on_b);
	return TRUE;
}
