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
 * gowl-mcp-tools-grab.c - Text and colours off the screen, over MCP.
 *
 * Tools: screen_text, pick_color
 *
 * The keybinds (screenshot-ocr, screenshot-color) are interactive: a
 * person drags or clicks.  An agent names the place instead, so these
 * take layout coordinates -- the ones list_clients and list_monitors
 * report -- and answer with the text or the colour.  Neither touches the
 * clipboard unless asked to: a program reading the screen should not
 * replace what the person copied.
 *
 * screen_text splits across threads deliberately.  The capture runs on
 * the compositor thread (it reads the scene), and is fast; tesseract
 * runs here, on the MCP thread, where taking a second costs nothing --
 * on the compositor thread it would freeze the desktop for that second.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-seat.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* What the compositor half hands the MCP half. */
typedef struct {
	gchar *path;       /* the region as a private PNG */
	gchar *command;    /* the OCR program, from the screenshot module */
	gchar *language;
	gchar *text;       /* for the clipboard half */
} GrabJob;

static McpToolResult *
error_result(const gchar *message)
{
	McpToolResult *result;

	result = mcp_tool_result_new(TRUE);
	mcp_tool_result_add_text(result, message);
	return result;
}

static gboolean
int_args(
	JsonObject  *arguments,
	const gchar *const *names,
	gint        *out
){
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		if (arguments == NULL || !json_object_has_member(arguments, names[i]))
			return FALSE;
		out[i] = (gint)json_object_get_int_member(arguments, names[i]);
	}
	return TRUE;
}

/*
 * ocr_config:
 *
 * The screenshot module's ocr-command and ocr-language, so the tool
 * and the keybind run the same program; tesseract/eng when the module
 * is not loaded.
 */
static void
ocr_config(
	GowlCompositor *compositor,
	GrabJob        *job
){
	g_autofree gchar *reply = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;

	job->command = g_strdup("tesseract");
	job->language = g_strdup("eng");
	reply = gowl_compositor_run_command(compositor, "screenshot-ocr-config");
	if (reply == NULL || !g_str_has_prefix(reply, "OK "))
		return;
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, reply + 3, -1, NULL))
		return;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return;
	g_free(job->command);
	job->command = g_strdup(json_object_get_string_member_with_default(
		json_node_get_object(root), "command", "tesseract"));
	g_free(job->language);
	job->language = g_strdup(json_object_get_string_member_with_default(
		json_node_get_object(root), "language", "eng"));
}

/* Compositor thread: capture the region into a private PNG. */
static McpToolResult *
tool_capture(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	static const gchar *const names[] = { "x", "y", "width", "height", NULL };
	GrabJob *job = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) data = NULL;
	gint v[4];
	gint out_w;
	gint out_h;
	gint fd;

	if (module == NULL || module->compositor == NULL)
		return error_result("Compositor not running");
	if (!int_args(arguments, names, v))
		return error_result("Missing required arguments: x, y, width, "
		                    "height");
	if (v[2] <= 0 || v[3] <= 0)
		return error_result("width and height must be positive");

	data = gowl_compositor_screenshot_region(module->compositor, NULL,
	           v[0], v[1], v[2], v[3], &out_w, &out_h, &error);
	if (data == NULL)
		return error_result(error != NULL ? error->message
		                                  : "Capture failed");

	/* $XDG_RUNTIME_DIR, 0600: a picture of the screen goes nowhere
	   anyone else can read, and is deleted after */
	job->path = g_build_filename(g_get_user_runtime_dir(),
	                             "gowl-mcp-ocr-XXXXXX.png", NULL);
	fd = g_mkstemp_full(job->path, O_RDWR, 0600);
	if (fd < 0) {
		g_autofree gchar *why = g_strdup_printf("Cannot create %s: %s",
			job->path, g_strerror(errno));

		g_clear_pointer(&job->path, g_free);
		return error_result(why);
	}
	close(fd);
	if (!gowl_compositor_save_png(data, out_w, out_h, job->path, &error)) {
		g_unlink(job->path);
		g_clear_pointer(&job->path, g_free);
		return error_result(error->message);
	}
	ocr_config(module->compositor, job);
	return mcp_tool_result_new(FALSE);
}

/* Compositor thread: put the text on the clipboard. */
static McpToolResult *
tool_copy(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GrabJob *job = user_data;
	GowlSeat *seat;

	(void)arguments;
	if (module == NULL || module->compositor == NULL)
		return error_result("Compositor not running");
	seat = gowl_compositor_get_seat(module->compositor);
	if (seat != NULL)
		gowl_seat_set_clipboard(seat, job->text);
	return mcp_tool_result_new(FALSE);
}

static McpToolResult *
handle_screen_text(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	GowlModuleMcp *module = user_data;
	GrabJob job = { NULL, NULL, NULL, NULL };
	g_autoptr(GPtrArray) argv = NULL;
	g_auto(GStrv) command = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	McpToolResult *result;
	const gchar *language;
	gboolean copy;
	gint status = 0;
	guint i;

	(void)server;
	(void)name;
	result = gowl_mcp_dispatch_call(module, tool_capture, arguments, &job);
	if (job.path == NULL)
		return result;
	mcp_tool_result_unref(result);

	language = arguments != NULL
		? json_object_get_string_member_with_default(arguments, "language",
		                                             NULL)
		: NULL;
	if (language == NULL || *language == '\0')
		language = job.language;

	/* MCP thread from here: tesseract may take its time */
	if (!g_shell_parse_argv(job.command, NULL, &command, &error)) {
		result = error_result(error->message);
		goto out;
	}
	argv = g_ptr_array_new();
	for (i = 0; command[i] != NULL; i++)
		g_ptr_array_add(argv, command[i]);
	g_ptr_array_add(argv, job.path);
	g_ptr_array_add(argv, (gpointer)"stdout");
	g_ptr_array_add(argv, (gpointer)"-l");
	g_ptr_array_add(argv, (gpointer)language);
	g_ptr_array_add(argv, NULL);
	if (!g_spawn_sync(NULL, (gchar **)argv->pdata, NULL,
	                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL,
	                  NULL, NULL, &out, &err, &status, &error)) {
		g_autofree gchar *why = g_strdup_printf("%s -- is tesseract "
			"installed? (Fedora: tesseract, tesseract-langpack-eng)",
			error->message);

		result = error_result(why);
		goto out;
	}
	if (!g_spawn_check_wait_status(status, &error)) {
		g_autofree gchar *why = g_strdup_printf("%s: %s", error->message,
			err != NULL ? g_strstrip(err) : "");

		result = error_result(why);
		goto out;
	}

	g_strdelimit(out, "\f", '\n');
	g_strstrip(out);
	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, *out != '\0' ? out : "");

	copy = arguments != NULL
		&& json_object_has_member(arguments, "copy")
		&& json_object_get_boolean_member(arguments, "copy");
	if (copy && *out != '\0') {
		McpToolResult *r;

		job.text = out;
		r = gowl_mcp_dispatch_call(module, tool_copy, arguments, &job);
		mcp_tool_result_unref(r);
		job.text = NULL;
	}

out:
	g_unlink(job.path);
	g_free(job.path);
	g_free(job.command);
	g_free(job.language);
	return result;
}

/* Compositor thread: one pixel, as #rrggbb. */
static McpToolResult *
tool_pick_color(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	static const gchar *const names[] = { "x", "y", NULL };
	g_autoptr(GBytes) data = NULL;
	g_autofree gchar *json = NULL;
	const guint8 *px;
	McpToolResult *result;
	gsize len = 0;
	gint v[2];
	gint w;
	gint h;

	(void)user_data;
	if (module == NULL || module->compositor == NULL)
		return error_result("Compositor not running");
	if (!int_args(arguments, names, v))
		return error_result("Missing required arguments: x, y");
	data = gowl_compositor_screenshot_region(module->compositor, NULL,
	           v[0], v[1], 1, 1, &w, &h, NULL);
	px = data != NULL ? g_bytes_get_data(data, &len) : NULL;
	if (px == NULL || len < 4)
		return error_result("Could not read that pixel (is it on an "
		                    "output?)");
	/* ARGB8888 little-endian: B, G, R, A in memory */
	json = g_strdup_printf("{\"color\":\"#%02x%02x%02x\",\"r\":%u,"
	                       "\"g\":%u,\"b\":%u}", px[2], px[1], px[0],
	                       px[2], px[1], px[0]);
	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json);
	return result;
}

static McpToolResult *
handle_pick_color(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	(void)server;
	(void)name;
	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_pick_color, arguments, NULL);
}

static void
add(
	McpServer      *server,
	GowlModuleMcp  *module,
	const gchar    *name,
	const gchar    *description,
	const gchar    *schema,
	gboolean        read_only,
	McpToolHandler  handler
){
	g_autoptr(McpTool) tool = NULL;
	g_autoptr(JsonNode) node = NULL;

	if (!gowl_module_mcp_is_tool_allowed(module, name))
		return;
	node = json_from_string(schema, NULL);
	g_return_if_fail(node != NULL);
	tool = mcp_tool_new(name, description);
	mcp_tool_set_read_only_hint(tool, read_only);
	mcp_tool_set_input_schema(tool, node);
	mcp_server_add_tool(server, tool, handler, module, NULL);
}

/**
 * gowl_mcp_register_grab_tools:
 * @server: the #McpServer to register tools on
 * @module: the MCP module instance
 *
 * Registers screen_text and pick_color.
 */
void
gowl_mcp_register_grab_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	add(server, module, "screen_text",
		"Read the text in a region of the screen (OCR, with tesseract -- "
		"the screenshot module's ocr-command). Layout coordinates, as "
		"list_clients and list_monitors report them. Returns the text, "
		"empty when there is none. The clipboard is left alone unless "
		"copy is true. Needs tesseract and a language pack installed.",
		"{\"type\":\"object\",\"properties\":{"
		"\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},"
		"\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},"
		"\"language\":{\"type\":\"string\",\"description\":\"tesseract "
		"languages, e.g. eng or eng+deu (default: the screenshot "
		"module's ocr-language)\"},"
		"\"copy\":{\"type\":\"boolean\",\"description\":\"Also put the "
		"text on the clipboard (default false)\"}},"
		"\"required\":[\"x\",\"y\",\"width\",\"height\"]}",
		FALSE, handle_screen_text);
	add(server, module, "pick_color",
		"The colour of one pixel of the screen, at layout coordinates: "
		"{\"color\":\"#rrggbb\",\"r\":..,\"g\":..,\"b\":..}. The clipboard "
		"is left alone.",
		"{\"type\":\"object\",\"properties\":{"
		"\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}},"
		"\"required\":[\"x\",\"y\"]}",
		TRUE, handle_pick_color);
}
