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
 * test-subprocess.c - Children watched on a wl_event_loop.
 *
 * OCR and voice run a program and wait for its output without blocking
 * the compositor; this checks the helper they share against a bare
 * wl_event_loop: output arrives, the callback runs on the loop's own
 * thread, a deadline kills a child that hangs, SIGINT reaches a whole
 * shell pipeline (the gowl-stt contract), large output is not lost,
 * and cancel leaves no child behind.
 *
 * The paths are /util/child-process/..., NOT /subprocess/...: GTest
 * treats a path that starts with /subprocess as one of its own forked
 * helpers and skips it, so the suite would report 0 tests and pass.
 */

#include <glib.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <wayland-server-core.h>

#include "util/gowl-subprocess.h"

typedef struct {
	gboolean  done;
	gint      status;
	gchar    *out;
	gchar    *err;
	GThread  *thread;
} Result;

static void
on_done(gint status, const gchar *out, const gchar *err, gpointer data)
{
	Result *r = data;

	r->done = TRUE;
	r->status = status;
	r->out = g_strdup(out);
	r->err = g_strdup(err);
	r->thread = g_thread_self();
}

/* Dispatches @loop until @r is done or @ms pass. */
static void
pump(struct wl_event_loop *loop, Result *r, guint ms)
{
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (!r->done && g_get_monotonic_time() < end)
		wl_event_loop_dispatch(loop, 20);
}

static void
result_clear(Result *r)
{
	g_free(r->out);
	g_free(r->err);
	memset(r, 0, sizeof *r);
}

static void
test_output(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = { "sh", "-c", "echo hello; echo oops >&2", NULL };
	g_autoptr(GError) error = NULL;
	Result r = { 0 };

	g_assert_nonnull(gowl_subprocess_spawn(loop, argv, 5000, on_done, &r,
	                                       &error));
	g_assert_no_error(error);
	pump(loop, &r, 5000);
	g_assert_true(r.done);
	g_assert_true(g_spawn_check_wait_status(r.status, NULL));
	g_assert_cmpstr(r.out, ==, "hello\n");
	g_assert_cmpstr(r.err, ==, "oops\n");
	/* on the thread that dispatches the loop -- this one */
	g_assert_true(r.thread == g_thread_self());
	result_clear(&r);
	wl_event_loop_destroy(loop);
}

static void
test_exit_status(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = { "sh", "-c", "exit 3", NULL };
	g_autoptr(GError) error = NULL;
	Result r = { 0 };

	g_assert_nonnull(gowl_subprocess_spawn(loop, argv, 5000, on_done, &r,
	                                       NULL));
	pump(loop, &r, 5000);
	g_assert_true(r.done);
	g_assert_false(g_spawn_check_wait_status(r.status, &error));
	g_assert_error(error, G_SPAWN_EXIT_ERROR, 3);
	result_clear(&r);
	wl_event_loop_destroy(loop);
}

/* A child that would sleep a minute is killed at its deadline, and the
   callback is told -1. */
static void
test_deadline(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = { "sh", "-c", "sleep 60", NULL };
	gint64 t0;
	Result r = { 0 };

	t0 = g_get_monotonic_time();
	g_assert_nonnull(gowl_subprocess_spawn(loop, argv, 200, on_done, &r,
	                                       NULL));
	pump(loop, &r, 5000);
	g_assert_true(r.done);
	g_assert_cmpint(r.status, ==, -1);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 4 * G_USEC_PER_SEC);
	result_clear(&r);
	wl_event_loop_destroy(loop);
}

/* SIGINT reaches the whole group: a shell that runs a recorder in the
   background and waits for it -- gowl-stt's shape -- hears it, and its
   output still arrives. */
static void
test_sigint_pipeline(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = {
		"sh", "-c",
		/* the background job ignores SIGINT (a non-interactive
		   shell's rule), so the trap ends it, as gowl-stt does */
		"sleep 30 & p=$!; trap 'kill $p; echo stopped; exit 0' INT; "
		"wait; echo late",
		NULL
	};
	GowlSubprocess *p;
	Result r = { 0 };

	p = gowl_subprocess_spawn(loop, argv, 10000, on_done, &r, NULL);
	g_assert_nonnull(p);
	/* let the shell reach its wait */
	{
		gint64 end = g_get_monotonic_time() + 300 * 1000;

		while (g_get_monotonic_time() < end)
			wl_event_loop_dispatch(loop, 20);
	}
	g_assert_false(r.done);
	gowl_subprocess_signal(p, SIGINT);
	pump(loop, &r, 5000);
	g_assert_true(r.done);
	g_assert_true(g_spawn_check_wait_status(r.status, NULL));
	g_assert_cmpstr(r.out, ==, "stopped\n");
	result_clear(&r);
	wl_event_loop_destroy(loop);
}

/* More than a pipe buffer: nothing is lost to EAGAIN. */
static void
test_large_output(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = { "sh", "-c", "head -c 300000 /dev/zero | tr '\\0' x",
	                        NULL };
	Result r = { 0 };

	g_assert_nonnull(gowl_subprocess_spawn(loop, argv, 10000, on_done, &r,
	                                       NULL));
	pump(loop, &r, 10000);
	g_assert_true(r.done);
	g_assert_cmpuint(strlen(r.out), ==, 300000);
	result_clear(&r);
	wl_event_loop_destroy(loop);
}

/* Cancel kills and reaps at once; the callback never runs. */
static void
test_cancel(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = { "sh", "-c", "sleep 60", NULL };
	GowlSubprocess *p;
	GPid pid;
	Result r = { 0 };

	p = gowl_subprocess_spawn(loop, argv, 0, on_done, &r, NULL);
	g_assert_nonnull(p);
	pid = gowl_subprocess_get_pid(p);
	gowl_subprocess_cancel(p);
	/* reaped: no such child any more */
	g_assert_cmpint(waitpid(pid, NULL, WNOHANG), ==, -1);
	g_assert_cmpint(errno, ==, ECHILD);
	wl_event_loop_dispatch(loop, 50);
	g_assert_false(r.done);
	/* and the loop holds nothing of it: destroying it is clean */
	wl_event_loop_destroy(loop);
}

static void
test_spawn_failure(void)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	const gchar *argv[] = { "gowl-no-such-program-anywhere", NULL };
	g_autoptr(GError) error = NULL;
	Result r = { 0 };

	g_assert_null(gowl_subprocess_spawn(loop, argv, 0, on_done, &r, &error));
	g_assert_nonnull(error);
	wl_event_loop_destroy(loop);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/util/child-process/output", test_output);
	g_test_add_func("/util/child-process/exit-status", test_exit_status);
	g_test_add_func("/util/child-process/deadline", test_deadline);
	g_test_add_func("/util/child-process/sigint-pipeline", test_sigint_pipeline);
	g_test_add_func("/util/child-process/large-output", test_large_output);
	g_test_add_func("/util/child-process/cancel", test_cancel);
	g_test_add_func("/util/child-process/spawn-failure", test_spawn_failure);
	return g_test_run();
}
