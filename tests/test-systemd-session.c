/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * test-systemd-session.c -- who owns the systemd user session
 *
 * This decides whether gowl runs `systemctl --user stop
 * gowl-session.target graphical-session.target' when it exits.  Get it
 * wrong in the permissive direction and a gowl nested inside GNOME
 * ends the host's entire desktop on the way out --- every application
 * in it, and any Emacs daemon that happened to be parented to the
 * session.  That is not a hypothetical: it is what this file exists to
 * stop happening again.
 *
 * Get it wrong in the other direction and a real seat session leaves
 * graphical-session.target active after logout, which makes the NEXT
 * login abort with "A graphical session is already running!" and wedges
 * every login until reboot.  Both directions are tested, because the
 * safe-looking fix for one is the cause of the other.
 *
 * Nothing here spawns systemctl.  The decision is a pure function
 * precisely so it can be tested without touching the machine's session.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "util/gowl-systemd.h"

/* ── The seat session, which gowl does own ───────────────────────── */

static void
test_seat_session_manages(void)
{
	/* Launched by a display manager on a VT: nothing around it. */
	g_assert_true(gowl_systemd_should_manage_session(FALSE, NULL, NULL));
}

static void
test_empty_strings_are_not_a_session(void)
{
	/*
	 * An exported-but-empty variable is not a display.  Shells and
	 * .desktop files produce these often enough that treating one as
	 * "a session is present" would silently stop a real seat session
	 * from cleaning up --- and that failure does not show up until the
	 * next login refuses to start.
	 */
	g_assert_true(gowl_systemd_should_manage_session(FALSE, "", ""));
	g_assert_true(gowl_systemd_should_manage_session(FALSE, "", NULL));
	g_assert_true(gowl_systemd_should_manage_session(FALSE, NULL, ""));
}

/* ── Everything else, which it does not ──────────────────────────── */

static void
test_nested_does_not_manage(void)
{
	/* Inside another Wayland compositor. */
	g_assert_false(gowl_systemd_should_manage_session(TRUE, NULL, NULL));
	g_assert_false(gowl_systemd_should_manage_session(TRUE, "wayland-0",
	                                                  NULL));
}

static void
test_parent_wayland_display_does_not_manage(void)
{
	/*
	 * The case that took down the desktop.  A gowl started from a
	 * terminal inside GNOME inherits GNOME's WAYLAND_DISPLAY, and the
	 * nested-backend flag alone does not catch it --- that flag is set
	 * from the output handler, so it is still false when this decision
	 * is made.
	 */
	g_assert_false(gowl_systemd_should_manage_session(FALSE, "wayland-0",
	                                                  NULL));
}

static void
test_parent_x_display_does_not_manage(void)
{
	/* Hosted by an X session; the same reasoning applies. */
	g_assert_false(gowl_systemd_should_manage_session(FALSE, NULL, ":0"));
}

static void
test_headless_under_a_desktop_does_not_manage(void)
{
	/*
	 * A headless run from a terminal is not nested --- there is no
	 * parent compositor backing an output --- but it is still inside
	 * somebody else's session, and stopping that session's targets
	 * ends it just as thoroughly.
	 */
	g_assert_false(gowl_systemd_should_manage_session(FALSE, "wayland-1",
	                                                  ":0"));
}

/* ── start and stop must agree ───────────────────────────────────── */

static void
test_stop_does_nothing_after_a_skipped_start(void)
{
	/*
	 * The actual defect was asymmetry: start() consulted the session
	 * kind and stop() did not, so a gowl that started nothing still
	 * stopped the host's targets.  Nothing here spawns systemctl ---
	 * GOWL_DISABLE_SYSTEMD keeps both calls inert --- but the flag they
	 * share must never come out of a start/stop pair set.
	 */
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);

	g_assert_false(gowl_systemd_is_managing_session());

	gowl_systemd_start(FALSE);
	g_assert_false(gowl_systemd_is_managing_session());

	gowl_systemd_stop();
	g_assert_false(gowl_systemd_is_managing_session());

	/* And a would-be seat session, still inert because systemd
	 * handling is disabled for this process. */
	gowl_systemd_start(TRUE);
	g_assert_false(gowl_systemd_is_managing_session());

	g_unsetenv("GOWL_DISABLE_SYSTEMD");
}

/*
 * The other half of the pair, and the one a careless fix breaks: a real
 * seat session must still stop its targets.  Leaving
 * graphical-session.target active after logout makes the next login
 * abort in gnome-session-init-worker and wedges every login until
 * reboot, so "just never stop anything" is not a fix.
 *
 * Run in a subprocess for two reasons: gowl_systemd_disabled() caches
 * its answer for the life of the process, and this test replaces
 * systemctl on PATH with a shim.  Nothing real is ever started or
 * stopped --- the shim only writes down what it was asked to do.
 */
static void
test_seat_really_manages_subprocess(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *shim = NULL;
	g_autofree gchar *log = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	gint i;

	dir = g_dir_make_tmp("gowl-systemd-XXXXXX", NULL);
	g_assert_nonnull(dir);

	shim = g_build_filename(dir, "systemctl", NULL);
	log = g_build_filename(dir, "calls.log", NULL);

	body = g_strdup_printf("#!/bin/sh\necho \"$@\" >> %s\nexit 0\n", log);
	g_assert_true(g_file_set_contents(shim, body, -1, NULL));
	g_assert_cmpint(g_chmod(shim, 0755), ==, 0);

	/* Ahead of the real one, which must never be reached. */
	path = g_strdup_printf("%s:%s", dir, g_getenv("PATH"));
	g_setenv("PATH", path, TRUE);
	g_unsetenv("GOWL_DISABLE_SYSTEMD");

	gowl_systemd_start(TRUE);
	g_assert_true(gowl_systemd_is_managing_session());

	gowl_systemd_stop();
	g_assert_false(gowl_systemd_is_managing_session());

	/* The calls are fire-and-forget, so give the shim a moment. */
	for (i = 0; i < 100; i++) {
		g_autofree gchar *seen = NULL;

		if (g_file_get_contents(log, &seen, NULL, NULL)
		    && strstr(seen, "stop gowl-session.target") != NULL) {
			g_assert_nonnull(strstr(seen, "start gowl-session.target"));
			g_assert_nonnull(strstr(seen,
			                        "graphical-session.target"));
			return;
		}
		g_usleep(20000);
	}

	g_error("seat session never ran `systemctl stop gowl-session.target'");
}

static void
test_seat_really_manages(void)
{
	g_test_trap_subprocess(
		"/systemd-session/seat-really-manages/subprocess", 0, 0);
	g_test_trap_assert_passed();
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/systemd-session/seat-manages",
	                test_seat_session_manages);
	g_test_add_func("/systemd-session/empty-is-not-a-session",
	                test_empty_strings_are_not_a_session);
	g_test_add_func("/systemd-session/nested",
	                test_nested_does_not_manage);
	g_test_add_func("/systemd-session/parent-wayland",
	                test_parent_wayland_display_does_not_manage);
	g_test_add_func("/systemd-session/parent-x11",
	                test_parent_x_display_does_not_manage);
	g_test_add_func("/systemd-session/headless-under-desktop",
	                test_headless_under_a_desktop_does_not_manage);
	g_test_add_func("/systemd-session/seat-really-manages",
	                test_seat_really_manages);
	g_test_add_func("/systemd-session/seat-really-manages/subprocess",
	                test_seat_really_manages_subprocess);

	/* Last: it disables systemd handling, and that is cached for the
	 * life of the process. */
	g_test_add_func("/systemd-session/stop-mirrors-start",
	                test_stop_does_nothing_after_a_skipped_start);

	return g_test_run();
}
