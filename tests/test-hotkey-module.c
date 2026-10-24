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
 * test-hotkey-module.c - The hotkey module end to end.
 *
 * The real hotkey.so -- with gabbrev.so (hotstrings) and macro.so
 * (macro actions) beside it -- in a headless compositor, a keyboard and
 * a pointer plugged in through the backend so keys take the hardware
 * path, and a real Wayland client recording every key that reaches its
 * windows.  Hotkeys "fire" into the compositor's custom-action handler,
 * which the test records, so what ran and in which order is visible.
 *
 * Covers: a hotkey swallowed and fired; `~'; an unbound key; a
 * CapsLock combination typing {Down} with the lock left alone; a remap
 * onto Control holding real modifiers; Send typing text, lifting the
 * hotkey's own modifiers, and a hotkey's Send not firing hotkeys again;
 * contexts over the focused window's app-id; a long press held and
 * tapped (the key put back); modes entered, timed out and one-shot;
 * the suspend key; shell (waited for, with the environment) and run
 * actions; YAML, C and custom-form functions; macro actions; inline C
 * (and a crashing one contained); timers; event hooks; an input hook;
 * window actions through the wire protocol; mouse and wheel hotkeys;
 * hotstrings and X hotstrings through the real gabbrev; every IPC
 * command's OK and ERROR; the JSON-RPC methods registered when the IPC
 * server arrives and gone when the module is switched off; and a remap
 * target released when the module is.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#include "macro-test-client.h"
#include "hotkey/gowl-hotkey-private.h"

static const gchar * const titles[] = { "Editor", "Prompt", NULL };
static const gchar * const app_ids[] = {
	"org.example.Editor", "org.example.Prompt", NULL
};

static struct wlr_keyboard_impl fake_keyboard_impl;
static struct wlr_pointer_impl  fake_pointer_impl;
static gchar *scratch;

typedef struct {
	Rig                  r;
	struct wlr_keyboard *kb;
	struct wlr_pointer  *ptr;
	gboolean             up;
	GPtrArray           *custom;   /* gchar*: custom actions, in order */
	GPtrArray           *events;   /* gchar*: the module's EVENT lines */
	gulong               event_handler;
	GowlIpc             *ipc;
} Bench;

static gboolean
custom_action(
	GowlCompositor *compositor,
	const gchar    *arg,
	gpointer        data
){
	Bench *b = data;

	(void)compositor;
	g_ptr_array_add(b->custom, g_strdup(arg));
	return TRUE;
}

static void
on_module_event(
	GowlCompositor *compositor,
	const gchar    *module,
	const gchar    *text,
	gpointer        data
){
	Bench *b = data;

	(void)compositor;
	if (g_strcmp0(module, "hotkey") == 0)
		g_ptr_array_add(b->events, g_strdup(text));
}

/* ── Commands ──────────────────────────────────────────────────────── */

static gchar *
command(
	Bench       *b,
	const gchar *line
){
	gchar *reply = gowl_compositor_run_command(b->r.compositor, line);

	g_assert_nonnull(reply);
	return reply;
}

static void
command_ok(
	Bench       *b,
	const gchar *line
){
	g_autofree gchar *reply = command(b, line);

	if (!g_str_has_prefix(reply, "OK"))
		g_error("`%s' answered `%s'", line, reply);
}

static void
command_error(
	Bench       *b,
	const gchar *line,
	const gchar *mentions
){
	g_autofree gchar *reply = command(b, line);

	if (!g_str_has_prefix(reply, "ERROR") || strstr(reply, mentions) == NULL)
		g_error("`%s' answered `%s', want an ERROR about `%s'", line, reply,
		        mentions);
}

static JsonNode *
command_json(
	Bench       *b,
	const gchar *line
){
	g_autofree gchar *reply = command(b, line);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	if (!g_str_has_prefix(reply, "OK "))
		g_error("`%s' answered `%s'", line, reply);
	g_assert_true(json_parser_load_from_data(parser, reply + 3, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

/* A command whose reply must start with PREFIX */
static void
command_expect(
	Bench       *b,
	const gchar *line,
	const gchar *prefix
){
	g_autofree gchar *reply = command(b, line);

	if (!g_str_has_prefix(reply, prefix))
		g_error("`%s' answered `%s', not `%s...'", line, reply, prefix);
}

/* Whether hotkey-list holds a hotkey with this id */
static gboolean
has_hotkey(
	Bench       *b,
	const gchar *id
){
	g_autoptr(JsonNode) list = command_json(b, "hotkey-list");
	JsonArray *arr = json_node_get_array(list);
	guint i;

	for (i = 0; i < json_array_get_length(arr); i++) {
		JsonObject *o = json_array_get_object_element(arr, i);

		if (json_object_has_member(o, "id")
		    && g_strcmp0(json_object_get_string_member(o, "id"), id) == 0)
			return TRUE;
	}
	return FALSE;
}

/* hotkey-add, which must be OK */
static void
add(
	Bench       *b,
	const gchar *yaml
){
	g_autofree gchar *line = g_strdup_printf("hotkey-add %s", yaml);

	command_ok(b, line);
}

/* ── Input ─────────────────────────────────────────────────────────── */

static void
key(
	Bench   *b,
	guint32  code,
	gboolean pressed
){
	struct wlr_keyboard_key_event ev;

	memset(&ev, 0, sizeof ev);
	ev.time_msec = 1;
	ev.keycode = code;
	ev.update_state = TRUE;
	ev.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                   : WL_KEYBOARD_KEY_STATE_RELEASED;
	wlr_keyboard_notify_key(b->kb, &ev);
	pump(&b->r, 3);
}

static void
tap(
	Bench   *b,
	guint32  code
){
	key(b, code, TRUE);
	key(b, code, FALSE);
}

/* A chord: every key down in order, then up in reverse. */
static void
chord(
	Bench         *b,
	const guint32 *codes,
	guint          n
){
	guint i;

	for (i = 0; i < n; i++)
		key(b, codes[i], TRUE);
	for (i = n; i > 0; i--)
		key(b, codes[i - 1], FALSE);
}

static void
button(
	Bench    *b,
	guint32   code,
	gboolean  pressed
){
	struct wlr_pointer_button_event ev;

	memset(&ev, 0, sizeof ev);
	ev.pointer = b->ptr;
	ev.time_msec = 1;
	ev.button = code;
	ev.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
	                   : WL_POINTER_BUTTON_STATE_RELEASED;
	wl_signal_emit_mutable(&b->ptr->events.button, &ev);
	pump(&b->r, 3);
}

static void
wheel(
	Bench  *b,
	gint32  v120
){
	struct wlr_pointer_axis_event ev;

	memset(&ev, 0, sizeof ev);
	ev.pointer = b->ptr;
	ev.time_msec = 1;
	ev.source = WL_POINTER_AXIS_SOURCE_WHEEL;
	ev.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
	ev.delta = (gdouble)v120 / 8.0;
	ev.delta_discrete = v120;
	wl_signal_emit_mutable(&b->ptr->events.axis, &ev);
	pump(&b->r, 3);
}

/* ── What happened ─────────────────────────────────────────────────── */

/* Waits until custom action @arg has run (or the time is up). */
static gboolean
await_custom(
	Bench       *b,
	const gchar *arg,
	gint         ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
	guint i;

	do {
		pump(&b->r, 5);
		for (i = 0; i < b->custom->len; i++)
			if (g_strcmp0(g_ptr_array_index(b->custom, i), arg) == 0)
				return TRUE;
	} while (g_get_monotonic_time() < deadline);
	for (i = 0; i < b->custom->len; i++)
		g_test_message("custom %u: %s", i,
		               (const gchar *)g_ptr_array_index(b->custom, i));
	return FALSE;
}

static guint
count_custom(
	Bench       *b,
	const gchar *arg
){
	guint i;
	guint n = 0;

	for (i = 0; i < b->custom->len; i++)
		if (g_strcmp0(g_ptr_array_index(b->custom, i), arg) == 0)
			n++;
	return n;
}

static gboolean
await_event(
	Bench       *b,
	const gchar *prefix,
	gint         ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
	guint i;

	do {
		pump(&b->r, 5);
		for (i = 0; i < b->events->len; i++)
			if (g_str_has_prefix(g_ptr_array_index(b->events, i), prefix))
				return TRUE;
	} while (g_get_monotonic_time() < deadline);
	for (i = 0; i < b->events->len; i++)
		g_test_message("event %u: %s", i,
		               (const gchar *)g_ptr_array_index(b->events, i));
	return FALSE;
}

/* Did window @w get a press of @code since reset_keys()? */
static gboolean
pressed_in(
	Bench   *b,
	gint     w,
	guint32  code
){
	gboolean seen = FALSE;
	gint i;

	pump(&b->r, 20);
	g_mutex_lock(&b->r.client.lock);
	for (i = 0; i < b->r.client.n_keys; i++) {
		KeySeen *k = &b->r.client.keys[i];

		if (k->window == w && k->state == 1 && k->key == code)
			seen = TRUE;
	}
	g_mutex_unlock(&b->r.client.lock);
	return seen;
}

/* The presses window 0 saw, as "code" or "code/mods" with modifiers */
static gchar *
presses(Bench *b)
{
	GString *out = g_string_new(NULL);
	gint i;

	pump(&b->r, 30);
	g_mutex_lock(&b->r.client.lock);
	for (i = 0; i < b->r.client.n_keys; i++) {
		KeySeen *k = &b->r.client.keys[i];

		if (k->window != 0 || k->state != 1)
			continue;
		if (out->len > 0)
			g_string_append_c(out, ' ');
		if (k->mods != 0)
			g_string_append_printf(out, "%u/%u", k->key, k->mods);
		else
			g_string_append_printf(out, "%u", k->key);
	}
	g_mutex_unlock(&b->r.client.lock);
	return g_string_free(out, FALSE);
}

/* Waits until window 0 has seen @n presses, then returns them all. */
static gchar *
await_presses(
	Bench *b,
	gint   n
){
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	gint have;

	do {
		gint i;

		pump(&b->r, 5);
		have = 0;
		g_mutex_lock(&b->r.client.lock);
		for (i = 0; i < b->r.client.n_keys; i++)
			if (b->r.client.keys[i].window == 0
			    && b->r.client.keys[i].state == 1)
				have++;
		g_mutex_unlock(&b->r.client.lock);
	} while (have < n && g_get_monotonic_time() < deadline);
	return presses(b);
}

/* ── The bench ─────────────────────────────────────────────────────── */

static void
bench_up(
	Bench         *b,
	gconstpointer  data
){
	static const gchar *modules[] = {
		GOWL_TEST_MODULE_DIR "/hotkey.so",
		GOWL_TEST_MODULE_DIR "/gabbrev.so",
		GOWL_TEST_MODULE_DIR "/macro.so",
		NULL
	};

	(void)data;
	memset(b, 0, sizeof *b);
	b->custom = g_ptr_array_new_with_free_func(g_free);
	b->events = g_ptr_array_new_with_free_func(g_free);
	fake_keyboard_impl.name = "gowl-test-keyboard";
	fake_pointer_impl.name = "gowl-test-pointer";
	gowl_hotkey_clear_sources();
	if (!rig_up(&b->r, titles, app_ids, modules))
		return;
	b->kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(b->kb, &fake_keyboard_impl, "test-kb");
	wl_signal_emit_mutable(&b->r.compositor->backend->events.new_input,
	                       &b->kb->base);
	b->ptr = g_new0(struct wlr_pointer, 1);
	wlr_pointer_init(b->ptr, &fake_pointer_impl, "test-pointer");
	wl_signal_emit_mutable(&b->r.compositor->backend->events.new_input,
	                       &b->ptr->base);
	pump(&b->r, 20);
	gowl_compositor_set_custom_action_handler(b->r.compositor, custom_action,
	                                          b);
	b->event_handler = g_signal_connect(b->r.compositor, "module-event",
	                                    G_CALLBACK(on_module_event), b);
	/* No toasts: there is no bar to show them */
	{
		g_autoptr(GHashTable) h = g_hash_table_new(g_str_hash, g_str_equal);
		GowlModule *m = gowl_module_manager_find_module(b->r.modules,
		                                                "hotkey");

		g_assert_nonnull(m);
		g_hash_table_insert(h, (gpointer)"toast", (gpointer)"false");
		gowl_module_configure(m, h);
	}
	focus_window(&b->r, 0);
	reset_keys(&b->r);
	b->up = TRUE;
}

static void
bench_down(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	if (b->ipc != NULL && b->r.compositor != NULL)
		gowl_compositor_set_ipc(b->r.compositor, NULL);
	g_clear_object(&b->ipc);
	if (b->kb != NULL) {
		wlr_keyboard_finish(b->kb);
		g_free(b->kb);
	}
	if (b->ptr != NULL) {
		wlr_pointer_finish(b->ptr);
		g_free(b->ptr);
	}
	if (b->event_handler != 0 && b->r.compositor != NULL)
		g_signal_handler_disconnect(b->r.compositor, b->event_handler);
	rig_down(&b->r);
	gowl_hotkey_clear_sources();
	g_clear_pointer(&b->custom, g_ptr_array_unref);
	g_clear_pointer(&b->events, g_ptr_array_unref);
}

#define BENCH_UP(b) do { if (!(b)->up) return; } while (0)

/* ── Hotkeys ───────────────────────────────────────────────────────── */

static void
test_fire_and_swallow(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 super_alt_t[] = { KEY_LEFTMETA, KEY_LEFTALT, KEY_T };
	static const guint32 ctrl_b[] = { KEY_LEFTCTRL, KEY_B };

	(void)data;
	BENCH_UP(b);
	add(b, "{key: \"#!t\", do: {custom: fired-t}}");
	add(b, "{key: \"~^b\", do: {custom: fired-b}}");
	chord(b, super_alt_t, 3);
	g_assert_true(await_custom(b, "fired-t", 1000));
	g_assert_true(await_event(b, "fired #!t", 500));
	/* the key itself never reached the window */
	g_assert_false(pressed_in(b, 0, KEY_T));
	/* ~: fired, and the window got it */
	chord(b, ctrl_b, 2);
	g_assert_true(await_custom(b, "fired-b", 1000));
	g_assert_true(pressed_in(b, 0, KEY_B));
	/* a key with no hotkey is a key */
	tap(b, KEY_A);
	g_assert_true(pressed_in(b, 0, KEY_A));
	g_assert_cmpuint(count_custom(b, "fired-t"), ==, 1);
}

/* CapsLock & j types Down, CapsLock reaches nobody, and the lock is
 * left as it was. */
static void
test_capslock_layer(
	Bench         *b,
	gconstpointer  data
){
	gboolean caps = TRUE;
	g_autofree gchar *got = NULL;

	(void)data;
	BENCH_UP(b);
	add(b, "hotkeys: [{key: \"CapsLock & j\", do: {send: \"{Down}\"}}, "
	       "{key: \"CapsLock & k\", do: {send: \"{Up}\"}}]");
	key(b, KEY_CAPSLOCK, TRUE);
	tap(b, KEY_J);
	tap(b, KEY_K);
	key(b, KEY_CAPSLOCK, FALSE);
	got = await_presses(b, 2);
	g_assert_cmpstr(got, ==, "108 103");      /* KEY_DOWN, KEY_UP */
	pump(&b->r, 30);
	g_assert_true(gowl_compositor_get_lock_state(b->r.compositor, &caps,
	                                             NULL));
	g_assert_false(caps);
}

/* CapsLock held is Control: j arrives with Control down. */
static void
test_remap_to_control(
	Bench         *b,
	gconstpointer  data
){
	gboolean caps = TRUE;
	g_autofree gchar *got = NULL;

	(void)data;
	BENCH_UP(b);
	add(b, "remaps: {CapsLock: Control_L}");
	key(b, KEY_CAPSLOCK, TRUE);
	tap(b, KEY_J);
	key(b, KEY_CAPSLOCK, FALSE);
	got = await_presses(b, 2);
	/* LeftCtrl (29), then j with the Control modifier (mask 4) */
	g_assert_cmpstr(got, ==, "29/4 36/4");
	gowl_compositor_get_lock_state(b->r.compositor, &caps, NULL);
	g_assert_false(caps);
	/* switching the module off lets go of a target still held */
	key(b, KEY_CAPSLOCK, TRUE);
	pump(&b->r, 20);
	reset_keys(&b->r);
	gowl_module_deactivate(gowl_module_manager_find_module(b->r.modules,
	                                                       "hotkey"));
	pump(&b->r, 30);
	{
		gboolean released = FALSE;
		gint i;

		g_mutex_lock(&b->r.client.lock);
		for (i = 0; i < b->r.client.n_keys; i++)
			if (b->r.client.keys[i].key == KEY_LEFTCTRL
			    && b->r.client.keys[i].state == 0)
				released = TRUE;
		g_mutex_unlock(&b->r.client.lock);
		g_assert_true(released);
	}
	key(b, KEY_CAPSLOCK, FALSE);
}

static void
test_send(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 super_alt_s[] = { KEY_LEFTMETA, KEY_LEFTALT, KEY_S };
	g_autofree gchar *got = NULL;

	(void)data;
	BENCH_UP(b);
	add(b, "{key: F1, do: {send-text: \"hi\"}}");
	add(b, "{key: \"#!s\", do: {send: \"x\"}}");
	tap(b, KEY_F1);
	got = await_presses(b, 2);
	g_assert_cmpstr(got, ==, "35 23");        /* h i */
	g_free(got);
	reset_keys(&b->r);
	/* the hotkey's own Super and Alt are lifted for the send: x arrives
	 * with no modifier, after the two the user holds */
	chord(b, super_alt_s, 3);
	got = await_presses(b, 3);
	g_assert_nonnull(strstr(got, " 45"));     /* KEY_X, unmodified */
	g_assert_null(strstr(got, "45/"));
	g_assert_true(g_str_has_prefix(got, "125"));
}

/* A hotkey whose Send types its own key does not fire itself again. */
static void
test_send_does_not_refire(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	add(b, "{key: a, do: [{send: \"a\"}, {custom: sent-a}]}");
	tap(b, KEY_A);
	g_assert_true(await_custom(b, "sent-a", 1000));
	pump(&b->r, 100);
	g_assert_cmpuint(count_custom(b, "sent-a"), ==, 1);
	/* the window got exactly the one `a' the hotkey typed */
	{
		g_autofree gchar *got = presses(b);

		g_assert_cmpstr(got, ==, "30");
	}
}

static void
test_context(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 ctrl_e[] = { KEY_LEFTCTRL, KEY_E };

	(void)data;
	BENCH_UP(b);
	add(b, "contexts: {editor: \"app-id=org.example.Editor\"}\n"
	       "hotkeys: [{key: \"^e\", context: editor, do: {custom: in-editor}}, "
	       "{key: \"^e\", when: \"title=Prompt\", do: {custom: in-prompt}}]");
	chord(b, ctrl_e, 2);
	g_assert_true(await_custom(b, "in-editor", 1000));
	focus_window(&b->r, 1);
	chord(b, ctrl_e, 2);
	g_assert_true(await_custom(b, "in-prompt", 1000));
	g_assert_cmpuint(count_custom(b, "in-editor"), ==, 1);
}

static void
test_long_press(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	add(b, "{key: Esc, hold: 80, do: {custom: held}}");
	/* tapped: Escape is put back */
	tap(b, KEY_ESC);
	g_assert_true(pressed_in(b, 0, KEY_ESC));
	g_assert_cmpuint(count_custom(b, "held"), ==, 0);
	reset_keys(&b->r);
	/* held: the hotkey, and the window sees nothing */
	key(b, KEY_ESC, TRUE);
	pump(&b->r, 150);
	key(b, KEY_ESC, FALSE);
	g_assert_true(await_custom(b, "held", 1000));
	g_assert_false(pressed_in(b, 0, KEY_ESC));
}

static void
test_modes(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 super_r[] = { KEY_LEFTMETA, KEY_R };
	static const guint32 super_o[] = { KEY_LEFTMETA, KEY_O };

	(void)data;
	BENCH_UP(b);
	add(b, "modes:\n"
	       "  resize:\n"
	       "    on-enter: {custom: entered}\n"
	       "    on-exit: {custom: left}\n"
	       "    hotkeys: [{key: h, do: {custom: shrink}}, "
	       "{key: Esc, do: {mode: default}}]\n"
	       "  quick: {timeout: 100}\n"
	       "  once: {oneshot: true, hotkeys: [{key: o, do: {custom: once}}]}\n"
	       "hotkeys:\n"
	       "  - {key: \"#r\", do: {mode: resize}}\n"
	       "  - {key: \"#q\", do: {mode: quick}}\n"
	       "  - {key: \"#o\", do: {mode: once}}\n");
	chord(b, super_r, 2);
	g_assert_true(await_custom(b, "entered", 1000));
	g_assert_cmpstr(gowl_compositor_get_key_mode(b->r.compositor), ==,
	                "resize");
	tap(b, KEY_H);
	g_assert_true(await_custom(b, "shrink", 1000));
	g_assert_false(pressed_in(b, 0, KEY_H));
	tap(b, KEY_ESC);
	g_assert_true(await_custom(b, "left", 1000));
	g_assert_cmpstr(gowl_compositor_get_key_mode(b->r.compositor), ==,
	                "default");
	/* in the default mode, h is h again */
	tap(b, KEY_H);
	g_assert_true(pressed_in(b, 0, KEY_H));
	/* a timeout */
	command_ok(b, "hotkey-mode quick");
	g_assert_cmpstr(gowl_compositor_get_key_mode(b->r.compositor), ==,
	                "quick");
	pump(&b->r, 250);
	g_assert_cmpstr(gowl_compositor_get_key_mode(b->r.compositor), ==,
	                "default");
	/* one-shot: back after one hotkey */
	chord(b, super_o, 2);
	pump(&b->r, 30);
	g_assert_cmpstr(gowl_compositor_get_key_mode(b->r.compositor), ==,
	                "once");
	tap(b, KEY_O);
	g_assert_true(await_custom(b, "once", 1000));
	pump(&b->r, 30);
	g_assert_cmpstr(gowl_compositor_get_key_mode(b->r.compositor), ==,
	                "default");
}

static void
test_suspend_key(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 suspend[] = { KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_PAUSE };
	g_autoptr(JsonNode) st = NULL;

	(void)data;
	BENCH_UP(b);
	add(b, "hotkeys: [{key: a, do: {custom: a}}, "
	       "{key: b, suspend-exempt: true, do: {custom: b}}]");
	chord(b, suspend, 3);
	g_assert_true(await_event(b, "suspended on", 500));
	st = command_json(b, "hotkey-status");
	g_assert_true(json_object_get_boolean_member(json_node_get_object(st),
	                                             "suspended"));
	tap(b, KEY_A);
	g_assert_true(pressed_in(b, 0, KEY_A));
	tap(b, KEY_B);
	g_assert_true(await_custom(b, "b", 1000));
	chord(b, suspend, 3);
	g_assert_true(await_event(b, "suspended off", 500));
	tap(b, KEY_A);
	g_assert_true(await_custom(b, "a", 1000));
	/* the suspend key never reached the window */
	g_assert_false(pressed_in(b, 0, KEY_PAUSE));
}

/* ── Actions ───────────────────────────────────────────────────────── */

static void
test_shell_and_run(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *file = g_build_filename(scratch, "shell-out", NULL);
	g_autofree gchar *touched = g_build_filename(scratch, "touched", NULL);
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *content = NULL;
	gint64 deadline;

	(void)data;
	BENCH_UP(b);
	g_unlink(file);
	g_unlink(touched);
	/* waited for: the custom action runs after the shell has written */
	yaml = g_strdup_printf("{id: sh, key: F2, do: [{shell: \"printf '%%s %%s' "
	                       "\\\"$GOWL_HOTKEY_ID\\\" \\\"$GOWL_HOTKEY_APP_ID\\\" > "
	                       "%s\", wait: true}, {custom: after-shell}]}", file);
	add(b, yaml);
	g_free(yaml);
	yaml = g_strdup_printf("{key: F3, do: {run: [touch, \"%s\"]}}", touched);
	add(b, yaml);
	tap(b, KEY_F2);
	g_assert_true(await_custom(b, "after-shell", 3000));
	g_assert_true(g_file_get_contents(file, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "sh org.example.Editor");
	tap(b, KEY_F3);
	deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	while (!g_file_test(touched, G_FILE_TEST_EXISTS)
	       && g_get_monotonic_time() < deadline)
		pump(&b->r, 10);
	g_assert_true(g_file_test(touched, G_FILE_TEST_EXISTS));
	g_unlink(file);
	g_unlink(touched);
}

static void
c_echo(
	GowlHotkeyCall *call,
	gpointer        data
){
	g_autofree gchar *joined = g_strjoinv(",",
		(gchar **)gowl_hotkey_call_get_args(call));

	(void)data;
	gowl_hotkey_call_set_result(call, joined);
}

static void
test_functions(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *reply = NULL;

	(void)data;
	BENCH_UP(b);
	gowl_hotkey_register_func("c-echo", c_echo, NULL, NULL);
	add(b, "functions: {tidy: [{custom: tidy-1}, {custom: tidy-2}]}\n"
	       "hotkeys: [{key: F4, do: [{call: tidy}, {custom: after-call}]}]");
	tap(b, KEY_F4);
	g_assert_true(await_custom(b, "after-call", 1000));
	g_assert_cmpstr(g_ptr_array_index(b->custom, 0), ==, "tidy-1");
	g_assert_cmpstr(g_ptr_array_index(b->custom, 1), ==, "tidy-2");
	/* a C function's result is hotkey-call's reply */
	reply = command(b, "hotkey-call c-echo one 'two words'");
	g_assert_cmpstr(reply, ==, "OK one,two words");
	g_clear_pointer(&reply, g_free);
	/* a YAML function is started */
	reply = command(b, "hotkey-call tidy");
	g_assert_cmpstr(reply, ==, "OK started");
	g_clear_pointer(&reply, g_free);
	/* a custom form (cmacs's Elisp functions), arguments as literals */
	command_ok(b, "hotkey-func-define say (say %i %a)");
	command_ok(b, "hotkey-call say x 'y \"z\"'");
	g_assert_true(await_custom(b, "(say \"say\" (list \"x\" \"y \\\"z\\\"\"))",
	                           1000));
	command_error(b, "hotkey-call nobody", "no function");
	gowl_hotkey_unregister_func("c-echo");
}

static void
test_macro_action(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	command_ok(b, "macro-define pip custom macro-ran");
	add(b, "{key: F5, do: [{macro: pip}, {custom: after-macro}]}");
	tap(b, KEY_F5);
	g_assert_true(await_custom(b, "macro-ran", 2000));
	g_assert_true(await_custom(b, "after-macro", 1000));
}

static void
test_c_action(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(JsonNode) st = NULL;
	GLogLevelFlags saved;
	gint64 failed;

	(void)data;
	BENCH_UP(b);
	add(b, "{key: F6, do: [{c: \"g_free(gowl_hotkey_command(comp, "
	       "\\\"action custom c-ran\\\"));\"}, {custom: after-c}]}");
	add(b, "{key: F7, do: [{c: \"*(volatile int *)0 = 1;\"}, "
	       "{custom: after-crash}]}");
	tap(b, KEY_F6);
	/* the first run builds it (gcc), then runs it */
	g_assert_true(await_custom(b, "c-ran", 30000));
	g_assert_true(await_custom(b, "after-c", 1000));
	/* again: already built */
	g_ptr_array_set_size(b->custom, 0);
	tap(b, KEY_F6);
	g_assert_true(await_custom(b, "c-ran", 1000));
	/* a crash is contained: reported, the list goes on, nothing dies
	 * (the guard's warning about it is expected) */
	saved = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
	tap(b, KEY_F7);
	g_assert_true(await_custom(b, "after-crash", 30000));
	g_log_set_always_fatal(saved);
	g_assert_true(await_event(b, "fault", 500));
	st = command_json(b, "hotkey-status");
	failed = json_object_get_int_member(json_node_get_object(st), "failed");
	g_assert_cmpint(failed, >=, 1);
}

static void
test_timers(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	add(b, "timers: [{id: tick, every: 40, do: {custom: tick}}, "
	       "{id: once, once: 30, do: {custom: once}}]");
	pump(&b->r, 250);
	g_assert_cmpuint(count_custom(b, "tick"), >=, 3);
	g_assert_cmpuint(count_custom(b, "once"), ==, 1);
	command_ok(b, "hotkey-timer-disable tick");
	g_ptr_array_set_size(b->custom, 0);
	pump(&b->r, 150);
	g_assert_cmpuint(count_custom(b, "tick"), ==, 0);
	command_error(b, "hotkey-timer-disable nope", "no timer");
}

static void
test_hooks(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	add(b, "on:\n"
	       "  focus: [{filter: \"app-id=org.example.Prompt\", "
	       "do: {custom: prompt-focused}}]\n"
	       "  mode: {custom: mode-changed}\n");
	focus_window(&b->r, 1);
	g_assert_true(await_custom(b, "prompt-focused", 1000));
	focus_window(&b->r, 0);
	pump(&b->r, 30);
	g_assert_cmpuint(count_custom(b, "prompt-focused"), ==, 1);
	command_ok(b, "hotkey-mode default");
	gowl_compositor_set_key_mode(b->r.compositor, "elsewhere");
	g_assert_true(await_custom(b, "mode-changed", 1000));
}

static void
test_input_hook(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	add(b, "{key: F8, do: [{input: tag, length: 2, end-keys: [Enter]}, "
	       "{custom: after-input}]}");
	tap(b, KEY_F8);
	pump(&b->r, 20);
	tap(b, KEY_1);
	g_assert_cmpuint(count_custom(b, "after-input"), ==, 0);
	tap(b, KEY_2);
	g_assert_true(await_custom(b, "after-input", 1000));
	g_assert_true(await_event(b, "input tag max 12", 500));
	/* swallowed while it collected */
	g_assert_false(pressed_in(b, 0, KEY_1));
	/* an end key ends it too */
	tap(b, KEY_F8);
	pump(&b->r, 20);
	tap(b, KEY_3);
	tap(b, KEY_ENTER);
	g_assert_true(await_event(b, "input tag endkey:Enter 3", 500));
	g_assert_false(pressed_in(b, 0, KEY_ENTER));
	/* from IPC, with a timeout */
	command_ok(b, "hotkey-input-start probe T50");
	g_assert_true(await_event(b, "input probe timeout", 500));
	command_error(b, "hotkey-input-stop", "no input hook");
}

static void
test_window_actions(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *sock = g_build_filename(scratch, "hk-ipc.sock", NULL);

	(void)data;
	BENCH_UP(b);
	b->ipc = gowl_ipc_new(sock);
	gowl_compositor_set_ipc(b->r.compositor, b->ipc);
	add(b, "groups: {examples: \"app-id=org.example.*\"}\n"
	       "hotkeys:\n"
	       "  - {key: F9, do: {window: float}}\n"
	       "  - {key: F10, do: {window: group-activate, arg: examples}}\n"
	       "  - {key: F11, do: {window: float, target: \"title=Prompt\"}}\n");
	g_assert_false(gowl_client_get_floating(b->r.win[0]));
	tap(b, KEY_F9);
	pump(&b->r, 50);
	g_assert_true(gowl_client_get_floating(b->r.win[0]));
	/* a target instead of the focused window */
	g_assert_false(gowl_client_get_floating(b->r.win[1]));
	tap(b, KEY_F11);
	pump(&b->r, 50);
	g_assert_true(gowl_client_get_floating(b->r.win[1]));
	/* the next window of the group */
	tap(b, KEY_F10);
	g_assert_cmpint(focused_now(&b->r), ==, 1);
	tap(b, KEY_F10);
	g_assert_cmpint(focused_now(&b->r), ==, 0);
}

static void
test_mouse_hotkeys(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	add(b, "hotkeys: [{key: XButton1, do: {custom: back}}, "
	       "{key: \"^WheelUp\", do: {custom: zoom}}, "
	       "{key: \"XButton2 & WheelDown\", do: {custom: combo}}]");
	button(b, BTN_SIDE, TRUE);
	button(b, BTN_SIDE, FALSE);
	g_assert_true(await_custom(b, "back", 1000));
	key(b, KEY_LEFTCTRL, TRUE);
	wheel(b, -120);
	key(b, KEY_LEFTCTRL, FALSE);
	g_assert_true(await_custom(b, "zoom", 1000));
	/* without Control the wheel is the window's */
	wheel(b, -120);
	pump(&b->r, 30);
	g_assert_cmpuint(count_custom(b, "zoom"), ==, 1);
	button(b, BTN_EXTRA, TRUE);
	wheel(b, 120);
	button(b, BTN_EXTRA, FALSE);
	g_assert_true(await_custom(b, "combo", 1000));
}

/* ── Hotstrings, through the real gabbrev ──────────────────────────── */

static void
test_hotstrings(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(JsonNode) st = NULL;
	g_autofree gchar *got = NULL;

	(void)data;
	BENCH_UP(b);
	add(b, "hotstrings: [\":*:;hk::ok\", "
	       "{hotstring: \":*X:;hx::\", do: {custom: hotstring-ran}}]");
	st = command_json(b, "hotkey-status");
	g_assert_true(json_object_get_boolean_member(json_node_get_object(st),
	                                             "hotstrings-active"));
	tap(b, KEY_SEMICOLON);
	tap(b, KEY_H);
	tap(b, KEY_K);
	/* ; h k, three Backspaces, o k */
	got = await_presses(b, 8);
	g_assert_cmpstr(got, ==, "39 35 37 14 14 14 24 37");
	tap(b, KEY_SEMICOLON);
	tap(b, KEY_H);
	tap(b, KEY_X);
	g_assert_true(await_custom(b, "hotstring-ran", 2000));
}

/* ── The IPC ───────────────────────────────────────────────────────── */

static void
test_ipc(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(JsonNode) list = NULL;
	g_autoptr(JsonNode) st = NULL;
	g_autoptr(JsonNode) hist = NULL;
	g_autoptr(JsonNode) ks = NULL;
	g_autofree gchar *reply = NULL;
	JsonArray *arr;
	gboolean caps = FALSE;
	guint i;
	gboolean found = FALSE;

	(void)data;
	BENCH_UP(b);
	reply = command(b, "hotkey-help");
	g_assert_true(g_str_has_prefix(reply, "OK hotkey-status"));
	add(b, "{id: t1, key: \"Super+Alt+t\", description: test, "
	       "do: {custom: t1}}");
	list = command_json(b, "hotkey-list");
	arr = json_node_get_array(list);
	for (i = 0; i < json_array_get_length(arr); i++) {
		JsonObject *o = json_array_get_object_element(arr, i);

		if (g_strcmp0(json_object_get_string_member(o, "id"), "t1") == 0) {
			found = TRUE;
			g_assert_cmpstr(json_object_get_string_member(o, "canonical"),
			                ==, "#!t");
			g_assert_cmpstr(json_object_get_string_member(o, "origin"),
			                ==, "t1");
		}
	}
	g_assert_true(found);
	/* refused, and nothing half-added */
	command_error(b, "hotkey-add {key: Frob, do: {custom: x}}", "Frob");
	command_error(b, "hotkey-add {key: a, do: {sneeze: x}}", "one of");
	command_error(b, "hotkey-add", "needs YAML");
	/* a JSON entry is an entry; --id names a section, re-adding under it
	 * replaces it, and removing it takes all of it back */
	command_expect(b, "hotkey-add {\"id\":\"j1\",\"key\":\"F9\","
	                  "\"do\":{\"custom\":\"j1\"}}", "OK added j1");
	command_expect(b, "hotkey-add --id=sec {\"hotkeys\":[{\"id\":\"s1\","
	                  "\"key\":\"F10\",\"do\":{\"custom\":\"s1\"}},"
	                  "{\"id\":\"s2\",\"key\":\"F11\","
	                  "\"do\":{\"custom\":\"s2\"}}]}", "OK added sec");
	g_assert_true(has_hotkey(b, "s1") && has_hotkey(b, "s2"));
	command_expect(b, "hotkey-add --id=sec {key: F10, id: s3, "
	                  "do: {custom: s3}}", "OK added sec");
	g_assert_true(has_hotkey(b, "s3"));
	g_assert_false(has_hotkey(b, "s1") || has_hotkey(b, "s2"));
	command_ok(b, "hotkey-remove sec");
	g_assert_false(has_hotkey(b, "s3"));
	command_error(b, "hotkey-add --id= {key: a}", "needs a name");
	/* enable / disable / toggle / fire */
	command_ok(b, "hotkey-disable t1");
	command_error(b, "hotkey-disable nope", "no hotkey");
	command_ok(b, "hotkey-fire t1");
	g_assert_true(await_custom(b, "t1", 1000));
	command_ok(b, "hotkey-toggle t1");
	command_error(b, "hotkey-fire nope", "no hotkey");
	/* history and key state */
	tap(b, KEY_A);
	hist = command_json(b, "hotkey-history");
	g_assert_cmpuint(json_array_get_length(json_node_get_array(hist)), >=, 2);
	ks = command_json(b, "hotkey-keystate CapsLock");
	g_assert_false(json_object_get_boolean_member(json_node_get_object(ks),
	                                              "toggled"));
	command_error(b, "hotkey-keystate Frob", "Frob");
	/* the locks */
	command_ok(b, "hotkey-capslock on");
	pump(&b->r, 10);
	gowl_compositor_get_lock_state(b->r.compositor, &caps, NULL);
	g_assert_true(caps);
	command_ok(b, "hotkey-capslock always-off");
	pump(&b->r, 10);
	gowl_compositor_get_lock_state(b->r.compositor, &caps, NULL);
	g_assert_false(caps);
	tap(b, KEY_CAPSLOCK);
	pump(&b->r, 30);
	gowl_compositor_get_lock_state(b->r.compositor, &caps, NULL);
	g_assert_false(caps);
	command_ok(b, "hotkey-capslock normal");
	command_error(b, "hotkey-capslock sideways", "takes on");
	/* contexts from outside, as cmacs sets its Elisp ones */
	add(b, "{key: F12, context: ext, do: {custom: ext}}");
	tap(b, KEY_F12);
	g_assert_true(pressed_in(b, 0, KEY_F12));
	command_ok(b, "hotkey-context-set ext 1");
	tap(b, KEY_F12);
	g_assert_true(await_custom(b, "ext", 1000));
	command_ok(b, "hotkey-context-clear ext");
	command_error(b, "hotkey-context-set ext maybe", "NAME 0|1");
	/* remove, mode, send errors, reload, cancel, problems */
	command_ok(b, "hotkey-remove t1");
	command_error(b, "hotkey-remove t1", "no hotkey");
	command_error(b, "hotkey-mode two words", "one word");
	command_error(b, "hotkey-send {Frob}", "Frob");
	command_ok(b, "hotkey-reload");
	command_ok(b, "hotkey-cancel");
	g_free(command(b, "hotkey-problems"));
	command_error(b, "hotkey-frobnicate", "unknown command");
	st = command_json(b, "hotkey-status");
	g_assert_true(json_object_get_boolean_member(json_node_get_object(st),
	                                             "active"));
}

static void
test_json_rpc(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *sock = g_build_filename(scratch, "hk-rpc.sock", NULL);
	g_autoptr(JsonObject) p = NULL;
	g_autoptr(JsonNode) n = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	BENCH_UP(b);
	/* The server arrives after the module started: the methods follow */
	b->ipc = gowl_ipc_new(sock);
	gowl_compositor_set_ipc(b->r.compositor, b->ipc);
	g_assert_true(gowl_ipc_has_method(b->ipc, "hotkey.list"));
	g_assert_true(gowl_ipc_has_method(b->ipc, "hotkey.set"));

	p = json_object_new();
	json_object_set_string_member(p, "yaml",
	                              "{id: r1, key: F1, do: {custom: r1}}");
	n = gowl_ipc_call(b->ipc, "hotkey.add", p, &error);
	g_assert_no_error(error);
	g_clear_pointer(&n, json_node_unref);
	g_clear_pointer(&p, json_object_unref);

	n = gowl_ipc_call(b->ipc, "hotkey.list", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(n));
	g_clear_pointer(&n, json_node_unref);

	p = json_object_new();
	json_object_set_string_member(p, "id", "r1");
	n = gowl_ipc_call(b->ipc, "hotkey.fire", p, &error);
	g_assert_no_error(error);
	g_assert_true(await_custom(b, "r1", 1000));
	g_clear_pointer(&n, json_node_unref);
	g_clear_pointer(&p, json_object_unref);

	/* typed parameters: a misspelt one is the transport's error */
	p = json_object_new();
	json_object_set_boolean_member(p, "suspnded", TRUE);
	n = gowl_ipc_call(b->ipc, "hotkey.set", p, &error);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_pointer(&p, json_object_unref);

	p = json_object_new();
	json_object_set_boolean_member(p, "suspended", TRUE);
	n = gowl_ipc_call(b->ipc, "hotkey.set", p, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(n),
	                                             "suspended"));
	g_clear_pointer(&n, json_node_unref);
	g_clear_pointer(&p, json_object_unref);

	p = json_object_new();
	json_object_set_string_member(p, "yaml", "{key: Frob}");
	n = gowl_ipc_call(b->ipc, "hotkey.add", p, &error);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_pointer(&p, json_object_unref);

	/* switched off: the methods go with it */
	gowl_module_deactivate(gowl_module_manager_find_module(b->r.modules,
	                                                       "hotkey"));
	g_assert_false(gowl_ipc_has_method(b->ipc, "hotkey.list"));
}

/* Everything under @path, then @path: the run's scratch XDG dirs */
static void
remove_tree(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *name;

	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);

		if (g_file_test(child, G_FILE_TEST_IS_DIR)
		    && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
			remove_tree(child);
		else
			g_unlink(child);
	}
	g_rmdir(path);
}

/* Module settings, as `modules: hotkey:' gives them */
static void
configure(
	Bench       *b,
	const gchar *key,
	const gchar *value
){
	g_autoptr(GHashTable) h = g_hash_table_new(g_str_hash, g_str_equal);
	GowlModule *m = gowl_module_manager_find_module(b->r.modules, "hotkey");

	g_hash_table_insert(h, (gpointer)key, (gpointer)value);
	gowl_module_configure(m, h);
}

static gboolean
func_listed(
	Bench       *b,
	const gchar *name
){
	g_autoptr(JsonNode) fns = command_json(b, "hotkey-functions");
	JsonArray *a = json_node_get_array(fns);
	guint i;

	for (i = 0; i < json_array_get_length(a); i++)
		if (g_strcmp0(json_array_get_string_element(a, i), name) == 0)
			return TRUE;
	return FALSE;
}

/* A crispy hotkey script: built, its init run, what it registered in
 * force; a broken one reported; dropped, its registrations taken back. */
static void
test_scripts(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *broken = g_build_filename(scratch, "broken.c", NULL);
	g_autofree gchar *both = NULL;
	gint64 deadline;

	(void)data;
	BENCH_UP(b);
	configure(b, "scripts", GOWL_TEST_DATA "/example-hotkey-script.c");
	g_assert_true(await_event(b, "script ", 60000));
	g_assert_true(func_listed(b, "stash"));
	g_assert_true(func_listed(b, "script-count"));
	{
		g_autoptr(JsonNode) list = command_json(b, "hotkey-list");
		JsonArray *a = json_node_get_array(list);
		gboolean found = FALSE;
		guint i;

		for (i = 0; i < json_array_get_length(a); i++)
			if (g_strcmp0(json_object_get_string_member(
				    json_array_get_object_element(a, i), "id"),
				    "script-count") == 0)
				found = TRUE;
		g_assert_true(found);
	}
	/* the script's function answers through hotkey-call */
	command_ok(b, "hotkey-call stash");

	/* a script that does not compile: said, nothing else hurt */
	g_assert_true(g_file_set_contents(broken,
		"#include <gowl/gowl.h>\nthis is not C;\n", -1, NULL));
	both = g_strdup_printf("%s\n%s", GOWL_TEST_DATA
	                       "/example-hotkey-script.c", broken);
	g_ptr_array_set_size(b->events, 0);
	configure(b, "scripts", both);
	g_assert_true(await_event(b, "fault script", 60000));
	g_assert_true(func_listed(b, "stash"));

	/* dropped: what it registered goes */
	configure(b, "scripts", "");
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (func_listed(b, "stash") && g_get_monotonic_time() < deadline)
		pump(&b->r, 10);
	g_assert_false(func_listed(b, "stash"));
	g_assert_false(func_listed(b, "script-count"));
	g_unlink(broken);
}

typedef struct {
	GVariant *reply;
	GError   *error;
	gboolean  done;
} DbusCall;

static void
dbus_call_done(
	GObject      *source,
	GAsyncResult *res,
	gpointer      data
){
	DbusCall *c = data;

	c->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res,
	                                         &c->error);
	c->done = TRUE;
}

/* A method of org.gowl.Hotkey1, answered by the compositor's loop --
 * so the test pumps that loop (and its own context, where GDBus
 * delivers the reply) while it waits. */
static gchar *
dbus_call(
	Bench           *b,
	GDBusConnection *conn,
	const gchar     *method,
	GVariant        *params
){
	DbusCall c;
	gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	gchar *text = NULL;

	memset(&c, 0, sizeof c);
	g_dbus_connection_call(conn, "org.gowl.Hotkey1", "/org/gowl/Hotkey1",
	                       "org.gowl.Hotkey1", method, params,
	                       G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
	                       3000, NULL, dbus_call_done, &c);
	while (!c.done && g_get_monotonic_time() < deadline) {
		pump(&b->r, 2);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
	g_assert_true(c.done);
	g_assert_no_error(c.error);
	g_variant_get(c.reply, "(s)", &text);
	g_variant_unref(c.reply);
	return text;
}

static void
on_fired_signal(
	GDBusConnection *conn,
	const gchar     *sender,
	const gchar     *path,
	const gchar     *iface,
	const gchar     *signal,
	GVariant        *params,
	gpointer         data
){
	gchar **last = data;
	const gchar *id = NULL;
	const gchar *k = NULL;

	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)signal;
	g_variant_get(params, "(&s&s)", &id, &k);
	g_free(*last);
	*last = g_strdup(id);
}

/* org.gowl.Hotkey1 on a private session bus */
static void
test_dbus(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(GTestDBus) bus = NULL;
	g_autoptr(GDBusConnection) conn = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *fired = NULL;
	g_autofree gchar *runtime = NULL;
	const gchar *none[] = { NULL };
	guint sub;
	gint64 deadline;

	(void)data;
	BENCH_UP(b);
	/* GTestDBus unsets XDG_RUNTIME_DIR; the compositor already has its
	 * socket, and the rig's teardown needs the variable back */
	runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	configure(b, "dbus", "true");
	conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	g_assert_nonnull(conn);
	sub = g_dbus_connection_signal_subscribe(conn, NULL, "org.gowl.Hotkey1",
	                                         "Fired", "/org/gowl/Hotkey1",
	                                         NULL, G_DBUS_SIGNAL_FLAGS_NONE,
	                                         on_fired_signal, &fired, NULL);

	reply = dbus_call(b, conn, "Add",
	                  g_variant_new("(s)", "{id: d1, key: F1, do: {custom: d1}}"));
	g_assert_cmpstr(reply, ==, "OK added d1");
	g_clear_pointer(&reply, g_free);
	reply = dbus_call(b, conn, "List", NULL);
	g_assert_nonnull(strstr(reply, "\"id\":\"d1\""));
	g_clear_pointer(&reply, g_free);
	reply = dbus_call(b, conn, "Fire", g_variant_new("(s)", "d1"));
	g_assert_true(g_str_has_prefix(reply, "OK"));
	g_assert_true(await_custom(b, "d1", 1000));
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (fired == NULL && g_get_monotonic_time() < deadline) {
		pump(&b->r, 2);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
	g_assert_cmpstr(fired, ==, "d1");
	g_clear_pointer(&reply, g_free);
	reply = dbus_call(b, conn, "SetSuspended", g_variant_new("(b)", TRUE));
	g_assert_cmpstr(reply, ==, "OK suspended");
	g_clear_pointer(&reply, g_free);
	reply = dbus_call(b, conn, "Command", g_variant_new("(s)", "hotkey-status"));
	g_assert_nonnull(strstr(reply, "\"suspended\":true"));
	g_assert_nonnull(strstr(reply, "\"dbus\":true"));
	g_clear_pointer(&reply, g_free);
	reply = dbus_call(b, conn, "Call",
	                  g_variant_new("(s^as)", "nobody", none));
	g_assert_true(g_str_has_prefix(reply, "ERROR"));
	g_clear_pointer(&reply, g_free);
	reply = dbus_call(b, conn, "Remove", g_variant_new("(s)", "d1"));
	g_assert_cmpstr(reply, ==, "OK removed d1");

	g_dbus_connection_signal_unsubscribe(conn, sub);
	configure(b, "dbus", "false");
	g_clear_object(&conn);
	g_test_dbus_down(bus);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	scratch = g_dir_make_tmp("gowl-hotkey-module-XXXXXX", NULL);
	g_setenv("XDG_STATE_HOME", scratch, TRUE);
	g_setenv("XDG_CACHE_HOME", scratch, TRUE);
	g_setenv("XDG_CONFIG_HOME", scratch, TRUE);
	g_setenv("GOWL_MACRO_PATH", scratch, TRUE);

#define T(path, fn) \
	g_test_add("/hotkey/module/" path, Bench, NULL, bench_up, fn, bench_down)
	T("fire-and-swallow", test_fire_and_swallow);
	T("capslock-layer", test_capslock_layer);
	T("remap-to-control", test_remap_to_control);
	T("send", test_send);
	T("send-does-not-refire", test_send_does_not_refire);
	T("context", test_context);
	T("long-press", test_long_press);
	T("modes", test_modes);
	T("suspend-key", test_suspend_key);
	T("shell-and-run", test_shell_and_run);
	T("functions", test_functions);
	T("macro-action", test_macro_action);
	T("c-action", test_c_action);
	T("timers", test_timers);
	T("hooks", test_hooks);
	T("input-hook", test_input_hook);
	T("window-actions", test_window_actions);
	T("mouse-hotkeys", test_mouse_hotkeys);
	T("hotstrings", test_hotstrings);
	T("ipc", test_ipc);
	T("json-rpc", test_json_rpc);
	T("scripts", test_scripts);
	T("dbus", test_dbus);
#undef T

	{
		gint rc = g_test_run();

		remove_tree(scratch);
		g_free(scratch);
		return rc;
	}
}
