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
 * test-fault-guard.c - The fault guard that keeps user code from
 * taking the session down.
 *
 * These tests fault ON PURPOSE -- NULL writes, abort(), SIGFPE, stack
 * overflows, endless loops -- and assert the process is still usable
 * afterwards, on the main thread and on workers.  The last two run in
 * subprocesses and check the other half of the contract: OUTSIDE a
 * guarded call a fault still reaches whoever handled it before, and
 * with nobody before, it still kills the process.
 */

#define _GNU_SOURCE
#include <glib.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "util/gowl-fault-guard.h"
#include "barkit/gowl-bar-guard.h"

/* Deliberate faults raise warnings; lift the fatal mask around them. */
static GLogLevelFlags saved_mask;

static void
allow_warnings(void)
{
	saved_mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
}

static void
restore_warnings(void)
{
	g_log_set_always_fatal(saved_mask);
}

#define NEED_GUARD() \
	do { \
		gowl_fault_guard_init(); \
		if (!gowl_fault_guard_is_available()) { \
			g_test_skip("the fault guard could not install"); \
			return; \
		} \
	} while (0)

/* ---- bodies ---- */

/* Where busy loops put their work, so none of it is optimised away. */
static volatile guint64 spin_sink;

/* How deep a recursion may go: volatile, so the compiler cannot prove
   the recursion below endless and refuse or flatten it. */
static volatile gint overflow_limit = G_MAXINT;

static void
body_ok(gpointer data)
{
	(*(gint *)data)++;
}

static void
body_segv(gpointer data)
{
	volatile gint *bad = NULL;

	(void)data;
	*bad = 1;
}

static void
body_abort(gpointer data)
{
	(void)data;
	abort();
}

static void
body_fpe(gpointer data)
{
	(void)data;
	raise(SIGFPE);
}

static void
body_bus(gpointer data)
{
	(void)data;
	raise(SIGBUS);
}

/* Recursion the compiler cannot flatten: a frame-sized volatile buffer
   and a use of the result after the call. */
static gint
overflow(gint depth)
{
	volatile gchar frame[4096];

	frame[0] = (gchar)depth;
	frame[sizeof frame - 1] = (gchar)depth;
	if (depth >= overflow_limit)
		return frame[0];
	return overflow(depth + 1) + frame[0] + frame[sizeof frame - 1];
}

static void
body_overflow(gpointer data)
{
	*(gint *)data = overflow(0);
}

static void
body_spin(gpointer data)
{
	(void)data;
	for (;;)
		spin_sink++;
}

/* Spins for *data milliseconds after granting itself 1000. */
static void
body_extend_then_work(gpointer data)
{
	gint64 end;

	g_assert_true(gowl_fault_guard_extend(1000));
	end = g_get_monotonic_time() + (gint64)(*(guint *)data) * 1000;
	while (g_get_monotonic_time() < end)
		spin_sink++;
}

static void
body_disable_then_work(gpointer data)
{
	gint64 end;

	g_assert_true(gowl_fault_guard_extend(0));
	end = g_get_monotonic_time() + (gint64)(*(guint *)data) * 1000;
	while (g_get_monotonic_time() < end)
		spin_sink++;
}

/* An inner guarded call runs directly: its fault unwinds the OUTER. */
static void
body_nested(gpointer data)
{
	gboolean inner;

	g_assert_true(gowl_fault_guard_is_armed());
	inner = gowl_fault_guard_call(body_segv, NULL, 0, NULL, "inner");
	/* never reached */
	*(gint *)data = inner ? 1 : 2;
}

/* ---- tests ---- */

static void
test_clean_call(void)
{
	gint counter = 0;
	gint signo = -1;

	NEED_GUARD();
	g_assert_true(gowl_fault_guard_call(body_ok, &counter, 0, &signo, "ok"));
	g_assert_cmpint(counter, ==, 1);
	g_assert_cmpint(signo, ==, 0);
	g_assert_false(gowl_fault_guard_is_armed());
}

static void
expect_caught(
	GowlFaultGuardFunc body,
	gint               expected
){
	gint signo = 0;
	gint counter = 0;
	guint before;

	before = gowl_fault_guard_get_fault_count();
	allow_warnings();
	g_assert_false(gowl_fault_guard_call(body, &counter, 0, &signo,
	                                     "a deliberate fault"));
	restore_warnings();
	g_assert_cmpint(signo, ==, expected);
	g_assert_cmpuint(gowl_fault_guard_get_fault_count(), ==, before + 1);
	g_assert_false(gowl_fault_guard_is_armed());

	/* still usable */
	g_assert_true(gowl_fault_guard_call(body_ok, &counter, 0, NULL, "after"));
	g_assert_cmpint(counter, ==, 1);
}

static void
test_catches_segv(void)
{
	NEED_GUARD();
	expect_caught(body_segv, SIGSEGV);
}

static void
test_catches_abort(void)
{
	NEED_GUARD();
	expect_caught(body_abort, SIGABRT);
}

static void
test_catches_fpe_and_bus(void)
{
	NEED_GUARD();
	expect_caught(body_fpe, SIGFPE);
	expect_caught(body_bus, SIGBUS);
}

/* A stack overflow on the main thread: its alternate stack is what
   gives the handler somewhere to run. */
static void
test_catches_stack_overflow(void)
{
	gint result = 0;
	gint signo = 0;

	NEED_GUARD();
	allow_warnings();
	g_assert_false(gowl_fault_guard_call(body_overflow, &result, 0, &signo,
	                                     "a deliberate overflow"));
	restore_warnings();
	g_assert_cmpint(signo, ==, SIGSEGV);
}

typedef struct {
	GowlFaultGuardFunc body;
	gboolean           returned;
	gint               signo;
} WorkerRun;

static gpointer
worker(gpointer data)
{
	WorkerRun *run = data;
	gint scratch = 0;

	run->returned = gowl_fault_guard_call(run->body, &scratch, 200,
	                                      &run->signo, "worker");
	return NULL;
}

/* On a worker thread, each with its own landing point, alternate stack
   and watchdog -- the bar guard gave only one thread an altstack. */
static void
test_worker_threads(void)
{
	static const struct {
		GowlFaultGuardFunc body;
		gint               expected;
	} cases[] = {
		{ body_segv, SIGSEGV },
		{ body_abort, SIGABRT },
		{ body_overflow, SIGSEGV },
		{ body_spin, -1 },           /* the watchdog */
	};
	guint i;

	NEED_GUARD();
	allow_warnings();
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		WorkerRun run = { cases[i].body, TRUE, 0 };
		GThread *t;

		t = g_thread_new("fault-worker", worker, &run);
		g_thread_join(t);
		g_assert_false(run.returned);
		if (cases[i].expected == -1)
			g_assert_true(gowl_fault_guard_is_watchdog(run.signo));
		else
			g_assert_cmpint(run.signo, ==, cases[i].expected);
	}
	restore_warnings();
}

/* An endless loop is stopped by its budget. */
static void
test_watchdog_stops_a_loop(void)
{
	gint signo = 0;
	gint64 start;

	NEED_GUARD();
	if (gowl_fault_guard_watchdog_signal() == 0) {
		g_test_skip("no watchdog signal on this system");
		return;
	}
	start = g_get_monotonic_time();
	allow_warnings();
	g_assert_false(gowl_fault_guard_call(body_spin, NULL, 50, &signo, "loop"));
	restore_warnings();
	g_assert_true(gowl_fault_guard_is_watchdog(signo));
	g_assert_cmpstr(gowl_fault_guard_signal_name(signo), ==, "timeout");
	/* roughly on time, and not the next second */
	g_assert_cmpint(g_get_monotonic_time() - start, <, 900 * 1000);
}

/* A call may grant itself more time -- or switch the watchdog off. */
static void
test_extend(void)
{
	guint work = 200;
	gint signo = 0;

	NEED_GUARD();
	if (gowl_fault_guard_watchdog_signal() == 0) {
		g_test_skip("no watchdog signal on this system");
		return;
	}
	g_assert_true(gowl_fault_guard_call(body_extend_then_work, &work, 50,
	                                    &signo, "extended"));
	g_assert_cmpint(signo, ==, 0);
	g_assert_true(gowl_fault_guard_call(body_disable_then_work, &work, 50,
	                                    &signo, "disabled"));
	g_assert_cmpint(signo, ==, 0);
	/* outside a call there is nothing to extend */
	g_assert_false(gowl_fault_guard_extend(10));
}

/* A watchdog that fires while a lock is held is delivered at the
   release: the body gets past the locked region, then unwinds.  The
   mutex is free afterwards -- the whole point. */
static GMutex hold_lock;
static volatile gint hold_reached_release;

static void
body_hold_past_budget(gpointer data)
{
	gint64 end;

	(void)data;
	gowl_fault_guard_hold();
	gowl_fault_guard_hold();          /* nesting */
	g_mutex_lock(&hold_lock);
	end = g_get_monotonic_time() + 200 * 1000;   /* 4x the budget */
	while (g_get_monotonic_time() < end)
		spin_sink++;
	g_mutex_unlock(&hold_lock);
	gowl_fault_guard_release();
	hold_reached_release = 1;         /* still held once: no unwind yet */
	gowl_fault_guard_release();       /* the deferred watchdog lands here */
	hold_reached_release = 2;
}

static void
test_hold_defers_watchdog(void)
{
	gint signo = 0;

	NEED_GUARD();
	if (gowl_fault_guard_watchdog_signal() == 0) {
		g_test_skip("no watchdog signal on this system");
		return;
	}
	hold_reached_release = 0;
	allow_warnings();
	g_assert_false(gowl_fault_guard_call(body_hold_past_budget, NULL, 50,
	                                     &signo, "held"));
	restore_warnings();
	g_assert_true(gowl_fault_guard_is_watchdog(signo));
	g_assert_cmpint(hold_reached_release, ==, 1);
	g_assert_true(g_mutex_trylock(&hold_lock));
	g_mutex_unlock(&hold_lock);

	/* the unwind dropped the hold: the next call is guarded normally */
	allow_warnings();
	g_assert_false(gowl_fault_guard_call(body_spin, NULL, 50, &signo,
	                                     "after"));
	restore_warnings();
	g_assert_true(gowl_fault_guard_is_watchdog(signo));

	/* unmatched release and a hold outside any call are harmless */
	gowl_fault_guard_release();
	gowl_fault_guard_hold();
	gowl_fault_guard_release();
}

/* A watchdog left behind by a call that returned just in time must not
   hit the next one: the timer is disarmed on the way out. */
static void
test_no_stale_watchdog(void)
{
	gint counter = 0;
	gint64 end;

	NEED_GUARD();
	g_assert_true(gowl_fault_guard_call(body_ok, &counter, 20, NULL, "short"));
	end = g_get_monotonic_time() + 60 * 1000;
	while (g_get_monotonic_time() < end)
		spin_sink++;
	g_assert_true(gowl_fault_guard_call(body_ok, &counter, 0, NULL, "next"));
	g_assert_cmpint(counter, ==, 2);
}

static void
test_nested(void)
{
	gint marker = 0;
	gint signo = 0;

	NEED_GUARD();
	allow_warnings();
	g_assert_false(gowl_fault_guard_call(body_nested, &marker, 0, &signo,
	                                     "outer"));
	restore_warnings();
	g_assert_cmpint(signo, ==, SIGSEGV);
	g_assert_cmpint(marker, ==, 0);
}

/* The bar's wrapper keeps its contract. */
static void
test_bar_wrapper(void)
{
	gint signo = 0;

	gowl_bar_guard_init();
	if (!gowl_bar_guard_is_available()) {
		g_test_skip("the fault guard could not install");
		return;
	}
	allow_warnings();
	g_assert_false(gowl_bar_guard_call(body_segv, NULL, &signo, "bar"));
	restore_warnings();
	g_assert_cmpint(signo, ==, SIGSEGV);
	g_assert_cmpstr(gowl_bar_guard_signal_name(SIGSEGV), ==, "SIGSEGV");
}

static void
chained_handler(int signo)
{
	(void)signo;
	_exit(0);
}

/* Outside a guarded call, the handler installed BEFORE the guard still
   gets the fault -- this is Emacs's SIGSEGV handler under cmacs. */
static void
test_unarmed_fault_chains(void)
{
	if (g_test_subprocess()) {
		volatile gint *bad = NULL;

		signal(SIGSEGV, chained_handler);
		gowl_fault_guard_init();
		*bad = 1;
		abort();   /* the chained handler exits 0 first */
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
}

/* ...and with nobody before it, the fault still kills the process. */
static void
test_unarmed_fault_still_fatal(void)
{
	if (g_test_subprocess()) {
		volatile gint *bad = NULL;

		gowl_fault_guard_init();
		*bad = 1;
		exit(0);
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fault-guard/clean-call", test_clean_call);
	g_test_add_func("/fault-guard/segv", test_catches_segv);
	g_test_add_func("/fault-guard/abort", test_catches_abort);
	g_test_add_func("/fault-guard/fpe-and-bus", test_catches_fpe_and_bus);
	g_test_add_func("/fault-guard/stack-overflow", test_catches_stack_overflow);
	g_test_add_func("/fault-guard/worker-threads", test_worker_threads);
	g_test_add_func("/fault-guard/watchdog-stops-a-loop",
	                test_watchdog_stops_a_loop);
	g_test_add_func("/fault-guard/extend", test_extend);
	g_test_add_func("/fault-guard/hold-defers-watchdog",
	                test_hold_defers_watchdog);
	g_test_add_func("/fault-guard/no-stale-watchdog", test_no_stale_watchdog);
	g_test_add_func("/fault-guard/nested", test_nested);
	g_test_add_func("/fault-guard/bar-wrapper", test_bar_wrapper);
	g_test_add_func("/fault-guard/unarmed-fault-chains",
	                test_unarmed_fault_chains);
	g_test_add_func("/fault-guard/unarmed-fault-still-fatal",
	                test_unarmed_fault_still_fatal);

	return g_test_run();
}
