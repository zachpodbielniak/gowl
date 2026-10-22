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
 * portal-gowl.c - the compositor's wire protocol, from the portal.
 *
 * One request per line, one JSON-RPC response per request, events as
 * notifications that may arrive before the response they belong to.
 * The reader therefore reads LINES until it sees its own id, keeping
 * every event it passes in a queue for portal_gowl_wait_event_text().
 */

#include "portal-gowl.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>

/* How long one ordinary request may take.  The compositor answers in
 * microseconds; anything near this is a compositor that is wedged, and
 * a portal call is better failing than hanging with it. */
#define PORTAL_GOWL_CALL_TIMEOUT_S (5)

struct _PortalGowl {
	GSocketConnection *conn;
	GDataInputStream  *in;
	GOutputStream     *out;
	gint64             next_id;
	GQueue             events;    /* gchar *: text lines of events seen */
};

gchar *
portal_gowl_socket_path(void)
{
	const gchar *env = g_getenv("GOWL_SOCKET");

	if (env != NULL && *env != '\0')
		return g_strdup(env);
	return g_build_filename(g_get_user_runtime_dir(), "gowl.sock", NULL);
}

PortalGowl *
portal_gowl_connect(GError **error)
{
	g_autofree gchar *path = portal_gowl_socket_path();
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketAddress) addr = NULL;
	GSocketConnection *conn;
	PortalGowl *self;

	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "no gowl socket at %s", path);
		return NULL;
	}
	client = g_socket_client_new();
	g_socket_client_set_timeout(client, PORTAL_GOWL_CALL_TIMEOUT_S);
	addr = g_unix_socket_address_new(path);
	conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL,
	                               error);
	if (conn == NULL)
		return NULL;

	self = g_new0(PortalGowl, 1);
	self->conn = conn;
	self->in = g_data_input_stream_new(
		g_io_stream_get_input_stream(G_IO_STREAM(conn)));
	g_data_input_stream_set_newline_type(self->in,
	                                     G_DATA_STREAM_NEWLINE_TYPE_LF);
	self->out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
	self->next_id = 1;
	g_queue_init(&self->events);
	return self;
}

void
portal_gowl_free(PortalGowl *self)
{
	if (self == NULL)
		return;
	g_queue_clear_full(&self->events, g_free);
	g_clear_object(&self->in);
	if (self->conn != NULL)
		g_io_stream_close(G_IO_STREAM(self->conn), NULL, NULL);
	g_clear_object(&self->conn);
	g_free(self);
}

/* The socket's read timeout, in seconds; 0 is none. */
static void
set_timeout(PortalGowl *self, guint seconds)
{
	g_socket_set_timeout(g_socket_connection_get_socket(self->conn),
	                     seconds);
}

/*
 * One line from the compositor, parsed.  An event is queued (as its
 * text form) and reported as such so the caller keeps reading.
 */
static JsonObject *
read_message(PortalGowl *self, gboolean *was_event, GError **error)
{
	g_autofree gchar *line = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonObject *obj;

	*was_event = FALSE;
	line = g_data_input_stream_read_line_utf8(self->in, NULL, NULL, error);
	if (line == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
			                    "the compositor closed the connection");
		return NULL;
	}
	if (!json_parser_load_from_data(parser, line, -1, error))
		return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                    "the compositor sent something not an object");
		return NULL;
	}
	obj = json_node_get_object(root);
	if (g_strcmp0(json_object_get_string_member_with_default(obj, "method",
	                                                         ""),
	              "event") == 0) {
		JsonObject *params = json_object_get_object_member(obj, "params");
		const gchar *text = params != NULL
			? json_object_get_string_member_with_default(params, "text",
			                                             NULL)
			: NULL;

		if (text != NULL)
			g_queue_push_tail(&self->events, g_strdup(text));
		*was_event = TRUE;
	}
	return json_object_ref(obj);
}

JsonNode *
portal_gowl_call(PortalGowl *self, const gchar *method, JsonObject *params,
                 GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) request = NULL;
	g_autofree gchar *text = NULL;
	gint64 id;

	g_return_val_if_fail(self != NULL, NULL);

	id = self->next_id++;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, id);
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, method);
	if (params != NULL) {
		JsonNode *p = json_node_new(JSON_NODE_OBJECT);

		json_node_take_object(p, params);
		json_builder_set_member_name(builder, "params");
		json_builder_add_value(builder, p);
	}
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	json_generator_set_root(gen, request);
	text = json_generator_to_data(gen, NULL);

	set_timeout(self, PORTAL_GOWL_CALL_TIMEOUT_S);
	if (!g_output_stream_write_all(self->out, text, strlen(text), NULL,
	                               NULL, error)
	    || !g_output_stream_write_all(self->out, "\n", 1, NULL, NULL,
	                                  error))
		return NULL;

	for (;;) {
		gboolean was_event = FALSE;
		g_autoptr(JsonObject) msg = read_message(self, &was_event, error);
		JsonNode *got;

		if (msg == NULL)
			return NULL;
		if (was_event)
			continue;
		got = json_object_get_member(msg, "id");
		if (got == NULL || json_node_get_value_type(got) != G_TYPE_INT64
		    || json_node_get_int(got) != id)
			continue;
		if (json_object_has_member(msg, "error")) {
			JsonObject *err = json_object_get_object_member(msg, "error");

			g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"%s: %s", method,
				err != NULL
				? json_object_get_string_member_with_default(err,
				        "message", "error")
				: "error");
			return NULL;
		}
		got = json_object_get_member(msg, "result");
		return got != NULL ? json_node_copy(got) : json_node_new(JSON_NODE_NULL);
	}
}

gchar *
portal_gowl_command(PortalGowl *self, const gchar *line, GError **error)
{
	JsonObject *params = json_object_new();
	g_autoptr(JsonNode) result = NULL;
	JsonObject *obj;
	const gchar *reply;

	json_object_set_string_member(params, "line", line);
	result = portal_gowl_call(self, "gowl.command", params, error);
	if (result == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_OBJECT(result)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                    "gowl.command answered something unexpected");
		return NULL;
	}
	obj = json_node_get_object(result);
	reply = json_object_get_string_member_with_default(obj, "reply", "");
	if (!json_object_get_boolean_member_with_default(obj, "ok", FALSE)
	    || g_str_has_prefix(reply, "ERROR")) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
		            *reply != '\0' ? reply : "the command failed");
		return NULL;
	}
	return g_strdup(reply);
}

gchar *
portal_gowl_wait_event_text(PortalGowl *self, const gchar *prefix,
                            guint timeout_ms, GError **error)
{
	gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

	g_return_val_if_fail(self != NULL, NULL);

	for (;;) {
		GList *l;
		gint64 left;
		gboolean was_event = FALSE;
		g_autoptr(JsonObject) msg = NULL;

		/* Queued first: the answer may have come in with a reply. */
		for (l = self->events.head; l != NULL; l = l->next) {
			if (g_str_has_prefix(l->data, prefix)) {
				gchar *hit = l->data;

				g_queue_delete_link(&self->events, l);
				return hit;
			}
		}
		left = (end - g_get_monotonic_time()) / 1000000;
		if (left <= 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			            "no `%s' event in %u ms", prefix, timeout_ms);
			return NULL;
		}
		set_timeout(self, (guint)left);
		msg = read_message(self, &was_event, error);
		if (msg == NULL)
			return NULL;
	}
}
