/*
 * loop-demo.c - a macro that never returns, to show the watchdog.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run loop-demo
 *
 * Without the watchdog an endless loop on the compositor thread would
 * freeze the desktop for good.  With it, the loop is interrupted when
 * its time budget runs out (`timeout-ms', two seconds by default), the
 * macro is held back and you are told.  Clear it with:
 *     gowl-msg macro-clear loop-demo
 *
 * long-demo.c is the other half: a macro that is long on purpose and
 * says so.
 */

#include <gowl/gowl.h>

static volatile guint64 spinning;

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Loop forever to show the watchdog stopping it";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	(void)ctx;
	for (;;)
		spinning++;
	return TRUE;
}
