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
 * gowl-mcp-tools-query.c - Read-only query tools for compositor state.
 *
 * Implements the following MCP tools:
 *   - list_clients      : List all managed windows
 *   - list_monitors     : List all outputs
 *   - get_tag_state     : Per-monitor tag information
 *   - get_config        : Current runtime config values
 *   - list_keybinds     : Active keybind table
 *   - list_modules      : Loaded modules
 *   - get_focused_client: Currently focused window details
 *
 * All tool handlers dispatch to the compositor thread via
 * gowl_mcp_dispatch_call() and return structured JSON.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "module/gowl-module.h"
#include "module/gowl-module-info.h"
#include "module/gowl-module-manager.h"
#include "gowl-enums.h"

#include <json-glib/json-glib.h>
#include <xkbcommon/xkbcommon.h>

/* ========================================================================== */
/* Helper: build a JSON object for a single client                            */
/* ========================================================================== */

/**
 * build_client_json:
 * @builder: an active #JsonBuilder
 * @client: a #GowlClient
 *
 * Adds a JSON object describing @client to the builder.
 * The caller must have begun an object or array.
 */
static void
build_client_json(
	JsonBuilder *builder,
	GowlClient  *client
){
	gint x, y, w, h;
	GowlMonitor *mon;
	const gchar *mon_name;

	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, gowl_client_get_id(client));

	json_builder_set_member_name(builder, "app_id");
	json_builder_add_string_value(builder,
		gowl_client_get_app_id(client) ? gowl_client_get_app_id(client) : "");

	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder,
		gowl_client_get_title(client) ? gowl_client_get_title(client) : "");

	json_builder_set_member_name(builder, "tags");
	json_builder_add_int_value(builder, gowl_client_get_tags(client));

	json_builder_set_member_name(builder, "floating");
	json_builder_add_boolean_value(builder, gowl_client_get_floating(client));

	json_builder_set_member_name(builder, "fullscreen");
	json_builder_add_boolean_value(builder, gowl_client_get_fullscreen(client));

	json_builder_set_member_name(builder, "urgent");
	json_builder_add_boolean_value(builder, gowl_client_get_urgent(client));

	/* geometry */
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

	/* monitor name */
	mon = (GowlMonitor *)gowl_client_get_monitor(client);
	mon_name = (mon != NULL) ? gowl_monitor_get_name(mon) : NULL;
	json_builder_set_member_name(builder, "monitor");
	json_builder_add_string_value(builder, mon_name ? mon_name : "");

	/* process id */
	json_builder_set_member_name(builder, "pid");
	json_builder_add_int_value(builder, (gint64)gowl_client_get_pid(client));

	json_builder_end_object(builder);
}

/* ========================================================================== */
/* Helper: build a JSON object for a single monitor                           */
/* ========================================================================== */

/**
 * build_monitor_json:
 * @builder: an active #JsonBuilder
 * @monitor: a #GowlMonitor
 *
 * Adds a JSON object describing @monitor to the builder.
 */
static void
build_monitor_json(
	JsonBuilder *builder,
	GowlMonitor *monitor
){
	gint x, y, w, h;

	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder,
		gowl_monitor_get_name(monitor) ? gowl_monitor_get_name(monitor) : "");

	/* geometry */
	gowl_monitor_get_geometry(monitor, &x, &y, &w, &h);
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
	json_builder_add_int_value(builder, gowl_monitor_get_tags(monitor));

	json_builder_set_member_name(builder, "layout");
	json_builder_add_string_value(builder,
		gowl_monitor_get_layout_symbol(monitor) ?
			gowl_monitor_get_layout_symbol(monitor) : "");

	json_builder_set_member_name(builder, "mfact");
	json_builder_add_double_value(builder, gowl_monitor_get_mfact(monitor));

	json_builder_set_member_name(builder, "nmaster");
	json_builder_add_int_value(builder, gowl_monitor_get_nmaster(monitor));

	json_builder_end_object(builder);
}

/* ========================================================================== */
/* Helper: serialize a JsonBuilder to text                                    */
/* ========================================================================== */

/**
 * builder_to_string:
 * @builder: a #JsonBuilder whose root has been set
 *
 * Generates a pretty-printed JSON string from the builder's root node.
 *
 * Returns: (transfer full): the JSON string; free with g_free()
 */
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
/* Tool: list_clients                                                         */
/* ========================================================================== */

/**
 * tool_list_clients:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Lists all managed client windows with their properties.
 *
 * Returns: (transfer full): JSON array of client objects
 */
static McpToolResult *
tool_list_clients(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GList *clients;
	GList *iter;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	clients = gowl_compositor_get_clients(module->compositor);

	builder = json_builder_new();
	json_builder_begin_array(builder);
	for (iter = clients; iter != NULL; iter = iter->next) {
		build_client_json(builder, GOWL_CLIENT(iter->data));
	}
	json_builder_end_array(builder);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

/**
 * handle_list_clients:
 *
 * MCP tool handler that dispatches list_clients to the compositor thread.
 */
static McpToolResult *
handle_list_clients(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_list_clients, arguments, NULL);
}

/* ========================================================================== */
/* Tool: list_monitors                                                        */
/* ========================================================================== */

/**
 * tool_list_monitors:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Lists all output monitors with their properties.
 *
 * Returns: (transfer full): JSON array of monitor objects
 */
static McpToolResult *
tool_list_monitors(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GList *monitors;
	GList *iter;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	monitors = gowl_compositor_get_monitors(module->compositor);

	builder = json_builder_new();
	json_builder_begin_array(builder);
	for (iter = monitors; iter != NULL; iter = iter->next) {
		build_monitor_json(builder, GOWL_MONITOR(iter->data));
	}
	json_builder_end_array(builder);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_list_monitors(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_list_monitors, arguments, NULL);
}

/* ========================================================================== */
/* Tool: get_tag_state                                                        */
/* ========================================================================== */

/**
 * tool_get_tag_state:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Returns per-monitor tag state: which tags are active and how many
 * clients are on each tag.
 *
 * Returns: (transfer full): JSON array of per-monitor tag state
 */
static McpToolResult *
tool_get_tag_state(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlConfig *config;
	gint tag_count;
	GList *monitors;
	GList *clients;
	GList *iter;
	gint i;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	config = gowl_compositor_get_config(module->compositor);
	tag_count = config ? gowl_config_get_tag_count(config) : 9;
	monitors = gowl_compositor_get_monitors(module->compositor);
	clients = gowl_compositor_get_clients(module->compositor);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (iter = monitors; iter != NULL; iter = iter->next) {
		GowlMonitor *mon;
		guint32 active_tags;
		GList *citer;

		mon = GOWL_MONITOR(iter->data);
		active_tags = gowl_monitor_get_tags(mon);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "monitor");
		json_builder_add_string_value(builder,
			gowl_monitor_get_name(mon) ? gowl_monitor_get_name(mon) : "");

		json_builder_set_member_name(builder, "active_tags");
		json_builder_add_int_value(builder, active_tags);

		/* per-tag client counts */
		json_builder_set_member_name(builder, "tags");
		json_builder_begin_array(builder);
		for (i = 0; i < tag_count; i++) {
			guint32 tag_bit;
			gint count;

			tag_bit = (1u << i);
			count = 0;

			/* count clients on this monitor with this tag */
			for (citer = clients; citer != NULL; citer = citer->next) {
				GowlClient *c;

				c = GOWL_CLIENT(citer->data);
				if (gowl_client_get_monitor(c) == (gpointer)mon &&
				    (gowl_client_get_tags(c) & tag_bit))
				{
					count++;
				}
			}

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "index");
			json_builder_add_int_value(builder, i);

			json_builder_set_member_name(builder, "active");
			json_builder_add_boolean_value(builder,
				(active_tags & tag_bit) != 0);

			json_builder_set_member_name(builder, "client_count");
			json_builder_add_int_value(builder, count);
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);

		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_get_tag_state(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_get_tag_state, arguments, NULL);
}

/* ========================================================================== */
/* Tool: get_config                                                           */
/* ========================================================================== */

/**
 * tool_get_config:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Returns the current runtime configuration values.
 *
 * Returns: (transfer full): JSON object with config properties
 */
static McpToolResult *
tool_get_config(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlConfig *config;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	config = gowl_compositor_get_config(module->compositor);
	if (config == NULL) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result, "No config available");
		return result;
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "border_width");
	json_builder_add_int_value(builder,
		gowl_config_get_border_width(config));

	json_builder_set_member_name(builder, "border_color_focus");
	json_builder_add_string_value(builder,
		gowl_config_get_border_color_focus(config) ?
			gowl_config_get_border_color_focus(config) : "");

	json_builder_set_member_name(builder, "border_color_unfocus");
	json_builder_add_string_value(builder,
		gowl_config_get_border_color_unfocus(config) ?
			gowl_config_get_border_color_unfocus(config) : "");

	json_builder_set_member_name(builder, "border_color_urgent");
	json_builder_add_string_value(builder,
		gowl_config_get_border_color_urgent(config) ?
			gowl_config_get_border_color_urgent(config) : "");

	json_builder_set_member_name(builder, "mfact");
	json_builder_add_double_value(builder,
		gowl_config_get_mfact(config));

	json_builder_set_member_name(builder, "nmaster");
	json_builder_add_int_value(builder,
		gowl_config_get_nmaster(config));

	json_builder_set_member_name(builder, "tag_count");
	json_builder_add_int_value(builder,
		gowl_config_get_tag_count(config));

	json_builder_set_member_name(builder, "repeat_rate");
	json_builder_add_int_value(builder,
		gowl_config_get_repeat_rate(config));

	json_builder_set_member_name(builder, "repeat_delay");
	json_builder_add_int_value(builder,
		gowl_config_get_repeat_delay(config));

	json_builder_set_member_name(builder, "terminal");
	json_builder_add_string_value(builder,
		gowl_config_get_terminal(config) ?
			gowl_config_get_terminal(config) : "");

	json_builder_set_member_name(builder, "menu");
	json_builder_add_string_value(builder,
		gowl_config_get_menu(config) ?
			gowl_config_get_menu(config) : "");

	json_builder_set_member_name(builder, "sloppyfocus");
	json_builder_add_boolean_value(builder,
		gowl_config_get_sloppyfocus(config));

	json_builder_set_member_name(builder, "log_level");
	json_builder_add_string_value(builder,
		gowl_config_get_log_level(config) ?
			gowl_config_get_log_level(config) : "");

	json_builder_end_object(builder);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_get_config(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_get_config, arguments, NULL);
}

/* ========================================================================== */
/* Tool: list_keybinds                                                        */
/* ========================================================================== */

/**
 * tool_list_keybinds:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Lists all active keybind entries with human-readable modifier
 * and action names.
 *
 * Returns: (transfer full): JSON array of keybind objects
 */
static McpToolResult *
tool_list_keybinds(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlConfig *config;
	GArray *keybinds;
	GEnumClass *action_class;
	guint i;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	config = gowl_compositor_get_config(module->compositor);
	if (config == NULL) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result, "No config available");
		return result;
	}

	keybinds = gowl_config_get_keybinds(config);
	action_class = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < keybinds->len; i++) {
		GowlKeybindEntry *entry;
		GEnumValue *action_val;
		gchar keysym_name[64];
		g_autoptr(GString) mods_str = NULL;

		entry = &g_array_index(keybinds, GowlKeybindEntry, i);

		json_builder_begin_object(builder);

		/* modifiers as human-readable string */
		mods_str = g_string_new(NULL);
		if (entry->modifiers & GOWL_KEY_MOD_LOGO)
			g_string_append(mods_str, "Logo+");
		if (entry->modifiers & GOWL_KEY_MOD_CTRL)
			g_string_append(mods_str, "Ctrl+");
		if (entry->modifiers & GOWL_KEY_MOD_ALT)
			g_string_append(mods_str, "Alt+");
		if (entry->modifiers & GOWL_KEY_MOD_SHIFT)
			g_string_append(mods_str, "Shift+");

		/* keysym name */
		xkb_keysym_get_name(entry->keysym, keysym_name,
		                    sizeof(keysym_name));

		/* build combo string: "Logo+Ctrl+Return" */
		g_string_append(mods_str, keysym_name);

		json_builder_set_member_name(builder, "combo");
		json_builder_add_string_value(builder, mods_str->str);

		/* action as nick */
		action_val = g_enum_get_value(action_class, entry->action);
		json_builder_set_member_name(builder, "action");
		json_builder_add_string_value(builder,
			action_val ? action_val->value_nick : "unknown");

		/* argument (nullable) */
		json_builder_set_member_name(builder, "arg");
		if (entry->arg != NULL)
			json_builder_add_string_value(builder, entry->arg);
		else
			json_builder_add_null_value(builder);

		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	g_type_class_unref(action_class);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_list_keybinds(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_list_keybinds, arguments, NULL);
}

/* ========================================================================== */
/* Tool: list_modules                                                         */
/* ========================================================================== */

/**
 * tool_list_modules:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Lists all loaded modules with their names, descriptions,
 * versions, and activation state.
 *
 * Returns: (transfer full): JSON array of module objects
 */
static McpToolResult *
tool_list_modules(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlModuleManager *manager;
	GList *modules;
	GList *iter;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	manager = gowl_compositor_get_module_manager(module->compositor);
	if (manager == NULL) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result, "No module manager available");
		return result;
	}

	modules = gowl_module_manager_get_modules(manager);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (iter = modules; iter != NULL; iter = iter->next) {
		GowlModuleInfo *info;
		const gchar *val;

		info = (GowlModuleInfo *)iter->data;

		json_builder_begin_object(builder);

		val = gowl_module_info_get_name(info);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, val ? val : "");

		val = gowl_module_info_get_description(info);
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, val ? val : "");

		val = gowl_module_info_get_version(info);
		json_builder_set_member_name(builder, "version");
		json_builder_add_string_value(builder, val ? val : "");

		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	g_list_free_full(modules, (GDestroyNotify)gowl_module_info_free);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_list_modules(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_list_modules, arguments, NULL);
}

/* ========================================================================== */
/* Tool: get_focused_client                                                   */
/* ========================================================================== */

/**
 * tool_get_focused_client:
 * @module: the MCP module (runs on compositor thread)
 * @arguments: (nullable): unused
 * @user_data: unused
 *
 * Returns details of the currently focused client, or null if none.
 *
 * Returns: (transfer full): JSON object or "null"
 */
static McpToolResult *
tool_get_focused_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlClient *focused;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;

	focused = gowl_compositor_get_focused_client(module->compositor);

	if (focused == NULL) {
		result = mcp_tool_result_new(FALSE);
		mcp_tool_result_add_text(result, "null");
		return result;
	}

	builder = json_builder_new();
	build_client_json(builder, focused);

	json_str = builder_to_string(builder);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_get_focused_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_get_focused_client, arguments, NULL);
}

/* ========================================================================== */
/* Helper: create a tool with no input schema (parameterless)                  */
/* ========================================================================== */

/**
 * make_no_param_tool:
 * @name: tool name
 * @description: tool description
 *
 * Creates an McpTool with an empty object input schema (no parameters).
 * Marks it as read-only and idempotent.
 *
 * Returns: (transfer full): the new tool
 */
static McpTool *
make_no_param_tool(
	const gchar *name,
	const gchar *description
){
	McpTool *tool;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) schema = NULL;

	tool = mcp_tool_new(name, description);
	mcp_tool_set_read_only_hint(tool, TRUE);
	mcp_tool_set_idempotent_hint(tool, TRUE);
	mcp_tool_set_destructive_hint(tool, FALSE);

	/* empty object schema */
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "object");
	json_builder_set_member_name(builder, "properties");
	json_builder_begin_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	schema = json_builder_get_root(builder);
	mcp_tool_set_input_schema(tool, schema);

	return tool;
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

/**
 * gowl_mcp_register_query_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Registers all read-only query tools, filtered by the allowlist.
 */
void
gowl_mcp_register_query_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	if (gowl_module_mcp_is_tool_allowed(module, "list_clients")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"list_clients",
			"List all managed windows with app_id, title, tags, "
			"geometry, floating/fullscreen/urgent state, monitor, "
			"and process ID.");
		mcp_server_add_tool(server, tool, handle_list_clients,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "list_monitors")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"list_monitors",
			"List all output monitors with name, geometry, active "
			"tags, layout symbol, mfact, and nmaster.");
		mcp_server_add_tool(server, tool, handle_list_monitors,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "get_tag_state")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"get_tag_state",
			"Get per-monitor tag state: which tags are active "
			"and how many clients are on each tag.");
		mcp_server_add_tool(server, tool, handle_get_tag_state,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "get_config")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"get_config",
			"Get the current runtime configuration values "
			"(border, layout defaults, keyboard, terminal, etc.).");
		mcp_server_add_tool(server, tool, handle_get_config,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "list_keybinds")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"list_keybinds",
			"List all active keybinds with key combo, action "
			"name, and optional argument.");
		mcp_server_add_tool(server, tool, handle_list_keybinds,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "list_modules")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"list_modules",
			"List all loaded compositor modules with name, "
			"description, version, priority, and active state.");
		mcp_server_add_tool(server, tool, handle_list_modules,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "get_focused_client")) {
		g_autoptr(McpTool) tool = make_no_param_tool(
			"get_focused_client",
			"Get details of the currently focused window, or "
			"null if no window is focused.");
		mcp_server_add_tool(server, tool, handle_get_focused_client,
		                    module, NULL);
	}

	g_debug("query tools registered");
}
