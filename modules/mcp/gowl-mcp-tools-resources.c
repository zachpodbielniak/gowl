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
 * gowl-mcp-tools-resources.c - The desktop as readable MCP resources.
 *
 * The query tools answer a question that was asked.  Resources are the
 * other half of MCP: a client lists them once and reads them whenever
 * it wants context, without a tool call and without deciding which
 * question to ask.  A `gowl://clients' attached to a conversation is
 * the window list as it is now, not as it was when somebody last
 * thought to look.
 *
 * Every URI here is backed by the same code the IPC socket answers
 * with (gowl_compositor_ipc_command), so a resource and `gowl-msg
 * clients' return the same JSON -- there is one implementation of
 * "what does the desktop look like", not three.
 *
 * Reads arrive on the MCP thread and are dispatched to the compositor
 * thread like every tool call; nothing here touches compositor state
 * directly.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-core-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* What a URI maps to, and what to say about it in a listing. */
typedef struct {
	const gchar *uri;
	const gchar *name;
	const gchar *command;
	const gchar *description;
} GowlMcpResource;

static const GowlMcpResource gowl_mcp_resources[] = {
	{ "gowl://clients", "Windows", "clients",
	  "Every open window as JSON: id, app_id, title, pid, tags, "
	  "monitor, geometry, and whether it is floating, fullscreen, "
	  "urgent, sticky or an X11 window. The window list as it is "
	  "right now." },
	{ "gowl://focused", "Focused window", "focused",
	  "The window with keyboard focus, in the same shape as an entry "
	  "of gowl://clients, or null when nothing is focused." },
	{ "gowl://monitors", "Outputs", "monitors",
	  "Every connected output: name, make, model, serial, position, "
	  "size, scale, which tags it views, its layout, and whether it "
	  "is enabled and powered on." },
	{ "gowl://layouts", "Layouts", "layouts",
	  "The layouts this session can select, by name -- what a "
	  "loaded layout module registered." },
	{ "gowl://keybinds", "Keybinds", "keybinds",
	  "Every bound key with its action, argument, description and key "
	  "mode. What the human can do from the keyboard, which is also "
	  "the vocabulary of the dispatch_key tool." },
	{ "gowl://profile", "Output profile", "profile",
	  "The output profile in force and the profiles defined: named "
	  "sets of outputs that apply when all of them are connected." },
};

/*
 * Runs the query on the compositor thread and hands back its text.
 * The command string rides in as the dispatch's user_data.
 */
static McpToolResult *
resource_query(GowlModuleMcp *module, JsonObject *arguments,
               gpointer user_data)
{
	const gchar *command = (const gchar *)user_data;
	g_autofree gchar *reply = NULL;
	McpToolResult *result;

	(void)arguments;

	if (module == NULL || module->compositor == NULL) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result, "the compositor is not running");
		return result;
	}
	reply = gowl_compositor_ipc_command(module->compositor, command);
	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, reply != NULL ? reply : "null");
	return result;
}

/* The first text block of a tool result, which is all these produce. */
static gchar *
result_text(McpToolResult *result)
{
	JsonArray *content;
	guint i;

	if (result == NULL)
		return NULL;
	content = mcp_tool_result_get_content(result);
	if (content == NULL)
		return NULL;
	for (i = 0; i < json_array_get_length(content); i++) {
		JsonObject *block = json_array_get_object_element(content, i);

		if (block != NULL && json_object_has_member(block, "text"))
			return g_strdup(json_object_get_string_member(block, "text"));
	}
	return NULL;
}

static GList *
handle_resource_read(McpServer *server, const gchar *uri, gpointer user_data)
{
	GowlModuleMcp *module = (GowlModuleMcp *)user_data;
	McpToolResult *result;
	g_autofree gchar *text = NULL;
	gsize i;
	const gchar *command = NULL;

	(void)server;

	for (i = 0; i < G_N_ELEMENTS(gowl_mcp_resources); i++) {
		if (g_strcmp0(gowl_mcp_resources[i].uri, uri) == 0) {
			command = gowl_mcp_resources[i].command;
			break;
		}
	}
	if (command == NULL)
		return NULL;

	result = gowl_mcp_dispatch_call(module, resource_query, NULL,
	                                (gpointer)command);
	text = result_text(result);
	g_clear_pointer(&result, mcp_tool_result_unref);
	if (text == NULL)
		return NULL;

	return g_list_append(NULL,
		mcp_resource_contents_new_text(uri, text, "application/json"));
}

/**
 * gowl_mcp_register_resources:
 * @server: the MCP server
 * @module: the MCP module
 *
 * Registers the desktop's state as readable resources.  The listing is
 * fixed: these are views of the running compositor, not files, so
 * there is nothing to discover at runtime.
 */
void
gowl_mcp_register_resources(
	McpServer     *server,
	GowlModuleMcp *module
){
	gsize i;

	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	for (i = 0; i < G_N_ELEMENTS(gowl_mcp_resources); i++) {
		g_autoptr(McpResource) resource = NULL;

		resource = mcp_resource_new(gowl_mcp_resources[i].uri,
		                            gowl_mcp_resources[i].name);
		mcp_resource_set_description(resource,
		                             gowl_mcp_resources[i].description);
		mcp_resource_set_mime_type(resource, "application/json");
		mcp_server_add_resource(server, resource, handle_resource_read,
		                        module, NULL);
	}
}
