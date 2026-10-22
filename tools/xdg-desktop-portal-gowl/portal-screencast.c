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
 * The chooser is portal-chooser.c: gowl's own menu card when the
 * portal runs inside gowl, else a dmenu-style command
 * ($GOWL_PORTAL_CHOOSER, or the first of bemenu/fuzzel/wofi/rofi/dmenu
 * on PATH).  With none of them available the cast falls back to the
 * first output, which is what portal-wlr would have given anyway -- a
 * call that starts sharing the wrong screen is recoverable, one that
 * fails to start is an unanswerable support question.
 *
 * VERSION 6.  Restore (v4): SelectSources' persist_mode and
 * restore_data are honoured through portal-restore.c, and Start
 * returns persist_mode and fresh restore_data whenever the session
 * asked to persist.  A restore that finds its source skips the
 * chooser; one that does not asks, exactly as a first session does.
 * pipewire-serial (v6) is in every stream's properties.  mapping_id
 * (v5) is optional and only means something to a RemoteDesktop session
 * with screen-cast streams, which this backend does not combine.
 *
 * CURSOR MODES.  Hidden and Embedded only.  Embedded is the
 * compositor's PAINT_CURSORS capture option; Metadata would need a
 * second, cursor-only capture session and SPA_META_Cursor buffers,
 * which this backend does not produce -- so it is not advertised, and a
 * session that asks for it anyway is closed, as the spec requires.
 */

#include "portal-screencast.h"
#include "portal-capture.h"
#include "portal-chooser.h"
#include "portal-pw.h"
#include "portal-restore.h"

#include <string.h>

/* The frontend's response codes. */
#define PORTAL_RESPONSE_SUCCESS   0
#define PORTAL_RESPONSE_CANCELLED 1
#define PORTAL_RESPONSE_OTHER     2

/* org.freedesktop.portal.ScreenCast source types. */
#define SOURCE_TYPE_MONITOR 1
#define SOURCE_TYPE_WINDOW  2

/* org.freedesktop.portal.ScreenCast cursor modes. */
#define CURSOR_MODE_HIDDEN   1
#define CURSOR_MODE_EMBEDDED 2
#define CURSOR_MODES_AVAILABLE (CURSOR_MODE_HIDDEN | CURSOR_MODE_EMBEDDED)

/* The interface version served: see the file comment. */
#define SCREENCAST_VERSION 6

/* Our restore_data: ("gowl", RESTORE_DATA_VERSION, <s token>). */
#define RESTORE_VENDOR       "gowl"
#define RESTORE_DATA_VERSION 1

#define SCREENCAST_PATH "/org/freedesktop/portal/desktop"

typedef struct {
	PortalScreenCast *portal;
	gchar            *handle;       /* the session object path */
	gchar            *app_id;
	guint             session_reg_id;
	guint32           source_types;  /* what SelectSources asked for */
	gboolean          with_cursor;
	guint32           persist_mode;  /* 0, 1 or 2, from SelectSources */
	gchar            *restore_token; /* out of restore_data, or NULL */
	PortalPwStream   *stream;
} ScreenCastSession;

struct _PortalScreenCast {
	GDBusConnection *conn;
	guint            reg_id;
	PortalCapture   *capture;    /* borrowed: shared with Screenshot */
	PortalPw        *pw;
	PortalRestore   *restore;
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

/*
 * Puts @sources in front of a human.  NULL with @cancelled unset means
 * there was no chooser to run, and the caller falls back.
 */
static const PortalCaptureSource *
choose_source(GPtrArray *sources, gboolean *cancelled)
{
	g_autofree const gchar **lines = NULL;
	guint index = 0;
	guint i;

	*cancelled = FALSE;
	lines = g_new0(const gchar *, sources->len + 1);
	for (i = 0; i < sources->len; i++)
		lines[i] = ((const PortalCaptureSource *)
		            g_ptr_array_index(sources, i))->title;

	switch (portal_chooser_run("Share", lines, &index)) {
	case PORTAL_CHOOSER_CHOSEN:
		return index < sources->len ? g_ptr_array_index(sources, index)
		                            : NULL;
	case PORTAL_CHOOSER_CANCELLED:
		*cancelled = TRUE;
		return NULL;
	case PORTAL_CHOOSER_UNAVAILABLE:
	default:
		return NULL;
	}
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
	g_free(session->restore_token);
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
	g_autoptr(GVariant) restore = NULL;
	ScreenCastSession *session;
	guint32 types = 0;
	guint32 cursor_mode = 0;
	guint32 persist = 0;

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
	/*
	 * A cursor mode we did not advertise closes the session: the spec
	 * says so, and quietly treating METADATA as HIDDEN would hand the
	 * application a cast without the pointer it asked for and no way
	 * to know.  Exactly one mode bit, and one of ours.
	 */
	if (g_variant_lookup(options, "cursor_mode", "u", &cursor_mode)) {
		if (cursor_mode == 0 || (cursor_mode & (cursor_mode - 1)) != 0
		    || (cursor_mode & ~(guint32)CURSOR_MODES_AVAILABLE) != 0) {
			g_message("screencast: %s asked for cursor mode %u, which "
			          "is not offered; closing the session", app_id,
			          cursor_mode);
			empty_reply(invocation, PORTAL_RESPONSE_OTHER);
			g_hash_table_remove(self->sessions, session_handle);
			return;
		}
		session->with_cursor = cursor_mode == CURSOR_MODE_EMBEDDED;
	}

	/* persist_mode: 0 none, 1 while the app runs, 2 until revoked. */
	if (g_variant_lookup(options, "persist_mode", "u", &persist))
		session->persist_mode = MIN(persist, 2u);

	/*
	 * restore_data is ANYBODY's: the frontend hands back whatever a
	 * backend once returned, and that may have been GNOME's or KDE's
	 * before the user switched desktops.  Ours, or ignored.
	 */
	restore = g_variant_lookup_value(options, "restore_data",
	                                 G_VARIANT_TYPE("(suv)"));
	if (restore != NULL) {
		const gchar *vendor = NULL;
		guint32 version = 0;
		g_autoptr(GVariant) data = NULL;

		g_variant_get(restore, "(&suv)", &vendor, &version, &data);
		g_clear_pointer(&session->restore_token, g_free);
		if (g_strcmp0(vendor, RESTORE_VENDOR) == 0
		    && version == RESTORE_DATA_VERSION
		    && g_variant_is_of_type(data, G_VARIANT_TYPE_STRING))
			session->restore_token = g_variant_dup_string(data, NULL);
		else
			g_debug("screencast: restore data from '%s' v%u is not "
			        "ours", vendor, version);
	}

	g_debug("screencast: %s wants types 0x%x, cursor %s, persist %u%s",
	        app_id, session->source_types,
	        session->with_cursor ? "on" : "off", session->persist_mode,
	        session->restore_token != NULL ? ", restoring" : "");
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
	const PortalCaptureSource *chosen = NULL;
	const PortalGrant *grant = NULL;
	g_autofree gchar *old_token = NULL;
	g_autofree gchar *stream_id = NULL;
	gboolean cancelled = FALSE;
	GVariantBuilder results;
	GVariantBuilder streams;
	GVariantBuilder props;
	guint32 node_id;
	guint64 serial;
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

	/*
	 * A restore first.  The grant has to be this application's, and
	 * its source has to still be here and be a kind the session asked
	 * for; anything less and the human is asked, as the spec wants --
	 * a restore that guesses shares the wrong thing silently.
	 */
	if (session->restore_token != NULL) {
		grant = portal_restore_lookup(self->restore,
		                              session->restore_token, app_id);
		if (grant != NULL)
			chosen = portal_restore_match(grant, sources);
		if (chosen != NULL) {
			old_token = g_strdup(grant->token);
			stream_id = g_strdup(grant->stream_id);
			g_message("screencast: restored '%s' for '%s' without "
			          "asking", chosen->title, app_id);
		} else {
			g_message("screencast: %s for '%s'; asking", grant != NULL
			          ? "the restored source is gone"
			          : "unknown restore token", app_id);
		}
	}

	if (chosen == NULL)
		chosen = choose_source(sources, &cancelled);
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
	serial = portal_pw_stream_serial(session->stream);
	portal_pw_stream_get_size(session->stream, &w, &h);

	/*
	 * The grant.  Single use: a restored one is consumed and replaced,
	 * so the token the application holds next time is a new one, and
	 * the stream keeps the id it had.
	 */
	if (old_token != NULL)
		portal_restore_remove(self->restore, old_token);
	grant = NULL;
	if (session->persist_mode > 0)
		grant = portal_restore_add(self->restore, app_id,
		                           session->persist_mode, chosen,
		                           stream_id);

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
	/* `id' persists across restored sessions (v4): the grant's. */
	g_variant_builder_add(&props, "{sv}", "id",
		g_variant_new_string(grant != NULL ? grant->stream_id
		                     : chosen->id != NULL ? chosen->id : ""));
	if (w > 0 && h > 0)
		g_variant_builder_add(&props, "{sv}", "size",
			g_variant_new("(ii)", (gint32)w, (gint32)h));
	/* `position' (compositor space) for a monitor only. */
	if (!chosen->is_window)
		g_variant_builder_add(&props, "{sv}", "position",
			g_variant_new("(ii)", chosen->x, chosen->y));
	if (serial != 0)
		g_variant_builder_add(&props, "{sv}", "pipewire-serial",
			g_variant_new_uint64(serial));
	g_variant_builder_add(&streams, "(u@a{sv})", node_id,
		g_variant_builder_end(&props));

	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "streams",
		g_variant_builder_end(&streams));
	/* Restore (v4): the mode granted, and what to hand back next time. */
	if (grant != NULL) {
		g_variant_builder_add(&results, "{sv}", "persist_mode",
			g_variant_new_uint32(grant->persist_mode));
		g_variant_builder_add(&results, "{sv}", "restore_data",
			g_variant_new("(suv)", RESTORE_VENDOR,
			              (guint32)RESTORE_DATA_VERSION,
			              g_variant_new_string(grant->token)));
	}
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
		return g_variant_new_uint32(SCREENCAST_VERSION);
	if (g_strcmp0(name, "AvailableSourceTypes") == 0)
		return g_variant_new_uint32(SOURCE_TYPE_MONITOR
			| (self->can_window ? SOURCE_TYPE_WINDOW : 0));
	if (g_strcmp0(name, "AvailableCursorModes") == 0)
		/* HIDDEN | EMBEDDED.  No METADATA: that needs a cursor stream
		 * of its own, which this backend does not produce. */
		return g_variant_new_uint32(CURSOR_MODES_AVAILABLE);
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
portal_screencast_new(GDBusConnection *conn, PortalCapture *capture,
                      GError **error)
{
	PortalScreenCast *self;

	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(capture != NULL, NULL);

	self = g_new0(PortalScreenCast, 1);
	/* Owned, not borrowed: the bus name can be lost (another backend
	 * replacing us) while sessions are still open, and tearing those
	 * down touches the connection.  A borrowed pointer is dangling by
	 * then. */
	self->conn = g_object_ref(conn);
	self->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                       session_destroy);

	self->capture = capture;
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
	self->restore = portal_restore_new(NULL);

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
	g_clear_pointer(&self->restore, portal_restore_free);
	/* The capture context is the caller's. */
	self->capture = NULL;
	g_free(self);
}
