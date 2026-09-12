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
 * The IPC socket, end to end: a headless compositor with its socket
 * up, and a plain socket client sending lines and reading lines.
 *
 * Before this the socket read every command and answered none -- the
 * `command-received' signal it emitted had no listener -- so every
 * command a script sent was silently dropped.  Now each line gets one
 * line back: JSON for a query, OK/ERROR otherwise, and a subscriber
 * sees the events the compositor pushes.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include "gowl.h"
#include "core/gowl-core-private.h"
#include "config/gowl-config.h"
#include "ipc/gowl-ipc.h"

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlCompositor    *compositor;
	GowlIpc           *ipc;
	GSocketConnection *conn;
	GDataInputStream  *in;
	GOutputStream     *out;
} Fixture;

static void
pump(Fixture *f, gint msec)
{
	gint64 until = g_get_monotonic_time() + (gint64)msec * 1000;

	while (g_get_monotonic_time() < until) {
		wl_display_flush_clients(f->compositor->wl_display);
		wl_event_loop_dispatch(f->compositor->event_loop, 5);
	}
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	const gchar *parent;
	gchar *state, *sock;
	GError *error = NULL;
	GSocketClient *client;
	GSocketAddress *addr;
	(void)data;

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	f->runtime = g_build_filename(parent, "gowl-ipc-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(f->runtime, 0700));
	state = g_build_filename(f->runtime, "state", NULL);
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", f->runtime, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_free(state);

	f->config = gowl_config_new();
	f->compositor = gowl_compositor_new();
	gowl_compositor_set_config(f->compositor, f->config);
	if (!gowl_compositor_start(f->compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);

	/* The socket, under the private runtime dir, and the compositor's
	 * commands behind it. */
	sock = g_build_filename(f->runtime, "gowl.sock", NULL);
	f->ipc = gowl_ipc_new(sock);
	g_assert_true(gowl_ipc_start(f->ipc, f->compositor->event_loop, &error));
	g_assert_no_error(error);
	gowl_compositor_set_ipc(f->compositor, f->ipc);

	client = g_socket_client_new();
	addr = g_unix_socket_address_new(sock);
	f->conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr),
	                                  NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(f->conn);
	f->out = g_io_stream_get_output_stream(G_IO_STREAM(f->conn));
	f->in = g_data_input_stream_new(
		g_io_stream_get_input_stream(G_IO_STREAM(f->conn)));
	g_object_unref(client);
	g_object_unref(addr);
	g_free(sock);
	/* The accept happens in the compositor's loop. */
	pump(f, 50);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	static const gchar *const store[] = { "state/gowl", "state" };
	gsize i;
	(void)data;

	g_object_unref(f->in);
	g_object_unref(f->conn);
	gowl_compositor_set_ipc(f->compositor, NULL);
	gowl_ipc_stop(f->ipc);
	g_object_unref(f->ipc);
	g_object_unref(f->compositor);
	g_object_unref(f->config);
	for (i = 0; i < G_N_ELEMENTS(store); i++) {
		gchar *path = g_build_filename(f->runtime, store[i], NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			g_rmdir(path);
		g_free(path);
	}
	g_rmdir(f->runtime);
	g_free(f->runtime);
}

/* Send a line; run the compositor's loop; read the reply line. */
static gchar *
ask(Fixture *f, const gchar *cmd)
{
	g_autofree gchar *line = g_strdup_printf("%s\n", cmd);
	GError *error = NULL;
	gchar *reply;

	g_assert_true(g_output_stream_write_all(f->out, line, strlen(line),
	                                        NULL, NULL, &error));
	g_assert_no_error(error);
	pump(f, 50);
	reply = g_data_input_stream_read_line_utf8(f->in, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reply);
	return reply;
}

static JsonNode *
parse(const gchar *text)
{
	JsonParser *p = json_parser_new();
	JsonNode *root;

	g_assert_true(json_parser_load_from_data(p, text, -1, NULL));
	root = json_node_copy(json_parser_get_root(p));
	g_object_unref(p);
	return root;
}

static void
test_queries_answer_json(Fixture *f, gconstpointer data)
{
	g_autofree gchar *r = NULL;
	JsonNode *n;
	JsonObject *o;
	(void)data;

	r = ask(f, "ping");
	g_assert_cmpstr(r, ==, "pong");
	g_free(r);

	/* No windows: an empty array, still JSON. */
	r = ask(f, "clients");
	n = parse(r);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(n));
	g_assert_cmpuint(json_array_get_length(json_node_get_array(n)), ==, 0);
	json_node_unref(n);
	g_free(r);

	r = ask(f, "focused");
	g_assert_cmpstr(r, ==, "null");
	g_free(r);

	/* The headless output, with its tags and layout. */
	r = ask(f, "monitors");
	n = parse(r);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(n));
	g_assert_cmpuint(json_array_get_length(json_node_get_array(n)), ==, 1);
	o = json_array_get_object_element(json_node_get_array(n), 0);
	g_assert_true(json_object_get_boolean_member(o, "focused"));
	g_assert_cmpint(json_object_get_int_member(o, "tags"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(o, "width"), >, 0);
	json_node_unref(n);
	g_free(r);

	/* No profiles configured: nothing active, an empty list. */
	r = ask(f, "profile");
	n = parse(r);
	o = json_node_get_object(n);
	g_assert_true(json_object_get_null_member(o, "active"));
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(o, "profiles")), ==, 0);
	json_node_unref(n);
	g_free(r);

	r = ask(f, "tags");
	n = parse(r);
	o = json_node_get_object(n);
	g_assert_cmpint(json_object_get_int_member(o, "active"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(o, "occupied"), ==, 0);
	json_node_unref(n);
	g_free(r);

	r = ask(f, "keyboard-layout");
	n = parse(r);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(n),
	                                           "index"), ==, 0);
	json_node_unref(n);
	r = NULL;
}

static void
test_commands_answer_ok_or_error(Fixture *f, gconstpointer data)
{
	g_autofree gchar *r = NULL;
	(void)data;

	r = ask(f, "mode");
	g_assert_cmpstr(r, ==, "default");
	g_free(r);
	r = ask(f, "mode resize");
	g_assert_cmpstr(r, ==, "resize");
	g_assert_cmpstr(gowl_compositor_get_key_mode(f->compositor), ==, "resize");
	g_free(r);
	r = ask(f, "mode default");
	g_assert_cmpstr(r, ==, "default");
	g_free(r);

	r = ask(f, "view 4");
	g_assert_cmpstr(r, ==, "OK");
	g_assert_cmpuint(f->compositor->selmon->tagset[f->compositor->selmon->seltags],
	                 ==, 4);
	g_free(r);
	r = ask(f, "view 0");
	g_assert_true(g_str_has_prefix(r, "ERROR"));
	g_free(r);

	/* An action by name, with its argument. */
	r = ask(f, "action tag-view 2");
	g_assert_cmpstr(r, ==, "OK");
	g_assert_cmpuint(f->compositor->selmon->tagset[f->compositor->selmon->seltags],
	                 ==, 2);
	g_free(r);
	r = ask(f, "action no-such-action");
	g_assert_true(g_str_has_prefix(r, "ERROR unknown action"));
	g_free(r);

	/* A key with nothing bound, then bound. */
	r = ask(f, "dispatch Super+F12");
	g_assert_true(g_str_has_prefix(r, "ERROR"));
	g_free(r);
	gowl_config_add_keybind_full(f->config, GOWL_KEY_MOD_LOGO, XKB_KEY_F12,
	                             GOWL_ACTION_TAG_VIEW, "1", NULL);
	r = ask(f, "dispatch Super+F12");
	g_assert_cmpstr(r, ==, "OK");
	g_free(r);

	r = ask(f, "focus 12345");
	g_assert_true(g_str_has_prefix(r, "ERROR no such client"));
	g_free(r);

	/* Nothing knows this one: said so, not silence. */
	r = ask(f, "frobnicate");
	g_assert_true(g_str_has_prefix(r, "ERROR unknown command"));
	r = NULL;
}

static void
test_subscriber_gets_events(Fixture *f, gconstpointer data)
{
	g_autofree gchar *r = NULL;
	gchar *ev;
	GError *error = NULL;
	(void)data;

	r = ask(f, "subscribe");
	g_assert_cmpstr(r, ==, "OK subscribed");
	g_free(r);

	/* A mode change from the compositor's side arrives as an event. */
	gowl_compositor_set_key_mode(f->compositor, "resize");
	pump(f, 50);
	ev = g_data_input_stream_read_line_utf8(f->in, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(ev, ==, "EVENT mode resize");
	g_free(ev);

	/* Powering the output off is an event too. */
	gowl_compositor_set_outputs_powered(f->compositor, FALSE);
	pump(f, 50);
	ev = g_data_input_stream_read_line_utf8(f->in, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_prefix(ev, "EVENT power "));
	g_assert_true(g_str_has_suffix(ev, " off"));
	g_free(ev);
	r = NULL;
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/ipc/queries-answer-json", Fixture, NULL,
	           fixture_setup, test_queries_answer_json, fixture_teardown);
	g_test_add("/ipc/commands-answer-ok-or-error", Fixture, NULL,
	           fixture_setup, test_commands_answer_ok_or_error,
	           fixture_teardown);
	g_test_add("/ipc/subscriber-gets-events", Fixture, NULL,
	           fixture_setup, test_subscriber_gets_events, fixture_teardown);

	return g_test_run();
}
