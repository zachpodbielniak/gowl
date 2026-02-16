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
 * gowl-mcp-dispatch.h - Thread-safe compositor dispatch for MCP tools.
 *
 * MCP tool handlers run on the MCP thread but need to read/write
 * compositor state which lives on the Wayland event loop thread.
 * This module provides a request queue + eventfd mechanism to
 * safely dispatch tool calls to the compositor thread.
 */

#ifndef GOWL_MCP_DISPATCH_H
#define GOWL_MCP_DISPATCH_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "mcp.h"

G_BEGIN_DECLS

/* forward declaration */
typedef struct _GowlModuleMcp GowlModuleMcp;

/**
 * GowlMcpToolFunc:
 * @module: the MCP module instance
 * @arguments: (nullable): JSON arguments from the tool call
 * @user_data: (nullable): per-tool user data
 *
 * Function signature for tool implementations that run on the
 * compositor thread.  The function has full access to compositor
 * state and must return a result.
 *
 * Returns: (transfer full): the tool result
 */
typedef McpToolResult *(*GowlMcpToolFunc)(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
);

/**
 * GowlMcpRequest:
 *
 * A pending tool dispatch request.  Created on the MCP thread,
 * enqueued to the compositor thread, and completed with a signal.
 */
typedef struct _GowlMcpRequest GowlMcpRequest;

struct _GowlMcpRequest {
	/* synchronisation */
	GMutex         mutex;
	GCond          cond;
	gboolean       done;

	/* input (set by MCP thread) */
	GowlMcpToolFunc  func;
	JsonObject      *arguments;   /* borrowed, valid until done */
	gpointer         user_data;

	/* output (set by compositor thread) */
	McpToolResult   *result;
};

/**
 * gowl_mcp_dispatch_init:
 * @module: the MCP module
 *
 * Initialises the dispatch queue and creates the eventfd.
 * Must be called before any dispatch calls (typically in startup).
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gowl_mcp_dispatch_init(GowlModuleMcp *module);

/**
 * gowl_mcp_dispatch_shutdown:
 * @module: the MCP module
 *
 * Shuts down the dispatch queue and closes the eventfd.
 * Any pending requests are drained and completed with an error.
 */
void
gowl_mcp_dispatch_shutdown(GowlModuleMcp *module);

/**
 * gowl_mcp_dispatch_call:
 * @module: the MCP module
 * @func: the tool function to execute on the compositor thread
 * @arguments: (nullable): JSON arguments from the tool call
 * @user_data: (nullable): per-tool user data
 *
 * Dispatches a tool call to the compositor thread and blocks
 * until it completes.  This function is called from the MCP
 * thread.
 *
 * Returns: (transfer full): the tool result
 */
McpToolResult *
gowl_mcp_dispatch_call(
	GowlModuleMcp   *module,
	GowlMcpToolFunc  func,
	JsonObject       *arguments,
	gpointer          user_data
);

G_END_DECLS

#endif /* GOWL_MCP_DISPATCH_H */
