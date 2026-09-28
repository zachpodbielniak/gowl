/*
 * crash-demo.c - a macro that crashes ON PURPOSE, to show containment.
 *
 * gowl - GObject Wayland Compositor.  Example macro.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Run it:   gowl-msg macro-run crash-demo            (NULL write)
 *           gowl-msg macro-run crash-demo abort      (abort())
 *           gowl-msg macro-run crash-demo divide     (SIGFPE)
 *
 * What happens: the fault guard catches the signal, the macro is
 * unwound and HELD BACK (quarantined), the log and the IPC event stream
 * say why, and a notification appears.  The session carries on.  Try
 * it again and it is refused until:
 *     gowl-msg macro-clear crash-demo
 */

#include <gowl/gowl.h>
#include <signal.h>
#include <stdlib.h>

G_MODULE_EXPORT const gchar *
gowl_macro_info(void)
{
	return "Crash deliberately (segv, abort or divide) to show containment";
}

G_MODULE_EXPORT gboolean
gowl_macro_run(GowlMacroContext *ctx)
{
	const gchar *how = gowl_macro_get_arg(ctx, 0);

	if (g_strcmp0(how, "abort") == 0)
		abort();
	if (g_strcmp0(how, "divide") == 0)
		raise(SIGFPE);
	{
		volatile gint *nowhere = NULL;

		*nowhere = 42;
	}
	return TRUE;
}
