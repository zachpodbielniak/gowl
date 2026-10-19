/*
 * gowl-msg - talk to a running gowl over its IPC socket
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
 * gowl-msg CMD [ARGS...]              text dialect: one line, one reply
 * gowl-msg -s                         text events until killed
 * gowl-msg -c METHOD [K=V...|JSON]    the wire protocol: one JSON-RPC call
 * gowl-msg -s -j [PATTERN...]         structured events, one JSON per line
 * gowl-msg -d [METHOD]                what the running compositor offers
 * gowl-msg --pipe                     stdin lines in, every reply out
 *
 * The socket is --socket, else $GOWL_SOCKET, else
 * $XDG_RUNTIME_DIR/gowl.sock.  Exit status: 0 success, 1 the compositor
 * answered with an error, 2 a usage error, 3 nothing answered.
 *
 * docs/ipc.org is the reference for both dialects.
 */

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gunixinputstream.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#define GOWL_MSG_EXIT_OK      (0)
#define GOWL_MSG_EXIT_ERROR   (1)
#define GOWL_MSG_EXIT_USAGE   (2)
#define GOWL_MSG_EXIT_CONNECT (3)

static const gchar *license_text =
	"gowl-msg  Copyright (C) 2026  Zach Podbielniak\n"
	"This program comes with ABSOLUTELY NO WARRANTY.  This is free software,\n"
	"and you are welcome to redistribute it under the terms of the GNU Affero\n"
	"General Public License, version 3 or later.\n";

/* --socket, else $GOWL_SOCKET, else the default every gowl listens on. */
static gchar *
socket_path_for(const gchar *given)
{
	const gchar *env = g_getenv("GOWL_SOCKET");

	if (given != NULL)
		return g_strdup(given);
	if (env != NULL && *env != '\0')
		return g_strdup(env);
	return g_strdup_printf("%s/gowl.sock", g_get_user_runtime_dir());
}

static GSocketConnection *
connect_to(const gchar *path, GError **error)
{
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(path);

	return g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL,
	                               error);
}

static gboolean
send_line(GOutputStream *out, const gchar *text, GError **error)
{
	g_autofree gchar *line = g_strdup_printf("%s\n", text);

	return g_output_stream_write_all(out, line, strlen(line), NULL, NULL,
	                                 error);
}

/*
 * One K=V argument's value: JSON if it reads as JSON (12, true, null,
 * [1,2], {"a":1}, "quoted"), otherwise the text itself as a string -- so
 * `id=12' is a number, `id=focused' a string, and `title="12"' forces one.
 */
static JsonNode *
value_from_text(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new_immutable();
	JsonNode *n;

	if (json_parser_load_from_data(parser, text, -1, NULL)
	    && json_parser_get_root(parser) != NULL)
		return json_node_copy(json_parser_get_root(parser));
	n = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(n, text);
	return n;
}

/*
 * The params of a --call: one JSON object argument, or K=V pairs, or
 * both (pairs override the object's members).
 */
static JsonObject *
params_from_args(gint argc, gchar **argv, GError **error)
{
	JsonObject *params = json_object_new();
	gint i;

	for (i = 0; i < argc; i++) {
		const gchar *arg = argv[i];
		const gchar *eq;

		if (arg[0] == '{') {
			g_autoptr(JsonParser) parser = json_parser_new_immutable();
			JsonObjectIter iter;
			const gchar *k;
			JsonNode *v;

			if (!json_parser_load_from_data(parser, arg, -1, error)) {
				json_object_unref(params);
				return NULL;
			}
			if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				            "params must be a JSON object");
				json_object_unref(params);
				return NULL;
			}
			json_object_iter_init(&iter,
				json_node_get_object(json_parser_get_root(parser)));
			while (json_object_iter_next(&iter, &k, &v))
				json_object_set_member(params, k, json_node_copy(v));
			continue;
		}

		eq = strchr(arg, '=');
		if (eq == NULL || eq == arg) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			            "expected KEY=VALUE or a JSON object, not '%s'", arg);
			json_object_unref(params);
			return NULL;
		}
		{
			g_autofree gchar *key = g_strndup(arg, (gsize)(eq - arg));

			json_object_set_member(params, key, value_from_text(eq + 1));
		}
	}
	return params;
}

static gchar *
node_to_text(JsonNode *node, gboolean pretty)
{
	return json_to_string(node, pretty);
}

/*
 * Sends one request and returns its response object, reading past any
 * event notification that arrives first on the same connection.
 */
static JsonObject *
rpc_call(GOutputStream *out, GDataInputStream *in, const gchar *method,
         JsonObject *params, gint64 id, GError **error)
{
	g_autoptr(JsonNode) req = json_node_new(JSON_NODE_OBJECT);
	JsonObject *o = json_object_new();
	g_autofree gchar *text = NULL;

	json_object_set_string_member(o, "jsonrpc", "2.0");
	json_object_set_int_member(o, "id", id);
	json_object_set_string_member(o, "method", method);
	if (params != NULL)
		json_object_set_object_member(o, "params", json_object_ref(params));
	json_node_take_object(req, o);
	text = json_to_string(req, FALSE);
	if (!send_line(out, text, error))
		return NULL;

	for (;;) {
		g_autofree gchar *line = g_data_input_stream_read_line_utf8(
			in, NULL, NULL, error);
		g_autoptr(JsonParser) parser = NULL;
		JsonObject *resp;

		if (line == NULL) {
			if (error != NULL && *error == NULL)
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
				            "the compositor closed the connection");
			return NULL;
		}
		parser = json_parser_new_immutable();
		if (!json_parser_load_from_data(parser, line, -1, error))
			return NULL;
		if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
			continue;
		resp = json_node_get_object(json_parser_get_root(parser));
		if (!json_object_has_member(resp, "id"))
			continue;   /* an event notification */
		return json_object_ref(resp);
	}
}

/* Prints a response; returns the exit status it means. */
static gint
print_response(JsonObject *resp, gboolean pretty, gboolean raw)
{
	if (raw) {
		g_autoptr(JsonNode) n = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *text = NULL;

		json_node_set_object(n, resp);
		text = node_to_text(n, pretty);
		g_print("%s\n", text);
		return json_object_has_member(resp, "error")
			? GOWL_MSG_EXIT_ERROR : GOWL_MSG_EXIT_OK;
	}
	if (json_object_has_member(resp, "error")) {
		JsonObject *err = json_object_get_object_member(resp, "error");

		g_printerr("gowl-msg: %s (%" G_GINT64_FORMAT ")\n",
		           json_object_get_string_member_with_default(
		               err, "message", "error"),
		           json_object_get_int_member_with_default(err, "code", 0));
		return GOWL_MSG_EXIT_ERROR;
	}
	{
		g_autofree gchar *text = node_to_text(
			json_object_get_member(resp, "result"), pretty);

		g_print("%s\n", text);
	}
	return GOWL_MSG_EXIT_OK;
}

/* --discover: the method list, readable by a person. */
static gint
print_discover(JsonObject *result, const gchar *only)
{
	JsonArray *methods = json_object_get_array_member(result, "methods");
	JsonArray *events = json_object_get_array_member(result, "events");
	guint i, j;

	if (only == NULL)
		g_print("%s v%" G_GINT64_FORMAT " -- %s (you are pid %"
		        G_GINT64_FORMAT ")\n\nMethods:\n",
		        json_object_get_string_member(result, "protocol"),
		        json_object_get_int_member(result, "version"),
		        json_object_get_string_member(result, "server"),
		        json_object_get_int_member(
		            json_object_get_object_member(result, "caller"), "pid"));

	for (i = 0; i < json_array_get_length(methods); i++) {
		JsonObject *m = json_array_get_object_element(methods, i);
		JsonArray *params = json_object_get_array_member(m, "params");

		if (only == NULL) {
			g_print("  %-24s %s\n", json_object_get_string_member(m, "name"),
			        json_object_get_string_member(m, "summary"));
			continue;
		}
		g_print("%s\n  %s\n", json_object_get_string_member(m, "name"),
		        json_object_get_string_member(m, "summary"));
		g_print("\n  Parameters:\n");
		if (json_array_get_length(params) == 0)
			g_print("    (none)\n");
		for (j = 0; j < json_array_get_length(params); j++) {
			JsonObject *p = json_array_get_object_element(params, j);

			g_print("    %-14s %-8s %s%s\n",
			        json_object_get_string_member(p, "name"),
			        json_object_get_string_member(p, "type"),
			        json_object_get_boolean_member(p, "required")
			        ? "(required) " : "",
			        json_object_get_string_member(p, "doc"));
		}
		g_print("\n  Returns: %s\n",
		        json_object_get_string_member(m, "result"));
	}

	if (only == NULL && events != NULL) {
		g_print("\nEvents (gowl-msg -s -j PATTERN...):\n");
		for (i = 0; i < json_array_get_length(events); i++) {
			JsonObject *e = json_array_get_object_element(events, i);

			g_print("  %-18s %s\n", json_object_get_string_member(e, "name"),
			        json_object_get_string_member(e, "doc"));
		}
	}
	return GOWL_MSG_EXIT_OK;
}

/* --pipe: copies the socket to stdout on its own thread. */
static gpointer
pump_socket_to_stdout(gpointer data)
{
	GDataInputStream *in = (GDataInputStream *)data;

	for (;;) {
		g_autofree gchar *line = g_data_input_stream_read_line_utf8(
			in, NULL, NULL, NULL);

		if (line == NULL)
			break;
		g_print("%s\n", line);
		fflush(stdout);
	}
	return NULL;
}

/*
 * --pipe: every stdin line to the socket, every socket line to stdout,
 * in either dialect, until stdin ends and the compositor has answered
 * what it was sent.  For a program with no socket library of its own:
 * `printf '%s\n' "$req" | gowl-msg --pipe'.
 */
static gint
run_pipe(GSocketConnection *conn, GOutputStream *out, GDataInputStream *in)
{
	g_autoptr(GDataInputStream) sin = NULL;
	g_autoptr(GInputStream) raw_in = NULL;
	g_autoptr(GError) error = NULL;
	GThread *reader;

	raw_in = g_unix_input_stream_new(0, FALSE);
	sin = g_data_input_stream_new(raw_in);
	reader = g_thread_new("gowl-msg-reader", pump_socket_to_stdout, in);

	for (;;) {
		g_autofree gchar *line = g_data_input_stream_read_line_utf8(
			sin, NULL, NULL, &error);

		if (line == NULL)
			break;
		if (*line == '\0')
			continue;
		if (!send_line(out, line, &error))
			break;
	}
	/* No more requests: the compositor finishes sending what it owes
	 * and closes, which ends the reader. */
	g_socket_shutdown(g_socket_connection_get_socket(conn), FALSE, TRUE, NULL);
	g_thread_join(reader);
	if (error != NULL) {
		g_printerr("gowl-msg: %s\n", error->message);
		return GOWL_MSG_EXIT_CONNECT;
	}
	return GOWL_MSG_EXIT_OK;
}

int
main(int argc, char *argv[])
{
	gboolean subscribe = FALSE;
	gboolean json = FALSE;
	gboolean call = FALSE;
	gboolean discover = FALSE;
	gboolean pipe_mode = FALSE;
	gboolean pretty = FALSE;
	gboolean raw = FALSE;
	gboolean show_license = FALSE;
	gchar *socket_arg = NULL;
	GOptionEntry entries[] = {
		{ "call", 'c', 0, G_OPTION_ARG_NONE, &call,
		  "Call a wire-protocol METHOD with K=V or JSON params", NULL },
		{ "subscribe", 's', 0, G_OPTION_ARG_NONE, &subscribe,
		  "Print events until killed (with -j: structured, filtered)", NULL },
		{ "json", 'j', 0, G_OPTION_ARG_NONE, &json,
		  "With -s: JSON events; arguments are name patterns", NULL },
		{ "discover", 'd', 0, G_OPTION_ARG_NONE, &discover,
		  "List the methods and events, or describe one METHOD", NULL },
		{ "pipe", 0, 0, G_OPTION_ARG_NONE, &pipe_mode,
		  "Send stdin lines, print every reply line", NULL },
		{ "pretty", 'p', 0, G_OPTION_ARG_NONE, &pretty,
		  "Indent JSON output", NULL },
		{ "raw", 'r', 0, G_OPTION_ARG_NONE, &raw,
		  "With -c: print the whole JSON-RPC response", NULL },
		{ "socket", 'S', 0, G_OPTION_ARG_FILENAME, &socket_arg,
		  "Socket path (default $GOWL_SOCKET, else "
		  "$XDG_RUNTIME_DIR/gowl.sock)", "PATH" },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &show_license,
		  "Show license information", NULL },
		{ NULL }
	};
	g_autoptr(GOptionContext) ctx = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketConnection) conn = NULL;
	g_autoptr(GDataInputStream) in = NULL;
	g_autofree gchar *socket_path = NULL;
	GOutputStream *out;
	gint ret = GOWL_MSG_EXIT_OK;

	ctx = g_option_context_new("[COMMAND [ARGS...]] - talk to gowl");
	g_option_context_add_main_entries(ctx, entries, NULL);
	g_option_context_set_description(ctx,
		"Text commands (one line in, one line back):\n"
		"  gowl-msg clients                 every window, as JSON\n"
		"  gowl-msg focused | jq .app_id    the focused window's app id\n"
		"  gowl-msg view 4                  view tag 3 (a bitmask)\n"
		"  gowl-msg dispatch Super+Return   run what a key is bound to\n"
		"  gowl-msg action tag-view 2       run an action with an argument\n"
		"  gowl-msg expo                    a module's command\n"
		"  gowl-msg -s                      stream text events\n"
		"  gowl-msg help                    the text command list\n"
		"\n"
		"The wire protocol (JSON-RPC 2.0, for programs):\n"
		"  gowl-msg -d                      every method and event\n"
		"  gowl-msg -d window.set_geometry  one method's parameters\n"
		"  gowl-msg -c windows.list mine=true\n"
		"  gowl-msg -c window.set_geometry id=12 x=0 y=0 width=0.5 \\\n"
		"           height=1 relative_to=area units=fraction\n"
		"  gowl-msg -c layout.apply '{\"windows\":[{\"id\":3,\"x\":0,\"y\":0,"
		"\"width\":960,\"height\":1080}]}'\n"
		"  gowl-msg -c spawn command=foot tag=2 floating=true width=800 "
		"height=500\n"
		"  gowl-msg -c tags.view tag=3\n"
		"  gowl-msg -s -j 'window.*' tags.changed   structured events\n"
		"  printf '%s\\n' '{\"id\":1,\"method\":\"gowl.state\"}' | "
		"gowl-msg --pipe\n"
		"\n"
		"Exit status: 0 ok, 1 the compositor answered an error, 2 usage,\n"
		"3 nothing answered.  The reference is docs/ipc.org.\n");
	if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
		g_printerr("gowl-msg: %s\n", error->message);
		return GOWL_MSG_EXIT_USAGE;
	}
	if (show_license) {
		g_print("%s", license_text);
		return GOWL_MSG_EXIT_OK;
	}
	if ((gint)call + (gint)subscribe + (gint)discover + (gint)pipe_mode > 1) {
		g_printerr("gowl-msg: -c, -s, -d and --pipe are separate modes\n");
		return GOWL_MSG_EXIT_USAGE;
	}
	if (json && !subscribe) {
		g_printerr("gowl-msg: -j goes with -s (for calls, use -c)\n");
		return GOWL_MSG_EXIT_USAGE;
	}
	if ((call && argc < 2)
	    || (!call && !subscribe && !discover && !pipe_mode && argc < 2)) {
		g_autofree gchar *help = g_option_context_get_help(ctx, TRUE, NULL);

		g_print("%s", help);
		return GOWL_MSG_EXIT_USAGE;
	}

	socket_path = socket_path_for(socket_arg);
	g_free(socket_arg);
	conn = connect_to(socket_path, &error);
	if (conn == NULL) {
		g_printerr("gowl-msg: cannot connect to %s: %s\n", socket_path,
		           error->message);
		return GOWL_MSG_EXIT_CONNECT;
	}

	out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
	in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));

	if (pipe_mode)
		return run_pipe(conn, out, in);

	if (subscribe && !json) {
		if (!send_line(out, "subscribe", &error)) {
			g_printerr("gowl-msg: %s\n", error->message);
			return GOWL_MSG_EXIT_CONNECT;
		}
		for (;;) {
			g_autofree gchar *reply = g_data_input_stream_read_line_utf8(
				in, NULL, NULL, &error);

			if (reply == NULL)
				break;
			g_print("%s\n", reply);
			fflush(stdout);
		}
		return error != NULL ? GOWL_MSG_EXIT_CONNECT : GOWL_MSG_EXIT_OK;
	}

	if (subscribe) {
		/* Structured events: subscribe with the patterns given, then
		 * print each event's params object, one per line, for jq. */
		JsonObject *params = json_object_new();
		JsonArray *pats = json_array_new();
		g_autoptr(JsonObject) resp = NULL;
		gint i;

		for (i = 1; i < argc; i++)
			json_array_add_string_element(pats, argv[i]);
		json_object_set_array_member(params, "events", pats);
		resp = rpc_call(out, in, "events.subscribe", params, 1, &error);
		json_object_unref(params);
		if (resp == NULL) {
			g_printerr("gowl-msg: %s\n", error->message);
			return GOWL_MSG_EXIT_CONNECT;
		}
		if (json_object_has_member(resp, "error"))
			return print_response(resp, pretty, FALSE);

		for (;;) {
			g_autofree gchar *line = g_data_input_stream_read_line_utf8(
				in, NULL, NULL, &error);
			g_autoptr(JsonParser) parser = NULL;
			JsonObject *msg;

			if (line == NULL)
				break;
			parser = json_parser_new_immutable();
			if (!json_parser_load_from_data(parser, line, -1, NULL)
			    || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
				continue;
			msg = json_node_get_object(json_parser_get_root(parser));
			if (g_strcmp0(json_object_get_string_member_with_default(
			                  msg, "method", ""), "event") != 0)
				continue;
			{
				g_autofree gchar *text = node_to_text(
					json_object_get_member(msg, "params"), pretty);

				g_print("%s\n", text);
				fflush(stdout);
			}
		}
		return error != NULL ? GOWL_MSG_EXIT_CONNECT : GOWL_MSG_EXIT_OK;
	}

	if (discover || call) {
		const gchar *method = discover ? "rpc.discover" : argv[1];
		g_autoptr(JsonObject) resp = NULL;
		JsonObject *params;

		if (discover) {
			params = json_object_new();
			if (argc >= 2)
				json_object_set_string_member(params, "method", argv[1]);
		} else {
			params = params_from_args(argc - 2, argv + 2, &error);
			if (params == NULL) {
				g_printerr("gowl-msg: %s\n", error->message);
				return GOWL_MSG_EXIT_USAGE;
			}
		}
		resp = rpc_call(out, in, method, params, 1, &error);
		json_object_unref(params);
		if (resp == NULL) {
			g_printerr("gowl-msg: no reply: %s\n", error->message);
			return GOWL_MSG_EXIT_CONNECT;
		}
		if (discover && !raw && !json_object_has_member(resp, "error"))
			return print_discover(json_object_get_object_member(resp,
			                                                    "result"),
			                      argc >= 2 ? argv[1] : NULL);
		return print_response(resp, pretty, raw);
	}

	/* The text dialect: everything after the options is the command
	 * line, verbatim. */
	{
		g_autofree gchar *joined = g_strjoinv(" ", argv + 1);
		g_autofree gchar *reply = NULL;

		if (!send_line(out, joined, &error)) {
			g_printerr("gowl-msg: %s\n", error->message);
			return GOWL_MSG_EXIT_CONNECT;
		}
		reply = g_data_input_stream_read_line_utf8(in, NULL, NULL, &error);
		if (reply == NULL) {
			g_printerr("gowl-msg: no reply%s%s\n", error != NULL ? ": " : "",
			           error != NULL ? error->message : "");
			return GOWL_MSG_EXIT_CONNECT;
		}
		g_print("%s\n", reply);
		if (g_str_has_prefix(reply, "ERROR"))
			ret = GOWL_MSG_EXIT_ERROR;
	}
	return ret;
}
