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
 * portal-request.c - the Request object a pending pick is closed by.
 */

#include "portal-request.h"

struct _PortalRequest {
	GDBusConnection *conn;
	gchar           *handle;
	guint            reg_id;
	GCancellable    *cancellable;
};

static GDBusNodeInfo *request_node_info;

static const gchar request_xml[] =
"<node>"
"  <interface name='org.freedesktop.impl.portal.Request'>"
"    <method name='Close'/>"
"  </interface>"
"</node>";

static void
request_method(GDBusConnection *conn, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *method, GVariant *params,
               GDBusMethodInvocation *invocation, gpointer user_data)
{
	PortalRequest *self = user_data;

	(void)conn; (void)sender; (void)path; (void)iface; (void)params;

	if (g_strcmp0(method, "Close") == 0) {
		g_debug("request: %s closed by the frontend", self->handle);
		/* Answered first: cancelling runs the pick's completion, which
		 * frees this request and unregisters the object. */
		g_dbus_method_invocation_return_value(invocation, NULL);
		g_cancellable_cancel(self->cancellable);
		return;
	}
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_UNKNOWN_METHOD, "no such method: %s", method);
}

static const GDBusInterfaceVTable request_vtable = {
	request_method, NULL, NULL, { 0 }
};

PortalRequest *
portal_request_new(GDBusConnection *conn, const gchar *handle)
{
	PortalRequest *self;
	g_autoptr(GError) error = NULL;

	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(handle != NULL, NULL);

	if (request_node_info == NULL)
		request_node_info = g_dbus_node_info_new_for_xml(request_xml, NULL);

	self = g_new0(PortalRequest, 1);
	self->conn = g_object_ref(conn);
	self->handle = g_strdup(handle);
	self->cancellable = g_cancellable_new();
	self->reg_id = g_dbus_connection_register_object(conn, handle,
		request_node_info->interfaces[0], &request_vtable, self, NULL,
		&error);
	if (self->reg_id == 0)
		g_warning("request: cannot export %s: %s", handle, error->message);
	return self;
}

GCancellable *
portal_request_get_cancellable(PortalRequest *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->cancellable;
}

void
portal_request_free(PortalRequest *self)
{
	if (self == NULL)
		return;
	if (self->reg_id != 0 && !g_dbus_connection_is_closed(self->conn))
		g_dbus_connection_unregister_object(self->conn, self->reg_id);
	g_clear_object(&self->cancellable);
	g_clear_object(&self->conn);
	g_free(self->handle);
	g_free(self);
}
