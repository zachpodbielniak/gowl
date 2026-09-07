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

#define _GNU_SOURCE
#include "barkit/gowl-bar-guard.h"

#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

/**
 * SECTION:gowl-bar-guard
 * @title: Bar fault guard
 * @short_description: makes a faulting plugin callback survivable
 *
 * Bar plugins are in-process, which is what lets them read compositor
 * state directly and draw into the bar's own buffer, and is also what
 * makes a bad one lethal.  The guard narrows that: a fault inside a
 * guarded callback unwinds back to the call site and the host unloads
 * the offender, instead of the session ending.
 *
 * This is a containment measure, not a sandbox.  See
 * gowl_bar_guard_init() for exactly what it does and does not promise.
 */

/* The signals a plugin can plausibly raise on itself.  SIGABRT is in
   the set because g_assert and g_error are the most likely way a
   plugin dies, and losing a session to somebody's failed assertion is
   exactly the outcome this exists to prevent. */
static const gint guarded_signals[] = {
	SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT
};

#define N_GUARDED_SIGNALS ((gint)(sizeof(guarded_signals) / sizeof(gint)))

static struct sigaction  previous[N_GUARDED_SIGNALS];
static gboolean          guard_ready;
static gsize             guard_init_once;
static volatile guint    guard_fault_count;

/* Per-thread landing point.  Thread-local because a worker thread
   polling a plugin must not longjmp into the compositor thread's
   frame, which is a far worse crash than the one being caught. */
static __thread sigjmp_buf guard_env;
static __thread gboolean   guard_armed;
static __thread volatile gint guard_signo;

/* Look up the saved disposition for @signo, or NULL when it is not one
   we installed. */
static struct sigaction *
guard_previous_for(gint signo)
{
	gint i;

	for (i = 0; i < N_GUARDED_SIGNALS; i++) {
		if (guarded_signals[i] == signo)
			return &previous[i];
	}
	return NULL;
}

/*
 * Async-signal-safe.  Everything here is either a load of a
 * thread-local, a siglongjmp, or a re-raise; no allocation, no
 * logging, no locks.  The message the user eventually sees is written
 * by gowl_bar_guard_call() after the unwind, where it is safe to.
 */
static void
guard_handler(int signo, siginfo_t *info, void *ctx)
{
	struct sigaction *prev;

	if (guard_armed) {
		guard_armed = FALSE;
		guard_signo = signo;
		siglongjmp(guard_env, 1);
	}

	/* Not inside a guarded call: hand the fault back to whoever had
	   it before, so a real compositor bug still cores exactly as it
	   would have without the bar loaded. */
	prev = guard_previous_for(signo);
	if (prev != NULL) {
		if ((prev->sa_flags & SA_SIGINFO) != 0 &&
		    prev->sa_sigaction != NULL) {
			prev->sa_sigaction(signo, info, ctx);
			return;
		}
		if (prev->sa_handler != NULL &&
		    prev->sa_handler != SIG_DFL &&
		    prev->sa_handler != SIG_IGN) {
			prev->sa_handler(signo);
			return;
		}
	}

	/* Default action: restore it and re-raise so the kernel produces
	   the core dump and the right exit status. */
	{
		struct sigaction dfl;

		memset(&dfl, 0, sizeof(dfl));
		dfl.sa_handler = SIG_DFL;
		sigemptyset(&dfl.sa_mask);
		sigaction(signo, &dfl, NULL);
		raise(signo);
	}
}

static void
guard_install(void)
{
	struct sigaction sa;
	stack_t          alt;
	gint             i;
	gboolean         ok;

	/* A stack overflow inside a plugin arrives as SIGSEGV with no
	   usable stack left; without an alternate stack the handler
	   itself faults and the process dies anyway. */
	memset(&alt, 0, sizeof(alt));
	alt.ss_size  = (SIGSTKSZ > 65536) ? (size_t)SIGSTKSZ : 65536;
	alt.ss_sp    = malloc(alt.ss_size);
	alt.ss_flags = 0;
	if (alt.ss_sp != NULL) {
		if (sigaltstack(&alt, NULL) != 0)
			free(alt.ss_sp);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = guard_handler;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigemptyset(&sa.sa_mask);

	ok = TRUE;
	for (i = 0; i < N_GUARDED_SIGNALS; i++) {
		if (sigaction(guarded_signals[i], &sa, &previous[i]) != 0)
			ok = FALSE;
	}

	guard_ready = ok;
	if (!ok) {
		g_warning("gowl-bar: fault guard could not be installed; "
		          "a faulting plugin will take the session down");
	}
}

/**
 * gowl_bar_guard_init:
 *
 * Installs the fault handlers.  Idempotent.
 */
void
gowl_bar_guard_init(void)
{
	if (g_once_init_enter(&guard_init_once)) {
		guard_install();
		g_once_init_leave(&guard_init_once, 1);
	}
}

/**
 * gowl_bar_guard_is_available:
 *
 * Returns: %TRUE when the handlers are installed
 */
gboolean
gowl_bar_guard_is_available(void)
{
	return guard_ready;
}

/**
 * gowl_bar_guard_signal_name:
 * @signo: a signal number
 *
 * Returns: (transfer none): a short name for @signo
 */
const gchar *
gowl_bar_guard_signal_name(gint signo)
{
	switch (signo) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGFPE:  return "SIGFPE";
	case SIGILL:  return "SIGILL";
	case SIGABRT: return "SIGABRT";
	default:      return "signal";
	}
}

/**
 * gowl_bar_guard_get_fault_count:
 *
 * Returns: how many faults have been caught this session
 */
guint
gowl_bar_guard_get_fault_count(void)
{
	return guard_fault_count;
}

/**
 * gowl_bar_guard_call:
 * @func: the callback to run
 * @user_data: passed to @func
 * @signo: (out) (optional): the signal that fired
 * @what: (nullable): a description for the log
 *
 * Returns: %TRUE when @func returned normally
 */
gboolean
gowl_bar_guard_call(GowlBarGuardFunc func, gpointer user_data,
                    gint *signo, const gchar *what)
{
	gboolean nested;
	gint     caught;

	if (signo != NULL)
		*signo = 0;
	if (func == NULL)
		return TRUE;

	gowl_bar_guard_init();

	if (!guard_ready) {
		func(user_data);
		return TRUE;
	}

	/* Nested guards would give the inner frame the landing point and
	   leave the outer frame's jmp_buf stale, so an inner fault would
	   unwind to a frame that had already returned. */
	nested = guard_armed;
	if (nested) {
		func(user_data);
		return TRUE;
	}

	guard_signo = 0;
	if (sigsetjmp(guard_env, 1) != 0) {
		caught = guard_signo;
		guard_armed = FALSE;
		guard_fault_count++;
		if (signo != NULL)
			*signo = caught;
		g_warning("gowl-bar: caught %s in %s -- the offending "
		          "component will be unloaded",
		          gowl_bar_guard_signal_name(caught),
		          (what != NULL) ? what : "a bar callback");
		return FALSE;
	}

	guard_armed = TRUE;
	func(user_data);
	guard_armed = FALSE;
	return TRUE;
}
