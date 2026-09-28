/*
 * sort-by-app.c - group the visible tiled windows by application, then
 * by title within each application.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run sort-by-app
 */

#include <gowl/gowl.h>

static gint
cmp_str(
	const gchar *a,
	const gchar *b
){
	g_autofree gchar *fa = g_utf8_casefold(a != NULL ? a : "", -1);
	g_autofree gchar *fb = g_utf8_casefold(b != NULL ? b : "", -1);

	return g_utf8_collate(fa, fb);
}

/* app-id first, title to break ties */
static gint
by_app_then_title(
	gconstpointer a,
	gconstpointer b
){
	gint c;

	c = cmp_str(gowl_client_get_app_id((GowlClient *)a),
	            gowl_client_get_app_id((GowlClient *)b));
	if (c != 0)
		return c;
	return cmp_str(gowl_client_get_title((GowlClient *)a),
	               gowl_client_get_title((GowlClient *)b));
}

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Group the visible tiled windows by app-id, then title";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GList *all;
	GList *tiled;
	GList *l;

	all = gowl_macro_list_clients(ctx, TRUE);
	tiled = NULL;
	for (l = all; l != NULL; l = l->next)
		if (!gowl_client_get_floating(l->data))
			tiled = g_list_append(tiled, l->data);
	g_list_free(all);

	gowl_macro_sort_clients(ctx, tiled, by_app_then_title);
	g_list_free(tiled);
	return TRUE;
}
