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
 *
 * The client also has a text-input-v3 text field that is always
 * focused, so the commit backend is seen the way an application sees
 * it; and the clipboard history module is loaded, to prove a paste
 * expansion never reaches it.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#define RIG_TEXT_INPUT
#include "macro-test-client.h"

/* $XDG_STATE_HOME and $XDG_CONFIG_HOME for the whole run: usage counts
   and accepted suggestions are written there, never to the real ones */
static gchar *xdg_scratch;

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
	GPtrArray           *events;   /* gchar*, the module's EVENT lines */
	gulong               event_handler;
} Bench;

static void
on_module_event(
	GowlCompositor *compositor,
	const gchar    *module,
	const gchar    *text,
	gpointer        data
){
	Bench *b = data;

	(void)compositor;
	if (g_strcmp0(module, "gabbrev") == 0)
		g_ptr_array_add(b->events, g_strdup(text));
}

/* Was an event starting with @prefix announced? */
static gboolean
saw_event(
	Bench       *b,
	const gchar *prefix
){
	guint i;

	for (i = 0; i < b->events->len; i++)
		if (g_str_has_prefix(g_ptr_array_index(b->events, i), prefix))
			return TRUE;
	return FALSE;
}

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

/* A JSON array reply */
static JsonArray *
command_array(
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
	return json_array_ref(json_node_get_array(json_parser_get_root(parser)));
}

/* The element of @arr whose string member @key is @value, or NULL */
static JsonObject *
find_in(
	JsonArray   *arr,
	const gchar *key,
	const gchar *value
){
	guint i;

	for (i = 0; i < json_array_get_length(arr); i++) {
		JsonObject *o = json_array_get_object_element(arr, i);

		if (json_object_has_member(o, key)
		    && !json_object_get_null_member(o, key)
		    && g_strcmp0(json_object_get_string_member(o, key), value) == 0)
			return o;
	}
	return NULL;
}

/* The compositor's loop and GLib's default context, which the clipboard
   history module reads on */
static void
pump_both(
	Bench *b,
	gint   ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		pump(&b->r, 2);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
}

/* Module settings, as a config's `modules: gabbrev:' block gives them */
static void
configure(
	Bench       *b,
	const gchar *key,
	const gchar *value
){
	g_autoptr(GHashTable) h = g_hash_table_new(g_str_hash, g_str_equal);
	GowlModule *m = gowl_module_manager_find_module(b->r.modules, "gabbrev");

	g_assert_nonnull(m);
	g_hash_table_insert(h, (gpointer)key, (gpointer)value);
	gowl_module_configure(m, h);
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

/* Nothing counted or accepted by an earlier test */
static void
forget_state(void)
{
	g_autofree gchar *stats = g_build_filename(xdg_scratch, "gowl",
	                                           "gabbrev-stats.tsv", NULL);
	g_autofree gchar *learned = g_build_filename(xdg_scratch, "gowl",
	                                             "gabbrev-learned.yml", NULL);

	g_unlink(stats);
	g_unlink(learned);
}

static void
bench_up(
	Bench         *b,
	gconstpointer  data
){
	static const gchar *modules[] = {
		GOWL_TEST_GABBREV_MODULE, GOWL_TEST_CLIPBOARD_MODULE, NULL
	};

	(void)data;
	memset(b, 0, sizeof *b);
	forget_state();
	b->events = g_ptr_array_new_with_free_func(g_free);
	fake_keyboard_impl.name = "gowl-test-keyboard";
	if (!rig_up(&b->r, titles, app_ids, modules))
		return;
	b->kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(b->kb, &fake_keyboard_impl, "test-kb");
	wl_signal_emit_mutable(&b->r.compositor->backend->events.new_input,
	                       &b->kb->base);
	pump(&b->r, 20);
	b->event_handler = g_signal_connect(b->r.compositor, "module-event",
	                                    G_CALLBACK(on_module_event), b);
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
	if (b->event_handler != 0 && b->r.compositor != NULL)
		g_signal_handler_disconnect(b->r.compositor, b->event_handler);
	rig_down(&b->r);
	g_clear_pointer(&b->events, g_ptr_array_unref);
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

/* The expand key: `brb' typed with no separator yet expands at once */
static void
test_expand_key(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE,
		KEY_B, KEY_E, KEY_SPACE, KEY_R, KEY_I, KEY_G, KEY_H, KEY_T,
		KEY_SPACE, KEY_B, KEY_A, KEY_C, KEY_K
	};
	g_autofree gchar *reply = NULL;

	(void)data;
	BENCH_UP(b);
	type(b, "ok brb");
	reply = command(b, "gabbrev-search");
	g_assert_true(g_str_has_prefix(reply, "OK expanded"));
	assert_typed(b, 6, want, G_N_ELEMENTS(want));
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
	g_autofree gchar *bash = g_find_program_in_path("bash");

	(void)data;
	BENCH_UP(b);
	if (bash == NULL) {
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

/* --- calling into gowl macros --- */

static gboolean
macro_answer(
	GowlMacroContext *ctx,
	gpointer          data
){
	(void)data;
	gowl_macro_set_result(ctx, "42");
	return TRUE;
}

/* A timeline macro: a step queued, so it finishes after macro-run
   replied; its result must still reach the variable */
static gboolean
macro_late(
	GowlMacroContext *ctx,
	gpointer          data
){
	(void)data;
	gowl_macro_wait(ctx, 60);
	gowl_macro_set_result(ctx, "late");
	return TRUE;
}

static gboolean
macro_args(
	GowlMacroContext *ctx,
	gpointer          data
){
	g_autofree gchar *joined = NULL;
	GString *s = g_string_new(NULL);
	guint i;

	(void)data;
	for (i = 0; i < gowl_macro_get_argc(ctx); i++)
		g_string_append(s, gowl_macro_get_arg(ctx, i));
	joined = g_string_free(s, FALSE);
	gowl_macro_set_result(ctx, joined);
	return TRUE;
}

static void
bench_up_macro(
	Bench         *b,
	gconstpointer  data
){
	static const gchar *modules[] = {
		GOWL_TEST_GABBREV_MODULE, GOWL_TEST_MACRO_MODULE, NULL
	};

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
	gowl_macro_register_func("gab-answer", macro_answer, NULL, NULL);
	gowl_macro_register_func("gab-late", macro_late, NULL, NULL);
	gowl_macro_register_func("gab-args", macro_args, NULL, NULL);
	command_ok(b, "gabbrev-add --name test {matches: ["
	           "{trigger: ';ma', replace: '{{m}}', vars: "
	           "[{name: m, type: macro, params: {name: gab-answer}}]},"
	           "{trigger: ';ml', replace: '{{m}}', vars: "
	           "[{name: m, type: macro, params: {name: gab-late}}]},"
	           "{trigger: ';mg', replace: '{{m}}', vars: "
	           "[{name: x, type: echo, params: {echo: b}},"
	           " {name: m, type: macro, params: {name: gab-args, "
	           "args: [a, '{{x}}']}}]},"
	           "{trigger: ';mx', replace: '{{m}}', vars: "
	           "[{name: m, type: macro, params: {name: no-such-macro}}]}"
	           "]}");
	focus_window(&b->r, 0);
	reset_keys(&b->r);
	b->up = TRUE;
}

static void
bench_down_macro(
	Bench         *b,
	gconstpointer  data
){
	gowl_macro_unregister_func("gab-answer");
	gowl_macro_unregister_func("gab-late");
	gowl_macro_unregister_func("gab-args");
	bench_down(b, data);
}

static void
test_macro_variables(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want_now[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_4, KEY_2
	};
	static const guint32 want_late[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_L, KEY_A, KEY_T, KEY_E
	};
	static const guint32 want_args[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_A, KEY_B
	};
	g_autoptr(GArray) p = NULL;
	g_autoptr(JsonObject) st = NULL;

	(void)data;
	BENCH_UP(b);
	/* a result given at once */
	type(b, ";ma");
	assert_typed(b, 3, want_now, G_N_ELEMENTS(want_now));
	/* a result given after a queued step: the variable waits */
	reset_keys(&b->r);
	type(b, ";ml");
	assert_typed(b, 3, want_late, G_N_ELEMENTS(want_late));
	/* arguments, substituted */
	reset_keys(&b->r);
	type(b, ";mg");
	assert_typed(b, 3, want_args, G_N_ELEMENTS(want_args));
	/* no such macro: nothing typed, a reason */
	reset_keys(&b->r);
	type(b, ";mx");
	pump(&b->r, 200);
	p = presses(b, 0);
	g_assert_cmpuint(p->len, ==, 3);
	st = command_json(b, "gabbrev-status");
	g_assert_nonnull(strstr(json_object_get_string_member(st, "last-error"),
	                        "no-such-macro"));
}


/* Counted uses: the list's and the stats' numbers, never-used matches,
   and the file the counts are kept in */
static void
test_stats(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want_hi[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_H, KEY_I
	};
	g_autoptr(JsonArray) list = NULL;
	g_autoptr(JsonArray) stats = NULL;
	g_autoptr(JsonArray) unused = NULL;
	g_autoptr(JsonObject) st = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *o;

	(void)data;
	BENCH_UP(b);
	type(b, ";xx");
	assert_typed(b, 3, want_hi, G_N_ELEMENTS(want_hi));
	reset_keys(&b->r);
	type(b, ";xx");
	assert_typed(b, 3, want_hi, G_N_ELEMENTS(want_hi));
	reset_keys(&b->r);
	type(b, ";cur");
	pump(&b->r, 200);

	list = command_array(b, "gabbrev-list");
	o = find_in(list, "id", ";xx");
	g_assert_nonnull(o);
	g_assert_cmpint(json_object_get_int_member(o, "uses"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(o, "last-used"), >, 0);
	o = find_in(list, "id", ";sh");
	g_assert_cmpint(json_object_get_int_member(o, "uses"), ==, 0);
	g_assert_true(json_object_get_null_member(o, "last-used"));

	/* Most used first */
	stats = command_array(b, "gabbrev-stats");
	o = json_array_get_object_element(stats, 0);
	g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, ";xx");
	g_assert_cmpint(json_object_get_int_member(o, "uses"), ==, 2);
	g_assert_true(json_object_get_boolean_member(o, "loaded"));
	g_assert_nonnull(find_in(stats, "id", ";cur"));

	/* What has never fired is what can go */
	unused = command_array(b, "gabbrev-stats --unused");
	g_assert_nonnull(find_in(unused, "id", ";sh"));
	g_assert_null(find_in(unused, "id", ";xx"));

	st = command_json(b, "gabbrev-status");
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(st, "stats"), "tracked"), ==, 2);

	/* A reset writes at once: the counts are on disk */
	command_ok(b, "gabbrev-stats-reset ;cur");
	path = g_build_filename(xdg_scratch, "gowl", "gabbrev-stats.tsv", NULL);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_true(g_str_has_prefix(text, "#"));
	g_assert_nonnull(strstr(text, "\n2\t"));
	g_assert_nonnull(strstr(text, "\t;xx\n"));
	g_assert_null(strstr(text, ";cur"));
}

/* A paste expansion is on the clipboard only to be pasted: marked, so
   the history never keeps it -- and the user's own clipboard comes
   back without being added a second time */
static void
test_transient_clipboard(
	Bench         *b,
	gconstpointer  data
){
	GowlSeat *seat;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *clip = NULL;
	gint64 deadline;
	const gchar *p;
	guint n;

	(void)data;
	BENCH_UP(b);
	seat = gowl_compositor_get_seat(b->r.compositor);
	gowl_seat_set_clipboard(seat, "keep me");
	g_assert_false(gowl_seat_clipboard_is_transient(seat));
	deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	do {
		g_free(before);
		pump_both(b, 20);
		before = command(b, "clipboard-list");
	} while (strstr(before, "keep me") == NULL
	         && g_get_monotonic_time() < deadline);
	g_assert_nonnull(strstr(before, "keep me"));

	type(b, ";arrow");
	pump_both(b, 800);
	clip = gowl_seat_get_clipboard(seat);
	g_assert_cmpstr(clip, ==, "keep me");
	g_assert_true(gowl_seat_clipboard_is_transient(seat));

	after = command(b, "clipboard-list");
	g_assert_null(strstr(after, "\xe2\x86\x92"));
	for (n = 0, p = after; (p = strstr(p, "keep me")) != NULL; p++)
		n++;
	g_assert_cmpuint(n, ==, 1);
}

/* Write @text to @path, atomically like an editor's save */
static void
write_file(
	const gchar *path,
	const gchar *text
){
	g_autoptr(GError) error = NULL;

	g_assert_true(g_file_set_contents(path, text, -1, &error));
	g_assert_no_error(error);
}

/* Wait until gabbrev-list has a match @id whose replace is @replace */
static gboolean
await_match(
	Bench       *b,
	const gchar *id,
	const gchar *replace
){
	gint64 deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		g_autoptr(JsonArray) list = NULL;
		JsonObject *o;

		pump(&b->r, 50);
		list = command_array(b, "gabbrev-list");
		o = find_in(list, "id", id);
		if (o != NULL && g_strcmp0(json_object_get_string_member(o, "replace"),
		                           replace) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Editing a loaded file, or adding one to a loaded directory, takes
   effect without a reload command */
static void
test_watch(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want_one[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_O, KEY_N, KEY_E
	};
	static const guint32 want_two[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_T, KEY_W, KEY_O
	};
	g_autofree gchar *dir = g_build_filename(xdg_scratch, "watched", NULL);
	g_autofree gchar *a = g_build_filename(dir, "a.yml", NULL);
	g_autofree gchar *nb = g_build_filename(dir, "b.yml", NULL);
	g_autofree gchar *line = NULL;
	g_autoptr(JsonObject) st = NULL;
	JsonArray *watching;
	guint i;
	gboolean seen = FALSE;

	(void)data;
	BENCH_UP(b);
	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	write_file(a, "matches:\n  - trigger: ';wa'\n    replace: one\n");
	line = g_strdup_printf("gabbrev-load --name watched %s", dir);
	command_ok(b, line);
	type(b, ";wa");
	assert_typed(b, 3, want_one, G_N_ELEMENTS(want_one));

	st = command_json(b, "gabbrev-status");
	watching = json_object_get_array_member(st, "watching");
	for (i = 0; i < json_array_get_length(watching); i++)
		if (g_strcmp0(json_array_get_string_element(watching, i), dir) == 0)
			seen = TRUE;
	g_assert_true(seen);

	/* Edited in place */
	write_file(a, "matches:\n  - trigger: ';wa'\n    replace: two\n");
	g_assert_true(await_match(b, ";wa", "two"));
	reset_keys(&b->r);
	type(b, ";wa");
	assert_typed(b, 3, want_two, G_N_ELEMENTS(want_two));

	/* A new file in the directory */
	write_file(nb, "matches:\n  - trigger: ';wb'\n    replace: new\n");
	g_assert_true(await_match(b, ";wb", "new"));
	g_assert_true(saw_event(b, "reloaded "));

	/* An editor's backup file is not a source change */
	g_ptr_array_set_size(b->events, 0);
	{
		g_autofree gchar *swap = g_build_filename(dir, ".a.yml.swp", NULL);

		write_file(swap, "junk");
		pump(&b->r, 700);
		g_assert_false(saw_event(b, "reloaded "));
		g_unlink(swap);
	}
	g_unlink(a);
	g_unlink(nb);
	g_rmdir(dir);
}

/* Learning: a phrase typed three times is suggested once, with a
   trigger; a denied window teaches nothing; accepting it makes a match
   that expands, kept in the learned file */
static void
test_learn(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE,
		KEY_S, KEY_E, KEY_E, KEY_SPACE, KEY_Y, KEY_O, KEY_U, KEY_SPACE,
		KEY_S, KEY_O, KEY_O, KEY_N
	};
	g_autoptr(JsonArray) sug = NULL;
	g_autoptr(JsonArray) after = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *o;
	guint i;

	(void)data;
	BENCH_UP(b);
	/* Off by default: nothing is counted */
	type(b, "see you soon. see you soon. see you soon. ");
	sug = command_array(b, "gabbrev-suggestions");
	g_assert_cmpuint(json_array_get_length(sug), ==, 0);
	g_clear_pointer(&sug, json_array_unref);

	command_ok(b, "gabbrev-learn on");
	/* The password prompt is denied: typing there teaches nothing */
	focus_window(&b->r, 1);
	for (i = 0; i < 4; i++)
		type(b, "my secret phrase. ");
	focus_window(&b->r, 0);
	for (i = 0; i < 3; i++)
		type(b, "see you soon. ");
	g_assert_true(saw_event(b, "suggestion ;sys \"see you soon\""));

	sug = command_array(b, "gabbrev-suggestions");
	g_assert_cmpuint(json_array_get_length(sug), ==, 1);
	o = json_array_get_object_element(sug, 0);
	g_assert_cmpstr(json_object_get_string_member(o, "phrase"), ==,
	                "see you soon");
	g_assert_cmpstr(json_object_get_string_member(o, "trigger"), ==, ";sys");
	g_assert_cmpint(json_object_get_int_member(o, "count"), ==, 3);

	/* A trigger that collides is refused */
	reply = command(b, "gabbrev-accept --trigger ;x see you soon");
	g_assert_true(g_str_has_prefix(reply, "ERROR"));
	command_ok(b, "gabbrev-accept see you soon");
	path = g_build_filename(xdg_scratch, "gowl", "gabbrev-learned.yml", NULL);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_nonnull(strstr(text, "trigger: \";sys\""));
	g_assert_nonnull(strstr(text, "replace: \"see you soon\""));

	/* It is a match now, and no longer a suggestion */
	after = command_array(b, "gabbrev-suggestions");
	g_assert_cmpuint(json_array_get_length(after), ==, 0);
	reset_keys(&b->r);
	type(b, ";sys");
	assert_typed(b, 4, want, G_N_ELEMENTS(want));

	command_ok(b, "gabbrev-learn clear");
	command_ok(b, "gabbrev-learn off");
}

/* A slow expansion is announced, listed, and cancelled by its id */
static void
test_pending_cancel(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(JsonObject) st = NULL;
	g_autoptr(JsonObject) st2 = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *bash = g_find_program_in_path("bash");
	g_autoptr(GArray) p = NULL;
	JsonArray *jobs;
	gint64 id;

	(void)data;
	BENCH_UP(b);
	if (bash == NULL) {
		g_test_skip("no bash");
		return;
	}
	configure(b, "pending-toast-ms", "100");
	command_ok(b, "gabbrev-add --name slow {matches: [{trigger: ';sl', "
	           "replace: '{{v}}', vars: [{name: v, type: shell, "
	           "params: {cmd: 'sleep 2; echo late'}}]}]}");
	type(b, ";sl");
	pump(&b->r, 400);
	g_assert_true(saw_event(b, "pending "));

	st = command_json(b, "gabbrev-status");
	jobs = json_object_get_array_member(st, "jobs");
	g_assert_cmpuint(json_array_get_length(jobs), ==, 1);
	id = json_object_get_int_member(json_array_get_object_element(jobs, 0), "id");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(jobs, 0), "waiting-for"), ==, "shell");

	reply = command(b, "gabbrev-cancel 99999");
	g_assert_true(g_str_has_prefix(reply, "ERROR"));
	line = g_strdup_printf("gabbrev-cancel %" G_GINT64_FORMAT, id);
	command_ok(b, line);
	st2 = command_json(b, "gabbrev-status");
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(st2, "jobs")), ==, 0);
	g_assert_true(saw_event(b, "failed ;sl cancelled"));
	/* Nothing was typed over the trigger, and nothing arrives later */
	pump(&b->r, 300);
	p = presses(b, 0);
	g_assert_cmpuint(p->len, ==, 3);
}

/* The text-input backend: the trigger out and the text in as one
   input-method edit -- no Backspaces, no clipboard, any character */
static void
test_text_input(
	Bench         *b,
	gconstpointer  data
){
	g_autoptr(JsonObject) st = NULL;
	g_autoptr(GArray) p = NULL;
	gint64 deadline;
	gboolean got = FALSE;

	(void)data;
	BENCH_UP(b);
	if (b->r.client.text_input == NULL) {
		g_test_skip("no text-input-v3 here");
		return;
	}
	st = command_json(b, "gabbrev-status");
	g_assert_true(json_object_get_boolean_member(st, "focused-text-input"));
	command_ok(b, "gabbrev-add --name ti {matches: [{trigger: ';ti', "
	           "replace: 'caf\xc3\xa9 \xe2\x86\x92', force_mode: text-input}]}");
	type(b, ";ti");
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (!got && g_get_monotonic_time() < deadline) {
		pump(&b->r, 10);
		g_mutex_lock(&b->r.client.lock);
		got = b->r.client.ti_log->len > 0;
		g_mutex_unlock(&b->r.client.lock);
	}
	pump(&b->r, 100);
	g_mutex_lock(&b->r.client.lock);
	g_assert_cmpstr(b->r.client.ti_log->str, ==,
	                "del=3,0 commit=caf\xc3\xa9 \xe2\x86\x92;");
	g_mutex_unlock(&b->r.client.lock);
	/* Only the user's own three keys: nothing typed, nothing pasted */
	p = presses(b, 0);
	g_assert_cmpuint(p->len, ==, 3);
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

/* No text field takes the commit (the client never enables the text
   input it is entered for): after a moment it is typed instead */
static void
test_text_input_fallback(
	Bench         *b,
	gconstpointer  data
){
	static const guint32 want[] = {
		KEY_BACKSPACE, KEY_BACKSPACE, KEY_BACKSPACE, KEY_O, KEY_K
	};

	(void)data;
	BENCH_UP(b);
	if (b->r.client.text_input == NULL) {
		g_test_skip("no text-input-v3 here");
		return;
	}
	command_ok(b, "gabbrev-add --name ti {matches: [{trigger: ';tk', "
	           "replace: ok, force_mode: text-input}]}");
	g_atomic_int_set(&rig_text_input_refuse, 1);
	type(b, ";tk");
	assert_typed(b, 3, want, G_N_ELEMENTS(want));
	g_atomic_int_set(&rig_text_input_refuse, 0);
	g_mutex_lock(&b->r.client.lock);
	g_assert_cmpstr(b->r.client.ti_log->str, ==, "");
	g_mutex_unlock(&b->r.client.lock);
}

#define ADD(name, fn) \
	g_test_add(name, Bench, NULL, bench_up, fn, bench_down)

int
main(int argc, char **argv)
{
	gint rc;

	g_test_init(&argc, &argv, NULL);
	xdg_scratch = g_dir_make_tmp("gabbrev-xdg-XXXXXX", NULL);
	g_assert_nonnull(xdg_scratch);
	g_setenv("XDG_STATE_HOME", xdg_scratch, TRUE);
	g_setenv("XDG_CONFIG_HOME", xdg_scratch, TRUE);
	rig_text_input = TRUE;
	ADD("/gabbrev/module/expand", test_expand);
	ADD("/gabbrev/module/word", test_word);
	ADD("/gabbrev/module/expand-key", test_expand_key);
	ADD("/gabbrev/module/undo", test_undo);
	ADD("/gabbrev/module/cursor-hint", test_cursor_hint);
	ADD("/gabbrev/module/paste", test_paste);
	ADD("/gabbrev/module/shell", test_shell);
	ADD("/gabbrev/module/failure", test_failure);
	ADD("/gabbrev/module/registered-type", test_registered_type);
	ADD("/gabbrev/module/c-variables", test_c_variables);
	ADD("/gabbrev/module/deny-and-off", test_deny_and_off);
	ADD("/gabbrev/module/ipc", test_ipc);
	ADD("/gabbrev/module/stats", test_stats);
	ADD("/gabbrev/module/transient-clipboard", test_transient_clipboard);
	ADD("/gabbrev/module/watch", test_watch);
	ADD("/gabbrev/module/learn", test_learn);
	ADD("/gabbrev/module/pending-cancel", test_pending_cancel);
	ADD("/gabbrev/module/text-input", test_text_input);
	ADD("/gabbrev/module/text-input-fallback", test_text_input_fallback);
	g_test_add("/gabbrev/module/macro-variables", Bench, NULL,
	           bench_up_macro, test_macro_variables, bench_down_macro);
	rc = g_test_run();
	forget_state();
	/* The clipboard history's files, and the directories */
	remove_tree(xdg_scratch);
	g_free(xdg_scratch);
	return rc;
}
