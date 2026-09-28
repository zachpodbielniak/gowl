/*
 * float-center.c - float the focused window and centre it on its
 * output at a given share of the usable area (60% by default).
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run float-center
 *           gowl-msg macro-run float-center 80
 * Keybind:  "Super+c": { action: ipc-command, arg: "macro-run float-center" }
 *
 * Talks to the compositor directly, which a macro in the default mode
 * may: it is running on the compositor thread.
 */

#include <gowl/gowl.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Float and centre the focused window at PERCENT (default 60)";
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

	comp = gowl_macro_get_compositor(ctx);
	c = gowl_compositor_get_focused_client(comp);
	if (c == NULL) {
		gowl_macro_set_result(ctx, "no focused window");
		return FALSE;
	}
	mon = gowl_client_get_monitor(c);
	if (mon == NULL)
		return FALSE;

	pct = 60;
	if (gowl_macro_get_arg(ctx, 0) != NULL
	    && !g_ascii_string_to_unsigned(gowl_macro_get_arg(ctx, 0), 10, 10,
	                                   100, &pct, NULL)) {
		gowl_macro_set_result(ctx, "PERCENT is 10..100");
		return FALSE;
	}

	gowl_monitor_get_window_area(mon, &ax, &ay, &aw, &ah);
	w = (gint)(aw * (gint)pct / 100);
	h = (gint)(ah * (gint)pct / 100);
	gowl_compositor_set_floating(comp, c, TRUE);
	gowl_compositor_resize_client(comp, c, ax + (aw - w) / 2,
	                              ay + (ah - h) / 2, w, h);
	return TRUE;
}
