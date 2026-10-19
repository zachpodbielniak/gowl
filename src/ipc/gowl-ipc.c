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
 * The socket's transport: connections, framing, the two dialects, and
 * event fan-out.  What the commands and methods DO lives elsewhere --
 * src/ipc/gowl-ipc-core.c answers the text commands and
 * src/ipc/gowl-ipc-methods.c the JSON-RPC methods.
 *
 * Three rules this file keeps, each the fix for a way the socket used to
 * be able to hurt the compositor it runs inside:
 *
 *   1. NOTHING BLOCKS.  Output is queued per connection and drained when
 *      the socket is writable.  The old send_line() spun on EAGAIN, so a
 *      client that connected and stopped reading -- a `gowl-msg -s' piped
 *      into a pager -- held the compositor's event loop, which is to say
 *      the whole desktop, until it read again.
 *
 *   2. NOTHING IS FREED UNDER ITS OWN FEET.  A failed write marks the
 *      connection dead; it is reaped only when no callback of this server
 *      is on the stack.  The old code freed the client in send_line() and
 *      then kept parsing the next line out of the freed buffer, and a
 *      command whose event went back to the same subscriber could free it
 *      from inside its own reply.
 *
 *   3. NOTHING GROWS WITHOUT BOUND.  A request line is capped at
 *      GOWL_IPC_MAX_LINE, and a connection's unread output at
 *      GOWL_IPC_MAX_PENDING, past which it is dropped.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* struct ucred */
#endif

#include "gowl-ipc.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <fcntl.h>
#include <wayland-server-core.h>

/* How much one read() takes at a time. */
#define GOWL_IPC_READ_CHUNK (65536)

G_DEFINE_QUARK(gowl-ipc-error-quark, gowl_ipc_error)

/**
 * GowlIpcMethod:
 *
 * One registered JSON-RPC method, with its declaration copied so the
 * registrant's strings may be static or not.
 */
typedef struct {
	gchar             *name;
	gchar             *summary;
	gchar             *result_doc;
	GowlIpcParam      *params;      /* owned copies; name==NULL ends it */
	guint              n_params;
	GowlIpcMethodFunc  func;
	gpointer           user_data;
	GDestroyNotify     destroy;
} GowlIpcMethod;

/**
 * GowlIpcClient:
 *
 * One connection.  @subscribed is the text dialect's `subscribe';
 * @json_subscribed is `events.subscribe', with @filters the glob
 * patterns its events are matched against (empty takes everything).
 */
typedef struct {
	int                      fd;
	struct wl_event_source  *event_source;
	GowlIpc                 *server;     /* back-pointer (not owned) */
	GowlIpcCaller            caller;

	gboolean                 subscribed;
	gboolean                 json_subscribed;
	GPtrArray               *filters;    /* gchar* glob patterns */

	/* Input: bytes not yet ending in a newline.  @discarding is set
	 * after an over-long line, until that line's newline goes past. */
	GString                 *inbuf;
	gboolean                 discarding;

	/* Output not yet accepted by the kernel, from @out_off on. */
	GString                 *outq;
	gsize                    out_off;
	gboolean                 want_write;

	/* The peer closed its sending side: no more requests, but the
	 * replies already queued are still owed.  Dead once they are out. */
	gboolean                 closing;

	/* Gone: write failed, hung up, or over its output allowance.
	 * Reaped by reap_dead() once nothing of ours is on the stack. */
	gboolean                 dead;
} GowlIpcClient;

/**
 * GowlIpcEventDecl:
 *
 * An event named in `rpc.discover'.
 */
typedef struct {
	gchar *name;
	gchar *doc;
} GowlIpcEventDecl;

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
	guint                    next_connection;

	/* How many of our callbacks are on the stack; see reap_dead(). */
	guint                    depth;

	/* What answers a text line; see gowl_ipc_set_command_handler() */
	GowlIpcCommandFunc       command_func;
	gpointer                 command_data;

	/* JSON-RPC */
	GHashTable              *methods;       /* name -> GowlIpcMethod* */
	GowlIpcEventFunc         event_func;
	gpointer                 event_data;
	GPtrArray               *event_decls;   /* GowlIpcEventDecl* */
	guint64                  event_seq;
};

G_DEFINE_FINAL_TYPE(GowlIpc, gowl_ipc, G_TYPE_OBJECT)

enum {
	SIGNAL_COMMAND_RECEIVED,
	N_IPC_SIGNALS
};

static guint ipc_signals[N_IPC_SIGNALS];

/* --- Forward declarations --- */

static int  on_client_event(int fd, uint32_t mask, void *data);
static int  on_listen_readable(int fd, uint32_t mask, void *data);
static void register_builtin_methods(GowlIpc *self);

/* ── Small helpers ───────────────────────────────────────────────── */

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

static void
method_free(gpointer data)
{
	GowlIpcMethod *m = (GowlIpcMethod *)data;
	guint i;

	if (m->destroy != NULL)
		m->destroy(m->user_data);
	for (i = 0; i < m->n_params; i++) {
		g_free((gchar *)m->params[i].name);
		g_free((gchar *)m->params[i].doc);
	}
	g_free(m->params);
	g_free(m->name);
	g_free(m->summary);
	g_free(m->result_doc);
	g_free(m);
}

static void
event_decl_free(gpointer data)
{
	GowlIpcEventDecl *d = (GowlIpcEventDecl *)data;

	g_free(d->name);
	g_free(d->doc);
	g_free(d);
}

static const gchar *
param_type_name(GowlIpcParamType t)
{
	switch (t) {
	case GOWL_IPC_PARAM_INT:     return "int";
	case GOWL_IPC_PARAM_NUMBER:  return "number";
	case GOWL_IPC_PARAM_BOOL:    return "bool";
	case GOWL_IPC_PARAM_STRING:  return "string";
	case GOWL_IPC_PARAM_ARRAY:   return "array";
	case GOWL_IPC_PARAM_OBJECT:  return "object";
	case GOWL_IPC_PARAM_TOGGLE:  return "toggle";
	case GOWL_IPC_PARAM_WINDOW:  return "window";
	case GOWL_IPC_PARAM_MONITOR: return "monitor";
	case GOWL_IPC_PARAM_ANY:
	default:                     return "any";
	}
}

/* ── Output ──────────────────────────────────────────────────────── */

/* Arms or disarms the writable interest on a connection's fd. */
static void
client_update_mask(GowlIpcClient *client, gboolean want_write)
{
	if (client->want_write == want_write || client->event_source == NULL)
		return;
	client->want_write = want_write;
	wl_event_source_fd_update(client->event_source,
	                          (client->closing ? 0 : WL_EVENT_READABLE)
	                          | (want_write ? WL_EVENT_WRITABLE : 0));
}

/*
 * Hands the kernel as much queued output as it will take.  Never waits:
 * what it will not take stays queued and the writable interest is armed,
 * so on_client_event() comes back for the rest.
 */
static void
client_flush(GowlIpcClient *client)
{
	while (!client->dead && client->out_off < client->outq->len) {
		ssize_t n = send(client->fd,
		                 client->outq->str + client->out_off,
		                 client->outq->len - client->out_off,
		                 MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			g_debug("gowl-ipc: client fd=%d write failed: %s",
			        client->fd, g_strerror(errno));
			client->dead = TRUE;
			return;
		}
		client->out_off += (gsize)n;
	}

	if (client->out_off >= client->outq->len) {
		g_string_truncate(client->outq, 0);
		client->out_off = 0;
		client_update_mask(client, FALSE);
		/* A subscriber that half-closed (`socat' does on stdin EOF)
		 * still wants its events; it goes when it hangs up. */
		if (client->closing && !client->subscribed
		    && !client->json_subscribed)
			client->dead = TRUE;
	} else {
		/* Compact once the consumed prefix is the bigger part, so a
		 * slow reader's queue does not only ever grow at the end. */
		if (client->out_off > client->outq->len / 2) {
			g_string_erase(client->outq, 0, (gssize)client->out_off);
			client->out_off = 0;
		}
		client_update_mask(client, TRUE);
	}
}

/* Queues @text and a newline for a connection, and tries to send it. */
static void
client_send_line(GowlIpcClient *client, const gchar *text)
{
	if (client->dead)
		return;

	g_string_append(client->outq, text);
	g_string_append_c(client->outq, '\n');

	if (client->outq->len - client->out_off > GOWL_IPC_MAX_PENDING) {
		/* A message, not a warning: the misbehaviour is the
		 * client's, and the compositor handled it. */
		g_message("gowl-ipc: client fd=%d (pid %d) is not reading; "
		          "%zu bytes queued, disconnecting it",
		          client->fd, client->caller.pid,
		          client->outq->len - client->out_off);
		client->dead = TRUE;
		return;
	}
	client_flush(client);
}

static void
client_send_json(GowlIpcClient *client, JsonNode *node)
{
	g_autofree gchar *text = json_to_string(node, FALSE);

	client_send_line(client, text);
}

/* ── Connection lifetime ─────────────────────────────────────────── */

static void
client_free(GowlIpcClient *client)
{
	if (client->event_source != NULL)
		wl_event_source_remove(client->event_source);
	if (client->fd >= 0)
		close(client->fd);
	g_string_free(client->inbuf, TRUE);
	g_string_free(client->outq, TRUE);
	g_ptr_array_unref(client->filters);
	g_free(client);
}

/*
 * Frees every connection marked dead -- only when none of our callbacks
 * is on the stack, which is what keeps a connection alive for the rest
 * of the line, request or event fan-out that found it dead.
 */
static void
reap_dead(GowlIpc *self)
{
	GList *l;
	GList *next;

	if (self->depth > 0)
		return;

	for (l = self->clients; l != NULL; l = next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;

		next = l->next;
		if (!client->dead)
			continue;
		g_debug("gowl-ipc: client fd=%d gone", client->fd);
		self->clients = g_list_delete_link(self->clients, l);
		client_free(client);
	}
}

static GowlIpcClient *
client_by_connection(GowlIpc *self, guint connection)
{
	GList *l;

	if (connection == 0)
		return NULL;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;

		if (client->caller.connection == connection && !client->dead)
			return client;
	}
	return NULL;
}

void
gowl_ipc_set_command_handler(
	GowlIpc            *self,
	GowlIpcCommandFunc  func,
	gpointer            user_data
){
	g_return_if_fail(GOWL_IS_IPC(self));

	self->command_func = func;
	self->command_data = user_data;
}

/* ── The text dialect ────────────────────────────────────────────── */

/**
 * process_text_line:
 * @self: the IPC server
 * @client: the client that sent the line
 * @line: the complete line (without newline)
 *
 * Parses a "command args..." line.  "subscribe" flags the client for
 * text events; everything else is answered by the command handler.
 * Every line gets exactly one line back.
 */
static void
process_text_line(
	GowlIpc       *self,
	GowlIpcClient *client,
	const gchar   *line
){
	g_autofree gchar *command = NULL;
	const gchar *args;
	const gchar *space;

	space = strchr(line, ' ');
	if (space != NULL) {
		command = g_strndup(line, (gsize)(space - line));
		args = space + 1;
	} else {
		command = g_strdup(line);
		args = "";
	}

	if (g_strcmp0(command, "subscribe") == 0) {
		if (!client->subscribed) {
			client->subscribed = TRUE;
			g_debug("gowl-ipc: client fd=%d subscribed", client->fd);
		}
		g_signal_emit(self, ipc_signals[SIGNAL_COMMAND_RECEIVED], 0,
		              command, args);
		client_send_line(client, "OK subscribed");
		return;
	}

	g_signal_emit(self, ipc_signals[SIGNAL_COMMAND_RECEIVED], 0,
	              command, args);

	if (self->command_func != NULL) {
		g_autofree gchar *reply =
			self->command_func(self, line, self->command_data);

		if (reply != NULL) {
			client_send_line(client, reply);
		} else {
			g_autofree gchar *err = g_strdup_printf(
				"ERROR unknown command '%s'", command);
			client_send_line(client, err);
		}
	} else {
		client_send_line(client, "ERROR no command handler");
	}
}

/* ── The JSON-RPC dialect ────────────────────────────────────────── */

static JsonNode *
make_error_response(JsonNode *id, gint code, const gchar *message,
                    const gchar *method)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *out;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	if (id != NULL)
		json_builder_add_value(b, json_node_copy(id));
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "error");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "code");
	json_builder_add_int_value(b, code);
	json_builder_set_member_name(b, "message");
	json_builder_add_string_value(b, message != NULL ? message : "error");
	if (method != NULL) {
		json_builder_set_member_name(b, "data");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "method");
		json_builder_add_string_value(b, method);
		json_builder_end_object(b);
	}
	json_builder_end_object(b);
	json_builder_end_object(b);

	out = json_builder_get_root(b);
	g_object_unref(b);
	return out;
}

static JsonNode *
make_result_response(JsonNode *id, JsonNode *result)
{
	JsonObject *o = json_object_new();
	JsonNode *out = json_node_new(JSON_NODE_OBJECT);

	json_object_set_string_member(o, "jsonrpc", "2.0");
	json_object_set_member(o, "id", json_node_copy(id));
	json_object_set_member(o, "result",
	                       result != NULL ? result
	                                      : json_node_new(JSON_NODE_NULL));
	json_node_take_object(out, o);
	return out;
}

/* Whether @node is a whole number, written as one or as 3.0. */
static gboolean
node_is_int(JsonNode *node)
{
	GType t;

	if (!JSON_NODE_HOLDS_VALUE(node))
		return FALSE;
	t = json_node_get_value_type(node);
	if (t == G_TYPE_INT64)
		return TRUE;
	if (t == G_TYPE_DOUBLE) {
		gdouble d = json_node_get_double(node);

		return d == (gdouble)(gint64)d;
	}
	return FALSE;
}

static gboolean
node_is_type(JsonNode *node, GowlIpcParamType type)
{
	GType vt = JSON_NODE_HOLDS_VALUE(node)
		? json_node_get_value_type(node) : G_TYPE_INVALID;

	switch (type) {
	case GOWL_IPC_PARAM_ANY:
		return TRUE;
	case GOWL_IPC_PARAM_INT:
		return node_is_int(node);
	case GOWL_IPC_PARAM_NUMBER:
		return vt == G_TYPE_INT64 || vt == G_TYPE_DOUBLE;
	case GOWL_IPC_PARAM_BOOL:
		return vt == G_TYPE_BOOLEAN;
	case GOWL_IPC_PARAM_STRING:
	case GOWL_IPC_PARAM_MONITOR:
		return vt == G_TYPE_STRING;
	case GOWL_IPC_PARAM_ARRAY:
		return JSON_NODE_HOLDS_ARRAY(node);
	case GOWL_IPC_PARAM_OBJECT:
		return JSON_NODE_HOLDS_OBJECT(node);
	case GOWL_IPC_PARAM_TOGGLE:
		return vt == G_TYPE_BOOLEAN
		       || (vt == G_TYPE_STRING
		           && g_strcmp0(json_node_get_string(node), "toggle") == 0);
	case GOWL_IPC_PARAM_WINDOW:
		return node_is_int(node)
		       || (vt == G_TYPE_STRING
		           && g_strcmp0(json_node_get_string(node), "focused") == 0);
	default:
		return FALSE;
	}
}

static const GowlIpcParam *
method_param(const GowlIpcMethod *m, const gchar *name)
{
	guint i;

	for (i = 0; i < m->n_params; i++)
		if (g_strcmp0(m->params[i].name, name) == 0)
			return &m->params[i];
	return NULL;
}

/*
 * Checks @params against @m's declaration and returns the object the
 * method sees: the same members, minus any that are JSON null (which a
 * client may send for "not given").  Unknown members, missing required
 * ones and wrong types are errors -- a typo in a parameter name must
 * not silently become the default.
 */
static JsonObject *
check_params(const GowlIpcMethod *m, JsonObject *params, GError **error)
{
	JsonObject *out = json_object_new();
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *value;
	guint i;

	if (params != NULL) {
		json_object_iter_init(&iter, params);
		while (json_object_iter_next(&iter, &name, &value)) {
			const GowlIpcParam *p = method_param(m, name);

			if (p == NULL) {
				GString *known = g_string_new(NULL);

				for (i = 0; i < m->n_params; i++)
					g_string_append_printf(known, "%s%s",
						i > 0 ? ", " : "", m->params[i].name);
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_INVALID_PARAMS,
				            "%s takes no parameter '%s' (it takes: %s)",
				            m->name, name,
				            known->len > 0 ? known->str : "nothing");
				g_string_free(known, TRUE);
				json_object_unref(out);
				return NULL;
			}
			if (JSON_NODE_HOLDS_NULL(value))
				continue;
			if (!node_is_type(value, p->type)) {
				g_set_error(error, GOWL_IPC_ERROR,
				            GOWL_IPC_ERROR_INVALID_PARAMS,
				            "%s: '%s' must be %s", m->name, name,
				            param_type_name(p->type));
				json_object_unref(out);
				return NULL;
			}
			json_object_set_member(out, name, json_node_copy(value));
		}
	}

	for (i = 0; i < m->n_params; i++) {
		if (m->params[i].required
		    && !json_object_has_member(out, m->params[i].name)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "%s needs '%s'", m->name, m->params[i].name);
			json_object_unref(out);
			return NULL;
		}
	}
	return out;
}

/*
 * Runs a method by name: looks it up, checks its parameters, calls it.
 * The one path both the socket and gowl_ipc_call() go through.
 */
static JsonNode *
invoke(GowlIpc *self, const GowlIpcCaller *caller, const gchar *method,
       JsonObject *params, GError **error)
{
	GowlIpcMethod *m;
	JsonObject *checked;
	JsonNode *result;

	m = (GowlIpcMethod *)g_hash_table_lookup(self->methods, method);
	if (m == NULL) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_METHOD_NOT_FOUND,
		            "unknown method '%s' (rpc.discover lists them)",
		            method);
		return NULL;
	}

	checked = check_params(m, params, error);
	if (checked == NULL)
		return NULL;

	result = m->func(self, caller, checked, m->user_data, error);
	json_object_unref(checked);
	return result;
}

/*
 * Handles one request object, from a line or from inside a batch.
 *
 * Returns: (transfer full) (nullable): the response, or %NULL for a
 *   notification, which JSON-RPC answers with nothing
 */
static JsonNode *
rpc_one(GowlIpc *self, GowlIpcClient *client, JsonNode *req)
{
	JsonObject *o;
	JsonNode *id = NULL;
	JsonNode *params_node;
	JsonObject *params = NULL;
	const gchar *method;
	gboolean is_notification;
	g_autoptr(GError) error = NULL;
	JsonNode *result;

	if (!JSON_NODE_HOLDS_OBJECT(req))
		return make_error_response(NULL, GOWL_IPC_ERROR_INVALID_REQUEST,
		                           "a request is a JSON object", NULL);
	o = json_node_get_object(req);

	/* The id, if any: a string, an integer or null.  An id that is
	 * none of those cannot be echoed back meaningfully. */
	is_notification = !json_object_has_member(o, "id");
	if (!is_notification) {
		id = json_object_get_member(o, "id");
		if (!JSON_NODE_HOLDS_NULL(id)
		    && !(JSON_NODE_HOLDS_VALUE(id)
		         && (json_node_get_value_type(id) == G_TYPE_STRING
		             || json_node_get_value_type(id) == G_TYPE_INT64
		             || json_node_get_value_type(id) == G_TYPE_DOUBLE)))
			return make_error_response(NULL,
				GOWL_IPC_ERROR_INVALID_REQUEST,
				"id must be a string, a number or null", NULL);
	}

	/* "jsonrpc" may be left out (it is noise in a shell one-liner),
	 * but if it is there it must say 2.0. */
	if (json_object_has_member(o, "jsonrpc")
	    && g_strcmp0(json_object_get_string_member_with_default(
	                     o, "jsonrpc", ""), "2.0") != 0)
		return is_notification ? NULL : make_error_response(id,
			GOWL_IPC_ERROR_INVALID_REQUEST,
			"only JSON-RPC 2.0 is spoken here", NULL);

	method = json_object_has_member(o, "method")
		&& JSON_NODE_HOLDS_VALUE(json_object_get_member(o, "method"))
		&& json_node_get_value_type(json_object_get_member(o, "method"))
		   == G_TYPE_STRING
		? json_object_get_string_member(o, "method") : NULL;
	if (method == NULL)
		return is_notification ? NULL : make_error_response(id,
			GOWL_IPC_ERROR_INVALID_REQUEST,
			"a request needs a string \"method\"", NULL);

	params_node = json_object_has_member(o, "params")
		? json_object_get_member(o, "params") : NULL;
	if (params_node != NULL && !JSON_NODE_HOLDS_NULL(params_node)) {
		if (!JSON_NODE_HOLDS_OBJECT(params_node))
			return is_notification ? NULL : make_error_response(id,
				GOWL_IPC_ERROR_INVALID_PARAMS,
				"params are named: send an object, not an array",
				method);
		params = json_node_get_object(params_node);
	}

	result = invoke(self, &client->caller, method, params, &error);

	if (is_notification) {
		if (error != NULL)
			g_debug("gowl-ipc: notification %s failed: %s",
			        method, error->message);
		if (result != NULL)
			json_node_unref(result);
		return NULL;
	}
	if (error != NULL) {
		gint code = error->domain == GOWL_IPC_ERROR
			? error->code : GOWL_IPC_ERROR_FAILED;

		if (result != NULL)
			json_node_unref(result);
		return make_error_response(id, code, error->message, method);
	}
	return make_result_response(id, result);
}

/*
 * One JSON-RPC line: a request, or a batch of them.  A batch's
 * requests run in order inside this one callback, so no frame is drawn
 * between them -- a batch that moves six windows lands as one picture.
 */
static void
process_rpc_line(GowlIpc *self, GowlIpcClient *client, const gchar *line)
{
	g_autoptr(JsonParser) parser = json_parser_new_immutable();
	g_autoptr(GError) error = NULL;
	JsonNode *root;
	JsonNode *resp;

	if (!json_parser_load_from_data(parser, line, -1, &error)) {
		resp = make_error_response(NULL, GOWL_IPC_ERROR_PARSE,
		                           error->message, NULL);
		client_send_json(client, resp);
		json_node_unref(resp);
		return;
	}
	root = json_parser_get_root(parser);

	if (JSON_NODE_HOLDS_ARRAY(root)) {
		JsonArray *batch = json_node_get_array(root);
		JsonArray *out;
		JsonNode *out_node;
		guint i, n = json_array_get_length(batch);

		if (n == 0) {
			resp = make_error_response(NULL,
				GOWL_IPC_ERROR_INVALID_REQUEST,
				"an empty batch", NULL);
			client_send_json(client, resp);
			json_node_unref(resp);
			return;
		}
		out = json_array_new();
		for (i = 0; i < n && !client->dead; i++) {
			resp = rpc_one(self, client,
			               json_array_get_element(batch, i));
			if (resp != NULL)
				json_array_add_element(out, resp);
		}
		/* A batch of notifications only is answered with nothing. */
		if (json_array_get_length(out) > 0) {
			out_node = json_node_new(JSON_NODE_ARRAY);
			json_node_take_array(out_node, out);
			client_send_json(client, out_node);
			json_node_unref(out_node);
		} else {
			json_array_unref(out);
		}
		return;
	}

	resp = rpc_one(self, client, root);
	if (resp != NULL) {
		client_send_json(client, resp);
		json_node_unref(resp);
	}
}

/* Which dialect a line is in: the first non-blank byte decides. */
static void
process_line(GowlIpc *self, GowlIpcClient *client, const gchar *line)
{
	const gchar *p = line;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0')
		return;
	if (*p == '{' || *p == '[')
		process_rpc_line(self, client, p);
	else
		process_text_line(self, client, line);
}

/* ── Reading ─────────────────────────────────────────────────────── */

/* An over-long line is answered in the dialect it seems to be in. */
static void
reply_line_too_long(GowlIpcClient *client)
{
	const gchar *p = client->inbuf->str;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '{' || *p == '[') {
		g_autofree gchar *msg = g_strdup_printf(
			"request longer than %d bytes", GOWL_IPC_MAX_LINE);
		JsonNode *resp = make_error_response(NULL,
			GOWL_IPC_ERROR_INVALID_REQUEST, msg, NULL);

		client_send_json(client, resp);
		json_node_unref(resp);
	} else {
		client_send_line(client, "ERROR line too long");
	}
}

/* Splits whatever complete lines the input buffer holds and runs them. */
static void
client_drain_lines(GowlIpc *self, GowlIpcClient *client)
{
	gchar *nl;

	while (!client->dead
	       && (nl = memchr(client->inbuf->str, '\n',
	                       client->inbuf->len)) != NULL) {
		gsize len = (gsize)(nl - client->inbuf->str);
		g_autofree gchar *line = g_strndup(client->inbuf->str, len);

		g_string_erase(client->inbuf, 0, (gssize)len + 1);
		if (client->discarding) {
			/* The tail of a line already refused as too long. */
			client->discarding = FALSE;
			continue;
		}
		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = '\0';
		process_line(self, client, line);
	}

	if (!client->dead && client->inbuf->len > GOWL_IPC_MAX_LINE) {
		if (!client->discarding)
			reply_line_too_long(client);
		g_message("gowl-ipc: client fd=%d sent a line over %d bytes; "
		          "discarding it", client->fd, GOWL_IPC_MAX_LINE);
		g_string_truncate(client->inbuf, 0);
		client->discarding = TRUE;
	}
}

/**
 * on_client_event:
 * @fd: the client socket fd
 * @mask: wl_event mask
 * @data: the #GowlIpcClient
 *
 * Readable: read what is there and run every complete line.  Writable:
 * send queued output.  Hangup or error: the connection is done.
 * Dead connections are reaped on the way out.
 */
static int
on_client_event(int fd, uint32_t mask, void *data)
{
	GowlIpcClient *client = (GowlIpcClient *)data;
	GowlIpc *self = client->server;

	g_object_ref(self);
	self->depth++;

	if (mask & WL_EVENT_WRITABLE)
		client_flush(client);

	if (!client->dead && !client->closing && (mask & WL_EVENT_READABLE)) {
		gchar chunk[GOWL_IPC_READ_CHUNK];
		ssize_t n = read(fd, chunk, sizeof chunk);

		if (n > 0) {
			if (client->discarding) {
				/* Still inside a refused line: keep only what
				 * follows its newline. */
				gchar *nl = memchr(chunk, '\n', (gsize)n);

				if (nl != NULL) {
					client->discarding = FALSE;
					g_string_append_len(client->inbuf, nl + 1,
					                    (gssize)(n - (nl + 1 - chunk)));
				}
			} else {
				g_string_append_len(client->inbuf, chunk, n);
			}
			client_drain_lines(self, client);
		} else if (n == 0) {
			/* A half-close: `printf ... | gowl-msg --pipe' sends its
			 * requests and shuts its side.  The replies are queued
			 * already -- every line is answered as it is read -- so
			 * stop reading, finish sending, and go. */
			g_debug("gowl-ipc: client fd=%d EOF", fd);
			client->closing = TRUE;
			if (client->outq->len == client->out_off) {
				if (!client->subscribed && !client->json_subscribed)
					client->dead = TRUE;
				else
					wl_event_source_fd_update(
						client->event_source, 0);
			} else {
				wl_event_source_fd_update(client->event_source,
				                          WL_EVENT_WRITABLE);
				client->want_write = TRUE;
			}
		} else if (errno != EAGAIN && errno != EWOULDBLOCK
		           && errno != EINTR) {
			g_debug("gowl-ipc: client fd=%d read %s", fd,
			        g_strerror(errno));
			client->dead = TRUE;
		}
	}

	if (!client->dead && (mask & WL_EVENT_ERROR))
		client->dead = TRUE;
	/* HANGUP with replies still queued is a half-close as well on some
	 * kernels; only a hangup with nothing owed ends it here. */
	if (!client->dead && (mask & WL_EVENT_HANGUP)
	    && client->outq->len == client->out_off)
		client->dead = TRUE;

	self->depth--;
	reap_dead(self);
	g_object_unref(self);
	return 0;
}

/* --- Listener readable callback --- */

/**
 * on_listen_readable:
 * @fd: the listening socket fd
 * @mask: wl_event mask
 * @data: pointer to GowlIpc
 *
 * Accepts a pending connection, records who it is (SO_PEERCRED), and
 * starts watching it.
 */
static int
on_listen_readable(int fd, uint32_t mask, void *data)
{
	GowlIpc *self;
	GowlIpcClient *client;
	int client_fd;
	struct ucred cred;
	socklen_t cred_len = sizeof cred;

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
	client->inbuf       = g_string_new(NULL);
	client->outq        = g_string_new(NULL);
	client->filters     = g_ptr_array_new_with_free_func(g_free);
	client->caller.pid  = -1;
	client->caller.uid  = (guint)getuid();
	client->caller.connection = ++self->next_connection;

	/* Who is calling: what "my windows" means for this connection. */
	memset(&cred, 0, sizeof cred);
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0
	    && cred.pid > 0) {
		client->caller.pid = (gint)cred.pid;
		client->caller.uid = (guint)cred.uid;
	}

	client->event_source = wl_event_loop_add_fd(
		self->event_loop, client_fd,
		WL_EVENT_READABLE,
		on_client_event, client);

	self->clients = g_list_prepend(self->clients, client);

	g_debug("gowl-ipc: client connected (fd=%d pid=%d)", client_fd,
	        client->caller.pid);
	return 0;
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

	/* Methods hold user data (a compositor weak ref, say) whose
	 * destroy notify may want the object still whole. */
	g_clear_pointer(&self->methods, g_hash_table_unref);

	G_OBJECT_CLASS(gowl_ipc_parent_class)->dispose(object);
}

static void
gowl_ipc_finalize(GObject *object)
{
	GowlIpc *self;

	self = GOWL_IPC(object);

	g_free(self->socket_path);
	g_clear_pointer(&self->event_decls, g_ptr_array_unref);

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
	 * Emitted when a text-dialect command is received from a client,
	 * before it is answered.  JSON-RPC requests do not emit it.
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
	self->methods        = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                             NULL, method_free);
	self->event_decls    = g_ptr_array_new_with_free_func(event_decl_free);

	register_builtin_methods(self);
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
 * listening for incoming connections, dispatched from @event_loop so
 * the server runs inside the compositor's own loop.
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
 * gowl_ipc_get_socket_path:
 * @self: a #GowlIpc
 *
 * Returns: (transfer none): the path the server listens on
 */
const gchar *
gowl_ipc_get_socket_path(GowlIpc *self)
{
	g_return_val_if_fail(GOWL_IS_IPC(self), NULL);
	return self->socket_path;
}

/**
 * gowl_ipc_stop:
 * @self: the IPC server
 *
 * Stops the IPC server, closes all client connections, and removes the
 * socket file.  Safe from inside a request: the connections are marked
 * dead and reaped once the request returns.
 */
void
gowl_ipc_stop(GowlIpc *self)
{
	GList *l;

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

	/* Every connection goes; reaped now, or when the request that
	 * stopped us returns. */
	for (l = self->clients; l != NULL; l = l->next)
		((GowlIpcClient *)l->data)->dead = TRUE;
	reap_dead(self);

	/* Remove the socket file */
	g_unlink(self->socket_path);

	g_debug("gowl-ipc: stopped");
}

/* ── Methods ─────────────────────────────────────────────────────── */

void
gowl_ipc_register_method(
	GowlIpc            *self,
	const gchar        *name,
	const gchar        *summary,
	const GowlIpcParam *params,
	const gchar        *result_doc,
	GowlIpcMethodFunc   func,
	gpointer            user_data,
	GDestroyNotify      destroy
){
	GowlIpcMethod *m;
	guint n = 0;
	guint i;

	g_return_if_fail(GOWL_IS_IPC(self));
	g_return_if_fail(name != NULL && *name != '\0');
	g_return_if_fail(func != NULL);

	while (params != NULL && params[n].name != NULL)
		n++;

	m = g_new0(GowlIpcMethod, 1);
	m->name       = g_strdup(name);
	m->summary    = g_strdup(summary != NULL ? summary : "");
	m->result_doc = g_strdup(result_doc);
	m->params     = g_new0(GowlIpcParam, n + 1);
	m->n_params   = n;
	for (i = 0; i < n; i++) {
		m->params[i].name     = g_strdup(params[i].name);
		m->params[i].type     = params[i].type;
		m->params[i].required = params[i].required;
		m->params[i].doc      = g_strdup(params[i].doc != NULL
		                                 ? params[i].doc : "");
	}
	m->func      = func;
	m->user_data = user_data;
	m->destroy   = destroy;

	/* The key is the method's own name string, so replacing frees the
	 * old method (and its key) through method_free. */
	g_hash_table_remove(self->methods, name);
	g_hash_table_insert(self->methods, m->name, m);
}

gboolean
gowl_ipc_unregister_method(GowlIpc *self, const gchar *name)
{
	g_return_val_if_fail(GOWL_IS_IPC(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	return self->methods != NULL
	       && g_hash_table_remove(self->methods, name);
}

gboolean
gowl_ipc_has_method(GowlIpc *self, const gchar *name)
{
	g_return_val_if_fail(GOWL_IS_IPC(self), FALSE);

	return name != NULL && self->methods != NULL
	       && g_hash_table_contains(self->methods, name);
}

JsonNode *
gowl_ipc_call(
	GowlIpc     *self,
	const gchar *method,
	JsonObject  *params,
	GError     **error
){
	GowlIpcCaller caller;
	JsonNode *result;

	g_return_val_if_fail(GOWL_IS_IPC(self), NULL);
	g_return_val_if_fail(method != NULL, NULL);

	caller.pid = -1;
	caller.uid = (guint)getuid();
	caller.connection = 0;

	g_object_ref(self);
	self->depth++;
	result = invoke(self, &caller, method, params, error);
	self->depth--;
	reap_dead(self);
	g_object_unref(self);
	return result;
}

/* ── Events ──────────────────────────────────────────────────────── */

void
gowl_ipc_set_event_formatter(
	GowlIpc          *self,
	GowlIpcEventFunc  func,
	gpointer          user_data
){
	g_return_if_fail(GOWL_IS_IPC(self));

	self->event_func = func;
	self->event_data = user_data;
}

void
gowl_ipc_declare_event(GowlIpc *self, const gchar *name, const gchar *doc)
{
	GowlIpcEventDecl *d;
	guint i;

	g_return_if_fail(GOWL_IS_IPC(self));
	g_return_if_fail(name != NULL);

	for (i = 0; i < self->event_decls->len; i++) {
		d = g_ptr_array_index(self->event_decls, i);
		if (g_strcmp0(d->name, name) == 0) {
			g_free(d->doc);
			d->doc = g_strdup(doc != NULL ? doc : "");
			return;
		}
	}
	d = g_new0(GowlIpcEventDecl, 1);
	d->name = g_strdup(name);
	d->doc  = g_strdup(doc != NULL ? doc : "");
	g_ptr_array_add(self->event_decls, d);
}

/* Whether a connection's filter takes an event of this name. */
static gboolean
client_takes(GowlIpcClient *client, const gchar *name)
{
	guint i;

	if (client->dead || !client->json_subscribed)
		return FALSE;
	if (client->filters->len == 0)
		return TRUE;
	for (i = 0; i < client->filters->len; i++)
		if (g_pattern_match_simple(
		        (const gchar *)g_ptr_array_index(client->filters, i),
		        name))
			return TRUE;
	return FALSE;
}

gboolean
gowl_ipc_wants_event(GowlIpc *self, const gchar *name)
{
	GList *l;

	g_return_val_if_fail(GOWL_IS_IPC(self), FALSE);

	for (l = self->clients; l != NULL; l = l->next)
		if (client_takes((GowlIpcClient *)l->data, name))
			return TRUE;
	return FALSE;
}

static gboolean
any_json_subscriber(GowlIpc *self)
{
	GList *l;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;

		if (!client->dead && client->json_subscribed)
			return TRUE;
	}
	return FALSE;
}

/*
 * Sends one structured event to every JSON subscriber that takes it, as
 * a JSON-RPC notification:
 *
 *   {"jsonrpc":"2.0","method":"event","params":{"event":NAME,
 *    "seq":N,"time":MS,"data":{...},"text":"EVENT ..."}}
 *
 * @text is the text form when there is one.  Takes @data.
 */
static void
fan_out_json(GowlIpc *self, const gchar *name, JsonNode *data,
             const gchar *text)
{
	JsonObject *params;
	JsonObject *msg;
	JsonNode *node;
	g_autofree gchar *line = NULL;
	GList *l;
	gboolean any = FALSE;

	for (l = self->clients; l != NULL && !any; l = l->next)
		any = client_takes((GowlIpcClient *)l->data, name);
	if (!any) {
		if (data != NULL)
			json_node_unref(data);
		return;
	}

	params = json_object_new();
	json_object_set_string_member(params, "event", name);
	json_object_set_int_member(params, "seq", (gint64)++self->event_seq);
	json_object_set_int_member(params, "time", g_get_real_time() / 1000);
	if (data == NULL) {
		data = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(data, json_object_new());
	}
	json_object_set_member(params, "data", data);
	if (text != NULL)
		json_object_set_string_member(params, "text", text);

	msg = json_object_new();
	json_object_set_string_member(msg, "jsonrpc", "2.0");
	json_object_set_string_member(msg, "method", "event");
	json_object_set_object_member(msg, "params", params);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, msg);
	line = json_to_string(node, FALSE);
	json_node_unref(node);

	for (l = self->clients; l != NULL; l = l->next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;

		if (client_takes(client, name))
			client_send_line(client, line);
	}
}

void
gowl_ipc_push_json_event(GowlIpc *self, const gchar *name, JsonNode *data)
{
	g_return_if_fail(GOWL_IS_IPC(self));
	g_return_if_fail(name != NULL);

	self->depth++;
	fan_out_json(self, name, data, NULL);
	self->depth--;
	reap_dead(self);
}

/**
 * gowl_ipc_push_event:
 * @self: the IPC server
 * @format: printf-style format string for the event line
 * @...: arguments for the format string
 *
 * Broadcasts a text event line to every text subscriber, and its
 * structured form to every JSON subscriber whose filter takes it.
 */
void
gowl_ipc_push_event(GowlIpc *self, const gchar *format, ...)
{
	va_list ap;
	g_autofree gchar *body = NULL;
	GList *l;
	gboolean text_subs = FALSE;
	gboolean json_subs;

	g_return_if_fail(GOWL_IS_IPC(self));

	for (l = self->clients; l != NULL && !text_subs; l = l->next) {
		GowlIpcClient *client = (GowlIpcClient *)l->data;

		text_subs = client->subscribed && !client->dead;
	}
	json_subs = any_json_subscriber(self);
	if (!text_subs && !json_subs)
		return;

	va_start(ap, format);
	body = g_strdup_vprintf(format, ap);
	va_end(ap);

	self->depth++;

	if (text_subs) {
		for (l = self->clients; l != NULL; l = l->next) {
			GowlIpcClient *client = (GowlIpcClient *)l->data;

			if (client->subscribed)
				client_send_line(client, body);
		}
	}

	if (json_subs) {
		/* "EVENT focus 12" -> word "focus", args "12".  An event
		 * pushed without the EVENT prefix (cmacs's
		 * `gowl-ipc-push-event' takes any string) is the same
		 * shape with no prefix to strip. */
		const gchar *rest = g_str_has_prefix(body, "EVENT ")
			? body + 6 : body;
		const gchar *space = strchr(rest, ' ');
		g_autofree gchar *word = space != NULL
			? g_strndup(rest, (gsize)(space - rest)) : g_strdup(rest);
		const gchar *args = space != NULL ? space + 1 : NULL;
		g_autofree gchar *name = NULL;
		JsonNode *data = NULL;

		if (self->event_func != NULL)
			data = self->event_func(self, word, args, &name,
			                        self->event_data);
		if (name != NULL && *name == '\0') {
			/* The formatter says JSON subscribers get this one some
			 * other way. */
			if (data != NULL)
				json_node_unref(data);
			goto out;
		}
		if (data == NULL) {
			JsonObject *o = json_object_new();

			json_object_set_string_member(o, "args",
				args != NULL ? args : "");
			data = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(data, o);
		}
		fan_out_json(self, name != NULL ? name : word, data, body);
	}

out:
	self->depth--;
	reap_dead(self);
}

/**
 * gowl_ipc_get_subscriber_count:
 * @self: the IPC server
 *
 * Returns the number of currently subscribed clients, text and JSON.
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

		if (!client->dead && (client->subscribed || client->json_subscribed))
			count++;
	}

	return count;
}

guint
gowl_ipc_get_client_count(GowlIpc *self)
{
	GList *l;
	guint count = 0;

	g_return_val_if_fail(GOWL_IS_IPC(self), 0);

	for (l = self->clients; l != NULL; l = l->next)
		if (!((GowlIpcClient *)l->data)->dead)
			count++;
	return count;
}

/* ── The transport's own methods ─────────────────────────────────── */

/* rpc.discover: the protocol described by the running compositor. */
static JsonNode *
m_rpc_discover(GowlIpc *self, const GowlIpcCaller *caller,
               JsonObject *params, gpointer data, GError **error)
{
	const gchar *only = json_object_get_string_member_with_default(
		params, "method", NULL);
	g_autoptr(GList) names = NULL;
	JsonBuilder *b;
	JsonNode *out;
	GList *l;
	guint i;

	(void)data;

	if (only != NULL && !g_hash_table_contains(self->methods, only)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_NOT_FOUND,
		            "no method '%s'", only);
		return NULL;
	}

	names = g_list_sort(g_hash_table_get_keys(self->methods),
	                    (GCompareFunc)g_strcmp0);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "protocol");
	json_builder_add_string_value(b, GOWL_IPC_PROTOCOL_NAME);
	json_builder_set_member_name(b, "version");
	json_builder_add_int_value(b, GOWL_IPC_PROTOCOL_VERSION);
	json_builder_set_member_name(b, "server");
	json_builder_add_string_value(b, "gowl " GOWL_VERSION);
	json_builder_set_member_name(b, "caller");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "pid");
	json_builder_add_int_value(b, caller->pid);
	json_builder_set_member_name(b, "uid");
	json_builder_add_int_value(b, caller->uid);
	json_builder_set_member_name(b, "connection");
	json_builder_add_int_value(b, caller->connection);
	json_builder_end_object(b);

	json_builder_set_member_name(b, "methods");
	json_builder_begin_array(b);
	for (l = names; l != NULL; l = l->next) {
		GowlIpcMethod *m = g_hash_table_lookup(self->methods, l->data);

		if (only != NULL && g_strcmp0(only, m->name) != 0)
			continue;
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, m->name);
		json_builder_set_member_name(b, "summary");
		json_builder_add_string_value(b, m->summary);
		json_builder_set_member_name(b, "params");
		json_builder_begin_array(b);
		for (i = 0; i < m->n_params; i++) {
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "name");
			json_builder_add_string_value(b, m->params[i].name);
			json_builder_set_member_name(b, "type");
			json_builder_add_string_value(b,
				param_type_name(m->params[i].type));
			json_builder_set_member_name(b, "required");
			json_builder_add_boolean_value(b, m->params[i].required);
			json_builder_set_member_name(b, "doc");
			json_builder_add_string_value(b, m->params[i].doc);
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		json_builder_set_member_name(b, "result");
		json_builder_add_string_value(b,
			m->result_doc != NULL ? m->result_doc : "");
		json_builder_end_object(b);
	}
	json_builder_end_array(b);

	json_builder_set_member_name(b, "events");
	json_builder_begin_array(b);
	for (i = 0; only == NULL && i < self->event_decls->len; i++) {
		GowlIpcEventDecl *d = g_ptr_array_index(self->event_decls, i);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, d->name);
		json_builder_set_member_name(b, "doc");
		json_builder_add_string_value(b, d->doc);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);

	out = json_builder_get_root(b);
	g_object_unref(b);
	return out;
}

static JsonNode *
m_rpc_ping(GowlIpc *self, const GowlIpcCaller *caller, JsonObject *params,
           gpointer data, GError **error)
{
	JsonNode *n = json_node_new(JSON_NODE_VALUE);

	(void)self; (void)caller; (void)params; (void)data; (void)error;
	json_node_set_string(n, "pong");
	return n;
}

static JsonNode *
subscription_result(GowlIpcClient *client)
{
	JsonObject *o = json_object_new();
	JsonArray *a = json_array_new();
	JsonNode *n = json_node_new(JSON_NODE_OBJECT);
	guint i;

	for (i = 0; i < client->filters->len; i++)
		json_array_add_string_element(a,
			(const gchar *)g_ptr_array_index(client->filters, i));
	json_object_set_boolean_member(o, "subscribed", client->json_subscribed);
	json_object_set_array_member(o, "events", a);
	json_node_take_object(n, o);
	return n;
}

/* events.subscribe: start (or change) this connection's event stream. */
static JsonNode *
m_events_subscribe(GowlIpc *self, const GowlIpcCaller *caller,
                   JsonObject *params, gpointer data, GError **error)
{
	GowlIpcClient *client = client_by_connection(self, caller->connection);
	JsonArray *events;
	gboolean add;
	guint i;

	(void)data;

	if (client == NULL) {
		g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
		                    "events need a socket connection to arrive on");
		return NULL;
	}

	events = json_object_has_member(params, "events")
		? json_object_get_array_member(params, "events") : NULL;
	add = json_object_get_boolean_member_with_default(params, "add", FALSE);

	/* Checked before anything changes, so a bad pattern list leaves
	 * the old subscription as it was. */
	for (i = 0; events != NULL && i < json_array_get_length(events); i++) {
		JsonNode *e = json_array_get_element(events, i);

		if (!JSON_NODE_HOLDS_VALUE(e)
		    || json_node_get_value_type(e) != G_TYPE_STRING) {
			g_set_error_literal(error, GOWL_IPC_ERROR,
			                    GOWL_IPC_ERROR_INVALID_PARAMS,
			                    "events.subscribe: 'events' is a list "
			                    "of name patterns");
			return NULL;
		}
	}

	if (!add)
		g_ptr_array_set_size(client->filters, 0);
	for (i = 0; events != NULL && i < json_array_get_length(events); i++) {
		const gchar *pat = json_array_get_string_element(events, i);

		/* "*" is the same as no filter; keeping it would only make
		 * every event pay for a pattern match. */
		if (g_strcmp0(pat, "*") == 0) {
			g_ptr_array_set_size(client->filters, 0);
			break;
		}
		g_ptr_array_add(client->filters, g_strdup(pat));
	}
	client->json_subscribed = TRUE;
	return subscription_result(client);
}

static JsonNode *
m_events_unsubscribe(GowlIpc *self, const GowlIpcCaller *caller,
                     JsonObject *params, gpointer data, GError **error)
{
	GowlIpcClient *client = client_by_connection(self, caller->connection);

	(void)params; (void)data;

	if (client == NULL) {
		g_set_error_literal(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
		                    "events need a socket connection");
		return NULL;
	}
	client->json_subscribed = FALSE;
	g_ptr_array_set_size(client->filters, 0);
	return subscription_result(client);
}

static JsonNode *
m_events_list(GowlIpc *self, const GowlIpcCaller *caller,
              JsonObject *params, gpointer data, GError **error)
{
	JsonArray *a = json_array_new();
	JsonNode *n = json_node_new(JSON_NODE_ARRAY);
	guint i;

	(void)caller; (void)params; (void)data; (void)error;

	for (i = 0; i < self->event_decls->len; i++) {
		GowlIpcEventDecl *d = g_ptr_array_index(self->event_decls, i);
		JsonObject *o = json_object_new();

		json_object_set_string_member(o, "name", d->name);
		json_object_set_string_member(o, "doc", d->doc);
		json_array_add_object_element(a, o);
	}
	json_node_take_array(n, a);
	return n;
}

static void
register_builtin_methods(GowlIpc *self)
{
	static const GowlIpcParam discover_params[] = {
		{ "method", GOWL_IPC_PARAM_STRING, FALSE,
		  "describe only this method" },
		{ NULL, 0, FALSE, NULL }
	};
	static const GowlIpcParam subscribe_params[] = {
		{ "events", GOWL_IPC_PARAM_ARRAY, FALSE,
		  "glob patterns over event names (\"window.*\"); absent or "
		  "[\"*\"] takes every event" },
		{ "add", GOWL_IPC_PARAM_BOOL, FALSE,
		  "add these patterns to the current ones instead of replacing "
		  "them" },
		{ NULL, 0, FALSE, NULL }
	};

	gowl_ipc_register_method(self, "rpc.discover",
		"Describe the protocol: version, the caller, every method with "
		"its parameters, every event",
		discover_params,
		"{protocol, version, server, caller, methods[], events[]}",
		m_rpc_discover, NULL, NULL);
	gowl_ipc_register_method(self, "rpc.ping",
		"Check the compositor is answering", NULL, "\"pong\"",
		m_rpc_ping, NULL, NULL);
	gowl_ipc_register_method(self, "events.subscribe",
		"Receive events on this connection as `event' notifications",
		subscribe_params, "{subscribed, events[]}",
		m_events_subscribe, NULL, NULL);
	gowl_ipc_register_method(self, "events.unsubscribe",
		"Stop receiving events on this connection", NULL,
		"{subscribed, events[]}",
		m_events_unsubscribe, NULL, NULL);
	gowl_ipc_register_method(self, "events.list",
		"The events the compositor documents", NULL,
		"[{name, doc}]",
		m_events_list, NULL, NULL);
}
