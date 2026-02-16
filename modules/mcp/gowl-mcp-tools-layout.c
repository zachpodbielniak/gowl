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
 * gowl-mcp-tools-layout.c - Layout management MCP tools.
 *
 * Implements the following MCP tools:
 *   - set_mfact   : Set master area ratio
 *   - set_nmaster : Set master window count
 *
 * Note: set_layout and set_gaps are deferred to a later step as they
 * require deeper integration with the layout provider system.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-core-private.h"

#include <json-glib/json-glib.h>

/* ========================================================================== */
/* Helper: resolve monitor (same pattern as tag tools)                        */
/* ========================================================================== */

static GowlMonitor *
resolve_monitor(
	GowlModuleMcp *module,
	JsonObject    *arguments
){
	GList *monitors;
	GList *iter;
	const gchar *name;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "monitor"))
	{
		monitors = gowl_compositor_get_monitors(module->compositor);
		return monitors ? GOWL_MONITOR(monitors->data) : NULL;
	}

	name = json_object_get_string_member(arguments, "monitor");
	if (name == NULL)
		return NULL;

	monitors = gowl_compositor_get_monitors(module->compositor);
	for (iter = monitors; iter != NULL; iter = iter->next) {
		GowlMonitor *mon;
		const gchar *mon_name;

		mon = GOWL_MONITOR(iter->data);
		mon_name = gowl_monitor_get_name(mon);
		if (mon_name != NULL && g_strcmp0(mon_name, name) == 0)
			return mon;
	}

	return NULL;
}

/* ========================================================================== */
/* Tool: set_mfact                                                            */
/* ========================================================================== */

/**
 * tool_set_mfact:
 *
 * Sets the master area ratio on a monitor (0.05 to 0.95).
 */
static McpToolResult *
tool_set_mfact(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlMonitor *mon;
	gdouble mfact;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "mfact"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameter: mfact");
		return r;
	}

	mon = resolve_monitor(module, arguments);
	if (mon == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Monitor not found");
		return r;
	}

	mfact = json_object_get_double_member(arguments, "mfact");
	if (mfact < 0.05 || mfact > 0.95) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"mfact must be between 0.05 and 0.95");
		return r;
	}

	gowl_monitor_set_mfact(mon, mfact);
	gowl_compositor_arrange(module->compositor, mon);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_set_mfact(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_set_mfact, arguments, NULL);
}

/* ========================================================================== */
/* Tool: set_nmaster                                                          */
/* ========================================================================== */

/**
 * tool_set_nmaster:
 *
 * Sets the number of windows in the master area.
 */
static McpToolResult *
tool_set_nmaster(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlMonitor *mon;
	gint nmaster;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "nmaster"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameter: nmaster");
		return r;
	}

	mon = resolve_monitor(module, arguments);
	if (mon == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Monitor not found");
		return r;
	}

	nmaster = (gint)json_object_get_int_member(arguments, "nmaster");
	if (nmaster < 0) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"nmaster must be non-negative");
		return r;
	}

	gowl_monitor_set_nmaster(mon, nmaster);
	gowl_compositor_arrange(module->compositor, mon);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_set_nmaster(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_set_nmaster, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_layout_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* set_mfact */
	if (gowl_module_mcp_is_tool_allowed(module, "set_mfact")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("set_mfact",
			"Set the master area ratio on a monitor "
			"(0.05 to 0.95, default 0.55).");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "mfact");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "number");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Master area ratio (0.05-0.95)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "monitor");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Monitor name. Defaults to focused monitor.");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "mfact");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_set_mfact,
		                    module, NULL);
	}

	/* set_nmaster */
	if (gowl_module_mcp_is_tool_allowed(module, "set_nmaster")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("set_nmaster",
			"Set the number of windows in the master area.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "nmaster");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Number of windows in master area (>= 0)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "monitor");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Monitor name. Defaults to focused monitor.");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "nmaster");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_set_nmaster,
		                    module, NULL);
	}

	g_debug("layout tools registered");
}
