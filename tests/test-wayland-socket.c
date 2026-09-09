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
 * test-wayland-socket.c -- a socket file is not a compositor
 *
 * The distinction this file defends: $XDG_RUNTIME_DIR/wayland-N EXISTS
 * and $XDG_RUNTIME_DIR/wayland-N ANSWERS are different questions, and
 * for the whole life of a boot they can disagree.  A compositor that
 * exits without destroying its wl_display leaves the file; nothing
 * removes it afterwards.
 *
 * Answering the first question when you meant the second is what made
 * a cmacs session unable to log back in after logging out: its own
 * leftover socket looked like a parent compositor to the next launch,
 * which then nested itself into a dead socket and died.
 *
 * The stale case is therefore the important one here.  A test that only
 * checks a live socket passes just as happily against access(F_OK).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>

#include "util/gowl-wayland-socket.h"

/* A listening AF_UNIX socket at @path, as libwayland would leave one. */
static gint
listen_at(const gchar *path)
{
	struct sockaddr_un	addr;
	gint			fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	g_assert_cmpint(fd, >=, 0);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	g_assert_cmpuint(strlen(path), <, sizeof(addr.sun_path));
	memcpy(addr.sun_path, path, strlen(path));

	g_assert_cmpint(bind(fd, (struct sockaddr *) &addr, sizeof(addr)),
	                ==, 0);
	g_assert_cmpint(listen(fd, 8), ==, 0);
	return fd;
}

/* A live compositor answers. */
static void
test_live_socket(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;
	gint			 fd;

	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);

	fd = listen_at(path);

	g_assert_true(gowl_wayland_socket_live(path));

	close(fd);
	g_unlink(path);
	g_rmdir(dir);
}

/*
 * The case the bug was made of: the file is there, nothing is behind
 * it.  access(F_OK) says yes; a client cannot connect.
 */
static void
test_stale_socket(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;
	gint			 fd;

	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);

	fd = listen_at(path);
	close(fd);	/* the compositor exits; the file stays */

	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_false(gowl_wayland_socket_live(path));

	g_unlink(path);
	g_rmdir(dir);
}

/* A name with nothing at all behind it. */
static void
test_missing_socket(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;

	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);

	g_assert_false(gowl_wayland_socket_live(path));

	g_rmdir(dir);
}

/* An ordinary file is not a socket, however plausibly it is named. */
static void
test_regular_file_is_not_a_socket(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;

	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);
	g_assert_true(g_file_set_contents(path, "", 0, NULL));

	g_assert_false(gowl_wayland_socket_live(path));

	g_unlink(path);
	g_rmdir(dir);
}

/* Nothing is not a display. */
static void
test_empty_and_null(void)
{
	g_assert_false(gowl_wayland_socket_live(NULL));
	g_assert_false(gowl_wayland_socket_live(""));
}

/*
 * A bare name resolves against $XDG_RUNTIME_DIR, the way libwayland
 * resolves it -- the form $WAYLAND_DISPLAY almost always takes.
 */
static void
test_relative_name_uses_runtime_dir(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;
	g_autofree gchar	*saved = NULL;
	const gchar		*runtime;
	gint			 fd;

	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);

	runtime = g_getenv("XDG_RUNTIME_DIR");
	saved = runtime != NULL ? g_strdup(runtime) : NULL;
	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);

	fd = listen_at(path);
	g_assert_true(gowl_wayland_socket_live("wayland-9"));

	close(fd);
	g_unlink(path);
	g_assert_false(gowl_wayland_socket_live("wayland-9"));

	if (saved != NULL)
		g_setenv("XDG_RUNTIME_DIR", saved, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");

	g_rmdir(dir);
}

/* Without a runtime dir a relative name cannot resolve to anything. */
static void
test_relative_name_without_runtime_dir(void)
{
	g_autofree gchar	*saved = NULL;
	const gchar		*runtime;

	runtime = g_getenv("XDG_RUNTIME_DIR");
	saved = runtime != NULL ? g_strdup(runtime) : NULL;
	g_unsetenv("XDG_RUNTIME_DIR");

	g_assert_false(gowl_wayland_socket_live("wayland-0"));

	if (saved != NULL)
		g_setenv("XDG_RUNTIME_DIR", saved, TRUE);
}

/*
 * Cleanup at exit.  The registration is an atexit() handler, so the
 * only honest way to test it is to let a process actually exit: the
 * subprocess registers, the parent checks afterwards that the file is
 * gone.
 *
 * The directory travels through the environment because
 * g_test_trap_subprocess re-executes the test binary and shares nothing
 * else with it.
 */
#define SOCKET_DIR_ENV	"GOWL_TEST_SOCKET_DIR"

static void
test_unlink_on_exit_subprocess(void)
{
	g_autofree gchar	*path = NULL;
	const gchar		*dir;
	pid_t			 child;
	gint			 status;

	dir = g_getenv(SOCKET_DIR_ENV);
	g_assert_nonnull(dir);
	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);

	path = g_build_filename(dir, "wayland-9", NULL);
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

	gowl_wayland_socket_unlink_on_exit("wayland-9");

	/*
	 * A forked child inherits the handler and the recorded path, and
	 * its parent is still serving that socket -- so an exiting child
	 * must leave the file alone.  Without the pid guard, every
	 * subprocess that exits through exit() would take the live
	 * session's socket with it.
	 */
	child = fork();
	g_assert_cmpint(child, >=, 0);
	if (child == 0)
		exit(0);
	g_assert_cmpint(waitpid(child, &status, 0), ==, child);
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

	/* Our own exit, on the way out of this test binary, removes it. */
}

static void
test_unlink_on_exit(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;
	gint			 fd;

	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);

	fd = listen_at(path);
	close(fd);

	g_setenv(SOCKET_DIR_ENV, dir, TRUE);
	g_test_trap_subprocess("/wayland-socket/unlink-on-exit/subprocess",
	                       0, 0);
	g_test_trap_assert_passed();
	g_unsetenv(SOCKET_DIR_ENV);

	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

	g_rmdir(dir);
}

/* ------------------------------------------------------------------
 * Which way an uncertain probe must fall.
 *
 * These are the GNOME tests.  gowl_wayland_detect_parent_session()
 * clears $WAYLAND_DISPLAY and $DISPLAY when it can prove they name
 * nothing, and those two variables are precisely what
 * gowl_systemd_should_manage_session() reads to decide whether gowl
 * owns the systemd user session.  Concluding "no parent" while nested
 * inside GNOME makes gowl start gowl-session.target and, on the way
 * out, stop graphical-session.target -- GNOME's -- ending the user's
 * whole desktop.  That has happened once already (gowl e4164d4); it
 * must not happen through this door.
 *
 * So the asymmetry is the thing under test.  A live socket and a
 * *proven* dead one are the easy cases; what matters is that a probe
 * which could not reach a verdict at all leaves the environment alone.
 */

/* Save and restore the three variables these tests move around: the
 * suite is one process, so a test that leaked $WAYLAND_DISPLAY would
 * silently change the meaning of every test after it. */
typedef struct {
	gchar	*wayland_display;
	gchar	*wayland_socket;
	gchar	*runtime_dir;
	gchar	*display;
} SavedEnv;

static void
env_save(SavedEnv *e)
{
	e->wayland_display = g_strdup(g_getenv("WAYLAND_DISPLAY"));
	e->wayland_socket = g_strdup(g_getenv("WAYLAND_SOCKET"));
	e->runtime_dir = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	e->display = g_strdup(g_getenv("DISPLAY"));
}

static void
env_restore_one(const gchar *name, gchar *value)
{
	if (value != NULL) {
		g_setenv(name, value, TRUE);
		g_free(value);
	} else {
		g_unsetenv(name);
	}
}

static void
env_restore(SavedEnv *e)
{
	env_restore_one("WAYLAND_DISPLAY", e->wayland_display);
	env_restore_one("WAYLAND_SOCKET", e->wayland_socket);
	env_restore_one("XDG_RUNTIME_DIR", e->runtime_dir);
	env_restore_one("DISPLAY", e->display);
}

/* A parent compositor is a parent compositor. */
static void
test_detect_live_parent(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;
	SavedEnv		 saved;
	gint			 fd;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);
	fd = listen_at(path);

	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);
	g_setenv("WAYLAND_DISPLAY", "wayland-9", TRUE);
	g_unsetenv("WAYLAND_SOCKET");

	g_assert_true(gowl_wayland_detect_parent_session());
	g_assert_cmpstr(g_getenv("WAYLAND_DISPLAY"), ==, "wayland-9");

	close(fd);
	g_unlink(path);
	g_rmdir(dir);
	env_restore(&saved);
}

/* The login wedge: a name whose socket refuses the connection. */
static void
test_detect_stale_parent(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*path = NULL;
	SavedEnv		 saved;
	gint			 fd;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "wayland-9", NULL);
	fd = listen_at(path);
	close(fd);	/* the file survives its compositor */

	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);
	g_setenv("WAYLAND_DISPLAY", "wayland-9", TRUE);
	g_unsetenv("WAYLAND_SOCKET");
	g_unsetenv("DISPLAY");

	g_assert_false(gowl_wayland_detect_parent_session());
	g_assert_null(g_getenv("WAYLAND_DISPLAY"));

	g_unlink(path);
	g_rmdir(dir);
	env_restore(&saved);
}

/*
 * An unprobeable name must be LEFT ALONE.  Here $XDG_RUNTIME_DIR is
 * missing, so a relative display name cannot be resolved at all -- the
 * probe has nothing to say, and saying "dead" would be an invention.
 */
static void
test_detect_unprobeable_name_is_kept(void)
{
	SavedEnv	saved;

	env_save(&saved);
	g_unsetenv("XDG_RUNTIME_DIR");
	g_unsetenv("WAYLAND_SOCKET");
	g_setenv("WAYLAND_DISPLAY", "wayland-0", TRUE);

	g_assert_true(gowl_wayland_detect_parent_session());
	g_assert_cmpstr(g_getenv("WAYLAND_DISPLAY"), ==, "wayland-0");

	env_restore(&saved);
}

/*
 * Same requirement, different reason to fail: a socket path longer than
 * sockaddr_un's 108 bytes cannot be connected to, which says nothing
 * about whether a compositor is running.
 */
static void
test_detect_overlong_path_is_kept(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*deep = NULL;
	SavedEnv		 saved;
	GString			*buf;
	gint			 i;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);

	buf = g_string_new(dir);
	for (i = 0; i < 12; i++) {
		g_string_append(buf, "/0123456789");
		g_assert_cmpint(g_mkdir(buf->str, 0700), ==, 0);
	}
	deep = g_string_free(buf, FALSE);
	g_assert_cmpuint(strlen(deep) + strlen("/wayland-9"), >=, 108);

	g_setenv("XDG_RUNTIME_DIR", deep, TRUE);
	g_setenv("WAYLAND_DISPLAY", "wayland-9", TRUE);
	g_unsetenv("WAYLAND_SOCKET");

	g_assert_true(gowl_wayland_detect_parent_session());
	g_assert_cmpstr(g_getenv("WAYLAND_DISPLAY"), ==, "wayland-9");

	/* Unwind the nesting from the inside out. */
	for (i = 0; i < 12; i++) {
		g_assert_cmpint(g_rmdir(deep), ==, 0);
		*strrchr(deep, '/') = '\0';
	}
	g_rmdir(dir);
	env_restore(&saved);
}

/* A connected fd from a parent settles it without any probing. */
static void
test_detect_wayland_socket_fd(void)
{
	SavedEnv	saved;

	env_save(&saved);
	g_unsetenv("WAYLAND_DISPLAY");
	g_setenv("WAYLAND_SOCKET", "7", TRUE);

	g_assert_true(gowl_wayland_detect_parent_session());

	env_restore(&saved);
}

/* A seat: nothing named, nothing listening, nothing invented. */
static void
test_detect_seat_session(void)
{
	g_autofree gchar	*dir = NULL;
	SavedEnv		 saved;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);

	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);
	g_unsetenv("WAYLAND_DISPLAY");
	g_unsetenv("WAYLAND_SOCKET");
	g_unsetenv("DISPLAY");

	g_assert_false(gowl_wayland_detect_parent_session());
	g_assert_null(g_getenv("WAYLAND_DISPLAY"));

	g_rmdir(dir);
	env_restore(&saved);
}

/* The unnamed-parent case: no variable, but a live socket to find. */
static void
test_detect_scan_finds_live_socket(void)
{
	g_autofree gchar	*dir = NULL;
	g_autofree gchar	*live = NULL;
	g_autofree gchar	*stale = NULL;
	SavedEnv		 saved;
	gint			 fd;
	gint			 dead;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);

	/* wayland-0 stale, wayland-1 live: the scan must walk past the
	 * leftover rather than stopping at the first file it sees. */
	stale = g_build_filename(dir, "wayland-0", NULL);
	dead = listen_at(stale);
	close(dead);

	live = g_build_filename(dir, "wayland-1", NULL);
	fd = listen_at(live);

	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);
	g_unsetenv("WAYLAND_DISPLAY");
	g_unsetenv("WAYLAND_SOCKET");

	g_assert_true(gowl_wayland_detect_parent_session());
	g_assert_cmpstr(g_getenv("WAYLAND_DISPLAY"), ==, "wayland-1");

	close(fd);
	g_unlink(live);
	g_unlink(stale);
	g_rmdir(dir);
	env_restore(&saved);
}

/* A $DISPLAY naming no X server, with no Wayland parent, is cleared. */
static void
test_detect_clears_dead_x_display(void)
{
	g_autofree gchar	*dir = NULL;
	SavedEnv		 saved;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);

	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);
	g_unsetenv("WAYLAND_DISPLAY");
	g_unsetenv("WAYLAND_SOCKET");
	/* :97 has no /tmp/.X11-unix/X97 on any machine running this. */
	g_setenv("DISPLAY", ":97", TRUE);

	g_assert_false(gowl_wayland_detect_parent_session());
	g_assert_null(g_getenv("DISPLAY"));

	g_rmdir(dir);
	env_restore(&saved);
}

/* A $DISPLAY we cannot interpret is somebody's deliberate choice. */
static void
test_detect_keeps_remote_x_display(void)
{
	g_autofree gchar	*dir = NULL;
	SavedEnv		 saved;

	env_save(&saved);
	dir = g_dir_make_tmp("gowl-wl-XXXXXX", NULL);
	g_assert_nonnull(dir);

	g_setenv("XDG_RUNTIME_DIR", dir, TRUE);
	g_unsetenv("WAYLAND_DISPLAY");
	g_unsetenv("WAYLAND_SOCKET");
	g_setenv("DISPLAY", "somehost:0", TRUE);

	g_assert_false(gowl_wayland_detect_parent_session());
	g_assert_cmpstr(g_getenv("DISPLAY"), ==, "somehost:0");

	g_rmdir(dir);
	env_restore(&saved);
}

/* live and absent are not each other's negation. */
static void
test_predicates_disagree_on_uncertainty(void)
{
	SavedEnv	saved;

	env_save(&saved);
	g_unsetenv("XDG_RUNTIME_DIR");

	/* Unresolvable: neither predicate may claim it. */
	g_assert_false(gowl_wayland_socket_live("wayland-0"));
	g_assert_false(gowl_wayland_socket_absent("wayland-0"));

	/* An absolute path that does not exist IS answerable. */
	g_assert_true(gowl_wayland_socket_absent("/nonexistent/wayland-9"));
	g_assert_false(gowl_wayland_socket_live("/nonexistent/wayland-9"));

	env_restore(&saved);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/wayland-socket/live", test_live_socket);
	g_test_add_func("/wayland-socket/stale", test_stale_socket);
	g_test_add_func("/wayland-socket/missing", test_missing_socket);
	g_test_add_func("/wayland-socket/regular-file",
	                test_regular_file_is_not_a_socket);
	g_test_add_func("/wayland-socket/empty", test_empty_and_null);
	g_test_add_func("/wayland-socket/relative-name",
	                test_relative_name_uses_runtime_dir);
	g_test_add_func("/wayland-socket/relative-name-no-runtime-dir",
	                test_relative_name_without_runtime_dir);
	g_test_add_func("/wayland-socket/unlink-on-exit",
	                test_unlink_on_exit);
	g_test_add_func("/wayland-socket/unlink-on-exit/subprocess",
	                test_unlink_on_exit_subprocess);

	g_test_add_func("/wayland-socket/detect/live-parent",
	                test_detect_live_parent);
	g_test_add_func("/wayland-socket/detect/stale-parent",
	                test_detect_stale_parent);
	g_test_add_func("/wayland-socket/detect/unprobeable-name-kept",
	                test_detect_unprobeable_name_is_kept);
	g_test_add_func("/wayland-socket/detect/overlong-path-kept",
	                test_detect_overlong_path_is_kept);
	g_test_add_func("/wayland-socket/detect/wayland-socket-fd",
	                test_detect_wayland_socket_fd);
	g_test_add_func("/wayland-socket/detect/seat", test_detect_seat_session);
	g_test_add_func("/wayland-socket/detect/scan-finds-live",
	                test_detect_scan_finds_live_socket);
	g_test_add_func("/wayland-socket/detect/dead-x-display",
	                test_detect_clears_dead_x_display);
	g_test_add_func("/wayland-socket/detect/remote-x-display",
	                test_detect_keeps_remote_x_display);
	g_test_add_func("/wayland-socket/predicates-disagree",
	                test_predicates_disagree_on_uncertainty);

	return g_test_run();
}
