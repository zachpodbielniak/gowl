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
 * The editing rules of a panel text field, with no compositor.
 *
 * What matters most here is that the cursor can never land inside a
 * multi-byte character: every later insert, delete and pango cursor
 * lookup assumes it sits on a boundary, and a field fed "é" or "日本"
 * that put it one byte off would corrupt the text on the next key.
 * Second is that the window manager's chords are never eaten --- a
 * text field that swallowed Super+anything would trap the user in it.
 */

#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "gowl-enums.h"
#include "barkit/gowl-bar-entry.h"

static void
test_typing_appends_at_the_cursor(void)
{
	g_autoptr(GowlBarEntry) e = gowl_bar_entry_new(NULL);

	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_h, 0, FALSE),
	                ==, GOWL_BAR_ENTRY_EDITED);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_i, 0, FALSE),
	                ==, GOWL_BAR_ENTRY_EDITED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "hi");
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 2);

	/* Navigation letters are letters here: j, k and space type. */
	gowl_bar_entry_handle_key(e, XKB_KEY_space, 0, FALSE);
	gowl_bar_entry_handle_key(e, XKB_KEY_j, 0, FALSE);
	gowl_bar_entry_handle_key(e, XKB_KEY_k, 0, FALSE);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "hi jk");

	/* A shifted keysym arrives already upper case. */
	gowl_bar_entry_handle_key(e, XKB_KEY_A, GOWL_KEY_MOD_SHIFT, FALSE);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "hi jkA");
}

static void
test_multibyte_text_keeps_the_cursor_on_a_boundary(void)
{
	g_autoptr(GowlBarEntry) e = gowl_bar_entry_new("a\xc3\xa9\xe6\x97\xa5");

	/* "a é 日": 1 + 2 + 3 bytes, cursor at the end. */
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 6);

	gowl_bar_entry_handle_key(e, XKB_KEY_Left, 0, FALSE);
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 3);
	gowl_bar_entry_handle_key(e, XKB_KEY_Left, 0, FALSE);
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 1);

	/* Delete removes the whole two-byte é, not half of it. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Delete, 0, FALSE),
	                ==, GOWL_BAR_ENTRY_EDITED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "a\xe6\x97\xa5");
	g_assert_true(g_utf8_validate(gowl_bar_entry_get_text(e), -1, NULL));

	gowl_bar_entry_handle_key(e, XKB_KEY_End, 0, FALSE);
	gowl_bar_entry_handle_key(e, XKB_KEY_BackSpace, 0, FALSE);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "a");

	/* A cursor set into the middle of a character backs up to it. */
	gowl_bar_entry_set_text(e, "\xe6\x97\xa5\xe6\x9c\xac");
	gowl_bar_entry_set_cursor(e, 4);
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 3);
	gowl_bar_entry_set_cursor(e, 99);
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 6);

	/* A non-ASCII keysym types its character. */
	gowl_bar_entry_set_text(e, "");
	gowl_bar_entry_handle_key(e, XKB_KEY_eacute, 0, FALSE);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "\xc3\xa9");
}

static void
test_editing_chords(void)
{
	g_autoptr(GowlBarEntry) e = gowl_bar_entry_new("rm -rf  build");

	/* Ctrl+W takes the trailing word and nothing more. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_w,
	                GOWL_KEY_MOD_CTRL, FALSE), ==, GOWL_BAR_ENTRY_EDITED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "rm -rf  ");
	gowl_bar_entry_handle_key(e, XKB_KEY_w, GOWL_KEY_MOD_CTRL, FALSE);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "rm ");

	/* Ctrl+A / Ctrl+E move to the ends. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_a,
	                GOWL_KEY_MOD_CTRL, FALSE), ==, GOWL_BAR_ENTRY_MOVED);
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 0);
	gowl_bar_entry_handle_key(e, XKB_KEY_e, GOWL_KEY_MOD_CTRL, FALSE);
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 3);

	/* Ctrl+U clears everything before the cursor. */
	gowl_bar_entry_handle_key(e, XKB_KEY_Left, 0, FALSE);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_u,
	                GOWL_KEY_MOD_CTRL, FALSE), ==, GOWL_BAR_ENTRY_EDITED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, " ");
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 0);

	/* Backspace at the start changes nothing and says so. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_BackSpace, 0,
	                FALSE), ==, GOWL_BAR_ENTRY_MOVED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, " ");

	/* Ctrl+V and Shift+Insert ask for a paste rather than doing one. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_v,
	                GOWL_KEY_MOD_CTRL, FALSE), ==, GOWL_BAR_ENTRY_PASTE);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Insert,
	                GOWL_KEY_MOD_SHIFT, FALSE), ==, GOWL_BAR_ENTRY_PASTE);

	/* Ctrl+Shift+V is the widget's (translate the clipboard). */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_V,
	                GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT, FALSE),
	                ==, GOWL_BAR_ENTRY_IGNORED);

	/* Any other Ctrl chord is left for the plugin. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_s,
	                GOWL_KEY_MOD_CTRL, FALSE), ==, GOWL_BAR_ENTRY_IGNORED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, " ");
}

static void
test_window_manager_chords_pass_through(void)
{
	g_autoptr(GowlBarEntry) e = gowl_bar_entry_new("x");

	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_q,
	                GOWL_KEY_MOD_LOGO, FALSE), ==, GOWL_BAR_ENTRY_IGNORED);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Tab,
	                GOWL_KEY_MOD_ALT, FALSE), ==, GOWL_BAR_ENTRY_IGNORED);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_BackSpace,
	                GOWL_KEY_MOD_LOGO, FALSE), ==, GOWL_BAR_ENTRY_IGNORED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "x");

	/* Function keys and bare modifiers type nothing. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_F5, 0, FALSE),
	                ==, GOWL_BAR_ENTRY_IGNORED);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Shift_L, 0,
	                FALSE), ==, GOWL_BAR_ENTRY_IGNORED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "x");
}

static void
test_return_and_tab(void)
{
	g_autoptr(GowlBarEntry) e = gowl_bar_entry_new("one");

	/* Single line: Return submits. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Return, 0,
	                FALSE), ==, GOWL_BAR_ENTRY_SUBMIT);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "one");

	/* Multiline: Return is a newline, Ctrl+Return submits. */
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Return, 0,
	                TRUE), ==, GOWL_BAR_ENTRY_EDITED);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "one\n");
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Return,
	                GOWL_KEY_MOD_CTRL, TRUE), ==, GOWL_BAR_ENTRY_SUBMIT);

	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Tab, 0, FALSE),
	                ==, GOWL_BAR_ENTRY_LEAVE_NEXT);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_Tab,
	                GOWL_KEY_MOD_SHIFT, FALSE),
	                ==, GOWL_BAR_ENTRY_LEAVE_PREV);
	g_assert_cmpint(gowl_bar_entry_handle_key(e, XKB_KEY_ISO_Left_Tab,
	                GOWL_KEY_MOD_SHIFT, FALSE),
	                ==, GOWL_BAR_ENTRY_LEAVE_PREV);
}

static void
test_pasted_text_is_cleaned(void)
{
	g_autoptr(GowlBarEntry) e = gowl_bar_entry_new(NULL);

	/* A one-line field flattens a pasted paragraph and its tabs. */
	g_assert_true(gowl_bar_entry_insert(e, "a\tb\r\nc", FALSE));
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "a b  c");

	/* A multiline one keeps the newline and drops the CR. */
	gowl_bar_entry_set_text(e, "");
	gowl_bar_entry_insert(e, "a\r\nb", TRUE);
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "a \nb");

	/* Invalid UTF-8 is refused outright, not half inserted. */
	gowl_bar_entry_set_text(e, "ok");
	g_assert_false(gowl_bar_entry_insert(e, "\xff\xfe", FALSE));
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "ok");

	/* So is invalid text handed over by a plugin. */
	gowl_bar_entry_set_text(e, "\xc3");
	g_assert_cmpstr(gowl_bar_entry_get_text(e), ==, "");
	g_assert_cmpuint(gowl_bar_entry_get_cursor(e), ==, 0);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-entry/typing", test_typing_appends_at_the_cursor);
	g_test_add_func("/bar-entry/multibyte-boundaries",
	                test_multibyte_text_keeps_the_cursor_on_a_boundary);
	g_test_add_func("/bar-entry/editing-chords", test_editing_chords);
	g_test_add_func("/bar-entry/wm-chords-pass-through",
	                test_window_manager_chords_pass_through);
	g_test_add_func("/bar-entry/return-and-tab", test_return_and_tab);
	g_test_add_func("/bar-entry/paste-cleaning",
	                test_pasted_text_is_cleaned);

	return g_test_run();
}
