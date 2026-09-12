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

#include "config/gowl-keybind.h"
#include "gowl-enums.h"
#include <xkbcommon/xkbcommon.h>
#include <string.h>

static void
test_keybind_parse_simple(void)
{
	guint mods = 0;
	guint sym = 0;
	gboolean ok;

	ok = gowl_keybind_parse("Return", &mods, &sym);
	g_assert_true(ok);
	g_assert_cmpuint(mods, ==, 0);
	g_assert_cmpuint(sym, ==, XKB_KEY_Return);
}

static void
test_keybind_parse_super(void)
{
	guint mods = 0;
	guint sym = 0;
	gboolean ok;

	ok = gowl_keybind_parse("Super+Return", &mods, &sym);
	g_assert_true(ok);
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_LOGO);
	g_assert_cmpuint(sym, ==, XKB_KEY_Return);
}

static void
test_keybind_parse_combo(void)
{
	guint mods = 0;
	guint sym = 0;
	gboolean ok;

	ok = gowl_keybind_parse("Super+Shift+c", &mods, &sym);
	g_assert_true(ok);
	g_assert_cmpuint(mods & GOWL_KEY_MOD_LOGO, !=, 0);
	g_assert_cmpuint(mods & GOWL_KEY_MOD_SHIFT, !=, 0);
	g_assert_cmpuint(sym, ==, XKB_KEY_c);
}

static void
test_keybind_parse_ctrl_alt(void)
{
	guint mods = 0;
	guint sym = 0;
	gboolean ok;

	ok = gowl_keybind_parse("Ctrl+Alt+Delete", &mods, &sym);
	g_assert_true(ok);
	g_assert_cmpuint(mods & GOWL_KEY_MOD_CTRL, !=, 0);
	g_assert_cmpuint(mods & GOWL_KEY_MOD_ALT, !=, 0);
	g_assert_cmpuint(sym, ==, XKB_KEY_Delete);
}

static void
test_keybind_parse_invalid(void)
{
	guint mods = 0;
	guint sym = 0;
	gboolean ok;

	ok = gowl_keybind_parse("", &mods, &sym);
	g_assert_false(ok);

	/* NULL triggers g_return_val_if_fail, test with subprocess to avoid abort */
	if (g_test_subprocess()) {
		gowl_keybind_parse(NULL, &mods, &sym);
		return;
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
}

static void
test_keybind_to_string(void)
{
	gchar *str;

	str = gowl_keybind_to_string(GOWL_KEY_MOD_LOGO, XKB_KEY_Return);
	g_assert_nonnull(str);
	/* Should contain "Logo" or "Super" and "Return" */
	g_assert_true(strstr(str, "Return") != NULL);
	g_free(str);
}

static void
test_keybind_roundtrip(void)
{
	guint mods = 0;
	guint sym = 0;
	gchar *str;
	guint mods2 = 0;
	guint sym2 = 0;

	gowl_keybind_parse("Super+Shift+Return", &mods, &sym);
	str = gowl_keybind_to_string(mods, sym);
	g_assert_nonnull(str);

	gowl_keybind_parse(str, &mods2, &sym2);
	g_assert_cmpuint(mods, ==, mods2);
	g_assert_cmpuint(sym, ==, sym2);

	g_free(str);
}

/*
 * The build defines G_LOG_USE_STRUCTURED, so a warning goes through the
 * writer rather than the legacy handler: g_test_expect_message() never
 * sees it and GTest's abort on G_LOG_LEVEL_WARNING fires instead.  The
 * one interception point is a writer that swallows the warning a test
 * expects.  Installed once; transparent while nothing is expected.
 */
static const char *expected_warning = NULL;
static gboolean saw_expected_warning = FALSE;

static GLogWriterOutput
expect_warning_writer(GLogLevelFlags level, const GLogField *fields,
                      gsize n_fields, gpointer user_data)
{
	gsize i;
	(void)user_data;

	if (expected_warning != NULL && (level & G_LOG_LEVEL_WARNING)) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && strstr((const char *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning = TRUE;
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(level, fields, n_fields, user_data);
}

static void
expect_warning(const char *substring)
{
	static gboolean installed = FALSE;

	if (!installed) {
		g_log_set_writer_func(expect_warning_writer, NULL, NULL);
		installed = TRUE;
	}
	expected_warning = substring;
	saw_expected_warning = FALSE;
}

static void
assert_expected_warning(void)
{
	g_assert_true(saw_expected_warning);
	expected_warning = NULL;
}

/* --- pointer binds --- */

static void
test_mousebind_parse(void)
{
	guint mods, button;

	g_assert_true(gowl_mousebind_parse("Super+Button1", &mods, &button));
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_LOGO);
	g_assert_cmpuint(button, ==, 0x110 /* BTN_LEFT */);

	g_assert_true(gowl_mousebind_parse("super+shift+Right", &mods, &button));
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT);
	g_assert_cmpuint(button, ==, 0x111 /* BTN_RIGHT */);

	g_assert_true(gowl_mousebind_parse("Super+WheelUp", &mods, &button));
	g_assert_cmpuint(button, ==, GOWL_BUTTON_WHEEL_UP);
	g_assert_true(gowl_mousebind_parse("Button5", &mods, &button));
	g_assert_cmpuint(mods, ==, 0);
	g_assert_cmpuint(button, ==, GOWL_BUTTON_WHEEL_DOWN);
	g_assert_true(gowl_mousebind_parse("Alt+Back", &mods, &button));
	g_assert_cmpuint(button, ==, 0x113 /* BTN_SIDE */);

	expect_warning("unknown button");
	g_assert_false(gowl_mousebind_parse("Super+Button99", &mods, &button));
	assert_expected_warning();
	expect_warning("unknown modifier");
	g_assert_false(gowl_mousebind_parse("Hyper+Button1", &mods, &button));
	assert_expected_warning();
}

static void
test_mousebind_roundtrip(void)
{
	guint mods, button;
	gchar *str;

	str = gowl_mousebind_to_string(GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_CTRL,
	                               GOWL_BUTTON_WHEEL_LEFT);
	g_assert_cmpstr(str, ==, "Super+Ctrl+Button6");
	g_assert_true(gowl_mousebind_parse(str, &mods, &button));
	g_assert_cmpuint(button, ==, GOWL_BUTTON_WHEEL_LEFT);
	g_free(str);
}

/* --- gestures --- */

static void
test_gesture_parse(void)
{
	GowlGestureKind kind;
	GowlGestureDirection dir;
	guint fingers;
	gchar *str;

	g_assert_true(gowl_gesture_parse("swipe-left-3", &kind, &dir, &fingers));
	g_assert_cmpint(kind, ==, GOWL_GESTURE_SWIPE);
	g_assert_cmpint(dir, ==, GOWL_GESTURE_LEFT);
	g_assert_cmpuint(fingers, ==, 3);
	g_assert_true(gowl_gesture_parse("Pinch-Out-4", &kind, &dir, &fingers));
	g_assert_cmpint(kind, ==, GOWL_GESTURE_PINCH);
	g_assert_cmpint(dir, ==, GOWL_GESTURE_OUT);
	g_assert_cmpuint(fingers, ==, 4);

	str = gowl_gesture_to_string(GOWL_GESTURE_SWIPE, GOWL_GESTURE_DOWN, 4);
	g_assert_cmpstr(str, ==, "swipe-down-4");
	g_free(str);

	expect_warning("is not swipe");
	g_assert_false(gowl_gesture_parse("swipe-in-3", &kind, &dir, &fingers));
	assert_expected_warning();
	expect_warning("is not swipe");
	g_assert_false(gowl_gesture_parse("tap-3", &kind, &dir, &fingers));
	assert_expected_warning();
}

static void
test_gesture_classify(void)
{
	/* The dominant axis decides; too short is nothing. */
	g_assert_cmpint(gowl_gesture_classify_swipe(-120, 10, 60), ==, GOWL_GESTURE_LEFT);
	g_assert_cmpint(gowl_gesture_classify_swipe(80, -70, 60), ==, GOWL_GESTURE_RIGHT);
	g_assert_cmpint(gowl_gesture_classify_swipe(5, -90, 60), ==, GOWL_GESTURE_UP);
	g_assert_cmpint(gowl_gesture_classify_swipe(0, 61, 60), ==, GOWL_GESTURE_DOWN);
	g_assert_cmpint(gowl_gesture_classify_swipe(30, 30, 60), ==, GOWL_GESTURE_NONE);

	g_assert_cmpint(gowl_gesture_classify_pinch(0.5), ==, GOWL_GESTURE_IN);
	g_assert_cmpint(gowl_gesture_classify_pinch(1.6), ==, GOWL_GESTURE_OUT);
	g_assert_cmpint(gowl_gesture_classify_pinch(1.1), ==, GOWL_GESTURE_NONE);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/keybind/parse/simple", test_keybind_parse_simple);
	g_test_add_func("/keybind/parse/super", test_keybind_parse_super);
	g_test_add_func("/keybind/parse/combo", test_keybind_parse_combo);
	g_test_add_func("/keybind/parse/ctrl-alt", test_keybind_parse_ctrl_alt);
	g_test_add_func("/keybind/parse/invalid", test_keybind_parse_invalid);
	g_test_add_func("/keybind/to-string", test_keybind_to_string);
	g_test_add_func("/keybind/roundtrip", test_keybind_roundtrip);
	g_test_add_func("/keybind/mouse/parse", test_mousebind_parse);
	g_test_add_func("/keybind/mouse/roundtrip", test_mousebind_roundtrip);
	g_test_add_func("/keybind/gesture/parse", test_gesture_parse);
	g_test_add_func("/keybind/gesture/classify", test_gesture_classify);

	return g_test_run();
}
