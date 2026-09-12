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
 * gowl-msg CMD [ARGS...]     send one command line, print the reply
 * gowl-msg -s | --subscribe  subscribe and print events until killed
 *
 * The protocol is lines: one line in, one line back.  Queries answer
 * JSON on one line; everything else answers `OK ...' or `ERROR ...',
 * and an ERROR exits non-zero.  The socket is $XDG_RUNTIME_DIR/gowl.sock
 * unless --socket says otherwise.  See docs/ipc.org for the commands.
 */

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

static const gchar *license_text =
	"gowl-msg  Copyright (C) 2026  Zach Podbielniak\n"
	"This program comes with ABSOLUTELY NO WARRANTY.  This is free software,\n"
	"and you are welcome to redistribute it under the terms of the GNU Affero\n"
	"General Public License, version 3 or later.\n";

static gchar *
default_socket(void)
{
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

int
main(int argc, char *argv[])
{
	gboolean subscribe = FALSE;
	gboolean show_license = FALSE;
	gchar *socket_path = NULL;
	GOptionEntry entries[] = {
		{ "subscribe", 's', 0, G_OPTION_ARG_NONE, &subscribe,
		  "Subscribe and print events until killed", NULL },
		{ "socket", 'S', 0, G_OPTION_ARG_FILENAME, &socket_path,
		  "Socket path (default $XDG_RUNTIME_DIR/gowl.sock)", "PATH" },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &show_license,
		  "Show license information", NULL },
		{ NULL }
	};
	g_autoptr(GOptionContext) ctx = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketConnection) conn = NULL;
	g_autoptr(GDataInputStream) in = NULL;
	GOutputStream *out;
	g_autofree gchar *line = NULL;
	gchar *reply;
	int ret = 0;

	ctx = g_option_context_new("COMMAND [ARGS...] - talk to gowl");
	g_option_context_add_main_entries(ctx, entries, NULL);
	g_option_context_set_description(ctx,
		"Examples:\n"
		"  gowl-msg clients                 every window, as JSON\n"
		"  gowl-msg focused | jq .app_id    the focused window's app id\n"
		"  gowl-msg monitors                outputs, tags, layouts\n"
		"  gowl-msg view 4                  view tag 3 (a bitmask)\n"
		"  gowl-msg dispatch Super+Return   run what a key is bound to\n"
		"  gowl-msg action tag-view 2       run an action with an argument\n"
		"  gowl-msg mode resize             enter a key mode\n"
		"  gowl-msg power off               screens off\n"
		"  gowl-msg expo                    a module's command\n"
		"  gowl-msg -s                      stream events\n"
		"  gowl-msg help                    the command list\n");
	if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
		g_printerr("gowl-msg: %s\n", error->message);
		return 2;
	}
	if (show_license) {
		g_print("%s", license_text);
		return 0;
	}
	if (!subscribe && argc < 2) {
		g_autofree gchar *help = g_option_context_get_help(ctx, TRUE, NULL);

		g_print("%s", help);
		return 2;
	}
	if (socket_path == NULL)
		socket_path = default_socket();

	conn = connect_to(socket_path, &error);
	if (conn == NULL) {
		g_printerr("gowl-msg: cannot connect to %s: %s\n", socket_path,
		           error->message);
		g_free(socket_path);
		return 1;
	}
	g_free(socket_path);

	out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
	in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));

	if (subscribe) {
		if (!g_output_stream_write_all(out, "subscribe\n", 10, NULL, NULL,
		                               &error)) {
			g_printerr("gowl-msg: %s\n", error->message);
			return 1;
		}
		for (;;) {
			reply = g_data_input_stream_read_line_utf8(in, NULL, NULL,
			                                           &error);
			if (reply == NULL)
				break;
			g_print("%s\n", reply);
			fflush(stdout);
			g_free(reply);
		}
		return error != NULL ? 1 : 0;
	}

	/* Everything after the options is the command line, verbatim. */
	{
		g_autofree gchar *joined = g_strjoinv(" ", argv + 1);

		line = g_strdup_printf("%s\n", joined);
	}
	if (!g_output_stream_write_all(out, line, strlen(line), NULL, NULL,
	                               &error)) {
		g_printerr("gowl-msg: %s\n", error->message);
		return 1;
	}
	reply = g_data_input_stream_read_line_utf8(in, NULL, NULL, &error);
	if (reply == NULL) {
		g_printerr("gowl-msg: no reply%s%s\n", error != NULL ? ": " : "",
		           error != NULL ? error->message : "");
		return 1;
	}
	g_print("%s\n", reply);
	if (g_str_has_prefix(reply, "ERROR"))
		ret = 1;
	g_free(reply);
	return ret;
}
