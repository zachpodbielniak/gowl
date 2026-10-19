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
 * test-gabbrev-module.c - Text expansion, end to end.
 *
 * A headless compositor with the real gabbrev.so, a keyboard plugged in
 * through the backend (the hardware path: the module sees the keys the
 * way it sees a real keyboard's), and a Wayland client recording every
 * key that reaches its windows.  Typing `;xx' must arrive at the client
 * as `;', `x', `x', three Backspaces and the replacement -- that is the
 * whole feature, from the client's side.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#include "macro-test-client.h"

static const gchar * const titles[] = {
	"Editor", "Enter your password", NULL
};
static const gchar * const app_ids[] = {
	"org.example.Editor", "org.example.Prompt", NULL
};

static struct wlr_keyboard_impl fake_keyboard_impl;

typedef struct {
	Rig                  r;
	struct wlr_keyboard *kb;
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

static JsonObject *
command_json(
	Bench       *b,
	const gchar *line
){
	g_autofree gchar *reply = command(b, line);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(g_str_has_prefix(reply, "OK "));
	g_assert_true(json_parser_load_from_data(parser, reply + 3, -1, &error));
	g_assert_no_error(error);
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
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

/* Types ASCII lowercase, digits, `;', space, `,' and `.' */
static void
type(
	Bench       *b,
	const gchar *text
){
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		guint32 code = 0;

		if (*p >= 'a' && *p <= 'z') {
			static const guint32 letters[] = {
				KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
				KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P,
				KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X,
				KEY_Y, KEY_Z
			};

			code = letters[*p - 'a'];
		} else if (*p == ';') {
			code = KEY_SEMICOLON;
		} else if (*p == ' ') {
			code = KEY_SPACE;
		} else if (*p == ',') {
			code = KEY_COMMA;
		} else if (*p == '.') {
			code = KEY_DOT;
		} else {
			g_assert_not_reached();
		}
		tap(b, code);
	}
}

/* Every key PRESS window @w saw since reset_keys(), in order */
static GArray *
presses(
	Bench *b,
	gint   w
){
	GArray *out = g_array_new(FALSE, FALSE, sizeof(KeySeen));
	gint i;

	g_mutex_lock(&b->r.client.lock);
	for (i = 0; i < b->r.client.n_keys; i++) {
		KeySeen *k = &b->r.client.keys[i];

		if (k->window == w && k->state == 1)
			g_array_append_val(out, *k);
	}
	g_mutex_unlock(&b->r.client.lock);
	return out;
}

/* The presses after the user's own typing (the first @skip ones) */
static void
assert_typed_within(
	Bench          *b,
	guint           skip,
	const guint32  *expect,
	guint           n,
	guint           seconds
){
	g_autoptr(GArray) p = NULL;
	gint64 deadline = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;
	guint i;

	do {
		pump(&b->r, 10);
		g_clear_pointer(&p, g_array_unref);
		p = presses(b, 0);
	} while (p->len < skip + n && g_get_monotonic_time() < deadline);
	pump(&b->r, 40);
	g_clear_pointer(&p, g_array_unref);
	p = presses(b, 0);

	for (i = 0; i < p->len; i++)
		g_test_message("press %u: key %u mods 0x%x", i,
		               g_array_index(p, KeySeen, i).key,
		               g_array_index(p, KeySeen, i).mods);
	g_assert_cmpuint(p->len, ==, skip + n);
	for (i = 0; i < n; i++)
		g_assert_cmpuint(g_array_index(p, KeySeen, skip + i).key, ==, expect[i]);
}

static void
assert_typed(
	Bench          *b,
	guint           skip,
	const guint32  *expect,
	guint           n
){
	assert_typed_within(b, skip, expect, n, 2);
}

static void
bench_up(
	Bench         *b,
	gconstpointer  data
){
	static const gchar *modules[] = { GOWL_TEST_GABBREV_MODULE, NULL };

	(void)data;
	memset(b, 0, sizeof *b);
	fake_keyboard_impl.name = "gowl-test-keyboard";
	if (!rig_up(&b->r, titles, app_ids, modules))
		return;
	b->kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(b->kb, &fake_keyboard_impl, "test-kb");
	wl_signal_emit_mutable(&b->r.compositor->backend->events.new_input,
	                       &b->kb->base);
	pump(&b->r, 20);
	/* No notifications: there is no bar to show them */
	command_ok(b, "gabbrev-add --name test {matches: ["
	           "{trigger: ';xx', replace: hi},"
	           "{trigger: 'brb', replace: 'be right back', word: true},"
	           "{trigger: ';cur', replace: 'a$|$b'},"
	           "{trigger: ';arrow', replace: '\xe2\x86\x92'},"
	           "{trigger: ';sh', replace: '{{o}}', vars: "
	           "[{name: o, type: shell, params: {cmd: 'echo ok'}}]},"
	           "{trigger: ';late', replace: '{{v}}', vars: "
	           "[{name: v, type: later}]},"
	           "{trigger: ';bad', replace: '{{v}}', vars: "
	           "[{name: v, type: shell, params: {cmd: 'exit 3'}}]},"
	           "{trigger: ';fn', replace: '{{v}}', vars: "
	           "[{name: v, type: c, params: {func: answer}}]},"
	           "{trigger: ';cc', replace: '{{v}}', vars: "
	           "[{name: v, type: c, params: {code: "
	           "'return g_strdup_printf(\"%d\", 6 * 7);'}}]}"
	           "]}");
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
	if (b->kb != NULL) {
		wlr_keyboard_finish(b->kb);
		g_free(b->kb);
	}
	rig_down(&b->r);
}

#define BENCH_UP(b) do { if (!(b)->up) return; } while (0)

/* ;xx -> hi: the three typed characters, three Backspaces, h, i */
static void
test_expand(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_H, KEY_I
	};

	(void)data;
	BENCH_UP(b);
	type(b, "a ;xx");
	assert_typed(b, 5, want, G_N_ELEMENTS(want));
}

/* A word trigger fires on the separator and types it back */
static void
test_word(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE,
		KEY_B, KEY_E, KEY_SPACE, KEY_R, KEY_I, KEY_G, KEY_H, KEY_T,
		KEY_SPACE, KEY_B, KEY_A, KEY_C, KEY_K, KEY_COMMA
	};

	(void)data;
	BENCH_UP(b);
	type(b, "abrb brb,");
	assert_typed(b, 9, want, G_N_ELEMENTS(want));
}

/* Backspace straight after an expansion puts the trigger back */
static void
test_undo(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_H, KEY_I,
		/* the user's Backspace is taken; two for `hi', then `;xx' */
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_SEMICOLON, KEY_X, KEY_X
	};

	(void)data;
	BENCH_UP(b);
	type(b, ";xx");
	pump(&b->r, 60);
	tap(b, KEY_BACKSPACE);
	assert_typed(b, 3, want, G_N_ELEMENTS(want));
}

/* The cursor hint: the text, then Left back to where the hint was */
static void
test_cursor_hint(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE,
		KEY_A, KEY_B, KEY_LEFT
	};

	(void)data;
	BENCH_UP(b);
	type(b, ";cur");
	assert_typed(b, 4, want, G_N_ELEMENTS(want));
}

/* A character no key types goes by the clipboard: Ctrl+V, with the
   text on the clipboard */
static void
test_paste(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE,
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_V
	};
	g_autoptr(GArray) p = NULL;
	g_autofree gchar *clip = NULL;

	(void)data;
	BENCH_UP(b);
	type(b, ";arrow");
	assert_typed(b, 6, want, G_N_ELEMENTS(want));
	p = presses(b, 0);
	/* Ctrl held for the V (the depressed mask has a modifier in it) */
	g_assert_cmpuint(g_array_index(p, KeySeen, p->len - 1).mods, !=, 0);
	clip = gowl_seat_get_clipboard(gowl_compositor_get_seat(b->r.compositor));
	g_assert_cmpstr(clip, ==, "\xe2\x86\x92");
}

/* A shell variable: the expansion waits for the command, on the loop */
static void
test_shell(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_O, KEY_K
	};

	(void)data;
	BENCH_UP(b);
	if (g_find_program_in_path("bash") == NULL) {
		g_test_skip("no bash");
		return;
	}
	type(b, ";sh");
	assert_typed(b, 3, want, G_N_ELEMENTS(want));
}

/* A failing variable types nothing and leaves the trigger alone */
static void
test_failure(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(JsonObject) st = NULL;
	g_autoptr(GArray) p = NULL;

	(void)data;
	BENCH_UP(b);
	type(b, ";bad");
	pump(&b->r, 300);
	p = presses(b, 0);
	g_assert_cmpuint(p->len, ==, 4);
	st = command_json(b, "gabbrev-status");
	g_assert_cmpint(json_object_get_int_member(st, "failed"), ==, 1);
	g_assert_nonnull(strstr(json_object_get_string_member(st, "last-error"),
	                        "shell failed"));
}

/* A type registered through libgowl, answered from another thread */
static gpointer
later_thread(gpointer data)
{
	GowlGabbrevRequest *req = data;

	g_usleep(20000);
	gowl_gabbrev_request_return(req, "zz");
	gowl_gabbrev_request_unref(req);
	return NULL;
}

static void
later_type(
	GowlGabbrevRequest *req,
	gpointer            data
){
	(void)data;
	g_thread_unref(g_thread_new("later", later_thread,
	                            gowl_gabbrev_request_ref(req)));
}

static void
test_registered_type(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE,
		KEY_BACKSPACE, KEY_Z, KEY_Z
	};

	(void)data;
	BENCH_UP(b);
	gowl_gabbrev_register_var_type("later", later_type, NULL, NULL);
	type(b, ";late");
	assert_typed(b, 5, want, G_N_ELEMENTS(want));
	gowl_gabbrev_unregister_var_type("later");
}

/* `c' variables: a function registered from C (a C config's way), and
   code in the YAML, compiled by crispy on a worker the first time */
static gchar *
answer_func(
	GowlGabbrevRequest *req,
	gpointer            data
){
	(void)req;
	(void)data;
	return g_strdup("yo");
}

static void
test_c_variables(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want_fn[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_Y, KEY_O
	};
	static const guint32 want_cc[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_4, KEY_2
	};
	g_autofree gchar *gcc = g_find_program_in_path("gcc");

	(void)data;
	BENCH_UP(b);
	gowl_gabbrev_register_func("answer", answer_func, NULL, NULL);
	type(b, ";fn");
	assert_typed(b, 3, want_fn, G_N_ELEMENTS(want_fn));
	gowl_gabbrev_unregister_func("answer");

	if (gcc == NULL) {
		g_test_skip("no gcc to build a c variable");
		return;
	}
	reset_keys(&b->r);
	type(b, ";cc");
	assert_typed_within(b, 3, want_cc, G_N_ELEMENTS(want_cc), 60);
}

/* Never into a password prompt; never while switched off */
static void
test_deny_and_off(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(GArray) p = NULL;
	g_autoptr(JsonObject) t = NULL;

	(void)data;
	BENCH_UP(b);
	focus_window(&b->r, 1);
	reset_keys(&b->r);
	type(b, ";xx");
	pump(&b->r, 100);
	p = presses(b, 1);
	g_assert_cmpuint(p->len, ==, 3);
	t = command_json(b, "gabbrev-test ;xx");
	g_assert_true(json_object_get_boolean_member(t, "denied"));

	focus_window(&b->r, 0);
	command_ok(b, "gabbrev-off");
	reset_keys(&b->r);
	type(b, ";xx");
	pump(&b->r, 100);
	g_clear_pointer(&p, g_array_unref);
	p = presses(b, 0);
	g_assert_cmpuint(p->len, ==, 3);
	command_ok(b, "gabbrev-on");
}

/* The IPC surface: list, test, insert, a runtime source replaced */
static void
test_ipc(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = { KEY_O, KEY_K };
	g_autoptr(JsonObject) t = NULL;
	g_autoptr(JsonObject) st = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *bad = NULL;

	(void)data;
	BENCH_UP(b);
	t = command_json(b, "gabbrev-test say ;today please");
	g_assert_cmpstr(json_object_get_string_member(t, "match"), ==, ";today");
	g_assert_cmpstr(json_object_get_string_member(t, "rest"), ==, " please");
	list = command(b, "gabbrev-list shrug");
	g_assert_nonnull(strstr(list, ";shrug"));
	st = command_json(b, "gabbrev-status");
	g_assert_true(json_object_get_boolean_member(st, "on"));
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(st, "problems")), ==, 0);
	bad = command(b, "gabbrev-add {matches: [");
	g_assert_true(g_str_has_prefix(bad, "ERROR"));

	command_ok(b, "gabbrev-insert ok");
	assert_typed(b, 0, want, G_N_ELEMENTS(want));

	/* the same name replaces the source: ;xx is gone */
	command_ok(b, "gabbrev-add --name test {matches: [{trigger: ';yy', "
	           "replace: y}]}");
	g_clear_pointer(&t, json_object_unref);
	t = command_json(b, "gabbrev-test ;xx");
	g_assert_true(json_object_get_null_member(t, "match"));
	command_ok(b, "gabbrev-remove test");
}

#define ADD(name, fn) \
	g_test_add(name, Bench, NULL, bench_up, fn, bench_down)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	ADD("/gabbrev/module/expand", test_expand);
	ADD("/gabbrev/module/word", test_word);
	ADD("/gabbrev/module/undo", test_undo);
	ADD("/gabbrev/module/cursor-hint", test_cursor_hint);
	ADD("/gabbrev/module/paste", test_paste);
	ADD("/gabbrev/module/shell", test_shell);
	ADD("/gabbrev/module/failure", test_failure);
	ADD("/gabbrev/module/registered-type", test_registered_type);
	ADD("/gabbrev/module/c-variables", test_c_variables);
	ADD("/gabbrev/module/deny-and-off", test_deny_and_off);
	ADD("/gabbrev/module/ipc", test_ipc);
	return g_test_run();
}
