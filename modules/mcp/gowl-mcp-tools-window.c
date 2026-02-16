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
 * gowl-mcp-tools-window.c - Window management MCP tools.
 *
 * Implements the following MCP tools:
 *   - focus_client      : Focus a client by ID or pattern
 *   - close_client      : Close a client window
 *   - move_client_to_tag: Move a client to tag(s)
 *   - set_client_state  : Set floating/fullscreen/urgent
 *   - resize_client     : Set geometry for floating client
 *   - move_client       : Reposition a floating client
 *   - swap_clients      : Swap two clients in the tiling stack
 *   - zoom_client       : Promote client to master area
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
/* Helper: find a client by ID                                                */
/* ========================================================================== */

/**
 * find_client_by_id:
 * @compositor: the compositor
 * @id: the client ID to find
 *
 * Iterates the client list to find a client with the given ID.
 *
 * Returns: (nullable) (transfer none): the found client, or %NULL
 */
static GowlClient *
find_client_by_id(
	GowlCompositor *compositor,
	guint           id
){
	GList *clients;
	GList *iter;

	clients = gowl_compositor_get_clients(compositor);
	for (iter = clients; iter != NULL; iter = iter->next) {
		GowlClient *c;

		c = GOWL_CLIENT(iter->data);
		if (gowl_client_get_id(c) == id)
			return c;
	}
	return NULL;
}

/**
 * resolve_client:
 * @module: the MCP module
 * @arguments: JSON arguments with "id", "app_id", or "title"
 *
 * Resolves a client from tool arguments.  Checks "id" first,
 * then "app_id" pattern, then "title" pattern.
 *
 * Returns: (nullable) (transfer none): the resolved client, or %NULL
 */
static GowlClient *
resolve_client(
	GowlModuleMcp *module,
	JsonObject    *arguments
){
	if (arguments == NULL)
		return NULL;

	if (json_object_has_member(arguments, "id")) {
		guint id;

		id = (guint)json_object_get_int_member(arguments, "id");
		return find_client_by_id(module->compositor, id);
	}

	if (json_object_has_member(arguments, "app_id")) {
		const gchar *pattern;

		pattern = json_object_get_string_member(arguments, "app_id");
		if (pattern != NULL)
			return gowl_compositor_find_client_by_app_id(
				module->compositor, pattern);
	}

	if (json_object_has_member(arguments, "title")) {
		const gchar *pattern;

		pattern = json_object_get_string_member(arguments, "title");
		if (pattern != NULL)
			return gowl_compositor_find_client_by_title(
				module->compositor, pattern);
	}

	return NULL;
}

/**
 * make_error_result:
 * @message: error description
 *
 * Returns: (transfer full): an error McpToolResult
 */
static McpToolResult *
make_error_result(const gchar *message)
{
	McpToolResult *result;

	result = mcp_tool_result_new(TRUE);
	mcp_tool_result_add_text(result, message);
	return result;
}

/* ========================================================================== */
/* Helper: build JSON input schema via JsonBuilder                            */
/* ========================================================================== */

/**
 * build_client_selector_schema:
 * @builder: an active #JsonBuilder positioned inside "properties"
 *
 * Adds the common client selector properties (id, app_id, title)
 * to the schema properties object.
 */
static void
build_client_selector_schema(JsonBuilder *builder)
{
	/* id */
	json_builder_set_member_name(builder, "id");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "integer");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder,
		"Client window ID (from list_clients)");
	json_builder_end_object(builder);

	/* app_id */
	json_builder_set_member_name(builder, "app_id");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "string");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder,
		"Glob pattern matching the app_id");
	json_builder_end_object(builder);

	/* title */
	json_builder_set_member_name(builder, "title");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "string");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder,
		"Glob pattern matching the window title");
	json_builder_end_object(builder);
}

/* ========================================================================== */
/* Tool: focus_client                                                         */
/* ========================================================================== */

static McpToolResult *
tool_focus_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	gowl_compositor_focus_client(module->compositor, client, TRUE);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_focus_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_focus_client, arguments, NULL);
}

/* ========================================================================== */
/* Tool: close_client                                                         */
/* ========================================================================== */

static McpToolResult *
tool_close_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	gowl_client_close(client);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_close_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_close_client, arguments, NULL);
}

/* ========================================================================== */
/* Tool: move_client_to_tag                                                   */
/* ========================================================================== */

static McpToolResult *
tool_move_client_to_tag(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	guint32 tags;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "tags"))
		return make_error_result("Missing required parameter: tags");

	tags = (guint32)json_object_get_int_member(arguments, "tags");
	if (tags == 0)
		return make_error_result("Tags bitmask must be non-zero");

	gowl_client_set_tags(client, tags);

	/* re-arrange the monitor */
	{
		GowlMonitor *mon;

		mon = (GowlMonitor *)gowl_client_get_monitor(client);
		if (mon != NULL)
			gowl_compositor_arrange(module->compositor, mon);
	}

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_move_client_to_tag(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_move_client_to_tag, arguments, NULL);
}

/* ========================================================================== */
/* Tool: set_client_state                                                     */
/* ========================================================================== */

static McpToolResult *
tool_set_client_state(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	if (arguments != NULL &&
	    json_object_has_member(arguments, "floating"))
	{
		gowl_client_set_floating(client,
			json_object_get_boolean_member(arguments, "floating"));
	}

	if (arguments != NULL &&
	    json_object_has_member(arguments, "fullscreen"))
	{
		gowl_client_set_fullscreen(client,
			json_object_get_boolean_member(arguments, "fullscreen"));
	}

	if (arguments != NULL &&
	    json_object_has_member(arguments, "urgent"))
	{
		gowl_client_set_urgent(client,
			json_object_get_boolean_member(arguments, "urgent"));
	}

	/* re-arrange after state change */
	{
		GowlMonitor *mon;

		mon = (GowlMonitor *)gowl_client_get_monitor(client);
		if (mon != NULL)
			gowl_compositor_arrange(module->compositor, mon);
	}

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_set_client_state(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_set_client_state, arguments, NULL);
}

/* ========================================================================== */
/* Tool: resize_client                                                        */
/* ========================================================================== */

static McpToolResult *
tool_resize_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	gint x, y, w, h;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	if (!gowl_client_get_floating(client))
		return make_error_result(
			"Client is not floating; set floating first");

	/* get current geometry, override with provided values */
	gowl_client_get_geometry(client, &x, &y, &w, &h);

	if (arguments != NULL &&
	    json_object_has_member(arguments, "width"))
		w = (gint)json_object_get_int_member(arguments, "width");

	if (arguments != NULL &&
	    json_object_has_member(arguments, "height"))
		h = (gint)json_object_get_int_member(arguments, "height");

	gowl_client_set_geometry(client, x, y, w, h);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_resize_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_resize_client, arguments, NULL);
}

/* ========================================================================== */
/* Tool: move_client                                                          */
/* ========================================================================== */

static McpToolResult *
tool_move_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	gint x, y, w, h;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	if (!gowl_client_get_floating(client))
		return make_error_result(
			"Client is not floating; set floating first");

	gowl_client_get_geometry(client, &x, &y, &w, &h);

	if (arguments != NULL &&
	    json_object_has_member(arguments, "x"))
		x = (gint)json_object_get_int_member(arguments, "x");

	if (arguments != NULL &&
	    json_object_has_member(arguments, "y"))
		y = (gint)json_object_get_int_member(arguments, "y");

	gowl_client_set_geometry(client, x, y, w, h);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_move_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_move_client, arguments, NULL);
}

/* ========================================================================== */
/* Tool: swap_clients                                                         */
/* ========================================================================== */

static McpToolResult *
tool_swap_clients(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client_a;
	GowlClient *client_b;
	GList *clients;
	GList *link_a;
	GList *link_b;
	guint id_a, id_b;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "id_a") ||
	    !json_object_has_member(arguments, "id_b"))
		return make_error_result(
			"Missing required parameters: id_a, id_b");

	id_a = (guint)json_object_get_int_member(arguments, "id_a");
	id_b = (guint)json_object_get_int_member(arguments, "id_b");

	client_a = find_client_by_id(module->compositor, id_a);
	client_b = find_client_by_id(module->compositor, id_b);

	if (client_a == NULL || client_b == NULL)
		return make_error_result("One or both clients not found");

	/* swap positions in the client list */
	clients = gowl_compositor_get_clients(module->compositor);
	link_a = g_list_find(clients, client_a);
	link_b = g_list_find(clients, client_b);

	if (link_a != NULL && link_b != NULL) {
		gpointer tmp;

		tmp = link_a->data;
		link_a->data = link_b->data;
		link_b->data = tmp;
	}

	/* re-arrange the monitor(s) */
	{
		GowlMonitor *mon_a;
		GowlMonitor *mon_b;

		mon_a = (GowlMonitor *)gowl_client_get_monitor(client_a);
		mon_b = (GowlMonitor *)gowl_client_get_monitor(client_b);
		if (mon_a != NULL)
			gowl_compositor_arrange(module->compositor, mon_a);
		if (mon_b != NULL && mon_b != mon_a)
			gowl_compositor_arrange(module->compositor, mon_b);
	}

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_swap_clients(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_swap_clients, arguments, NULL);
}

/* ========================================================================== */
/* Tool: zoom_client                                                          */
/* ========================================================================== */

/**
 * tool_zoom_client:
 *
 * Promotes a client to the master area by moving it to the
 * front of the client list (same as dwl/dwm zoom).
 */
static McpToolResult *
tool_zoom_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	GList *clients;
	GList *link;
	GowlMonitor *mon;

	client = resolve_client(module, arguments);
	if (client == NULL)
		return make_error_result("Client not found");

	if (gowl_client_get_floating(client))
		return make_error_result(
			"Cannot zoom a floating client");

	/* move to head of client list */
	clients = gowl_compositor_get_clients(module->compositor);
	link = g_list_find(clients, client);
	if (link != NULL && link != clients) {
		/*
		 * Swap data with the first element to promote to master.
		 * We can't reorder the GList from outside the compositor,
		 * so we swap the data pointers.
		 */
		gpointer tmp;

		tmp = clients->data;
		clients->data = link->data;
		link->data = tmp;
	}

	mon = (GowlMonitor *)gowl_client_get_monitor(client);
	if (mon != NULL)
		gowl_compositor_arrange(module->compositor, mon);

	gowl_compositor_focus_client(module->compositor, client, TRUE);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_zoom_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_zoom_client, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

/**
 * gowl_mcp_register_window_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Registers all window management tools, filtered by the allowlist.
 */
void
gowl_mcp_register_window_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* focus_client */
	if (gowl_module_mcp_is_tool_allowed(module, "focus_client")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("focus_client",
			"Focus a client window by ID, app_id pattern, "
			"or title pattern.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);
		mcp_tool_set_destructive_hint(tool, FALSE);

		mcp_server_add_tool(server, tool, handle_focus_client,
		                    module, NULL);
	}

	/* close_client */
	if (gowl_module_mcp_is_tool_allowed(module, "close_client")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("close_client",
			"Close a client window (sends close request).");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_close_client,
		                    module, NULL);
	}

	/* move_client_to_tag */
	if (gowl_module_mcp_is_tool_allowed(module, "move_client_to_tag")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("move_client_to_tag",
			"Move a client to the specified tag(s). "
			"Tags is a bitmask (e.g. 1=tag1, 2=tag2, 3=tags 1+2).");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);
		json_builder_set_member_name(b, "tags");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Tag bitmask to assign to the client");
		json_builder_end_object(b);
		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "tags");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_move_client_to_tag,
		                    module, NULL);
	}

	/* set_client_state */
	if (gowl_module_mcp_is_tool_allowed(module, "set_client_state")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("set_client_state",
			"Set floating, fullscreen, and/or urgent state "
			"on a client window.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);

		json_builder_set_member_name(b, "floating");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "boolean");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "Set floating state");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "fullscreen");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "boolean");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "Set fullscreen state");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "urgent");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "boolean");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "Set urgent state");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_set_client_state,
		                    module, NULL);
	}

	/* resize_client */
	if (gowl_module_mcp_is_tool_allowed(module, "resize_client")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("resize_client",
			"Resize a floating client window. "
			"Only works on floating clients.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);

		json_builder_set_member_name(b, "width");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "New width in pixels");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "height");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "New height in pixels");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_resize_client,
		                    module, NULL);
	}

	/* move_client */
	if (gowl_module_mcp_is_tool_allowed(module, "move_client")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("move_client",
			"Move a floating client to a new position. "
			"Only works on floating clients.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);

		json_builder_set_member_name(b, "x");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "New X position in pixels");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "y");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "New Y position in pixels");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_move_client,
		                    module, NULL);
	}

	/* swap_clients */
	if (gowl_module_mcp_is_tool_allowed(module, "swap_clients")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("swap_clients",
			"Swap two clients' positions in the tiling stack.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "id_a");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "First client ID");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "id_b");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, "Second client ID");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "id_a");
		json_builder_add_string_value(b, "id_b");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_swap_clients,
		                    module, NULL);
	}

	/* zoom_client */
	if (gowl_module_mcp_is_tool_allowed(module, "zoom_client")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("zoom_client",
			"Promote a tiled client to the master area "
			"(equivalent to the zoom keybind).");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		build_client_selector_schema(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_zoom_client,
		                    module, NULL);
	}

	g_debug("window tools registered");
}
