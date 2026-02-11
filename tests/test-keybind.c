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

	return g_test_run();
}
