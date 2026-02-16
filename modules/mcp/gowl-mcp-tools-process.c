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
 * gowl-mcp-tools-process.c - Process introspection MCP tools.
 *
 * Implements:
 *   - get_client_process_info : PID, cmdline, cwd from /proc
 *   - signal_client           : Send POSIX signal to client process
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"

#include <json-glib/json-glib.h>
#include <signal.h>
#include <sys/types.h>

/* ========================================================================== */
/* Helper: find client by ID                                                  */
/* ========================================================================== */

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

/* ========================================================================== */
/* Tool: get_client_process_info                                              */
/* ========================================================================== */

/**
 * tool_get_client_process_info:
 *
 * Returns PID, cmdline, and cwd for a client's owning process.
 * Reads from /proc/PID/ on the compositor thread.
 */
static McpToolResult *
tool_get_client_process_info(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	GowlClient *client;
	pid_t pid;
	guint id;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *json_str = NULL;
	g_autofree gchar *cmdline_path = NULL;
	g_autofree gchar *cwd_path = NULL;
	g_autofree gchar *cwd = NULL;
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "id"))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Missing required parameter: id");
		return result;
	}

	id = (guint)json_object_get_int_member(arguments, "id");
	client = find_client_by_id(module->compositor, id);
	if (client == NULL) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result, "Client not found");
		return result;
	}

	pid = gowl_client_get_pid(client);
	if (pid <= 0) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Could not determine client PID");
		return result;
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "pid");
	json_builder_add_int_value(builder, (gint64)pid);

	/*
	 * Read /proc/PID/cmdline (NUL-separated arguments).
	 * Replace NUL separators with spaces for readability.
	 */
	cmdline_path = g_strdup_printf("/proc/%d/cmdline", (int)pid);
	{
		g_autofree gchar *raw = NULL;
		gsize raw_len;
		gsize i;

		if (g_file_get_contents(cmdline_path, &raw,
		                        &raw_len, NULL))
		{
			for (i = 0; i < raw_len; i++) {
				if (raw[i] == '\0')
					raw[i] = ' ';
			}
			g_strchomp(raw);
			json_builder_set_member_name(builder, "cmdline");
			json_builder_add_string_value(builder, raw);
		} else {
			json_builder_set_member_name(builder, "cmdline");
			json_builder_add_null_value(builder);
		}
	}

	/* read /proc/PID/cwd (symlink) */
	cwd_path = g_strdup_printf("/proc/%d/cwd", (int)pid);
	cwd = g_file_read_link(cwd_path, NULL);
	json_builder_set_member_name(builder, "cwd");
	if (cwd != NULL)
		json_builder_add_string_value(builder, cwd);
	else
		json_builder_add_null_value(builder);

	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	gen = json_generator_new();
	json_generator_set_pretty(gen, TRUE);
	json_generator_set_root(gen, root);
	json_str = json_generator_to_data(gen, NULL);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json_str);
	return result;
}

static McpToolResult *
handle_get_client_process_info(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_get_client_process_info, arguments, NULL);
}

/* ========================================================================== */
/* Tool: signal_client                                                        */
/* ========================================================================== */

/**
 * tool_signal_client:
 *
 * Sends a POSIX signal to a client's owning process.
 */
static McpToolResult *
tool_signal_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	pid_t pid;
	gint sig;
	guint id;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "id") ||
	    !json_object_has_member(arguments, "signal"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameters: id, signal");
		return r;
	}

	id = (guint)json_object_get_int_member(arguments, "id");
	sig = (gint)json_object_get_int_member(arguments, "signal");

	client = find_client_by_id(module->compositor, id);
	if (client == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Client not found");
		return r;
	}

	pid = gowl_client_get_pid(client);
	if (pid <= 0) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Could not determine client PID");
		return r;
	}

	if (kill(pid, sig) != 0) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to send signal");
		return r;
	}

	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_signal_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_signal_client, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_process_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* get_client_process_info */
	if (gowl_module_mcp_is_tool_allowed(module,
	    "get_client_process_info"))
	{
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("get_client_process_info",
			"Get process information for a client window: "
			"PID, command line, and working directory.");
		mcp_tool_set_read_only_hint(tool, TRUE);

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

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "id");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool,
			handle_get_client_process_info, module, NULL);
	}

	/* signal_client */
	if (gowl_module_mcp_is_tool_allowed(module, "signal_client")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("signal_client",
			"Send a POSIX signal to a client's process. "
			"Common signals: 15 (SIGTERM), 9 (SIGKILL).");
		mcp_tool_set_destructive_hint(tool, TRUE);

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
			"Client window ID");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "signal");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Signal number (e.g. 15 for SIGTERM, "
			"9 for SIGKILL)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "id");
		json_builder_add_string_value(b, "signal");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_signal_client,
		                    module, NULL);
	}

	g_debug("process tools registered");
}
