/*
 * tidy-on-map.c - when a window appears, send known applications to
 * their tags.  Window rules as code: anything C can decide, a rule can.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Trigger it on every new window -- in `modules: macro:':
 *     triggers:
 *       - "client-added: tidy-on-map"
 *
 * The event's detail carries the new window's app-id and title
 * ("client-added app-id=foot title=~"); this reads the app-id from it.
 * Edit the table to taste.
 */

#include <gowl/gowl.h>
#include <string.h>

typedef struct {
	const gchar *app_glob;
	guint        tag;        /* 1..9 */
} Placement;

static const Placement placements[] = {
	{ "*firefox*",        2 },
	{ "*thunderbird*",    3 },
	{ "Element",          4 },
	{ "*steam*",          9 },
	{ NULL, 0 }
};

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "On client-added, move known apps to their tags";
}

/* The app-id out of "client-added app-id=X title=Y". */
static gchar *
detail_app_id(const gchar *detail)
{
	const gchar *start;
	const gchar *end;

	start = strstr(detail, "app-id=");
	if (start == NULL)
		return NULL;
	start += strlen("app-id=");
	end = strstr(start, " title=");
	return end != NULL ? g_strndup(start, (gsize)(end - start))
	                   : g_strdup(start);
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	g_autofree gchar *app = NULL;
	g_autofree gchar *pattern = NULL;
	GowlClient *c;
	guint i;

	app = detail_app_id(gowl_macro_get_trigger_detail(ctx));
	if (app == NULL || *app == '\0')
		return TRUE;             /* not started by client-added */

	for (i = 0; placements[i].app_glob != NULL; i++) {
		if (!g_pattern_match_simple(placements[i].app_glob, app))
			continue;
		pattern = g_strdup_printf("app-id:%s", app);
		c = gowl_macro_find_client(ctx, pattern);
		if (c != NULL)
			gowl_macro_move_client(ctx, c, NULL,
			                       1u << (placements[i].tag - 1));
		gowl_macro_log(ctx, "sent %s to tag %u", app, placements[i].tag);
		return TRUE;
	}
	return TRUE;
}
