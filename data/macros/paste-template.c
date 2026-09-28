/*
 * paste-template.c - type a named snippet into the focused window.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run paste-template sig
 *           gowl-msg macro-run paste-template date
 * Keybind:  "Super+Alt+s": { action: ipc-command, arg: "macro-run paste-template sig" }
 *
 * Edit the table below: the next run recompiles and uses the new text.
 */

#include <gowl/gowl.h>

typedef struct {
	const gchar *name;
	const gchar *text;
} Snippet;

static const Snippet snippets[] = {
	{ "sig",   "Best regards,\n-- \n" },
	{ "shrug", "\xc2\xaf\\_(\xe3\x83\x84)_/\xc2\xaf" },
	{ "lgtm",  "LGTM, thanks!" },
	{ NULL, NULL }
};

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Type a named snippet (sig, shrug, lgtm, date, time) into the "
	       "focused window";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *name;
	guint i;

	name = gowl_macro_get_arg(ctx, 0);
	if (name == NULL) {
		gowl_macro_set_result(ctx, "usage: paste-template NAME");
		return FALSE;
	}

	/* Computed snippets */
	if (g_strcmp0(name, "date") == 0 || g_strcmp0(name, "time") == 0) {
		g_autoptr(GDateTime) now = g_date_time_new_now_local();
		g_autofree gchar *s = g_date_time_format(
			now, g_strcmp0(name, "date") == 0 ? "%Y-%m-%d" : "%H:%M");

		gowl_macro_text(ctx, NULL, s);
		return TRUE;
	}

	for (i = 0; snippets[i].name != NULL; i++) {
		if (g_strcmp0(snippets[i].name, name) == 0) {
			gowl_macro_text(ctx, NULL, snippets[i].text);
			return TRUE;
		}
	}
	gowl_macro_set_result(ctx, "no such snippet");
	return FALSE;
}
