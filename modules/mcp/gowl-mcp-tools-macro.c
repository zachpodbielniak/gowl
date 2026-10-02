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
 * gowl-mcp-tools-macro.c - Macros over MCP.
 *
 * Tools: macro_run, macro_stop, macro_list, macro_status, macro_info,
 *        macro_clear, macro_triggers, macro_filter_test, macro_record,
 *        macro_voice_match, macro_voice_status
 *
 * macro_record starts a recording only with the input recorder's
 * `input-recording' consent (the module's --require-consent): the
 * Super+Alt+r key needs none because the person pressed it, and a
 * program asking is a different thing.  There is deliberately no tool
 * that turns the microphone on; macro_voice_match runs the macro a
 * sentence names, which is the half an agent has a use for.
 *
 * Thin wrappers over the macro module's IPC words (`macro-run',
 * `macro-stop', ...), run on the compositor thread through
 * gowl_compositor_run_command(), exactly as a keybind or gowl-msg would.
 * One path: the same lookup, the same guard, the same quarantine.
 *
 * Arguments are shell-quoted onto the command line, so an argument with
 * spaces or quotes arrives at the macro as the one argument it was.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"

#include <json-glib/json-glib.h>
#include <string.h>

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
 * @line: a `macro-' command line
 *
 * "OK x" becomes a success carrying x, "ERROR x" an error carrying x,
 * and no reply means the macro module is not loaded.
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
		return error_result("The macro module is not loaded. It is "
		                    "opt-in: add `modules: macro: {enabled: "
		                    "true}' to the gowl config; cmacs loads it on "
		                    "first use of any cmacs-gowl-macro function.");
	if (g_str_has_prefix(reply, "ERROR"))
		return error_result(reply + (reply[5] == ' ' ? 6 : 5));

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result,
		g_str_has_prefix(reply, "OK ") ? reply + 3 : reply);
	return result;
}

static const gchar *
string_arg(
	JsonObject  *arguments,
	const gchar *name
){
	if (arguments == NULL || !json_object_has_member(arguments, name))
		return NULL;
	return json_object_get_string_member(arguments, name);
}

/* `WORD ARG', ARG shell-quoted, or bare `WORD' without one. */
static McpToolResult *
run_word(
	GowlModuleMcp *module,
	const gchar   *word,
	const gchar   *arg
){
	g_autofree gchar *line = NULL;

	if (arg != NULL && *arg != '\0') {
		g_autofree gchar *q = g_shell_quote(arg);

		line = g_strdup_printf("%s %s", word, q);
	} else {
		line = g_strdup(word);
	}
	return run_ipc(module, line);
}

/* ---- tools (compositor thread) ---- */

static McpToolResult *
tool_run(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *name;
	GString *line;
	McpToolResult *result;

	(void)user_data;
	name = string_arg(arguments, "name");
	if (name == NULL || *name == '\0')
		return error_result("Missing required argument: name");

	line = g_string_new("macro-run --trigger=api");
	{
		g_autofree gchar *q = g_shell_quote(name);

		g_string_append_printf(line, " -- %s", q);
	}
	if (arguments != NULL && json_object_has_member(arguments, "args")) {
		JsonNode *node = json_object_get_member(arguments, "args");
		JsonArray *array;
		guint i;

		if (!JSON_NODE_HOLDS_ARRAY(node)) {
			g_string_free(line, TRUE);
			return error_result("`args' must be an array of strings");
		}
		array = json_node_get_array(node);
		for (i = 0; i < json_array_get_length(array); i++) {
			const gchar *a = json_array_get_string_element(array, i);
			g_autofree gchar *q = NULL;

			if (a == NULL) {
				g_string_free(line, TRUE);
				return error_result("`args' must be an array of strings");
			}
			q = g_shell_quote(a);
			g_string_append_printf(line, " %s", q);
		}
	}
	result = run_ipc(module, line->str);
	g_string_free(line, TRUE);
	return result;
}

static McpToolResult *
tool_stop(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)user_data;
	return run_word(module, "macro-stop", string_arg(arguments, "which"));
}

static McpToolResult *
tool_list(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "macro-list");
}

static McpToolResult *
tool_status(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "macro-status");
}

static McpToolResult *
tool_info(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *name;

	(void)user_data;
	name = string_arg(arguments, "name");
	if (name == NULL || *name == '\0')
		return error_result("Missing required argument: name");
	return run_word(module, "macro-info", name);
}

static McpToolResult *
tool_triggers(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "macro-triggers");
}

static McpToolResult *
tool_filter_test(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *filter = string_arg(arguments, "filter");
	const gchar *event = string_arg(arguments, "event");
	g_autofree gchar *line = NULL;

	(void)user_data;
	if (filter == NULL || *filter == '\0')
		return error_result("Missing required argument: filter");
	/* The filter is the rest of the line, as typed: not shell-quoted */
	line = event != NULL && *event != '\0'
		? g_strdup_printf("macro-filter-test --event=%s %s", event, filter)
		: g_strdup_printf("macro-filter-test %s", filter);
	return run_ipc(module, line);
}

static McpToolResult *
tool_clear(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)user_data;
	return run_word(module, "macro-clear", string_arg(arguments, "name"));
}

/* The MCP-thread handlers: each forwards to the compositor thread. */
#define GOWL_MCP_MACRO_HANDLER(handler, func)                            \
	static McpToolResult *                                               \
	handler(McpServer *server, const gchar *name,                        \
	        JsonObject *arguments, gpointer user_data)                   \
	{                                                                    \
		(void)server;                                                    \
		(void)name;                                                      \
		return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,        \
		                              func, arguments, NULL);            \
	}

/* macro_record: action start|stop|cancel|status, optional name */
static McpToolResult *
tool_record(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *action;
	const gchar *name;
	g_autofree gchar *line = NULL;

	(void)user_data;
	action = string_arg(arguments, "action");
	name = string_arg(arguments, "name");
	if (action == NULL || *action == '\0')
		action = "status";
	if (g_strcmp0(action, "start") == 0) {
		g_autofree gchar *q = NULL;

		if (name != NULL && *name != '\0') {
			q = g_shell_quote(name);
			line = g_strdup_printf("macro-record --require-consent "
			                       "start %s", q);
		} else {
			line = g_strdup("macro-record --require-consent start");
		}
	} else if (g_strcmp0(action, "stop") == 0
	           || g_strcmp0(action, "cancel") == 0
	           || g_strcmp0(action, "status") == 0) {
		line = g_strdup_printf("macro-record %s", action);
	} else {
		return error_result("action must be start, stop, cancel or "
		                    "status");
	}
	return run_ipc(module, line);
}

/* macro_voice_match: text, optional dry_run */
static McpToolResult *
tool_voice_match(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *text;
	gboolean dry;
	g_autofree gchar *flat = NULL;
	g_autofree gchar *line = NULL;

	(void)user_data;
	text = string_arg(arguments, "text");
	if (text == NULL || *text == '\0')
		return error_result("Missing required argument: text");
	dry = arguments != NULL && json_object_has_member(arguments, "dry_run")
	      && json_object_get_boolean_member(arguments, "dry_run");
	/* one line: the IPC word reads to the end of it */
	flat = g_strdelimit(g_strdup(text), "\r\n\t", ' ');
	line = g_strdup_printf("macro-voice-match %s%s",
	                       dry ? "--dry-run " : "", flat);
	return run_ipc(module, line);
}

static McpToolResult *
tool_voice_status(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "macro-voice status");
}

GOWL_MCP_MACRO_HANDLER(handle_run, tool_run)
GOWL_MCP_MACRO_HANDLER(handle_record, tool_record)
GOWL_MCP_MACRO_HANDLER(handle_voice_match, tool_voice_match)
GOWL_MCP_MACRO_HANDLER(handle_voice_status, tool_voice_status)
GOWL_MCP_MACRO_HANDLER(handle_stop, tool_stop)
GOWL_MCP_MACRO_HANDLER(handle_list, tool_list)
GOWL_MCP_MACRO_HANDLER(handle_status, tool_status)
GOWL_MCP_MACRO_HANDLER(handle_info, tool_info)
GOWL_MCP_MACRO_HANDLER(handle_clear, tool_clear)
GOWL_MCP_MACRO_HANDLER(handle_triggers, tool_triggers)
GOWL_MCP_MACRO_HANDLER(handle_filter_test, tool_filter_test)

/* ---- registration ---- */

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

/*
 * A schema of string properties: @props is name, description pairs,
 * NULL-terminated; @required (nullable) the one required name.
 */
static JsonNode *
string_schema(
	const gchar * const *props,
	const gchar         *required
){
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	for (i = 0; props != NULL && props[i] != NULL; i += 2) {
		json_builder_set_member_name(b, props[i]);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, props[i + 1]);
		json_builder_end_object(b);
	}
	json_builder_end_object(b);
	if (required != NULL) {
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, required);
		json_builder_end_array(b);
	}
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static JsonNode *
run_schema(void)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "string");
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b,
		"A macro: a name found on the macro search path (`sort-windows' "
		"or `sort-windows.c'), a full path to a .c or .so file, or a "
		"name registered from C or defined with macro-define");
	json_builder_end_object(b);
	json_builder_set_member_name(b, "args");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "array");
	json_builder_set_member_name(b, "items");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "string");
	json_builder_end_object(b);
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b, "The macro's arguments");
	json_builder_end_object(b);
	json_builder_end_object(b);
	json_builder_set_member_name(b, "required");
	json_builder_begin_array(b);
	json_builder_add_string_value(b, "name");
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/**
 * gowl_mcp_register_macro_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Registers the macro tools.  They need the opt-in macro module at call
 * time, not at registration: each reports plainly when it is missing.
 */
void
gowl_mcp_register_macro_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	static const gchar * const stop_props[] = {
		"which", "A macro name, a run id, or `all' (the default)", NULL
	};
	static const gchar * const name_props[] = {
		"name", "The macro's name", NULL
	};
	static const gchar * const filter_props[] = {
		"filter", "A trigger filter, e.g. app-id=firefox* and "
		"(title=*YouTube* or title=*Twitch*); fields: event, app-id, "
		"title, floating, fullscreen, urgent, xwayland, focused-app-id, "
		"focused-title, monitor, layout, tag, tags, clients, arg, time, "
		"hour, weekday",
		"event", "The event name the fields are filled in for (optional)",
		NULL
	};
	static const gchar * const clear_props[] = {
		"name", "The macro to let run again; all of them when omitted",
		NULL
	};

	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	register_tool(server, module, "macro_run",
		"Run a gowl macro: in-process C (crispy) that drives the "
		"compositor -- sort windows, type into a named window, move "
		"windows between tags. It runs under a fault guard and a time "
		"budget; a macro that crashes or runs away is stopped, held "
		"back and reported instead of taking the session down. The "
		"reply is the macro's result, or `started NAME id=N' when it "
		"queued timed steps or runs on a worker thread.",
		FALSE, run_schema(), handle_run);
	register_tool(server, module, "macro_stop",
		"Stop running macros: queued steps are dropped, a threaded "
		"macro is told at its next step or sleep.",
		FALSE, string_schema(stop_props, NULL), handle_stop);
	register_tool(server, module, "macro_list",
		"Every macro that can be run by name: files on the search path "
		"(first of each name), macros registered from C, and ones "
		"defined with macro-define; with whether each is held back.",
		TRUE, string_schema(NULL, NULL), handle_list);
	register_tool(server, module, "macro_status",
		"Running macros (id, mode, age, steps left), those held back "
		"after a fault and why, the fault count and the default time "
		"budget.",
		TRUE, string_schema(NULL, NULL), handle_status);
	register_tool(server, module, "macro_info",
		"One macro: where it was found, its description, whether it is "
		"threaded, its time budget, and whether it is held back. "
		"Compiles it if it has changed.",
		TRUE, string_schema(name_props, "name"), handle_info);
	register_tool(server, module, "macro_triggers",
		"The event and timer triggers in force: each line, its filter as "
		"understood (fully parenthesised), the macro and arguments, and "
		"how often it fired or its filter turned the event away.",
		TRUE, string_schema(NULL, NULL), handle_triggers);
	register_tool(server, module, "macro_filter_test",
		"Judge a trigger filter against the focused window and selected "
		"monitor now: the verdict, the filter as understood, and every "
		"field's current value -- write a filter against real values.",
		TRUE, string_schema(filter_props, "filter"), handle_filter_test);
	register_tool(server, module, "macro_record",
		"The macro recorder (Super+Alt+r): `start' records the person's "
		"keys, clicks, drags and scrolls until `stop', which writes "
		"last-recording.c (and NAME.c when a name was given) and replies "
		"`recorded NAME STEPS PATH'; `cancel' discards; `status' says "
		"whether one is running. Starting from here needs the gowl "
		"config's `input-recording: true' -- the same consent as "
		"start_recording -- because it watches the person's input. The "
		"screen is framed while it runs, password prompts are not "
		"recorded, and Super+Shift+Escape stops it.",
		FALSE, json_from_string(
			"{\"type\":\"object\",\"properties\":{"
			"\"action\":{\"type\":\"string\",\"enum\":[\"start\","
			"\"stop\",\"cancel\",\"status\"]},"
			"\"name\":{\"type\":\"string\",\"description\":"
			"\"Also write NAME.c (letters, digits, - and _)\"}},"
			"\"required\":[\"action\"]}", NULL), handle_record);
	register_tool(server, module, "macro_voice_match",
		"Run the macro a sentence names, as if it had been said to "
		"Super+Alt+m: a configured voice phrase, else a macro's name said "
		"as words (\"pip corner 25\" runs pip-corner 25), else every "
		"word of a name in any order. With dry_run, only report what it "
		"would run (heard, normalised, macro, args).",
		FALSE, json_from_string(
			"{\"type\":\"object\",\"properties\":{"
			"\"text\":{\"type\":\"string\"},"
			"\"dry_run\":{\"type\":\"boolean\"}},"
			"\"required\":[\"text\"]}", NULL), handle_voice_match);
	register_tool(server, module, "macro_voice_status",
		"The voice listener: whether it is listening, the voice-command, "
		"the time limit, the last sentence heard, the last error and "
		"how many phrases are configured. There is no tool to start "
		"listening: the microphone is the person's to turn on.",
		TRUE, string_schema(NULL, NULL), handle_voice_status);
	register_tool(server, module, "macro_clear",
		"Let a macro that was held back after a crash or a runaway loop "
		"run again.",
		FALSE, string_schema(clear_props, NULL), handle_clear);
}
