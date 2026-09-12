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
 * gowl-mcp-tools-clipboard.c - The clipboard history, as MCP tools.
 *
 * gowl keeps clipboard history in the compositor rather than in a
 * helper process, so an agent that wants what the human copied five
 * minutes ago does not need a second daemon running.  The clipboard
 * module owns the store and answers `clipboard-*' commands; these
 * tools are that command set with schemas and descriptions on it.
 *
 * Everything routes through gowl_compositor_run_command, which means a
 * session with the clipboard module unloaded answers "not loaded"
 * rather than failing in some other way -- the tools are registered
 * either way, since modules can be loaded and unloaded while the
 * compositor runs.
 *
 * The read side is deliberately separate from the write side in the
 * allowlist: `list_clipboard' is history disclosure and
 * `copy_clipboard_entry' changes what the next paste produces, and a
 * `tools:' list should be able to grant one without the other.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"

#include <json-glib/json-glib.h>

static McpToolResult *
clip_error(const gchar *message)
{
	McpToolResult *result = mcp_tool_result_new(TRUE);

	mcp_tool_result_add_text(result, message);
	return result;
}

/*
 * Runs one `clipboard-*' command on the compositor thread.  A NULL
 * reply means no module claimed it, which for these commands means the
 * clipboard module is not loaded; the compositor answers the rest.
 */
static McpToolResult *
clip_run(GowlModuleMcp *module, const gchar *line)
{
	g_autofree gchar *reply = NULL;
	McpToolResult *result;

	if (module == NULL || module->compositor == NULL)
		return clip_error("the compositor is not running");

	reply = gowl_compositor_run_command(module->compositor, line);
	if (reply == NULL)
		return clip_error("the clipboard module is not loaded");
	if (g_str_has_prefix(reply, "ERROR "))
		return clip_error(reply + 6);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, reply);
	return result;
}

/* The entry id every single-entry tool takes. */
static guint64
clip_id_arg(JsonObject *arguments)
{
	if (arguments == NULL || !json_object_has_member(arguments, "id"))
		return 0;
	return (guint64)json_object_get_int_member(arguments, "id");
}

static void
clip_add_id_property(JsonBuilder *b)
{
	json_builder_set_member_name(b, "id");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "integer");
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b,
		"The entry's id, as list_clipboard reports it.");
	json_builder_end_object(b);
}

/* An object schema with just `id', required. */
static JsonNode *
clip_id_schema(void)
{
	g_autoptr(JsonBuilder) b = json_builder_new();

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	clip_add_id_property(b);
	json_builder_end_object(b);
	json_builder_set_member_name(b, "required");
	json_builder_begin_array(b);
	json_builder_add_string_value(b, "id");
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static JsonNode *
clip_empty_schema(void)
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

/* --- list_clipboard --- */

static McpToolResult *
tool_list_clipboard(GowlModuleMcp *module, JsonObject *arguments,
                    gpointer user_data)
{
	(void)arguments;
	(void)user_data;
	return clip_run(module, "clipboard-list");
}

static McpToolResult *
handle_list_clipboard(McpServer *server, const gchar *name,
                      JsonObject *arguments, gpointer user_data)
{
	(void)server;
	(void)name;
	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_list_clipboard, arguments, NULL);
}

/* --- get_clipboard_entry --- */

static McpToolResult *
tool_get_clipboard_entry(GowlModuleMcp *module, JsonObject *arguments,
                         gpointer user_data)
{
	g_autofree gchar *line = NULL;
	guint64 id = clip_id_arg(arguments);
	(void)user_data;

	if (id == 0)
		return clip_error("an entry id is required");
	line = g_strdup_printf("clipboard-path %" G_GUINT64_FORMAT, id);
	return clip_run(module, line);
}

static McpToolResult *
handle_get_clipboard_entry(McpServer *server, const gchar *name,
                           JsonObject *arguments, gpointer user_data)
{
	(void)server;
	(void)name;
	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_get_clipboard_entry, arguments, NULL);
}

/* --- copy_clipboard_entry --- */

static McpToolResult *
tool_copy_clipboard_entry(GowlModuleMcp *module, JsonObject *arguments,
                          gpointer user_data)
{
	g_autofree gchar *line = NULL;
	guint64 id = clip_id_arg(arguments);
	(void)user_data;

	if (id == 0)
		return clip_error("an entry id is required");
	line = g_strdup_printf("clipboard-copy %" G_GUINT64_FORMAT, id);
	return clip_run(module, line);
}

static McpToolResult *
handle_copy_clipboard_entry(McpServer *server, const gchar *name,
                            JsonObject *arguments, gpointer user_data)
{
	(void)server;
	(void)name;
	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_copy_clipboard_entry, arguments, NULL);
}

/* --- delete_clipboard_entry --- */

static McpToolResult *
tool_delete_clipboard_entry(GowlModuleMcp *module, JsonObject *arguments,
                            gpointer user_data)
{
	g_autofree gchar *line = NULL;
	guint64 id = clip_id_arg(arguments);
	(void)user_data;

	if (id == 0)
		return clip_error("an entry id is required");
	line = g_strdup_printf("clipboard-delete %" G_GUINT64_FORMAT, id);
	return clip_run(module, line);
}

static McpToolResult *
handle_delete_clipboard_entry(McpServer *server, const gchar *name,
                              JsonObject *arguments, gpointer user_data)
{
	(void)server;
	(void)name;
	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_delete_clipboard_entry, arguments, NULL);
}

/* --- clear_clipboard --- */

static McpToolResult *
tool_clear_clipboard(GowlModuleMcp *module, JsonObject *arguments,
                     gpointer user_data)
{
	(void)arguments;
	(void)user_data;
	return clip_run(module, "clipboard-clear");
}

static McpToolResult *
handle_clear_clipboard(McpServer *server, const gchar *name,
                       JsonObject *arguments, gpointer user_data)
{
	(void)server;
	(void)name;
	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_clear_clipboard, arguments, NULL);
}

/**
 * gowl_mcp_register_clipboard_tools:
 * @server: the MCP server
 * @module: the MCP module
 *
 * Registers the clipboard-history tools each session's `tools:'
 * allowlist admits.
 */
void
gowl_mcp_register_clipboard_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	if (gowl_module_mcp_is_tool_allowed(module, "list_clipboard")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonNode) schema = clip_empty_schema();

		tool = mcp_tool_new("list_clipboard",
			"List the compositor's clipboard history, newest "
			"first: one line per entry with its id, MIME type, "
			"size and a preview. Use get_clipboard_entry for the "
			"whole of one entry. This is everything the human "
			"copied since the session started, which may include "
			"passwords and tokens they never meant to show you -- "
			"read it when the task needs it, not by habit.");
		mcp_tool_set_read_only_hint(tool, TRUE);
		mcp_tool_set_input_schema(tool, schema);
		mcp_server_add_tool(server, tool, handle_list_clipboard,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "get_clipboard_entry")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonNode) schema = clip_id_schema();

		tool = mcp_tool_new("get_clipboard_entry",
			"Return the filesystem path of one clipboard entry's "
			"stored contents. The file holds exactly what was "
			"copied, in the MIME type list_clipboard reports, so "
			"an image entry is the image. The path is stable "
			"until the entry is deleted or the history is "
			"cleared.");
		mcp_tool_set_read_only_hint(tool, TRUE);
		mcp_tool_set_input_schema(tool, schema);
		mcp_server_add_tool(server, tool, handle_get_clipboard_entry,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "copy_clipboard_entry")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonNode) schema = clip_id_schema();

		tool = mcp_tool_new("copy_clipboard_entry",
			"Put a history entry back on the clipboard, so the "
			"next paste in any window produces it. This replaces "
			"what the human currently has copied; their own "
			"selection is pushed into history first and is not "
			"lost.");
		mcp_tool_set_read_only_hint(tool, FALSE);
		mcp_tool_set_input_schema(tool, schema);
		mcp_server_add_tool(server, tool, handle_copy_clipboard_entry,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "delete_clipboard_entry")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonNode) schema = clip_id_schema();

		tool = mcp_tool_new("delete_clipboard_entry",
			"Remove one entry from the clipboard history and "
			"delete its stored contents. Use this to drop "
			"something that should not have been captured, such "
			"as a password a manager copied. It cannot be "
			"undone.");
		mcp_tool_set_read_only_hint(tool, FALSE);
		mcp_tool_set_destructive_hint(tool, TRUE);
		mcp_tool_set_input_schema(tool, schema);
		mcp_server_add_tool(server, tool, handle_delete_clipboard_entry,
		                    module, NULL);
	}

	if (gowl_module_mcp_is_tool_allowed(module, "clear_clipboard")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonNode) schema = clip_empty_schema();

		tool = mcp_tool_new("clear_clipboard",
			"Delete the whole clipboard history and every stored "
			"entry with it. The current selection is unaffected "
			"until the next copy. It cannot be undone.");
		mcp_tool_set_read_only_hint(tool, FALSE);
		mcp_tool_set_destructive_hint(tool, TRUE);
		mcp_tool_set_input_schema(tool, schema);
		mcp_server_add_tool(server, tool, handle_clear_clipboard,
		                    module, NULL);
	}
}
