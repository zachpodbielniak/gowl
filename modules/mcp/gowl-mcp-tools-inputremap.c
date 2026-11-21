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
 * gowl-mcp-tools-inputremap.c - Per-device input remapping over MCP.
 *
 * Tools: input_remap_list_devices, input_remap_list, input_remap_add,
 *        input_remap_remove, input_remap_status, input_remap_identify,
 *        input_remap_identify_result, input_remap_pads
 *
 * Every tool is a thin wrapper over the inputremap module's IPC words
 * (`inputremap-devices', `inputremap-add', ...), run on the compositor
 * thread through gowl_compositor_run_command().  One code path means the
 * socket, MCP, cmacs and Elisp cannot disagree about what a rule does
 * or which rules are refused -- in particular, a rule shaped like a
 * macro is refused here by the same validator as everywhere else.
 *
 * A rule may be given as YAML text or as a JSON object: JSON is valid
 * YAML flow syntax, so the object is serialised and handed to the same
 * parser.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

static McpToolResult *
error_result(const gchar *message)
{
	McpToolResult *result;

	result = mcp_tool_result_new(TRUE);
	mcp_tool_result_add_text(result, message);
	return result;
}

/*
 * run_ipc:
 * @module: the MCP module
 * @line: an `inputremap-' command line
 *
 * Runs @line and turns the module's reply into a tool result: "OK x"
 * becomes a success carrying x, "ERROR x" an error carrying x, and no
 * reply at all means the inputremap module is not loaded.
 */
static McpToolResult *
run_ipc(
	GowlModuleMcp *module,
	const gchar   *line
){
	g_autofree gchar *reply = NULL;
	McpToolResult *result;

	if (module == NULL || module->compositor == NULL)
		return error_result("Compositor not running");

	reply = gowl_compositor_run_command(module->compositor, line);
	if (reply == NULL)
		return error_result("The inputremap module is not loaded. It is "
		                    "opt-in: add `modules: inputremap: "
		                    "{enabled: true}' to the gowl config, or in "
		                    "cmacs call (cmacs-gowl-input-remap-enable).");
	if (g_str_has_prefix(reply, "ERROR"))
		return error_result(reply + (reply[5] == ' ' ? 6 : 5));

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result,
		g_str_has_prefix(reply, "OK ") ? reply + 3 : reply);
	return result;
}

/* A schema with no properties. */
static JsonNode *
empty_schema(void)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	json_builder_end_object(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
add_string_property(
	JsonBuilder *b,
	const gchar *name,
	const gchar *description
){
	json_builder_set_member_name(b, name);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "string");
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b, description);
	json_builder_end_object(b);
}

/* ========================================================================== */
/* Tools                                                                      */
/* ========================================================================== */

static McpToolResult *
tool_list_devices(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "inputremap-devices");
}

static McpToolResult *
tool_list(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "inputremap-list");
}

static McpToolResult *
tool_status(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "inputremap-status");
}

static McpToolResult *
tool_pads(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "inputremap-pads");
}

static McpToolResult *
tool_identify_result(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "inputremap-identify-result");
}

/*
 * input_remap_add: `yaml' (text) or `rule' (an object).  The rule text
 * goes on one command line, so a newline -- legal in block YAML -- is
 * folded to a space first; flow and JSON forms never need one.
 */
static McpToolResult *
tool_add(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *line = NULL;

	(void)user_data;

	if (arguments != NULL && json_object_has_member(arguments, "rule")) {
		JsonNode *node = json_object_get_member(arguments, "rule");
		g_autoptr(JsonGenerator) gen = json_generator_new();

		if (!JSON_NODE_HOLDS_OBJECT(node))
			return error_result("`rule' must be an object");
		json_generator_set_root(gen, node);
		text = json_generator_to_data(gen, NULL);
	} else if (arguments != NULL
	           && json_object_has_member(arguments, "yaml")) {
		const gchar *yaml = json_object_get_string_member(arguments, "yaml");

		if (yaml == NULL || *yaml == '\0')
			return error_result("`yaml' is empty");
		if (strchr(yaml, '\n') != NULL)
			return error_result("`yaml' must be one line of flow-style "
			                    "YAML, e.g. {name: p, match: {id: "
			                    "\"1a86:e026\"}, map: {KEY_A: {button: "
			                    "middle}}}; or pass `rule' as an object");
		text = g_strdup(yaml);
	} else {
		return error_result("Pass the rule as `rule' (an object) or "
		                    "`yaml' (flow-style text)");
	}

	line = g_strdup_printf("inputremap-add %s", text);
	return run_ipc(module, line);
}

static McpToolResult *
tool_remove(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *name;
	g_autofree gchar *line = NULL;

	(void)user_data;

	name = arguments != NULL && json_object_has_member(arguments, "name")
		? json_object_get_string_member(arguments, "name") : NULL;
	if (name == NULL || *name == '\0')
		return error_result("Missing required argument: name");
	line = g_strdup_printf("inputremap-remove %s", name);
	return run_ipc(module, line);
}

static McpToolResult *
tool_identify(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	g_autofree gchar *line = NULL;
	gint64 secs;

	(void)user_data;

	secs = arguments != NULL && json_object_has_member(arguments, "seconds")
		? json_object_get_int_member(arguments, "seconds") : 0;
	line = secs > 0 ? g_strdup_printf("inputremap-identify %" G_GINT64_FORMAT,
	                                  secs)
	                : g_strdup("inputremap-identify");
	return run_ipc(module, line);
}

/* The MCP-thread handlers: each forwards to the compositor thread. */
#define GOWL_MCP_IR_HANDLER(handler, func)                               \
	static McpToolResult *                                               \
	handler(McpServer *server, const gchar *name,                        \
	        JsonObject *arguments, gpointer user_data)                   \
	{                                                                    \
		(void)server;                                                    \
		(void)name;                                                      \
		return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,        \
		                              func, arguments, NULL);            \
	}

GOWL_MCP_IR_HANDLER(handle_list_devices, tool_list_devices)
GOWL_MCP_IR_HANDLER(handle_list, tool_list)
GOWL_MCP_IR_HANDLER(handle_status, tool_status)
GOWL_MCP_IR_HANDLER(handle_add, tool_add)
GOWL_MCP_IR_HANDLER(handle_remove, tool_remove)
GOWL_MCP_IR_HANDLER(handle_identify, tool_identify)
GOWL_MCP_IR_HANDLER(handle_identify_result, tool_identify_result)
GOWL_MCP_IR_HANDLER(handle_pads, tool_pads)

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

/* Adds one tool if the allowlist lets it through. */
static void
register_tool(
	McpServer      *server,
	GowlModuleMcp  *module,
	const gchar    *name,
	const gchar    *description,
	gboolean        read_only,
	JsonNode       *schema,
	McpToolHandler  handler
){
	g_autoptr(McpTool) tool = NULL;
	g_autoptr(JsonNode) owned = schema;

	if (!gowl_module_mcp_is_tool_allowed(module, name))
		return;
	tool = mcp_tool_new(name, description);
	mcp_tool_set_read_only_hint(tool, read_only);
	mcp_tool_set_input_schema(tool, owned);
	mcp_server_add_tool(server, tool, handler, module, NULL);
}

/**
 * gowl_mcp_register_inputremap_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Registers the per-device input remapping tools.  They need the
 * opt-in inputremap module at call time, not at registration: each
 * reports plainly when it is not loaded.
 */
void
gowl_mcp_register_inputremap_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	register_tool(server, module, "input_remap_list_devices",
		"Every connected keyboard, pointer and game controller: id, "
		"type (keyboard, pointer, gamepad, joystick), name, "
		"vendor-product (hex, as lsusb prints it), sysname and whether "
		"an input-remap rule claims it; a controller also lists its "
		"axes (ranges once open) and buttons. Use it to find a device's "
		"identity for a rule's `match:'.",
		TRUE, empty_schema(), handle_list_devices);

	register_tool(server, module, "input_remap_list",
		"The input-remap rules in force: name, source (config or "
		"runtime), number of mappings, the rule as YAML, and the ids of "
		"the connected devices it matches.",
		TRUE, empty_schema(), handle_list);

	register_tool(server, module, "input_remap_status",
		"Whether the input remapper is enabled, how many rules and "
		"claimed devices there are, the log level, and whether identify "
		"mode is listening.",
		TRUE, empty_schema(), handle_status);

	{
		g_autoptr(JsonBuilder) b = json_builder_new();

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "rule");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"The rule: {name, match: {name|id|vendor|product|sysname|"
			"type: keyboard|pointer|gamepad|joystick}, map: {INPUT: "
			"TARGET}, unmatched: pass|drop, log, grab, analog: {deadzone, "
			"press, release, invert, pointer-speed, pointer-curve, "
			"scroll-rate, mouse-gain, mouse-idle-ms}, virtual-pad: "
			"{name}}. INPUT is KEY_*, BTN_* (BTN_SOUTH...), "
			"left/middle/right/side/extra, WHEEL_UP/DOWN/LEFT/RIGHT, a "
			"stick direction ABS_X+ / ABS_X- (left-stick-left, dpad-up, "
			"...), a whole axis ABS_RX, or REL_X/REL_Y (mouse motion). "
			"TARGET is \"drop\", \"pass\" or exactly one of {key: "
			"\"Super+9\"}, {button: middle}, {action: tag-view, arg: "
			"\"256\"}, {command: \"...\"}, {macro: NAME}, {stick: "
			"left|right|dpad, x|y: -1..1}, {trigger: left|right, value}, "
			"{pad-button: BTN_SOUTH} (these three drive the rule's "
			"virtual pad), or -- on a whole axis -- {pointer: x|y}, "
			"{scroll: vertical|horizontal}. Macros, pointer, scroll and "
			"a stick following REL_X/REL_Y are the declared exceptions "
			"to one-to-one.");
		json_builder_end_object(b);
		add_string_property(b, "yaml",
			"The same rule as one line of flow-style YAML, instead of "
			"`rule'.");
		json_builder_end_object(b);
		json_builder_end_object(b);

		register_tool(server, module, "input_remap_add",
			"Add, or replace by name, a per-device input remap rule. "
			"Each input maps to exactly ONE output and the release "
			"mirrors the press: sequences, delays and repeats are "
			"refused (a {macro:} target runs a gowl macro, whose code "
			"may do more). Devices the rule matches are claimed at once.",
			FALSE, json_builder_get_root(b), handle_add);
	}

	{
		g_autoptr(JsonBuilder) b = json_builder_new();

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		add_string_property(b, "name", "The rule's name");
		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "name");
		json_builder_end_array(b);
		json_builder_end_object(b);

		register_tool(server, module, "input_remap_remove",
			"Remove an input remap rule by name. Devices only it "
			"claimed rejoin the shared keyboard, and anything they held "
			"down is released first.",
			FALSE, json_builder_get_root(b), handle_remove);
	}

	{
		g_autoptr(JsonBuilder) b = json_builder_new();

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "seconds");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"How long to listen, 1-300 (default from the module's "
			"identify-timeout, 10)");
		json_builder_end_object(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		register_tool(server, module, "input_remap_identify",
			"Start listening for the next key, button or stick direction "
			"on any device (game controllers included), without "
			"consuming it. Ask the person to press the "
			"device, then call input_remap_identify_result.",
			FALSE, json_builder_get_root(b), handle_identify);
	}

	register_tool(server, module, "input_remap_identify_result",
		"What input_remap_identify caught: the device (as "
		"input_remap_list_devices describes it) and the input name to "
		"use in a rule's `map:'. \"pending\" while still listening, "
		"\"none\" if it timed out.",
		TRUE, empty_schema(), handle_identify_result);

	register_tool(server, module, "input_remap_pads",
		"The virtual gamepads the input remapper drives (outputs of "
		"stick, trigger and pad-button targets): name, device name, "
		"vendor-product, whether the uinput device exists, whether "
		"creating it failed, and how many inputs hold it.",
		TRUE, empty_schema(), handle_pads);

	g_debug("inputremap tools registered");
}
