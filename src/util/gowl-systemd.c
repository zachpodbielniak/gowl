/* gowl-systemd.c -- best-effort systemd user-session integration.
 * See gowl-systemd.h for the rationale. */

#include "util/gowl-systemd.h"

#include <glib.h>
#include <stdlib.h>

/* Once we have decided we cannot talk to the user manager -- explicit
 * opt-out (GOWL_DISABLE_SYSTEMD) or no systemctl on PATH -- cache it so
 * _stop() is a cheap no-op rather than re-probing at quit time. */
static gboolean	gowl_systemd_inactive = FALSE;

/* ------------------------------------------------------------------ */

static gboolean
gowl_systemd_disabled (void)
{
	if (gowl_systemd_inactive)
		return TRUE;
	if (g_getenv ("GOWL_DISABLE_SYSTEMD") != NULL
	    || g_find_program_in_path ("systemctl") == NULL)
	{
		gowl_systemd_inactive = TRUE;
		return TRUE;
	}
	return FALSE;
}

/* Run `systemctl --user <args>` synchronously, returning TRUE on
 * success.  Used for import-environment, where ordering matters: the
 * env must reach the user manager before the session target pulls in
 * the services that consume it. */
static gboolean
gowl_systemd_run_sync (gchar **argv)
{
	gint		status = 0;
	GError		*err = NULL;
	gboolean	ok;

	ok = g_spawn_sync (NULL, argv, NULL,
	                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, NULL, NULL, &status, &err);
	if (!ok)
	{
		g_debug ("gowl-systemd: %s failed: %s",
		         argv[0], err != NULL ? err->message : "?");
		if (err != NULL)
			g_error_free (err);
		return FALSE;
	}
	return g_spawn_check_wait_status (status, NULL);
}

/* Run `systemctl --user <args>` asynchronously; fire-and-forget for the
 * session target start/stop, so the compositor never blocks on a wedged
 * user manager -- least of all at quit time. */
static void
gowl_systemd_run_async (gchar **argv)
{
	GError		*err = NULL;

	if (!g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
	                   NULL, NULL, NULL, &err))
	{
		g_debug ("gowl-systemd: %s failed: %s",
		         argv[0], err != NULL ? err->message : "?");
		if (err != NULL)
			g_error_free (err);
	}
}

/* ------------------------------------------------------------------ */

void
gowl_systemd_start (gboolean seat_session)
{
	static const gchar *const	vars[] = {
		"WAYLAND_DISPLAY",
		"XDG_CURRENT_DESKTOP",
		"XDG_SESSION_TYPE",
		"XDG_SESSION_DESKTOP",
		"XDG_RUNTIME_DIR",
		"DISPLAY",
		NULL
	};
	GPtrArray		*args;
	gchar			**start_argv;
	guint			 i;

	if (gowl_systemd_disabled ())
		return;

	/* 1. Import the live compositor environment into the user manager
	 *    so DBus-activated services don't inherit a stale prior session
	 *    (the cause of "flatpak app can't see the compositor / picks up
	 *    the wrong desktop's theme").  Synchronous: the target started
	 *    next may pull in services that need this environment. */
	args = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (args, g_strdup ("systemctl"));
	g_ptr_array_add (args, g_strdup ("--user"));
	g_ptr_array_add (args, g_strdup ("import-environment"));
	for (i = 0; vars[i] != NULL; i++)
	{
		if (g_getenv (vars[i]) != NULL)
			g_ptr_array_add (args, g_strdup (vars[i]));
	}
	g_ptr_array_add (args, NULL);

	if (!gowl_systemd_run_sync ((gchar **) args->pdata))
	{
		g_warning ("gowl-systemd: `systemctl --user "
		           "import-environment` failed; graphical-session "
		           "services may inherit a stale environment");
	}
	g_ptr_array_free (args, TRUE);

	/* 1b. Seat session only: restart xdg-desktop-portal so it re-reads
	 *     the environment we just imported.  A frontend left running by a
	 *     previous login keeps the prior XDG_CURRENT_DESKTOP -- read once
	 *     at startup to pick portal backends -- so e.g. ScreenCast keeps
	 *     routing to a backend that does not work under gowl (the GNOME
	 *     backend needs Mutter), and screen-sharing silently fails even
	 *     after the desktop file is corrected.  `try-restart` is a no-op
	 *     when the unit is not already running, so we never start the
	 *     portal prematurely (its own units pull it in via
	 *     graphical-session.target below).  Nested gowl skips this: the
	 *     portal is the host session's and must not be disturbed. */
	if (seat_session)
	{
		gchar	*portal_argv[6];

		portal_argv[0] = g_strdup ("systemctl");
		portal_argv[1] = g_strdup ("--user");
		portal_argv[2] = g_strdup ("try-restart");
		portal_argv[3] = g_strdup ("xdg-desktop-portal.service");
		portal_argv[4] = NULL;
		gowl_systemd_run_async (portal_argv);
		for (i = 0; portal_argv[i] != NULL; i++)
			g_free (portal_argv[i]);
	}

	/* 2. Start gowl-session.target, which Wants=graphical-session.target.
	 *    The target itself refuses manual start, so it can only be pulled
	 *    in by a dependency -- this is the wlroots-style bootstrap that
	 *    gnome-session performs automatically but a bare compositor does
	 *    not. */
	start_argv = g_new (gchar *, 5);
	start_argv[0] = g_strdup ("systemctl");
	start_argv[1] = g_strdup ("--user");
	start_argv[2] = g_strdup ("start");
	start_argv[3] = g_strdup ("gowl-session.target");
	start_argv[4] = NULL;
	gowl_systemd_run_async (start_argv);
	g_strfreev (start_argv);
}

void
gowl_systemd_stop (void)
{
	gchar		**stop_argv;

	if (gowl_systemd_disabled ())
		return;

	stop_argv = g_new (gchar *, 5);
	stop_argv[0] = g_strdup ("systemctl");
	stop_argv[1] = g_strdup ("--user");
	stop_argv[2] = g_strdup ("stop");
	stop_argv[3] = g_strdup ("gowl-session.target");
	stop_argv[4] = NULL;
	gowl_systemd_run_async (stop_argv);
	g_strfreev (stop_argv);
}