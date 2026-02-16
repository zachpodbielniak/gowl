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
 * gowl-mcp-tools-compound.c - Compound / aggregate MCP tools.
 *
 * Implements:
 *   - describe_desktop : Full desktop state as a single JSON response
 *   - find_window      : Search for windows by app_id/title pattern
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"

#include <json-glib/json-glib.h>

/* ========================================================================== */
/* Helper: JSON serialisation (duplicated from query tools for encapsulation) */
/* ========================================================================== */

static void
build_client_json_compact(
	JsonBuilder *builder,
	GowlClient  *client
){
	gint x, y, w, h;

	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, gowl_client_get_id(client));

	json_builder_set_member_name(builder, "app_id");
	json_builder_add_string_value(builder,
		gowl_client_get_app_id(client) ?
			gowl_client_get_app_id(client) : "");

	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder,
		gowl_client_get_title(client) ?
			gowl_client_get_title(client) : "");

	json_builder_set_member_name(builder, "tags");
	json_builder_add_int_value(builder, gowl_client_get_tags(client));

	json_builder_set_member_name(builder, "floating");
	json_builder_add_boolean_value(builder,
		gowl_client_get_floating(client));

	json_builder_set_member_name(builder, "fullscreen");
	json_builder_add_boolean_value(builder,
		gowl_client_get_fullscreen(client));

	gowl_client_get_geometry(client, &x, &y, &w, &h);
	json_builder_set_member_name(builder, "geometry");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "x");
	json_builder_add_int_value(builder, x);
	json_builder_set_member_name(builder, "y");
	json_builder_add_int_value(builder, y);
	json_builder_set_member_name(builder, "width");
	json_builder_add_int_value(builder, w);
	json_builder_set_member_name(builder, "height");
	json_builder_add_int_value(builder, h);
	json_builder_end_object(builder);

	json_builder_end_object(builder);
}

static gchar *
builder_to_string(JsonBuilder *builder)
{
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(JsonGenerator) gen = NULL;

	root = json_builder_get_root(builder);
	gen = json_generator_new();
	json_generator_set_pretty(gen, TRUE);
	json_generator_set_root(gen, root);

	return json_generator_to_data(gen, NULL);
}

/* ========================================================================== */
/* Tool: describe_desktop                                                     */
/* ========================================================================== */

/**
 * tool_describe_desktop:
 *
 * Returns a comprehensive snapshot of the entire desktop state:
 * monitors, their tags, and all clients.  Useful for LLMs to
 * understand the full context in one call.
 */
static McpToolResult *
tool_describe_desktop(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlClient *focused;
	GList *monitors;
	GList *clients;
	GList *iter;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	monitors = gowl_compositor_get_monitors(module->compositor);
	clients = gowl_compositor_get_clients(module->compositor);
	focused = gowl_compositor_get_focused_client(module->compositor);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	/* focused client ID (or -1) */
	json_builder_set_member_name(builder, "focused_client_id");
	json_builder_add_int_value(builder,
		focused ? (gint64)gowl_client_get_id(focused) : -1);

	/* total counts */
	json_builder_set_member_name(builder, "client_count");
	json_builder_add_int_value(builder,
		gowl_compositor_get_client_count(module->compositor));

	json_builder_set_member_name(builder, "monitor_count");
	json_builder_add_int_value(builder,
		gowl_compositor_get_monitor_count(module->compositor));

	/* monitors */
	json_builder_set_member_name(builder, "monitors");
	json_builder_begin_array(builder);
	for (iter = monitors; iter != NULL; iter = iter->next) {
		GowlMonitor *mon;
		gint x, y, w, h;

		mon = GOWL_MONITOR(iter->data);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder,
			gowl_monitor_get_name(mon) ?
				gowl_monitor_get_name(mon) : "");

		gowl_monitor_get_geometry(mon, &x, &y, &w, &h);
		json_builder_set_member_name(builder, "geometry");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "x");
		json_builder_add_int_value(builder, x);
		json_builder_set_member_name(builder, "y");
		json_builder_add_int_value(builder, y);
		json_builder_set_member_name(builder, "width");
		json_builder_add_int_value(builder, w);
		json_builder_set_member_name(builder, "height");
		json_builder_add_int_value(builder, h);
		json_builder_end_object(builder);

		json_builder_set_member_name(builder, "active_tags");
		json_builder_add_int_value(builder,
			gowl_monitor_get_tags(mon));

		json_builder_set_member_name(builder, "layout");
		json_builder_add_string_value(builder,
			gowl_monitor_get_layout_symbol(mon) ?
				gowl_monitor_get_layout_symbol(mon) : "");

		json_builder_set_member_name(builder, "mfact");
		json_builder_add_double_value(builder,
			gowl_monitor_get_mfact(mon));

		json_builder_set_member_name(builder, "nmaster");
		json_builder_add_int_value(builder,
			gowl_monitor_get_nmaster(mon));

		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	/* clients */
	json_builder_set_member_name(builder, "clients");
	json_builder_begin_array(builder);
	for (iter = clients; iter != NULL; iter = iter->next) {
		build_client_json_compact(builder,
			GOWL_CLIENT(iter->data));
	}
	json_builder_end_array(builder);

	json_builder_end_object(builder);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_describe_desktop(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_describe_desktop, arguments, NULL);
}

/* ========================================================================== */
/* Tool: find_window                                                          */
/* ========================================================================== */

/**
 * tool_find_window:
 *
 * Searches for windows matching an app_id or title pattern.
 * Returns all matches (not just the first).
 */
static McpToolResult *
tool_find_window(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	const gchar *app_id_pattern;
	const gchar *title_pattern;
	GList *clients;
	GList *iter;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	app_id_pattern = NULL;
	title_pattern = NULL;

	if (arguments != NULL) {
		if (json_object_has_member(arguments, "app_id"))
			app_id_pattern = json_object_get_string_member(
				arguments, "app_id");
		if (json_object_has_member(arguments, "title"))
			title_pattern = json_object_get_string_member(
				arguments, "title");
	}

	if (app_id_pattern == NULL && title_pattern == NULL) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"At least one of app_id or title pattern is required");
		return result;
	}

	clients = gowl_compositor_get_clients(module->compositor);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (iter = clients; iter != NULL; iter = iter->next) {
		GowlClient *c;
		gboolean matches;
		const gchar *c_app_id;
		const gchar *c_title;

		c = GOWL_CLIENT(iter->data);
		matches = FALSE;
		c_app_id = gowl_client_get_app_id(c);
		c_title = gowl_client_get_title(c);

		if (app_id_pattern != NULL && c_app_id != NULL) {
			if (g_pattern_match_simple(app_id_pattern, c_app_id))
				matches = TRUE;
		}

		if (title_pattern != NULL && c_title != NULL) {
			if (g_pattern_match_simple(title_pattern, c_title))
				matches = TRUE;
		}

		if (matches)
			build_client_json_compact(builder, c);
	}

	json_builder_end_array(builder);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_find_window(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_find_window, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_compound_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* describe_desktop */
	if (gowl_module_mcp_is_tool_allowed(module, "describe_desktop")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("describe_desktop",
			"Get a complete snapshot of the desktop state: "
			"all monitors, tags, clients, and focused window. "
			"Best tool to call first to understand the desktop.");
		mcp_tool_set_read_only_hint(tool, TRUE);
		mcp_tool_set_idempotent_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_describe_desktop,
		                    module, NULL);
	}

	/* find_window */
	if (gowl_module_mcp_is_tool_allowed(module, "find_window")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("find_window",
			"Search for windows matching an app_id or title "
			"glob pattern. Returns all matching windows.");
		mcp_tool_set_read_only_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "app_id");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Glob pattern to match against app_id "
			"(e.g. 'firefox*', '*term*')");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "title");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Glob pattern to match against window title");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_find_window,
		                    module, NULL);
	}

	g_debug("compound tools registered");
}
