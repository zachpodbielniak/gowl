/*
 * screenshot-annotated.c - screenshot the focused window, and say which
 * window it was.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run screenshot-annotated
 * Needs the screenshot module; the module chooses where the file goes.
 *
 * Shows chaining: a macro can use any module's IPC words, here
 * `screenshot-window', and read the reply (gowl_macro_run_command runs
 * NOW, unlike the queued gowl_macro_command).
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Screenshot the focused window and notify with its title";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlClient *c;
	const gchar *title;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *body = NULL;

	c = gowl_compositor_get_focused_client(gowl_macro_get_compositor(ctx));
	title = c != NULL ? gowl_client_get_title(c) : NULL;

	reply = gowl_macro_run_command(ctx, "screenshot-window");
	if (reply == NULL) {
		gowl_macro_set_result(ctx, "the screenshot module is not loaded");
		return FALSE;
	}
	body = g_strdup_printf("%s -- %s", title != NULL ? title : "(no title)",
	                       reply);
	gowl_macro_notify(ctx, "Window captured", body);
	gowl_macro_set_result(ctx, reply);
	return TRUE;
}
