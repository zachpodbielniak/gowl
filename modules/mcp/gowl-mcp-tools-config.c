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
 * gowl-mcp-tools-config.c - Config, keybind, and control MCP tools.
 *
 * Implements:
 *   - add_rule          : Add a window rule at runtime
 *   - add_keybind       : Bind a key combo to an action
 *   - spawn             : Launch an external process
 *   - compositor_quit   : Shut down the compositor
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "gowl-enums.h"

#include <json-glib/json-glib.h>
#include <xkbcommon/xkbcommon.h>

/* ========================================================================== */
/* Tool: add_rule                                                             */
/* ========================================================================== */

/**
 * tool_add_rule:
 *
 * Adds a window rule at runtime.  Parameters: app_id, title,
 * tags, floating, monitor.
 */
static McpToolResult *
tool_add_rule(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlConfig *config;
	const gchar *app_id;
	const gchar *title;
	guint32 tags;
	gboolean floating;
	gint monitor;

	config = gowl_compositor_get_config(module->compositor);
	if (config == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "No config available");
		return r;
	}

	app_id = NULL;
	title = NULL;
	tags = 0;
	floating = FALSE;
	monitor = -1;

	if (arguments != NULL) {
		if (json_object_has_member(arguments, "app_id"))
			app_id = json_object_get_string_member(
				arguments, "app_id");
		if (json_object_has_member(arguments, "title"))
			title = json_object_get_string_member(
				arguments, "title");
		if (json_object_has_member(arguments, "tags"))
			tags = (guint32)json_object_get_int_member(
				arguments, "tags");
		if (json_object_has_member(arguments, "floating"))
			floating = json_object_get_boolean_member(
				arguments, "floating");
		if (json_object_has_member(arguments, "monitor"))
			monitor = (gint)json_object_get_int_member(
				arguments, "monitor");
	}

	if (app_id == NULL && title == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"At least one of app_id or title is required");
		return r;
	}

	gowl_config_add_rule(config, app_id, title, tags,
	                     floating, monitor);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_add_rule(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_add_rule, arguments, NULL);
}

/* ========================================================================== */
/* Tool: add_keybind                                                          */
/* ========================================================================== */

/**
 * parse_modifier_string:
 * @mods_str: modifier string like "Logo+Ctrl+Shift"
 *
 * Parses a human-readable modifier string into a bitmask.
 *
 * Returns: the modifier bitmask
 */
static guint
parse_modifier_string(const gchar *mods_str)
{
	guint mods;
	g_auto(GStrv) parts = NULL;
	gint i;

	mods = GOWL_KEY_MOD_NONE;
	if (mods_str == NULL || mods_str[0] == '\0')
		return mods;

	parts = g_strsplit(mods_str, "+", -1);
	for (i = 0; parts[i] != NULL; i++) {
		g_strstrip(parts[i]);
		if (g_ascii_strcasecmp(parts[i], "logo") == 0 ||
		    g_ascii_strcasecmp(parts[i], "super") == 0 ||
		    g_ascii_strcasecmp(parts[i], "mod4") == 0)
			mods |= GOWL_KEY_MOD_LOGO;
		else if (g_ascii_strcasecmp(parts[i], "ctrl") == 0 ||
		         g_ascii_strcasecmp(parts[i], "control") == 0)
			mods |= GOWL_KEY_MOD_CTRL;
		else if (g_ascii_strcasecmp(parts[i], "alt") == 0 ||
		         g_ascii_strcasecmp(parts[i], "mod1") == 0)
			mods |= GOWL_KEY_MOD_ALT;
		else if (g_ascii_strcasecmp(parts[i], "shift") == 0)
			mods |= GOWL_KEY_MOD_SHIFT;
	}

	return mods;
}

/**
 * tool_add_keybind:
 *
 * Adds a keybind at runtime.  Parameters: modifiers (string),
 * key (string keysym name), action (string nick), arg (optional).
 */
static McpToolResult *
tool_add_keybind(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlConfig *config;
	const gchar *mods_str;
	const gchar *key_str;
	const gchar *action_str;
	const gchar *arg;
	guint modifiers;
	xkb_keysym_t keysym;
	GEnumClass *action_class;
	GEnumValue *action_val;
	gint action;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "key") ||
	    !json_object_has_member(arguments, "action"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameters: key, action");
		return r;
	}

	config = gowl_compositor_get_config(module->compositor);
	if (config == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "No config available");
		return r;
	}

	/* parse modifiers */
	mods_str = json_object_has_member(arguments, "modifiers")
		? json_object_get_string_member(arguments, "modifiers")
		: NULL;
	modifiers = parse_modifier_string(mods_str);

	/* parse keysym */
	key_str = json_object_get_string_member(arguments, "key");
	keysym = xkb_keysym_from_name(key_str, XKB_KEYSYM_CASE_INSENSITIVE);
	if (keysym == XKB_KEY_NoSymbol) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Unknown keysym name");
		return r;
	}

	/* parse action */
	action_str = json_object_get_string_member(arguments, "action");
	action_class = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	action_val = g_enum_get_value_by_nick(action_class, action_str);
	if (action_val == NULL) {
		g_type_class_unref(action_class);
		{
			McpToolResult *r;

			r = mcp_tool_result_new(TRUE);
			mcp_tool_result_add_text(r,
				"Unknown action name");
			return r;
		}
	}
	action = action_val->value;
	g_type_class_unref(action_class);

	/* optional argument */
	arg = json_object_has_member(arguments, "arg")
		? json_object_get_string_member(arguments, "arg")
		: NULL;

	gowl_config_add_keybind(config, modifiers, keysym, action, arg);

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_add_keybind(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_add_keybind, arguments, NULL);
}

/* ========================================================================== */
/* Tool: spawn                                                                */
/* ========================================================================== */

/**
 * tool_spawn:
 *
 * Launches an external process asynchronously.
 */
static McpToolResult *
tool_spawn(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *command;
	g_autoptr(GError) error = NULL;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "command"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameter: command");
		return r;
	}

	command = json_object_get_string_member(arguments, "command");
	if (command == NULL || command[0] == '\0') {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Command cannot be empty");
		return r;
	}

	if (!g_spawn_command_line_async(command, &error)) {
		McpToolResult *r;
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf("Spawn failed: %s",
			error->message);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, msg);
		return r;
	}

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_spawn(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_spawn, arguments, NULL);
}

/* ========================================================================== */
/* Tool: compositor_quit                                                      */
/* ========================================================================== */

/**
 * tool_compositor_quit:
 *
 * Shuts down the compositor.
 */
static McpToolResult *
tool_compositor_quit(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;

	gowl_compositor_quit(module->compositor);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, "Compositor quit requested");
	return result;
}

static McpToolResult *
handle_compositor_quit(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_compositor_quit, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_config_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* add_rule */
	if (gowl_module_mcp_is_tool_allowed(module, "add_rule")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("add_rule",
			"Add a window rule at runtime. "
			"Rules match by app_id and/or title pattern and "
			"set tags, floating state, and target monitor.");

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
			"App ID pattern to match");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "title");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Window title pattern to match");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "tags");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Tag bitmask to assign when matched");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "floating");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "boolean");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Whether matched client should float");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "monitor");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Monitor index to place client on (-1 for default)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_add_rule,
		                    module, NULL);
	}

	/* add_keybind */
	if (gowl_module_mcp_is_tool_allowed(module, "add_keybind")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("add_keybind",
			"Bind a key combination to a compositor action. "
			"Modifiers: Logo, Ctrl, Alt, Shift. "
			"Key: XKB keysym name (e.g. Return, a, F1). "
			"Action: spawn, kill-client, toggle-float, etc.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "modifiers");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Modifier string: Logo+Ctrl+Alt+Shift "
			"(combine with +)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "key");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"XKB keysym name (e.g. Return, a, F1)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "action");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Action nick: spawn, kill-client, toggle-float, "
			"toggle-fullscreen, focus-stack, tag-view, "
			"set-mfact, inc-nmaster, set-layout, zoom, quit");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "arg");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Optional argument for the action "
			"(e.g. command for spawn)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "key");
		json_builder_add_string_value(b, "action");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_add_keybind,
		                    module, NULL);
	}

	/* spawn */
	if (gowl_module_mcp_is_tool_allowed(module, "spawn")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("spawn",
			"Launch an external process asynchronously.");

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "command");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Command line to execute (parsed by shell)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "command");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_spawn,
		                    module, NULL);
	}

	/* compositor_quit */
	if (gowl_module_mcp_is_tool_allowed(module, "compositor_quit")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("compositor_quit",
			"Shut down the compositor. This will end "
			"the Wayland session.");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_compositor_quit,
		                    module, NULL);
	}

	g_debug("config tools registered");
}
