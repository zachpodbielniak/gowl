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
 * gowl-mcp-tools.h - Tool registration declarations for each category.
 *
 * Each tool file exports a registration function that adds its
 * tools to the given McpServer, filtered by the module's allowlist.
 */

#ifndef GOWL_MCP_TOOLS_H
#define GOWL_MCP_TOOLS_H

#include "mcp.h"

G_BEGIN_DECLS

/* forward declaration */
typedef struct _GowlModuleMcp GowlModuleMcp;

/**
 * gowl_mcp_register_all_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Master registration function: calls all per-category registration
 * functions, filtering by the module's tool allowlist.
 */
void gowl_mcp_register_all_tools(McpServer *server, GowlModuleMcp *module);

/* Per-category registration functions */
void gowl_mcp_register_query_tools   (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_window_tools  (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_tag_tools     (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_layout_tools  (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_config_tools  (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_input_tools   (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_screenshot_tools(McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_clipboard_tools(McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_process_tools (McpServer *server, GowlModuleMcp *module);
void gowl_mcp_register_compound_tools(McpServer *server, GowlModuleMcp *module);

/* Resource registration */
void gowl_mcp_register_resources     (McpServer *server, GowlModuleMcp *module);

G_END_DECLS

#endif /* GOWL_MCP_TOOLS_H */
