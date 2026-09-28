/*
 * presentation.c - get the desktop ready to share: effects off, bar
 * toasts cleared, the focused window fullscreen -- and back.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run presentation on
 *           gowl-msg macro-run presentation off
 *
 * Every step is an ordinary compositor action or module command, queued
 * and played in order -- the same things a keybind would do, in one go.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "on: no effects, no toasts, focused window fullscreen; off: undo";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlClient *c;
	gboolean on;

	on = g_strcmp0(gowl_macro_get_arg(ctx, 0), "off") != 0;
	c = gowl_compositor_get_focused_client(gowl_macro_get_compositor(ctx));

	/* Effects: no tube, nothing through translucent windows */
	gowl_macro_action(ctx, GOWL_ACTION_TOGGLE_CRT, "off");
	gowl_macro_action(ctx, GOWL_ACTION_CYCLE_BACKDROP, on ? "none" : "glass");
	/* Clear the bar's toasts (no reply if the bar is not loaded) */
	gowl_macro_command(ctx, "bar-dismiss");
	/* Fullscreen the focused window, or take it back out */
	if (c != NULL && gowl_client_get_fullscreen(c) != on)
		gowl_macro_action(ctx, GOWL_ACTION_TOGGLE_FULLSCREEN, NULL);

	gowl_macro_set_result(ctx, on ? "presenting" : "back to normal");
	return TRUE;
}
