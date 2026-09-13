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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-logind"

#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "gowl-logind.h"
#include "gowl-compositor.h"
#include "gowl-core-private.h"
#include "util/gowl-systemd.h"

#define LOGIND_BUS       "org.freedesktop.login1"
#define LOGIND_PATH      "/org/freedesktop/login1"
#define LOGIND_MANAGER   "org.freedesktop.login1.Manager"
#define LOGIND_SESSION   "org.freedesktop.login1.Session"

/* How long the suspend path waits for the lock screen to actually be up
 * before it lets the machine sleep anyway.  logind's own
 * InhibitDelayMaxSec defaults to 5 s and it will take the inhibitor away
 * at that point regardless, so this stays comfortably under it: better
 * to suspend a fraction of a second early than to have logind decide we
 * are broken. */
#define LOCK_WAIT_US     (G_USEC_PER_SEC * 4)
#define LOCK_POLL_US     (10 * 1000)

/* What the bus thread asks the compositor thread to do.  A bitmask
 * rather than a queue: the only two requests are idempotent, and
 * collapsing a burst of them is the right answer anyway. */
#define REQ_LOCK   (1u << 0)
#define REQ_UNLOCK (1u << 1)

struct _GowlLogind {
	GObject         parent_instance;

	GowlCompositor *compositor;   /* weak: it owns us */

	/* Bus thread */
	GThread        *thread;
	GMainContext   *ctx;
	GMainLoop      *loop;
	GDBusConnection *conn;
	gchar          *session_path;  /* our own login session object */
	gboolean        own_session;   /* ... and whether it is ours to report on */
	gboolean        inhibit_sleep;
	gint            inhibit_fd;
	guint           sleep_sub;
	guint           lock_sub;
	guint           unlock_sub;

	/* Compositor thread */
	struct wl_event_source *wake_source;
	gint            wake_fd;

	/* Shared */
	GMutex          mutex;
	guint           requests;      /* REQ_* bits */
	gint            locked;        /* atomic: the screen's state */
	gint            stopping;      /* atomic: stop() has been called */
	gboolean        started;
};

G_DEFINE_FINAL_TYPE(GowlLogind, gowl_logind, G_TYPE_OBJECT)

/* ------------------------------------------------------------------
 * Compositor thread
 * ------------------------------------------------------------------ */

/* The eventfd is readable: run what the bus thread asked for.  This runs
 * on the compositor's thread, which under cmacs --gowl is the dispatch
 * thread holding the gowl lock -- the same place every IPC command and
 * keybind runs, so locking from here is no different from a keypress. */
static int
on_wake(int fd, uint32_t mask, void *data)
{
	GowlLogind *self = (GowlLogind *)data;
	guint64 val;
	ssize_t r;
	guint requests;

	(void)mask;
	r = read(fd, &val, sizeof val);
	(void)r;

	g_mutex_lock(&self->mutex);
	requests = self->requests;
	self->requests = 0;
	g_mutex_unlock(&self->mutex);

	if (self->compositor == NULL)
		return 0;

	/* Unlock first when both arrived in one burst: a suspend that is
	 * cancelled sends PrepareForSleep(false) with no unlock, so the only
	 * way to see both is lock-then-unlock, and the last one wins. */
	if ((requests & REQ_LOCK) != 0)
		gowl_compositor_lock_session(self->compositor);
	if ((requests & REQ_UNLOCK) != 0)
		gowl_compositor_unlock_session(self->compositor);
	return 0;
}

/* Ask the compositor thread for something, from the bus thread. */
static void
post_request(GowlLogind *self, guint bits)
{
	guint64 one = 1;
	ssize_t w;

	g_mutex_lock(&self->mutex);
	self->requests |= bits;
	g_mutex_unlock(&self->mutex);

	if (self->wake_fd < 0)
		return;
	w = write(self->wake_fd, &one, sizeof one);
	if (w < 0 && errno != EAGAIN)
		g_warning("could not wake the compositor thread: %s",
		          g_strerror(errno));
}

/* Runs on the bus thread: tell logind what the screen is doing, so
 * `loginctl show-session' and anything reading LockedHint agree with
 * it.  Fire-and-forget -- nothing waits on the hint, and a failure to
 * set it is not a reason to do anything differently. */
static gboolean
send_locked_hint(gpointer data)
{
	GowlLogind *self = (GowlLogind *)data;

	if (self->conn != NULL && self->session_path != NULL)
		g_dbus_connection_call(self->conn, LOGIND_BUS,
			self->session_path, LOGIND_SESSION, "SetLockedHint",
			g_variant_new("(b)", g_atomic_int_get(&self->locked) != 0),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
	return G_SOURCE_REMOVE;
}

void
gowl_logind_set_locked(GowlLogind *self, gboolean locked)
{
	g_return_if_fail(GOWL_IS_LOGIND(self));

	g_atomic_int_set(&self->locked, locked ? 1 : 0);

	/* SetLockedHint is a bus call, so it has to happen on the bus
	 * thread; an idle on that thread's context is how it gets there.
	 * The reference keeps this object alive until it runs, since the
	 * compositor could be tearing down in between. */
	if (self->ctx == NULL || !self->own_session)
		return;
	g_main_context_invoke_full(self->ctx, G_PRIORITY_DEFAULT,
	                           send_locked_hint, g_object_ref(self),
	                           g_object_unref);
}

/* ------------------------------------------------------------------
 * Bus thread
 * ------------------------------------------------------------------ */

/* Take (or re-take) the delay inhibitor.  While the returned fd is open
 * logind will not suspend, which is the whole mechanism: without it,
 * PrepareForSleep is an announcement rather than a chance to act. */
static void
take_inhibitor(GowlLogind *self)
{
	GVariant     *reply;
	GUnixFDList  *fds = NULL;
	GError       *error = NULL;
	gint          handle = -1;

	if (!self->inhibit_sleep || self->conn == NULL || self->inhibit_fd >= 0)
		return;

	reply = g_dbus_connection_call_with_unix_fd_list_sync(
		self->conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MANAGER, "Inhibit",
		g_variant_new("(ssss)", "sleep", "gowl",
		              "Locking the session before sleep", "delay"),
		G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1,
		NULL, &fds, NULL, &error);
	if (reply == NULL) {
		g_message("no sleep inhibitor (%s); the screen will not lock "
		          "itself before a suspend", error->message);
		g_error_free(error);
		return;
	}
	g_variant_get(reply, "(h)", &handle);
	g_variant_unref(reply);

	if (fds != NULL) {
		self->inhibit_fd = g_unix_fd_list_get(fds, handle, NULL);
		g_object_unref(fds);
	}
	if (self->inhibit_fd < 0)
		g_message("logind gave no inhibitor fd; suspend will not wait "
		          "for the lock");
	else
		g_debug("sleep inhibitor taken");
}

static void
drop_inhibitor(GowlLogind *self)
{
	if (self->inhibit_fd >= 0) {
		close(self->inhibit_fd);
		self->inhibit_fd = -1;
		g_debug("sleep inhibitor released");
	}
}

/*
 * PrepareForSleep(true) -- the machine is about to suspend, and is
 * waiting on our inhibitor to do it.
 *
 * Lock, then wait for the lock to actually be up before letting go.
 * Releasing the inhibitor first would be the same race as having no
 * inhibitor at all: the screen would be locked by a process that is
 * about to be frozen mid-startup, and the wake-up would show whatever
 * it had drawn by then -- which for a lock program that has not mapped
 * yet is the desktop.
 *
 * The wait is bounded.  A lock program that never comes up must not be
 * able to stop the machine from sleeping; the screen then goes dark
 * unlocked, which is bad, but a laptop that will not suspend in a bag is
 * worse, and logind would take the inhibitor away at its own deadline
 * regardless.
 */
static void
on_prepare_for_sleep(GDBusConnection *conn, const gchar *sender,
                     const gchar *path, const gchar *iface,
                     const gchar *signal, GVariant *params,
                     gpointer user_data)
{
	GowlLogind *self = (GowlLogind *)user_data;
	gboolean going_to_sleep = FALSE;
	gint64 deadline;

	(void)conn; (void)sender; (void)path; (void)iface; (void)signal;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(b)")))
		return;
	g_variant_get(params, "(b)", &going_to_sleep);

	if (!going_to_sleep) {
		/* Awake again: arm for the next time. */
		take_inhibitor(self);
		return;
	}

	if (g_atomic_int_get(&self->locked)) {
		drop_inhibitor(self);
		return;
	}

	g_message("suspending: locking the session first");
	post_request(self, REQ_LOCK);

	deadline = g_get_monotonic_time() + LOCK_WAIT_US;
	while (!g_atomic_int_get(&self->locked)
	       && g_get_monotonic_time() < deadline)
		g_usleep(LOCK_POLL_US);

	if (!g_atomic_int_get(&self->locked))
		g_warning("the lock screen did not come up in time; suspending "
		          "anyway rather than blocking sleep");
	drop_inhibitor(self);
}

/* `loginctl lock-session' and `unlock-session'. */
static void
on_session_lock_signal(GDBusConnection *conn, const gchar *sender,
                       const gchar *path, const gchar *iface,
                       const gchar *signal, GVariant *params,
                       gpointer user_data)
{
	GowlLogind *self = (GowlLogind *)user_data;

	(void)conn; (void)sender; (void)path; (void)iface; (void)params;

	if (g_strcmp0(signal, "Lock") == 0)
		post_request(self, REQ_LOCK);
	else if (g_strcmp0(signal, "Unlock") == 0)
		post_request(self, REQ_UNLOCK);
}

/* This process's own login session object, or %NULL when there is not
 * one (a headless test, a container, a nested run started by something
 * logind never saw). */
static gchar *
resolve_session_path(GDBusConnection *conn)
{
	GVariant *reply;
	GError   *error = NULL;
	gchar    *path = NULL;

	reply = g_dbus_connection_call_sync(
		conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MANAGER,
		"GetSessionByPID", g_variant_new("(u)", (guint32)getpid()),
		G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (reply == NULL) {
		g_debug("no login session for this process: %s", error->message);
		g_error_free(error);
		return NULL;
	}
	g_variant_get(reply, "(o)", &path);
	g_variant_unref(reply);
	return path;
}

static gpointer
bus_thread(gpointer data)
{
	GowlLogind *self = (GowlLogind *)data;
	GError *error = NULL;

	g_main_context_push_thread_default(self->ctx);

	self->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (self->conn == NULL) {
		g_message("no system bus (%s); the session will not lock on "
		          "suspend or on `loginctl lock-session'",
		          error->message);
		g_error_free(error);
		g_main_context_pop_thread_default(self->ctx);
		return NULL;
	}

	self->sleep_sub = g_dbus_connection_signal_subscribe(
		self->conn, LOGIND_BUS, LOGIND_MANAGER, "PrepareForSleep",
		LOGIND_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_prepare_for_sleep, self, NULL);

	self->session_path = resolve_session_path(self->conn);
	if (self->session_path != NULL) {
		self->lock_sub = g_dbus_connection_signal_subscribe(
			self->conn, LOGIND_BUS, LOGIND_SESSION, "Lock",
			self->session_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			on_session_lock_signal, self, NULL);
		self->unlock_sub = g_dbus_connection_signal_subscribe(
			self->conn, LOGIND_BUS, LOGIND_SESSION, "Unlock",
			self->session_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			on_session_lock_signal, self, NULL);
	}

	take_inhibitor(self);

	/*
	 * Connecting to the bus takes a moment, and a compositor that
	 * starts and stops inside it -- a test, a failed startup -- would
	 * have called stop() before this thread ever reached the loop.
	 * g_main_loop_quit() on a loop that is not running yet is lost
	 * (g_main_loop_run sets is_running itself), so the loop would run
	 * forever and the join would never return.  The flag catches that
	 * window; the idle source stop() posts catches everything after it.
	 */
	if (!g_atomic_int_get(&self->stopping))
		g_main_loop_run(self->loop);

	drop_inhibitor(self);
	if (self->sleep_sub != 0)
		g_dbus_connection_signal_unsubscribe(self->conn, self->sleep_sub);
	if (self->lock_sub != 0)
		g_dbus_connection_signal_unsubscribe(self->conn, self->lock_sub);
	if (self->unlock_sub != 0)
		g_dbus_connection_signal_unsubscribe(self->conn, self->unlock_sub);
	self->sleep_sub = self->lock_sub = self->unlock_sub = 0;
	/* Unsubscribing drops references taken on this context; turn it
	 * until they are gone, or the connection outlives the thread. */
	while (g_main_context_iteration(self->ctx, FALSE))
		;
	g_clear_object(&self->conn);
	g_clear_pointer(&self->session_path, g_free);
	g_main_context_pop_thread_default(self->ctx);
	return NULL;
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/* Runs on the bus thread.  Posted rather than called directly so that a
 * quit issued before the loop starts is not lost: an idle attached to
 * the context waits there until the loop turns. */
static gboolean
quit_loop(gpointer data)
{
	GowlLogind *self = (GowlLogind *)data;

	if (self->loop != NULL)
		g_main_loop_quit(self->loop);
	return G_SOURCE_REMOVE;
}

void
gowl_logind_start(GowlLogind *self, gboolean inhibit_sleep)
{
	struct wl_event_loop *loop;

	g_return_if_fail(GOWL_IS_LOGIND(self));

	if (self->started || self->compositor == NULL)
		return;

	/*
	 * The same switch that turns off the systemd session integration
	 * turns this off, plus one of its own.  Every test in the tree sets
	 * GOWL_DISABLE_SYSTEMD, and a run that must not touch the user's
	 * session must not take a sleep inhibitor on it either.
	 */
	if (g_getenv("GOWL_DISABLE_LOGIND") != NULL
	    || g_getenv("GOWL_DISABLE_SYSTEMD") != NULL) {
		g_debug("logind integration disabled by the environment");
		return;
	}

	g_atomic_int_set(&self->stopping, 0);
	self->inhibit_sleep = inhibit_sleep;

	self->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (self->wake_fd < 0) {
		g_warning("eventfd: %s", g_strerror(errno));
		return;
	}
	loop = gowl_compositor_get_event_loop(self->compositor);
	if (loop == NULL) {
		close(self->wake_fd);
		self->wake_fd = -1;
		return;
	}
	self->wake_source = wl_event_loop_add_fd(loop, self->wake_fd,
	                                         WL_EVENT_READABLE, on_wake,
	                                         self);
	if (self->wake_source == NULL) {
		g_warning("could not watch the eventfd");
		close(self->wake_fd);
		self->wake_fd = -1;
		return;
	}

	/* Only a session gowl actually owns may have its locked hint set:
	 * a gowl nested inside another desktop shares that desktop's login
	 * session, and would be reporting on its behalf. */
	self->own_session = gowl_systemd_is_managing_session();

	self->ctx = g_main_context_new();
	self->loop = g_main_loop_new(self->ctx, FALSE);
	self->thread = g_thread_new("gowl-logind", bus_thread, self);
	self->started = TRUE;
}

void
gowl_logind_stop(GowlLogind *self)
{
	g_return_if_fail(GOWL_IS_LOGIND(self));

	/* The bus thread goes first: nothing may write the eventfd after
	 * its source has been removed. */
	g_atomic_int_set(&self->stopping, 1);
	if (self->thread != NULL) {
		g_main_context_invoke(self->ctx, quit_loop, self);
		g_main_context_wakeup(self->ctx);
		g_thread_join(self->thread);
		self->thread = NULL;
	}
	g_clear_pointer(&self->loop, g_main_loop_unref);
	g_clear_pointer(&self->ctx, g_main_context_unref);

	if (self->wake_source != NULL) {
		wl_event_source_remove(self->wake_source);
		self->wake_source = NULL;
	}
	if (self->wake_fd >= 0) {
		close(self->wake_fd);
		self->wake_fd = -1;
	}
	self->started = FALSE;
}

GowlLogind *
gowl_logind_new(GowlCompositor *compositor)
{
	GowlLogind *self;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(compositor), NULL);

	self = (GowlLogind *)g_object_new(GOWL_TYPE_LOGIND, NULL);
	self->compositor = compositor;
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&self->compositor);
	return self;
}

static void
gowl_logind_finalize(GObject *object)
{
	GowlLogind *self = GOWL_LOGIND(object);

	gowl_logind_stop(self);
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
	g_mutex_clear(&self->mutex);
	G_OBJECT_CLASS(gowl_logind_parent_class)->finalize(object);
}

static void
gowl_logind_class_init(GowlLogindClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_logind_finalize;
}

static void
gowl_logind_init(GowlLogind *self)
{
	g_mutex_init(&self->mutex);
	self->wake_fd    = -1;
	self->inhibit_fd = -1;
}
