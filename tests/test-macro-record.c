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
 * test-macro-record.c - Recordings into macros, on their own.
 *
 * The translator is pure GLib and json-glib, so each rule it applies is
 * checked against hand-written recorder payloads: the record key's own
 * half-presses are dropped, a chord is one step with its modifiers,
 * lone modifiers vanish, a click and a drag come out differently, a
 * scroll keeps its notches, pauses are written and capped, and an empty
 * recording is an error rather than an empty macro.  That the result
 * compiles and runs is test-macro-runner's job, against the real module.
 */

#include <glib.h>
#include <string.h>

#include "../modules/macro/gowl-macro-record.c"

/* Wraps event objects into a recorder payload. */
static gchar *
payload(const gchar *events, guint suppressed)
{
	return g_strdup_printf("{\"token\":null,\"owner\":\"macro\","
	                       "\"suppressed_total\":%u,\"events\":[%s]}",
	                       suppressed, events);
}

static gchar *
translate(const gchar *events, GowlMacroRecordStats *st)
{
	g_autofree gchar *p = payload(events, 0);
	g_autoptr(GError) error = NULL;
	gchar *src;

	src = gowl_macro_record_to_source(p, "t", 3000, st, &error);
	g_assert_no_error(error);
	g_assert_nonnull(src);
	return src;
}

/* How many times @needle occurs in @hay. */
static guint
count(const gchar *hay, const gchar *needle)
{
	guint n = 0;
	const gchar *p = hay;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

#define KEY(ms, code, sym, state, mods) \
	"{\"type\":\"key\",\"offset_ms\":" #ms ",\"keycode\":" #code \
	",\"keysym\":\"" sym "\",\"state\":\"" state "\",\"mods\":" #mods "}"
#define MOVE(ms, x, y) \
	"{\"type\":\"pointer_motion\",\"offset_ms\":" #ms ",\"x\":" #x \
	",\"y\":" #y ",\"dx\":1,\"dy\":1,\"merged\":0}"
#define BTN(ms, state, x, y) \
	"{\"type\":\"pointer_button\",\"offset_ms\":" #ms ",\"button\":272" \
	",\"state\":\"" state "\",\"x\":" #x ",\"y\":" #y ",\"mods\":0}"

/* The record key: its release at the start and its press at the end
   have no partners, and are not in the macro. */
static void
test_record_key_dropped(void)
{
	GowlMacroRecordStats st;
	g_autofree gchar *src = NULL;

	src = translate(
		/* Super+Alt+r still held as recording begins */
		KEY(1, 19, "r", "release", 72) ","
		KEY(2, 56, "Alt_L", "release", 64) ","
		KEY(3, 125, "Super_L", "release", 0) ","
		/* what was meant: type `a' */
		KEY(100, 30, "a", "press", 0) ","
		KEY(150, 30, "a", "release", 0) ","
		/* Super+Alt+r again, to stop: never released */
		KEY(400, 125, "Super_L", "press", 0) ","
		KEY(410, 56, "Alt_L", "press", 64) ","
		KEY(420, 19, "r", "press", 72), &st);

	g_assert_cmpuint(st.steps, ==, 1);
	g_assert_nonnull(strstr(src, "gowl_macro_key_code(ctx, NULL, 30, 0);"));
	g_assert_nonnull(strstr(src, "/* a */"));
	g_assert_null(strstr(src, ", 19,"));
	/* a lone release, two lone modifiers each way, a lone press */
	g_assert_cmpuint(st.dropped, >=, 2);
}

/* Ctrl+c is one step carrying CTRL, not three key taps. */
static void
test_chord_is_one_step(void)
{
	GowlMacroRecordStats st;
	g_autofree gchar *src = NULL;

	src = translate(
		KEY(0, 29, "Control_L", "press", 0) ","
		KEY(10, 46, "c", "press", 4) ","
		KEY(30, 46, "c", "release", 4) ","
		KEY(40, 29, "Control_L", "release", 4) ","
		/* Super+Shift+3 is a compositor keybind; it replays as one */
		KEY(500, 4, "numbersign", "press", 65) ","
		KEY(520, 4, "numbersign", "release", 65), &st);

	g_assert_cmpuint(st.steps, ==, 2);
	g_assert_nonnull(strstr(src,
		"gowl_macro_key_code(ctx, NULL, 46, GOWL_KEY_MOD_CTRL);"));
	g_assert_nonnull(strstr(src,
		"gowl_macro_key_code(ctx, NULL, 4, GOWL_KEY_MOD_LOGO | "
		"GOWL_KEY_MOD_SHIFT);"));
	g_assert_null(strstr(src, "Control_L"));
}

/* Caps Lock and Num Lock in the mask are not replayed. */
static void
test_lock_bits_masked(void)
{
	g_autofree gchar *src = NULL;

	/* 2 = Caps Lock, 16 = Num Lock (Mod2) */
	src = translate(KEY(0, 30, "A", "press", 18) ","
	                KEY(20, 30, "A", "release", 18), NULL);
	g_assert_nonnull(strstr(src, "gowl_macro_key_code(ctx, NULL, 30, 0);"));
}

/* A press and a release in the same place is a click; the wander to
   it is one move, not every sample. */
static void
test_click(void)
{
	GowlMacroRecordStats st;
	g_autofree gchar *src = NULL;

	src = translate(
		MOVE(0, 10, 10) "," MOVE(10, 50, 60) "," MOVE(20, 100, 120) ","
		BTN(40, "press", 100, 120) "," BTN(90, "release", 100, 120), &st);

	g_assert_cmpuint(count(src, "gowl_macro_pointer("), ==, 1);
	g_assert_nonnull(strstr(src, "gowl_macro_pointer(ctx, 100, 120);"));
	g_assert_nonnull(strstr(src, "gowl_macro_button(ctx, BTN_LEFT);"));
	g_assert_null(strstr(src, "button_state"));
	g_assert_cmpuint(st.steps, ==, 2);
}

/* Moving with the button down is a drag: press, a sampled path,
   release where it ended. */
static void
test_drag(void)
{
	g_autofree gchar *src = NULL;

	src = translate(
		BTN(0, "press", 10, 10) ","
		MOVE(5, 11, 11) "," MOVE(30, 40, 40) "," MOVE(60, 80, 90) ","
		BTN(70, "release", 80, 90), NULL);

	g_assert_nonnull(strstr(src,
		"gowl_macro_button_state(ctx, BTN_LEFT, TRUE);"));
	g_assert_nonnull(strstr(src,
		"gowl_macro_button_state(ctx, BTN_LEFT, FALSE);"));
	g_assert_nonnull(strstr(src, "gowl_macro_pointer(ctx, 80, 90);"));
	g_assert_null(strstr(src, "gowl_macro_button(ctx"));
	/* the press comes before the path, the release after it */
	g_assert_true(strstr(src, "TRUE);") < strstr(src, "pointer(ctx, 80"));
	g_assert_true(strstr(src, "pointer(ctx, 80") < strstr(src, "FALSE);"));
}

/* A scroll happens where the pointer is, with its notch count. */
static void
test_scroll(void)
{
	g_autofree gchar *src = NULL;

	src = translate(
		"{\"type\":\"pointer_axis\",\"offset_ms\":5,\"axis\":\"vertical\","
		"\"value\":15.5,\"discrete\":120,\"x\":300,\"y\":200}", NULL);

	g_assert_nonnull(strstr(src, "gowl_macro_pointer(ctx, 300, 200);"));
	/* a point, never a comma, whatever the locale */
	g_assert_nonnull(strstr(src, "gowl_macro_scroll(ctx, FALSE, 15.5, 120);"));
}

/* Pauses become waits, scaled at run time; a long one is capped and a
   tiny one is not written at all. */
static void
test_waits(void)
{
	GowlMacroRecordStats st;
	g_autofree gchar *src = NULL;

	src = translate(
		KEY(0, 30, "a", "press", 0) "," KEY(5, 30, "a", "release", 0) ","
		KEY(3, 48, "b", "press", 0) "," KEY(9, 48, "b", "release", 0) ","
		KEY(503, 46, "c", "press", 0) "," KEY(510, 46, "c", "release", 0) ","
		KEY(60503, 32, "d", "press", 0) ","
		KEY(60510, 32, "d", "release", 0), &st);

	/* a->b is 3 ms: no wait; b->c 500 ms; c->d a minute, capped */
	g_assert_cmpuint(count(src, "gowl_macro_wait("), ==, 2);
	g_assert_nonnull(strstr(src, "gowl_macro_wait(ctx, scaled(ctx, 500));"));
	g_assert_nonnull(strstr(src, "gowl_macro_wait(ctx, scaled(ctx, 3000));"));
	g_assert_cmpuint(st.duration_ms, ==, 3500);
	g_assert_cmpuint(st.steps, ==, 4);
	/* the speed factor is the macro's own first argument */
	g_assert_nonnull(strstr(src, "gowl_macro_get_arg(ctx, 0)"));
	g_assert_nonnull(strstr(src, "G_MODULE_EXPORT gboolean\ngowl_macro_run"));
	g_assert_nonnull(strstr(src, "G_MODULE_EXPORT const gchar *\n"
	                              "gowl_macro_info"));
}

/* Nothing but the record key is not a macro. */
static void
test_empty(void)
{
	g_autofree gchar *p = NULL;
	g_autofree gchar *protected = NULL;
	g_autoptr(GError) error = NULL;
	gchar *src;

	p = payload(KEY(1, 19, "r", "release", 72) ","
	            KEY(400, 19, "r", "press", 72), 0);
	src = gowl_macro_record_to_source(p, "t", 0, NULL, &error);
	g_assert_null(src);
	g_assert_error(error, GOWL_MACRO_RECORD_ERROR,
	               GOWL_MACRO_RECORD_ERROR_EMPTY);
	g_clear_error(&error);

	/* everything went to a password prompt: the message says so */
	protected = payload("", 12);
	src = gowl_macro_record_to_source(protected, "t", 0, NULL, &error);
	g_assert_null(src);
	g_assert_nonnull(strstr(error->message, "password"));
}

/* Protected input is called out in the file, so the gap is not a
   mystery when the replay misses it. */
static void
test_suppressed_noted(void)
{
	g_autofree gchar *p = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *src = NULL;
	GowlMacroRecordStats st;

	p = payload(KEY(0, 30, "a", "press", 0) ","
	            KEY(10, 30, "a", "release", 0), 7);
	src = gowl_macro_record_to_source(p, "t", 0, &st, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(st.suppressed, ==, 7);
	g_assert_nonnull(strstr(src, "7 event(s) were NOT recorded"));
}

static void
test_bad_payload(void)
{
	g_autoptr(GError) error = NULL;

	g_assert_null(gowl_macro_record_to_source("[1,2]", "t", 0, NULL,
	                                          &error));
	g_assert_error(error, GOWL_MACRO_RECORD_ERROR,
	               GOWL_MACRO_RECORD_ERROR_PARSE);
	g_clear_error(&error);
	g_assert_null(gowl_macro_record_to_source("{}", "t", 0, NULL, &error));
	g_assert_error(error, GOWL_MACRO_RECORD_ERROR,
	               GOWL_MACRO_RECORD_ERROR_PARSE);
	g_clear_error(&error);
	g_assert_null(gowl_macro_record_to_source("{nope", "t", 0, NULL,
	                                          &error));
	g_assert_nonnull(error);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/macro/record/record-key-dropped",
	                test_record_key_dropped);
	g_test_add_func("/macro/record/chord-is-one-step", test_chord_is_one_step);
	g_test_add_func("/macro/record/lock-bits-masked", test_lock_bits_masked);
	g_test_add_func("/macro/record/click", test_click);
	g_test_add_func("/macro/record/drag", test_drag);
	g_test_add_func("/macro/record/scroll", test_scroll);
	g_test_add_func("/macro/record/waits", test_waits);
	g_test_add_func("/macro/record/empty", test_empty);
	g_test_add_func("/macro/record/suppressed-noted", test_suppressed_noted);
	g_test_add_func("/macro/record/bad-payload", test_bad_payload);
	return g_test_run();
}
