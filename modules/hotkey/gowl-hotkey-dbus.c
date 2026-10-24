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
 * gowl-hotkey-dbus.c - org.gowl.Hotkey1, for driving the hotkey module
 * from anything on the session bus.  Opt-in (`dbus: true'); under cmacs
 * the editor's org.cmacs.Editor1.Compositor `Hotkey' method does this.
 *
 * The macro module's pattern (gowl-macro-dbus.c): the bus lives on a
 * thread of its own with a private GMainContext -- the compositor
 * thread runs no GLib main loop, and under cmacs the default context is
 * the editor's.  A call is queued, the compositor woken through an
 * eventfd on its wl_event_loop, answered there by the same dispatcher
 * the IPC uses, and replied through the invocation (GDBus replies are
 * thread-safe).
 *
 *   Command(s line) -> s       any hotkey-* command without the prefix
 *   List() -> s                Status() -> s
 *   Add(s yaml) -> s           Remove(s id) -> s
 *   SetEnabled(s id, b on) -> s
 *   SetSuspended(b on) -> s    SetMode(s mode) -> s
 *   Send(s keys) -> s          SendText(s text) -> s
 *   Fire(s id) -> s            Call(s name, as args) -> s
 *   signals Fired(s id, s key), Suspended(s state, s why),
 *           ModeChanged(s mode, s unused), InputEnded(s reason, s text),
 *           Fault(s message, s unused)
 *
 * Every reply is the IPC's: `OK ...' or `ERROR ...'.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include <gio/gio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "gowl-hotkey-module.h"

#define HK_BUS_NAME  "org.gowl.Hotkey1"
#define HK_OBJ_PATH  "/org/gowl/Hotkey1"
#define HK_IFACE     "org.gowl.Hotkey1"

static const gchar introspection_xml[] =
	"<node>"
	"  <interface name='org.gowl.Hotkey1'>"
	"    <method name='Command'>"
	"      <arg type='s' name='line' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='List'><arg type='s' name='json' direction='out'/></method>"
	"    <method name='Status'><arg type='s' name='json' direction='out'/></method>"
	"    <method name='Add'>"
	"      <arg type='s' name='yaml' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='Remove'>"
	"      <arg type='s' name='id' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='SetEnabled'>"
	"      <arg type='s' name='id' direction='in'/>"
	"      <arg type='b' name='enabled' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='SetSuspended'>"
	"      <arg type='b' name='suspended' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='SetMode'>"
	"      <arg type='s' name='mode' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='Send'>"
	"      <arg type='s' name='keys' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='SendText'>"
	"      <arg type='s' name='text' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='Fire'>"
	"      <arg type='s' name='id' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <method name='Call'>"
	"      <arg type='s' name='name' direction='in'/>"
	"      <arg type='as' name='args' direction='in'/>"
	"      <arg type='s' name='reply' direction='out'/>"
	"    </method>"
	"    <signal name='Fired'><arg type='s' name='id'/><arg type='s' name='key'/></signal>"
	"    <signal name='Suspended'><arg type='s' name='state'/><arg type='s' name='why'/></signal>"
	"    <signal name='ModeChanged'><arg type='s' name='mode'/><arg type='s' name='unused'/></signal>"
	"    <signal name='InputEnded'><arg type='s' name='reason'/><arg type='s' name='text'/></signal>"
	"    <signal name='Fault'><arg type='s' name='message'/><arg type='s' name='unused'/></signal>"
	"  </interface>"
	"</node>";

struct _HkDbus {
	GowlModuleHotkey       *module;

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
	gchar                 *verb;
	gchar                 *args;
} Request;

static void
request_free(Request *r)
{
	if (r->invocation != NULL)
		g_object_unref(r->invocation);
	g_free(r->verb);
	g_free(r->args);
	g_free(r);
}

/* A method as a dispatcher verb and its argument line. */
static void
to_verb(
	const gchar  *method,
	GVariant     *params,
	gchar       **verb,
	gchar       **args
){
	const gchar *s = NULL;
	gboolean b = FALSE;

	*args = NULL;
	if (g_strcmp0(method, "Command") == 0) {
		gchar *sp;

		g_variant_get(params, "(&s)", &s);
		*verb = g_strdup(s);
		sp = strchr(*verb, ' ');
		if (sp != NULL) {
			*args = g_strdup(sp + 1);
			*sp = '\0';
		}
		/* `hotkey-list' as well as `list' */
		if (g_str_has_prefix(*verb, HK_PREFIX))
			memmove(*verb, *verb + strlen(HK_PREFIX),
			        strlen(*verb) - strlen(HK_PREFIX) + 1);
	} else if (g_strcmp0(method, "List") == 0) {
		*verb = g_strdup("list");
	} else if (g_strcmp0(method, "Status") == 0) {
		*verb = g_strdup("status");
	} else if (g_strcmp0(method, "Add") == 0) {
		g_variant_get(params, "(&s)", &s);
		*verb = g_strdup("add");
		*args = g_strdup(s);
	} else if (g_strcmp0(method, "Remove") == 0) {
		g_variant_get(params, "(&s)", &s);
		*verb = g_strdup("remove");
		*args = g_strdup(s);
	} else if (g_strcmp0(method, "SetEnabled") == 0) {
		g_variant_get(params, "(&sb)", &s, &b);
		*verb = g_strdup(b ? "enable" : "disable");
		*args = g_strdup(s);
	} else if (g_strcmp0(method, "SetSuspended") == 0) {
		g_variant_get(params, "(b)", &b);
		*verb = g_strdup("suspend");
		*args = g_strdup(b ? "on" : "off");
	} else if (g_strcmp0(method, "SetMode") == 0) {
		g_variant_get(params, "(&s)", &s);
		*verb = g_strdup("mode");
		*args = g_strdup(s);
	} else if (g_strcmp0(method, "Send") == 0
	           || g_strcmp0(method, "SendText") == 0) {
		g_variant_get(params, "(&s)", &s);
		*verb = g_strdup(method[4] == 'T' ? "send-text" : "send");
		*args = g_strdup(s);
	} else if (g_strcmp0(method, "Fire") == 0) {
		g_variant_get(params, "(&s)", &s);
		*verb = g_strdup("fire");
		*args = g_strdup(s);
	} else if (g_strcmp0(method, "Call") == 0) {
		g_autofree const gchar **argv = NULL;
		GString *line;
		gchar *q;
		guint i;

		g_variant_get(params, "(&s^a&s)", &s, &argv);
		q = g_shell_quote(s);
		line = g_string_new(q);
		g_free(q);
		for (i = 0; argv != NULL && argv[i] != NULL; i++) {
			q = g_shell_quote(argv[i]);
			g_string_append_printf(line, " %s", q);
			g_free(q);
		}
		*verb = g_strdup("call");
		*args = g_string_free(line, FALSE);
	} else {
		*verb = g_strdup("help");
	}
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
	HkDbus *self = user_data;
	Request *r;
	guint64 one = 1;
	ssize_t n;

	(void)connection;
	(void)sender;
	(void)object_path;
	(void)interface_name;

	/* The handler owns @invocation (transfer full); the reply on the
	   compositor thread consumes it */
	r = g_new0(Request, 1);
	r->invocation = invocation;
	to_verb(method_name, parameters, &r->verb, &r->args);
	g_mutex_lock(&self->lock);
	g_queue_push_tail(&self->pending, r);
	g_mutex_unlock(&self->lock);
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
	HkDbus *self = user_data;
	g_autoptr(GDBusNodeInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	(void)name;
	info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
	self->connection = g_object_ref(connection);
	self->object_id = g_dbus_connection_register_object(
		connection, HK_OBJ_PATH, info->interfaces[0], &vtable, self, NULL,
		&error);
	if (self->object_id == 0)
		g_warning("hotkey: cannot register %s: %s", HK_OBJ_PATH,
		          error->message);
}

static void
on_name(
	GDBusConnection *connection,
	const gchar     *name,
	gboolean         acquired,
	HkDbus          *self
){
	(void)connection;
	g_atomic_int_set(&self->owned, acquired ? 1 : 0);
	if (!acquired)
		g_message("hotkey: %s is owned by someone else; the D-Bus service "
		          "is off", name);
	g_mutex_lock(&self->lock);
	self->ready = TRUE;
	g_cond_broadcast(&self->ready_cond);
	g_mutex_unlock(&self->lock);
}

static void
on_name_acquired(
	GDBusConnection *connection,
	const gchar     *name,
	gpointer         user_data
){
	on_name(connection, name, TRUE, user_data);
}

static void
on_name_lost(
	GDBusConnection *connection,
	const gchar     *name,
	gpointer         user_data
){
	on_name(connection, name, FALSE, user_data);
}

static gpointer
bus_thread(gpointer data)
{
	HkDbus *self = data;

	g_main_context_push_thread_default(self->context);
	self->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, HK_BUS_NAME,
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
	HkDbus *self = data;
	guint64 v;
	ssize_t n;
	Request *r;

	(void)mask;
	n = read(fd, &v, sizeof v);
	(void)n;
	for (;;) {
		g_autofree gchar *reply = NULL;

		g_mutex_lock(&self->lock);
		r = g_queue_pop_head(&self->pending);
		g_mutex_unlock(&self->lock);
		if (r == NULL)
			break;
		reply = hk_dispatch(self->module, r->verb, r->args);
		g_dbus_method_invocation_return_value(
			g_steal_pointer(&r->invocation),
			g_variant_new("(s)", reply != NULL ? reply : ""));
		request_free(r);
	}
	return 0;
}

/**
 * hk_dbus_new: (skip)
 * @module: the hotkey module, started
 *
 * Returns: (transfer full) (nullable): the running service
 */
HkDbus *
hk_dbus_new(GowlModuleHotkey *module)
{
	struct wl_event_loop *loop;
	HkDbus *self;
	gint64 until;

	if (module->compositor == NULL)
		return NULL;
	loop = gowl_compositor_get_event_loop(module->compositor);
	if (loop == NULL)
		return NULL;
	self = g_new0(HkDbus, 1);
	self->module = module;
	g_mutex_init(&self->lock);
	g_cond_init(&self->ready_cond);
	g_queue_init(&self->pending);
	self->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (self->efd < 0) {
		g_mutex_clear(&self->lock);
		g_cond_clear(&self->ready_cond);
		g_free(self);
		return NULL;
	}
	self->source = wl_event_loop_add_fd(loop, self->efd, WL_EVENT_READABLE,
	                                    on_wake, self);
	self->context = g_main_context_new();
	self->loop = g_main_loop_new(self->context, FALSE);
	self->thread = g_thread_new("gowl-hotkey-dbus", bus_thread, self);

	/* A moment for the name, so hotkey-status is truthful at once; never
	   long -- a missing bus must not stall the compositor */
	until = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
	g_mutex_lock(&self->lock);
	while (!self->ready)
		if (!g_cond_wait_until(&self->ready_cond, &self->lock, until))
			break;
	g_mutex_unlock(&self->lock);
	return self;
}

/**
 * hk_dbus_free: (skip)
 * @self: (nullable): the service
 *
 * Releases the name and stops the bus thread.  Calls still queued are
 * answered with an error.
 */
void
hk_dbus_free(HkDbus *self)
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
			G_DBUS_ERROR_FAILED, "the hotkey module stopped");
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
 * hk_dbus_emit: (skip)
 * @self: the service
 * @signal: Fired, Suspended, ModeChanged, InputEnded or Fault
 * @a: the first argument
 * @b: the second
 */
void
hk_dbus_emit(
	HkDbus      *self,
	const gchar *signal,
	const gchar *a,
	const gchar *b
){
	if (self == NULL || self->connection == NULL
	    || !g_atomic_int_get(&self->owned))
		return;
	g_dbus_connection_emit_signal(self->connection, NULL, HK_OBJ_PATH,
	                              HK_IFACE, signal,
	                              g_variant_new("(ss)", a != NULL ? a : "",
	                                            b != NULL ? b : ""),
	                              NULL);
}
