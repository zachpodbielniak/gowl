/*
 * long-demo.c - a macro that is long ON PURPOSE and says so, so the
 * watchdog lets it finish.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run long-demo        (works for 3 s, then done)
 *
 * Two ways to ask for more time:
 *   - in the source, before anything else:  #define GOWL_MACRO_TIMEOUT_MS 5000
 *   - at run time, first thing:             gowl_macro_set_timeout(ctx, 5000);
 * This one does the second.  Zero switches the watchdog off.
 *
 * It runs on the compositor thread and does not return for three
 * seconds -- the desktop DOES stall for those seconds.  For real long
 * work use a threaded macro (threaded-poll.c); this only demonstrates
 * the budget.
 */

#include <gowl/gowl.h>

static volatile guint64 work;

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Busy for 3 s after raising its own time budget to 5 s";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	gint64 end;
	guint64 ms;

	gowl_macro_set_timeout(ctx, 5000);

	ms = 3000;
	if (gowl_macro_get_arg(ctx, 0) != NULL)
		g_ascii_string_to_unsigned(gowl_macro_get_arg(ctx, 0), 10, 0,
		                           60000, &ms, NULL);
	end = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < end)
		work++;

	gowl_macro_set_result(ctx, "finished within the budget it asked for");
	return TRUE;
}
