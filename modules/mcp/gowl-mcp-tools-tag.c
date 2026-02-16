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
 * gowl-mcp-tools-tag.c - Tag/workspace management MCP tools.
 *
 * Implements the following MCP tools:
 *   - view_tag        : Switch to a tag on a monitor
 *   - toggle_tag_view : Toggle a tag's visibility on a monitor
 *   - set_client_tags : Assign tag(s) to a client
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-core-private.h"

#include <json-glib/json-glib.h>

/* ========================================================================== */
/* Helper: resolve a monitor from arguments                                   */
/* ========================================================================== */

/**
 * resolve_monitor:
 * @module: the MCP module
 * @arguments: JSON arguments with optional "monitor" (name string)
 *
 * Resolves a monitor from arguments.  If no "monitor" key is present,
 * returns the selected (focused) monitor.
 *
 * Returns: (nullable) (transfer none): the resolved monitor
 */
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
		/* return the selected monitor (first in list) */
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
/* Tool: view_tag                                                             */
/* ========================================================================== */

/**
 * tool_view_tag:
 *
 * Switch to the specified tag(s) on a monitor.
 * Replaces the active tag set entirely.
 */
static McpToolResult *
tool_view_tag(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlMonitor *mon;
	guint32 tags;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "tags"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameter: tags");
		return r;
	}

	mon = resolve_monitor(module, arguments);
	if (mon == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Monitor not found");
		return r;
	}

	tags = (guint32)json_object_get_int_member(arguments, "tags");
	if (tags == 0) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Tags bitmask must be non-zero");
		return r;
	}

	gowl_monitor_set_tags(mon, tags);
	gowl_compositor_arrange(module->compositor, mon);
	gowl_compositor_focus_client(module->compositor, NULL, FALSE);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_view_tag(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_view_tag, arguments, NULL);
}

/* ========================================================================== */
/* Tool: toggle_tag_view                                                      */
/* ========================================================================== */

/**
 * tool_toggle_tag_view:
 *
 * Toggle a single tag's visibility on a monitor.
 * The tag parameter is the tag index (0-based).
 */
static McpToolResult *
tool_toggle_tag_view(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlMonitor *mon;
	guint32 tag_bit;
	gint tag_index;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "tag"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameter: tag");
		return r;
	}

	mon = resolve_monitor(module, arguments);
	if (mon == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Monitor not found");
		return r;
	}

	tag_index = (gint)json_object_get_int_member(arguments, "tag");
	if (tag_index < 0 || tag_index > 31) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Tag index must be 0-31");
		return r;
	}

	tag_bit = (1u << tag_index);
	gowl_monitor_toggle_tag(mon, tag_bit);
	gowl_compositor_arrange(module->compositor, mon);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_toggle_tag_view(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_toggle_tag_view, arguments, NULL);
}

/* ========================================================================== */
/* Tool: set_client_tags                                                      */
/* ========================================================================== */

/**
 * tool_set_client_tags:
 *
 * Assign tag(s) to a client window directly.
 */
static McpToolResult *
tool_set_client_tags(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	guint32 tags;
	GowlMonitor *mon;
	guint id;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "id") ||
	    !json_object_has_member(arguments, "tags"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameters: id, tags");
		return r;
	}

	id = (guint)json_object_get_int_member(arguments, "id");
	tags = (guint32)json_object_get_int_member(arguments, "tags");

	/* find client by iterating list */
	{
		GList *clients;
		GList *iter;

		client = NULL;
		clients = gowl_compositor_get_clients(module->compositor);
		for (iter = clients; iter != NULL; iter = iter->next) {
			GowlClient *c;

			c = GOWL_CLIENT(iter->data);
			if (gowl_client_get_id(c) == id) {
				client = c;
				break;
			}
		}
	}

	if (client == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Client not found");
		return r;
	}

	if (tags == 0) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Tags bitmask must be non-zero");
		return r;
	}

	gowl_client_set_tags(client, tags);

	mon = (GowlMonitor *)gowl_client_get_monitor(client);
	if (mon != NULL)
		gowl_compositor_arrange(module->compositor, mon);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_set_client_tags(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_set_client_tags, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_tag_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* view_tag */
	if (gowl_module_mcp_is_tool_allowed(module, "view_tag")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("view_tag",
			"Switch to the specified tag(s) on a monitor. "
			"Tags is a bitmask (1=tag1, 2=tag2, 4=tag3, etc.).");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "tags");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Tag bitmask to view (e.g. 1 for tag 1, "
			"3 for tags 1+2)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "monitor");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Monitor name (e.g. 'eDP-1'). "
			"Defaults to focused monitor.");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "tags");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_view_tag,
		                    module, NULL);
	}

	/* toggle_tag_view */
	if (gowl_module_mcp_is_tool_allowed(module, "toggle_tag_view")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("toggle_tag_view",
			"Toggle a single tag's visibility on a monitor.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "tag");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Tag index (0-based) to toggle");
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
		json_builder_add_string_value(b, "tag");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_toggle_tag_view,
		                    module, NULL);
	}

	/* set_client_tags */
	if (gowl_module_mcp_is_tool_allowed(module, "set_client_tags")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("set_client_tags",
			"Assign tag(s) to a client window. "
			"Tags is a bitmask.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "id");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Client window ID (from list_clients)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "tags");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Tag bitmask to assign");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "id");
		json_builder_add_string_value(b, "tags");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_set_client_tags,
		                    module, NULL);
	}

	g_debug("tag tools registered");
}
