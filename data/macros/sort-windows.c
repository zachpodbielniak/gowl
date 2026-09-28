/*
 * sort-windows.c - sort the tiled windows on the focused output's
 * current tags alphabetically by title.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:     gowl-msg macro-run sort-windows          (A..Z)
 *             gowl-msg macro-run sort-windows reverse  (Z..A)
 * Keybind:    "Super+Shift+s": { action: ipc-command, arg: "macro-run sort-windows" }
 *
 * The client list IS the tiling order, so sorting windows is: collect
 * the ones on screen, sort them, and hand the order back.  Only the
 * sorted windows move; everything else keeps its slot.
 */

#include <gowl/gowl.h>

static gboolean reverse;

/* Case-insensitive, locale-aware, by title. */
static gint
by_title(
	gconstpointer a,
	gconstpointer b
){
	g_autofree gchar *ta = NULL;
	g_autofree gchar *tb = NULL;
	const gchar *sa;
	const gchar *sb;
	gint cmp;

	sa = gowl_client_get_title((GowlClient *)a);
	sb = gowl_client_get_title((GowlClient *)b);
	ta = g_utf8_casefold(sa != NULL ? sa : "", -1);
	tb = g_utf8_casefold(sb != NULL ? sb : "", -1);
	cmp = g_utf8_collate(ta, tb);
	return reverse ? -cmp : cmp;
}

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Sort the visible tiled windows by title (arg: reverse)";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlCompositor *comp;
	GowlMonitor *mon;
	GList *all;
	GList *mine;
	GList *l;
	g_autofree gchar *msg = NULL;

	comp = gowl_macro_get_compositor(ctx);
	mon = gowl_compositor_get_selected_monitor(comp);
	reverse = g_strcmp0(gowl_macro_get_arg(ctx, 0), "reverse") == 0;

	/* The tiled windows showing on the focused output */
	all = gowl_macro_list_clients(ctx, TRUE);
	mine = NULL;
	for (l = all; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (gowl_client_get_monitor(c) == (gpointer)mon
		    && !gowl_client_get_floating(c))
			mine = g_list_append(mine, c);
	}
	g_list_free(all);

	gowl_macro_sort_clients(ctx, mine, by_title);
	msg = g_strdup_printf("sorted %u windows", g_list_length(mine));
	gowl_macro_set_result(ctx, msg);
	g_list_free(mine);
	return TRUE;
}
