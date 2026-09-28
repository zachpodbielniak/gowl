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
 * gowl-fault-guard.c - see the header.
 *
 * Grew out of the bar's plugin guard, which it now serves too
 * (gowl-bar-guard.c is a wrapper).  Two things were added on the way:
 *
 *   - An alternate signal stack PER THREAD.  sigaltstack() is a
 *     per-thread setting, and the bar guard installed one only on
 *     whichever thread initialised it first -- so a stack overflow on
 *     any other thread (the compositor thread under cmacs, a worker)
 *     arrived with no stack to run the handler on and killed the
 *     process anyway.
 *   - A watchdog.  A crash can be caught; an infinite loop just
 *     freezes the desktop.  Each guarded call may carry a budget: a
 *     POSIX timer aimed at the calling thread (SIGEV_THREAD_ID) fires a
 *     real-time signal when it runs out, and the handler unwinds exactly
 *     as for a fault.  The running call can re-arm its own budget with
 *     gowl_fault_guard_extend() -- a macro that knows it is long says so.
 */

#define _GNU_SOURCE
#include "util/gowl-fault-guard.h"

#include <errno.h>
#include <setjmp.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The fatal signals user code can plausibly raise on itself.  SIGABRT
   is in because g_assert and g_error are the most likely way it dies. */
static const gint fault_signals[] = {
	SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT
};

#define N_FAULT_SIGNALS ((gint)(sizeof(fault_signals) / sizeof(gint)))

/* The watchdog's signal, an offset into the real-time range.  Chosen
   clear of SIGRTMIN+6 (40 on glibc), which cmacs points JSC's GC at
   (JSC_SIGNAL_FOR_GC=40). */
#define WATCHDOG_RT_OFFSET (9)

/* Room for the handler on a thread whose own stack just overflowed. */
#define ALT_STACK_SIZE (65536)

/* glibc exposes the target thread of SIGEV_THREAD_ID only through the
   union member; the sigev_notify_thread_id spelling is newer than some
   of the libcs gowl builds against. */
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

static struct sigaction previous[N_FAULT_SIGNALS];
static struct sigaction previous_watchdog;
static gboolean         guard_ready;
static gint             watchdog_signo;
static gsize            guard_init_once;
static volatile guint   guard_fault_count;

/* Per-thread landing point and state.  Thread-local because a worker
   must never longjmp into another thread's frame, which is a far worse
   crash than the one being caught. */
static __thread sigjmp_buf    guard_env;
static __thread gboolean      guard_armed;
static __thread volatile gint guard_signo;
/* gowl_fault_guard_hold() nesting depth on this thread */
static __thread guint         guard_hold_depth;

/* Per-thread resources, released at thread exit through the GPrivate
   destroy notify. */
typedef struct {
	gpointer alt_stack;
	timer_t  timer;
	gboolean has_timer;
} GuardThread;

static void guard_thread_free(gpointer data);
static GPrivate guard_thread_key = G_PRIVATE_INIT(guard_thread_free);

static struct sigaction *
previous_for(gint signo)
{
	gint i;

	if (signo == watchdog_signo)
		return &previous_watchdog;
	for (i = 0; i < N_FAULT_SIGNALS; i++) {
		if (fault_signals[i] == signo)
			return &previous[i];
	}
	return NULL;
}

/*
 * Async-signal-safe: loads of thread-locals, a siglongjmp, or a
 * re-raise.  No allocation, no logging, no locks.  The message the user
 * sees is written by gowl_fault_guard_call() after the unwind.
 */
static void
guard_handler(
	int        signo,
	siginfo_t *info,
	void      *ctx
){
	struct sigaction *prev;

	if (guard_armed) {
		guard_armed = FALSE;
		guard_signo = signo;
		siglongjmp(guard_env, 1);
	}

	/* A watchdog tick that lost the race with the end of its call has
	   nothing to unwind: drop it. */
	if (signo == watchdog_signo)
		return;

	/* Not inside a guarded call: hand the fault to whoever had it
	   before -- Emacs's handler under cmacs, the default elsewhere --
	   so a real compositor bug still behaves exactly as it would have. */
	prev = previous_for(signo);
	if (prev != NULL) {
		if ((prev->sa_flags & SA_SIGINFO) != 0
		    && prev->sa_sigaction != NULL) {
			prev->sa_sigaction(signo, info, ctx);
			return;
		}
		if (prev->sa_handler != NULL && prev->sa_handler != SIG_DFL
		    && prev->sa_handler != SIG_IGN) {
			prev->sa_handler(signo);
			return;
		}
	}

	/* Default action: restore it and re-raise, for the core dump and
	   the right exit status. */
	{
		struct sigaction dfl;

		memset(&dfl, 0, sizeof dfl);
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
	gint i;
	gboolean ok;

	watchdog_signo = SIGRTMIN + WATCHDOG_RT_OFFSET;

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = guard_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigemptyset(&sa.sa_mask);

	ok = TRUE;
	for (i = 0; i < N_FAULT_SIGNALS; i++) {
		if (sigaction(fault_signals[i], &sa, &previous[i]) != 0)
			ok = FALSE;
	}
	if (watchdog_signo > SIGRTMAX
	    || sigaction(watchdog_signo, &sa, &previous_watchdog) != 0)
		watchdog_signo = 0;

	guard_ready = ok;
	if (!ok)
		g_warning("gowl: fault guard could not be installed; a "
		          "faulting plugin or macro will take the session down");
}

/**
 * gowl_fault_guard_init:
 *
 * Installs the process-wide handlers.  Idempotent and thread-safe; the
 * per-thread half is gowl_fault_guard_thread_init(), which
 * gowl_fault_guard_call() runs on first use in each thread.
 */
void
gowl_fault_guard_init(void)
{
	if (g_once_init_enter(&guard_init_once)) {
		guard_install();
		g_once_init_leave(&guard_init_once, 1);
	}
}

/**
 * gowl_fault_guard_is_available:
 *
 * Returns: %TRUE when the fault handlers are installed
 */
gboolean
gowl_fault_guard_is_available(void)
{
	return guard_ready;
}

static void
guard_thread_free(gpointer data)
{
	GuardThread *t;
	stack_t cur;

	t = (GuardThread *)data;
	if (t == NULL)
		return;
	if (t->has_timer)
		timer_delete(t->timer);
	if (t->alt_stack != NULL) {
		/* Only free a stack that is ours and not in use. */
		if (sigaltstack(NULL, &cur) == 0 && cur.ss_sp == t->alt_stack
		    && (cur.ss_flags & SS_ONSTACK) == 0) {
			stack_t off;

			memset(&off, 0, sizeof off);
			off.ss_flags = SS_DISABLE;
			sigaltstack(&off, NULL);
			free(t->alt_stack);
		}
	}
	g_free(t);
}

/**
 * gowl_fault_guard_thread_init:
 *
 * Prepares the calling thread: an alternate signal stack if the thread
 * has none (one already set up -- Emacs's, on its main thread -- is
 * kept), and the thread's watchdog timer.  Idempotent; released at
 * thread exit.  Call it early on a thread that will run guarded code
 * deep in its stack, so a stack overflow is catchable.
 */
void
gowl_fault_guard_thread_init(void)
{
	GuardThread *t;
	stack_t cur;

	gowl_fault_guard_init();
	if (g_private_get(&guard_thread_key) != NULL)
		return;

	t = g_new0(GuardThread, 1);

	/* A stack overflow arrives as SIGSEGV with no stack left to run
	   the handler on; without an alternate stack it dies anyway. */
	if (sigaltstack(NULL, &cur) == 0 && (cur.ss_flags & SS_DISABLE) != 0) {
		stack_t alt;

		memset(&alt, 0, sizeof alt);
		alt.ss_size = (SIGSTKSZ > ALT_STACK_SIZE) ? (size_t)SIGSTKSZ
		                                          : ALT_STACK_SIZE;
		alt.ss_sp = malloc(alt.ss_size);
		if (alt.ss_sp != NULL) {
			if (sigaltstack(&alt, NULL) == 0)
				t->alt_stack = alt.ss_sp;
			else
				free(alt.ss_sp);
		}
	}

	/* The watchdog: a timer whose expiry is delivered to THIS thread,
	   so it interrupts the call that is running over, not whichever
	   thread happens to take a process-directed signal. */
	if (watchdog_signo != 0) {
		struct sigevent sev;

		memset(&sev, 0, sizeof sev);
		sev.sigev_notify = SIGEV_THREAD_ID;
		sev.sigev_signo = watchdog_signo;
		sev.sigev_notify_thread_id = (pid_t)syscall(SYS_gettid);
		if (timer_create(CLOCK_MONOTONIC, &sev, &t->timer) == 0)
			t->has_timer = TRUE;
	}

	g_private_set(&guard_thread_key, t);
}

/* Arms (or, with 0, disarms) the calling thread's watchdog. */
static gboolean
watchdog_arm(guint timeout_ms)
{
	GuardThread *t;
	struct itimerspec its;

	t = (GuardThread *)g_private_get(&guard_thread_key);
	if (t == NULL || !t->has_timer)
		return FALSE;
	memset(&its, 0, sizeof its);
	its.it_value.tv_sec = (time_t)(timeout_ms / 1000);
	its.it_value.tv_nsec = (glong)(timeout_ms % 1000) * 1000000L;
	return timer_settime(t->timer, 0, &its, NULL) == 0;
}

/**
 * gowl_fault_guard_call:
 * @func: the callback to run
 * @user_data: passed to @func
 * @timeout_ms: the watchdog budget, 0 for none
 * @signo: (out) (optional): the signal that ended the call, 0 when it
 *   returned normally; the watchdog signal when it ran out of time
 * @what: (nullable): a description for the log, e.g. `macro "sort"'
 *
 * Runs @func with the guard armed for the calling thread.
 *
 * Guards do not nest: a nested call runs @func directly (under the
 * outer budget), so an inner frame cannot steal the outer frame's
 * landing point.  A %FALSE return means the callback is not trustworthy
 * any more; nothing it held is released.
 *
 * Returns: %TRUE when @func returned normally, %FALSE when a fatal
 *   signal or the watchdog ended it
 */
gboolean
gowl_fault_guard_call(
	GowlFaultGuardFunc  func,
	gpointer            user_data,
	guint               timeout_ms,
	gint               *signo,
	const gchar        *what
){
	gint caught;

	if (signo != NULL)
		*signo = 0;
	if (func == NULL)
		return TRUE;

	gowl_fault_guard_thread_init();
	if (!guard_ready) {
		func(user_data);
		return TRUE;
	}

	if (guard_armed) {
		func(user_data);
		return TRUE;
	}

	guard_signo = 0;
	if (sigsetjmp(guard_env, 1) != 0) {
		caught = guard_signo;
		guard_armed = FALSE;
		/* sigsetjmp(.., 1) restored the signal mask of the call's
		   start, so any hold taken inside it is gone with the frame */
		guard_hold_depth = 0;
		watchdog_arm(0);
		guard_fault_count++;
		if (signo != NULL)
			*signo = caught;
		if (caught == watchdog_signo)
			g_warning("gowl: %s ran past its time budget and was "
			          "stopped", (what != NULL) ? what : "a guarded call");
		else
			g_warning("gowl: caught %s in %s",
			          gowl_fault_guard_signal_name(caught),
			          (what != NULL) ? what : "a guarded call");
		return FALSE;
	}

	guard_armed = TRUE;
	if (timeout_ms > 0)
		watchdog_arm(timeout_ms);
	func(user_data);
	guard_armed = FALSE;
	watchdog_arm(0);
	return TRUE;
}

/**
 * gowl_fault_guard_extend:
 * @timeout_ms: the new budget, from now; 0 switches the watchdog off
 *   for the rest of the call
 *
 * Re-arms the running call's watchdog.  For code that knows it is long:
 * a macro calls this first thing to say "I need ten seconds".
 *
 * Returns: %TRUE when called inside a guarded call on this thread
 */
gboolean
gowl_fault_guard_extend(guint timeout_ms)
{
	if (!guard_armed)
		return FALSE;
	watchdog_arm(timeout_ms);
	return TRUE;
}

/**
 * gowl_fault_guard_hold:
 *
 * Defers the watchdog on the calling thread until the matching
 * gowl_fault_guard_release().  Code that takes a lock another thread
 * also takes wraps the locked region in a hold, because a watchdog
 * unwind inside it would leave the lock held for good -- and the other
 * thread is usually the compositor's, so that is a frozen desktop.  A
 * watchdog that fires during the hold is delivered at the release, once
 * the lock is let go.  Holds nest.  Crashes are not deferred.
 */
void
gowl_fault_guard_hold(void)
{
	sigset_t set;

	gowl_fault_guard_init();
	if (watchdog_signo == 0)
		return;
	if (guard_hold_depth++ > 0)
		return;
	sigemptyset(&set);
	sigaddset(&set, watchdog_signo);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/**
 * gowl_fault_guard_release:
 *
 * Ends a gowl_fault_guard_hold().  A watchdog that fired meanwhile
 * unwinds the guarded call here.  An unmatched release does nothing.
 */
void
gowl_fault_guard_release(void)
{
	sigset_t set;

	if (watchdog_signo == 0 || guard_hold_depth == 0)
		return;
	if (--guard_hold_depth > 0)
		return;
	sigemptyset(&set);
	sigaddset(&set, watchdog_signo);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

/**
 * gowl_fault_guard_is_armed:
 *
 * Returns: %TRUE inside a guarded call on the calling thread
 */
gboolean
gowl_fault_guard_is_armed(void)
{
	return guard_armed;
}

/**
 * gowl_fault_guard_watchdog_signal:
 *
 * Returns: the watchdog's signal number, 0 when there is no watchdog
 */
gint
gowl_fault_guard_watchdog_signal(void)
{
	gowl_fault_guard_init();
	return watchdog_signo;
}

/**
 * gowl_fault_guard_is_watchdog:
 * @signo: a signal number from gowl_fault_guard_call()
 *
 * Returns: %TRUE when @signo means "ran out of time" rather than a fault
 */
gboolean
gowl_fault_guard_is_watchdog(gint signo)
{
	return signo != 0 && signo == watchdog_signo;
}

/**
 * gowl_fault_guard_signal_name:
 * @signo: a signal number
 *
 * Returns: (transfer none): a short name for @signo
 */
const gchar *
gowl_fault_guard_signal_name(gint signo)
{
	if (signo != 0 && signo == watchdog_signo)
		return "timeout";
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
 * gowl_fault_guard_get_fault_count:
 *
 * Returns: how many faults and timeouts have been caught this session
 */
guint
gowl_fault_guard_get_fault_count(void)
{
	return guard_fault_count;
}
