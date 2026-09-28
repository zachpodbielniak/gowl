/*
 * type-into.c - type text into a window found by app-id or title,
 * without taking focus away from the window you are in.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run type-into app-id:foot 'make -j8\n'
 *           gowl-msg macro-run type-into 'title:*htop*' q
 * The first argument is "app-id:GLOB", "title:GLOB" or a bare app-id
 * glob; the rest are joined with spaces.  `\n' in the text is Return.
 *
 * The keys go to that window through a brief keyboard focus change and
 * focus comes straight back.  Refused while the screen is locked.
 */

#include <gowl/gowl.h>
#include <string.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Type TEXT into the window matching PATTERN, keeping focus";
}

/* "\n" and "\t" written literally on a command line become the real
   characters. */
static gchar *
unescape(const gchar *s)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	for (p = s; *p != '\0'; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			g_string_append_c(out, '\n');
			p++;
		} else if (p[0] == '\\' && p[1] == 't') {
			g_string_append_c(out, '\t');
			p++;
		} else {
			g_string_append_c(out, *p);
		}
	}
	return g_string_free(out, FALSE);
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *pattern;
	GowlClient *target;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *text = NULL;

	pattern = gowl_macro_get_arg(ctx, 0);
	if (pattern == NULL || gowl_macro_get_argc(ctx) < 2) {
		gowl_macro_set_result(ctx, "usage: type-into PATTERN TEXT...");
		return FALSE;
	}
	target = gowl_macro_find_client(ctx, pattern);
	if (target == NULL) {
		gowl_macro_set_result(ctx, "no window matches");
		return FALSE;
	}

	joined = g_strjoinv(" ", (gchar **)gowl_macro_get_argv(ctx) + 1);
	text = unescape(joined);
	gowl_macro_text(ctx, target, text);
	return TRUE;
}
