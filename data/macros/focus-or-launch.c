/*
 * focus-or-launch.c - jump to an application if it is running, start
 * it if it is not.  The "raise or run" every window manager grows.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run focus-or-launch app-id:firefox firefox
 *           gowl-msg macro-run focus-or-launch 'title:*WoW*' lutris lutris:rungame/wow
 * Keybind:  "Super+b": { action: ipc-command,
 *                        arg: "macro-run focus-or-launch app-id:firefox firefox" }
 * Pedal:    KEY_B: { macro: focus-or-launch, args: "'title:World of Warcraft*' lutris" }
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Focus the window matching PATTERN, or run COMMAND...";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *pattern;
	GowlClient *c;
	g_autofree gchar *command = NULL;

	pattern = gowl_macro_get_arg(ctx, 0);
	if (pattern == NULL || gowl_macro_get_argc(ctx) < 2) {
		gowl_macro_set_result(ctx, "usage: focus-or-launch PATTERN COMMAND...");
		return FALSE;
	}

	c = gowl_macro_find_client(ctx, pattern);
	if (c != NULL) {
		/* Views its tags on its monitor and focuses it */
		gowl_macro_focus(ctx, c);
		gowl_macro_set_result(ctx, "focused");
		return TRUE;
	}

	command = g_strjoinv(" ", (gchar **)gowl_macro_get_argv(ctx) + 1);
	gowl_macro_action(ctx, GOWL_ACTION_SPAWN, command);
	gowl_macro_set_result(ctx, "launched");
	return TRUE;
}
