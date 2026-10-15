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
 * gowl-mcp-tools-broadcast.c - Input broadcasting over MCP.
 *
 * Tools: broadcast_status, broadcast_list, broadcast_set,
 *        broadcast_add_team, broadcast_add_rule, broadcast_remove,
 *        broadcast_assign, broadcast_focus_slot
 *
 * Thin wrappers over the broadcast module's IPC words (`broadcast-status',
 * `broadcast-add-rule', ...), run on the compositor thread through
 * gowl_compositor_run_command(), so the socket, MCP, cmacs and Elisp all
 * reach the same validation.  A team or rule may be given as YAML text
 * or as a JSON object (JSON is valid YAML flow syntax).
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

/* Runs @line; "OK x" is a success carrying x, "ERROR x" an error, and
 * no reply at all means the broadcast module is not loaded. */
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
		return error_result("The broadcast module is not loaded. It is "
		                    "opt-in: add `modules: broadcast: {enabled: "
		                    "true}' to the gowl config, or in cmacs set "
		                    "cmacs-gowl-broadcast-enable.");
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

/* A team or rule from `object' (JSON) or `yaml' (one line), as text. */
static gchar *
definition_text(
	JsonObject   *arguments,
	const gchar  *object_name,
	gchar       **out_error
){
	const gchar *yaml;

	*out_error = NULL;
	if (arguments != NULL && json_object_has_member(arguments, object_name)) {
		JsonNode *node = json_object_get_member(arguments, object_name);
		g_autoptr(JsonGenerator) gen = json_generator_new();

		if (!JSON_NODE_HOLDS_OBJECT(node)) {
			*out_error = g_strdup_printf("`%s' must be an object",
			                             object_name);
			return NULL;
		}
		json_generator_set_root(gen, node);
		return json_generator_to_data(gen, NULL);
	}
	yaml = string_arg(arguments, "yaml");
	if (yaml != NULL && *yaml != '\0') {
		if (strchr(yaml, '\n') != NULL) {
			*out_error = g_strdup("`yaml' must be one line of flow-style "
			                      "YAML; or pass an object");
			return NULL;
		}
		return g_strdup(yaml);
	}
	*out_error = g_strdup_printf("Pass `%s' (an object) or `yaml' "
	                             "(flow-style text)", object_name);
	return NULL;
}

/* Names go on one command line: no spaces, no newlines. */
static gboolean
word_ok(const gchar *s)
{
	const gchar *p;

	if (s == NULL || *s == '\0')
		return FALSE;
	for (p = s; *p != '\0'; p++)
		if (g_ascii_isspace(*p) || g_ascii_iscntrl(*p))
			return FALSE;
	return TRUE;
}

/* ========================================================================== */
/* Tools                                                                      */
/* ========================================================================== */

static McpToolResult *
tool_status(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "broadcast-status");
}

static McpToolResult *
tool_list(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	(void)arguments;
	(void)user_data;
	return run_ipc(module, "broadcast-list");
}

static McpToolResult *
tool_set(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	static const gchar * const states[] = {
		"on", "off", "toggle", "panic", NULL
	};
	const gchar *state = string_arg(arguments, "state");
	const gchar *strict = string_arg(arguments, "strict");
	g_autofree gchar *line = NULL;

	(void)user_data;
	if (strict != NULL) {
		if (g_strcmp0(strict, "on") != 0 && g_strcmp0(strict, "off") != 0
		    && g_strcmp0(strict, "toggle") != 0)
			return error_result("`strict' is on, off or toggle");
		line = g_strdup_printf("broadcast-strict %s", strict);
		return run_ipc(module, line);
	}
	if (state == NULL || !g_strv_contains(states, state))
		return error_result("`state' is on, off, toggle or panic");
	line = g_strdup_printf("broadcast-%s", state);
	return run_ipc(module, line);
}

static McpToolResult *
tool_add_team(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *line = NULL;

	(void)user_data;
	text = definition_text(arguments, "team", &err);
	if (text == NULL)
		return error_result(err);
	line = g_strdup_printf("broadcast-add-team %s", text);
	return run_ipc(module, line);
}

static McpToolResult *
tool_add_rule(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *line = NULL;

	(void)user_data;
	text = definition_text(arguments, "rule", &err);
	if (text == NULL)
		return error_result(err);
	line = g_strdup_printf("broadcast-add-rule %s", text);
	return run_ipc(module, line);
}

static McpToolResult *
tool_remove(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *kind = string_arg(arguments, "kind");
	const gchar *name = string_arg(arguments, "name");
	g_autofree gchar *line = NULL;

	(void)user_data;
	if (g_strcmp0(kind, "team") != 0 && g_strcmp0(kind, "rule") != 0)
		return error_result("`kind' is team or rule");
	if (!word_ok(name))
		return error_result("`name' is required, one word");
	line = g_strdup_printf("broadcast-remove-%s %s", kind, name);
	return run_ipc(module, line);
}

static McpToolResult *
tool_assign(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *team = string_arg(arguments, "team");
	const gchar *slot = string_arg(arguments, "slot");
	gint64 id;
	g_autofree gchar *line = NULL;

	(void)user_data;
	if (!word_ok(team) || !word_ok(slot))
		return error_result("`team' and `slot' are required, one word "
		                    "each");
	id = arguments != NULL && json_object_has_member(arguments, "client_id")
		? json_object_get_int_member(arguments, "client_id") : -1;
	if (id == 0)
		line = g_strdup_printf("broadcast-unassign %s %s", team, slot);
	else if (id > 0)
		line = g_strdup_printf("broadcast-assign %s %s %" G_GINT64_FORMAT,
		                       team, slot, id);
	else
		line = g_strdup_printf("broadcast-assign %s %s", team, slot);
	return run_ipc(module, line);
}

static McpToolResult *
tool_focus_slot(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	const gchar *team = string_arg(arguments, "team");
	const gchar *slot = string_arg(arguments, "slot");
	g_autofree gchar *line = NULL;

	(void)user_data;
	if (!word_ok(slot))
		return error_result("`slot' is required, one word");
	if (team != NULL && !word_ok(team))
		return error_result("`team' is one word");
	line = team != NULL
		? g_strdup_printf("broadcast-focus-slot %s %s", team, slot)
		: g_strdup_printf("broadcast-focus-slot %s", slot);
	return run_ipc(module, line);
}

/* The MCP-thread handlers: each forwards to the compositor thread. */
#define GOWL_MCP_BC_HANDLER(handler, func)                               \
	static McpToolResult *                                               \
	handler(McpServer *server, const gchar *name,                        \
	        JsonObject *arguments, gpointer user_data)                   \
	{                                                                    \
		(void)server;                                                    \
		(void)name;                                                      \
		return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,        \
		                              func, arguments, NULL);            \
	}

GOWL_MCP_BC_HANDLER(handle_status, tool_status)
GOWL_MCP_BC_HANDLER(handle_list, tool_list)
GOWL_MCP_BC_HANDLER(handle_set, tool_set)
GOWL_MCP_BC_HANDLER(handle_add_team, tool_add_team)
GOWL_MCP_BC_HANDLER(handle_add_rule, tool_add_rule)
GOWL_MCP_BC_HANDLER(handle_remove, tool_remove)
GOWL_MCP_BC_HANDLER(handle_assign, tool_assign)
GOWL_MCP_BC_HANDLER(handle_focus_slot, tool_focus_slot)

/* ========================================================================== */
/* Schemas and registration                                                   */
/* ========================================================================== */

/* {type: object, properties: {...}, required: [...]} built from a
 * NULL-terminated list of (name, type, description) triples. */
static JsonNode *
schema(
	const gchar * const *props,
	const gchar * const *required
){
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	for (i = 0; props != NULL && props[i] != NULL; i += 3) {
		json_builder_set_member_name(b, props[i]);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, props[i + 1]);
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, props[i + 2]);
		json_builder_end_object(b);
	}
	json_builder_end_object(b);
	if (required != NULL && required[0] != NULL) {
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		for (i = 0; required[i] != NULL; i++)
			json_builder_add_string_value(b, required[i]);
		json_builder_end_array(b);
	}
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
register_tool(
	McpServer      *server,
	GowlModuleMcp  *module,
	const gchar    *name,
	const gchar    *description,
	gboolean        read_only,
	JsonNode       *input_schema,
	McpToolHandler  handler
){
	g_autoptr(McpTool) tool = NULL;
	g_autoptr(JsonNode) owned = input_schema;

	if (!gowl_module_mcp_is_tool_allowed(module, name))
		return;
	tool = mcp_tool_new(name, description);
	mcp_tool_set_read_only_hint(tool, read_only);
	mcp_tool_set_input_schema(tool, owned);
	mcp_server_add_tool(server, tool, handler, module, NULL);
}

/**
 * gowl_mcp_register_broadcast_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Registers the input-broadcasting tools.  They need the opt-in
 * broadcast module at call time, not at registration: each reports
 * plainly when it is not loaded.
 */
void
gowl_mcp_register_broadcast_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	static const gchar * const set_props[] = {
		"state", "string", "on, off, toggle, or panic (off and release "
		                   "every key held anywhere)",
		"strict", "string", "Instead of state: on, off or toggle strict "
		                    "one-press-one-window mode",
		NULL
	};
	static const gchar * const team_props[] = {
		"team", "object", "{name, slots: [{name, match, focus-key, "
		                  "delivery}], groups: {NAME: [slot, ...]}}. "
		                  "match is a filter over app-id, title, pid, "
		                  "xwayland, tag, tags, monitor, floating, "
		                  "fullscreen, e.g. \"app-id = eqgame.exe\"",
		"yaml", "string", "The same team as one line of flow YAML",
		NULL
	};
	static const gchar * const rule_props[] = {
		"rule", "object", "{name, team, when, enabled, keys: {include, "
		                  "exclude}, modifiers: any|none, targets: [all|"
		                  "all-other|current|under-mouse|slot:NAME|"
		                  "group:NAME], translate: {SLOT|*: {FROM: TO}}, "
		                  "mode: mirror|round-robin, reset-ms, delivery: "
		                  "auto|seat|activate, mouse: off|same|scaled, "
		                  "delay-ms, local: pass|block}",
		"yaml", "string", "The same rule as one line of flow YAML",
		NULL
	};
	static const gchar * const remove_props[] = {
		"kind", "string", "team or rule",
		"name", "string", "Its name",
		NULL
	};
	static const gchar * const remove_req[] = { "kind", "name", NULL };
	static const gchar * const assign_props[] = {
		"team", "string", "The team",
		"slot", "string", "The slot (name or 1-based number)",
		"client_id", "integer", "The window id (gowl_list_clients); "
		                        "omitted: the focused window; 0: back to "
		                        "the slot's match",
		NULL
	};
	static const gchar * const assign_req[] = { "team", "slot", NULL };
	static const gchar * const focus_props[] = {
		"team", "string", "The team (default: the focused window's)",
		"slot", "string", "The slot (name or 1-based number)",
		NULL
	};
	static const gchar * const focus_req[] = { "slot", NULL };

	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	register_tool(server, module, "broadcast_status",
		"Input broadcasting (multiboxing): whether it is on, strict "
		"mode, the toggle and panic keys, every team with the window "
		"filling each slot, every rule with the reason it cannot run if "
		"any, and delivery counters.",
		TRUE, schema(NULL, NULL), handle_status);
	register_tool(server, module, "broadcast_list",
		"The broadcast teams and rules in force, each as YAML.",
		TRUE, schema(NULL, NULL), handle_list);
	register_tool(server, module, "broadcast_set",
		"Switch input broadcasting on, off or toggle it (a toast shows "
		"the result), or panic: off, and every key held in any window "
		"released. Or set strict mode.",
		FALSE, schema(set_props, NULL), handle_set);
	register_tool(server, module, "broadcast_add_team",
		"Add, or replace by name, a broadcast team for this session: "
		"named slots, each filled by the first window its match accepts.",
		FALSE, schema(team_props, NULL), handle_add_team);
	register_tool(server, module, "broadcast_add_rule",
		"Add, or replace by name, a broadcast rule for this session. The "
		"reply says when it cannot run (missing team or slot, or refused "
		"by strict mode).",
		FALSE, schema(rule_props, NULL), handle_add_rule);
	register_tool(server, module, "broadcast_remove",
		"Remove a broadcast team or rule by name.",
		FALSE, schema(remove_props, remove_req), handle_remove);
	register_tool(server, module, "broadcast_assign",
		"Put a window in a team's slot by hand (it holds until the "
		"window closes), or with client_id 0 return the slot to its "
		"match.",
		FALSE, schema(assign_props, assign_req), handle_assign);
	register_tool(server, module, "broadcast_focus_slot",
		"Focus the window in a team's slot, viewing its tag.",
		FALSE, schema(focus_props, focus_req), handle_focus_slot);
}
