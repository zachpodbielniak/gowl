/*
 * gather-app.c - pull every window of one application onto the tags
 * you are looking at.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run gather-app 'org.gnome.Nautilus'
 *           gowl-msg macro-run gather-app '*firefox*'
 * The argument is an app-id glob (* and ?).
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Bring every window whose app-id matches GLOB to the current tags";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlCompositor *comp;
	GowlMonitor *here;
	const gchar *glob;
	GList *all;
	GList *l;
	guint moved;
	g_autofree gchar *msg = NULL;

	glob = gowl_macro_get_arg(ctx, 0);
	if (glob == NULL) {
		gowl_macro_set_result(ctx, "usage: gather-app APP-ID-GLOB");
		return FALSE;
	}
	comp = gowl_macro_get_compositor(ctx);
	here = gowl_compositor_get_selected_monitor(comp);

	moved = 0;
	all = gowl_macro_list_clients(ctx, FALSE);
	for (l = all; l != NULL; l = l->next) {
		const gchar *app = gowl_client_get_app_id(l->data);

		if (app == NULL || !g_pattern_match_simple(glob, app))
			continue;
		/* monitor + the monitor's current view (tags 0) */
		gowl_macro_move_client(ctx, l->data, here,
		                       here != NULL ? gowl_monitor_get_tags(here) : 0);
		moved++;
	}
	g_list_free(all);

	msg = g_strdup_printf("gathered %u window(s)", moved);
	gowl_macro_set_result(ctx, msg);
	return TRUE;
}
