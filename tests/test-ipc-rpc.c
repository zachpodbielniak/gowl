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
 * The wire protocol, end to end: a headless compositor with two real
 * xdg toplevels mapped by a client thread, its socket up, and this
 * process talking JSON-RPC to it the way any program would.
 *
 * What is pinned here is what a program relies on: errors are typed and
 * say why, parameters are checked by name and type, batches answer in
 * order and leave notifications out, "mine" really is the caller's
 * windows, the geometry arithmetic lands windows where the request
 * said, layout.apply is all or nothing, cmacs-embedded windows are
 * refused rather than silently moved, and the structured events arrive
 * -- including the ones the text dialect never had.
 */

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "macro-test-client.h"
#include "ipc/gowl-ipc.h"

typedef struct {
	Rig                rig;
	GowlIpc           *ipc;
	gchar             *sock;
	GSocketConnection *conn;
	GDataInputStream  *in;
	GOutputStream     *out;
	gint64             next_id;
	/* Event lines read while looking for something else.  A request's
	 * own events arrive BEFORE its reply -- they are pushed while it
	 * runs -- so a reader waiting for a reply sees them first. */
	GQueue             early;
} Fx;

static gboolean
fx_up(Fx *f)
{
	static const gchar *const titles[] = { "alpha", "beta", NULL };
	static const gchar *const apps[] = { "org.test.alpha", "org.test.beta",
	                                     NULL };
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketAddress) addr = NULL;
	GError *error = NULL;

	memset(f, 0, sizeof *f);
	if (!rig_up(&f->rig, titles, apps, NULL))
		return FALSE;

	f->sock = g_build_filename(f->rig.runtime, "gowl.sock", NULL);
	f->ipc = gowl_ipc_new(f->sock);
	g_assert_true(gowl_ipc_start(f->ipc, f->rig.compositor->event_loop,
	                             &error));
	g_assert_no_error(error);
	gowl_compositor_set_ipc(f->rig.compositor, f->ipc);

	client = g_socket_client_new();
	addr = g_unix_socket_address_new(f->sock);
	f->conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr),
	                                  NULL, &error);
	g_assert_no_error(error);
	/* A read that would block means a reply that never came: fail the
	 * test, do not hang it. */
	g_socket_set_timeout(g_socket_connection_get_socket(f->conn), 3);
	f->out = g_io_stream_get_output_stream(G_IO_STREAM(f->conn));
	f->in = g_data_input_stream_new(
		g_io_stream_get_input_stream(G_IO_STREAM(f->conn)));
	pump(&f->rig, 30);
	return TRUE;
}

static void
fx_down(Fx *f)
{
	g_queue_clear_full(&f->early, (GDestroyNotify)json_node_unref);
	g_clear_object(&f->in);
	g_clear_object(&f->conn);
	if (f->rig.compositor != NULL && f->ipc != NULL)
		gowl_compositor_set_ipc(f->rig.compositor, NULL);
	if (f->ipc != NULL)
		gowl_ipc_stop(f->ipc);
	g_clear_object(&f->ipc);
	if (f->sock != NULL)
		g_unlink(f->sock);
	g_free(f->sock);
	rig_down(&f->rig);
}

static void
send_raw(Fx *f, const gchar *line)
{
	g_autofree gchar *l = g_strdup_printf("%s\n", line);
	GError *error = NULL;

	g_assert_true(g_output_stream_write_all(f->out, l, strlen(l), NULL,
	                                        NULL, &error));
	g_assert_no_error(error);
	pump(&f->rig, 30);
}

static JsonNode *
read_json(Fx *f)
{
	GError *error = NULL;
	g_autofree gchar *line = g_data_input_stream_read_line_utf8(
		f->in, NULL, NULL, &error);
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_no_error(error);
	g_assert_nonnull(line);
	g_assert_true(json_parser_load_from_data(parser, line, -1, NULL));
	return json_node_copy(json_parser_get_root(parser));
}

/* Reads lines until the response (an object with an id). */
static JsonObject *
read_response(Fx *f)
{
	for (;;) {
		JsonNode *n = read_json(f);
		JsonObject *o;

		if (JSON_NODE_HOLDS_OBJECT(n)
		    && json_object_has_member(json_node_get_object(n), "id")) {
			o = json_object_ref(json_node_get_object(n));
			json_node_unref(n);
			return o;
		}
		g_queue_push_tail(&f->early, n);
	}
}

/* One call; @params is JSON text or NULL.  Returns the response. */
static JsonObject *
call(Fx *f, const gchar *method, const gchar *params)
{
	g_autofree gchar *req = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT
		",\"method\":\"%s\"%s%s}", ++f->next_id, method,
		params != NULL ? ",\"params\":" : "",
		params != NULL ? params : "");
	JsonObject *resp;

	send_raw(f, req);
	resp = read_response(f);
	g_assert_cmpint(json_object_get_int_member(resp, "id"), ==, f->next_id);
	return resp;
}

static JsonNode *
result_of(JsonObject *resp)
{
	if (json_object_has_member(resp, "error"))
		g_error("unexpected error: %s",
		        json_object_get_string_member(
		            json_object_get_object_member(resp, "error"),
		            "message"));
	return json_object_get_member(resp, "result");
}

static gint64
error_code(JsonObject *resp)
{
	g_assert_true(json_object_has_member(resp, "error"));
	return json_object_get_int_member(
		json_object_get_object_member(resp, "error"), "code");
}

/* ── Framing and errors ─────────────────────────────────────────── */

static void
test_discover_and_errors(void)
{
	Fx f;
	g_autoptr(JsonObject) r = NULL;
	JsonObject *res;
	JsonArray *methods;
	guint i;
	gboolean saw_apply = FALSE;

	if (!fx_up(&f))
		return;

	r = call(&f, "rpc.discover", NULL);
	res = json_node_get_object(result_of(r));
	g_assert_cmpstr(json_object_get_string_member(res, "protocol"), ==,
	                GOWL_IPC_PROTOCOL_NAME);
	g_assert_cmpint(json_object_get_int_member(res, "version"), ==,
	                GOWL_IPC_PROTOCOL_VERSION);
	/* The caller is this process: SO_PEERCRED works. */
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(res, "caller"), "pid"), ==, getpid());
	methods = json_object_get_array_member(res, "methods");
	for (i = 0; i < json_array_get_length(methods); i++)
		if (g_strcmp0(json_object_get_string_member(
		        json_array_get_object_element(methods, i), "name"),
		        "layout.apply") == 0)
			saw_apply = TRUE;
	g_assert_true(saw_apply);
	g_clear_pointer(&r, json_object_unref);

	/* Unknown method, unknown parameter, wrong type. */
	r = call(&f, "window.frobnicate", NULL);
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_METHOD_NOT_FOUND);
	g_clear_pointer(&r, json_object_unref);
	r = call(&f, "window.set_geometry", "{\"widht\":10}");
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_INVALID_PARAMS);
	g_assert_nonnull(strstr(json_object_get_string_member(
		json_object_get_object_member(r, "error"), "message"), "widht"));
	g_clear_pointer(&r, json_object_unref);
	r = call(&f, "window.set_geometry", "{\"x\":\"left\"}");
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_INVALID_PARAMS);
	g_clear_pointer(&r, json_object_unref);
	r = call(&f, "window.get", "{\"id\":999999}");
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_NOT_FOUND);
	g_clear_pointer(&r, json_object_unref);
	/* Params must be named. */
	r = call(&f, "window.get", "[1]");
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_INVALID_PARAMS);
	g_clear_pointer(&r, json_object_unref);

	/* Not JSON at all: a parse error with a null id. */
	{
		JsonNode *n;

		send_raw(&f, "{not json");
		n = read_json(&f);
		g_assert_cmpint(json_object_get_int_member(
			json_object_get_object_member(json_node_get_object(n),
			                              "error"), "code"), ==,
			GOWL_IPC_ERROR_PARSE);
		g_assert_true(json_object_get_null_member(json_node_get_object(n),
		                                          "id"));
		json_node_unref(n);
	}

	/* The text dialect on the same connection still works. */
	{
		GError *error = NULL;
		g_autofree gchar *line = NULL;

		send_raw(&f, "ping");
		line = g_data_input_stream_read_line_utf8(f.in, NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(line, ==, "pong");
	}

	/* gowl.command: a text command from JSON, a query's reply parsed. */
	r = call(&f, "gowl.command", "{\"line\":\"tags\"}");
	res = json_node_get_object(result_of(r));
	g_assert_true(json_object_get_boolean_member(res, "ok"));
	g_assert_true(json_object_has_member(res, "json"));
	fx_down(&f);
}

static void
test_batch_and_notifications(void)
{
	Fx f;
	JsonNode *n;
	JsonArray *a;
	g_autoptr(JsonObject) r = NULL;

	if (!fx_up(&f))
		return;

	/* A notification is answered with nothing: the next line read is
	 * the response to the request after it. */
	send_raw(&f, "{\"jsonrpc\":\"2.0\",\"method\":\"rpc.ping\"}");
	r = call(&f, "rpc.ping", NULL);
	g_assert_cmpstr(json_node_get_string(result_of(r)), ==, "pong");

	/* A batch: responses in order, the notification left out, an error
	 * in the middle does not stop the rest. */
	send_raw(&f, "[{\"id\":\"a\",\"method\":\"rpc.ping\"},"
	              "{\"method\":\"rpc.ping\"},"
	              "{\"id\":\"b\",\"method\":\"nope\"},"
	              "{\"id\":\"c\",\"method\":\"tags.get\"}]");
	n = read_json(&f);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(n));
	a = json_node_get_array(n);
	g_assert_cmpuint(json_array_get_length(a), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(a, 0), "id"), ==, "a");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(a, 1), "id"), ==, "b");
	g_assert_true(json_object_has_member(
		json_array_get_object_element(a, 1), "error"));
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(a, 2), "id"), ==, "c");
	json_node_unref(n);

	send_raw(&f, "[]");
	n = read_json(&f);
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(json_node_get_object(n), "error"),
		"code"), ==, GOWL_IPC_ERROR_INVALID_REQUEST);
	json_node_unref(n);
	fx_down(&f);
}

/* A client that sends and half-closes still gets every reply. */
static void
test_half_close_gets_replies(void)
{
	Fx f;
	GError *error = NULL;
	gchar *line;
	gint n = 0;

	if (!fx_up(&f))
		return;
	send_raw(&f, "{\"id\":1,\"method\":\"rpc.ping\"}\n"
	             "{\"id\":2,\"method\":\"gowl.version\"}\nping");
	g_socket_shutdown(g_socket_connection_get_socket(f.conn), FALSE, TRUE,
	                  &error);
	g_assert_no_error(error);
	pump(&f.rig, 60);
	while ((line = g_data_input_stream_read_line_utf8(f.in, NULL, NULL,
	                                                  &error)) != NULL) {
		n++;
		g_free(line);
	}
	g_assert_no_error(error);
	g_assert_cmpint(n, ==, 3);
	fx_down(&f);
}

/* ── Windows ────────────────────────────────────────────────────── */

static gint64
window_id(Fx *f, gint i)
{
	return gowl_client_get_id(f->rig.win[i]);
}

static void
test_windows_and_geometry(void)
{
	Fx f;
	g_autoptr(JsonObject) r = NULL;
	g_autofree gchar *p = NULL;
	JsonArray *a;
	JsonObject *w;
	GowlMonitor *m;
	GowlClient *c;

	if (!fx_up(&f))
		return;
	m = f.rig.compositor->selmon;

	/* Both windows; both "mine", since the client thread is in this
	 * process. */
	r = call(&f, "windows.list", "{\"mine\":true}");
	a = json_node_get_array(result_of(r));
	g_assert_cmpuint(json_array_get_length(a), ==, 2);
	g_assert_true(json_object_get_boolean_member(
		json_array_get_object_element(a, 0), "mine"));
	g_clear_pointer(&r, json_object_unref);

	r = call(&f, "windows.list", "{\"app_id\":\"org.test.beta\"}");
	a = json_node_get_array(result_of(r));
	g_assert_cmpuint(json_array_get_length(a), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(a, 0), "title"), ==, "beta");
	g_clear_pointer(&r, json_object_unref);

	/* The right half of the window area, as fractions. */
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"x\":0.5,\"y\":0,"
	                    "\"width\":0.5,\"height\":1,\"relative_to\":\"area\","
	                    "\"units\":\"fraction\"}", window_id(&f, 0));
	r = call(&f, "window.set_geometry", p);
	w = json_node_get_object(result_of(r));
	c = f.rig.win[0];
	g_assert_true(json_object_get_boolean_member(w, "floating"));
	g_assert_cmpint(c->geom.x, ==, m->w.x + m->w.width / 2);
	g_assert_cmpint(c->geom.y, ==, m->w.y);
	g_assert_cmpint(c->geom.width, ==, m->w.width / 2);
	g_assert_cmpint(c->geom.height, ==, m->w.height);
	g_clear_pointer(&r, json_object_unref);
	g_clear_pointer(&p, g_free);

	/* A nudge keeps the size and moves the corner. */
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"dx\":-10,"
	                    "\"dy\":5}", window_id(&f, 0));
	r = call(&f, "window.move_by", p);
	result_of(r);
	g_assert_cmpint(c->geom.x, ==, m->w.x + m->w.width / 2 - 10);
	g_assert_cmpint(c->geom.y, ==, m->w.y + 5);
	g_clear_pointer(&r, json_object_unref);
	g_clear_pointer(&p, g_free);

	/* Tag 3 is the bitmask 4. */
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"tag\":3}",
	                    window_id(&f, 1));
	r = call(&f, "window.set_tags", p);
	result_of(r);
	g_assert_cmpuint(f.rig.win[1]->tags, ==, 4);
	g_clear_pointer(&r, json_object_unref);
	g_clear_pointer(&p, g_free);

	r = call(&f, "tags.view", "{\"tag\":3}");
	g_assert_cmpint(json_object_get_int_member(
		json_node_get_object(result_of(r)), "active"), ==, 4);
	g_assert_cmpuint(m->tagset[m->seltags], ==, 4);
	g_clear_pointer(&r, json_object_unref);

	/* Opacity is range-checked. */
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"opacity\":1.5}",
	                    window_id(&f, 1));
	r = call(&f, "window.set_opacity", p);
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_INVALID_PARAMS);
	fx_down(&f);
}

static void
test_layout_apply_all_or_nothing(void)
{
	Fx f;
	g_autoptr(JsonObject) r = NULL;
	g_autofree gchar *p = NULL;
	GowlMonitor *m;
	struct wlr_box before;

	if (!fx_up(&f))
		return;
	m = f.rig.compositor->selmon;

	/* Two windows, side by side, in one request. */
	p = g_strdup_printf(
		"{\"units\":\"fraction\",\"windows\":["
		"{\"id\":%" G_GINT64_FORMAT ",\"x\":0,\"y\":0,\"width\":0.5,\"height\":1},"
		"{\"id\":%" G_GINT64_FORMAT ",\"x\":0.5,\"y\":0,\"width\":0.5,\"height\":1}"
		"]}", window_id(&f, 0), window_id(&f, 1));
	r = call(&f, "layout.apply", p);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(result_of(r))),
	                 ==, 2);
	g_assert_cmpint(f.rig.win[0]->geom.x, ==, m->w.x);
	g_assert_cmpint(f.rig.win[1]->geom.x, ==, m->w.x + m->w.width / 2);
	g_assert_cmpint(f.rig.win[1]->geom.width, ==, m->w.width / 2);
	g_clear_pointer(&r, json_object_unref);
	g_clear_pointer(&p, g_free);

	/* A typo in the second entry: an error, and the FIRST did not move
	 * either. */
	before = f.rig.win[0]->geom;
	p = g_strdup_printf(
		"{\"windows\":["
		"{\"id\":%" G_GINT64_FORMAT ",\"x\":7,\"y\":7,\"width\":100,\"height\":100},"
		"{\"id\":%" G_GINT64_FORMAT ",\"widht\":5}"
		"]}", window_id(&f, 0), window_id(&f, 1));
	r = call(&f, "layout.apply", p);
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_INVALID_PARAMS);
	g_assert_cmpint(f.rig.win[0]->geom.x, ==, before.x);
	g_assert_cmpint(f.rig.win[0]->geom.width, ==, before.width);
	g_clear_pointer(&r, json_object_unref);
	g_clear_pointer(&p, g_free);

	/* layout.order with tile: both back in the layout. */
	p = g_strdup_printf("{\"ids\":[%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
	                    "],\"tile\":true}", window_id(&f, 1), window_id(&f, 0));
	r = call(&f, "layout.order", p);
	result_of(r);
	g_assert_false(f.rig.win[0]->isfloating);
	g_assert_false(f.rig.win[1]->isfloating);
	fx_down(&f);
}

/* cmacs owns an embedded window's place; the protocol says so. */
static void
test_embedded_is_refused(void)
{
	Fx f;
	g_autoptr(JsonObject) r = NULL;
	g_autofree gchar *p = NULL;

	if (!fx_up(&f))
		return;
	f.rig.win[0]->isembedded = TRUE;
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"x\":0}",
	                    window_id(&f, 0));
	r = call(&f, "window.set_geometry", p);
	g_assert_cmpint(error_code(r), ==, GOWL_IPC_ERROR_REFUSED);
	g_clear_pointer(&r, json_object_unref);
	/* Queries still see it, marked. */
	g_clear_pointer(&p, g_free);
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT "}", window_id(&f, 0));
	r = call(&f, "window.get", p);
	g_assert_true(json_object_get_boolean_member(
		json_node_get_object(result_of(r)), "embedded"));
	f.rig.win[0]->isembedded = FALSE;
	fx_down(&f);
}

/* ── Events ─────────────────────────────────────────────────────── */

/* A text command on the same connection; its one-line reply. */
static void
text_ok(Fx *f, const gchar *cmd)
{
	GError *error = NULL;
	g_autofree gchar *line = NULL;

	send_raw(f, cmd);
	for (;;) {
		g_autoptr(JsonParser) parser = json_parser_new();

		g_clear_pointer(&line, g_free);
		line = g_data_input_stream_read_line_utf8(f->in, NULL, NULL,
		                                          &error);
		g_assert_no_error(error);
		g_assert_nonnull(line);
		if (line[0] != '{'
		    || !json_parser_load_from_data(parser, line, -1, NULL))
			break;
		g_queue_push_tail(&f->early,
		                  json_node_copy(json_parser_get_root(parser)));
	}
	g_assert_cmpstr(line, ==, "OK");
}

/* Reads until an event named @name arrives; returns its params. */
static JsonObject *
await_event(Fx *f, const gchar *name)
{
	gint tries;

	for (tries = 0; tries < 50; tries++) {
		JsonNode *n;
		JsonObject *o;

		if (g_queue_is_empty(&f->early)) {
			pump(&f->rig, 20);
			n = read_json(f);
		} else {
			n = g_queue_pop_head(&f->early);
		}
		o = json_node_get_object(n);
		if (g_strcmp0(json_object_get_string_member_with_default(
		                  o, "method", ""), "event") == 0
		    && g_strcmp0(json_object_get_string_member(
		                     json_object_get_object_member(o, "params"),
		                     "event"), name) == 0) {
			JsonObject *params = json_object_ref(
				json_object_get_object_member(o, "params"));

			json_node_unref(n);
			return params;
		}
		json_node_unref(n);
	}
	g_error("no %s event arrived", name);
	return NULL;
}

static void
test_structured_events(void)
{
	Fx f;
	g_autoptr(JsonObject) r = NULL;
	g_autoptr(JsonObject) ev = NULL;
	g_autofree gchar *p = NULL;
	JsonObject *data;
	JsonArray *changed;
	guint i;
	gboolean geometry = FALSE;

	if (!fx_up(&f))
		return;

	r = call(&f, "events.subscribe",
	         "{\"events\":[\"tags.changed\",\"window.changed\","
	         "\"keymode.*\"]}");
	g_assert_true(json_object_get_boolean_member(
		json_node_get_object(result_of(r)), "subscribed"));
	g_clear_pointer(&r, json_object_unref);
	/* First sight of every window is recorded, not announced. */
	pump(&f.rig, 60);

	/* A tag view from the TEXT dialect: the text dialect never pushed
	 * a `tags' event for it, the structured one comes anyway. */
	text_ok(&f, "view 2");
	ev = await_event(&f, "tags.changed");
	data = json_object_get_object_member(ev, "data");
	g_assert_cmpint(json_object_get_int_member(data, "active"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(data, "previous"), ==, 1);
	g_clear_pointer(&ev, json_object_unref);
	text_ok(&f, "view 1");
	ev = await_event(&f, "tags.changed");
	g_clear_pointer(&ev, json_object_unref);

	/* A geometry change is window.changed with "geometry" in it. */
	p = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"x\":20,\"y\":30,"
	                    "\"width\":300,\"height\":200}", window_id(&f, 0));
	r = call(&f, "window.set_geometry", p);
	result_of(r);
	/* Floating window 0 re-tiles window 1, whose change may come
	 * first: wait for the one about the window that was moved. */
	/* Its floating change (from the arrange inside set_floating) can
	 * be announced before its geometry; wait for the geometry. */
	while (!geometry) {
		ev = await_event(&f, "window.changed");
		data = json_object_get_object_member(ev, "data");
		if (json_object_get_int_member(data, "id") == window_id(&f, 0)) {
			changed = json_object_get_array_member(data, "changed");
			for (i = 0; i < json_array_get_length(changed); i++)
				if (g_strcmp0(json_array_get_string_element(changed, i),
				              "geometry") == 0)
					geometry = TRUE;
		}
		if (geometry)
			g_assert_cmpint(json_object_get_int_member(
				json_object_get_object_member(data, "window"), "x"),
				==, 20);
		g_clear_pointer(&ev, json_object_unref);
	}

	/* A text event, structured, through a glob. */
	gowl_compositor_set_key_mode(f.rig.compositor, "resize");
	ev = await_event(&f, "keymode.changed");
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(ev, "data"), "mode"), ==, "resize");
	g_assert_cmpstr(json_object_get_string_member(ev, "text"), ==,
	                "EVENT mode resize");
	fx_down(&f);
}

/*
 * A subscriber that stops reading cannot stall the compositor: its
 * output queues, and past GOWL_IPC_MAX_PENDING it is dropped.  Before
 * the queue, one write spun on EAGAIN until the reader came back --
 * the whole desktop frozen behind a paused `gowl-msg -s | less'.
 */
static void
test_stalled_reader_is_dropped(void)
{
	Fx f;
	g_autoptr(JsonObject) r = NULL;
	g_autofree gchar *pad = g_strnfill(900, 'x');
	gint64 t0;
	gint i;

	if (!fx_up(&f))
		return;
	r = call(&f, "events.subscribe", NULL);
	result_of(r);
	g_assert_cmpuint(gowl_ipc_get_client_count(f.ipc), ==, 1);

	/* ~1 KiB per event, as both a text and a JSON subscriber would get
	 * it; well past the 16 MiB allowance, never read. */
	t0 = g_get_monotonic_time();
	for (i = 0; i < 40000; i++)
		gowl_ipc_push_event(f.ipc, "EVENT flood %d %s", i, pad);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 10 * G_USEC_PER_SEC);
	g_assert_cmpuint(gowl_ipc_get_client_count(f.ipc), ==, 0);
	fx_down(&f);
}

/* In process, through the same parameter check. */
static void
test_in_process_call(void)
{
	Fx f;
	g_autoptr(JsonObject) params = json_object_new();
	g_autoptr(GError) error = NULL;
	JsonNode *n;

	if (!fx_up(&f))
		return;
	n = gowl_ipc_call(f.ipc, "layout.list", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(n));
	json_node_unref(n);

	json_object_set_int_member(params, "bogus", 1);
	n = gowl_ipc_call(f.ipc, "tags.get", params, &error);
	g_assert_null(n);
	g_assert_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS);
	fx_down(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ipc-rpc/discover-and-errors", test_discover_and_errors);
	g_test_add_func("/ipc-rpc/batch-and-notifications",
	                test_batch_and_notifications);
	g_test_add_func("/ipc-rpc/half-close-gets-replies",
	                test_half_close_gets_replies);
	g_test_add_func("/ipc-rpc/windows-and-geometry",
	                test_windows_and_geometry);
	g_test_add_func("/ipc-rpc/layout-apply-all-or-nothing",
	                test_layout_apply_all_or_nothing);
	g_test_add_func("/ipc-rpc/embedded-is-refused", test_embedded_is_refused);
	g_test_add_func("/ipc-rpc/structured-events", test_structured_events);
	g_test_add_func("/ipc-rpc/stalled-reader-is-dropped",
	                test_stalled_reader_is_dropped);
	g_test_add_func("/ipc-rpc/in-process-call", test_in_process_call);

	return g_test_run();
}
