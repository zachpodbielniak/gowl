/*
 * threaded-poll.c - a THREADED macro: watch a file and notify when it
 * changes, for as long as you let it run.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run threaded-poll ~/build.log
 * Stop it:  gowl-msg macro-stop threaded-poll   (or Super+Escape)
 *
 * `#define GOWL_MACRO_THREADED 1' puts the macro on a worker thread of
 * its own, where gowl_macro_sleep() really sleeps without freezing the
 * desktop.  It must then touch the compositor only through the
 * gowl_macro_* calls (they hop to the compositor thread) or
 * gowl_macro_call_on_compositor().
 *
 * It switches its own watchdog off first thing: a watcher is long on
 * purpose, and the default two-second budget would stop it.
 */

#include <gowl/gowl.h>
#include <glib/gstdio.h>

#define GOWL_MACRO_THREADED 1

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Threaded: notify each time FILE changes, until stopped";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *arg;
	g_autofree gchar *path = NULL;
	GStatBuf st;
	gint64 last;
	guint changes;

	/* Long by design: no watchdog for this run */
	gowl_macro_set_timeout(ctx, 0);

	arg = gowl_macro_get_arg(ctx, 0);
	if (arg == NULL) {
		gowl_macro_set_result(ctx, "usage: threaded-poll FILE");
		return FALSE;
	}
	path = g_str_has_prefix(arg, "~/")
		? g_build_filename(g_get_home_dir(), arg + 2, NULL) : g_strdup(arg);

	last = g_stat(path, &st) == 0 ? (gint64)st.st_mtime : 0;
	changes = 0;

	/* gowl_macro_sleep() returns FALSE the moment the run is stopped */
	while (gowl_macro_sleep(ctx, 1000)) {
		gint64 now = g_stat(path, &st) == 0 ? (gint64)st.st_mtime : 0;

		if (now != last) {
			last = now;
			changes++;
			gowl_macro_notify(ctx, "File changed", path);
		}
	}

	{
		g_autofree gchar *msg = g_strdup_printf("saw %u change(s)", changes);

		gowl_macro_set_result(ctx, msg);
	}
	return TRUE;
}
