/*
 * kill-by-title.c - close every window whose title matches a glob.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run kill-by-title '*Picture-in-Picture*'
 *           gowl-msg macro-run kill-by-title '*.pdf - Document Viewer'
 *
 * Closing is a polite request (the window may ask to save), the same
 * as the kill-client action.  Nothing is closed without a pattern.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Close every window whose title matches GLOB";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *glob;
	GList *all;
	GList *l;
	guint closed;
	g_autofree gchar *msg = NULL;

	glob = gowl_macro_get_arg(ctx, 0);
	if (glob == NULL || *glob == '\0') {
		gowl_macro_set_result(ctx, "usage: kill-by-title GLOB");
		return FALSE;
	}

	closed = 0;
	all = gowl_macro_list_clients(ctx, FALSE);
	for (l = all; l != NULL; l = l->next) {
		const gchar *title = gowl_client_get_title(l->data);

		if (title != NULL && g_pattern_match_simple(glob, title)) {
			gowl_client_close(l->data);
			closed++;
		}
	}
	g_list_free(all);

	msg = g_strdup_printf("asked %u window(s) to close", closed);
	gowl_macro_set_result(ctx, msg);
	return TRUE;
}
