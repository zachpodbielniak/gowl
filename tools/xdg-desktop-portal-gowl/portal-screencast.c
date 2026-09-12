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
 * portal-screencast.c - org.freedesktop.impl.portal.ScreenCast.
 *
 * The handshake is the standard one: CreateSession, then SelectSources
 * (where the application says whether it wants a monitor or a window),
 * then Start (where a human chooses which, and a PipeWire node id comes
 * back).  What is different here is that "a window" is a real answer
 * rather than "the screen it happens to be on", because the compositor
 * makes a toplevel an addressable capture source.
 *
 * The chooser is an external dmenu-style command, the way
 * xdg-desktop-portal-wlr does it: this process has no toolkit and
 * should not grow one, and the session already has a menu the user
 * configured.  $GOWL_PORTAL_CHOOSER overrides it; otherwise the first
 * of bemenu/fuzzel/wofi/rofi/dmenu found on PATH is used.  With none of
 * them installed the cast falls back to the focused output, which is
 * what portal-wlr would have given anyway -- a call that starts
 * sharing the wrong screen is recoverable, one that fails to start is
 * an unanswerable support question.
 */

#include "portal-screencast.h"
#include "portal-capture.h"
#include "portal-pw.h"

#include <string.h>

/* The frontend's response codes. */
#define PORTAL_RESPONSE_SUCCESS   0
#define PORTAL_RESPONSE_CANCELLED 1
#define PORTAL_RESPONSE_OTHER     2

/* org.freedesktop.portal.ScreenCast source types. */
#define SOURCE_TYPE_MONITOR 1
#define SOURCE_TYPE_WINDOW  2

#define SCREENCAST_PATH "/org/freedesktop/portal/desktop"

typedef struct {
	PortalScreenCast *portal;
	gchar            *handle;       /* the session object path */
	gchar            *app_id;
	guint             session_reg_id;
	guint32           source_types;  /* what SelectSources asked for */
	gboolean          with_cursor;
	PortalPwStream   *stream;
} ScreenCastSession;

struct _PortalScreenCast {
	GDBusConnection *conn;
	guint            reg_id;
	PortalCapture   *capture;
	PortalPw        *pw;
	gboolean         can_window;
	GHashTable      *sessions;   /* handle -> ScreenCastSession* */
};

static GDBusNodeInfo *sc_node_info;
static GDBusNodeInfo *sc_session_node_info;

static const gchar sc_introspection_xml[] =
"<node>"
"  <interface name='org.freedesktop.impl.portal.ScreenCast'>"
"    <property name='version' type='u' access='read'/>"
"    <property name='AvailableSourceTypes' type='u' access='read'/>"
"    <property name='AvailableCursorModes' type='u' access='read'/>"
"    <method name='CreateSession'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='SelectSources'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='Start'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='s' name='parent_window' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"  </interface>"
"</node>";

static const gchar sc_session_introspection_xml[] =
"<node>"
"  <interface name='org.freedesktop.impl.portal.Session'>"
"    <method name='Close'/>"
"    <signal name='Closed'/>"
"    <property name='version' type='u' access='read'/>"
"  </interface>"
"</node>";

static void session_destroy(gpointer data);

/* ---------------------------------------------------------------
 * The chooser
 * --------------------------------------------------------------- */

/* The menu program to pipe the source list through, or NULL. */
static gchar *
chooser_command(void)
{
	const gchar *env = g_getenv("GOWL_PORTAL_CHOOSER");
	const gchar *candidates[] = {
		"bemenu", "fuzzel", "wofi", "rofi", "dmenu", NULL
	};
	gsize i;

	if (env != NULL && *env != '\0')
		return g_strdup(env);
	for (i = 0; candidates[i] != NULL; i++) {
		g_autofree gchar *path = g_find_program_in_path(candidates[i]);

		if (path == NULL)
			continue;
		/* Each of these reads lines on stdin and prints the chosen one,
		 * given the flag that makes it a picker rather than a runner. */
		if (g_strcmp0(candidates[i], "bemenu") == 0)
			return g_strdup_printf("%s -p 'Share:' -l 15", path);
		if (g_strcmp0(candidates[i], "fuzzel") == 0)
			return g_strdup_printf("%s --dmenu --prompt 'Share: '", path);
		if (g_strcmp0(candidates[i], "wofi") == 0)
			return g_strdup_printf("%s --dmenu --prompt 'Share'", path);
		if (g_strcmp0(candidates[i], "rofi") == 0)
			return g_strdup_printf("%s -dmenu -p 'Share'", path);
		return g_strdup_printf("%s -p 'Share:'", path);
	}
	return NULL;
}

/*
 * Runs the chooser over @sources and returns the chosen one.  A chooser
 * that fails or is cancelled returns NULL, and the caller decides
 * whether that is a cancellation or a reason to fall back.
 */
static const PortalCaptureSource *
run_chooser(GPtrArray *sources, gboolean *cancelled)
{
	g_autofree gchar *cmd = chooser_command();
	g_auto(GStrv) argv = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) menu = g_string_new(NULL);
	g_autofree gchar *out = NULL;
	guint i;

	*cancelled = FALSE;
	if (cmd == NULL || sources->len == 0)
		return NULL;
	if (!g_shell_parse_argv(cmd, NULL, &argv, &error)) {
		g_warning("screencast: chooser '%s' is unparseable: %s", cmd,
		          error->message);
		return NULL;
	}

	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		g_string_append_printf(menu, "%s\n", s->title);
	}

	proc = g_subprocess_newv((const gchar * const *)argv,
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
		&error);
	if (proc == NULL) {
		g_warning("screencast: could not run the chooser: %s", error->message);
		return NULL;
	}
	if (!g_subprocess_communicate_utf8(proc, menu->str, NULL, &out, NULL,
	                                   &error)) {
		g_warning("screencast: the chooser failed: %s", error->message);
		return NULL;
	}
	if (out == NULL || *out == '\0') {
		/* An empty answer from a chooser that ran is the human pressing
		 * Escape, which is a cancellation and not an error. */
		*cancelled = TRUE;
		return NULL;
	}
	g_strchomp(out);
	for (i = 0; i < sources->len; i++) {
		const PortalCaptureSource *s = g_ptr_array_index(sources, i);

		if (g_strcmp0(s->title, out) == 0)
			return s;
	}
	return NULL;
}

/* ---------------------------------------------------------------
 * Session objects
 * --------------------------------------------------------------- */

static void
session_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *method, GVariant *params,
               GDBusMethodInvocation *invocation, gpointer user_data)
{
	ScreenCastSession *session = user_data;

	(void)conn; (void)sender; (void)path; (void)iface; (void)params;

	if (g_strcmp0(method, "Close") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_hash_table_remove(session->portal->sessions, session->handle);
		return;
	}
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "no such method: %s", method);
}

static GVariant *
session_get_property(GDBusConnection *conn, const gchar *sender,
                     const gchar *path, const gchar *iface,
                     const gchar *name, GError **error, gpointer user_data)
{
	(void)conn; (void)sender; (void)path; (void)iface; (void)user_data;

	if (g_strcmp0(name, "version") == 0)
		return g_variant_new_uint32(2);
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
	            "no such property: %s", name);
	return NULL;
}

static const GDBusInterfaceVTable sc_session_vtable = {
	session_method, session_get_property, NULL, { 0 }
};

static void
session_destroy(gpointer data)
{
	ScreenCastSession *session = data;

	if (session == NULL)
		return;
	g_clear_pointer(&session->stream, portal_pw_stream_free);
	if (session->session_reg_id != 0
	    && session->portal->conn != NULL
	    && !g_dbus_connection_is_closed(session->portal->conn)) {
		g_dbus_connection_emit_signal(session->portal->conn, NULL,
			session->handle, "org.freedesktop.impl.portal.Session",
			"Closed", NULL, NULL);
		g_dbus_connection_unregister_object(session->portal->conn,
		                                    session->session_reg_id);
	}
	g_free(session->handle);
	g_free(session->app_id);
	g_free(session);
}

/* ---------------------------------------------------------------
 * The interface
 * --------------------------------------------------------------- */

static void
empty_reply(GDBusMethodInvocation *invocation, guint32 response)
{
	GVariantBuilder results;

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_dbus_method_invocation_return_value(invocation,
		g_variant_new("(ua{sv})", response, &results));
}

static void
handle_create_session(PortalScreenCast *self, GVariant *params,
                      GDBusMethodInvocation *invocation)
{
	const gchar *handle;
	const gchar *session_handle;
	const gchar *app_id;
	ScreenCastSession *session;
	g_autoptr(GError) error = NULL;

	g_variant_get(params, "(&o&o&s@a{sv})", &handle, &session_handle,
	              &app_id, NULL);

	session = g_new0(ScreenCastSession, 1);
	session->portal = self;
	session->handle = g_strdup(session_handle);
	session->app_id = g_strdup(app_id);
	/* Both by default, narrowed by SelectSources. */
	session->source_types = SOURCE_TYPE_MONITOR
		| (self->can_window ? SOURCE_TYPE_WINDOW : 0);

	session->session_reg_id = g_dbus_connection_register_object(self->conn,
		session_handle, sc_session_node_info->interfaces[0],
		&sc_session_vtable, session, NULL, &error);
	if (session->session_reg_id == 0) {
		g_warning("screencast: could not export the session: %s",
		          error->message);
		g_free(session->handle);
		g_free(session->app_id);
		g_free(session);
		empty_reply(invocation, PORTAL_RESPONSE_OTHER);
		return;
	}
	g_hash_table_replace(self->sessions, session->handle, session);
	g_debug("screencast: session %s for '%s'", session_handle, app_id);
	empty_reply(invocation, PORTAL_RESPONSE_SUCCESS);
}

static void
handle_select_sources(PortalScreenCast *self, GVariant *params,
                      GDBusMethodInvocation *invocation)
{
	const gchar *handle;
	const gchar *session_handle;
	const gchar *app_id;
	g_autoptr(GVariant) options = NULL;
	ScreenCastSession *session;
	guint32 types = 0;
	guint32 cursor_mode = 0;

	g_variant_get(params, "(&o&o&s@a{sv})", &handle, &session_handle,
	              &app_id, &options);

	session = g_hash_table_lookup(self->sessions, session_handle);
	if (session == NULL) {
		empty_reply(invocation, PORTAL_RESPONSE_OTHER);
		return;
	}
	if (g_variant_lookup(options, "types", "u", &types) && types != 0) {
		if (!self->can_window)
			types &= ~(guint32)SOURCE_TYPE_WINDOW;
		session->source_types = types;
	}
	/* EMBEDDED (2) is the only mode with a meaning here: the compositor
	 * either paints the cursor into the frame or it does not.  METADATA
	 * would need a separate cursor stream, which this does not do, so
	 * anything else is treated as "no cursor". */
	if (g_variant_lookup(options, "cursor_mode", "u", &cursor_mode))
		session->with_cursor = cursor_mode == 2;

	g_debug("screencast: %s wants types 0x%x, cursor %s", app_id,
	        session->source_types, session->with_cursor ? "on" : "off");
	empty_reply(invocation, PORTAL_RESPONSE_SUCCESS);
}

static void
handle_start(PortalScreenCast *self, GVariant *params,
             GDBusMethodInvocation *invocation)
{
	const gchar *handle;
	const gchar *session_handle;
	const gchar *app_id;
	const gchar *parent_window;
	ScreenCastSession *session;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GError) error = NULL;
	const PortalCaptureSource *chosen;
	gboolean cancelled = FALSE;
	GVariantBuilder results;
	GVariantBuilder streams;
	GVariantBuilder props;
	guint32 node_id;
	guint32 w = 0;
	guint32 h = 0;

	g_variant_get(params, "(&o&o&s&s@a{sv})", &handle, &session_handle,
	              &app_id, &parent_window, NULL);

	session = g_hash_table_lookup(self->sessions, session_handle);
	if (session == NULL) {
		empty_reply(invocation, PORTAL_RESPONSE_OTHER);
		return;
	}

	sources = portal_capture_list_sources(self->capture,
		(session->source_types & SOURCE_TYPE_WINDOW) != 0,
		(session->source_types & SOURCE_TYPE_MONITOR) != 0);
	if (sources->len == 0) {
		g_warning("screencast: nothing to share");
		empty_reply(invocation, PORTAL_RESPONSE_OTHER);
		return;
	}

	chosen = run_chooser(sources, &cancelled);
	if (cancelled) {
		empty_reply(invocation, PORTAL_RESPONSE_CANCELLED);
		return;
	}
	if (chosen == NULL) {
		/* No chooser installed, or it answered with something not on the
		 * menu.  The first source is the first output, which is the
		 * behaviour portal-wlr has without a chooser configured. */
		chosen = g_ptr_array_index(sources, 0);
		g_message("screencast: no chooser; sharing '%s'. Set "
		          "GOWL_PORTAL_CHOOSER to pick.", chosen->title);
	}

	session->stream = portal_pw_stream_new(self->pw, self->capture, chosen,
	                                       session->with_cursor, &error);
	if (session->stream == NULL) {
		g_warning("screencast: could not start the stream: %s",
		          error->message);
		empty_reply(invocation, PORTAL_RESPONSE_OTHER);
		return;
	}
	node_id = portal_pw_stream_node_id(session->stream);
	portal_pw_stream_get_size(session->stream, &w, &h);

	/*
	 * One stream, as (node_id, properties).  `size' and `source_type'
	 * are what a consumer reads to lay the video out; `position' is
	 * meaningless for a window and omitted for one.
	 */
	g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&props, "{sv}", "source_type",
		g_variant_new_uint32(chosen->is_window
		                     ? SOURCE_TYPE_WINDOW : SOURCE_TYPE_MONITOR));
	g_variant_builder_add(&props, "{sv}", "id",
		g_variant_new_string(chosen->id != NULL ? chosen->id : ""));
	if (w > 0 && h > 0)
		g_variant_builder_add(&props, "{sv}", "size",
			g_variant_new("(ii)", (gint32)w, (gint32)h));
	g_variant_builder_add(&streams, "(u@a{sv})", node_id,
		g_variant_builder_end(&props));

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "streams",
		g_variant_builder_end(&streams));
	g_dbus_method_invocation_return_value(invocation,
		g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS, &results));

	g_message("screencast: '%s' is casting %s (node %u)", app_id,
	          chosen->title, node_id);
}

static void
sc_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
          const gchar *iface, const gchar *method, GVariant *params,
          GDBusMethodInvocation *invocation, gpointer user_data)
{
	PortalScreenCast *self = user_data;

	(void)conn; (void)sender; (void)path; (void)iface;

	if (g_strcmp0(method, "CreateSession") == 0)
		handle_create_session(self, params, invocation);
	else if (g_strcmp0(method, "SelectSources") == 0)
		handle_select_sources(self, params, invocation);
	else if (g_strcmp0(method, "Start") == 0)
		handle_start(self, params, invocation);
	else
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "no such method: %s", method);
}

static GVariant *
sc_get_property(GDBusConnection *conn, const gchar *sender, const gchar *path,
                const gchar *iface, const gchar *name, GError **error,
                gpointer user_data)
{
	PortalScreenCast *self = user_data;

	(void)conn; (void)sender; (void)path; (void)iface;

	if (g_strcmp0(name, "version") == 0)
		return g_variant_new_uint32(4);
	if (g_strcmp0(name, "AvailableSourceTypes") == 0)
		return g_variant_new_uint32(SOURCE_TYPE_MONITOR
			| (self->can_window ? SOURCE_TYPE_WINDOW : 0));
	if (g_strcmp0(name, "AvailableCursorModes") == 0)
		/* HIDDEN | EMBEDDED.  No METADATA: that needs a cursor stream
		 * of its own, which this backend does not produce. */
		return g_variant_new_uint32(1 | 2);
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
	            "no such property: %s", name);
	return NULL;
}

static const GDBusInterfaceVTable sc_vtable = {
	sc_method, sc_get_property, NULL, { 0 }
};

/* ---------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------- */

PortalScreenCast *
portal_screencast_new(GDBusConnection *conn, GError **error)
{
	PortalScreenCast *self;

	g_return_val_if_fail(conn != NULL, NULL);

	self = g_new0(PortalScreenCast, 1);
	/* Owned, not borrowed: the bus name can be lost (another backend
	 * replacing us) while sessions are still open, and tearing those
	 * down touches the connection.  A borrowed pointer is dangling by
	 * then. */
	self->conn = g_object_ref(conn);
	self->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                       session_destroy);

	self->capture = portal_capture_new(error);
	if (self->capture == NULL) {
		portal_screencast_free(self);
		return NULL;
	}
	if (!portal_capture_available(self->capture, &self->can_window)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"the compositor offers no image-copy-capture");
		portal_screencast_free(self);
		return NULL;
	}
	self->pw = portal_pw_new(error);
	if (self->pw == NULL) {
		portal_screencast_free(self);
		return NULL;
	}

	if (sc_node_info == NULL)
		sc_node_info = g_dbus_node_info_new_for_xml(sc_introspection_xml,
		                                            NULL);
	if (sc_session_node_info == NULL)
		sc_session_node_info = g_dbus_node_info_new_for_xml(
			sc_session_introspection_xml, NULL);

	self->reg_id = g_dbus_connection_register_object(conn, SCREENCAST_PATH,
		sc_node_info->interfaces[0], &sc_vtable, self, NULL, error);
	if (self->reg_id == 0) {
		portal_screencast_free(self);
		return NULL;
	}

	g_message("screencast: serving %s window capture",
	          self->can_window ? "output and" : "output-only, no");
	return self;
}

void
portal_screencast_free(PortalScreenCast *self)
{
	if (self == NULL)
		return;
	if (self->reg_id != 0 && self->conn != NULL
	    && !g_dbus_connection_is_closed(self->conn))
		g_dbus_connection_unregister_object(self->conn, self->reg_id);
	/* The sessions first: each unregisters its own object. */
	g_clear_pointer(&self->sessions, g_hash_table_unref);
	g_clear_object(&self->conn);
	g_clear_pointer(&self->pw, portal_pw_free);
	g_clear_pointer(&self->capture, portal_capture_free);
	g_free(self);
}
