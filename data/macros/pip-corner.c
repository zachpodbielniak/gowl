/*
 * pip-corner.c - float a picture-in-picture window into the corner.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Meant for a FILTERED trigger: the filter decides which new windows are
 * picture-in-picture, the macro only moves them.  In `modules: macro:'
 *
 *     triggers:
 *       - "client-added [(title='Picture-in-Picture' or app-id=mpv)
 *          and clients>=2 and fullscreen=false]: pip-corner 30"
 *
 * (one line in the real config).  So a PiP window or an mpv player that
 * opens on a tag already showing something floats at 30% of the screen,
 * bottom right, out of the way -- and one opened on an empty tag, or
 * fullscreen, is left alone.
 *
 * By hand it takes the focused window:
 *           gowl-msg macro-run pip-corner 25
 *
 * The event's detail is "client-added app-id=APP title=TITLE"; the
 * window is looked up by both, so two players with different titles are
 * not confused.
 */

#include <gowl/gowl.h>
#include <string.h>

#define MARGIN (16)

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Float the new (or focused) window at PERCENT, bottom right";
}

/* The window the event was about, or NULL when not run by one. */
static GowlClient *
event_window(GowlMacroContext *ctx)
{
	const gchar *detail;
	const gchar *a;
	const gchar *t;
	g_autofree gchar *app = NULL;
	GList *clients;
	GList *l;
	GowlClient *found = NULL;

	detail = gowl_macro_get_trigger_detail(ctx);
	a = strstr(detail, " app-id=");
	t = strstr(detail, " title=");
	if (a == NULL || t == NULL || t < a)
		return NULL;
	app = g_strndup(a + 8, (gsize)(t - (a + 8)));

	clients = gowl_macro_list_clients(ctx, FALSE);
	for (l = clients; l != NULL && found == NULL; l = l->next) {
		const gchar *ca = gowl_client_get_app_id(l->data);
		const gchar *ct = gowl_client_get_title(l->data);

		if (g_strcmp0(ca != NULL ? ca : "", app) == 0
		    && g_strcmp0(ct != NULL ? ct : "", t + 7) == 0)
			found = l->data;
	}
	g_list_free(clients);
	return found;
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	GowlCompositor *comp;
	GowlClient *c;
	GowlMonitor *mon;
	gint ax;
	gint ay;
	gint aw;
	gint ah;
	gint w;
	gint h;
	guint64 pct;

	pct = 30;
	if (gowl_macro_get_arg(ctx, 0) != NULL
	    && !g_ascii_string_to_unsigned(gowl_macro_get_arg(ctx, 0), 10, 10,
	                                   90, &pct, NULL)) {
		gowl_macro_set_result(ctx, "PERCENT is 10..90");
		return FALSE;
	}

	comp = gowl_macro_get_compositor(ctx);
	c = event_window(ctx);
	if (c == NULL)
		c = gowl_compositor_get_focused_client(comp);
	if (c == NULL) {
		gowl_macro_set_result(ctx, "no window");
		return FALSE;
	}
	mon = gowl_client_get_monitor(c);
	if (mon == NULL)
		return FALSE;

	/* PERCENT of the usable area, in the bottom-right corner */
	gowl_monitor_get_window_area(mon, &ax, &ay, &aw, &ah);
	w = aw * (gint)pct / 100;
	h = ah * (gint)pct / 100;
	gowl_compositor_set_floating(comp, c, TRUE);
	gowl_compositor_resize_client(comp, c, ax + aw - w - MARGIN,
	                              ay + ah - h - MARGIN, w, h);
	gowl_macro_set_result(ctx, gowl_client_get_title(c));
	return TRUE;
}
