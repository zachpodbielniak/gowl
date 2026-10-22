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

#ifndef PORTAL_GOWL_H
#define PORTAL_GOWL_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * PortalGowl -- a client of the compositor's own IPC socket.
 *
 * The Wayland protocols say what can be captured; they do not say
 * which output has focus, which window is active, where the pointer
 * is, or how to put a list in front of a human.  gowl answers all four
 * over its wire protocol (JSON-RPC 2.0 on $GOWL_SOCKET, docs/ipc.org),
 * so when the portal runs inside a gowl session it asks.  Outside one
 * -- portal-wlr's territory, or a compositor without the socket -- every
 * caller has a fallback, and nothing here is required.
 *
 * Deliberately synchronous: each call is one short request on a
 * dedicated connection, answered by the compositor in microseconds.
 * The one long wait, a human choosing from the menu, is bounded by a
 * timeout and reads the answer as an event.
 */
typedef struct _PortalGowl PortalGowl;

/**
 * portal_gowl_socket_path:
 *
 * Where the compositor's socket is: $GOWL_SOCKET, else
 * $XDG_RUNTIME_DIR/gowl.sock.
 *
 * Returns: (transfer full): the path
 */
gchar *portal_gowl_socket_path (void);

/**
 * portal_gowl_connect:
 * @error: return location for an error
 *
 * Connects to the running compositor.  Fails quickly when there is no
 * gowl: no socket, or nothing listening on it.
 *
 * Returns: (transfer full) (nullable): the connection
 */
PortalGowl *portal_gowl_connect (GError **error);

/**
 * portal_gowl_call:
 * @self: a #PortalGowl
 * @method: a wire-protocol method, e.g. `monitor.get'
 * @params: (transfer full) (nullable): its parameters
 * @error: return location for an error; the compositor's own error
 *   message when it answered with one
 *
 * One request and its response.  Events that arrive first (the
 * protocol sends a request's own events before its reply) are kept for
 * portal_gowl_wait_event().
 *
 * Returns: (transfer full) (nullable): the result
 */
JsonNode *portal_gowl_call (PortalGowl  *self,
                            const gchar *method,
                            JsonObject  *params,
                            GError     **error);

/**
 * portal_gowl_command:
 * @self: a #PortalGowl
 * @line: a text command, module commands included
 * @error: return location for an error
 *
 * Runs a text command through `gowl.command'.  A reply starting
 * `ERROR' is returned as an error.
 *
 * Returns: (transfer full) (nullable): the reply line
 */
gchar *portal_gowl_command (PortalGowl  *self,
                            const gchar *line,
                            GError     **error);

/**
 * portal_gowl_take_event_text:
 * @self: a #PortalGowl
 * @prefix: the start of an event's text line, e.g. `EVENT menu '
 *
 * Takes the first event already read whose text form starts with
 * @prefix.  No I/O: events arrive through portal_gowl_call() (those
 * that came before its reply) and portal_gowl_read_async().
 *
 * Returns: (transfer full) (nullable): the event's text line
 */
gchar *portal_gowl_take_event_text (PortalGowl  *self,
                                    const gchar *prefix);

/**
 * portal_gowl_read_async:
 * @self: a #PortalGowl
 * @cancellable: (nullable): cancels the read
 * @callback: called when one line has been read
 * @user_data: for @callback
 *
 * Reads one message without blocking, queueing it if it is an event.
 * The wait for a human goes through here, so the portal's main loop
 * -- running casts, other sessions -- keeps turning meanwhile.  No
 * timeout: the caller bounds the wait with @cancellable.
 */
void portal_gowl_read_async (PortalGowl          *self,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data);

/**
 * portal_gowl_read_finish:
 * @self: a #PortalGowl
 * @result: the #GAsyncResult
 * @error: return location for an error
 *
 * Returns: %TRUE if a message was read; %FALSE at the end of the
 *          connection, on an error, or when cancelled
 */
gboolean portal_gowl_read_finish (PortalGowl    *self,
                                  GAsyncResult  *result,
                                  GError       **error);

void portal_gowl_free (PortalGowl *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalGowl, portal_gowl_free)

G_END_DECLS

#endif /* PORTAL_GOWL_H */
