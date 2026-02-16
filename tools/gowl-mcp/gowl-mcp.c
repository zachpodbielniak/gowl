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
 * gowl-mcp - MCP stdio-to-socket relay binary.
 *
 * This small binary bridges stdin/stdout (used by MCP clients like
 * Claude Code) to the gowl MCP module's Unix domain socket.
 *
 * Usage:
 *   gowl-mcp [--socket PATH]
 *
 * Default socket: $XDG_RUNTIME_DIR/gowl-mcp.sock
 * Override with:  $GOWL_MCP_SOCKET or --socket flag
 *
 * Data flow:
 *   stdin  --> socket (NDJSON lines)
 *   socket --> stdout (NDJSON lines)
 */

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *main_loop = NULL;

/**
 * on_stdin_line:
 * @stream: the stdin data input stream
 * @result: the async result
 * @user_data: the socket output stream
 *
 * Reads a line from stdin and writes it to the socket.
 * On EOF, quits the main loop.
 */
static void
on_stdin_line(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GDataInputStream *stream;
	GOutputStream    *sock_out;
	g_autoptr(GError) error = NULL;
	gchar            *line;
	gsize             length;
	g_autofree gchar *msg = NULL;

	stream   = G_DATA_INPUT_STREAM(source);
	sock_out = G_OUTPUT_STREAM(user_data);

	line = g_data_input_stream_read_line_finish(stream, result,
	                                            &length, &error);
	if (line == NULL) {
		/* EOF or error -- shut down */
		if (error != NULL)
			g_printerr("gowl-mcp: stdin read error: %s\n",
			           error->message);
		g_main_loop_quit(main_loop);
		return;
	}

	/* forward the line to the socket with a newline terminator */
	msg = g_strdup_printf("%s\n", line);
	g_free(line);

	if (!g_output_stream_write_all(sock_out, msg, strlen(msg),
	                               NULL, NULL, &error)) {
		g_printerr("gowl-mcp: socket write error: %s\n",
		           error->message);
		g_main_loop_quit(main_loop);
		return;
	}

	/* continue reading */
	g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT,
	                                    NULL, on_stdin_line, sock_out);
}

/**
 * on_socket_line:
 * @stream: the socket data input stream
 * @result: the async result
 * @user_data: the stdout output stream
 *
 * Reads a line from the socket and writes it to stdout.
 * On disconnect, quits the main loop.
 */
static void
on_socket_line(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GDataInputStream *stream;
	g_autoptr(GError) error = NULL;
	gchar            *line;
	gsize             length;
	g_autofree gchar *msg = NULL;

	stream = G_DATA_INPUT_STREAM(source);

	line = g_data_input_stream_read_line_finish(stream, result,
	                                            &length, &error);
	if (line == NULL) {
		if (error != NULL)
			g_printerr("gowl-mcp: socket read error: %s\n",
			           error->message);
		g_main_loop_quit(main_loop);
		return;
	}

	/* forward the line to stdout with a newline */
	msg = g_strdup_printf("%s\n", line);
	g_free(line);

	if (!g_output_stream_write_all(
		G_OUTPUT_STREAM(user_data),
		msg, strlen(msg), NULL, NULL, &error))
	{
		g_printerr("gowl-mcp: stdout write error: %s\n",
		           error->message);
		g_main_loop_quit(main_loop);
		return;
	}

	/* continue reading */
	g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT,
	                                    NULL, on_socket_line, user_data);
}

int
main(int argc, char **argv)
{
	const gchar          *socket_path;
	g_autoptr(GError)     error = NULL;
	g_autoptr(GSocketClient)    client = NULL;
	g_autoptr(GSocketConnection) conn  = NULL;
	g_autoptr(GSocketAddress)    address = NULL;
	GInputStream         *sock_in;
	GOutputStream        *sock_out;
	g_autoptr(GInputStream) stdin_stream = NULL;
	g_autoptr(GOutputStream) stdout_stream = NULL;
	g_autoptr(GDataInputStream) stdin_data = NULL;
	g_autoptr(GDataInputStream) sock_data  = NULL;

	/* determine socket path */
	socket_path = g_getenv("GOWL_MCP_SOCKET");

	/* check for --socket flag */
	if (argc >= 3 && strcmp(argv[1], "--socket") == 0)
		socket_path = argv[2];

	if (socket_path == NULL) {
		const gchar *runtime_dir;

		runtime_dir = g_get_user_runtime_dir();
		socket_path = g_build_filename(runtime_dir,
		                               "gowl-mcp.sock", NULL);
	}

	/* help / license flags */
	if (argc >= 2 && (strcmp(argv[1], "-h") == 0 ||
	                  strcmp(argv[1], "--help") == 0)) {
		g_print("Usage: gowl-mcp [--socket PATH]\n\n"
		        "MCP stdio-to-socket relay for the gowl compositor.\n\n"
		        "Options:\n"
		        "  --socket PATH   Unix socket path\n"
		        "                  (default: $XDG_RUNTIME_DIR/gowl-mcp.sock)\n"
		        "  -h, --help      Show this help\n"
		        "  --license       Show license information\n\n"
		        "Environment:\n"
		        "  GOWL_MCP_SOCKET  Override socket path\n\n"
		        "Examples:\n"
		        "  gowl-mcp\n"
		        "  gowl-mcp --socket /tmp/my-gowl.sock\n"
		        "  GOWL_MCP_SOCKET=/tmp/test.sock gowl-mcp\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "--license") == 0) {
		g_print("gowl-mcp - part of gowl, the GObject Wayland Compositor\n"
		        "Copyright (C) 2026  Zach Podbielniak\n"
		        "License: GNU AGPL v3 or later\n"
		        "https://www.gnu.org/licenses/agpl-3.0.html\n");
		return 0;
	}

	/* connect to the Unix socket */
	client  = g_socket_client_new();
	address = g_unix_socket_address_new(socket_path);

	conn = g_socket_client_connect(client,
	                               G_SOCKET_CONNECTABLE(address),
	                               NULL, &error);
	if (conn == NULL) {
		g_printerr("gowl-mcp: failed to connect to %s: %s\n",
		           socket_path, error->message);
		return 1;
	}

	/* get socket streams */
	sock_in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
	sock_out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

	/* wrap stdin/stdout in GIO streams */
	stdin_stream  = g_unix_input_stream_new(0, FALSE);
	stdout_stream = g_unix_output_stream_new(1, FALSE);

	/* create data input streams for line-based reading */
	stdin_data = g_data_input_stream_new(stdin_stream);
	sock_data  = g_data_input_stream_new(sock_in);

	/* start async line readers */
	main_loop = g_main_loop_new(NULL, FALSE);

	g_data_input_stream_read_line_async(stdin_data, G_PRIORITY_DEFAULT,
	                                    NULL, on_stdin_line, sock_out);
	g_data_input_stream_read_line_async(sock_data, G_PRIORITY_DEFAULT,
	                                    NULL, on_socket_line,
	                                    stdout_stream);

	g_main_loop_run(main_loop);

	g_main_loop_unref(main_loop);
	return 0;
}
