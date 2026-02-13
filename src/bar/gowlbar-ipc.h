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

#ifndef GOWLBAR_IPC_H
#define GOWLBAR_IPC_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_IPC (gowlbar_ipc_get_type())

G_DECLARE_FINAL_TYPE(GowlbarIpc, gowlbar_ipc, GOWLBAR, IPC, GObject)

/**
 * gowlbar_ipc_new:
 * @socket_path: (nullable): path for the Unix socket, or %NULL for
 *   the default ($XDG_RUNTIME_DIR/gowl.sock)
 *
 * Creates a new IPC client that connects to the gowl compositor
 * for receiving state events and sending commands.
 *
 * Returns: (transfer full): a new #GowlbarIpc
 */
GowlbarIpc *gowlbar_ipc_new(const gchar *socket_path);

/**
 * gowlbar_ipc_connect:
 * @self: the IPC client
 *
 * Initiates an asynchronous connection to the compositor socket.
 * On success the "connected" signal is emitted.  On failure or
 * disconnect, auto-reconnect with backoff is attempted.
 */
void gowlbar_ipc_connect(GowlbarIpc *self);

/**
 * gowlbar_ipc_disconnect:
 * @self: the IPC client
 *
 * Closes the IPC connection and stops reconnection attempts.
 */
void gowlbar_ipc_disconnect(GowlbarIpc *self);

/**
 * gowlbar_ipc_send_command:
 * @self: the IPC client
 * @command: the command line to send (newline appended automatically)
 *
 * Sends a command string to the compositor.
 */
void gowlbar_ipc_send_command(GowlbarIpc *self, const gchar *command);

/**
 * gowlbar_ipc_is_connected:
 * @self: the IPC client
 *
 * Returns: %TRUE if currently connected to the compositor
 */
gboolean gowlbar_ipc_is_connected(GowlbarIpc *self);

G_END_DECLS

#endif /* GOWLBAR_IPC_H */
