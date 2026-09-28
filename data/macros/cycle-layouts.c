/*
 * cycle-layouts.c - step through YOUR list of layouts, not all of them.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run cycle-layouts tile monocle scrolling
 * Keybind:  "Super+Tab": { action: ipc-command,
 *                          arg: "macro-run cycle-layouts tile monocle scrolling" }
 *
 * Where it is in the list comes from the output's current layout
 * symbol matched against the names, so it picks up where you are even
 * after switching layouts some other way.
 */

#include <gowl/gowl.h>
#include <string.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Switch to the next of the LAYOUTS given";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	static guint position;
	GowlMonitor *mon;
	const gchar *symbol;
	guint argc;
	guint i;
	const gchar *next;

	argc = gowl_macro_get_argc(ctx);
	if (argc < 2) {
		gowl_macro_set_result(ctx, "usage: cycle-layouts LAYOUT LAYOUT...");
		return FALSE;
	}

	/* Find the current one in the list, by its symbol's name */
	mon = gowl_compositor_get_selected_monitor(gowl_macro_get_compositor(ctx));
	symbol = mon != NULL ? gowl_monitor_get_layout_symbol(mon) : NULL;
	for (i = 0; symbol != NULL && i < argc; i++) {
		g_autofree gchar *lower = g_ascii_strdown(symbol, -1);

		if (strstr(lower, gowl_macro_get_arg(ctx, i)) != NULL) {
			position = i;
			break;
		}
	}

	position = (position + 1) % argc;
	next = gowl_macro_get_arg(ctx, position);
	gowl_macro_action(ctx, GOWL_ACTION_SET_LAYOUT, next);
	gowl_macro_set_result(ctx, next);
	return TRUE;
}
