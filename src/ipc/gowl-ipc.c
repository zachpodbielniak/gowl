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
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <wayland-server-core.h>

/**
 * GOWL_IPC_BUF_SIZE:
 *
 * Per-client read buffer size.  Lines longer than this are
 * silently truncated to prevent unbounded allocation.
 */
#define GOWL_IPC_BUF_SIZE (4096)

/**
 * GowlIpcClient:
 *
 * Tracks a single connected IPC client.  Holds the file descriptor,
 * a wl_event_source for readable events, a line buffer for incremental
 * reads, and a flag indicating whether the client has subscribed
 * to receive broadcast events.
 */
typedef struct {
	int                      fd;
	struct wl_event_source  *event_source;
	GowlIpc                *server;     /* back-pointer (not owned) */
	gboolean                 subscribed;

	/* Incremental line buffer */
	gchar                    buf[GOWL_IPC_BUF_SIZE];
	gsize                    buf_len;
} GowlIpcClient;

/**
 * GowlIpc:
 *
 * IPC server that listens on a Unix domain socket for line-based
 * commands.  Uses wl_event_loop for I/O so that it integrates
 * directly into the compositor's Wayland event loop (instead of
 * requiring a separate GLib main loop).
 *
 * Each incoming line is parsed as "command args..." and the
 * "command-received" GObject signal is emitted.  Clients that
 * send "subscribe" are flagged to receive broadcast events via
 * gowl_ipc_push_event().
 */
struct _GowlIpc {
	GObject parent_instance;

	gchar                   *socket_path;
	gboolean                 running;

	/* Listening socket */
	int                      listen_fd;
	struct wl_event_source  *listen_source;
	struct wl_event_loop    *event_loop;    /* borrowed, not owned */

	/* Connected clients */
	GList                   *clients;       /* GList of GowlIpcClient* */
};

G_DEFINE_FINAL_TYPE(GowlIpc, gowl_ipc, G_TYPE_OBJECT)

enum {
	SIGNAL_COMMAND_RECEIVED,
	N_IPC_SIGNALS
};

static guint ipc_signals[N_IPC_SIGNALS];

/* --- Forward declarations --- */

static int  on_client_readable(int fd, uint32_t mask, void *data);
static int  on_listen_readable(int fd, uint32_t mask, void *data);
static void remove_client(GowlIpc *self, GowlIpcClient *client);

/* --- Helper: set fd non-blocking --- */

/**
 * set_nonblocking:
 * @fd: file descriptor
 *
 * Sets O_NONBLOCK on @fd.  Returns 0 on success, -1 on error.
 */
static int
set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Client line processing --- */

/**
 * process_line:
 * @self: the IPC server
 * @client: the client that sent the line
 * @line: the complete line (without newline)
 * @length: length of @line
 *
 * Parses a "command args..." line.  If the command is "subscribe",
 * flags the client for broadcast events.  Emits the
 * "command-received" signal for all commands.
 */
static void
process_line(
	GowlIpc       *self,
	GowlIpcClient *client,
	const gchar   *line,
	gsize          length
){
	gchar *command;
	const gchar *args;
	gchar *space;

	if (length == 0)
		return;

	space = strchr(line, ' ');
	if (space != NULL) {
		command = g_strndup(line, (gsize)(space - line));
		args = space + 1;
	} else {
		command = g_strdup(line);
		args = "";
	}

	/* Handle "subscribe" command: flag this client for events */
	if (g_strcmp0(command, "subscribe") == 0) {
		if (!client->subscribed) {
			client->subscribed = TRUE;
			g_debug("gowl-ipc: client fd=%d subscribed", client->fd);
		}
	}

	g_signal_emit(self, ipc_signals[SIGNAL_COMMAND_RECEIVED], 0,
	              command, args);

	g_free(command);
}

/* --- Client readable callback --- */

/**
 * on_client_readable:
 * @fd: the client socket fd
 * @mask: wl_event mask (WL_EVENT_READABLE, WL_EVENT_HANGUP, etc.)
 * @data: pointer to GowlIpcClient
 *
 * Called by the Wayland event loop when a client socket has data
 * to read.  Reads into the per-client line buffer and processes
 * complete newline-terminated lines.
 *
 * Returns: 0 to keep the source, or 0 (removal happens via
 *          remove_client which destroys the wl_event_source)
 */
static int
on_client_readable(int fd, uint32_t mask, void *data)
{
	GowlIpcClient *client;
	GowlIpc *self;
	ssize_t n;
	gchar *newline;

	client = (GowlIpcClient *)data;
	self = client->server;

	/* Handle hangup or error */
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		g_debug("gowl-ipc: client fd=%d disconnected", fd);
		remove_client(self, client);
		return 0;
	}

	/* Read available data into the line buffer */
	n = read(fd, client->buf + client->buf_len,
	         GOWL_IPC_BUF_SIZE - client->buf_len - 1);

	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
			g_debug("gowl-ipc: client fd=%d read %s",
			        fd, n == 0 ? "EOF" : g_strerror(errno));
			remove_client(self, client);
		}
		return 0;
	}

	client->buf_len += (gsize)n;
	client->buf[client->buf_len] = '\0';

	/* Process all complete lines in the buffer */
	while ((newline = strchr(client->buf, '\n')) != NULL) {
		gsize line_len;

		*newline = '\0';
		line_len = (gsize)(newline - client->buf);

		/* Strip trailing \r if present */
		if (line_len > 0 && client->buf[line_len - 1] == '\r') {
			client->buf[line_len - 1] = '\0';
			line_len--;
		}

		process_line(self, client, client->buf, line_len);

		/* Shift remaining data to front of buffer */
		{
			gsize remaining;

			remaining = client->buf_len - (gsize)(newline - client->buf) - 1;
			if (remaining > 0)
				memmove(client->buf, newline + 1, remaining);
			client->buf_len = remaining;
			client->buf[client->buf_len] = '\0';
		}
	}

	/* If the buffer is full with no newline, discard it to avoid
	 * blocking on a misbehaving client */
	if (client->buf_len >= GOWL_IPC_BUF_SIZE - 1) {
		g_warning("gowl-ipc: client fd=%d buffer overflow, discarding",
		          client->fd);
		client->buf_len = 0;
	}

	return 0;
}

/* --- Listener readable callback --- */

/**
 * on_listen_readable:
 * @fd: the listening socket fd
 * @mask: wl_event mask
 * @data: pointer to GowlIpc
 *
 * Called when the listening socket has a pending connection.
 * Accepts the connection, creates a GowlIpcClient, and adds
 * a readable event source for the new client fd.
 */
static int
on_listen_readable(int fd, uint32_t mask, void *data)
{
	GowlIpc *self;
	GowlIpcClient *client;
	int client_fd;

	self = (GowlIpc *)data;
	(void)mask;

	client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			g_warning("gowl-ipc: accept failed: %s", g_strerror(errno));
		return 0;
	}

	if (set_nonblocking(client_fd) < 0) {
		g_warning("gowl-ipc: failed to set non-blocking on fd=%d", client_fd);
		close(client_fd);
		return 0;
	}

	client = g_new0(GowlIpcClient, 1);
	client->fd          = client_fd;
	client->server      = self;
	client->subscribed  = FALSE;
	client->buf_len     = 0;

	client->event_source = wl_event_loop_add_fd(
		self->event_loop, client_fd,
		WL_EVENT_READABLE,
		on_client_readable, client);

	self->clients = g_list_prepend(self->clients, client);

	g_debug("gowl-ipc: client connected (fd=%d)", client_fd);
	return 0;
}

/* --- Client removal --- */

/**
 * remove_client:
 * @self: the IPC server
 * @client: the client to remove
 *
 * Removes a client from the server's client list, destroys its
 * wl_event_source, closes the fd, and frees the client struct.
 */
static void
remove_client(GowlIpc *self, GowlIpcClient *client)
{
	self->clients = g_list_remove(self->clients, client);

	if (client->event_source != NULL)
		wl_event_source_remove(client->event_source);

	close(client->fd);
	g_free(client);
}

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
	self->socket_path    = NULL;
	self->running        = FALSE;
	self->listen_fd      = -1;
	self->listen_source  = NULL;
	self->event_loop     = NULL;
	self->clients        = NULL;
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
 * @event_loop: the Wayland event loop to integrate with
 * @error: (nullable): return location for a #GError
 *
 * Creates a Unix domain socket at the configured path and starts
 * listening for incoming connections.  Uses @event_loop for I/O
 * dispatch so that the IPC server runs inside the compositor's
 * main event loop.  Commands are read as line-delimited text and
 * emitted via the "command-received" signal.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_ipc_start(
	GowlIpc              *self,
	struct wl_event_loop *event_loop,
	GError              **error
){
	struct sockaddr_un addr;
	int fd;

	g_return_val_if_fail(GOWL_IS_IPC(self), FALSE);
	g_return_val_if_fail(event_loop != NULL, FALSE);

	if (self->running) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED,
			"IPC server is already running");
		return FALSE;
	}

	/* Remove stale socket file if present */
	g_unlink(self->socket_path);

	/* Create the listening socket */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "socket() failed: %s", g_strerror(errno));
		return FALSE;
	}

	if (set_nonblocking(fd) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "fcntl(O_NONBLOCK) failed: %s", g_strerror(errno));
		close(fd);
		return FALSE;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	g_strlcpy(addr.sun_path, self->socket_path, sizeof(addr.sun_path));

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "bind(%s) failed: %s",
		            self->socket_path, g_strerror(errno));
		close(fd);
		return FALSE;
	}

	if (listen(fd, 16) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "listen() failed: %s", g_strerror(errno));
		close(fd);
		g_unlink(self->socket_path);
		return FALSE;
	}

	/* Register with the Wayland event loop */
	self->event_loop = event_loop;
	self->listen_fd = fd;
	self->listen_source = wl_event_loop_add_fd(
		event_loop, fd,
		WL_EVENT_READABLE,
		on_listen_readable, self);

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
	GList *next;

	g_return_if_fail(GOWL_IS_IPC(self));

	if (!self->running)
		return;

	self->running = FALSE;

	/* Remove the listener event source */
	if (self->listen_source != NULL) {
		wl_event_source_remove(self->listen_source);
		self->listen_source = NULL;
	}

	/* Close the listening socket */
	if (self->listen_fd >= 0) {
		close(self->listen_fd);
		self->listen_fd = -1;
	}

	/* Close all client connections */
	for (l = self->clients; l != NULL; l = next) {
		GowlIpcClient *client;

		next = l->next;
		client = (GowlIpcClient *)l->data;

		if (client->event_source != NULL)
			wl_event_source_remove(client->event_source);
		close(client->fd);
		g_free(client);
	}
	g_list_free(self->clients);
	self->clients = NULL;

	/* Remove the socket file */
	g_unlink(self->socket_path);

	self->event_loop = NULL;

	g_debug("gowl-ipc: stopped");
}

/**
 * gowl_ipc_push_event:
 * @self: the IPC server
 * @format: printf-style format string for the event line
 * @...: arguments for the format string
 *
 * Broadcasts a newline-terminated event line to all subscribed
 * clients.  Clients that have disconnected or whose write fails
 * are silently removed from the client list.
 */
void
gowl_ipc_push_event(GowlIpc *self, const gchar *format, ...)
{
	va_list ap;
	g_autofree gchar *body = NULL;
	g_autofree gchar *line = NULL;
	gsize line_len;
	GList *l;
	GList *next;
	gboolean has_subscribers;

	g_return_if_fail(GOWL_IS_IPC(self));

	/* Quick check: any subscribed clients? */
	has_subscribers = FALSE;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;
		if (client->subscribed) {
			has_subscribers = TRUE;
			break;
		}
	}
	if (!has_subscribers)
		return;

	/* Format the event line with trailing newline */
	va_start(ap, format);
	body = g_strdup_vprintf(format, ap);
	va_end(ap);

	line = g_strdup_printf("%s\n", body);
	line_len = strlen(line);

	/* Broadcast to all subscribed clients, removing dead ones */
	for (l = self->clients; l != NULL; l = next) {
		GowlIpcClient *client;
		ssize_t written;

		next = l->next;
		client = (GowlIpcClient *)l->data;

		if (!client->subscribed)
			continue;

		written = write(client->fd, line, line_len);
		if (written < 0) {
			g_debug("gowl-ipc: subscriber fd=%d write failed: %s",
			        client->fd, g_strerror(errno));
			remove_client(self, client);
		}
	}
}

/**
 * gowl_ipc_get_subscriber_count:
 * @self: the IPC server
 *
 * Returns the number of currently subscribed clients.
 *
 * Returns: the subscriber count
 */
guint
gowl_ipc_get_subscriber_count(GowlIpc *self)
{
	GList *l;
	guint count;

	g_return_val_if_fail(GOWL_IS_IPC(self), 0);

	count = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;
		if (client->subscribed)
			count++;
	}

	return count;
}
