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

	return g_test_run();
}
