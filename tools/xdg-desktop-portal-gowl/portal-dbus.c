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
 * The D-Bus impl-portal backend.  Owns
 * org.freedesktop.impl.portal.desktop.gowl and implements:
 *
 *   org.freedesktop.impl.portal.InputCapture  (egress: deskflow server)
 *   org.freedesktop.impl.portal.RemoteDesktop (ingress: deskflow client)
 *   org.freedesktop.impl.portal.Session       (the per-session object)
 *
 * The methods return synchronously with a portal response code (0 = ok)
 * and delegate to PortalWayland (the compositor protocol) and PortalEis
 * (the EIS server).  Activation/zone-changed forwarded from the Wayland
 * layer are emitted as InputCapture signals.
 *
 * This minimal backend supports a single active session (deskflow uses
 * one at a time), which keeps the session bookkeeping simple while
 * remaining spec-correct for the common case.
 */

#include "portal-dbus.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#define PORTAL_BUS_NAME  "org.freedesktop.impl.portal.desktop.gowl"
#define PORTAL_OBJ_PATH  "/org/freedesktop/portal/desktop"

#define PORTAL_RESPONSE_SUCCESS   0
#define PORTAL_RESPONSE_CANCELLED 1
#define PORTAL_RESPONSE_OTHER     2

/* Capabilities: KEYBOARD (1) | POINTER (2). */
#define PORTAL_CAPS (1u | 2u)
/* RemoteDesktop device types: KEYBOARD (1) | POINTER (2). */
#define PORTAL_DEVICE_TYPES (1u | 2u)

struct _PortalDbus {
	PortalWayland   *wl;     /* not owned */
	PortalEis       *eis;    /* not owned */

	GDBusConnection *conn;
	guint            owner_id;
	guint            ic_reg_id;     /* InputCapture object */
	guint            rd_reg_id;     /* RemoteDesktop object */

	/* The single active session.  @session_handle is the object path the
	 * frontend supplied to CreateSession; @session_reg_id is the
	 * registration of the org.freedesktop.impl.portal.Session object we
	 * export at that path (0 when no session is active). */
	gchar           *session_handle;
	guint            session_reg_id;
	guint            zone_set;      /* last zones reported */
};

static GDBusNodeInfo *node_info;
static GDBusNodeInfo *session_node_info;

/* ---------------------------------------------------------------
 * Introspection XML
 * --------------------------------------------------------------- */

static const gchar introspection_xml[] =
"<node>"
"  <interface name='org.freedesktop.impl.portal.InputCapture'>"
"    <property name='version' type='u' access='read'/>"
"    <property name='SupportedCapabilities' type='u' access='read'/>"
"    <method name='CreateSession'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='s' name='parent_window' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='CreateSession2'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
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
"    <method name='GetZones'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='SetPointerBarriers'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='aa{sv}' name='barriers' direction='in'/>"
"      <arg type='u' name='zone_set' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='Enable'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"    </method>"
"    <method name='Disable'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"    </method>"
"    <method name='Release'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"    </method>"
"    <method name='ConnectToEIS'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='h' name='fd' direction='out'/>"
"    </method>"
"    <signal name='Activated'>"
"      <arg type='o' name='session_handle'/>"
"      <arg type='a{sv}' name='options'/>"
"    </signal>"
"    <signal name='Deactivated'>"
"      <arg type='o' name='session_handle'/>"
"      <arg type='a{sv}' name='options'/>"
"    </signal>"
"    <signal name='Disabled'>"
"      <arg type='o' name='session_handle'/>"
"      <arg type='a{sv}' name='options'/>"
"    </signal>"
"    <signal name='ZonesChanged'>"
"      <arg type='o' name='session_handle'/>"
"      <arg type='a{sv}' name='options'/>"
"    </signal>"
"  </interface>"
"  <interface name='org.freedesktop.impl.portal.RemoteDesktop'>"
"    <property name='version' type='u' access='read'/>"
"    <property name='AvailableDeviceTypes' type='u' access='read'/>"
"    <method name='CreateSession'>"
"      <arg type='o' name='handle' direction='in'/>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='response' direction='out'/>"
"      <arg type='a{sv}' name='results' direction='out'/>"
"    </method>"
"    <method name='SelectDevices'>"
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
"    <method name='NotifyPointerMotion'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='d' name='dx' direction='in'/>"
"      <arg type='d' name='dy' direction='in'/>"
"    </method>"
"    <method name='NotifyPointerMotionAbsolute'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='stream' direction='in'/>"
"      <arg type='d' name='x' direction='in'/>"
"      <arg type='d' name='y' direction='in'/>"
"    </method>"
"    <method name='NotifyPointerButton'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='i' name='button' direction='in'/>"
"      <arg type='u' name='state' direction='in'/>"
"    </method>"
"    <method name='NotifyPointerAxisDiscrete'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='u' name='axis' direction='in'/>"
"      <arg type='i' name='steps' direction='in'/>"
"    </method>"
"    <method name='NotifyKeyboardKeycode'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='i' name='keycode' direction='in'/>"
"      <arg type='u' name='state' direction='in'/>"
"    </method>"
"    <method name='ConnectToEIS'>"
"      <arg type='o' name='session_handle' direction='in'/>"
"      <arg type='s' name='app_id' direction='in'/>"
"      <arg type='a{sv}' name='options' direction='in'/>"
"      <arg type='h' name='fd' direction='out'/>"
"    </method>"
"  </interface>"
"</node>";

/*
 * The shared session object interface.  CreateSession (on both portals)
 * must register an object implementing this at the caller-supplied
 * session_handle object path; the xdg-desktop-portal frontend drives the
 * session through it and calls Close() to end it.  Without this object the
 * frontend's "input capture session created ... closed" immediately tears
 * the session down (Failed to close session implementation: ... Object
 * does not exist), and the public CreateSession fails.
 */
static const gchar session_introspection_xml[] =
"<node>"
"  <interface name='org.freedesktop.impl.portal.Session'>"
"    <method name='Close'/>"
"    <signal name='Closed'/>"
"    <property name='version' type='u' access='read'/>"
"  </interface>"
"</node>";

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */

/*
 * Fix: CWE-862 Missing Authorization — validate that an incoming D-Bus
 * method call's session_handle matches the one we recorded during
 * CreateSession before performing any privileged operation.  Without this
 * check any client on the session bus can call Enable, ConnectToEIS, etc.
 * without having first called CreateSession, bypassing the session handshake
 * entirely.  ConnectToEIS is the most dangerous path: a caller that skips
 * CreateSession would receive a live EIS fd granting full keyboard+mouse
 * injection into the compositor.
 *
 * Returns TRUE and replies with PORTAL_RESPONSE_OTHER if the session is
 * not active or the handle does not match, so the caller can return
 * immediately after invoking this guard.  Returns FALSE when the handle is
 * valid and the caller should proceed normally.
 */
static gboolean
reject_if_no_session(PortalDbus *self, GDBusMethodInvocation *invocation,
                     const gchar *session_handle)
{
	if (self->session_handle == NULL
	    || g_strcmp0(session_handle, self->session_handle) != 0) {
		g_dbus_method_invocation_return_error_literal(invocation,
			G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
			"no active session or session_handle mismatch");
		return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------
 * Session object (org.freedesktop.impl.portal.Session)
 *
 * Exported at the caller's session_handle path during CreateSession.  The
 * frontend calls Close() on it to end the session; we also unexport it
 * (and emit Closed) when the session tears down on our side.
 * --------------------------------------------------------------- */

static void portal_dbus_end_session(PortalDbus *self, gboolean emit_closed);

static void
session_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *method, GVariant *params,
               GDBusMethodInvocation *invocation, gpointer user_data)
{
	PortalDbus *self = user_data;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)params;

	if (g_strcmp0(method, "Close") == 0) {
		/* The frontend is closing the session: tear down our state and
		 * unexport the object, then ack.  Do not emit Closed (the peer
		 * initiated the close). */
		portal_dbus_end_session(self, FALSE);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method);
}

static GVariant *
session_get_property(GDBusConnection *conn, const gchar *sender,
                     const gchar *path, const gchar *iface, const gchar *prop,
                     GError **error, gpointer user_data)
{
	(void)conn; (void)sender; (void)path; (void)iface;
	(void)error; (void)user_data;

	if (g_strcmp0(prop, "version") == 0)
		return g_variant_new_uint32(1);
	return NULL;
}

static const GDBusInterfaceVTable session_vtable = {
	session_method, session_get_property, NULL, { 0 }
};

/*
 * Register the Session object at @session_handle and record it as the
 * single active session.  Any previously-active session is torn down
 * first (deskflow only runs one at a time).  Returns TRUE on success.
 */
static gboolean
portal_dbus_begin_session(PortalDbus *self, const gchar *session_handle)
{
	GError *error = NULL;
	guint   reg_id;

	if (self->session_reg_id != 0)
		portal_dbus_end_session(self, TRUE);

	reg_id = g_dbus_connection_register_object(self->conn, session_handle,
		session_node_info->interfaces[0], &session_vtable,
		self, NULL, &error);
	if (reg_id == 0) {
		g_warning("portal: failed to export Session at %s: %s",
			session_handle, error ? error->message : "?");
		g_clear_error(&error);
		return FALSE;
	}

	g_free(self->session_handle);
	self->session_handle = g_strdup(session_handle);
	self->session_reg_id = reg_id;
	return TRUE;
}

/*
 * Tear down the active session: stop capture, optionally emit Closed (when
 * we initiate the teardown), unexport the Session object, and clear state.
 */
static void
portal_dbus_end_session(PortalDbus *self, gboolean emit_closed)
{
	if (self->session_reg_id == 0)
		return;

	if (emit_closed && self->conn != NULL && self->session_handle != NULL)
		g_dbus_connection_emit_signal(self->conn, NULL,
			self->session_handle,
			"org.freedesktop.impl.portal.Session", "Closed",
			NULL, NULL);

	g_dbus_connection_unregister_object(self->conn, self->session_reg_id);
	self->session_reg_id = 0;
	g_clear_pointer(&self->session_handle, g_free);
}

/* Return an empty a{sv} results dict. */
static GVariant *
empty_vardict(void)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	return g_variant_builder_end(&b);
}

/* Mint the EIS client fd and return it via a method invocation that has
 * an `h' out arg, attaching it to a GUnixFDList. */
static void
return_eis_fd(PortalDbus *self, GDBusMethodInvocation *invocation)
{
	GUnixFDList *fds;
	int          client_fd;
	int          idx;
	GError      *error = NULL;

	client_fd = portal_eis_connect_fd(self->eis);
	if (client_fd < 0) {
		g_dbus_method_invocation_return_error_literal(invocation,
			G_IO_ERROR, G_IO_ERROR_FAILED,
			"failed to create an EIS client fd");
		return;
	}

	fds = g_unix_fd_list_new();
	idx = g_unix_fd_list_append(fds, client_fd, &error);
	close(client_fd);
	if (idx < 0) {
		g_dbus_method_invocation_return_gerror(invocation, error);
		g_clear_error(&error);
		g_object_unref(fds);
		return;
	}

	g_dbus_method_invocation_return_value_with_unix_fd_list(invocation,
		g_variant_new("(h)", idx), fds);
	g_object_unref(fds);
}

/* ---------------------------------------------------------------
 * Activation / zones-changed bridges -> InputCapture signals
 * --------------------------------------------------------------- */

static void
on_activation(guint32 activation_id, double x, double y, guint32 barrier_id,
              gboolean activated, gpointer user_data)
{
	PortalDbus *self = user_data;
	GVariantBuilder opts;

	if (self->session_handle == NULL)
		return;

	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	if (activated) {
		g_variant_builder_add(&opts, "{sv}", "activation_id",
			g_variant_new_uint32(activation_id));
		g_variant_builder_add(&opts, "{sv}", "cursor_position",
			g_variant_new("(dd)", x, y));
		g_variant_builder_add(&opts, "{sv}", "barrier_id",
			g_variant_new_uint32(barrier_id));
		g_dbus_connection_emit_signal(self->conn, NULL,
			PORTAL_OBJ_PATH,
			"org.freedesktop.impl.portal.InputCapture", "Activated",
			g_variant_new("(oa{sv})", self->session_handle, &opts),
			NULL);
	} else {
		g_variant_builder_add(&opts, "{sv}", "activation_id",
			g_variant_new_uint32(activation_id));
		g_dbus_connection_emit_signal(self->conn, NULL,
			PORTAL_OBJ_PATH,
			"org.freedesktop.impl.portal.InputCapture", "Deactivated",
			g_variant_new("(oa{sv})", self->session_handle, &opts),
			NULL);
	}
}

static void
on_zones_changed(guint32 zone_set, gpointer user_data)
{
	PortalDbus *self = user_data;
	GVariantBuilder opts;

	if (self->session_handle == NULL)
		return;

	self->zone_set = zone_set;
	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&opts, "{sv}", "zone_set",
		g_variant_new_uint32(zone_set));
	g_dbus_connection_emit_signal(self->conn, NULL, PORTAL_OBJ_PATH,
		"org.freedesktop.impl.portal.InputCapture", "ZonesChanged",
		g_variant_new("(oa{sv})", self->session_handle, &opts), NULL);
}

/* ---------------------------------------------------------------
 * InputCapture method dispatch
 * --------------------------------------------------------------- */

static void
ic_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
          const gchar *iface, const gchar *method, GVariant *params,
          GDBusMethodInvocation *invocation, gpointer user_data)
{
	PortalDbus *self = user_data;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;

	if (g_strcmp0(method, "CreateSession") == 0) {
		const gchar *handle, *session_handle, *app_id, *parent;
		GVariant *opts;
		GVariantBuilder results;

		g_variant_get(params, "(&o&o&s&s@a{sv})", &handle,
			&session_handle, &app_id, &parent, &opts);
		g_variant_unref(opts);

		/* Export the Session object at session_handle; the frontend
		 * drives and Close()s the session through it.  Without this the
		 * frontend tears the session down immediately and the public
		 * CreateSession fails. */
		if (!portal_dbus_begin_session(self, session_handle)) {
			g_variant_builder_init(&results,
				G_VARIANT_TYPE("a{sv}"));
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(ua{sv})", PORTAL_RESPONSE_OTHER,
					&results));
			return;
		}

		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&results, "{sv}", "capabilities",
			g_variant_new_uint32(PORTAL_CAPS));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "CreateSession2") == 0) {
		const gchar *session_handle, *app_id;
		GVariant *opts;
		GVariantBuilder results;

		/* The form xdg-desktop-portal >= 1.20 prefers: no handle and no
		 * response code, just results.  Export the Session object at
		 * session_handle (without it the frontend tears the session down
		 * immediately and the public CreateSession fails). */
		g_variant_get(params, "(&o&s@a{sv})", &session_handle, &app_id,
			&opts);
		g_variant_unref(opts);

		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		if (!portal_dbus_begin_session(self, session_handle)) {
			/* Signal failure via an empty result with no
			 * capabilities; the frontend treats a missing Session
			 * object as failure regardless. */
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(a{sv})", &results));
			return;
		}
		g_variant_builder_add(&results, "{sv}", "capabilities",
			g_variant_new_uint32(PORTAL_CAPS));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(a{sv})", &results));
		return;
	}

	if (g_strcmp0(method, "Start") == 0) {
		const gchar *handle, *session_handle, *app_id, *parent;
		GVariant *opts;
		GVariantBuilder results;

		/* Start confirms the session is ready to capture.  Validate the
		 * handle, then return success with the negotiated capabilities. */
		g_variant_get(params, "(&o&o&s&s@a{sv})", &handle,
			&session_handle, &app_id, &parent, &opts);
		g_variant_unref(opts);
		if (reject_if_no_session(self, invocation, session_handle))
			return;

		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&results, "{sv}", "capabilities",
			g_variant_new_uint32(PORTAL_CAPS));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "GetZones") == 0) {
		const gchar *handle, *session_handle, *app_id;
		GVariant *opts;
		PortalZone *zones = NULL;
		guint n = 0, i;
		guint32 zone_set = 0;
		GVariantBuilder results, zarr;

		/* GetZones has NO parent_window arg: (o handle, o session_handle,
		 * s app_id, a{sv} options).  Fix: CWE-862 — validate
		 * session_handle before exposing zone layout. */
		g_variant_get(params, "(&o&o&s@a{sv})", &handle,
			&session_handle, &app_id, &opts);
		g_variant_unref(opts);
		if (reject_if_no_session(self, invocation, session_handle))
			return;

		portal_wayland_get_zones(self->wl, &zones, &n, &zone_set);
		self->zone_set = zone_set;

		g_variant_builder_init(&zarr, G_VARIANT_TYPE("a(uuii)"));
		for (i = 0; i < n; i++)
			g_variant_builder_add(&zarr, "(uuii)",
				zones[i].width, zones[i].height,
				zones[i].x, zones[i].y);

		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&results, "{sv}", "zones",
			g_variant_builder_end(&zarr));
		g_variant_builder_add(&results, "{sv}", "zone_set",
			g_variant_new_uint32(zone_set));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "SetPointerBarriers") == 0) {
		const gchar *handle, *session_handle, *app_id;
		GVariant *opts, *barriers;
		GVariantIter it;
		GVariant *barrier;
		guint32 zone_set;
		GVariantBuilder results, failed;

		g_variant_get(params, "(&o&o&s@a{sv}@aa{sv}u)", &handle,
			&session_handle, &app_id, &opts, &barriers, &zone_set);
		g_variant_unref(opts);

		/* Fix: CWE-862 — reject barrier installation from any caller
		 * that does not own the active session. */
		if (reject_if_no_session(self, invocation, session_handle)) {
			g_variant_unref(barriers);
			return;
		}

		/* Stage each barrier, then install. */
		g_variant_iter_init(&it, barriers);
		while ((barrier = g_variant_iter_next_value(&it)) != NULL) {
			GVariantDict d;
			guint32 id = 0;
			GVariant *posv;

			g_variant_dict_init(&d, barrier);
			g_variant_dict_lookup(&d, "barrier_id", "u", &id);
			posv = g_variant_dict_lookup_value(&d, "position",
				G_VARIANT_TYPE("(iiii)"));
			if (posv != NULL) {
				gint x1, y1, x2, y2;
				g_variant_get(posv, "(iiii)", &x1, &y1, &x2, &y2);
				portal_wayland_add_barrier(self->wl, id,
					x1, y1, x2, y2);
				g_variant_unref(posv);
			}
			g_variant_dict_clear(&d);
			g_variant_unref(barrier);
		}
		g_variant_unref(barriers);

		portal_wayland_set_barriers(self->wl, zone_set);

		/* failed_barriers: empty (the compositor accepted the valid
		 * subset; per-id status is reported on the wire but the
		 * minimal backend reports none failed). */
		g_variant_builder_init(&failed, G_VARIANT_TYPE("au"));
		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&results, "{sv}", "failed_barriers",
			g_variant_builder_end(&failed));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "Enable") == 0) {
		const gchar *session_handle, *app_id;
		GVariant *opts;

		/* Fix: CWE-862 — parse and validate session_handle; previously
		 * Enable operated unconditionally on any D-Bus call, allowing a
		 * rogue client to arm input capture without owning a session. */
		g_variant_get(params, "(&o&s@a{sv})", &session_handle,
			&app_id, &opts);
		g_variant_unref(opts);
		if (reject_if_no_session(self, invocation, session_handle))
			return;

		portal_wayland_enable(self->wl);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "Disable") == 0) {
		const gchar *session_handle, *app_id;
		GVariant *opts;

		/* Fix: CWE-862 — parse and validate session_handle; previously
		 * Disable operated unconditionally, letting any D-Bus client
		 * disarm another session's capture and emit a Disabled signal
		 * with a forged object path. */
		g_variant_get(params, "(&o&s@a{sv})", &session_handle,
			&app_id, &opts);
		g_variant_unref(opts);
		if (reject_if_no_session(self, invocation, session_handle))
			return;

		portal_wayland_disable(self->wl);
		g_dbus_connection_emit_signal(self->conn, NULL, PORTAL_OBJ_PATH,
			"org.freedesktop.impl.portal.InputCapture", "Disabled",
			g_variant_new("(oa{sv})",
				self->session_handle ? self->session_handle
				                     : "/",
				empty_vardict()), NULL);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "Release") == 0) {
		const gchar *session_handle, *app_id;
		GVariant *opts;
		GVariantDict d;
		guint32 activation_id = 0;
		gboolean has_pos = FALSE;
		GVariant *cursor;
		double x = 0, y = 0;

		g_variant_get(params, "(&o&s@a{sv})", &session_handle,
			&app_id, &opts);

		/* Fix: CWE-862 — validate session_handle before releasing
		 * capture; without this any D-Bus client could force-release
		 * an active capture belonging to another session. */
		if (reject_if_no_session(self, invocation, session_handle)) {
			g_variant_unref(opts);
			return;
		}

		g_variant_dict_init(&d, opts);
		g_variant_dict_lookup(&d, "activation_id", "u", &activation_id);
		cursor = g_variant_dict_lookup_value(&d, "cursor_position",
			G_VARIANT_TYPE("(dd)"));
		if (cursor != NULL) {
			g_variant_get(cursor, "(dd)", &x, &y);
			has_pos = TRUE;
			g_variant_unref(cursor);
		}
		g_variant_dict_clear(&d);
		g_variant_unref(opts);

		portal_wayland_release(self->wl, activation_id, has_pos, x, y);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "ConnectToEIS") == 0) {
		const gchar *session_handle, *app_id;
		GVariant *opts;

		/* Fix: CWE-862 — ConnectToEIS returns a live EIS fd that grants
		 * full keyboard+mouse injection into the compositor.  Without
		 * session validation any D-Bus session-bus client could obtain
		 * one without going through CreateSession. */
		g_variant_get(params, "(&o&s@a{sv})", &session_handle,
			&app_id, &opts);
		g_variant_unref(opts);
		if (reject_if_no_session(self, invocation, session_handle))
			return;

		return_eis_fd(self, invocation);
		return;
	}

	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method);
}

/* ---------------------------------------------------------------
 * RemoteDesktop method dispatch
 * --------------------------------------------------------------- */

static void
rd_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
          const gchar *iface, const gchar *method, GVariant *params,
          GDBusMethodInvocation *invocation, gpointer user_data)
{
	PortalDbus *self = user_data;
	GVariantBuilder results;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;

	if (g_strcmp0(method, "CreateSession") == 0) {
		const gchar *handle, *session_handle, *app_id;
		GVariant *opts;

		g_variant_get(params, "(&o&o&s@a{sv})", &handle,
			&session_handle, &app_id, &opts);
		g_variant_unref(opts);

		/* Export the Session object at session_handle (see the
		 * InputCapture CreateSession above). */
		if (!portal_dbus_begin_session(self, session_handle)) {
			g_variant_builder_init(&results,
				G_VARIANT_TYPE("a{sv}"));
			g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(ua{sv})", PORTAL_RESPONSE_OTHER,
					&results));
			return;
		}

		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "SelectDevices") == 0) {
		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&results, "{sv}", "devices",
			g_variant_new_uint32(PORTAL_DEVICE_TYPES));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "Start") == 0) {
		g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&results, "{sv}", "devices",
			g_variant_new_uint32(PORTAL_DEVICE_TYPES));
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(ua{sv})", PORTAL_RESPONSE_SUCCESS,
				&results));
		return;
	}

	if (g_strcmp0(method, "NotifyPointerMotion") == 0) {
		const gchar *sh;
		GVariant *opts;
		double dx, dy;

		g_variant_get(params, "(&o@a{sv}dd)", &sh, &opts, &dx, &dy);
		g_variant_unref(opts);
		portal_wayland_inject_rel_motion(self->wl, dx, dy);
		portal_wayland_inject_frame(self->wl);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "NotifyPointerMotionAbsolute") == 0) {
		const gchar *sh;
		GVariant *opts;
		guint32 stream;
		double x, y;

		g_variant_get(params, "(&o@a{sv}udd)", &sh, &opts, &stream,
			&x, &y);
		g_variant_unref(opts);
		/* x/y are in stream coordinates; the compositor maps the
		 * normalized form.  Pass through as normalized for now. */
		portal_wayland_inject_abs_motion(self->wl, x, y);
		portal_wayland_inject_frame(self->wl);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "NotifyPointerButton") == 0) {
		const gchar *sh;
		GVariant *opts;
		gint32 button;
		guint32 state;

		g_variant_get(params, "(&o@a{sv}iu)", &sh, &opts, &button,
			&state);
		g_variant_unref(opts);
		portal_wayland_inject_button(self->wl, (guint32)button,
			state != 0);
		portal_wayland_inject_frame(self->wl);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "NotifyPointerAxisDiscrete") == 0) {
		const gchar *sh;
		GVariant *opts;
		guint32 axis;
		gint32 steps;

		g_variant_get(params, "(&o@a{sv}ui)", &sh, &opts, &axis,
			&steps);
		g_variant_unref(opts);
		portal_wayland_inject_axis(self->wl, axis, (double)steps);
		portal_wayland_inject_frame(self->wl);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "NotifyKeyboardKeycode") == 0) {
		const gchar *sh;
		GVariant *opts;
		gint32 keycode;
		guint32 state;

		g_variant_get(params, "(&o@a{sv}iu)", &sh, &opts, &keycode,
			&state);
		g_variant_unref(opts);
		portal_wayland_inject_key(self->wl, (guint32)keycode,
			state != 0);
		portal_wayland_inject_frame(self->wl);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (g_strcmp0(method, "ConnectToEIS") == 0) {
		const gchar *session_handle, *app_id;
		GVariant *opts;

		/* Fix: CWE-862 — same EIS fd session guard as the InputCapture
		 * path.  The RemoteDesktop ConnectToEIS fd grants pointer+keyboard
		 * injection via the inject Wayland object; require a session. */
		g_variant_get(params, "(&o&s@a{sv})", &session_handle,
			&app_id, &opts);
		g_variant_unref(opts);
		if (reject_if_no_session(self, invocation, session_handle))
			return;

		return_eis_fd(self, invocation);
		return;
	}

	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method);
}

/* ---------------------------------------------------------------
 * Properties
 * --------------------------------------------------------------- */

static GVariant *
ic_get_property(GDBusConnection *conn, const gchar *sender,
                const gchar *path, const gchar *iface, const gchar *prop,
                GError **error, gpointer user_data)
{
	(void)conn; (void)sender; (void)path; (void)iface;
	(void)error; (void)user_data;

	if (g_strcmp0(prop, "version") == 0)
		return g_variant_new_uint32(2);
	if (g_strcmp0(prop, "SupportedCapabilities") == 0)
		return g_variant_new_uint32(PORTAL_CAPS);
	return NULL;
}

static GVariant *
rd_get_property(GDBusConnection *conn, const gchar *sender,
                const gchar *path, const gchar *iface, const gchar *prop,
                GError **error, gpointer user_data)
{
	(void)conn; (void)sender; (void)path; (void)iface;
	(void)error; (void)user_data;

	if (g_strcmp0(prop, "version") == 0)
		return g_variant_new_uint32(2);
	if (g_strcmp0(prop, "AvailableDeviceTypes") == 0)
		return g_variant_new_uint32(PORTAL_DEVICE_TYPES);
	return NULL;
}

static const GDBusInterfaceVTable ic_vtable = {
	ic_method, ic_get_property, NULL, { 0 }
};

static const GDBusInterfaceVTable rd_vtable = {
	rd_method, rd_get_property, NULL, { 0 }
};

/* ---------------------------------------------------------------
 * Bus name lifecycle
 * --------------------------------------------------------------- */

static void
on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	PortalDbus *self = user_data;
	GError *error = NULL;

	(void)name;
	self->conn = conn;

	self->ic_reg_id = g_dbus_connection_register_object(conn,
		PORTAL_OBJ_PATH, node_info->interfaces[0], &ic_vtable,
		self, NULL, &error);
	if (self->ic_reg_id == 0) {
		g_warning("portal: register InputCapture failed: %s",
			error ? error->message : "?");
		g_clear_error(&error);
	}

	self->rd_reg_id = g_dbus_connection_register_object(conn,
		PORTAL_OBJ_PATH, node_info->interfaces[1], &rd_vtable,
		self, NULL, &error);
	if (self->rd_reg_id == 0) {
		g_warning("portal: register RemoteDesktop failed: %s",
			error ? error->message : "?");
		g_clear_error(&error);
	}
}

static void
on_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	(void)conn; (void)user_data;
	g_message("portal: acquired %s", name);
}

static void
on_name_lost(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	(void)conn; (void)user_data;
	g_warning("portal: lost bus name %s", name);
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

PortalDbus *
portal_dbus_new(PortalWayland *wl, PortalEis *eis)
{
	PortalDbus *self;
	GError *error = NULL;

	if (node_info == NULL) {
		node_info = g_dbus_node_info_new_for_xml(introspection_xml,
			&error);
		if (node_info == NULL) {
			g_warning("portal: bad introspection XML: %s",
				error ? error->message : "?");
			g_clear_error(&error);
			return NULL;
		}
	}

	if (session_node_info == NULL) {
		session_node_info = g_dbus_node_info_new_for_xml(
			session_introspection_xml, &error);
		if (session_node_info == NULL) {
			g_warning("portal: bad Session introspection XML: %s",
				error ? error->message : "?");
			g_clear_error(&error);
			return NULL;
		}
	}

	self = g_new0(PortalDbus, 1);
	self->wl = wl;
	self->eis = eis;

	portal_wayland_set_activation_callback(wl, on_activation, self);
	portal_wayland_set_zones_changed_callback(wl, on_zones_changed, self);

	self->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, PORTAL_BUS_NAME,
		G_BUS_NAME_OWNER_FLAGS_REPLACE,
		on_bus_acquired, on_name_acquired, on_name_lost, self, NULL);

	return self;
}

void
portal_dbus_free(PortalDbus *self)
{
	if (self == NULL)
		return;

	if (self->conn != NULL) {
		portal_dbus_end_session(self, FALSE);
		if (self->ic_reg_id)
			g_dbus_connection_unregister_object(self->conn,
				self->ic_reg_id);
		if (self->rd_reg_id)
			g_dbus_connection_unregister_object(self->conn,
				self->rd_reg_id);
	}
	if (self->owner_id)
		g_bus_unown_name(self->owner_id);
	g_free(self->session_handle);
	g_free(self);
}
