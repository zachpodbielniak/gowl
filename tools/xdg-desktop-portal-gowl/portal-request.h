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

#ifndef PORTAL_REQUEST_H
#define PORTAL_REQUEST_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * PortalRequest -- org.freedesktop.impl.portal.Request, for one call
 * that waits on a human.
 *
 * A backend method that puts up UI exports a Request object on the
 * `handle' path it was called with, for as long as the UI is up.  The
 * frontend calls Close() on it when the application cancels -- or
 * vanishes, which is the frontend's to notice -- and the UI has to go.
 * Here Close() cancels a GCancellable the pick is running under, so
 * whatever is waiting (the menu card, a dmenu, slurp) is withdrawn and
 * the call is answered as cancelled.
 */
typedef struct _PortalRequest PortalRequest;

/**
 * portal_request_new:
 * @conn: the bus connection the call came in on
 * @handle: the request's object path
 *
 * Exports the Request object.  An export that fails (the path taken)
 * is logged and leaves a request whose Close() simply never comes: the
 * pick still runs, it just cannot be withdrawn from the frontend.
 *
 * Returns: (transfer full): the request
 */
PortalRequest *portal_request_new (GDBusConnection *conn,
                                   const gchar     *handle);

/**
 * portal_request_get_cancellable:
 * @self: a #PortalRequest
 *
 * Returns: (transfer none): cancelled when the frontend closes the
 *          request
 */
GCancellable *portal_request_get_cancellable (PortalRequest *self);

/**
 * portal_request_free:
 * @self: (nullable): a #PortalRequest
 *
 * Unexports the object.  Call it when the method has been answered.
 */
void portal_request_free (PortalRequest *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalRequest, portal_request_free)

G_END_DECLS

#endif /* PORTAL_REQUEST_H */
