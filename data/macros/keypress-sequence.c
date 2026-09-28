/*
 * keypress-sequence.c - a timeline: keys, pauses and text, played in
 * order into one window, while the desktop keeps running.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run keypress-sequence app-id:firefox gowl.dev
 *
 * Opens the address bar of the matching window (Ctrl+L), waits for it,
 * types the text and presses Return -- aimed at that window, so the
 * one you are working in keeps focus.
 *
 * The macro function returns at once; the steps it queued play on
 * timers afterwards.  Stop them half-way with `macro-stop' or
 * Super+Escape.
 *
 * NOT for games: automating input breaks most games' rules.  gowl's
 * input remapper is the 1:1 tool for those; macros are not 1:1.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Ctrl+L, wait, type TEXT, Return -- into the window matching PATTERN";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlClient *target;
	const gchar *text;

	target = gowl_macro_get_arg(ctx, 0) != NULL
		? gowl_macro_find_client(ctx, gowl_macro_get_arg(ctx, 0)) : NULL;
	text = gowl_macro_get_arg(ctx, 1);
	if (target == NULL || text == NULL) {
		gowl_macro_set_result(ctx, "usage: keypress-sequence PATTERN TEXT");
		return FALSE;
	}

	gowl_macro_key(ctx, target, "ctrl+l");
	gowl_macro_wait(ctx, 150);
	gowl_macro_text(ctx, target, text);
	gowl_macro_wait(ctx, 50);
	gowl_macro_key(ctx, target, "Return");
	return TRUE;
}
