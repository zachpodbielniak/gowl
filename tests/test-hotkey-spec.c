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
 * test-hotkey-spec.c - Hotkeys as AutoHotkey and gowl write them.
 *
 * libgowl's gowl_hotkey_spec_parse(): every AutoHotkey prefix symbol,
 * the handed modifiers and AltGr, `UP', custom combinations and their
 * refusals, mouse and wheel names, gowl notation meaning the same as
 * AutoHotkey's, every AutoHotkey key name, the canonical printing, and
 * the error each kind of mistake gets.
 */

#include <glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"

static GowlHotkeySpec
parse_ok(const gchar *text)
{
	GowlHotkeySpec spec;
	g_autoptr(GError) error = NULL;

	if (!gowl_hotkey_spec_parse(text, &spec, &error))
		g_test_message("%s: %s", text, error->message);
	g_assert_no_error(error);
	return spec;
}

static void
parse_fails(
	const gchar *text,
	gint         code,
	const gchar *mentions
){
	GowlHotkeySpec spec;
	g_autoptr(GError) error = NULL;

	g_assert_false(gowl_hotkey_spec_parse(text, &spec, &error));
	g_assert_error(error, GOWL_HOTKEY_ERROR, code);
	if (mentions != NULL && strstr(error->message, mentions) == NULL)
		g_test_message("`%s' error lacks `%s': %s", text, mentions,
		               error->message);
	if (mentions != NULL)
		g_assert_nonnull(strstr(error->message, mentions));
}

static void
assert_canonical(
	const gchar *text,
	const gchar *want
){
	GowlHotkeySpec spec = parse_ok(text);
	g_autofree gchar *got = gowl_hotkey_spec_to_string(&spec);

	g_assert_cmpstr(got, ==, want);
}

/* `#' Super, `!' Alt, `^' Control, `+' Shift; the last character is
 * always the key. */
static void
test_modifier_symbols(void)
{
	GowlHotkeySpec s;

	s = parse_ok("#!t");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_ALT);
	g_assert_cmpint(s.trigger.kind, ==, GOWL_HOTKEY_INPUT_KEY);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_t);
	g_assert_cmpuint(s.flags, ==, 0);

	s = parse_ok("^+a");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_a);

	/* `^+' is Control and the plus key; `+' alone is the plus key */
	s = parse_ok("^+");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_CTRL);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_plus);
	s = parse_ok("+");
	g_assert_cmpuint(s.mods, ==, 0);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_plus);
	s = parse_ok("#!");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_LOGO);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_exclam);

	/* An upper-case letter is the same key */
	s = parse_ok("^A");
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_a);
}

static void
test_sides_and_altgr(void)
{
	GowlHotkeySpec s;

	s = parse_ok("<^a");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_CTRL);
	g_assert_cmpuint(s.left, ==, GOWL_KEY_MOD_CTRL);
	g_assert_cmpuint(s.right, ==, 0);

	s = parse_ok(">!b");
	g_assert_cmpuint(s.right, ==, GOWL_KEY_MOD_ALT);
	g_assert_cmpuint(s.left, ==, 0);

	/* `<^>!' is AltGr, with other modifiers after it too */
	s = parse_ok("<^>!+x");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_MOD5 | GOWL_KEY_MOD_SHIFT);
	g_assert_cmpuint(s.left, ==, 0);
	s = parse_ok("<^>!e");
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_MOD5);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_e);
}

static void
test_flags_and_up(void)
{
	GowlHotkeySpec s;

	s = parse_ok("*~$F1");
	g_assert_cmpuint(s.flags, ==, GOWL_HOTKEY_FLAG_WILDCARD
	                 | GOWL_HOTKEY_FLAG_PASSTHROUGH
	                 | GOWL_HOTKEY_FLAG_NO_SYNTHETIC);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_F1);

	/* the symbols may sit among the modifiers too */
	s = parse_ok("^~a");
	g_assert_cmpuint(s.flags, ==, GOWL_HOTKEY_FLAG_PASSTHROUGH);
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_CTRL);

	s = parse_ok("a UP");
	g_assert_cmpuint(s.flags, ==, GOWL_HOTKEY_FLAG_ON_RELEASE);
	s = parse_ok("^a up");
	g_assert_cmpuint(s.flags, ==, GOWL_HOTKEY_FLAG_ON_RELEASE);
	g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_CTRL);
	/* `~' alone is the tilde key */
	s = parse_ok("~");
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_asciitilde);
}

static void
test_combinations(void)
{
	GowlHotkeySpec s;

	s = parse_ok("CapsLock & j");
	g_assert_cmpint(s.prefix.kind, ==, GOWL_HOTKEY_INPUT_KEY);
	g_assert_cmpuint(s.prefix.keysym, ==, XKB_KEY_Caps_Lock);
	g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_j);
	/* a combination is its own wildcard */
	g_assert_true(s.flags & GOWL_HOTKEY_FLAG_WILDCARD);

	s = parse_ok("~CapsLock & j");
	g_assert_true(s.flags & GOWL_HOTKEY_FLAG_PASSTHROUGH);

	s = parse_ok("XButton1 & WheelUp");
	g_assert_cmpint(s.prefix.kind, ==, GOWL_HOTKEY_INPUT_BUTTON);
	g_assert_cmpuint(s.prefix.button, ==, BTN_SIDE);
	g_assert_cmpint(s.trigger.kind, ==, GOWL_HOTKEY_INPUT_WHEEL);
	g_assert_cmpuint(s.trigger.button, ==, GOWL_BUTTON_WHEEL_UP);

	s = parse_ok("Numpad0 & Numpad1 UP");
	g_assert_true(s.flags & GOWL_HOTKEY_FLAG_ON_RELEASE);
	g_assert_cmpuint(s.prefix.keysym, ==, XKB_KEY_KP_0);
}

static void
test_refusals(void)
{
	parse_fails("", GOWL_HOTKEY_ERROR_SYNTAX, "empty");
	parse_fails("a & b & c", GOWL_HOTKEY_ERROR_UNSUPPORTED, "three-key");
	parse_fails("WheelUp UP", GOWL_HOTKEY_ERROR_UNSUPPORTED, "no release");
	parse_fails("WheelUp & a", GOWL_HOTKEY_ERROR_UNSUPPORTED, "prefix");
	parse_fails("Nope", GOWL_HOTKEY_ERROR_UNKNOWN_KEY, "Nope");
	parse_fails("^Frobnicate", GOWL_HOTKEY_ERROR_UNKNOWN_KEY, "Frobnicate");
	parse_fails("Super+Frob+t", GOWL_HOTKEY_ERROR_UNKNOWN_KEY, "Frob");
	parse_fails("code:0", GOWL_HOTKEY_ERROR_SYNTAX, "code:");
	parse_fails("code:x", GOWL_HOTKEY_ERROR_SYNTAX, "code:");
	parse_fails("Super+", GOWL_HOTKEY_ERROR_SYNTAX, NULL);
}

/* Both notations, one hotkey */
static void
test_gowl_notation(void)
{
	static const struct { const gchar *gowl; const gchar *ahk; } same[] = {
		{ "Super+Alt+t",          "#!t" },
		{ "Super+Shift+Return",   "#+Enter" },
		{ "Ctrl+Alt+Delete",      "^!Del" },
		{ "LCtrl+Shift+Return",   "<^+Enter" },
		{ "RAlt+F4",              ">!F4" },
		{ "AltGr+e",              "<^>!e" },
		{ "Super+plus",           "#+" },
		{ "~Super+Alt+t",         "~#!t" },
		{ "Super+Alt+t UP",       "#!t UP" },
		{ "Mod4+Mod1+t",          "#!t" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(same); i++) {
		GowlHotkeySpec a = parse_ok(same[i].gowl);
		GowlHotkeySpec b = parse_ok(same[i].ahk);

		if (!gowl_hotkey_spec_equal(&a, &b))
			g_test_message("%s != %s", same[i].gowl, same[i].ahk);
		g_assert_true(gowl_hotkey_spec_equal(&a, &b));
	}
	/* `Super++' is Super and the plus key */
	{
		GowlHotkeySpec s = parse_ok("Super++");

		g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_LOGO);
		g_assert_cmpuint(s.trigger.keysym, ==, XKB_KEY_plus);
	}
	/* Hyper has no AutoHotkey symbol */
	{
		GowlHotkeySpec s = parse_ok("Hyper+a");

		g_assert_cmpuint(s.mods, ==, GOWL_KEY_MOD_MOD3);
	}
}

/* Every AutoHotkey key name this file knows, one row each */
static void
test_key_names(void)
{
	static const struct {
		const gchar *name;
		guint32      keysym;
		guint32      keysym_alt;
	} rows[] = {
		{ "Enter", XKB_KEY_Return, 0 },        { "Esc", XKB_KEY_Escape, 0 },
		{ "Escape", XKB_KEY_Escape, 0 },       { "Space", XKB_KEY_space, 0 },
		{ "Tab", XKB_KEY_Tab, 0 },             { "BS", XKB_KEY_BackSpace, 0 },
		{ "Backspace", XKB_KEY_BackSpace, 0 }, { "Del", XKB_KEY_Delete, 0 },
		{ "Ins", XKB_KEY_Insert, 0 },          { "Home", XKB_KEY_Home, 0 },
		{ "End", XKB_KEY_End, 0 },             { "PgUp", XKB_KEY_Prior, 0 },
		{ "PgDn", XKB_KEY_Next, 0 },           { "Up", XKB_KEY_Up, 0 },
		{ "Down", XKB_KEY_Down, 0 },           { "Left", XKB_KEY_Left, 0 },
		{ "Right", XKB_KEY_Right, 0 },
		{ "CapsLock", XKB_KEY_Caps_Lock, 0 },
		{ "ScrollLock", XKB_KEY_Scroll_Lock, 0 },
		{ "NumLock", XKB_KEY_Num_Lock, 0 },
		{ "PrintScreen", XKB_KEY_Print, 0 },   { "Pause", XKB_KEY_Pause, 0 },
		{ "CtrlBreak", XKB_KEY_Break, 0 },     { "AppsKey", XKB_KEY_Menu, 0 },
		{ "LWin", XKB_KEY_Super_L, 0 },        { "RWin", XKB_KEY_Super_R, 0 },
		{ "LCtrl", XKB_KEY_Control_L, 0 },     { "RControl", XKB_KEY_Control_R, 0 },
		{ "LShift", XKB_KEY_Shift_L, 0 },      { "RShift", XKB_KEY_Shift_R, 0 },
		{ "LAlt", XKB_KEY_Alt_L, 0 },          { "RAlt", XKB_KEY_Alt_R, 0 },
		{ "Ctrl", XKB_KEY_Control_L, XKB_KEY_Control_R },
		{ "Shift", XKB_KEY_Shift_L, XKB_KEY_Shift_R },
		{ "Alt", XKB_KEY_Alt_L, XKB_KEY_Alt_R },
		{ "Win", XKB_KEY_Super_L, XKB_KEY_Super_R },
		{ "Numpad0", XKB_KEY_KP_0, XKB_KEY_KP_Insert },
		{ "Numpad5", XKB_KEY_KP_5, XKB_KEY_KP_Begin },
		{ "NumpadEnter", XKB_KEY_KP_Enter, 0 },
		{ "NumpadAdd", XKB_KEY_KP_Add, 0 },
		{ "Volume_Up", XKB_KEY_XF86AudioRaiseVolume, 0 },
		{ "Volume_Mute", XKB_KEY_XF86AudioMute, 0 },
		{ "Media_Next", XKB_KEY_XF86AudioNext, 0 },
		{ "Browser_Back", XKB_KEY_XF86Back, 0 },
		{ "Launch_Mail", XKB_KEY_XF86Mail, 0 },
		{ "F13", XKB_KEY_F13, 0 },             { "F24", XKB_KEY_F24, 0 },
		/* XKB names work as they are */
		{ "Prior", XKB_KEY_Prior, 0 },
		{ "XF86AudioPlay", XKB_KEY_XF86AudioPlay, 0 },
		{ "KP_7", XKB_KEY_KP_7, 0 },
		/* AutoHotkey's names are not case-sensitive */
		{ "enter", XKB_KEY_Return, 0 },        { "CAPSLOCK", XKB_KEY_Caps_Lock, 0 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++) {
		GowlHotkeyInput in;
		g_autoptr(GError) error = NULL;

		g_assert_true(gowl_hotkey_input_parse(rows[i].name, &in, &error));
		g_assert_no_error(error);
		g_assert_cmpint(in.kind, ==, GOWL_HOTKEY_INPUT_KEY);
		if (in.keysym != rows[i].keysym)
			g_test_message("%s: 0x%x, want 0x%x", rows[i].name, in.keysym,
			               rows[i].keysym);
		g_assert_cmpuint(in.keysym, ==, rows[i].keysym);
		g_assert_cmpuint(in.keysym_alt, ==, rows[i].keysym_alt);
	}
}

static void
test_buttons_and_codes(void)
{
	static const struct {
		const gchar         *name;
		GowlHotkeyInputKind  kind;
		guint32              code;
	} rows[] = {
		{ "LButton", GOWL_HOTKEY_INPUT_BUTTON, BTN_LEFT },
		{ "RButton", GOWL_HOTKEY_INPUT_BUTTON, BTN_RIGHT },
		{ "MButton", GOWL_HOTKEY_INPUT_BUTTON, BTN_MIDDLE },
		{ "XButton1", GOWL_HOTKEY_INPUT_BUTTON, BTN_SIDE },
		{ "XButton2", GOWL_HOTKEY_INPUT_BUTTON, BTN_EXTRA },
		{ "BTN_BACK", GOWL_HOTKEY_INPUT_BUTTON, BTN_BACK },
		{ "WheelUp", GOWL_HOTKEY_INPUT_WHEEL, GOWL_BUTTON_WHEEL_UP },
		{ "WheelDown", GOWL_HOTKEY_INPUT_WHEEL, GOWL_BUTTON_WHEEL_DOWN },
		{ "WheelLeft", GOWL_HOTKEY_INPUT_WHEEL, GOWL_BUTTON_WHEEL_LEFT },
		{ "WheelRight", GOWL_HOTKEY_INPUT_WHEEL, GOWL_BUTTON_WHEEL_RIGHT },
	};
	GowlHotkeyInput in;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++) {
		g_assert_true(gowl_hotkey_input_parse(rows[i].name, &in, NULL));
		g_assert_cmpint(in.kind, ==, rows[i].kind);
		g_assert_cmpuint(in.button, ==, rows[i].code);
	}
	g_assert_true(gowl_hotkey_input_parse("code:183", &in, NULL));
	g_assert_cmpint(in.kind, ==, GOWL_HOTKEY_INPUT_KEY);
	g_assert_cmpuint(in.keycode, ==, 183);
	g_assert_cmpuint(in.keysym, ==, 0);
}

static void
test_canonical(void)
{
	assert_canonical("!#t", "#!t");
	assert_canonical("Super+Alt+t", "#!t");
	assert_canonical("+^a", "^+a");
	assert_canonical("$~*a UP", "*~$a UP");
	assert_canonical("<^a", "<^a");
	assert_canonical("AltGr+e", "<^>!e");
	assert_canonical("capslock & J", "CapsLock & j");
	assert_canonical("Return", "Enter");
	assert_canonical("Prior", "PgUp");
	assert_canonical("code:183", "code:183");
	assert_canonical("xbutton1", "XButton1");
	assert_canonical("Hyper+LCtrl+a", "LCtrl+Hyper+a");
	assert_canonical("F13", "F13");
}

static void
test_matches(void)
{
	GowlHotkeyInput in;

	gowl_hotkey_input_parse("a", &in, NULL);
	/* shifted or not, it is the a key */
	g_assert_true(gowl_hotkey_input_matches_key(&in, 30, XKB_KEY_A, XKB_KEY_a));
	g_assert_true(gowl_hotkey_input_matches_key(&in, 30, XKB_KEY_a, XKB_KEY_a));
	g_assert_false(gowl_hotkey_input_matches_key(&in, 48, XKB_KEY_b, XKB_KEY_b));
	gowl_hotkey_input_parse("Ctrl", &in, NULL);
	g_assert_true(gowl_hotkey_input_matches_key(&in, 97, XKB_KEY_Control_R,
	                                            XKB_KEY_Control_R));
	g_assert_true(gowl_hotkey_input_matches_key(&in, 29, XKB_KEY_Control_L,
	                                            XKB_KEY_Control_L));
	gowl_hotkey_input_parse("code:30", &in, NULL);
	g_assert_true(gowl_hotkey_input_matches_key(&in, 30, 0, 0));
	g_assert_false(gowl_hotkey_input_matches_key(&in, 31, XKB_KEY_a, XKB_KEY_a));
	/* a button is no key */
	gowl_hotkey_input_parse("LButton", &in, NULL);
	g_assert_false(gowl_hotkey_input_matches_key(&in, 30, XKB_KEY_a, XKB_KEY_a));
}

static void
test_modifier_keys(void)
{
	g_assert_cmpuint(gowl_hotkey_keysym_modifier(XKB_KEY_Shift_R), ==,
	                 GOWL_KEY_MOD_SHIFT);
	g_assert_cmpuint(gowl_hotkey_keysym_modifier(XKB_KEY_Super_L), ==,
	                 GOWL_KEY_MOD_LOGO);
	g_assert_cmpuint(gowl_hotkey_keysym_modifier(XKB_KEY_ISO_Level3_Shift), ==,
	                 GOWL_KEY_MOD_MOD5);
	g_assert_cmpuint(gowl_hotkey_keysym_modifier(XKB_KEY_Caps_Lock), ==, 0);
	g_assert_cmpuint(gowl_hotkey_keysym_modifier(XKB_KEY_a), ==, 0);
	g_assert_cmpint(gowl_hotkey_keysym_side(XKB_KEY_Control_L), ==, -1);
	g_assert_cmpint(gowl_hotkey_keysym_side(XKB_KEY_Alt_R), ==, 1);
	g_assert_cmpint(gowl_hotkey_keysym_side(XKB_KEY_a), ==, 0);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hotkey/spec/modifier-symbols", test_modifier_symbols);
	g_test_add_func("/hotkey/spec/sides-and-altgr", test_sides_and_altgr);
	g_test_add_func("/hotkey/spec/flags-and-up", test_flags_and_up);
	g_test_add_func("/hotkey/spec/combinations", test_combinations);
	g_test_add_func("/hotkey/spec/refusals", test_refusals);
	g_test_add_func("/hotkey/spec/gowl-notation", test_gowl_notation);
	g_test_add_func("/hotkey/spec/key-names", test_key_names);
	g_test_add_func("/hotkey/spec/buttons-and-codes", test_buttons_and_codes);
	g_test_add_func("/hotkey/spec/canonical", test_canonical);
	g_test_add_func("/hotkey/spec/matches", test_matches);
	g_test_add_func("/hotkey/spec/modifier-keys", test_modifier_keys);

	return g_test_run();
}
