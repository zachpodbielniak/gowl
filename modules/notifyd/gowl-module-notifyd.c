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
 * gowl-module-notifyd.c - A notification daemon for standalone gowl.
 *
 * Owns org.freedesktop.Notifications on the session bus and turns each
 * Notify into a toast on the bar (the `bar-notify' command) and a count
 * on the bar's `notifications' widget, the same two things cmacs's own
 * daemon does for the bar under `cmacs --gowl'.  Under cmacs the name
 * is already taken and this module stays idle: the bus refuses the
 * name, the module logs that and serves nothing, so loading it in both
 * sessions is harmless.
 *
 * The compositor's event loop is a wl_event_loop, not a GMainLoop, so
 * GDBus cannot run on it.  The bus lives on its own thread with its own
 * GMainContext; a Notify is answered there straight away with its id,
 * and the toast is queued to the compositor thread through an eventfd
 * on the wl_event_loop, the way the mcp module dispatches tool calls.
 * Nothing on the bus thread ever touches the compositor.
 *
 * Commands (IPC, keybind `ipc_command', gowl-msg):
 *   notify-dnd [on|off|toggle]  Do-not-disturb: notifications are
 *                               counted but raise no toast
 *   notify-clear                Marks every notification read
 *   notify-count                The unread count
 *
 * Settings under modules: notifyd:
 *   enabled: bool (default true)
 *   timeout: default toast time in ms when the sender does not say
 *            (default 5000; 0 never expires)
 *   widget: the bar widget that gets count/dnd/last (default
 *           "notifications")
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-notifyd"

#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <wayland-server-core.h>

#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"
#include "core/gowl-compositor.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-ipc-handler.h"

#define GOWL_TYPE_MODULE_NOTIFYD (gowl_module_notifyd_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleNotifyd, gowl_module_notifyd,
                     GOWL, MODULE_NOTIFYD, GowlModule)

/* One notification, built on the bus thread, consumed on the
 * compositor thread. */
typedef struct {
	guint32  id;
	gchar   *app;
	gchar   *summary;
	gchar   *body;
	guint    urgency;   /* 0 low, 1 normal, 2 critical */
	gint     timeout;   /* ms; 0 never */
} GowlNotification;

struct _GowlModuleNotifyd {
	GowlModule       parent_instance;

	GowlCompositor  *compositor;    /* weak; NULL once it is gone */
	gboolean         enabled;
	gint             default_timeout;
	gchar           *widget;

	/* the bus thread */
	GThread         *thread;
	GMainContext    *ctx;
	GMainLoop       *loop;
	GDBusConnection *conn;          /* owned by the bus thread */
	guint            name_id;
	guint            reg_id;
	gboolean         owns_name;
	guint32          next_id;       /* bus thread only */

	/* bus thread -> compositor thread */
	int                       wake_fd;
	struct wl_event_source   *wake_source;
	GMutex                    queue_mutex;
	GQueue                    pending;     /* GowlNotification* */

	/* compositor thread state, mirrored to the bar widget */
	guint            unread;
	gboolean         dnd;
	gchar           *last;
};

static void notifyd_startup_init  (GowlStartupHandlerInterface *iface);
static void notifyd_shutdown_init (GowlShutdownHandlerInterface *iface);
static void notifyd_ipc_init      (GowlIpcHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleNotifyd, gowl_module_notifyd,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, notifyd_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, notifyd_shutdown_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, notifyd_ipc_init))

static const gchar introspection_xml[] =
	"<node>"
	"  <interface name='org.freedesktop.Notifications'>"
	"    <method name='Notify'>"
	"      <arg type='s' name='app_name' direction='in'/>"
	"      <arg type='u' name='replaces_id' direction='in'/>"
	"      <arg type='s' name='app_icon' direction='in'/>"
	"      <arg type='s' name='summary' direction='in'/>"
	"      <arg type='s' name='body' direction='in'/>"
	"      <arg type='as' name='actions' direction='in'/>"
	"      <arg type='a{sv}' name='hints' direction='in'/>"
	"      <arg type='i' name='expire_timeout' direction='in'/>"
	"      <arg type='u' name='id' direction='out'/>"
	"    </method>"
	"    <method name='CloseNotification'>"
	"      <arg type='u' name='id' direction='in'/>"
	"    </method>"
	"    <method name='GetCapabilities'>"
	"      <arg type='as' name='capabilities' direction='out'/>"
	"    </method>"
	"    <method name='GetServerInformation'>"
	"      <arg type='s' name='name' direction='out'/>"
	"      <arg type='s' name='vendor' direction='out'/>"
	"      <arg type='s' name='version' direction='out'/>"
	"      <arg type='s' name='spec_version' direction='out'/>"
	"    </method>"
	"    <signal name='NotificationClosed'>"
	"      <arg type='u' name='id'/>"
	"      <arg type='u' name='reason'/>"
	"    </signal>"
	"    <signal name='ActionInvoked'>"
	"      <arg type='u' name='id'/>"
	"      <arg type='s' name='action_key'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

static void
notification_free(GowlNotification *n)
{
	if (n == NULL)
		return;
	g_free(n->app);
	g_free(n->summary);
	g_free(n->body);
	g_free(n);
}

/* ------------------------------------------------------------------
 * Compositor thread
 * ------------------------------------------------------------------ */

/**
 * push_widget_state:
 *
 * Publishes count/dnd/last to the bar widget, through the bar
 * module's configure path -- the same keys cmacs-notify-bar sets.  A
 * session without the bar module has nowhere to put them; nothing
 * happens.
 */
static void
push_widget_state(GowlModuleNotifyd *self)
{
	GowlModuleManager *mgr;
	GowlModule *bar;
	GHashTable *settings;

	if (self->compositor == NULL)
		return;
	mgr = gowl_compositor_get_module_manager(self->compositor);
	if (mgr == NULL)
		return;
	bar = gowl_module_manager_find_module(mgr, "bar");
	if (bar == NULL)
		return;

	settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(settings,
	                    g_strdup_printf("widget-data-%s.count", self->widget),
	                    g_strdup_printf("%u", self->unread));
	g_hash_table_insert(settings,
	                    g_strdup_printf("widget-data-%s.dnd", self->widget),
	                    g_strdup(self->dnd ? "1" : "0"));
	g_hash_table_insert(settings,
	                    g_strdup_printf("widget-data-%s.last", self->widget),
	                    g_strdup(self->last != NULL ? self->last : ""));
	gowl_module_configure(bar, settings);
	g_hash_table_unref(settings);
}

/**
 * show_notification:
 *
 * Raises the toast and bumps the count.  `bar-notify' takes
 * `summary|body', so a bar in the text is replaced; it is the
 * separator, not a character the command can carry.
 */
static void
show_notification(GowlModuleNotifyd *self, GowlNotification *n)
{
	g_autofree gchar *summary = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *line = NULL;
	gchar *reply;

	self->unread++;
	g_free(self->last);
	self->last = g_strdup(n->summary);

	/* Critical notifications get through do-not-disturb, as the
	 * specification asks. */
	if (self->dnd && n->urgency < 2) {
		push_widget_state(self);
		return;
	}
	if (self->compositor == NULL)
		return;

	summary = g_strdelimit(g_strdup(n->summary), "|\n", '/');
	body = n->body != NULL ? g_strdelimit(g_strdup(n->body), "|", '/') : NULL;
	line = g_strdup_printf("bar-notify %s|%s", summary,
	                       body != NULL ? body : "");
	reply = gowl_compositor_run_command(self->compositor, line);
	if (reply == NULL)
		g_debug("no bar to show '%s' on", n->summary);
	g_free(reply);
	push_widget_state(self);
}

/* The eventfd is readable: take every queued notification. */
static int
on_wake(int fd, uint32_t mask, void *data)
{
	GowlModuleNotifyd *self = (GowlModuleNotifyd *)data;
	guint64 val;
	ssize_t r;
	(void)mask;

	r = read(fd, &val, sizeof val);
	(void)r;
	for (;;) {
		GowlNotification *n;

		g_mutex_lock(&self->queue_mutex);
		n = (GowlNotification *)g_queue_pop_head(&self->pending);
		g_mutex_unlock(&self->queue_mutex);
		if (n == NULL)
			break;
		show_notification(self, n);
		notification_free(n);
	}
	return 0;
}

/* ------------------------------------------------------------------
 * Bus thread
 * ------------------------------------------------------------------ */

typedef struct {
	GowlModuleNotifyd *self;
	guint32 id;
} ExpireData;

/* The toast's time is up: tell the sender, as the spec expects, so a
 * client waiting on NotificationClosed does not wait forever. */
static gboolean
on_expire(gpointer data)
{
	ExpireData *e = (ExpireData *)data;

	if (e->self->conn != NULL)
		g_dbus_connection_emit_signal(e->self->conn, NULL,
			"/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", "NotificationClosed",
			g_variant_new("(uu)", e->id, 1), NULL);
	return G_SOURCE_REMOVE;
}

static void
handle_method_call(
	GDBusConnection       *conn,
	const gchar           *sender,
	const gchar           *object_path,
	const gchar           *interface_name,
	const gchar           *method_name,
	GVariant              *parameters,
	GDBusMethodInvocation *invocation,
	gpointer               user_data
){
	GowlModuleNotifyd *self = (GowlModuleNotifyd *)user_data;
	(void)sender;
	(void)object_path;
	(void)interface_name;

	if (g_strcmp0(method_name, "Notify") == 0) {
		const gchar *app;
		guint32 replaces;
		const gchar *icon;
		const gchar *summary;
		const gchar *body;
		GVariant *hints;
		gint32 expire;
		GowlNotification *n;
		guint8 urgency = 1;
		guint64 one = 1;
		ssize_t r;

		g_variant_get(parameters, "(&su&s&s&s^as@a{sv}i)", &app, &replaces,
		              &icon, &summary, &body, NULL, &hints, &expire);
		g_variant_lookup(hints, "urgency", "y", &urgency);
		g_variant_unref(hints);

		n = g_new0(GowlNotification, 1);
		n->id = replaces != 0 ? replaces : self->next_id++;
		if (self->next_id == 0)
			self->next_id = 1;
		n->app = g_strdup(app);
		n->summary = g_strdup(summary);
		n->body = body != NULL && body[0] != '\0' ? g_strdup(body) : NULL;
		n->urgency = urgency;
		n->timeout = expire < 0 ? self->default_timeout : expire;

		g_dbus_method_invocation_return_value(invocation,
		                                      g_variant_new("(u)", n->id));

		if (n->timeout > 0) {
			ExpireData *e = g_new0(ExpireData, 1);
			GSource *src;

			e->self = self;
			e->id = n->id;
			src = g_timeout_source_new((guint)n->timeout);
			g_source_set_callback(src, on_expire, e, g_free);
			g_source_attach(src, self->ctx);
			g_source_unref(src);
		}

		g_mutex_lock(&self->queue_mutex);
		g_queue_push_tail(&self->pending, n);
		g_mutex_unlock(&self->queue_mutex);
		r = write(self->wake_fd, &one, sizeof one);
		(void)r;
		return;
	}
	if (g_strcmp0(method_name, "CloseNotification") == 0) {
		guint32 id;

		g_variant_get(parameters, "(u)", &id);
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_dbus_connection_emit_signal(conn, NULL,
			"/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", "NotificationClosed",
			g_variant_new("(uu)", id, 3), NULL);
		return;
	}
	if (g_strcmp0(method_name, "GetCapabilities") == 0) {
		const gchar *caps[] = { "body", "persistence", NULL };

		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(^as)", caps));
		return;
	}
	if (g_strcmp0(method_name, "GetServerInformation") == 0) {
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ssss)", "gowl", "gowl", "0.5.0", "1.2"));
		return;
	}
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "no such method");
}

static const GDBusInterfaceVTable vtable = {
	handle_method_call, NULL, NULL, { 0 }
};

static void
on_name_acquired(GDBusConnection *conn, const gchar *name, gpointer data)
{
	GowlModuleNotifyd *self = (GowlModuleNotifyd *)data;
	(void)conn;

	self->owns_name = TRUE;
	g_message("serving %s", name);
}

static void
on_name_lost(GDBusConnection *conn, const gchar *name, gpointer data)
{
	GowlModuleNotifyd *self = (GowlModuleNotifyd *)data;
	(void)conn;

	if (self->owns_name)
		g_warning("lost %s to another daemon", name);
	else
		g_message("%s is already served (cmacs's daemon, most likely); "
		          "idle", name);
	self->owns_name = FALSE;
}

/* The bus thread: its own context, the connection, the object, the
 * name, and the loop until deactivate stops it. */
static gpointer
bus_thread(gpointer data)
{
	GowlModuleNotifyd *self = (GowlModuleNotifyd *)data;
	GDBusNodeInfo *info;
	GError *error = NULL;

	g_main_context_push_thread_default(self->ctx);

	self->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (self->conn == NULL) {
		g_warning("no session bus: %s", error->message);
		g_error_free(error);
		g_main_context_pop_thread_default(self->ctx);
		return NULL;
	}
	info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
	self->reg_id = g_dbus_connection_register_object(self->conn,
		"/org/freedesktop/Notifications", info->interfaces[0], &vtable,
		self, NULL, &error);
	g_dbus_node_info_unref(info);
	if (self->reg_id == 0) {
		g_warning("could not register the object: %s", error->message);
		g_error_free(error);
	} else {
		/* NONE: never take the name from a daemon that has it, and
		 * never queue for it -- cmacs's daemon is the one that
		 * should win under cmacs --gowl. */
		self->name_id = g_bus_own_name_on_connection(self->conn,
			"org.freedesktop.Notifications", G_BUS_NAME_OWNER_FLAGS_NONE,
			on_name_acquired, on_name_lost, self, NULL);
	}

	g_main_loop_run(self->loop);

	if (self->name_id != 0)
		g_bus_unown_name(self->name_id);
	self->name_id = 0;
	if (self->reg_id != 0)
		g_dbus_connection_unregister_object(self->conn, self->reg_id);
	self->reg_id = 0;
	/* Unowning the name and unregistering the object release their
	 * references to the connection from this context; turn it until
	 * they have, or the connection outlives the thread. */
	while (g_main_context_iteration(self->ctx, FALSE))
		;
	g_clear_object(&self->conn);
	g_main_context_pop_thread_default(self->ctx);
	return NULL;
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

static void
notifyd_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleNotifyd *self = GOWL_MODULE_NOTIFYD(handler);
	struct wl_event_loop *loop;

	if (!self->enabled) {
		g_debug("disabled");
		return;
	}
	self->compositor = GOWL_COMPOSITOR(compositor);
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&self->compositor);

	self->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (self->wake_fd < 0) {
		g_warning("eventfd: %s", g_strerror(errno));
		return;
	}
	loop = gowl_compositor_get_event_loop(self->compositor);
	self->wake_source = wl_event_loop_add_fd(loop, self->wake_fd,
	                                         WL_EVENT_READABLE, on_wake, self);
	if (self->wake_source == NULL) {
		g_warning("could not watch the eventfd");
		close(self->wake_fd);
		self->wake_fd = -1;
		return;
	}

	self->ctx = g_main_context_new();
	self->loop = g_main_loop_new(self->ctx, FALSE);
	self->thread = g_thread_new("gowl-notifyd", bus_thread, self);
	push_widget_state(self);
}

/* Stop the bus first, so nothing writes the eventfd after its source
 * is gone; the compositor's loop is still alive here. */
static void
notifyd_stop(GowlModuleNotifyd *self)
{
	if (self->thread != NULL) {
		g_main_loop_quit(self->loop);
		g_thread_join(self->thread);
		self->thread = NULL;
	}
	g_clear_pointer(&self->loop, g_main_loop_unref);
	g_clear_pointer(&self->ctx, g_main_context_unref);
	if (self->wake_source != NULL && self->compositor != NULL) {
		wl_event_source_remove(self->wake_source);
		self->wake_source = NULL;
	}
	if (self->wake_fd >= 0) {
		close(self->wake_fd);
		self->wake_fd = -1;
	}
	g_mutex_lock(&self->queue_mutex);
	while (!g_queue_is_empty(&self->pending))
		notification_free((GowlNotification *)g_queue_pop_head(&self->pending));
	g_mutex_unlock(&self->queue_mutex);
}

static void
notifyd_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	(void)compositor;
	notifyd_stop(GOWL_MODULE_NOTIFYD(handler));
}

static gchar *
notifyd_handle_command(GowlIpcHandler *handler, const gchar *command,
                       const gchar *args)
{
	GowlModuleNotifyd *self = GOWL_MODULE_NOTIFYD(handler);

	if (g_strcmp0(command, "notify-dnd") == 0) {
		if (args == NULL || args[0] == '\0' || g_strcmp0(args, "toggle") == 0)
			self->dnd = !self->dnd;
		else
			self->dnd = g_strcmp0(args, "on") == 0
			            || g_strcmp0(args, "1") == 0
			            || g_strcmp0(args, "true") == 0;
		push_widget_state(self);
		return g_strdup(self->dnd ? "on\n" : "off\n");
	}
	if (g_strcmp0(command, "notify-clear") == 0) {
		self->unread = 0;
		push_widget_state(self);
		return g_strdup("ok\n");
	}
	if (g_strcmp0(command, "notify-count") == 0)
		return g_strdup_printf("%u\n", self->unread);
	return NULL;
}

static void
notifyd_configure(GowlModule *mod, gpointer config)
{
	GowlModuleNotifyd *self = GOWL_MODULE_NOTIFYD(mod);
	GHashTable *settings = (GHashTable *)config;
	const gchar *val;

	if (settings == NULL)
		return;
	val = (const gchar *)g_hash_table_lookup(settings, "enabled");
	if (val != NULL)
		self->enabled = g_strcmp0(val, "false") != 0
		                && g_strcmp0(val, "0") != 0
		                && g_strcmp0(val, "no") != 0;
	val = (const gchar *)g_hash_table_lookup(settings, "timeout");
	if (val != NULL)
		self->default_timeout = (gint)g_ascii_strtoll(val, NULL, 10);
	val = (const gchar *)g_hash_table_lookup(settings, "widget");
	if (val != NULL && val[0] != '\0') {
		g_free(self->widget);
		self->widget = g_strdup(val);
	}
}

static gboolean
notifyd_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
notifyd_deactivate(GowlModule *mod)
{
	GowlModuleNotifyd *self = GOWL_MODULE_NOTIFYD(mod);

	notifyd_stop(self);
	if (self->compositor != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
		self->compositor = NULL;
	}
}

static const gchar *
notifyd_get_name(GowlModule *mod)
{
	(void)mod;
	return "notifyd";
}

static const gchar *
notifyd_get_description(GowlModule *mod)
{
	(void)mod;
	return "org.freedesktop.Notifications daemon feeding the bar";
}

static const gchar *
notifyd_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
notifyd_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = notifyd_on_startup;
}

static void
notifyd_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = notifyd_on_shutdown;
}

static void
notifyd_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = notifyd_handle_command;
}

static void
gowl_module_notifyd_finalize(GObject *object)
{
	GowlModuleNotifyd *self = GOWL_MODULE_NOTIFYD(object);

	notifyd_deactivate(GOWL_MODULE(self));
	g_mutex_clear(&self->queue_mutex);
	g_free(self->widget);
	g_free(self->last);

	G_OBJECT_CLASS(gowl_module_notifyd_parent_class)->finalize(object);
}

static void
gowl_module_notifyd_class_init(GowlModuleNotifydClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_notifyd_finalize;
	mod_class->activate        = notifyd_activate;
	mod_class->deactivate      = notifyd_deactivate;
	mod_class->get_name        = notifyd_get_name;
	mod_class->get_description = notifyd_get_description;
	mod_class->get_version     = notifyd_get_version;
	mod_class->configure       = notifyd_configure;
}

static void
gowl_module_notifyd_init(GowlModuleNotifyd *self)
{
	self->enabled = TRUE;
	self->default_timeout = 5000;
	self->widget = g_strdup("notifications");
	self->wake_fd = -1;
	self->next_id = 1;
	g_mutex_init(&self->queue_mutex);
	g_queue_init(&self->pending);
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_NOTIFYD;
}
