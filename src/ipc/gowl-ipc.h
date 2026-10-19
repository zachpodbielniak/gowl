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

#ifndef GOWL_IPC_H
#define GOWL_IPC_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <wayland-server-core.h>

G_BEGIN_DECLS

/*
 * The socket speaks two dialects on one connection, told apart by the
 * first byte of each line:
 *
 *   text       `clients', `view 4', `subscribe' ...  one line in, one
 *              line back; the protocol gowl-msg has always spoken.
 *   JSON-RPC   a line starting `{' (one request) or `[' (a batch) is
 *              JSON-RPC 2.0, newline-framed: the "gowl wire protocol".
 *              Requests carry an id and get a response with that id;
 *              notifications (no id) get nothing back; events arrive
 *              as `event' notifications after `events.subscribe'.
 *
 * docs/ipc.org is the reference; `rpc.discover' is the same reference
 * asked of a running compositor.
 */

/**
 * GOWL_IPC_PROTOCOL_NAME:
 *
 * The name `rpc.discover' reports for the JSON-RPC dialect.
 */
#define GOWL_IPC_PROTOCOL_NAME "gowl-wire"

/**
 * GOWL_IPC_PROTOCOL_VERSION:
 *
 * The JSON-RPC dialect's version.  Bumped only when an existing method,
 * parameter or event changes meaning; adding one does not bump it, so
 * a client checks for what it needs in `rpc.discover' instead.
 */
#define GOWL_IPC_PROTOCOL_VERSION (1)

/**
 * GOWL_IPC_MAX_LINE:
 *
 * The longest request line accepted, in bytes.  A longer one is
 * answered with an error and discarded up to its newline.
 */
#define GOWL_IPC_MAX_LINE (1024 * 1024)

/**
 * GOWL_IPC_MAX_PENDING:
 *
 * The most output allowed to queue for one connection that is not
 * reading.  A subscriber that falls this far behind is disconnected
 * rather than allowed to grow the compositor's memory without bound.
 */
#define GOWL_IPC_MAX_PENDING (16 * 1024 * 1024)

#define GOWL_TYPE_IPC (gowl_ipc_get_type())

G_DECLARE_FINAL_TYPE(GowlIpc, gowl_ipc, GOWL, IPC, GObject)

/**
 * GOWL_IPC_ERROR:
 *
 * Error domain for JSON-RPC method failures.  The codes are the
 * JSON-RPC error codes sent on the wire.
 */
#define GOWL_IPC_ERROR (gowl_ipc_error_quark())

GQuark gowl_ipc_error_quark(void);

/**
 * GowlIpcErrorCode:
 * @GOWL_IPC_ERROR_PARSE: the line is not JSON
 * @GOWL_IPC_ERROR_INVALID_REQUEST: JSON, but not a JSON-RPC request
 * @GOWL_IPC_ERROR_METHOD_NOT_FOUND: no such method
 * @GOWL_IPC_ERROR_INVALID_PARAMS: a parameter is missing, unknown or
 *   of the wrong type
 * @GOWL_IPC_ERROR_INTERNAL: the compositor failed in a way that is
 *   its fault
 * @GOWL_IPC_ERROR_FAILED: the operation did not succeed
 * @GOWL_IPC_ERROR_NOT_FOUND: a window, output, layout or action the
 *   request named does not exist
 * @GOWL_IPC_ERROR_REFUSED: the compositor will not do this now --
 *   the session is locked, the window is embedded in an Emacs buffer,
 *   the output cannot do HDR
 * @GOWL_IPC_ERROR_UNSUPPORTED: not in this build, or the module that
 *   provides it is not loaded
 *
 * JSON-RPC error codes.  The first five are the specification's own;
 * the rest are in its "server error" range.
 */
typedef enum {
	GOWL_IPC_ERROR_PARSE            = -32700,
	GOWL_IPC_ERROR_INVALID_REQUEST  = -32600,
	GOWL_IPC_ERROR_METHOD_NOT_FOUND = -32601,
	GOWL_IPC_ERROR_INVALID_PARAMS   = -32602,
	GOWL_IPC_ERROR_INTERNAL         = -32603,
	GOWL_IPC_ERROR_FAILED           = -32000,
	GOWL_IPC_ERROR_NOT_FOUND        = -32001,
	GOWL_IPC_ERROR_REFUSED          = -32002,
	GOWL_IPC_ERROR_UNSUPPORTED      = -32003
} GowlIpcErrorCode;

/**
 * GowlIpcCaller:
 * @pid: the connecting process, from SO_PEERCRED; -1 when unknown
 * @uid: its user id
 * @connection: a serial unique to this connection for the server's
 *   lifetime
 *
 * Who sent a request.  Methods use @pid to answer "my own windows":
 * the windows of the calling process and everything it started.
 */
typedef struct {
	gint  pid;
	guint uid;
	guint connection;
} GowlIpcCaller;

/**
 * GowlIpcParamType:
 * @GOWL_IPC_PARAM_ANY: any JSON value
 * @GOWL_IPC_PARAM_INT: an integer
 * @GOWL_IPC_PARAM_NUMBER: an integer or a real
 * @GOWL_IPC_PARAM_BOOL: true or false
 * @GOWL_IPC_PARAM_STRING: a string
 * @GOWL_IPC_PARAM_ARRAY: an array
 * @GOWL_IPC_PARAM_OBJECT: an object
 * @GOWL_IPC_PARAM_TOGGLE: true, false or the string "toggle"
 * @GOWL_IPC_PARAM_WINDOW: a window id (integer) or "focused"
 * @GOWL_IPC_PARAM_MONITOR: an output name (string) or "focused"
 *
 * What a declared parameter accepts.  The transport checks every
 * request against its method's declaration before the method runs,
 * so a method never sees a parameter it did not declare (a typo is an
 * error, not a silently ignored key) or one of the wrong type.
 */
typedef enum {
	GOWL_IPC_PARAM_ANY,
	GOWL_IPC_PARAM_INT,
	GOWL_IPC_PARAM_NUMBER,
	GOWL_IPC_PARAM_BOOL,
	GOWL_IPC_PARAM_STRING,
	GOWL_IPC_PARAM_ARRAY,
	GOWL_IPC_PARAM_OBJECT,
	GOWL_IPC_PARAM_TOGGLE,
	GOWL_IPC_PARAM_WINDOW,
	GOWL_IPC_PARAM_MONITOR
} GowlIpcParamType;

/**
 * GowlIpcParam:
 * @name: the parameter's name; %NULL ends a list
 * @type: what it accepts
 * @required: whether a request must carry it
 * @doc: one line on what it means, for `rpc.discover'
 *
 * One declared parameter of a JSON-RPC method.
 */
typedef struct {
	const gchar      *name;
	GowlIpcParamType  type;
	gboolean          required;
	const gchar      *doc;
} GowlIpcParam;

/**
 * gowl_ipc_new:
 * @socket_path: (nullable): path for the UNIX socket, or NULL for default
 *
 * Creates a new IPC server.
 *
 * Returns: (transfer full): a new #GowlIpc
 */
GowlIpc *
gowl_ipc_new(const gchar *socket_path);

/**
 * gowl_ipc_start:
 * @self: the IPC server
 * @event_loop: the Wayland event loop to integrate with
 * @error: (nullable): return location for a #GError
 *
 * Starts listening for IPC connections.  Uses @event_loop for
 * I/O dispatch so the IPC server runs within the compositor's
 * Wayland event loop.
 *
 * Returns: TRUE on success
 */
gboolean
gowl_ipc_start(
	GowlIpc              *self,
	struct wl_event_loop *event_loop,
	GError              **error
);

/**
 * GowlIpcCommandFunc:
 * @ipc: the IPC server
 * @line: the whole line a client sent, without its newline
 * @user_data: what was given to gowl_ipc_set_command_handler()
 *
 * Answers one text command line.  The reply is written back to the
 * client as one line; %NULL means "unknown command" and the client is
 * told so.  A reply containing newlines is sent as is, so a handler
 * that wants one line per reply keeps them out.
 *
 * Returns: (transfer full) (nullable): the reply
 */
typedef gchar *(*GowlIpcCommandFunc) (GowlIpc     *ipc,
                                      const gchar *line,
                                      gpointer     user_data);

/**
 * gowl_ipc_set_command_handler:
 * @self: the IPC server
 * @func: (nullable) (scope notified): the handler, or %NULL for none
 * @user_data: passed to @func
 *
 * Installs what answers the socket's text commands.  The compositor
 * installs its own from gowl_compositor_set_ipc(): core queries first
 * (`clients', `monitors', `mode', ...), then every module command.
 * Without a handler a command is acknowledged with an error.
 */
void
gowl_ipc_set_command_handler(GowlIpc            *self,
                             GowlIpcCommandFunc  func,
                             gpointer            user_data);

/**
 * GowlIpcMethodFunc:
 * @ipc: the IPC server
 * @caller: who sent the request
 * @params: the request's parameters, already checked against the
 *   method's declaration; never %NULL (an absent "params" is empty)
 * @user_data: what was given to gowl_ipc_register_method()
 * @error: return location for a #GError; a %GOWL_IPC_ERROR code is
 *   sent as is, any other domain as %GOWL_IPC_ERROR_FAILED
 *
 * Runs one JSON-RPC method.  Returning %NULL with @error unset is a
 * success whose result is JSON null.
 *
 * Returns: (transfer full) (nullable): the result
 */
typedef JsonNode *(*GowlIpcMethodFunc) (GowlIpc             *ipc,
                                        const GowlIpcCaller *caller,
                                        JsonObject          *params,
                                        gpointer             user_data,
                                        GError             **error);

/**
 * gowl_ipc_register_method: (skip)
 * @self: the IPC server
 * @name: the method name, `namespace.verb' by convention
 * @summary: one line on what it does
 * @params: (nullable) (array zero-terminated=1): the parameters it
 *   accepts, ended by an entry whose name is %NULL; copied
 * @result_doc: (nullable): one line on what it returns
 * @func: the implementation
 * @user_data: passed to @func
 * @destroy: (nullable): frees @user_data when the method goes
 *
 * Adds a JSON-RPC method, replacing any of the same name.  The core
 * methods are registered by the compositor (src/ipc/gowl-ipc-methods.c);
 * a module adds its own the same way, so `rpc.discover' lists them
 * and the transport checks their parameters.
 */
void
gowl_ipc_register_method(GowlIpc            *self,
                         const gchar        *name,
                         const gchar        *summary,
                         const GowlIpcParam *params,
                         const gchar        *result_doc,
                         GowlIpcMethodFunc   func,
                         gpointer            user_data,
                         GDestroyNotify      destroy);

/**
 * gowl_ipc_unregister_method:
 * @self: the IPC server
 * @name: a method name
 *
 * Removes a method added by gowl_ipc_register_method().
 *
 * Returns: %TRUE if there was one
 */
gboolean
gowl_ipc_unregister_method(GowlIpc     *self,
                           const gchar *name);

/**
 * gowl_ipc_has_method:
 * @self: the IPC server
 * @name: a method name
 *
 * Returns: whether @name is registered
 */
gboolean
gowl_ipc_has_method(GowlIpc     *self,
                    const gchar *name);

/**
 * gowl_ipc_call:
 * @self: the IPC server
 * @method: a method name
 * @params: (nullable): its parameters
 * @error: return location for a #GError
 *
 * Runs a registered method in process, through the same parameter
 * check a socket request gets, as a caller with pid -1.  For tests and
 * for embedders that want the protocol's semantics without a socket.
 *
 * Returns: (transfer full) (nullable): the result
 */
JsonNode *
gowl_ipc_call(GowlIpc     *self,
              const gchar *method,
              JsonObject  *params,
              GError     **error);

/**
 * GowlIpcEventFunc:
 * @ipc: the IPC server
 * @word: the event's text name, as a text subscriber sees it after
 *   `EVENT ' (`focus', `client-added', ...)
 * @args: (nullable): the rest of the text event line
 * @out_name: (out) (transfer full): the event's JSON name
 *   (`window.focus', ...); left %NULL to keep @word, or set to the
 *   empty string to send JSON subscribers nothing for this text event
 *   (one whose structured form is produced some other, more complete
 *   way)
 * @user_data: what was given to gowl_ipc_set_event_formatter()
 *
 * Turns a text event into the structured data a JSON subscriber gets.
 *
 * Returns: (transfer full) (nullable): the event's `data' object, or
 *   %NULL for the generic {"args": ...} form
 */
typedef JsonNode *(*GowlIpcEventFunc) (GowlIpc     *ipc,
                                       const gchar *word,
                                       const gchar *args,
                                       gchar      **out_name,
                                       gpointer     user_data);

/**
 * gowl_ipc_set_event_formatter:
 * @self: the IPC server
 * @func: (nullable) (scope notified): the formatter
 * @user_data: passed to @func
 *
 * Installs what translates text events for JSON subscribers.  The
 * compositor installs its own; without one every event goes out in the
 * generic form.
 */
void
gowl_ipc_set_event_formatter(GowlIpc          *self,
                             GowlIpcEventFunc  func,
                             gpointer          user_data);

/**
 * gowl_ipc_declare_event:
 * @self: the IPC server
 * @name: a JSON event name
 * @doc: one line on when it fires and what its data holds
 *
 * Lists an event in `rpc.discover' and `events.list'.  Declaring is
 * documentation only: an undeclared event is still delivered.
 */
void
gowl_ipc_declare_event(GowlIpc     *self,
                       const gchar *name,
                       const gchar *doc);

/**
 * gowl_ipc_wants_event:
 * @self: the IPC server
 * @name: a JSON event name
 *
 * Whether any JSON subscriber's filter takes @name.  For an event that
 * costs something to compute, so it is only computed for somebody.
 *
 * Returns: %TRUE if at least one subscriber would receive it
 */
gboolean
gowl_ipc_wants_event(GowlIpc     *self,
                     const gchar *name);

/**
 * gowl_ipc_push_json_event:
 * @self: the IPC server
 * @name: a JSON event name
 * @data: (transfer full) (nullable): the event's data
 *
 * Sends an event to JSON subscribers only -- one with no text form,
 * such as `window.changed'.
 */
void
gowl_ipc_push_json_event(GowlIpc     *self,
                         const gchar *name,
                         JsonNode    *data);

/**
 * gowl_ipc_stop:
 * @self: the IPC server
 *
 * Stops the IPC server and cleans up the socket.
 */
void
gowl_ipc_stop(GowlIpc *self);

/**
 * gowl_ipc_get_socket_path:
 * @self: a #GowlIpc
 *
 * The path the server listens on, whether it was given to
 * gowl_ipc_new() or defaulted to $XDG_RUNTIME_DIR/gowl.sock.
 *
 * Returns: (transfer none): the socket path
 */
const gchar *
gowl_ipc_get_socket_path(GowlIpc *self);

/**
 * gowl_ipc_push_event:
 * @self: the IPC server
 * @format: printf-style format string for the event line
 * @...: arguments for the format string
 *
 * Broadcasts an event line to every text subscriber, and its
 * structured form (see gowl_ipc_set_event_formatter()) to every JSON
 * subscriber whose filter takes it.
 */
void
gowl_ipc_push_event(GowlIpc *self, const gchar *format, ...)
	G_GNUC_PRINTF(2, 3);

/**
 * gowl_ipc_get_subscriber_count:
 * @self: the IPC server
 *
 * Returns the number of currently subscribed clients, text and JSON.
 *
 * Returns: the subscriber count
 */
guint
gowl_ipc_get_subscriber_count(GowlIpc *self);

/**
 * gowl_ipc_get_client_count:
 * @self: the IPC server
 *
 * Returns: the number of open connections
 */
guint
gowl_ipc_get_client_count(GowlIpc *self);

G_END_DECLS

#endif /* GOWL_IPC_H */
