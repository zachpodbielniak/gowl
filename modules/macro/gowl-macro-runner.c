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
 * gowl-macro-runner.c - see the header.
 *
 * The part that is easy to get wrong is the thread hop.  A worker that
 * wants a step performed pushes a SyncCall onto the queue, pokes the
 * eventfd, and waits.  The compositor thread runs it (under the guard:
 * gowl_macro_call_on_compositor() runs the macro's own code there) and
 * signals.  Two things must never happen:
 *
 *   - the worker waits for ever: it wakes every 50 ms to check whether
 *     it was cancelled or the module went away, and then ABANDONS a call
 *     the compositor has not started -- the compositor skips abandoned
 *     calls, so it never touches the worker's (by then gone) stack;
 *   - the compositor waits for the worker: nothing on the compositor
 *     side ever blocks on a worker.  Stop cancels and moves on; the
 *     worker's DONE message is handled whenever it arrives.  Under
 *     cmacs a blocking join would deadlock outright: the Emacs thread
 *     holds the gowl lock while the dispatch thread that would service
 *     the worker waits for it.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include "gowl-macro-runner.h"
#include "util/gowl-fault-guard.h"
#include "core/gowl-compositor.h"

#include <errno.h>
#include <json-glib/json-glib.h>
#include <sys/eventfd.h>
#include <string.h>
#include <unistd.h>

/* A compositor-side call a threaded macro asked for gets this long. */
#define SYNC_CALL_TIMEOUT_MS (5000)
/* How often a waiting worker checks for cancellation. */
#define WAIT_SLICE_US (50 * G_TIME_SPAN_MILLISECOND)

typedef struct _Run Run;

struct _GowlMacroRunner {
	gint                            ref_count;
	GowlCompositor                 *compositor;   /* NULL once shut down */
	struct wl_event_loop           *loop;
	const GowlMacroRunnerCallbacks *cb;
	gpointer                        module;
	gboolean                        dead;

	guint                           next_id;
	GHashTable                     *runs;         /* id -> Run* */

	/* worker -> compositor */
	GMutex                          qlock;
	GQueue                          queue;        /* Message* */
	gint                            efd;
	struct wl_event_source         *efd_source;
};

struct _Run {
	gint              ref_count;
	GowlMacroRunner  *runner;       /* ref */
	guint             id;
	gchar            *name;
	GowlMacroContext *ctx;          /* ref */
	gboolean          threaded;
	guint             timeout_ms;
	gint64            started;

	/* timeline */
	GQueue            steps;        /* GowlMacroStep* */
	guint             max_steps;
	guint             n_queued;
	struct wl_event_source *timer;

	/* threaded */
	GowlMacroRunnerBody body;
	gpointer          body_data;
	gint              signo;        /* set by the worker */
	gboolean          body_ok;
};

typedef enum {
	CALL_PENDING,
	CALL_EXECUTING,
	CALL_DONE,
	CALL_ABANDONED
} CallState;

typedef struct {
	gint              ref_count;
	GMutex            lock;
	GCond             cond;
	CallState         state;
	gboolean          ok;
	GowlMacroSyncFunc func;
	gpointer          data;
	Run              *run;          /* ref */
} SyncCall;

typedef enum {
	MSG_SYNC,
	MSG_DONE
} MessageKind;

typedef struct {
	MessageKind kind;
	SyncCall   *call;               /* MSG_SYNC, ref */
	Run        *run;                /* MSG_DONE, ref */
} Message;

static void run_finish(Run *run, gboolean cancelled);

/* --- refcounting --- */

static Run *
run_ref(Run *run)
{
	g_atomic_int_inc(&run->ref_count);
	return run;
}

static void
run_unref(Run *run)
{
	if (run == NULL || !g_atomic_int_dec_and_test(&run->ref_count))
		return;
	g_queue_clear_full(&run->steps, (GDestroyNotify)gowl_macro_step_free);
	gowl_macro_context_unref(run->ctx);
	gowl_macro_runner_unref(run->runner);
	g_free(run->name);
	g_free(run);
}

static void
call_unref(SyncCall *c)
{
	if (c == NULL || !g_atomic_int_dec_and_test(&c->ref_count))
		return;
	g_mutex_clear(&c->lock);
	g_cond_clear(&c->cond);
	run_unref(c->run);
	g_free(c);
}

/**
 * gowl_macro_runner_ref: (skip)
 * @self: a runner
 *
 * Returns: (transfer full): @self
 */
GowlMacroRunner *
gowl_macro_runner_ref(GowlMacroRunner *self)
{
	g_atomic_int_inc(&self->ref_count);
	return self;
}

/**
 * gowl_macro_runner_unref: (skip)
 * @self: (nullable): a runner
 *
 * The last reference -- the module's, or the last worker's to finish --
 * frees it.
 */
void
gowl_macro_runner_unref(GowlMacroRunner *self)
{
	Message *m;

	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;
	while ((m = g_queue_pop_head(&self->queue)) != NULL) {
		call_unref(m->call);
		run_unref(m->run);
		g_free(m);
	}
	if (self->efd >= 0)
		close(self->efd);
	g_mutex_clear(&self->qlock);
	g_hash_table_unref(self->runs);
	g_free(self);
}

/* --- the queue --- */

static void
post(
	GowlMacroRunner *self,
	Message         *m
){
	guint64 one;
	ssize_t n;

	one = 1;
	gowl_fault_guard_hold();
	g_mutex_lock(&self->qlock);
	g_queue_push_tail(&self->queue, m);
	g_mutex_unlock(&self->qlock);
	gowl_fault_guard_release();
	if (self->efd >= 0) {
		n = write(self->efd, &one, sizeof one);
		(void)n;
	}
}

static void
sync_call_trampoline(gpointer data)
{
	SyncCall *c = data;

	c->func(c->data);
}

/* A compositor-side call on a worker's behalf, guarded: it may be the
   macro's own code (gowl_macro_call_on_compositor). */
static void
handle_sync(
	GowlMacroRunner *self,
	SyncCall        *c
){
	gint signo;
	gboolean ok;
	g_autofree gchar *what = NULL;

	g_mutex_lock(&c->lock);
	if (c->state == CALL_ABANDONED || self->dead) {
		c->state = CALL_ABANDONED;
		g_cond_broadcast(&c->cond);
		g_mutex_unlock(&c->lock);
		return;
	}
	c->state = CALL_EXECUTING;
	g_mutex_unlock(&c->lock);

	what = g_strdup_printf("macro \"%s\" (compositor call)", c->run->name);
	ok = gowl_fault_guard_call(sync_call_trampoline, c,
	                           SYNC_CALL_TIMEOUT_MS, &signo, what);
	if (!ok) {
		/* The whole run is untrustworthy now: stop it. */
		gowl_macro_context_cancel(c->run->ctx);
		if (self->cb->faulted != NULL)
			self->cb->faulted(self->module, c->run->id, c->run->name,
			                  signo);
	}

	g_mutex_lock(&c->lock);
	c->ok = ok;
	c->state = CALL_DONE;
	g_cond_broadcast(&c->cond);
	g_mutex_unlock(&c->lock);
}

/* A worker finished (normally, cancelled, or faulted). */
static void
handle_done(
	GowlMacroRunner *self,
	Run             *run
){
	if (!g_hash_table_contains(self->runs, GUINT_TO_POINTER(run->id)))
		return;
	if (run->signo != 0) {
		if (self->cb->journal != NULL)
			self->cb->journal(self->module, run->name, FALSE);
		if (self->cb->faulted != NULL)
			self->cb->faulted(self->module, run->id, run->name, run->signo);
		g_hash_table_remove(self->runs, GUINT_TO_POINTER(run->id));
		return;
	}
	run_finish(run, gowl_macro_is_cancelled(run->ctx));
}

static gint
on_queue_ready(
	gint     fd,
	guint32  mask,
	gpointer data
){
	GowlMacroRunner *self = data;
	guint64 v;
	ssize_t n;
	Message *m;

	(void)mask;
	n = read(fd, &v, sizeof v);
	(void)n;

	gowl_macro_runner_ref(self);
	for (;;) {
		g_mutex_lock(&self->qlock);
		m = g_queue_pop_head(&self->queue);
		g_mutex_unlock(&self->qlock);
		if (m == NULL)
			break;
		if (m->kind == MSG_SYNC)
			handle_sync(self, m->call);
		else if (!self->dead)
			handle_done(self, m->run);
		call_unref(m->call);
		run_unref(m->run);
		g_free(m);
	}
	gowl_macro_runner_unref(self);
	return 0;
}

/* --- the host vtable (what a context calls) --- */

static gboolean
host_queue_step(
	GowlMacroContext *ctx,
	GowlMacroStep    *step
){
	Run *run = gowl_macro_context_get_host_data(ctx);

	if (run->n_queued >= run->max_steps) {
		if (run->n_queued == run->max_steps && run->runner->cb->log != NULL)
			run->runner->cb->log(run->runner->module, run->name,
			                     "step limit reached; the rest are dropped "
			                     "(max-steps)");
		run->n_queued++;
		gowl_macro_step_free(step);
		return FALSE;
	}
	run->n_queued++;
	g_queue_push_tail(&run->steps, step);
	return TRUE;
}

static gboolean
host_invoke_sync(
	GowlMacroContext  *ctx,
	GowlMacroSyncFunc  func,
	gpointer           data
){
	Run *run = gowl_macro_context_get_host_data(ctx);
	GowlMacroRunner *self = run->runner;
	SyncCall *c;
	Message *m;
	gboolean ok;

	if (self->dead || gowl_macro_is_cancelled(ctx))
		return FALSE;

	c = g_new0(SyncCall, 1);
	c->ref_count = 2;                /* ours and the message's */
	g_mutex_init(&c->lock);
	g_cond_init(&c->cond);
	c->state = CALL_PENDING;
	c->func = func;
	c->data = data;
	c->run = run_ref(run);

	m = g_new0(Message, 1);
	m->kind = MSG_SYNC;
	m->call = c;
	post(self, m);

	/* Wait, but never for ever: give up on a call that has not started
	   once the run is cancelled or the module is gone.  A call already
	   executing is waited for -- its guard bounds it. */
	gowl_fault_guard_hold();
	g_mutex_lock(&c->lock);
	while (c->state != CALL_DONE && c->state != CALL_ABANDONED) {
		gint64 until = g_get_monotonic_time() + WAIT_SLICE_US;

		g_cond_wait_until(&c->cond, &c->lock, until);
		if (c->state == CALL_PENDING
		    && (self->dead || gowl_macro_is_cancelled(ctx)))
			c->state = CALL_ABANDONED;
	}
	ok = c->state == CALL_DONE && c->ok;
	g_mutex_unlock(&c->lock);
	gowl_fault_guard_release();
	call_unref(c);
	return ok;
}

typedef struct {
	gchar *summary;
	gchar *body;
	Run   *run;
} NotifyCall;

static void
notify_on_compositor(gpointer data)
{
	NotifyCall *n = data;

	if (n->run->runner->cb->notify != NULL)
		n->run->runner->cb->notify(n->run->runner->module, n->run->name,
		                           n->summary, n->body);
}

static void
host_notify(
	GowlMacroContext *ctx,
	const gchar      *summary,
	const gchar      *body
){
	Run *run = gowl_macro_context_get_host_data(ctx);
	NotifyCall n;

	n.summary = (gchar *)summary;
	n.body = (gchar *)body;
	n.run = run;
	/* The notification path touches the compositor: hop, if threaded */
	if (run->threaded)
		host_invoke_sync(ctx, notify_on_compositor, &n);
	else
		notify_on_compositor(&n);
}

static void
host_log(
	GowlMacroContext *ctx,
	const gchar      *message
){
	Run *run = gowl_macro_context_get_host_data(ctx);

	/* The log is a file (or stderr) with its own locking: fine from a
	   worker. */
	if (run->runner->cb->log != NULL)
		run->runner->cb->log(run->runner->module, run->name, message);
}

static const GowlMacroHost runner_host = {
	host_queue_step,
	host_invoke_sync,
	host_notify,
	host_log
};

/* --- lifecycle --- */

/**
 * gowl_macro_runner_new: (skip)
 * @compositor: the compositor
 * @loop: its event loop (timers and the worker queue live there)
 * @callbacks: what the module wants told, on the compositor thread
 * @module: passed to @callbacks
 *
 * Returns: (transfer full): a runner
 */
GowlMacroRunner *
gowl_macro_runner_new(
	GowlCompositor                 *compositor,
	struct wl_event_loop           *loop,
	const GowlMacroRunnerCallbacks *callbacks,
	gpointer                        module
){
	GowlMacroRunner *self;

	self = g_new0(GowlMacroRunner, 1);
	self->ref_count = 1;
	self->compositor = compositor;
	self->loop = loop;
	self->cb = callbacks;
	self->module = module;
	self->next_id = 1;
	self->runs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                   (GDestroyNotify)run_unref);
	g_mutex_init(&self->qlock);
	g_queue_init(&self->queue);
	self->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (self->efd >= 0 && loop != NULL)
		self->efd_source = wl_event_loop_add_fd(loop, self->efd,
		                                        WL_EVENT_READABLE,
		                                        on_queue_ready, self);
	return self;
}

/**
 * gowl_macro_runner_shutdown: (skip)
 * @self: a runner
 *
 * Cancels every run and detaches from the compositor.  Never waits for
 * a worker: one still running notices it was cancelled (or that the
 * runner is dead) at its next step, and drops its reference on exit.
 */
void
gowl_macro_runner_shutdown(GowlMacroRunner *self)
{
	GHashTableIter iter;
	gpointer value;

	g_return_if_fail(self != NULL);
	if (self->dead)
		return;
	self->dead = TRUE;
	g_hash_table_iter_init(&iter, self->runs);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Run *run = value;

		gowl_macro_context_cancel(run->ctx);
		if (run->timer != NULL) {
			wl_event_source_remove(run->timer);
			run->timer = NULL;
		}
	}
	g_hash_table_remove_all(self->runs);
	if (self->efd_source != NULL) {
		wl_event_source_remove(self->efd_source);
		self->efd_source = NULL;
	}
	self->compositor = NULL;
}

/* --- timeline --- */

static void
run_finish(
	Run      *run,
	gboolean  cancelled
){
	GowlMacroRunner *self = run->runner;

	if (run->timer != NULL) {
		wl_event_source_remove(run->timer);
		run->timer = NULL;
	}
	if (self->cb->journal != NULL)
		self->cb->journal(self->module, run->name, FALSE);
	if (self->cb->finished != NULL)
		self->cb->finished(self->module, run->id, run->name,
		                   gowl_macro_context_get_result(run->ctx), cancelled);
	g_hash_table_remove(self->runs, GUINT_TO_POINTER(run->id));
}

/*
 * Plays queued steps until a wait (re-arms the timer) or the end.  On
 * the compositor thread, from the run's timer.
 */
static gint
on_timeline_tick(gpointer data)
{
	Run *run = data;
	GowlMacroStep *step;

	if (run->runner->dead || run->runner->compositor == NULL)
		return 0;
	run_ref(run);
	while (!gowl_macro_is_cancelled(run->ctx)
	       && (step = g_queue_pop_head(&run->steps)) != NULL) {
		if (step->kind == GOWL_MACRO_STEP_WAIT) {
			wl_event_source_timer_update(run->timer,
			                             step->ms > 0 ? (gint)step->ms : 1);
			gowl_macro_step_free(step);
			run_unref(run);
			return 0;
		}
		gowl_macro_step_execute(run->runner->compositor, step);
		gowl_macro_step_free(step);
	}
	run_finish(run, gowl_macro_is_cancelled(run->ctx));
	run_unref(run);
	return 0;
}

typedef struct {
	GowlMacroRunnerBody body;
	gpointer            data;
	GowlMacroContext   *ctx;
	gboolean            ok;
} BodyCall;

static void
body_trampoline(gpointer data)
{
	BodyCall *b = data;

	b->ok = b->body(b->ctx, b->data);
}

/* --- threaded --- */

static gpointer
worker_main(gpointer data)
{
	Run *run = data;
	BodyCall b;
	Message *m;
	g_autofree gchar *what = NULL;

	gowl_fault_guard_thread_init();
	what = g_strdup_printf("macro \"%s\" (threaded)", run->name);
	b.body = run->body;
	b.data = run->body_data;
	b.ctx = run->ctx;
	b.ok = FALSE;
	if (!gowl_fault_guard_call(body_trampoline, &b, run->timeout_ms,
	                           &run->signo, what))
		gowl_macro_context_cancel(run->ctx);
	run->body_ok = b.ok;

	m = g_new0(Message, 1);
	m->kind = MSG_DONE;
	m->run = run;                    /* takes the worker's reference */
	post(run->runner, m);
	return NULL;
}

/* --- starting and stopping --- */

/**
 * gowl_macro_runner_start: (skip)
 * @self: a runner
 * @ctx: the run's context (the runner takes a reference)
 * @body: the macro body
 * @body_data: passed to @body
 * @timeout_ms: the watchdog budget, 0 for none
 * @max_steps: the most steps a timeline run may queue
 * @out_reply: (out) (transfer full): the IPC reply line
 *
 * Returns: the run's id, 0 when it faulted or could not start
 */
guint
gowl_macro_runner_start(
	GowlMacroRunner      *self,
	GowlMacroContext     *ctx,
	GowlMacroRunnerBody   body,
	gpointer              body_data,
	guint                 timeout_ms,
	guint                 max_steps,
	gchar               **out_reply
){
	Run *run;
	const gchar *result;

	g_return_val_if_fail(self != NULL && ctx != NULL && body != NULL, 0);
	*out_reply = NULL;
	if (self->dead) {
		*out_reply = g_strdup("ERROR the macro module is shutting down");
		return 0;
	}

	run = g_new0(Run, 1);
	run->ref_count = 1;
	run->runner = gowl_macro_runner_ref(self);
	run->id = self->next_id++;
	run->name = g_strdup(gowl_macro_get_name(ctx));
	run->ctx = gowl_macro_context_ref(ctx);
	run->threaded = gowl_macro_is_threaded(ctx);
	run->timeout_ms = timeout_ms;
	run->max_steps = max_steps;
	run->started = g_get_monotonic_time();
	run->body = body;
	run->body_data = body_data;
	g_queue_init(&run->steps);
	gowl_macro_context_set_host(ctx, &runner_host, run);
	gowl_macro_context_set_timeout_default(ctx, timeout_ms);
	g_hash_table_insert(self->runs, GUINT_TO_POINTER(run->id), run);

	if (self->cb->journal != NULL)
		self->cb->journal(self->module, run->name, TRUE);
	if (self->cb->started != NULL)
		self->cb->started(self->module, run->id, run->name, run->threaded);

	/* Threaded: the worker owns a reference until its DONE is handled */
	if (run->threaded) {
		GThread *t;
		GError *error = NULL;

		t = g_thread_try_new("gowl-macro", worker_main, run_ref(run), &error);
		if (t == NULL) {
			*out_reply = g_strdup_printf("ERROR no thread for %s: %s",
			                             run->name, error->message);
			g_error_free(error);
			run_unref(run);
			run_finish(run, TRUE);
			return 0;
		}
		g_thread_unref(t);
		*out_reply = g_strdup_printf("OK started %s id=%u threaded",
		                             run->name, run->id);
		return run->id;
	}

	/* Timeline: the body, guarded, here and now */
	{
		BodyCall b;
		gint signo;
		g_autofree gchar *what = NULL;
		guint id;

		what = g_strdup_printf("macro \"%s\"", run->name);
		b.body = body;
		b.data = body_data;
		b.ctx = ctx;
		b.ok = FALSE;
		id = run->id;
		if (!gowl_fault_guard_call(body_trampoline, &b, timeout_ms, &signo,
		                           what)) {
			if (self->cb->journal != NULL)
				self->cb->journal(self->module, run->name, FALSE);
			*out_reply = g_strdup_printf(
				"ERROR %s %s and was stopped",
				run->name, gowl_fault_guard_is_watchdog(signo)
				           ? "ran past its time budget"
				           : gowl_fault_guard_signal_name(signo));
			if (self->cb->faulted != NULL)
				self->cb->faulted(self->module, id, run->name, signo);
			g_hash_table_remove(self->runs, GUINT_TO_POINTER(id));
			return 0;
		}

		result = gowl_macro_context_get_result(ctx);
		if (!b.ok) {
			*out_reply = g_strdup_printf("ERROR %s failed%s%s", run->name,
			                             result != NULL ? ": " : "",
			                             result != NULL ? result : "");
			g_queue_clear_full(&run->steps,
			                   (GDestroyNotify)gowl_macro_step_free);
			run_finish(run, FALSE);
			return 0;
		}
		if (g_queue_is_empty(&run->steps)) {
			*out_reply = result != NULL
				? g_strdup_printf("OK %s: %s", run->name, result)
				: g_strdup_printf("OK ran %s", run->name);
			run_finish(run, FALSE);
			return id;
		}

		*out_reply = g_strdup_printf("OK started %s id=%u steps=%u",
		                             run->name, id,
		                             g_queue_get_length(&run->steps));
		if (self->loop != NULL) {
			run->timer = wl_event_loop_add_timer(self->loop,
			                                     on_timeline_tick, run);
			if (run->timer != NULL)
				wl_event_source_timer_update(run->timer, 1);
		}
		if (run->timer == NULL)
			on_timeline_tick(run);   /* no loop (a test): play now */
		return id;
	}
}

/**
 * gowl_macro_runner_stop: (skip)
 * @self: a runner
 * @which: (nullable): a macro name, a run id, or "all"/NULL
 *
 * Cancels matching runs.  A timeline run ends at once; a threaded one
 * is told and ends at its next step, sleep or cancellation check.
 *
 * Returns: how many runs were told to stop
 */
guint
gowl_macro_runner_stop(
	GowlMacroRunner *self,
	const gchar     *which
){
	g_autoptr(GPtrArray) hit = NULL;
	GHashTableIter iter;
	gpointer value;
	guint64 id;
	gboolean by_id;
	guint i;

	g_return_val_if_fail(self != NULL, 0);
	by_id = which != NULL && g_ascii_string_to_unsigned(which, 10, 1,
	                                                    G_MAXUINT, &id, NULL);
	hit = g_ptr_array_new_with_free_func((GDestroyNotify)run_unref);
	g_hash_table_iter_init(&iter, self->runs);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Run *run = value;

		if (which == NULL || g_strcmp0(which, "all") == 0
		    || (by_id && run->id == (guint)id)
		    || g_strcmp0(which, run->name) == 0)
			g_ptr_array_add(hit, run_ref(run));
	}
	for (i = 0; i < hit->len; i++) {
		Run *run = g_ptr_array_index(hit, i);

		gowl_macro_context_cancel(run->ctx);
		if (!run->threaded)
			run_finish(run, TRUE);
	}
	return hit->len;
}

/**
 * gowl_macro_runner_n_active: (skip)
 * @self: a runner
 *
 * Returns: runs still playing steps or running on a worker
 */
guint
gowl_macro_runner_n_active(GowlMacroRunner *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return g_hash_table_size(self->runs);
}

/**
 * gowl_macro_runner_is_running: (skip)
 * @self: a runner
 * @name: a macro name
 *
 * Returns: %TRUE when a run of @name is active
 */
gboolean
gowl_macro_runner_is_running(
	GowlMacroRunner *self,
	const gchar     *name
){
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, FALSE);
	g_hash_table_iter_init(&iter, self->runs);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (g_strcmp0(((Run *)value)->name, name) == 0)
			return TRUE;
	return FALSE;
}

/**
 * gowl_macro_runner_status_json: (skip)
 * @self: a runner
 *
 * Returns: (transfer full): the active runs as a JSON array
 */
gchar *
gowl_macro_runner_status_json(GowlMacroRunner *self)
{
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	GHashTableIter iter;
	gpointer value;
	gint64 now;

	b = json_builder_new();
	now = g_get_monotonic_time();
	json_builder_begin_array(b);
	g_hash_table_iter_init(&iter, self->runs);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Run *run = value;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "id");
		json_builder_add_int_value(b, run->id);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, run->name);
		json_builder_set_member_name(b, "threaded");
		json_builder_add_boolean_value(b, run->threaded);
		json_builder_set_member_name(b, "elapsed-ms");
		json_builder_add_int_value(b, (now - run->started) / 1000);
		json_builder_set_member_name(b, "steps-left");
		json_builder_add_int_value(b, g_queue_get_length(&run->steps));
		json_builder_set_member_name(b, "cancelled");
		json_builder_add_boolean_value(b, gowl_macro_is_cancelled(run->ctx));
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}
