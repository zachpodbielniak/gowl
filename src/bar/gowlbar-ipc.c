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

#include "gowlbar-ipc.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <stdlib.h>

/**
 * GowlbarIpc:
 *
 * IPC client that connects to the gowl compositor's Unix domain
 * socket.  Sends "subscribe" on connect to receive state events,
 * then parses incoming EVENT lines and emits GObject signals for
 * each event type.  Automatically reconnects with exponential
 * backoff when the connection is lost.
 */
struct _GowlbarIpc {
	GObject parent_instance;

	gchar               *socket_path;
	GSocketConnection   *connection;
	GDataInputStream    *input_stream;
	gboolean             connected;
	gboolean             active;   /* FALSE after disconnect() called */

	/* Reconnection state */
	guint                reconnect_source_id;
	guint                reconnect_delay;  /* current delay in seconds */
};

G_DEFINE_FINAL_TYPE(GowlbarIpc, gowlbar_ipc, G_TYPE_OBJECT)

/* --- Reconnection constants --- */

#define GOWLBAR_IPC_RECONNECT_MIN  (1)
#define GOWLBAR_IPC_RECONNECT_MAX  (30)

/* --- Signals --- */

enum {
	SIGNAL_CONNECTED,
	SIGNAL_DISCONNECTED,
	SIGNAL_TAGS_CHANGED,
	SIGNAL_LAYOUT_CHANGED,
	SIGNAL_TITLE_CHANGED,
	SIGNAL_FOCUS_CHANGED,
	N_IPC_SIGNALS
};

static guint ipc_signals[N_IPC_SIGNALS];

/* --- Forward declarations --- */

static void start_read_loop(GowlbarIpc *self);
static void schedule_reconnect(GowlbarIpc *self);

/* --- Event parsing --- */

/**
 * parse_event_line:
 * @self: the IPC client
 * @line: the raw event line (without newline)
 *
 * Parses an incoming event line and emits the appropriate signal.
 * Expected formats:
 *   EVENT tags <output> <active_mask> <occupied_mask> <urgent_mask> <sel_tags>
 *   EVENT layout <output> <layout_name>
 *   EVENT title <title_text>
 *   EVENT focus <output_name>
 */
static void
parse_event_line(GowlbarIpc *self, const gchar *line)
{
	/* All events start with "EVENT " */
	if (!g_str_has_prefix(line, "EVENT "))
		return;

	line += 6;  /* skip "EVENT " */

	if (g_str_has_prefix(line, "tags ")) {
		/*
		 * Format: tags <output> <active> <occupied> <urgent> <sel>
		 * All mask values are decimal unsigned integers.
		 */
		const gchar *rest;
		gchar *output_name;
		guint32 active_mask, occupied_mask, urgent_mask, sel_tags;
		gchar *space;

		rest = line + 5;

		/* Extract output name */
		space = strchr(rest, ' ');
		if (space == NULL)
			return;
		output_name = g_strndup(rest, (gsize)(space - rest));
		rest = space + 1;

		/* Parse four mask values */
		active_mask   = (guint32)strtoul(rest, (char **)&rest, 10);
		occupied_mask = (guint32)strtoul(rest, (char **)&rest, 10);
		urgent_mask   = (guint32)strtoul(rest, (char **)&rest, 10);
		sel_tags      = (guint32)strtoul(rest, NULL, 10);

		g_signal_emit(self, ipc_signals[SIGNAL_TAGS_CHANGED], 0,
		              output_name, active_mask, occupied_mask,
		              urgent_mask, sel_tags);

		g_free(output_name);

	} else if (g_str_has_prefix(line, "layout ")) {
		/*
		 * Format: layout <output> <layout_name>
		 */
		const gchar *rest;
		gchar *output_name;
		gchar *space;

		rest = line + 7;

		space = strchr(rest, ' ');
		if (space == NULL)
			return;
		output_name = g_strndup(rest, (gsize)(space - rest));

		g_signal_emit(self, ipc_signals[SIGNAL_LAYOUT_CHANGED], 0,
		              output_name, space + 1);

		g_free(output_name);

	} else if (g_str_has_prefix(line, "title ")) {
		/*
		 * Format: title <title_text>
		 * Title may contain spaces, so everything after "title " is
		 * the title text.
		 */
		g_signal_emit(self, ipc_signals[SIGNAL_TITLE_CHANGED], 0,
		              line + 6);

	} else if (g_str_has_prefix(line, "focus ")) {
		/*
		 * Format: focus <output_name>
		 */
		g_signal_emit(self, ipc_signals[SIGNAL_FOCUS_CHANGED], 0,
		              line + 6);
	}
}

/* --- Async read loop --- */

/**
 * on_read_line:
 *
 * Callback for async line reads.  Parses the line as an event
 * and continues reading.
 */
static void
on_read_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GDataInputStream *dis;
	GowlbarIpc *self;
	g_autofree gchar *line = NULL;
	gsize length;
	GError *error;

	dis = G_DATA_INPUT_STREAM(source);
	self = (GowlbarIpc *)user_data;

	error = NULL;
	line = g_data_input_stream_read_line_finish(dis, result,
	                                             &length, &error);

	if (line == NULL) {
		/* EOF or error: connection lost */
		if (error != NULL) {
			g_debug("gowlbar-ipc: read error: %s", error->message);
			g_error_free(error);
		} else {
			g_debug("gowlbar-ipc: connection closed by server");
		}

		self->connected = FALSE;
		g_signal_emit(self, ipc_signals[SIGNAL_DISCONNECTED], 0);

		/* Clean up connection */
		g_clear_object(&self->input_stream);
		if (self->connection != NULL) {
			g_io_stream_close(G_IO_STREAM(self->connection),
			                   NULL, NULL);
			g_clear_object(&self->connection);
		}

		/* Schedule reconnect if still active */
		if (self->active)
			schedule_reconnect(self);

		return;
	}

	/* Parse the event */
	parse_event_line(self, line);

	/* Continue reading */
	if (self->connected && self->input_stream != NULL) {
		g_data_input_stream_read_line_async(
			self->input_stream,
			G_PRIORITY_DEFAULT, NULL,
			on_read_line, self);
	}
}

/**
 * start_read_loop:
 *
 * Sets up async line-based reading on the connection.
 */
static void
start_read_loop(GowlbarIpc *self)
{
	GInputStream *istream;

	istream = g_io_stream_get_input_stream(
		G_IO_STREAM(self->connection));
	self->input_stream = g_data_input_stream_new(istream);

	g_data_input_stream_read_line_async(
		self->input_stream,
		G_PRIORITY_DEFAULT, NULL,
		on_read_line, self);
}

/* --- Connection --- */

/**
 * on_connect_ready:
 *
 * Callback when the async connect completes.  On success, sends
 * "subscribe" and starts reading events.  On failure, schedules
 * reconnection.
 */
static void
on_connect_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GSocketClient *client;
	GowlbarIpc *self;
	GSocketConnection *conn;
	GOutputStream *ostream;
	GError *error;
	const gchar *subscribe_cmd;

	client = G_SOCKET_CLIENT(source);
	self = (GowlbarIpc *)user_data;

	error = NULL;
	conn = g_socket_client_connect_finish(client, result, &error);

	if (conn == NULL) {
		g_debug("gowlbar-ipc: connect failed: %s", error->message);
		g_error_free(error);

		/* Schedule reconnect */
		if (self->active)
			schedule_reconnect(self);

		g_object_unref(client);
		return;
	}

	g_object_unref(client);

	self->connection = conn;
	self->connected = TRUE;
	self->reconnect_delay = GOWLBAR_IPC_RECONNECT_MIN;

	g_debug("gowlbar-ipc: connected to %s", self->socket_path);

	/* Send subscribe command to start receiving events */
	subscribe_cmd = "subscribe\n";
	ostream = g_io_stream_get_output_stream(G_IO_STREAM(conn));

	error = NULL;
	if (!g_output_stream_write_all(ostream,
	                                subscribe_cmd,
	                                strlen(subscribe_cmd),
	                                NULL, NULL, &error)) {
		g_warning("gowlbar-ipc: failed to send subscribe: %s",
		          error->message);
		g_error_free(error);
	}

	/* Emit connected signal */
	g_signal_emit(self, ipc_signals[SIGNAL_CONNECTED], 0);

	/* Start reading events */
	start_read_loop(self);
}

/**
 * do_connect:
 *
 * Performs the async connection to the compositor socket.
 */
static void
do_connect(GowlbarIpc *self)
{
	GSocketClient *client;
	GSocketAddress *address;

	client = g_socket_client_new();
	address = g_unix_socket_address_new(self->socket_path);

	g_socket_client_connect_async(
		client,
		G_SOCKET_CONNECTABLE(address),
		NULL,
		on_connect_ready,
		self);

	g_object_unref(address);
	/* client is unref'd in on_connect_ready */
}

/* --- Reconnection --- */

/**
 * on_reconnect_timer:
 *
 * GSource callback that fires after the reconnection delay.
 * Attempts to reconnect to the compositor.
 */
static gboolean
on_reconnect_timer(gpointer user_data)
{
	GowlbarIpc *self;

	self = (GowlbarIpc *)user_data;
	self->reconnect_source_id = 0;

	if (self->active && !self->connected) {
		g_debug("gowlbar-ipc: attempting reconnect...");
		do_connect(self);
	}

	return G_SOURCE_REMOVE;
}

/**
 * schedule_reconnect:
 *
 * Schedules a reconnection attempt with exponential backoff.
 * Delay starts at 1 second and doubles up to 30 seconds.
 */
static void
schedule_reconnect(GowlbarIpc *self)
{
	if (self->reconnect_source_id > 0)
		return;  /* already scheduled */

	g_debug("gowlbar-ipc: reconnecting in %u seconds",
	        self->reconnect_delay);

	self->reconnect_source_id = g_timeout_add_seconds(
		self->reconnect_delay,
		on_reconnect_timer,
		self);

	/* Double the delay for next time, capped */
	self->reconnect_delay *= 2;
	if (self->reconnect_delay > GOWLBAR_IPC_RECONNECT_MAX)
		self->reconnect_delay = GOWLBAR_IPC_RECONNECT_MAX;
}

/* --- GObject lifecycle --- */

static void
gowlbar_ipc_finalize(GObject *object)
{
	GowlbarIpc *self;

	self = GOWLBAR_IPC(object);

	gowlbar_ipc_disconnect(self);

	g_free(self->socket_path);

	G_OBJECT_CLASS(gowlbar_ipc_parent_class)->finalize(object);
}

static void
gowlbar_ipc_class_init(GowlbarIpcClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gowlbar_ipc_finalize;

	/**
	 * GowlbarIpc::connected:
	 * @self: the IPC client
	 *
	 * Emitted when the IPC client successfully connects to the
	 * compositor and has sent the subscribe command.
	 */
	ipc_signals[SIGNAL_CONNECTED] = g_signal_new(
		"connected",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GowlbarIpc::disconnected:
	 * @self: the IPC client
	 *
	 * Emitted when the connection to the compositor is lost.
	 */
	ipc_signals[SIGNAL_DISCONNECTED] = g_signal_new(
		"disconnected",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 0);

	/**
	 * GowlbarIpc::tags-changed:
	 * @self: the IPC client
	 * @output: the output name
	 * @active_mask: bitmask of active tags
	 * @occupied_mask: bitmask of occupied tags
	 * @urgent_mask: bitmask of urgent tags
	 * @sel_tags: bitmask of selected tags
	 *
	 * Emitted when tag state changes on an output.
	 */
	ipc_signals[SIGNAL_TAGS_CHANGED] = g_signal_new(
		"tags-changed",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 5,
		G_TYPE_STRING,
		G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);

	/**
	 * GowlbarIpc::layout-changed:
	 * @self: the IPC client
	 * @output: the output name
	 * @layout_name: the new layout symbol
	 *
	 * Emitted when the layout changes on an output.
	 */
	ipc_signals[SIGNAL_LAYOUT_CHANGED] = g_signal_new(
		"layout-changed",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 2,
		G_TYPE_STRING, G_TYPE_STRING);

	/**
	 * GowlbarIpc::title-changed:
	 * @self: the IPC client
	 * @title: the new window title
	 *
	 * Emitted when the focused window title changes.
	 */
	ipc_signals[SIGNAL_TITLE_CHANGED] = g_signal_new(
		"title-changed",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 1,
		G_TYPE_STRING);

	/**
	 * GowlbarIpc::focus-changed:
	 * @self: the IPC client
	 * @output: the newly focused output name
	 *
	 * Emitted when focus moves to a different output.
	 */
	ipc_signals[SIGNAL_FOCUS_CHANGED] = g_signal_new(
		"focus-changed",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 1,
		G_TYPE_STRING);
}

static void
gowlbar_ipc_init(GowlbarIpc *self)
{
	self->socket_path          = NULL;
	self->connection           = NULL;
	self->input_stream         = NULL;
	self->connected            = FALSE;
	self->active               = FALSE;
	self->reconnect_source_id  = 0;
	self->reconnect_delay      = GOWLBAR_IPC_RECONNECT_MIN;
}

/* --- Public API --- */

/**
 * gowlbar_ipc_new:
 * @socket_path: (nullable): path for the Unix socket, or %NULL
 *   for the default ($XDG_RUNTIME_DIR/gowl.sock)
 *
 * Creates a new IPC client.
 *
 * Returns: (transfer full): a new #GowlbarIpc
 */
GowlbarIpc *
gowlbar_ipc_new(const gchar *socket_path)
{
	GowlbarIpc *self;

	self = (GowlbarIpc *)g_object_new(GOWLBAR_TYPE_IPC, NULL);

	if (socket_path != NULL) {
		self->socket_path = g_strdup(socket_path);
	} else {
		const gchar *runtime_dir;

		runtime_dir = g_get_user_runtime_dir();
		self->socket_path = g_strdup_printf(
			"%s/gowl.sock", runtime_dir);
	}

	return self;
}

/**
 * gowlbar_ipc_connect:
 * @self: the IPC client
 *
 * Initiates connection to the compositor socket.  On success,
 * sends "subscribe" and starts receiving events.  Auto-reconnects
 * with exponential backoff on connection loss.
 */
void
gowlbar_ipc_connect(GowlbarIpc *self)
{
	g_return_if_fail(GOWLBAR_IS_IPC(self));

	self->active = TRUE;
	self->reconnect_delay = GOWLBAR_IPC_RECONNECT_MIN;

	if (!self->connected)
		do_connect(self);
}

/**
 * gowlbar_ipc_disconnect:
 * @self: the IPC client
 *
 * Closes the connection and stops reconnection attempts.
 */
void
gowlbar_ipc_disconnect(GowlbarIpc *self)
{
	g_return_if_fail(GOWLBAR_IS_IPC(self));

	self->active = FALSE;

	/* Cancel pending reconnect */
	if (self->reconnect_source_id > 0) {
		g_source_remove(self->reconnect_source_id);
		self->reconnect_source_id = 0;
	}

	/* Close input stream */
	g_clear_object(&self->input_stream);

	/* Close connection */
	if (self->connection != NULL) {
		g_io_stream_close(G_IO_STREAM(self->connection), NULL, NULL);
		g_clear_object(&self->connection);
	}

	self->connected = FALSE;
}

/**
 * gowlbar_ipc_send_command:
 * @self: the IPC client
 * @command: the command to send (newline appended automatically)
 *
 * Sends a command string to the compositor over the IPC connection.
 * Does nothing if not currently connected.
 */
void
gowlbar_ipc_send_command(GowlbarIpc *self, const gchar *command)
{
	GOutputStream *ostream;
	g_autofree gchar *line = NULL;
	GError *error;

	g_return_if_fail(GOWLBAR_IS_IPC(self));
	g_return_if_fail(command != NULL);

	if (!self->connected || self->connection == NULL)
		return;

	line = g_strdup_printf("%s\n", command);
	ostream = g_io_stream_get_output_stream(
		G_IO_STREAM(self->connection));

	error = NULL;
	if (!g_output_stream_write_all(ostream, line, strlen(line),
	                                NULL, NULL, &error)) {
		g_debug("gowlbar-ipc: send failed: %s", error->message);
		g_error_free(error);
	}
}

/**
 * gowlbar_ipc_is_connected:
 * @self: the IPC client
 *
 * Returns: %TRUE if currently connected to the compositor
 */
gboolean
gowlbar_ipc_is_connected(GowlbarIpc *self)
{
	g_return_val_if_fail(GOWLBAR_IS_IPC(self), FALSE);

	return self->connected;
}
