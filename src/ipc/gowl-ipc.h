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
 * @error: (nullable): return location for a #GError
 *
 * Starts listening for IPC connections.
 *
 * Returns: TRUE on success
 */
gboolean
gowl_ipc_start(GowlIpc *self, GError **error);

/**
 * gowl_ipc_stop:
 * @self: the IPC server
 *
 * Stops the IPC server and cleans up the socket.
 */
void
gowl_ipc_stop(GowlIpc *self);

G_END_DECLS

#endif /* GOWL_IPC_H */
