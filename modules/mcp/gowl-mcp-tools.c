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
 * gowl-mcp-tools.c - Master tool registration.
 *
 * Calls all per-category registration functions.  Individual tool
 * files will be added as they are implemented.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-tools.h"

void
gowl_mcp_register_all_tools(McpServer *server, GowlModuleMcp *module)
{
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/*
	 * Per-category registrations will be enabled as each tool
	 * file is implemented.  Each function internally checks
	 * gowl_module_mcp_is_tool_allowed() before registering.
	 */

	gowl_mcp_register_query_tools(server, module);
	gowl_mcp_register_window_tools(server, module);
	gowl_mcp_register_tag_tools(server, module);
	gowl_mcp_register_layout_tools(server, module);
	gowl_mcp_register_config_tools(server, module);
	gowl_mcp_register_process_tools(server, module);
	gowl_mcp_register_compound_tools(server, module);
	gowl_mcp_register_input_tools(server, module);
	gowl_mcp_register_screenshot_tools(server, module);

	/* TODO: enable as tool files are implemented
	gowl_mcp_register_clipboard_tools(server, module);
	gowl_mcp_register_resources(server, module);
	*/

	g_debug("gowl-mcp: tool registration complete");
}
