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

#include "gowl-ipc-handler.h"

G_DEFINE_INTERFACE(GowlIpcHandler, gowl_ipc_handler, G_TYPE_OBJECT)

static void
gowl_ipc_handler_default_init(GowlIpcHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_ipc_handler_handle_command:
 * @self: a #GowlIpcHandler
 * @command: the IPC command name
 * @args: (nullable): arguments for the command
 *
 * Handles an IPC command with the given arguments. Returns a
 * newly-allocated response string.
 *
 * Returns: (transfer full) (nullable): the command response, or %NULL
 */
gchar *
gowl_ipc_handler_handle_command(
	GowlIpcHandler *self,
	const gchar    *command,
	const gchar    *args
){
	GowlIpcHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_IPC_HANDLER(self), NULL);

	iface = GOWL_IPC_HANDLER_GET_IFACE(self);
	if (iface->handle_command != NULL)
		return iface->handle_command(self, command, args);
	return NULL;
}
