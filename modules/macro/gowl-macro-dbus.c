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
 * gowl-macro-dbus.c - org.gowl.Macro1, for running macros from anything
 * on the session bus.  Opt-in (`dbus: true').
 *
 * The notifyd pattern: the bus lives on a thread of its own with a
 * private GMainContext (the compositor thread runs no GLib main loop,
 * and under cmacs the default context is the editor's).  A method call
 * is queued, the compositor is woken through an eventfd on its
 * wl_event_loop, the macro module answers there, and the reply goes
 * back through the invocation -- GDBus replies are thread-safe.
 *
 *   Run(s name, as args) -> (s reply)
 *   Stop(s which)        -> (s reply)      "" or "all" stops everything
 *   List()               -> (s json)
 *   Status()             -> (s json)
 *   signal Started(s name, s detail)
 *   signal Finished(s name, s result)
 *   signal Faulted(s name, s signal)
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include <gio/gio.h>
#include <sys/eventfd.h>
#include <string.h>
#include <unistd.h>

#include "gowl-module-macro.h"

#define MACRO_BUS_NAME  "org.gowl.Macro1"
#define MACRO_OBJ_PATH  "/org/gowl/Macro1"
#define MACRO_IFACE     "org.gowl.Macro1"

static const gchar introspection_xml[] =
	"<node>"
	"  <interface name='org.gowl.Macro1'>"
	"    <method name='Run'>"
	"      <arg type='s' name='name' direction='in'/>"
	"      <arg type='as' name='args' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='Stop'>"
	"      <arg type='s' name='which' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='List'>"
	"      <arg type='s' name='json' direction='out'/>"
	"    </method>"
	"    <method name='Status'>"
	"      <arg type='s' name='json' direction='out'/>"
	"    </method>"
	"    <signal name='Started'>"
	"      <arg type='s' name='name'/><arg type='s' name='detail'/>"
	"    </signal>"
	"    <signal name='Finished'>"
	"      <arg type='s' name='name'/><arg type='s' name='result'/>"
	"    </signal>"
	"    <signal name='Faulted'>"
	"      <arg type='s' name='name'/><arg type='s' name='signal'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

struct _GowlMacroDbus {
	GowlMacroDbusHandler    handler;
	gpointer                module;
	GBusType                bus_type;

	GThread                *thread;
	GMainContext           *context;
	GMainLoop              *loop;
	GDBusConnection        *connection;   /* set on the bus thread */
	guint                   owner_id;
	guint                   object_id;
	gint                    owned;

	GMutex                  lock;
	GQueue                  pending;      /* Request* */
	gint                    efd;
	struct wl_event_source *source;
	GCond                   ready_cond;
	gboolean                ready;
};

typedef struct {
	GDBusMethodInvocation *invocation;
	gchar                 *method;
	gchar                 *name;
	GStrv                  args;
} Request;

static void
request_free(Request *r)
{
	if (r->invocation != NULL)
		g_object_unref(r->invocation);
	g_free(r->method);
	g_free(r->name);
	g_strfreev(r->args);
	g_free(r);
}

/* Bus thread: queue the call and wake the compositor. */
static void
on_method_call(
	GDBusConnection       *connection,
	const gchar           *sender,
	const gchar           *object_path,
	const gchar           *interface_name,
	const gchar           *method_name,
	GVariant              *parameters,
	GDBusMethodInvocation *invocation,
	gpointer               user_data
){
	GowlMacroDbus *self = user_data;
	Request *r;
	guint64 one;
	ssize_t n;

	(void)connection;
	(void)sender;
	(void)object_path;
	(void)interface_name;

	/* The handler owns @invocation (transfer full); the reply on the
	   compositor thread consumes it.  An extra ref here leaked one per
	   call, each pinning the bus connection. */
	r = g_new0(Request, 1);
	r->invocation = invocation;
	r->method = g_strdup(method_name);
	if (g_strcmp0(method_name, "Run") == 0)
		g_variant_get(parameters, "(s^as)", &r->name, &r->args);
	else if (g_strcmp0(method_name, "Stop") == 0)
		g_variant_get(parameters, "(s)", &r->name);
	if (r->name == NULL)
		r->name = g_strdup("");

	g_mutex_lock(&self->lock);
	g_queue_push_tail(&self->pending, r);
	g_mutex_unlock(&self->lock);
	one = 1;
	n = write(self->efd, &one, sizeof one);
	(void)n;
}

static const GDBusInterfaceVTable vtable = {
	on_method_call, NULL, NULL, { NULL }
};

static void
on_bus_acquired(
	GDBusConnection *connection,
	const gchar     *name,
	gpointer         user_data
){
	GowlMacroDbus *self = user_data;
	g_autoptr(GDBusNodeInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	(void)name;
	info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
	self->connection = g_object_ref(connection);
	self->object_id = g_dbus_connection_register_object(
		connection, MACRO_OBJ_PATH, info->interfaces[0], &vtable, self,
		NULL, &error);
	if (self->object_id == 0)
		g_warning("macro: cannot register %s: %s", MACRO_OBJ_PATH,
		          error->message);
}

static void
on_name_acquired(
	GDBusConnection *connection,
	const gchar     *name,
	gpointer         user_data
){
	GowlMacroDbus *self = user_data;

	(void)connection;
	(void)name;
	g_atomic_int_set(&self->owned, 1);
	g_mutex_lock(&self->lock);
	self->ready = TRUE;
	g_cond_broadcast(&self->ready_cond);
	g_mutex_unlock(&self->lock);
}

static void
on_name_lost(
	GDBusConnection *connection,
	const gchar     *name,
	gpointer         user_data
){
	GowlMacroDbus *self = user_data;

	(void)connection;
	g_atomic_int_set(&self->owned, 0);
	g_message("macro: %s is owned by someone else; the D-Bus service is "
	          "off", name);
	g_mutex_lock(&self->lock);
	self->ready = TRUE;
	g_cond_broadcast(&self->ready_cond);
	g_mutex_unlock(&self->lock);
}

static gpointer
bus_thread(gpointer data)
{
	GowlMacroDbus *self = data;

	g_main_context_push_thread_default(self->context);
	self->owner_id = g_bus_own_name(self->bus_type, MACRO_BUS_NAME,
	                                G_BUS_NAME_OWNER_FLAGS_NONE,
	                                on_bus_acquired, on_name_acquired,
	                                on_name_lost, self, NULL);
	g_main_loop_run(self->loop);

	if (self->object_id != 0 && self->connection != NULL)
		g_dbus_connection_unregister_object(self->connection,
		                                    self->object_id);
	g_bus_unown_name(self->owner_id);
	/* Let the unown reach the bus before the context goes */
	while (g_main_context_iteration(self->context, FALSE))
		;
	g_main_context_pop_thread_default(self->context);
	return NULL;
}

/* Compositor thread: answer every queued call. */
static gint
on_wake(
	gint     fd,
	guint32  mask,
	gpointer data
){
	GowlMacroDbus *self = data;
	guint64 v;
	ssize_t n;
	Request *r;

	(void)mask;
	n = read(fd, &v, sizeof v);
	(void)n;
	for (;;) {
		gchar *reply;

		g_mutex_lock(&self->lock);
		r = g_queue_pop_head(&self->pending);
		g_mutex_unlock(&self->lock);
		if (r == NULL)
			break;
		reply = self->handler(self->module, r->method, r->name, r->args);
		g_dbus_method_invocation_return_value(
			g_steal_pointer(&r->invocation),
			g_variant_new("(s)", reply != NULL ? reply : ""));
		g_free(reply);
		request_free(r);
	}
	return 0;
}

/**
 * gowl_macro_dbus_start: (skip)
 * @loop: the compositor's event loop
 * @bus_type: the bus (the session bus; tests pass a private one)
 * @handler: answers a call on the compositor thread
 * @module: passed to @handler
 *
 * Returns: (transfer full) (nullable): the running service
 */
GowlMacroDbus *
gowl_macro_dbus_start(
	struct wl_event_loop *loop,
	GBusType              bus_type,
	GowlMacroDbusHandler  handler,
	gpointer              module
){
	GowlMacroDbus *self;
	gint64 until;

	if (loop == NULL)
		return NULL;
	self = g_new0(GowlMacroDbus, 1);
	self->handler = handler;
	self->module = module;
	self->bus_type = bus_type;
	g_mutex_init(&self->lock);
	g_cond_init(&self->ready_cond);
	g_queue_init(&self->pending);
	self->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (self->efd < 0) {
		g_free(self);
		return NULL;
	}
	self->source = wl_event_loop_add_fd(loop, self->efd, WL_EVENT_READABLE,
	                                    on_wake, self);
	self->context = g_main_context_new();
	self->loop = g_main_loop_new(self->context, FALSE);
	self->thread = g_thread_new("gowl-macro-dbus", bus_thread, self);

	/* A moment for the name, so `macro-status' is truthful at once;
	   never long -- a missing bus must not stall the compositor. */
	until = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
	g_mutex_lock(&self->lock);
	while (!self->ready)
		if (!g_cond_wait_until(&self->ready_cond, &self->lock, until))
			break;
	g_mutex_unlock(&self->lock);
	return self;
}

/**
 * gowl_macro_dbus_stop: (skip)
 * @self: (nullable): the service
 *
 * Releases the name and stops the bus thread.  Calls still queued are
 * answered with an error.
 */
void
gowl_macro_dbus_stop(GowlMacroDbus *self)
{
	Request *r;

	if (self == NULL)
		return;
	g_main_loop_quit(self->loop);
	g_thread_join(self->thread);
	if (self->source != NULL)
		wl_event_source_remove(self->source);
	while ((r = g_queue_pop_head(&self->pending)) != NULL) {
		g_dbus_method_invocation_return_error_literal(
			g_steal_pointer(&r->invocation), G_DBUS_ERROR,
			G_DBUS_ERROR_FAILED, "the macro module stopped");
		request_free(r);
	}
	close(self->efd);
	g_clear_object(&self->connection);
	g_main_loop_unref(self->loop);
	g_main_context_unref(self->context);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->ready_cond);
	g_free(self);
}

/**
 * gowl_macro_dbus_emit: (skip)
 * @self: the service
 * @signal: Started, Finished or Faulted
 * @name: the macro
 * @detail: the second argument
 */
void
gowl_macro_dbus_emit(
	GowlMacroDbus *self,
	const gchar   *signal,
	const gchar   *name,
	const gchar   *detail
){
	if (self == NULL || self->connection == NULL
	    || !g_atomic_int_get(&self->owned))
		return;
	g_dbus_connection_emit_signal(self->connection, NULL, MACRO_OBJ_PATH,
	                              MACRO_IFACE, signal,
	                              g_variant_new("(ss)", name,
	                                            detail != NULL ? detail : ""),
	                              NULL);
}

/**
 * gowl_macro_dbus_is_owned: (skip)
 * @self: the service
 *
 * Returns: %TRUE while the bus name is ours
 */
gboolean
gowl_macro_dbus_is_owned(GowlMacroDbus *self)
{
	return self != NULL && g_atomic_int_get(&self->owned) != 0;
}
