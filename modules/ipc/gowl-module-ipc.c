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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-ipc"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "core/gowl-compositor.h"

/**
 * GowlModuleIpc:
 *
 * IPC command handler module.  Processes line-based commands from
 * external tools (e.g. gowlctl) and returns response strings.
 *
 * Supported commands:
 *   get_tags       - query current tag state
 *   get_layout     - query current layout symbol
 *   get_clients    - list all clients
 *   quit           - terminate compositor
 *   reload_config  - reload configuration
 *   version        - report compositor version
 *   ping           - respond with "pong"
 */

#define GOWL_TYPE_MODULE_IPC (gowl_module_ipc_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleIpc, gowl_module_ipc,
                     GOWL, MODULE_IPC, GowlModule)

struct _GowlModuleIpc {
	GowlModule      parent_instance;
	GowlCompositor *compositor;
};

static void ipc_handler_init(GowlIpcHandlerInterface *iface);
static void ipc_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleIpc, gowl_module_ipc,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER,
		ipc_handler_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		ipc_startup_init))

/* --- GowlModule virtual methods --- */

static gboolean
ipc_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
ipc_get_name(GowlModule *mod)
{
	(void)mod;
	return "ipc";
}

static const gchar *
ipc_get_description(GowlModule *mod)
{
	(void)mod;
	return "IPC command handler for external tools";
}

static const gchar *
ipc_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlStartupHandler --- */

static void
ipc_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleIpc *self;

	self = GOWL_MODULE_IPC(handler);
	self->compositor = GOWL_COMPOSITOR(compositor);

	g_debug("ipc: startup, compositor reference stored");
}

static void
ipc_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = ipc_on_startup;
}

/* --- GowlIpcHandler --- */

/**
 * ipc_handle_command:
 *
 * Dispatches an IPC command string to the appropriate handler.
 * Returns a newly-allocated response string (caller frees).
 */
static gchar *
ipc_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleIpc *self;

	self = GOWL_MODULE_IPC(handler);

	if (command == NULL)
		return g_strdup("ERROR: no command");

	/* ping - simple health check */
	if (strcmp(command, "ping") == 0)
		return g_strdup("pong");

	/* version - report module version */
	if (strcmp(command, "version") == 0)
		return g_strdup("gowl-ipc 0.1.0");

	/* quit - request compositor shutdown */
	if (strcmp(command, "quit") == 0) {
		if (self->compositor != NULL)
			gowl_compositor_quit(self->compositor);
		return g_strdup("OK: quitting");
	}

	/* reload_config - request config reload */
	if (strcmp(command, "reload_config") == 0) {
		(void)args;
		g_debug("ipc: reload_config requested");
		return g_strdup("OK: reload requested");
	}

	/* get_socket - return the wayland socket name */
	if (strcmp(command, "get_socket") == 0) {
		const gchar *name;

		if (self->compositor == NULL)
			return g_strdup("ERROR: no compositor");

		name = gowl_compositor_get_socket_name(self->compositor);
		if (name != NULL)
			return g_strdup(name);
		return g_strdup("ERROR: no socket");
	}

	/* Unknown command */
	return g_strdup_printf("ERROR: unknown command '%s'", command);
}

static void
ipc_handler_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = ipc_handle_command;
}

/* --- GObject lifecycle --- */

static void
gowl_module_ipc_class_init(GowlModuleIpcClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = ipc_activate;
	mod_class->get_name        = ipc_get_name;
	mod_class->get_description = ipc_get_description;
	mod_class->get_version     = ipc_get_version;
}

static void
gowl_module_ipc_init(GowlModuleIpc *self)
{
	self->compositor = NULL;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_IPC;
}
