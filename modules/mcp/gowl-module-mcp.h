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
 * gowl-module-mcp.h - MCP (Model Context Protocol) server module.
 *
 * Exposes compositor state and controls to AI assistants via the
 * MCP protocol.  Supports HTTP+SSE and Unix socket (stdio relay)
 * transports.  All tools are configurable via YAML allowlist.
 */

#ifndef GOWL_MODULE_MCP_H
#define GOWL_MODULE_MCP_H

#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "module/gowl-module.h"
#include "core/gowl-compositor.h"

/* mcp-glib */
#include "mcp.h"

G_BEGIN_DECLS

#define GOWL_TYPE_MODULE_MCP (gowl_module_mcp_get_type())

G_DECLARE_FINAL_TYPE(GowlModuleMcp, gowl_module_mcp,
                     GOWL, MODULE_MCP, GowlModule)

/**
 * GowlMcpSession:
 *
 * Tracks a single connected MCP client over the Unix socket
 * transport.  Each accepted connection gets its own McpServer
 * instance and McpStdioTransport wrapping the socket streams.
 */
typedef struct _GowlMcpSession GowlMcpSession;

struct _GowlMcpSession {
	McpServer              *server;
	McpStdioTransport      *transport;
	GSocketConnection      *connection;
	GowlModuleMcp          *module;   /* back-reference (unowned) */
};

/**
 * struct _GowlModuleMcp:
 *
 * MCP server module state.  Holds configuration, thread state,
 * server instances, and the compositor dispatch queue.
 */
struct _GowlModuleMcp {
	GowlModule       parent_instance;

	/* compositor reference (set on startup) */
	GowlCompositor  *compositor;

	/* ---- MCP thread ---- */
	GThread          *mcp_thread;
	GMainLoop        *mcp_loop;
	GMainContext     *mcp_context;

	/* HTTP+SSE server (nullable if transport-http is disabled) */
	McpServer                *http_server;
	McpHttpServerTransport   *http_transport;

	/* Unix socket server for stdio relay */
	GSocketService   *socket_service;
	gchar            *socket_path;
	GList            *socket_sessions;  /* GList of GowlMcpSession* */

	/* ---- Thread-safe compositor dispatch ---- */
	GMutex            queue_mutex;
	GQueue            pending_requests;
	gint              wake_fd;           /* eventfd(2) */
	struct wl_event_source *wake_source; /* wl_event_loop fd source */

	/* ---- Configuration ---- */
	gboolean          transport_http;
	gboolean          transport_stdio;
	gchar            *http_host;
	gint              http_port;
	gchar            *http_auth_token;
	gchar            *instructions;
	GHashTable       *allowed_tools;  /* set of tool names, NULL = all */
};

/**
 * gowl_module_mcp_is_tool_allowed:
 * @self: a #GowlModuleMcp
 * @tool_name: the tool name to check
 *
 * Returns whether @tool_name is in the module's allowlist.
 * If no allowlist is configured (all tools enabled), returns %TRUE.
 *
 * Returns: %TRUE if the tool is allowed
 */
gboolean
gowl_module_mcp_is_tool_allowed(
	GowlModuleMcp *self,
	const gchar   *tool_name
);

/**
 * gowl_module_mcp_setup_server:
 * @self: a #GowlModuleMcp
 * @server: a newly created #McpServer
 *
 * Configures an McpServer with instructions and registers all
 * allowed tools on it.  Used for both HTTP and socket servers.
 */
void
gowl_module_mcp_setup_server(
	GowlModuleMcp *self,
	McpServer     *server
);

G_END_DECLS

#endif /* GOWL_MODULE_MCP_H */
