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
 * gowl-mcp-tools-record.c - Input recording MCP tools.
 *
 * Implements:
 *   - start_recording  : begin a bounded recording, returns a token
 *   - drain_recording  : take everything buffered so far, keep recording
 *   - stop_recording   : stop and take the tail
 *   - recording_status : whether a recording is running, and its counters
 *
 * These are registered in their own category rather than beside
 * send_key and friends, so an operator's `tools:` allowlist can grant
 * input *injection* without granting input *capture*.  The compositor
 * enforces the same separation again with its own `input-recording`
 * config key, which is off by default -- an allowlist entry alone
 * cannot turn recording on.
 *
 * There is no push notification of a state change over MCP.  mcp-glib's
 * notify targets one McpServer, the module runs two different server
 * objects, and both live on the MCP thread while the recorder's signal
 * fires on the compositor thread; a cross-thread notify would be worse
 * than a poll.  Instead every payload -- drain, stop and status alike --
 * carries `active` and `stop_reason`, so a consumer that is already
 * draining learns that the recording ended, and why, on its next call.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-compositor.h"
#include "core/gowl-input-recorder.h"

#include <json-glib/json-glib.h>

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

/**
 * recorder_for:
 *
 * The compositor's recorder, or %NULL before startup has run.
 */
static GowlInputRecorder *
recorder_for(GowlModuleMcp *module)
{
	if (module == NULL || module->compositor == NULL)
		return NULL;

	return gowl_compositor_get_input_recorder(module->compositor);
}

/**
 * error_result:
 *
 * An error result carrying @message.
 */
static McpToolResult *
error_result(const gchar *message)
{
	McpToolResult *result;

	result = mcp_tool_result_new(TRUE);
	mcp_tool_result_add_text(result, message);
	return result;
}

/**
 * json_result:
 *
 * A success result carrying @json, which is consumed.
 */
static McpToolResult *
json_result(gchar *json)
{
	McpToolResult *result;

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, json);
	g_free(json);
	return result;
}

/**
 * token_arg:
 *
 * Reads the required "token" argument.
 */
static const gchar *
token_arg(JsonObject *arguments)
{
	if (arguments == NULL || !json_object_has_member(arguments, "token"))
		return NULL;

	return json_object_get_string_member(arguments, "token");
}

/**
 * uint_arg:
 *
 * Reads an optional non-negative integer argument, @fallback when
 * absent or out of range.  A negative value is a caller mistake, not a
 * request for "unbounded": there is no unbounded here.
 */
static guint
uint_arg(JsonObject *arguments, const gchar *name, guint fallback)
{
	gint64 val;

	if (arguments == NULL || !json_object_has_member(arguments, name))
		return fallback;

	val = json_object_get_int_member(arguments, name);
	if (val <= 0)
		return fallback;
	if (val > (gint64)G_MAXUINT)
		return G_MAXUINT;

	return (guint)val;
}

/* ========================================================================== */
/* Tool: start_recording                                                      */
/* ========================================================================== */

static McpToolResult *
tool_start_recording(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlInputRecorder  *rec;
	g_autoptr(GError)   error = NULL;
	g_autofree gchar   *token = NULL;

	(void)user_data;

	rec = recorder_for(module);
	if (rec == NULL)
		return error_result("Compositor not running");

	token = gowl_input_recorder_start(
		rec,
		uint_arg(arguments, "max_seconds", 0),
		uint_arg(arguments, "max_events", 0),
		&error);
	if (token == NULL)
		return error_result(error->message);

	/* The token is in the status payload, so the caller gets the handle
	 * and the guard's actual reach in one reply rather than having to
	 * ask for the second. */
	return json_result(gowl_input_recorder_status(rec));
}

static McpToolResult *
handle_start_recording(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	(void)server;
	(void)name;

	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_start_recording, arguments, NULL);
}

/* ========================================================================== */
/* Tool: drain_recording                                                      */
/* ========================================================================== */

static McpToolResult *
tool_drain_recording(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlInputRecorder *rec;
	g_autoptr(GError)  error = NULL;
	const gchar       *token;
	gchar             *json;

	(void)user_data;

	rec = recorder_for(module);
	if (rec == NULL)
		return error_result("Compositor not running");

	token = token_arg(arguments);
	if (token == NULL)
		return error_result("Missing required argument: token");

	json = gowl_input_recorder_drain(rec, token, &error);
	if (json == NULL)
		return error_result(error->message);

	return json_result(json);
}

static McpToolResult *
handle_drain_recording(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	(void)server;
	(void)name;

	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_drain_recording, arguments, NULL);
}

/* ========================================================================== */
/* Tool: stop_recording                                                       */
/* ========================================================================== */

static McpToolResult *
tool_stop_recording(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlInputRecorder *rec;
	g_autoptr(GError)  error = NULL;
	const gchar       *token;
	gchar             *json;

	(void)user_data;

	rec = recorder_for(module);
	if (rec == NULL)
		return error_result("Compositor not running");

	token = token_arg(arguments);
	if (token == NULL)
		return error_result("Missing required argument: token");

	json = gowl_input_recorder_stop(rec, token, &error);
	if (json == NULL)
		return error_result(error->message);

	return json_result(json);
}

static McpToolResult *
handle_stop_recording(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	(void)server;
	(void)name;

	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_stop_recording, arguments, NULL);
}

/* ========================================================================== */
/* Tool: recording_status                                                     */
/* ========================================================================== */

static McpToolResult *
tool_recording_status(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlInputRecorder *rec;

	(void)arguments;
	(void)user_data;

	rec = recorder_for(module);
	if (rec == NULL)
		return error_result("Compositor not running");

	return json_result(gowl_input_recorder_status(rec));
}

static McpToolResult *
handle_recording_status(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	(void)server;
	(void)name;

	return gowl_mcp_dispatch_call((GowlModuleMcp *)user_data,
	                              tool_recording_status, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

/**
 * add_uint_property:
 *
 * Emits one optional integer property into an in-progress schema.
 */
static void
add_uint_property(
	JsonBuilder *b,
	const gchar *name,
	const gchar *description
){
	json_builder_set_member_name(b, name);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "integer");
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b, description);
	json_builder_end_object(b);
}

/**
 * add_token_property:
 *
 * Emits the required "token" property into an in-progress schema.
 */
static void
add_token_property(JsonBuilder *b)
{
	json_builder_set_member_name(b, "token");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "string");
	json_builder_set_member_name(b, "description");
	json_builder_add_string_value(b,
		"The token returned by start_recording");
	json_builder_end_object(b);
}

void
gowl_mcp_register_record_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* start_recording */
	if (gowl_module_mcp_is_tool_allowed(module, "start_recording")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("start_recording",
			"Record real keyboard and pointer input so a human "
			"demonstration can be turned into a procedure. "
			"Returns a token for drain_recording and "
			"stop_recording. Bounded: the ring holds at most "
			"max_events and the recording stops itself after "
			"max_seconds. Requires the compositor's "
			"`input-recording` config key, which is separate "
			"from the send_* tools and off by default. While it "
			"runs the whole screen is framed in red. Capture is "
			"suppressed while the session is locked and while "
			"the focused window matches the deny list, but gowl "
			"cannot see a password field inside an ordinary "
			"window -- review a trace before storing or sharing "
			"it.");
		/* Not destructive -- it changes nothing -- but far from
		 * read-only, and marking it open-world is the closest the
		 * hint vocabulary gets to "this observes the human". */
		mcp_tool_set_read_only_hint(tool, FALSE);
		mcp_tool_set_open_world_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		add_uint_property(b, "max_seconds",
			"Stop automatically after this many seconds "
			"(default 120, maximum 3600)");
		add_uint_property(b, "max_events",
			"Ring size in events; older events are dropped and "
			"counted (default 4096, maximum 100000)");
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_start_recording,
		                    module, NULL);
	}

	/* drain_recording */
	if (gowl_module_mcp_is_tool_allowed(module, "drain_recording")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("drain_recording",
			"Take everything recorded since the last drain and "
			"leave the recording running. The reply carries "
			"`dropped` (since the last drain) and "
			"`dropped_total`: a demonstration that overflowed "
			"the ring is reported, never passed off as "
			"complete. It also carries `active` and "
			"`stop_reason`, so a caller polling this learns "
			"that the recording ended and why.");
		mcp_tool_set_read_only_hint(tool, FALSE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		add_token_property(b);
		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "token");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_drain_recording,
		                    module, NULL);
	}

	/* stop_recording */
	if (gowl_module_mcp_is_tool_allowed(module, "stop_recording")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("stop_recording",
			"Stop the recording and take the tail, in the same "
			"shape drain_recording returns. Stopping a "
			"recording that already stopped itself still "
			"returns its remaining events exactly once.");
		mcp_tool_set_read_only_hint(tool, FALSE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		add_token_property(b);
		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "token");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_stop_recording,
		                    module, NULL);
	}

	/* recording_status */
	if (gowl_module_mcp_is_tool_allowed(module, "recording_status")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("recording_status",
			"Whether a recording is running, its token, its "
			"limits and its counters, without consuming "
			"anything. Poll this to notice that a recording "
			"started, stopped, or was stopped from the keyboard "
			"with Super+Shift+Escape.");
		mcp_tool_set_read_only_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);
		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_recording_status,
		                    module, NULL);
	}

	g_debug("record tools registered");
}
