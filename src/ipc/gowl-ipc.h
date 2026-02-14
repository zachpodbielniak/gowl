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

#ifndef GOWL_IPC_H
#define GOWL_IPC_H

#include <glib-object.h>
#include <wayland-server-core.h>

G_BEGIN_DECLS

#define GOWL_TYPE_IPC (gowl_ipc_get_type())

G_DECLARE_FINAL_TYPE(GowlIpc, gowl_ipc, GOWL, IPC, GObject)

/**
 * gowl_ipc_new:
 * @socket_path: (nullable): path for the UNIX socket, or NULL for default
 *
 * Creates a new IPC server.
 *
 * Returns: (transfer full): a new #GowlIpc
 */
GowlIpc *
gowl_ipc_new(const gchar *socket_path);

/**
 * gowl_ipc_start:
 * @self: the IPC server
 * @event_loop: the Wayland event loop to integrate with
 * @error: (nullable): return location for a #GError
 *
 * Starts listening for IPC connections.  Uses @event_loop for
 * I/O dispatch so the IPC server runs within the compositor's
 * Wayland event loop.
 *
 * Returns: TRUE on success
 */
gboolean
gowl_ipc_start(
	GowlIpc              *self,
	struct wl_event_loop *event_loop,
	GError              **error
);

/**
 * gowl_ipc_stop:
 * @self: the IPC server
 *
 * Stops the IPC server and cleans up the socket.
 */
void
gowl_ipc_stop(GowlIpc *self);

/**
 * gowl_ipc_push_event:
 * @self: the IPC server
 * @format: printf-style format string for the event line
 * @...: arguments for the format string
 *
 * Broadcasts an event line to all subscribed clients.
 * Clients subscribe by sending the "subscribe" command.
 * The event is sent as a single newline-terminated line.
 */
void
gowl_ipc_push_event(GowlIpc *self, const gchar *format, ...)
	G_GNUC_PRINTF(2, 3);

/**
 * gowl_ipc_get_subscriber_count:
 * @self: the IPC server
 *
 * Returns the number of currently subscribed clients.
 *
 * Returns: the subscriber count
 */
guint
gowl_ipc_get_subscriber_count(GowlIpc *self);

G_END_DECLS

#endif /* GOWL_IPC_H */
