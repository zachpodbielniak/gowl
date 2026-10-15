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
 * test-broadcast-delivery.c - Broadcasting, end to end, to Wayland windows.
 *
 * A headless compositor with the real broadcast.so, a test client with
 * three toplevels standing in for three EverQuest clients (app-id
 * eqgame.exe, titles naming the character), a keyboard and a pointer
 * plugged in through the backend -- the hardware path, so the module
 * sees real, non-synthetic input -- and the client recording which
 * surface every key and click reached.
 *
 * The X11 half (Xwayland routes by X focus, not by wl_surface) is
 * tests/test-broadcast-x11.c.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#include "macro-test-client.h"

#define TEAM \
	"{name: eq, slots: [" \
	"{name: tank, match: 'title ~ Tank'}, " \
	"{name: cleric, match: 'title ~ Cleric', focus-key: Super+F2}, " \
	"{name: wizard, match: 'title ~ Wizard'}]}"

static const gchar * const titles[] = {
	"EverQuest - Tank", "EverQuest - Cleric", "EverQuest - Wizard", NULL
};
static const gchar * const app_ids[] = {
	"eqgame.exe", "eqgame.exe", "eqgame.exe", NULL
};

static struct wlr_keyboard_impl fake_keyboard_impl;
static struct wlr_pointer_impl  fake_pointer_impl;

typedef struct {
	Rig                  r;
	struct wlr_keyboard *kb;
	struct wlr_pointer  *ptr;
	gboolean             up;
} Bench;

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

/* broadcast-status, as a JSON object */
static JsonObject *
status(Bench *b)
{
	g_autofree gchar *reply = command(b, "broadcast-status");
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(g_str_has_prefix(reply, "OK "));
	g_assert_true(json_parser_load_from_data(parser, reply + 3, -1, &error));
	g_assert_no_error(error);
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gboolean
is_on(Bench *b)
{
	JsonObject *o = status(b);
	gboolean on = json_object_get_boolean_member(o, "on");

	json_object_unref(o);
	return on;
}

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
	pump(&b->r, 5);
}

static void
tap(
	Bench   *b,
	guint32  code
){
	key(b, code, TRUE);
	key(b, code, FALSE);
}

/* Super+<code> */
static void
super_tap(
	Bench   *b,
	guint32  code
){
	key(b, KEY_LEFTMETA, TRUE);
	tap(b, code);
	key(b, KEY_LEFTMETA, FALSE);
}

static void
button(
	Bench   *b,
	guint32  code,
	gboolean pressed
){
	struct wlr_pointer_button_event ev;

	memset(&ev, 0, sizeof ev);
	ev.pointer = b->ptr;
	ev.time_msec = 1;
	ev.button = code;
	ev.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
	                   : WL_POINTER_BUTTON_STATE_RELEASED;
	wl_signal_emit_mutable(&b->ptr->events.button, &ev);
	pump(&b->r, 5);
}

/* How many press (@state 1) / release (0) events of @code window @w got. */
static gint
count_key(
	Bench   *b,
	gint     w,
	guint32  code,
	guint32  state
){
	gint i;
	gint n = 0;

	g_mutex_lock(&b->r.client.lock);
	for (i = 0; i < b->r.client.n_keys; i++) {
		KeySeen *k = &b->r.client.keys[i];

		if (k->window == w && k->key == code && k->state == state)
			n++;
	}
	g_mutex_unlock(&b->r.client.lock);
	return n;
}

static void
bench_up(
	Bench         *b,
	gconstpointer  data
){
	static const gchar *modules[] = { GOWL_TEST_BROADCAST_MODULE, NULL };

	(void)data;
	memset(b, 0, sizeof *b);
	fake_keyboard_impl.name = "gowl-test-keyboard";
	fake_pointer_impl.name = "gowl-test-pointer";

	/* Three sizes, so a scaled click has something to scale */
	rig_window_size[0][0] = 200;
	rig_window_size[0][1] = 100;
	rig_window_size[1][0] = 400;
	rig_window_size[1][1] = 300;
	rig_window_size[2][0] = 100;
	rig_window_size[2][1] = 50;
	if (!rig_up(&b->r, titles, app_ids, modules))
		return;

	b->kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(b->kb, &fake_keyboard_impl, "test-kb");
	wl_signal_emit_mutable(&b->r.compositor->backend->events.new_input,
	                       &b->kb->base);
	b->ptr = g_new0(struct wlr_pointer, 1);
	wlr_pointer_init(b->ptr, &fake_pointer_impl, "test-ptr");
	wl_signal_emit_mutable(&b->r.compositor->backend->events.new_input,
	                       &b->ptr->base);
	pump(&b->r, 20);

	command_ok(b, "broadcast-add-team " TEAM);
	b->up = TRUE;
}

static void
bench_down(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	if (b->kb != NULL) {
		wlr_keyboard_finish(b->kb);
		g_free(b->kb);
	}
	if (b->ptr != NULL) {
		wlr_pointer_finish(b->ptr);
		g_free(b->ptr);
	}
	rig_down(&b->r);
	memset(rig_window_size, 0, sizeof rig_window_size);
}

#define BENCH_UP(b) do { if (!(b)->up) return; } while (0)

/* --- the primitive --- */

static void
test_deliver_key_seat(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	focus_window(&b->r, 0);
	reset_keys(&b->r);

	/* a press and, separately, its release, in a window without focus */
	g_assert_true(gowl_compositor_deliver_key(b->r.compositor, b->r.win[1],
	              KEY_W, TRUE, -1, GOWL_KEY_DELIVERY_AUTO));
	g_assert_true(gowl_compositor_deliver_key(b->r.compositor, b->r.win[1],
	              KEY_W, FALSE, -1, GOWL_KEY_DELIVERY_AUTO));
	await_keys(&b->r, 2);
	g_assert_cmpint(count_key(b, 1, KEY_W, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_W, 0), ==, 1);
	g_assert_cmpint(count_key(b, 0, KEY_W, 1), ==, 0);
	/* focus came back */
	g_assert_cmpint(focused_now(&b->r), ==, 0);
}

/* --- the module --- */

static void
test_toggle_and_mirror(
	Bench         *b,
	gconstpointer  data
){
	JsonObject *o;

	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-add-rule {name: follow, team: eq, "
	              "keys: ['1-9', w]}");
	o = status(b);
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(o, "teams")), ==, 1);
	json_object_unref(o);

	focus_window(&b->r, 0);
	g_assert_false(is_on(b));
	reset_keys(&b->r);
	tap(b, KEY_1);
	await_keys(&b->r, 2);
	/* off: only the focused window */
	g_assert_cmpint(count_key(b, 0, KEY_1, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_1, 1), ==, 0);

	/* Super+b switches it on, and the key never reaches a window */
	reset_keys(&b->r);
	super_tap(b, KEY_B);
	g_assert_true(is_on(b));
	g_assert_cmpint(count_key(b, 0, KEY_B, 1), ==, 0);

	reset_keys(&b->r);
	tap(b, KEY_1);
	await_keys(&b->r, 6);
	g_assert_cmpint(count_key(b, 0, KEY_1, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_1, 1), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_1, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_1, 0), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_1, 0), ==, 1);
	g_assert_cmpint(focused_now(&b->r), ==, 0);

	/* a key the rule does not name stays local */
	reset_keys(&b->r);
	tap(b, KEY_Q);
	await_keys(&b->r, 2);
	g_assert_cmpint(count_key(b, 0, KEY_Q, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_Q, 1), ==, 0);

	/* injected (synthetic) input is never repeated */
	reset_keys(&b->r);
	gowl_compositor_inject_key(b->r.compositor, KEY_2, TRUE);
	gowl_compositor_inject_key(b->r.compositor, KEY_2, FALSE);
	await_keys(&b->r, 2);
	g_assert_cmpint(count_key(b, 0, KEY_2, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_2, 1), ==, 0);

	/* and off again */
	super_tap(b, KEY_B);
	g_assert_false(is_on(b));
}

static void
test_translate_and_block(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-add-rule {name: heal, team: eq, keys: [h], "
	              "targets: slot:cleric, local: block, "
	              "translate: {cleric: {h: '5'}}}");
	command_ok(b, "broadcast-on");
	focus_window(&b->r, 0);
	reset_keys(&b->r);

	tap(b, KEY_H);
	await_keys(&b->r, 2);
	/* kept from the tank, sent to the cleric as 5 */
	g_assert_cmpint(count_key(b, 0, KEY_H, 1), ==, 0);
	g_assert_cmpint(count_key(b, 1, KEY_5, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_5, 0), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_5, 1), ==, 0);

	/* the release was taken too: the tank saw no stray release */
	g_assert_cmpint(count_key(b, 0, KEY_H, 0), ==, 0);
}

static void
test_held_key_released_on_off(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-add-rule {name: run, team: eq, keys: [w]}");
	command_ok(b, "broadcast-on");
	focus_window(&b->r, 0);
	reset_keys(&b->r);

	key(b, KEY_W, TRUE);
	await_keys(&b->r, 3);
	g_assert_cmpint(count_key(b, 1, KEY_W, 1), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_W, 1), ==, 1);

	/* switching off mid-press lets go everywhere at once */
	command_ok(b, "broadcast-off");
	await_keys(&b->r, 5);
	g_assert_cmpint(count_key(b, 1, KEY_W, 0), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_W, 0), ==, 1);

	/* the physical release reaches the focused window only, once */
	key(b, KEY_W, FALSE);
	await_keys(&b->r, 6);
	g_assert_cmpint(count_key(b, 0, KEY_W, 0), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_W, 0), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_W, 0), ==, 1);
}

static void
test_panic(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-add-rule {name: run, team: eq, keys: [w]}");
	command_ok(b, "broadcast-on");
	focus_window(&b->r, 0);
	reset_keys(&b->r);

	key(b, KEY_W, TRUE);
	await_keys(&b->r, 3);
	/* Super+Shift+b, with W still down */
	key(b, KEY_LEFTMETA, TRUE);
	key(b, KEY_LEFTSHIFT, TRUE);
	tap(b, KEY_B);
	key(b, KEY_LEFTSHIFT, FALSE);
	key(b, KEY_LEFTMETA, FALSE);
	g_assert_false(is_on(b));
	await_keys(&b->r, 5);
	g_assert_cmpint(count_key(b, 1, KEY_W, 0), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_W, 0), ==, 1);
	key(b, KEY_W, FALSE);
}

static void
test_round_robin(
	Bench         *b,
	gconstpointer  data
){
	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-add-rule {name: steps, team: eq, keys: [r], "
	              "mode: round-robin, local: block, targets: all}");
	command_ok(b, "broadcast-on");
	focus_window(&b->r, 0);
	reset_keys(&b->r);

	tap(b, KEY_R);
	tap(b, KEY_R);
	tap(b, KEY_R);
	await_keys(&b->r, 6);
	g_assert_cmpint(count_key(b, 0, KEY_R, 1), ==, 1);
	g_assert_cmpint(count_key(b, 1, KEY_R, 1), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_R, 1), ==, 1);
	/* each release followed its own press */
	g_assert_cmpint(count_key(b, 1, KEY_R, 0), ==, 1);
	g_assert_cmpint(count_key(b, 2, KEY_R, 0), ==, 1);
}

static void
test_focus_key_and_assign(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *reply = NULL;
	g_autofree gchar *assign = NULL;
	JsonObject *o;

	(void)data;
	BENCH_UP(b);
	focus_window(&b->r, 0);

	/* Super+F2 is the cleric's focus key -- broadcasting on or off */
	super_tap(b, KEY_F2);
	g_assert_cmpint(focused_now(&b->r), ==, 1);

	/* by hand: the wizard window as the tank */
	assign = g_strdup_printf("broadcast-assign eq tank %u",
	                         gowl_client_get_id(b->r.win[2]));
	command_ok(b, assign);
	o = status(b);
	{
		JsonObject *team = json_array_get_object_element(
			json_object_get_array_member(o, "teams"), 0);
		JsonObject *slot = json_array_get_object_element(
			json_object_get_array_member(team, "slots"), 0);
		JsonObject *client = json_object_get_object_member(slot, "client");

		g_assert_cmpint(json_object_get_int_member(client, "id"), ==,
		                gowl_client_get_id(b->r.win[2]));
	}
	json_object_unref(o);

	reply = command(b, "broadcast-focus-slot eq bard");
	g_assert_true(g_str_has_prefix(reply, "ERROR"));
}

static void
test_scaled_click(
	Bench         *b,
	gconstpointer  data
){
	struct wlr_box box;
	gint i;
	gint found = 0;

	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-add-rule {name: clicks, team: eq, "
	              "mouse: scaled, keys: [], targets: all-other}");
	command_ok(b, "broadcast-on");
	focus_window(&b->r, 0);

	/* side by side, so the pointer can only be over one of them */
	for (i = 0; i < 3; i++) {
		gint bw = (gint)gowl_client_get_border_width(b->r.win[i]);

		gowl_compositor_resize_client(b->r.compositor, b->r.win[i],
			i * 450, 0, rig_window_size[i][0] + 2 * bw,
			rig_window_size[i][1] + 2 * bw);
	}
	pump(&b->r, 30);

	/* the middle of the tank's surface */
	gowl_client_get_geometry(b->r.win[0], &box.x, &box.y, &box.width,
	                         &box.height);
	gowl_compositor_inject_pointer_warp(b->r.compositor,
		(gdouble)box.x + (gdouble)gowl_client_get_border_width(b->r.win[0])
		+ 100.0,
		(gdouble)box.y + (gdouble)gowl_client_get_border_width(b->r.win[0])
		+ 50.0);
	pump(&b->r, 30);
	g_mutex_lock(&b->r.client.lock);
	b->r.client.n_buttons = 0;
	g_mutex_unlock(&b->r.client.lock);

	button(b, BTN_LEFT, TRUE);
	button(b, BTN_LEFT, FALSE);
	pump(&b->r, 60);

	/* each other window: the same relative point, a press and release */
	g_mutex_lock(&b->r.client.lock);
	for (i = 0; i < b->r.client.n_buttons; i++) {
		ButtonSeen *s = &b->r.client.buttons[i];

		if (s->window == 1 && s->state == 1) {
			g_assert_cmpfloat_with_epsilon(s->x, 200.0, 1.0);
			g_assert_cmpfloat_with_epsilon(s->y, 150.0, 1.0);
			found++;
		} else if (s->window == 2 && s->state == 1) {
			g_assert_cmpfloat_with_epsilon(s->x, 50.0, 1.0);
			g_assert_cmpfloat_with_epsilon(s->y, 25.0, 1.0);
			found++;
		}
	}
	g_mutex_unlock(&b->r.client.lock);
	g_assert_cmpint(found, ==, 2);
}

static void
test_strict_refuses(
	Bench         *b,
	gconstpointer  data
){
	g_autofree gchar *reply = NULL;

	(void)data;
	BENCH_UP(b);
	command_ok(b, "broadcast-strict on");
	reply = command(b, "broadcast-add-rule {name: follow, team: eq}");
	g_assert_nonnull(strstr(reply, "cannot run"));
	command_ok(b, "broadcast-on");
	focus_window(&b->r, 0);
	reset_keys(&b->r);
	tap(b, KEY_1);
	await_keys(&b->r, 2);
	g_assert_cmpint(count_key(b, 1, KEY_1, 1), ==, 0);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, fn) \
	g_test_add("/broadcast/delivery/" path, Bench, NULL, bench_up, fn, \
	           bench_down)
	ADD("deliver-key-seat", test_deliver_key_seat);
	ADD("toggle-and-mirror", test_toggle_and_mirror);
	ADD("translate-and-block", test_translate_and_block);
	ADD("held-key-released-on-off", test_held_key_released_on_off);
	ADD("panic", test_panic);
	ADD("round-robin", test_round_robin);
	ADD("focus-key-and-assign", test_focus_key_and_assign);
	ADD("scaled-click", test_scaled_click);
	ADD("strict-refuses", test_strict_refuses);
#undef ADD

	return g_test_run();
}
