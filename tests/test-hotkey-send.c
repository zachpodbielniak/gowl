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
 * test-hotkey-send.c - AutoHotkey's Send syntax, parsed.
 *
 * Every escape: modifier prefixes for one key, named keys, counts,
 * down/up, U+ code points, {Blind}, {Raw}, {Text}, the brace and
 * symbol literals, {Click}, {Sleep}; newlines and tabs; UTF-8; and the
 * error each malformed string gets.
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "gowl.h"
#include "../modules/hotkey/gowl-hotkey-send.c"

static void
assert_plan(
	const gchar *keys,
	const gchar *want
){
	g_autoptr(GError) error = NULL;
	g_autoptr(HkSendPlan) plan = hk_send_parse(keys, &error);
	g_autofree gchar *got = NULL;

	g_assert_no_error(error);
	g_assert_nonnull(plan);
	got = hk_send_describe(plan);
	if (g_strcmp0(got, want) != 0)
		g_test_message("%s -> `%s', want `%s'", keys, got, want);
	g_assert_cmpstr(got, ==, want);
}

static void
assert_refused(
	const gchar *keys,
	const gchar *mentions
){
	g_autoptr(GError) error = NULL;
	g_autoptr(HkSendPlan) plan = hk_send_parse(keys, &error);

	g_assert_null(plan);
	g_assert_nonnull(error);
	if (strstr(error->message, mentions) == NULL)
		g_test_message("%s: %s", keys, error->message);
	g_assert_nonnull(strstr(error->message, mentions));
}

static void
test_characters(void)
{
	assert_plan("hi", "'h' 'i'");
	assert_plan("a\nb\tc", "'a' Return 'b' Tab 'c'");
	assert_plan("é€", "'é' '€'");
	assert_plan("a\r\nb", "'a' Return 'b'");
}

static void
test_modifier_prefixes(void)
{
	/* a modifier holds for the next key only */
	assert_plan("^c", "Ctrl+c");
	assert_plan("^+t", "Ctrl+Shift+t");
	assert_plan("#!{Del}", "Super+Alt+Delete");
	assert_plan("^ab", "Ctrl+a 'b'");
	assert_plan("+{Tab 2}", "Shift+Tab Shift+Tab");
}

static void
test_named_keys(void)
{
	assert_plan("{Enter}", "Return");
	assert_plan("{Esc}{PgDn}", "Escape Next");
	assert_plan("{F13}", "F13");
	assert_plan("{Tab 3}", "Tab Tab Tab");
	assert_plan("{Left 0}", "");
	assert_plan("{Shift down}x{Shift up}", "Shift_L:down 'x' Shift_L:up");
	assert_plan("{code:183}", "code:183");
	assert_plan("{XF86AudioMute}", "XF86AudioMute");
}

static void
test_literals(void)
{
	assert_plan("{{}x{}}", "'{' 'x' '}'");
	assert_plan("{!}{^}{+}{#}", "'!' '^' '+' '#'");
	assert_plan("{U+00E9}", "'é'");
	assert_plan("{ }", "' '");
}

static void
test_modes(void)
{
	assert_plan("{Blind}^{Right}", "blind Ctrl+Right");
	/* {Raw}: nothing after it is special */
	assert_plan("{Raw}^c{Enter}", "'^' 'c' '{' 'E' 'n' 't' 'e' 'r' '}'");
	/* {Text}: the rest is one text run */
	assert_plan("x{Text}^c {Enter}", "'x' text:\"^c {Enter}\"");
}

static void
test_clicks_and_sleep(void)
{
	assert_plan("{Click}", "button:272");
	assert_plan("{Click right}", "button:273");
	assert_plan("^{LButton}", "Ctrl+button:272");
	assert_plan("{XButton1 down}", "button:275:down");
	assert_plan("a{Sleep 100}b", "'a' sleep:100 'b'");
}

static void
test_refusals(void)
{
	assert_refused("{Enter", "with no }");
	assert_refused("{}", "empty");
	assert_refused("{Nope}", "Nope");
	assert_refused("{Tab x}", "count");
	assert_refused("{Tab 999}", "count");
	assert_refused("{U+zz}", "hexadecimal");
	assert_refused("^", "ends in a modifier");
	assert_refused("{Sleep}", "milliseconds");
	assert_refused("{WheelUp}", "wheel");
	assert_refused("{Click sideways}", "Click");
}

static void
test_send_text(void)
{
	g_autoptr(HkSendPlan) plan = hk_send_text_plan("^c{x}\n");
	g_autofree gchar *got = hk_send_describe(plan);

	/* SendText: every character is itself */
	g_assert_cmpstr(got, ==, "'^' 'c' '{' 'x' '}' Return");
	g_assert_false(plan->blind);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hotkey/send/characters", test_characters);
	g_test_add_func("/hotkey/send/modifier-prefixes", test_modifier_prefixes);
	g_test_add_func("/hotkey/send/named-keys", test_named_keys);
	g_test_add_func("/hotkey/send/literals", test_literals);
	g_test_add_func("/hotkey/send/modes", test_modes);
	g_test_add_func("/hotkey/send/clicks-and-sleep", test_clicks_and_sleep);
	g_test_add_func("/hotkey/send/refusals", test_refusals);
	g_test_add_func("/hotkey/send/text", test_send_text);

	return g_test_run();
}
