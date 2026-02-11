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

#include "gowl-ipc.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>

/**
 * GowlIpc:
 *
 * IPC server that listens on a Unix domain socket for line-based
 * commands.  Each line is parsed as "command args..." and emits
 * the "command-received" signal for the compositor to handle.
 * Uses GLib async I/O for non-blocking operation.
 */
struct _GowlIpc {
	GObject parent_instance;

	gchar           *socket_path;
	gboolean         running;
	GSocketService  *service;     /* GSocketService for accepting clients */
	GList           *connections; /* active GSocketConnection* list */
};

G_DEFINE_FINAL_TYPE(GowlIpc, gowl_ipc, G_TYPE_OBJECT)

enum {
	SIGNAL_COMMAND_RECEIVED,
	N_IPC_SIGNALS
};

static guint ipc_signals[N_IPC_SIGNALS];

/* --- Forward declarations --- */

static gboolean on_incoming(GSocketService    *service,
                            GSocketConnection *connection,
                            GObject           *source_object,
                            gpointer           user_data);
static void     read_line_async(GowlIpc           *self,
                                GSocketConnection  *conn);

/* --- GObject lifecycle --- */

static void
gowl_ipc_dispose(GObject *object)
{
	GowlIpc *self;

	self = GOWL_IPC(object);

	/* Stop the server if running */
	if (self->running)
		gowl_ipc_stop(self);

	G_OBJECT_CLASS(gowl_ipc_parent_class)->dispose(object);
}

static void
gowl_ipc_finalize(GObject *object)
{
	GowlIpc *self;

	self = GOWL_IPC(object);

	g_free(self->socket_path);
	g_list_free(self->connections);

	G_OBJECT_CLASS(gowl_ipc_parent_class)->finalize(object);
}

static void
gowl_ipc_class_init(GowlIpcClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_ipc_dispose;
	object_class->finalize = gowl_ipc_finalize;

	/**
	 * GowlIpc::command-received:
	 * @self: the IPC server
	 * @command: the command string
	 * @args: the arguments string (may be empty)
	 *
	 * Emitted when an IPC command is received from a client.
	 * The handler should process the command and may return a
	 * response string.
	 */
	ipc_signals[SIGNAL_COMMAND_RECEIVED] = g_signal_new(
		"command-received",
		G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST,
		0,
		NULL, NULL,
		NULL,
		G_TYPE_NONE, 2,
		G_TYPE_STRING, G_TYPE_STRING);
}

static void
gowl_ipc_init(GowlIpc *self)
{
	self->socket_path = NULL;
	self->running     = FALSE;
	self->service     = NULL;
	self->connections = NULL;
}

/* --- Async read callback --- */

/**
 * on_read_line:
 *
 * Callback for async line reads.  Parses the command and args
 * from the line, emits command-received, then reads the next line.
 */
static void
on_read_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GDataInputStream *dis;
	GowlIpc *self;
	GSocketConnection *conn;
	gchar *line;
	gsize length;
	GError *error;

	dis = G_DATA_INPUT_STREAM(source);
	self = (GowlIpc *)g_object_get_data(source, "gowl-ipc");
	conn = (GSocketConnection *)g_object_get_data(source, "gowl-conn");

	error = NULL;
	line = g_data_input_stream_read_line_finish(dis, result, &length, &error);

	if (line == NULL) {
		/* EOF or error: client disconnected */
		if (error != NULL) {
			g_debug("gowl-ipc: read error: %s", error->message);
			g_error_free(error);
		}

		/* Clean up this connection */
		self->connections = g_list_remove(self->connections, conn);
		g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
		g_object_unref(conn);
		g_object_unref(dis);
		return;
	}

	/* Parse "command args..." */
	if (length > 0) {
		gchar *command;
		const gchar *args;
		gchar *space;

		space = strchr(line, ' ');
		if (space != NULL) {
			command = g_strndup(line, (gsize)(space - line));
			args = space + 1;
		} else {
			command = g_strdup(line);
			args = "";
		}

		g_signal_emit(self, ipc_signals[SIGNAL_COMMAND_RECEIVED], 0,
		              command, args);

		g_free(command);
	}

	g_free(line);

	/* Continue reading if still running */
	if (self->running) {
		g_data_input_stream_read_line_async(
			dis, G_PRIORITY_DEFAULT, NULL, on_read_line, NULL);
	} else {
		self->connections = g_list_remove(self->connections, conn);
		g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
		g_object_unref(conn);
		g_object_unref(dis);
	}
}

/**
 * read_line_async:
 *
 * Sets up async line-based reading on a new connection.
 */
static void
read_line_async(
	GowlIpc            *self,
	GSocketConnection   *conn
){
	GInputStream *istream;
	GDataInputStream *dis;

	istream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
	dis = g_data_input_stream_new(istream);

	/* Attach references for the callback */
	g_object_set_data(G_OBJECT(dis), "gowl-ipc", self);
	g_object_set_data(G_OBJECT(dis), "gowl-conn", conn);

	g_data_input_stream_read_line_async(
		dis, G_PRIORITY_DEFAULT, NULL, on_read_line, NULL);
}

/* --- Incoming connection handler --- */

/**
 * on_incoming:
 *
 * Called when a new client connects to the IPC socket.
 * Starts async line reading on the connection.
 */
static gboolean
on_incoming(
	GSocketService    *service,
	GSocketConnection *connection,
	GObject           *source_object,
	gpointer           user_data
){
	GowlIpc *self;

	self = (GowlIpc *)user_data;
	(void)service;
	(void)source_object;

	/* Take a reference and track the connection */
	g_object_ref(connection);
	self->connections = g_list_prepend(self->connections, connection);

	/* Start reading commands */
	read_line_async(self, connection);

	g_debug("gowl-ipc: client connected");
	return TRUE;
}

/* --- Public API --- */

/**
 * gowl_ipc_new:
 * @socket_path: (nullable): path for the Unix socket, or %NULL
 *   to use the default ($XDG_RUNTIME_DIR/gowl.sock)
 *
 * Creates a new #GowlIpc server instance.
 *
 * Returns: (transfer full): a newly allocated #GowlIpc
 */
GowlIpc *
gowl_ipc_new(const gchar *socket_path)
{
	GowlIpc *self;

	self = (GowlIpc *)g_object_new(GOWL_TYPE_IPC, NULL);

	if (socket_path != NULL) {
		self->socket_path = g_strdup(socket_path);
	} else {
		const gchar *runtime_dir;

		runtime_dir = g_get_user_runtime_dir();
		self->socket_path = g_strdup_printf("%s/gowl.sock", runtime_dir);
	}

	return self;
}

/**
 * gowl_ipc_start:
 * @self: the IPC server
 * @error: (nullable): return location for a #GError
 *
 * Creates a Unix domain socket at the configured path and starts
 * listening for incoming connections.  Commands are read as
 * line-delimited text and emitted via the "command-received" signal.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_ipc_start(GowlIpc *self, GError **error)
{
	GSocketAddress *address;
	GError *local_error;

	g_return_val_if_fail(GOWL_IS_IPC(self), FALSE);

	if (self->running) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED,
			"IPC server is already running");
		return FALSE;
	}

	/* Remove stale socket file if present */
	g_unlink(self->socket_path);

	/* Create the socket service */
	self->service = g_socket_service_new();

	address = g_unix_socket_address_new(self->socket_path);

	local_error = NULL;
	if (!g_socket_listener_add_address(
			G_SOCKET_LISTENER(self->service),
			address, G_SOCKET_TYPE_STREAM,
			G_SOCKET_PROTOCOL_DEFAULT,
			NULL, NULL, &local_error)) {
		g_propagate_error(error, local_error);
		g_object_unref(address);
		g_clear_object(&self->service);
		return FALSE;
	}
	g_object_unref(address);

	/* Connect the incoming signal */
	g_signal_connect(self->service, "incoming",
	                 G_CALLBACK(on_incoming), self);

	/* Start accepting connections */
	g_socket_service_start(self->service);
	self->running = TRUE;

	g_debug("gowl-ipc: started on %s", self->socket_path);
	return TRUE;
}

/**
 * gowl_ipc_stop:
 * @self: the IPC server
 *
 * Stops the IPC server, closes all client connections,
 * and removes the socket file.
 */
void
gowl_ipc_stop(GowlIpc *self)
{
	GList *l;

	g_return_if_fail(GOWL_IS_IPC(self));

	if (!self->running)
		return;

	self->running = FALSE;

	/* Stop accepting new connections */
	if (self->service != NULL) {
		g_socket_service_stop(self->service);
		g_clear_object(&self->service);
	}

	/* Close all active connections */
	for (l = self->connections; l != NULL; l = l->next) {
		GSocketConnection *conn = (GSocketConnection *)l->data;
		g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
		g_object_unref(conn);
	}
	g_list_free(self->connections);
	self->connections = NULL;

	/* Remove the socket file */
	g_unlink(self->socket_path);

	g_debug("gowl-ipc: stopped");
}
