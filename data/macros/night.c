/*
 * night.c - a timer macro: in the evening, calm the desktop down once.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it every ten minutes -- in `modules: macro:':
 *     triggers:
 *       - "every 600000: night"
 *
 * Between 22:00 and 06:00 it turns effects down and says so, once per
 * night (it remembers in a static, which lives as long as the compiled
 * macro -- until you edit the file and it is rebuilt).
 */

#include <gowl/gowl.h>

static gint last_night_day = -1;

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "After 22:00, turn effects down once per night";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	g_autoptr(GDateTime) now = NULL;
	gint hour;
	gint day;

	now = g_date_time_new_now_local();
	hour = g_date_time_get_hour(now);
	/* The "night" a 01:00 run belongs to is the day before */
	day = g_date_time_get_day_of_year(now) - (hour < 6 ? 1 : 0);

	if (hour >= 6 && hour < 22) {
		gowl_macro_set_result(ctx, "daytime");
		return TRUE;
	}
	if (day == last_night_day) {
		gowl_macro_set_result(ctx, "already done tonight");
		return TRUE;
	}
	last_night_day = day;

	gowl_macro_action(ctx, GOWL_ACTION_CYCLE_BACKDROP, "none");
	gowl_macro_action(ctx, GOWL_ACTION_TOGGLE_CRT, "off");
	gowl_macro_notify(ctx, "Night mode", "Effects turned down for the night");
	gowl_macro_set_result(ctx, "night mode on");
	return TRUE;
}
