/*
 * scatter.c - spread the windows on the current tags one per tag:
 * the first to tag 1, the second to tag 2, ... (at most 9).
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run scatter
 * Undo it:  gather-app with a glob, or view several tags at once.
 */

#include <gowl/gowl.h>

#define MAX_TAGS (9)

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Put each visible window on a tag of its own (1..9)";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlMonitor *mon;
	GList *all;
	GList *l;
	guint i;

	mon = gowl_compositor_get_selected_monitor(gowl_macro_get_compositor(ctx));
	all = gowl_macro_list_clients(ctx, TRUE);
	i = 0;
	for (l = all; l != NULL && i < MAX_TAGS; l = l->next) {
		if (gowl_client_get_monitor(l->data) != (gpointer)mon)
			continue;
		/* Tags are a bitmask: tag N is 1 << (N - 1) */
		gowl_macro_move_client(ctx, l->data, NULL, 1u << i);
		i++;
	}
	g_list_free(all);
	return TRUE;
}
