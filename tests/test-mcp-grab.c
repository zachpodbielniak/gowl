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
 * test-mcp-grab.c - The recorder, voice and screen tools, over MCP.
 *
 * The real mcp, macro, screenshot and clipboard modules in a headless
 * compositor whose desktop is a known picture (black, one #336699
 * rectangle), driven the way an agent drives them: JSON-RPC over
 * $XDG_RUNTIME_DIR/gowl-mcp.sock.  The tool calls are answered on the
 * module's own thread and hop to the compositor thread, so this test
 * pumps the compositor's event loop while it waits for each reply.
 *
 * What it holds: the tools are listed; pick_color reads the exact
 * pixel; screen_text runs the screenshot module's ocr-command on a
 * private PNG it deletes, and leaves the clipboard alone unless asked;
 * macro_record refuses to start without the `input-recording' consent
 * and works with it; macro_voice_match runs (or, dry, names) the macro
 * a sentence names; macro_voice_status reports; nothing turns the
 * microphone on.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-input-recorder.h"
#include "core/gowl-seat.h"

#define MARK_X (200)
#define MARK_Y (150)
#define MARK_W (100)
#define MARK_H (80)

typedef struct {
	gchar             *runtime;
	gchar             *macros;
	gchar             *script;
	gchar             *argfile;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GSocketConnection *conn;
	GString           *inbuf;
	gint               next_id;
	gboolean           up;
} Rig;

static GPtrArray *actions;
/* $XDG_RUNTIME_DIR for the whole run: GLib reads it once and caches
   it, and the MCP socket goes where GLib says, so it cannot change
   between tests.  Each test keeps its own files in a directory of its
   own under it. */
static gchar *run_dir;

static gboolean
custom_action(GowlCompositor *c, const gchar *arg, gpointer data)
{
	(void)c;
	(void)data;
	g_ptr_array_add(actions, g_strdup(arg));
	return TRUE;
}

static void
rm_rf(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *e;

	if (dir != NULL) {
		while ((e = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *p = g_build_filename(path, e, NULL);

			if (g_file_test(p, G_FILE_TEST_IS_DIR)
			    && !g_file_test(p, G_FILE_TEST_IS_SYMLINK))
				rm_rf(p);
			else
				g_unlink(p);
		}
	}
	g_rmdir(path);
}

static gboolean
load(Rig *r, const gchar *name)
{
	g_autofree gchar *so = g_strdup_printf("%s/%s.so", GOWL_TEST_MODULE_DIR,
	                                       name);
	g_autoptr(GError) error = NULL;

	if (!gowl_module_manager_load_module(r->modules, so, &error)) {
		g_test_message("%s: %s", so, error->message);
		return FALSE;
	}
	return TRUE;
}

static void
configure(Rig *r, const gchar *module, const gchar *key, const gchar *value)
{
	g_autoptr(GHashTable) s = g_hash_table_new(g_str_hash, g_str_equal);

	g_hash_table_insert(s, (gpointer)key, (gpointer)value);
	gowl_module_configure(gowl_module_manager_find_module(r->modules,
	                                                      module), s);
}

static void
pump(Rig *r, guint ms)
{
	struct wl_event_loop *loop = gowl_compositor_get_event_loop(r->compositor);
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < end) {
		wl_event_loop_dispatch(loop, 5);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
}

typedef struct {
	gboolean done;
	gchar   *text;
} ClipRead;

static void
on_clip_read(gchar *text, gpointer data)
{
	ClipRead *c = data;

	c->text = text;
	c->done = TRUE;
}

/*
 * The clipboard's text, read without blocking.  gowl_seat_get_clipboard()
 * blocks on the pipe, and a source that writes from the main loop (the
 * one clipboard-copy restores into) would then never write: in a real
 * session the reader is another process, here it is this thread.
 */
static gchar *
clipboard_text(Rig *r)
{
	ClipRead c = { FALSE, NULL };
	struct wl_event_loop *loop = gowl_compositor_get_event_loop(r->compositor);
	gint i;

	if (!gowl_seat_read_clipboard_async(gowl_compositor_get_seat(r->compositor),
	                                    loop, on_clip_read, &c))
		return NULL;
	for (i = 0; i < 300 && !c.done; i++)
		pump(r, 10);
	return c.text;
}

/* Sends one JSON-RPC message, newline-terminated. */
static void
send_line(Rig *r, const gchar *json)
{
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(r->conn));
	g_autofree gchar *line = g_strdup_printf("%s\n", json);

	g_assert_true(g_output_stream_write_all(out, line, strlen(line), NULL,
	                                        NULL, NULL));
}

/*
 * The reply with id @id, as a parsed object, pumping the compositor
 * while waiting -- the tool runs part of its work there.
 */
static JsonNode *
await_reply(Rig *r, gint id, guint ms)
{
	GSocket *sock = g_socket_connection_get_socket(r->conn);
	struct wl_event_loop *loop = gowl_compositor_get_event_loop(r->compositor);
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < end) {
		gchar buf[65536];
		gssize n;
		gchar *nl;

		while ((nl = strchr(r->inbuf->str, '\n')) != NULL) {
			g_autofree gchar *line = g_strndup(r->inbuf->str,
			                                   (gsize)(nl - r->inbuf->str));
			JsonNode *node;

			g_string_erase(r->inbuf, 0, (nl - r->inbuf->str) + 1);
			node = json_from_string(line, NULL);
			if (node != NULL && JSON_NODE_HOLDS_OBJECT(node)
			    && json_object_has_member(json_node_get_object(node), "id")
			    && json_object_get_int_member(json_node_get_object(node),
			                                  "id") == id)
				return node;
			if (node != NULL)
				json_node_unref(node);
		}
		n = g_socket_receive_with_blocking(sock, buf, sizeof buf, FALSE,
		                                   NULL, NULL);
		if (n > 0) {
			g_string_append_len(r->inbuf, buf, n);
			continue;
		}
		wl_event_loop_dispatch(loop, 5);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
	g_error("no reply to request %d", id);
	return NULL;
}

/* A request; returns its reply. */
static JsonNode *
rpc(Rig *r, const gchar *method, const gchar *params)
{
	gint id = ++r->next_id;
	g_autofree gchar *msg = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
		id, method, params != NULL ? params : "{}");

	send_line(r, msg);
	return await_reply(r, id, 15000);
}

/*
 * tools/call @tool with @args; @is_error receives isError and the
 * text content is returned.
 */
static gchar *
call(Rig *r, const gchar *tool, const gchar *args, gboolean *is_error)
{
	g_autofree gchar *params = g_strdup_printf(
		"{\"name\":\"%s\",\"arguments\":%s}", tool, args != NULL ? args : "{}");
	g_autoptr(JsonNode) reply = rpc(r, "tools/call", params);
	JsonObject *obj = json_node_get_object(reply);
	JsonObject *result;
	JsonArray *content;
	GString *text;
	guint i;

	if (json_object_has_member(obj, "error")) {
		*is_error = TRUE;
		return g_strdup(json_object_get_string_member_with_default(
			json_object_get_object_member(obj, "error"), "message", ""));
	}
	result = json_object_get_object_member(obj, "result");
	*is_error = json_object_get_boolean_member_with_default(result, "isError",
	                                                        FALSE);
	content = json_object_get_array_member(result, "content");
	text = g_string_new(NULL);
	for (i = 0; content != NULL && i < json_array_get_length(content); i++) {
		JsonObject *c = json_array_get_object_element(content, i);

		g_string_append(text, json_object_get_string_member_with_default(
			c, "text", ""));
	}
	return g_string_free(text, FALSE);
}

static void
rig_setup(Rig *r, gconstpointer data)
{
	g_autofree gchar *sock = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketAddress) addr = NULL;
	g_autoptr(GError) error = NULL;
	gint i;

	(void)data;
	memset(r, 0, sizeof *r);
	actions = g_ptr_array_new_with_free_func(g_free);
	r->inbuf = g_string_new(NULL);
	r->runtime = g_build_filename(run_dir, "test-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	r->macros = g_build_filename(r->runtime, "macros", NULL);
	g_mkdir(r->macros, 0700);

	/* the stand-in for tesseract, as test-screengrab's */
	r->argfile = g_build_filename(r->runtime, "ocr-args", NULL);
	r->script = g_build_filename(r->runtime, "fake-ocr", NULL);
	body = g_strdup_printf(
		"#!/bin/sh\n"
		"printf '%%s|%%s|%%s|%%s' \"$1\" \"$2\" \"$3\" \"$4\" > '%s'\n"
		"head -c 8 \"$1\" | grep -q PNG || { echo 'not a png' >&2; exit 1; }\n"
		"printf 'Text an agent read\\n\\f'\n", r->argfile);
	g_assert_true(g_file_set_contents(r->script, body, -1, NULL));
	g_chmod(r->script, 0755);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	if (!load(r, "mcp") || !load(r, "macro") || !load(r, "screenshot")
	    || !load(r, "clipboard")) {
		g_test_skip("a module did not load (gowl built without MCP=1?)");
		return;
	}
	gowl_module_manager_activate_all(r->modules);

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		return;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	gowl_compositor_set_custom_action_handler(r->compositor, custom_action,
	                                          NULL);
	configure(r, "macro", "macro-dir", r->macros);
	configure(r, "macro", "record-dir", r->macros);
	configure(r, "macro", "journal", "none");
	configure(r, "screenshot", "ocr-command", r->script);

	{
		struct wlr_scene_tree *bg;
		struct wlr_scene_rect *ground;
		struct wlr_scene_rect *mark;
		float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float blue[4] = { 0x33 / 255.0f, 0x66 / 255.0f, 0x99 / 255.0f, 1.0f };
		gint mw = 0;
		gint mh = 0;

		bg = gowl_compositor_get_scene_layer(r->compositor,
		                                     GOWL_SCENE_LAYER_BG);
		gowl_monitor_get_geometry(r->compositor->selmon, NULL, NULL,
		                          &mw, &mh);
		ground = wlr_scene_rect_create(bg, mw, mh, black);
		wlr_scene_node_set_position(&ground->node, 0, 0);
		mark = wlr_scene_rect_create(bg, MARK_W, MARK_H, blue);
		wlr_scene_node_set_position(&mark->node, MARK_X, MARK_Y);
	}

	/* the socket comes up on the module's thread */
	sock = g_build_filename(run_dir, "gowl-mcp.sock", NULL);
	for (i = 0; i < 200 && !g_file_test(sock, G_FILE_TEST_EXISTS); i++)
		pump(r, 10);
	if (!g_file_test(sock, G_FILE_TEST_EXISTS)) {
		g_test_skip("the MCP socket never appeared");
		return;
	}
	client = g_socket_client_new();
	addr = g_unix_socket_address_new(sock);
	r->conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr),
	                                  NULL, &error);
	g_assert_no_error(error);

	{
		g_autoptr(JsonNode) init = rpc(r, "initialize",
			"{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
			"\"clientInfo\":{\"name\":\"test-mcp-grab\",\"version\":\"1\"}}");

		g_assert_nonnull(init);
	}
	send_line(r, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
	r->up = TRUE;
}

static void
rig_teardown(Rig *r, gconstpointer data)
{
	(void)data;
	if (r->conn != NULL) {
		g_io_stream_close(G_IO_STREAM(r->conn), NULL, NULL);
		g_clear_object(&r->conn);
	}
	if (r->compositor != NULL && r->modules != NULL)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	rm_rf(r->runtime);
	g_free(r->runtime);
	g_free(r->macros);
	g_free(r->script);
	g_free(r->argfile);
	g_string_free(r->inbuf, TRUE);
	g_ptr_array_unref(actions);
}

#define RIG_UP(r) do { if (!(r)->up) return; } while (0)

static void
test_tools_listed(Rig *r, gconstpointer data)
{
	static const gchar *const want[] = {
		"screen_text", "pick_color", "macro_record", "macro_voice_match",
		"macro_voice_status", "list_clipboard", NULL
	};
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autofree gchar *text = NULL;
	guint i;

	(void)data;
	RIG_UP(r);
	/* tools/list is paged: follow nextCursor to the end */
	{
		GString *all = g_string_new(NULL);
		g_autofree gchar *cursor = NULL;
		gint pages;

		for (pages = 0; pages < 50; pages++) {
			g_autofree gchar *params = cursor != NULL
				? g_strdup_printf("{\"cursor\":\"%s\"}", cursor)
				: g_strdup("{}");
			g_autofree gchar *page = NULL;
			JsonObject *result;

			g_clear_pointer(&reply, json_node_unref);
			reply = rpc(r, "tools/list", params);
			json_generator_set_root(gen, reply);
			page = json_generator_to_data(gen, NULL);
			g_string_append(all, page);
			result = json_object_get_object_member(
				json_node_get_object(reply), "result");
			g_clear_pointer(&cursor, g_free);
			if (result != NULL
			    && json_object_has_member(result, "nextCursor")
			    && !json_object_get_null_member(result, "nextCursor"))
				cursor = g_strdup(json_object_get_string_member(
					result, "nextCursor"));
			if (cursor == NULL)
				break;
		}
		text = g_string_free(all, FALSE);
	}
	for (i = 0; want[i] != NULL; i++) {
		g_autofree gchar *needle = g_strdup_printf("\"name\":\"%s\"", want[i]);

		if (strstr(text, needle) == NULL)
			g_error("%s is not listed (%" G_GSIZE_FORMAT " bytes)",
			        want[i], strlen(text));
	}
	/* nothing turns the microphone on */
	g_assert_null(strstr(text, "\"name\":\"macro_voice\""));
	g_assert_null(strstr(text, "\"name\":\"macro_voice_start\""));
}

static void
test_pick_color(Rig *r, gconstpointer data)
{
	g_autofree gchar *got = NULL;
	g_autofree gchar *bad = NULL;
	g_autofree gchar *clip = NULL;
	gboolean err;

	(void)data;
	RIG_UP(r);
	gowl_seat_set_clipboard(gowl_compositor_get_seat(r->compositor),
	                        "what the person copied");
	got = call(r, "pick_color", "{\"x\":230,\"y\":170}", &err);
	g_assert_false(err);
	g_assert_nonnull(strstr(got, "\"color\":\"#336699\""));
	g_assert_nonnull(strstr(got, "\"r\":51,\"g\":102,\"b\":153"));
	bad = call(r, "pick_color", "{\"x\":1}", &err);
	g_assert_true(err);
	/* read, not copied */
	clip = clipboard_text(r);
	g_assert_cmpstr(clip, ==, "what the person copied");
}

/* Whether a gowl-mcp-ocr temporary is left behind. */
static gboolean
ocr_left(Rig *r)
{
	g_autoptr(GDir) dir = g_dir_open(run_dir, 0, NULL);
	const gchar *e;

	(void)r;
	while (dir != NULL && (e = g_dir_read_name(dir)) != NULL)
		if (g_str_has_prefix(e, "gowl-mcp-ocr-"))
			return TRUE;
	return FALSE;
}

static void
test_screen_text(Rig *r, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *clip = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *clip2 = NULL;
	g_autofree gchar *bad = NULL;
	g_auto(GStrv) parts = NULL;
	gboolean err;

	(void)data;
	RIG_UP(r);
	gowl_seat_set_clipboard(gowl_compositor_get_seat(r->compositor),
	                        "what the person copied");
	text = call(r, "screen_text",
	            "{\"x\":190,\"y\":140,\"width\":120,\"height\":100}", &err);
	g_assert_false(err);
	g_assert_cmpstr(text, ==, "Text an agent read");
	/* the screenshot module's command, its language, a real PNG */
	g_assert_true(g_file_get_contents(r->argfile, &args, NULL, NULL));
	parts = g_strsplit(args, "|", -1);
	g_assert_cmpuint(g_strv_length(parts), ==, 4);
	g_assert_true(g_str_has_prefix(parts[0], run_dir));
	g_assert_cmpstr(parts[1], ==, "stdout");
	g_assert_cmpstr(parts[3], ==, "eng");
	g_assert_false(ocr_left(r));
	/* the clipboard is the person's */
	clip = clipboard_text(r);
	g_assert_cmpstr(clip, ==, "what the person copied");

	/* ... unless asked, and the language is an argument */
	again = call(r, "screen_text",
	             "{\"x\":190,\"y\":140,\"width\":120,\"height\":100,"
	             "\"language\":\"deu\",\"copy\":true}", &err);
	g_assert_false(err);
	clip2 = clipboard_text(r);
	g_assert_cmpstr(clip2, ==, "Text an agent read");
	g_clear_pointer(&args, g_free);
	g_assert_true(g_file_get_contents(r->argfile, &args, NULL, NULL));
	g_assert_true(g_str_has_suffix(args, "|deu"));

	bad = call(r, "screen_text", "{\"x\":1,\"y\":1,\"width\":0,\"height\":5}",
	           &err);
	g_assert_true(err);
	g_assert_false(ocr_left(r));
}

static void
test_macro_record_needs_consent(Rig *r, gconstpointer data)
{
	GowlInputRecorder *rec;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *started = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *cancelled = NULL;
	g_autofree gchar *bad = NULL;
	gboolean err;

	(void)data;
	RIG_UP(r);
	rec = gowl_compositor_get_input_recorder(r->compositor);
	gowl_input_recorder_set_consent(rec, FALSE);
	refused = call(r, "macro_record", "{\"action\":\"start\"}", &err);
	g_assert_true(err);
	g_assert_nonnull(strstr(refused, "input-recording"));
	g_assert_false(gowl_input_recorder_is_active(rec));

	gowl_input_recorder_set_consent(rec, TRUE);
	started = call(r, "macro_record",
	               "{\"action\":\"start\",\"name\":\"agent-made\"}", &err);
	g_assert_false(err);
	g_assert_cmpstr(started, ==, "recording agent-made");
	status = call(r, "macro_record", "{\"action\":\"status\"}", &err);
	g_assert_nonnull(strstr(status, "\"recording\":true"));
	cancelled = call(r, "macro_record", "{\"action\":\"cancel\"}", &err);
	g_assert_false(err);
	g_assert_false(gowl_input_recorder_is_active(rec));
	bad = call(r, "macro_record", "{\"action\":\"explode\"}", &err);
	g_assert_true(err);
	gowl_input_recorder_set_consent(rec, FALSE);
}

static gboolean
said(GowlMacroContext *ctx, gpointer data)
{
	g_autofree gchar *s = NULL;

	(void)data;
	s = g_strdup_printf("said %s|%s",
		gowl_macro_get_trigger(ctx) == GOWL_MACRO_TRIGGER_VOICE ? "voice"
		                                                        : "other",
		gowl_macro_get_arg(ctx, 0) != NULL ? gowl_macro_get_arg(ctx, 0)
		                                   : "-");
	gowl_macro_action(ctx, GOWL_ACTION_CUSTOM, s);
	return TRUE;
}

static void
test_voice_tools(Rig *r, gconstpointer data)
{
	g_autofree gchar *dry = NULL;
	g_autofree gchar *ran = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *status = NULL;
	gboolean err;
	gint i;

	(void)data;
	RIG_UP(r);
	gowl_macro_register_func("tile-two", said, NULL, NULL);

	dry = call(r, "macro_voice_match",
	           "{\"text\":\"Tile two, thirty.\",\"dry_run\":true}", &err);
	g_assert_false(err);
	g_assert_nonnull(strstr(dry, "\"macro\":\"tile-two\""));
	g_assert_nonnull(strstr(dry, "\"args\":\"30\""));
	g_assert_cmpuint(actions->len, ==, 0);

	ran = call(r, "macro_voice_match", "{\"text\":\"tile two\\nplease\"}",
	           &err);
	g_assert_false(err);
	for (i = 0; i < 100 && actions->len == 0; i++)
		pump(r, 10);
	g_assert_cmpuint(actions->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(actions, 0), ==, "said voice|-");

	none = call(r, "macro_voice_match", "{\"text\":\"make tea\"}", &err);
	g_assert_true(err);
	status = call(r, "macro_voice_status", "{}", &err);
	g_assert_false(err);
	g_assert_nonnull(strstr(status, "\"listening\":false"));
	/* the last sentence, matched or not */
	g_assert_nonnull(strstr(status, "\"last-heard\":\"make tea\""));
	gowl_macro_unregister_func("tile-two");
}

/* The clipboard history over MCP, with what gowl set itself in it. */
static void
test_clipboard_tools(Rig *r, gconstpointer data)
{
	g_autofree gchar *list = NULL;
	g_autofree gchar *entry = NULL;
	g_autofree gchar *copied = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *line = NULL;
	gboolean err;
	gint i;

	(void)data;
	RIG_UP(r);
	gowl_seat_set_clipboard(gowl_compositor_get_seat(r->compositor), "#336699");
	for (i = 0; i < 300; i++) {
		g_free(list);
		list = call(r, "list_clipboard", "{}", &err);
		if (strstr(list, "#336699") != NULL)
			break;
		pump(r, 10);
	}
	g_assert_nonnull(strstr(list, "#336699"));
	gowl_seat_set_clipboard(gowl_compositor_get_seat(r->compositor), "later");
	for (i = 0; i < 300 && strstr(list, "later") == NULL; i++) {
		g_free(list);
		list = call(r, "list_clipboard", "{}", &err);
		pump(r, 10);
	}
	/* the colour's id is the number its line starts with */
	{
		g_auto(GStrv) lines = g_strsplit(list, "\n", -1);
		guint k;

		for (k = 0; lines[k] != NULL; k++)
			if (strstr(lines[k], "#336699") != NULL)
				args = g_strdup_printf("{\"id\":%" G_GUINT64_FORMAT "}",
				                       g_ascii_strtoull(lines[k], NULL, 10));
	}
	g_assert_nonnull(args);
	/* the entry's stored bytes, by path: exactly what gowl set */
	entry = call(r, "get_clipboard_entry", args, &err);
	g_assert_false(err);
	g_strstrip(entry);
	g_assert_true(g_file_get_contents(entry, &line, NULL, NULL));
	g_assert_cmpstr(line, ==, "#336699");
	copied = call(r, "copy_clipboard_entry", args, &err);
	g_assert_false(err);
	now = clipboard_text(r);
	g_assert_cmpstr(now, ==, "#336699");
}

int
main(
	int    argc,
	char **argv
){
	g_autofree gchar *home = NULL;
	gint rc;

	/* GLib caches the XDG dirs on first use: point them somewhere
	   private before anything asks */
	home = g_build_filename(g_get_tmp_dir(), "gowl-mcpgrab-home-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(home, 0700));
	{
		const gchar *const vars[] = { "XDG_CACHE_HOME", "XDG_STATE_HOME",
		                              "XDG_CONFIG_HOME", "XDG_DATA_HOME",
		                              NULL };
		guint i;

		for (i = 0; vars[i] != NULL; i++) {
			g_autofree gchar *d = g_build_filename(home, vars[i], NULL);

			g_mkdir(d, 0700);
			g_setenv(vars[i], d, TRUE);
		}
	}
	g_unsetenv("GOWL_MACRO_DIR");
	run_dir = g_build_filename(home, "run", NULL);
	g_mkdir(run_dir, 0700);
	g_setenv("XDG_RUNTIME_DIR", run_dir, TRUE);
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_test_init(&argc, &argv, NULL);
#define ADD(path, fn) \
	g_test_add("/mcp-grab/" path, Rig, NULL, rig_setup, fn, rig_teardown)
	ADD("tools-listed", test_tools_listed);
	ADD("pick-color", test_pick_color);
	ADD("screen-text", test_screen_text);
	ADD("macro-record-needs-consent", test_macro_record_needs_consent);
	ADD("voice-tools", test_voice_tools);
	ADD("clipboard-tools", test_clipboard_tools);
#undef ADD
	rc = g_test_run();
	rm_rf(home);
	g_free(run_dir);
	return rc;
}
