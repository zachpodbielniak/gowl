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
 * gowl-hotkey-spec.c - Hotkeys as AutoHotkey (and gowl) write them.
 *
 * Two notations, told apart by their first token:
 *
 *   AutoHotkey   [*~$][<>][#!^+]...KEY[ & KEY][ UP]
 *                #!t   ~LButton   <^>!e   CapsLock & j   ^a UP
 *   gowl         [*~$]MOD+MOD+...+KEY[ UP]
 *                Super+Alt+t   LCtrl+Shift+Return
 *
 * The AutoHotkey modifier symbols are `#' Super, `!' Alt, `^' Control,
 * `+' Shift; `<' and `>' before one of them ask for the left- or
 * right-hand key, and `<^>!' is AltGr (the Level 3 shift, Mod5, on
 * Linux -- AutoHotkey's spelling of it, not its Windows meaning of
 * "left Control plus right Alt").  The symbols are read while more
 * than one character is left, so `^+' is Control and the plus key.
 *
 * Key names are AutoHotkey's (Enter, Esc, PgUp, Numpad5, LWin, AppsKey,
 * Volume_Up, LButton, WheelDown, XButton1, ...), any XKB keysym name
 * (Return, Prior, KP_5, XF86AudioRaiseVolume), a single character, or
 * `code:NN' for an evdev keycode -- the way to reach a macro pad's key
 * that has no keysym.  Nothing here touches a compositor: the module,
 * the tests and a C config all parse the same way.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "hotkey/gowl-hotkey.h"
#include "config/gowl-keybind.h"
#include "gowl-enums.h"

#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

G_DEFINE_QUARK(gowl-hotkey-error-quark, gowl_hotkey_error)

/* ── Names ─────────────────────────────────────────────────────────── */

/* An AutoHotkey key name and the XKB keysyms it stands for.  The first
 * entry for a keysym is the name it is printed with. */
typedef struct {
	const gchar *name;
	guint32      keysym;
	guint32      keysym_alt;
} KeyName;

static const KeyName key_names[] = {
	/* Neutral modifiers: either hand */
	{ "Ctrl",          XKB_KEY_Control_L,   XKB_KEY_Control_R },
	{ "Control",       XKB_KEY_Control_L,   XKB_KEY_Control_R },
	{ "Shift",         XKB_KEY_Shift_L,     XKB_KEY_Shift_R },
	{ "Alt",           XKB_KEY_Alt_L,       XKB_KEY_Alt_R },
	{ "Win",           XKB_KEY_Super_L,     XKB_KEY_Super_R },
	{ "Super",         XKB_KEY_Super_L,     XKB_KEY_Super_R },
	/* Handed modifiers */
	{ "LCtrl",         XKB_KEY_Control_L,   0 },
	{ "LControl",      XKB_KEY_Control_L,   0 },
	{ "RCtrl",         XKB_KEY_Control_R,   0 },
	{ "RControl",      XKB_KEY_Control_R,   0 },
	{ "LShift",        XKB_KEY_Shift_L,     0 },
	{ "RShift",        XKB_KEY_Shift_R,     0 },
	{ "LAlt",          XKB_KEY_Alt_L,       0 },
	{ "RAlt",          XKB_KEY_Alt_R,       0 },
	{ "LWin",          XKB_KEY_Super_L,     0 },
	{ "RWin",          XKB_KEY_Super_R,     0 },
	{ "LSuper",        XKB_KEY_Super_L,     0 },
	{ "RSuper",        XKB_KEY_Super_R,     0 },
	{ "AltGr",         XKB_KEY_ISO_Level3_Shift, XKB_KEY_Alt_R },
	/* Editing and movement */
	{ "Enter",         XKB_KEY_Return,      0 },
	{ "Return",        XKB_KEY_Return,      0 },
	{ "Escape",        XKB_KEY_Escape,      0 },
	{ "Esc",           XKB_KEY_Escape,      0 },
	{ "Space",         XKB_KEY_space,       0 },
	{ "Tab",           XKB_KEY_Tab,         0 },
	{ "Backspace",     XKB_KEY_BackSpace,   0 },
	{ "BS",            XKB_KEY_BackSpace,   0 },
	{ "Delete",        XKB_KEY_Delete,      0 },
	{ "Del",           XKB_KEY_Delete,      0 },
	{ "Insert",        XKB_KEY_Insert,      0 },
	{ "Ins",           XKB_KEY_Insert,      0 },
	{ "Home",          XKB_KEY_Home,        0 },
	{ "End",           XKB_KEY_End,         0 },
	{ "PgUp",          XKB_KEY_Prior,       0 },
	{ "PgDn",          XKB_KEY_Next,        0 },
	{ "Up",            XKB_KEY_Up,          0 },
	{ "Down",          XKB_KEY_Down,        0 },
	{ "Left",          XKB_KEY_Left,        0 },
	{ "Right",         XKB_KEY_Right,       0 },
	/* Locks and system */
	{ "CapsLock",      XKB_KEY_Caps_Lock,   0 },
	{ "ScrollLock",    XKB_KEY_Scroll_Lock, 0 },
	{ "NumLock",       XKB_KEY_Num_Lock,    0 },
	{ "PrintScreen",   XKB_KEY_Print,       0 },
	{ "Pause",         XKB_KEY_Pause,       0 },
	{ "CtrlBreak",     XKB_KEY_Break,       0 },
	{ "AppsKey",       XKB_KEY_Menu,        0 },
	{ "Help",          XKB_KEY_Help,        0 },
	{ "Sleep",         XKB_KEY_XF86Sleep,   0 },
	/* Numpad */
	{ "Numpad0",       XKB_KEY_KP_0,        XKB_KEY_KP_Insert },
	{ "Numpad1",       XKB_KEY_KP_1,        XKB_KEY_KP_End },
	{ "Numpad2",       XKB_KEY_KP_2,        XKB_KEY_KP_Down },
	{ "Numpad3",       XKB_KEY_KP_3,        XKB_KEY_KP_Next },
	{ "Numpad4",       XKB_KEY_KP_4,        XKB_KEY_KP_Left },
	{ "Numpad5",       XKB_KEY_KP_5,        XKB_KEY_KP_Begin },
	{ "Numpad6",       XKB_KEY_KP_6,        XKB_KEY_KP_Right },
	{ "Numpad7",       XKB_KEY_KP_7,        XKB_KEY_KP_Home },
	{ "Numpad8",       XKB_KEY_KP_8,        XKB_KEY_KP_Up },
	{ "Numpad9",       XKB_KEY_KP_9,        XKB_KEY_KP_Prior },
	{ "NumpadDot",     XKB_KEY_KP_Decimal,  XKB_KEY_KP_Delete },
	{ "NumpadDiv",     XKB_KEY_KP_Divide,   0 },
	{ "NumpadMult",    XKB_KEY_KP_Multiply, 0 },
	{ "NumpadAdd",     XKB_KEY_KP_Add,      0 },
	{ "NumpadSub",     XKB_KEY_KP_Subtract, 0 },
	{ "NumpadEnter",   XKB_KEY_KP_Enter,    0 },
	{ "NumpadIns",     XKB_KEY_KP_Insert,   0 },
	{ "NumpadEnd",     XKB_KEY_KP_End,      0 },
	{ "NumpadDown",    XKB_KEY_KP_Down,     0 },
	{ "NumpadPgDn",    XKB_KEY_KP_Next,     0 },
	{ "NumpadLeft",    XKB_KEY_KP_Left,     0 },
	{ "NumpadClear",   XKB_KEY_KP_Begin,    0 },
	{ "NumpadRight",   XKB_KEY_KP_Right,    0 },
	{ "NumpadHome",    XKB_KEY_KP_Home,     0 },
	{ "NumpadUp",      XKB_KEY_KP_Up,       0 },
	{ "NumpadPgUp",    XKB_KEY_KP_Prior,    0 },
	{ "NumpadDel",     XKB_KEY_KP_Delete,   0 },
	/* Media and browser keys */
	{ "Volume_Mute",       XKB_KEY_XF86AudioMute,        0 },
	{ "Volume_Down",       XKB_KEY_XF86AudioLowerVolume, 0 },
	{ "Volume_Up",         XKB_KEY_XF86AudioRaiseVolume, 0 },
	{ "Media_Next",        XKB_KEY_XF86AudioNext,        0 },
	{ "Media_Prev",        XKB_KEY_XF86AudioPrev,        0 },
	{ "Media_Stop",        XKB_KEY_XF86AudioStop,        0 },
	{ "Media_Play_Pause",  XKB_KEY_XF86AudioPlay,        XKB_KEY_XF86AudioPause },
	{ "Browser_Back",      XKB_KEY_XF86Back,             0 },
	{ "Browser_Forward",   XKB_KEY_XF86Forward,          0 },
	{ "Browser_Refresh",   XKB_KEY_XF86Refresh,          0 },
	{ "Browser_Stop",      XKB_KEY_XF86Stop,             0 },
	{ "Browser_Search",    XKB_KEY_XF86Search,           0 },
	{ "Browser_Favorites", XKB_KEY_XF86Favorites,        0 },
	{ "Browser_Home",      XKB_KEY_XF86HomePage,         0 },
	{ "Launch_Mail",       XKB_KEY_XF86Mail,             0 },
	{ "Launch_Media",      XKB_KEY_XF86AudioMedia,       0 },
	{ "Launch_App1",       XKB_KEY_XF86MyComputer,       0 },
	{ "Launch_App2",       XKB_KEY_XF86Calculator,       0 },
	{ NULL, 0, 0 }
};

/* A pointer button or wheel direction by AutoHotkey's name, or the
 * evdev one. */
typedef struct {
	const gchar         *name;
	GowlHotkeyInputKind  kind;
	guint32              code;
} ButtonName;

static const ButtonName button_names[] = {
	{ "LButton",    GOWL_HOTKEY_INPUT_BUTTON, BTN_LEFT },
	{ "RButton",    GOWL_HOTKEY_INPUT_BUTTON, BTN_RIGHT },
	{ "MButton",    GOWL_HOTKEY_INPUT_BUTTON, BTN_MIDDLE },
	{ "XButton1",   GOWL_HOTKEY_INPUT_BUTTON, BTN_SIDE },
	{ "XButton2",   GOWL_HOTKEY_INPUT_BUTTON, BTN_EXTRA },
	{ "BTN_LEFT",   GOWL_HOTKEY_INPUT_BUTTON, BTN_LEFT },
	{ "BTN_RIGHT",  GOWL_HOTKEY_INPUT_BUTTON, BTN_RIGHT },
	{ "BTN_MIDDLE", GOWL_HOTKEY_INPUT_BUTTON, BTN_MIDDLE },
	{ "BTN_SIDE",   GOWL_HOTKEY_INPUT_BUTTON, BTN_SIDE },
	{ "BTN_EXTRA",  GOWL_HOTKEY_INPUT_BUTTON, BTN_EXTRA },
	{ "BTN_FORWARD", GOWL_HOTKEY_INPUT_BUTTON, BTN_FORWARD },
	{ "BTN_BACK",   GOWL_HOTKEY_INPUT_BUTTON, BTN_BACK },
	{ "BTN_TASK",   GOWL_HOTKEY_INPUT_BUTTON, BTN_TASK },
	{ "WheelUp",    GOWL_HOTKEY_INPUT_WHEEL,  GOWL_BUTTON_WHEEL_UP },
	{ "WheelDown",  GOWL_HOTKEY_INPUT_WHEEL,  GOWL_BUTTON_WHEEL_DOWN },
	{ "WheelLeft",  GOWL_HOTKEY_INPUT_WHEEL,  GOWL_BUTTON_WHEEL_LEFT },
	{ "WheelRight", GOWL_HOTKEY_INPUT_WHEEL,  GOWL_BUTTON_WHEEL_RIGHT },
	{ NULL, GOWL_HOTKEY_INPUT_NONE, 0 }
};

/* gowl-notation modifier names.  @side: -1 left, 1 right, 0 either. */
typedef struct {
	const gchar *name;
	guint        mod;
	gint         side;
} ModName;

static const ModName mod_names[] = {
	{ "Super",   GOWL_KEY_MOD_LOGO,  0 },
	{ "Logo",    GOWL_KEY_MOD_LOGO,  0 },
	{ "Mod4",    GOWL_KEY_MOD_LOGO,  0 },
	{ "Win",     GOWL_KEY_MOD_LOGO,  0 },
	{ "LSuper",  GOWL_KEY_MOD_LOGO, -1 },
	{ "RSuper",  GOWL_KEY_MOD_LOGO,  1 },
	{ "LWin",    GOWL_KEY_MOD_LOGO, -1 },
	{ "RWin",    GOWL_KEY_MOD_LOGO,  1 },
	{ "Alt",     GOWL_KEY_MOD_ALT,   0 },
	{ "Mod1",    GOWL_KEY_MOD_ALT,   0 },
	{ "Meta",    GOWL_KEY_MOD_ALT,   0 },
	{ "LAlt",    GOWL_KEY_MOD_ALT,  -1 },
	{ "RAlt",    GOWL_KEY_MOD_ALT,   1 },
	{ "Ctrl",    GOWL_KEY_MOD_CTRL,  0 },
	{ "Control", GOWL_KEY_MOD_CTRL,  0 },
	{ "LCtrl",   GOWL_KEY_MOD_CTRL, -1 },
	{ "LControl", GOWL_KEY_MOD_CTRL, -1 },
	{ "RCtrl",   GOWL_KEY_MOD_CTRL,  1 },
	{ "RControl", GOWL_KEY_MOD_CTRL, 1 },
	{ "Shift",   GOWL_KEY_MOD_SHIFT, 0 },
	{ "LShift",  GOWL_KEY_MOD_SHIFT, -1 },
	{ "RShift",  GOWL_KEY_MOD_SHIFT, 1 },
	{ "AltGr",   GOWL_KEY_MOD_MOD5,  0 },
	{ "Mod5",    GOWL_KEY_MOD_MOD5,  0 },
	{ "Level3",  GOWL_KEY_MOD_MOD5,  0 },
	{ "Hyper",   GOWL_KEY_MOD_MOD3,  0 },
	{ "Mod3",    GOWL_KEY_MOD_MOD3,  0 },
	{ NULL, 0, 0 }
};

static const ModName *
find_mod_name(const gchar *name)
{
	guint i;

	for (i = 0; mod_names[i].name != NULL; i++)
		if (g_ascii_strcasecmp(mod_names[i].name, name) == 0)
			return &mod_names[i];
	return NULL;
}

/**
 * gowl_hotkey_keysym_modifier:
 * @keysym: an XKB keysym
 *
 * Returns: the #GowlKeyMod bit the key @keysym is (Shift_L is
 *   %GOWL_KEY_MOD_SHIFT, ISO_Level3_Shift %GOWL_KEY_MOD_MOD5), or 0 for
 *   a key that is not a modifier.  Caps Lock is a lock, not a modifier,
 *   and is 0.
 */
guint
gowl_hotkey_keysym_modifier(guint32 keysym)
{
	switch (keysym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		return GOWL_KEY_MOD_SHIFT;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		return GOWL_KEY_MOD_CTRL;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
		return GOWL_KEY_MOD_ALT;
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
		return GOWL_KEY_MOD_LOGO;
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
		return GOWL_KEY_MOD_MOD3;
	case XKB_KEY_ISO_Level3_Shift:
		return GOWL_KEY_MOD_MOD5;
	default:
		return 0;
	}
}

/**
 * gowl_hotkey_keysym_side:
 * @keysym: an XKB keysym
 *
 * Returns: -1 for a left-hand modifier key, 1 for a right-hand one
 *   (ISO_Level3_Shift, usually the right Alt key, counts as right),
 *   0 otherwise
 */
gint
gowl_hotkey_keysym_side(guint32 keysym)
{
	switch (keysym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Control_L:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Super_L:
	case XKB_KEY_Hyper_L:
		return -1;
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_ISO_Level3_Shift:
		return 1;
	default:
		return 0;
	}
}

/* ── One input ─────────────────────────────────────────────────────── */

/**
 * gowl_hotkey_input_parse:
 * @name: a key, button or wheel name: `a', `Enter', `F13', `LButton',
 *   `WheelUp', `XF86AudioMute', `code:183'
 * @input: (out caller-allocates): where to put it
 * @error: return location for an error
 *
 * Reads one input the way a hotkey, a remap target or a Send `{Name}'
 * names it.
 *
 * Returns: %TRUE when @name is an input
 */
gboolean
gowl_hotkey_input_parse(
	const gchar      *name,
	GowlHotkeyInput  *input,
	GError          **error
){
	g_autofree gchar *s = NULL;
	guint i;
	xkb_keysym_t sym;

	g_return_val_if_fail(name != NULL, FALSE);
	g_return_val_if_fail(input != NULL, FALSE);

	memset(input, 0, sizeof *input);
	s = g_strstrip(g_strdup(name));
	if (*s == '\0') {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_SYNTAX,
		            "no key named");
		return FALSE;
	}

	/* One character: the key that types it */
	if (g_utf8_validate(s, -1, NULL) && g_utf8_strlen(s, -1) == 1) {
		sym = xkb_utf32_to_keysym(g_utf8_get_char(s));
		if (sym == XKB_KEY_NoSymbol) {
			g_set_error(error, GOWL_HOTKEY_ERROR,
			            GOWL_HOTKEY_ERROR_UNKNOWN_KEY,
			            "no key types `%s'", s);
			return FALSE;
		}
		input->kind = GOWL_HOTKEY_INPUT_KEY;
		input->keysym = xkb_keysym_to_lower(sym);
		return TRUE;
	}

	/* code:NN -- a raw evdev keycode */
	if (g_ascii_strncasecmp(s, "code:", 5) == 0) {
		guint64 code;

		if (!g_ascii_string_to_unsigned(s + 5, 10, 1, KEY_MAX, &code, NULL)) {
			g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_SYNTAX,
			            "`%s': code: takes an evdev keycode, 1..%d",
			            s, KEY_MAX);
			return FALSE;
		}
		input->kind = GOWL_HOTKEY_INPUT_KEY;
		input->keycode = (guint32)code;
		return TRUE;
	}

	for (i = 0; button_names[i].name != NULL; i++)
		if (g_ascii_strcasecmp(button_names[i].name, s) == 0) {
			input->kind = button_names[i].kind;
			input->button = button_names[i].code;
			return TRUE;
		}

	/* F1..F35 directly: XKB knows them, but AutoHotkey users expect
	 * F13 and up to work on any layout */
	for (i = 0; key_names[i].name != NULL; i++)
		if (g_ascii_strcasecmp(key_names[i].name, s) == 0) {
			input->kind = GOWL_HOTKEY_INPUT_KEY;
			input->keysym = key_names[i].keysym;
			input->keysym_alt = key_names[i].keysym_alt;
			return TRUE;
		}

	/* Exact first, so `Prior' is never read as some other keysym that
	 * merely matches without case */
	sym = xkb_keysym_from_name(s, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(s, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_UNKNOWN_KEY,
		            "unknown key `%s' (an AutoHotkey name such as Enter or "
		            "LButton, an XKB keysym name, one character, or "
		            "code:NN)", s);
		return FALSE;
	}
	input->kind = GOWL_HOTKEY_INPUT_KEY;
	input->keysym = xkb_keysym_to_lower(sym);
	return TRUE;
}

/**
 * gowl_hotkey_input_to_string:
 * @input: an input
 *
 * Returns: (transfer full): its name, the AutoHotkey one where there
 *   is one -- `Enter', `LButton', `a', `code:183'
 */
gchar *
gowl_hotkey_input_to_string(const GowlHotkeyInput *input)
{
	guint i;
	gchar buf[64];
	guint32 cp;

	g_return_val_if_fail(input != NULL, NULL);

	switch (input->kind) {
	case GOWL_HOTKEY_INPUT_BUTTON:
	case GOWL_HOTKEY_INPUT_WHEEL:
		for (i = 0; button_names[i].name != NULL; i++)
			if (button_names[i].kind == input->kind
			    && button_names[i].code == input->button)
				return g_strdup(button_names[i].name);
		return g_strdup_printf("button:%u", input->button);
	case GOWL_HOTKEY_INPUT_KEY:
		if (input->keycode != 0)
			return g_strdup_printf("code:%u", input->keycode);
		for (i = 0; key_names[i].name != NULL; i++)
			if (key_names[i].keysym == input->keysym
			    && key_names[i].keysym_alt == input->keysym_alt)
				return g_strdup(key_names[i].name);
		/* A printable character prints as itself */
		cp = xkb_keysym_to_utf32(input->keysym);
		if (cp > 0x20 && cp != 0x7f) {
			gint n = g_unichar_to_utf8((gunichar)cp, buf);

			buf[n] = '\0';
			return g_strdup(buf);
		}
		if (xkb_keysym_get_name(input->keysym, buf, sizeof buf) > 0)
			return g_strdup(buf);
		return g_strdup_printf("0x%x", input->keysym);
	case GOWL_HOTKEY_INPUT_NONE:
	default:
		return g_strdup("");
	}
}

/**
 * gowl_hotkey_input_matches_key:
 * @input: a key input
 * @keycode: the evdev keycode of a key event
 * @keysym: its keysym with the modifiers applied (Shift+a is `A')
 * @base_keysym: its keysym with none (Shift+a is `a', Shift+1 is `1')
 *
 * Returns: %TRUE when the event is this key.  A keysym matches either
 *   form without case, so `+a' and `A' are both the a key.
 */
gboolean
gowl_hotkey_input_matches_key(
	const GowlHotkeyInput *input,
	guint32                keycode,
	guint32                keysym,
	guint32                base_keysym
){
	guint32 lk;
	guint32 lb;

	g_return_val_if_fail(input != NULL, FALSE);
	if (input->kind != GOWL_HOTKEY_INPUT_KEY)
		return FALSE;
	if (input->keycode != 0)
		return input->keycode == keycode;
	lk = xkb_keysym_to_lower(keysym);
	lb = xkb_keysym_to_lower(base_keysym);
	if (lb == input->keysym || lk == input->keysym)
		return TRUE;
	return input->keysym_alt != 0
	       && (lb == input->keysym_alt || lk == input->keysym_alt);
}

/* ── A whole hotkey ────────────────────────────────────────────────── */

/* Strips one trailing ` UP' (any case).  Returns TRUE when it did. */
static gboolean
strip_up(gchar *s)
{
	gsize n = strlen(s);

	if (n > 3 && g_ascii_strcasecmp(s + n - 3, " up") == 0) {
		s[n - 3] = '\0';
		g_strchomp(s);
		return TRUE;
	}
	return FALSE;
}

/* `CapsLock & j': two plain names, no modifiers (AutoHotkey's rule:
 * a combination is its own wildcard). */
static gboolean
parse_combination(
	const gchar     *text,
	const gchar     *amp,
	GowlHotkeySpec  *spec,
	GError         **error
){
	g_autofree gchar *left = NULL;
	g_autofree gchar *right = NULL;

	left = g_strstrip(g_strndup(text, (gsize)(amp - text)));
	right = g_strstrip(g_strdup(amp + 3));
	if (strstr(right, " & ") != NULL) {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_UNSUPPORTED,
		            "`%s': a combination is two keys (A & B); three-key "
		            "combinations are not supported -- use a mode, or a "
		            "context on a two-key one", text);
		return FALSE;
	}
	if (*left == '\0' || *right == '\0') {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_SYNTAX,
		            "`%s': a combination needs a key on both sides of &",
		            text);
		return FALSE;
	}
	if (!gowl_hotkey_input_parse(left, &spec->prefix, error)
	    || !gowl_hotkey_input_parse(right, &spec->trigger, error))
		return FALSE;
	if (spec->prefix.kind == GOWL_HOTKEY_INPUT_WHEEL) {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_UNSUPPORTED,
		            "`%s': a wheel cannot be held, so it cannot be a "
		            "prefix", text);
		return FALSE;
	}
	spec->flags |= GOWL_HOTKEY_FLAG_WILDCARD;
	return TRUE;
}

/* `Super+Alt+t': is the first token a modifier name?  Then it is gowl
 * notation (a one-character first token is AutoHotkey's `+'). */
static gboolean
is_gowl_notation(const gchar *s)
{
	const gchar *plus = strchr(s, '+');
	g_autofree gchar *first = NULL;

	if (plus == NULL || plus == s)
		return FALSE;
	first = g_strstrip(g_strndup(s, (gsize)(plus - s)));
	return strlen(first) > 1 && find_mod_name(first) != NULL;
}

static gboolean
parse_gowl_notation(
	const gchar     *s,
	GowlHotkeySpec  *spec,
	GError         **error
){
	g_auto(GStrv) tok = NULL;
	guint n;
	guint i;
	const gchar *key;

	tok = g_strsplit(s, "+", -1);
	n = g_strv_length(tok);
	/* `Super++' is Super and the plus key */
	if (n >= 3 && *g_strstrip(tok[n - 1]) == '\0'
	    && *g_strstrip(tok[n - 2]) == '\0') {
		key = "plus";
		n -= 2;
	} else {
		key = g_strstrip(tok[n - 1]);
		n -= 1;
	}
	for (i = 0; i < n; i++) {
		const ModName *m = find_mod_name(g_strstrip(tok[i]));

		if (m == NULL) {
			g_set_error(error, GOWL_HOTKEY_ERROR,
			            GOWL_HOTKEY_ERROR_UNKNOWN_KEY,
			            "`%s': unknown modifier `%s' (Super, Alt, Ctrl, "
			            "Shift, AltGr, Hyper, or LCtrl/RAlt/... for one "
			            "hand)", s, tok[i]);
			return FALSE;
		}
		spec->mods |= m->mod;
		if (m->side < 0)
			spec->left |= m->mod;
		else if (m->side > 0)
			spec->right |= m->mod;
	}
	return gowl_hotkey_input_parse(key, &spec->trigger, error);
}

static gboolean
parse_ahk_notation(
	const gchar     *s,
	GowlHotkeySpec  *spec,
	GError         **error
){
	gint side = 0;
	gsize n = strlen(s);
	gsize i = 0;

	/* Symbols while more than one character is left: the last one is
	 * always the key (`^+' is Control and plus) */
	while (n - i > 1) {
		guint mod = 0;
		gchar c = s[i];

		if (strncmp(s + i, "<^>!", 4) == 0 && n - i > 4) {
			spec->mods |= GOWL_KEY_MOD_MOD5;
			i += 4;
			continue;
		}
		switch (c) {
		case '#': mod = GOWL_KEY_MOD_LOGO;  break;
		case '!': mod = GOWL_KEY_MOD_ALT;   break;
		case '^': mod = GOWL_KEY_MOD_CTRL;  break;
		case '+': mod = GOWL_KEY_MOD_SHIFT; break;
		case '<': side = -1; i++; continue;
		case '>': side = 1;  i++; continue;
		case '*': spec->flags |= GOWL_HOTKEY_FLAG_WILDCARD;     i++; continue;
		case '~': spec->flags |= GOWL_HOTKEY_FLAG_PASSTHROUGH;  i++; continue;
		case '$': spec->flags |= GOWL_HOTKEY_FLAG_NO_SYNTHETIC; i++; continue;
		default:
			break;
		}
		if (mod == 0)
			break;
		spec->mods |= mod;
		if (side < 0)
			spec->left |= mod;
		else if (side > 0)
			spec->right |= mod;
		side = 0;
		i++;
	}
	if (side != 0) {
		/* `<' or `>' with nothing to apply to: it was the key */
		i--;
	}
	return gowl_hotkey_input_parse(s + i, &spec->trigger, error);
}

/**
 * gowl_hotkey_spec_parse:
 * @text: a hotkey: `#!t', `~LButton', `CapsLock & j', `^a UP',
 *   `<^>!e', `*F13', `Super+Alt+t'
 * @spec: (out caller-allocates): where to put it
 * @error: return location for an error, which says what is wrong and
 *   with which part
 *
 * Reads a hotkey in AutoHotkey's notation or gowl's (see the top of
 * this file).
 *
 * Returns: %TRUE when @text is a hotkey
 */
gboolean
gowl_hotkey_spec_parse(
	const gchar     *text,
	GowlHotkeySpec  *spec,
	GError         **error
){
	g_autofree gchar *s = NULL;
	const gchar *p;
	const gchar *amp;
	gboolean ok;

	g_return_val_if_fail(text != NULL, FALSE);
	g_return_val_if_fail(spec != NULL, FALSE);

	memset(spec, 0, sizeof *spec);
	s = g_strstrip(g_strdup(text));
	if (*s == '\0') {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_SYNTAX,
		            "an empty hotkey");
		return FALSE;
	}
	if (strip_up(s))
		spec->flags |= GOWL_HOTKEY_FLAG_ON_RELEASE;

	/* Leading flags apply to every notation */
	p = s;
	while (strlen(p) > 1 && (*p == '*' || *p == '~' || *p == '$')) {
		if (*p == '*')
			spec->flags |= GOWL_HOTKEY_FLAG_WILDCARD;
		else if (*p == '~')
			spec->flags |= GOWL_HOTKEY_FLAG_PASSTHROUGH;
		else
			spec->flags |= GOWL_HOTKEY_FLAG_NO_SYNTHETIC;
		p++;
	}

	amp = strstr(p, " & ");
	if (amp != NULL)
		ok = parse_combination(p, amp, spec, error);
	else if (is_gowl_notation(p))
		ok = parse_gowl_notation(p, spec, error);
	else
		ok = parse_ahk_notation(p, spec, error);
	if (!ok) {
		if (error != NULL && *error != NULL
		    && !g_str_has_prefix((*error)->message, "`"))
			g_prefix_error(error, "`%s': ", text);
		return FALSE;
	}
	if (spec->trigger.kind == GOWL_HOTKEY_INPUT_WHEEL
	    && (spec->flags & GOWL_HOTKEY_FLAG_ON_RELEASE)) {
		g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_UNSUPPORTED,
		            "`%s': a wheel notch has no release, so it cannot be "
		            "an UP hotkey", text);
		return FALSE;
	}
	return TRUE;
}

/* The AutoHotkey symbols for @mods, with sides. */
static void
append_mods(
	GString              *out,
	const GowlHotkeySpec *spec
){
	static const struct { guint mod; gchar sym; } order[] = {
		{ GOWL_KEY_MOD_LOGO,  '#' },
		{ GOWL_KEY_MOD_ALT,   '!' },
		{ GOWL_KEY_MOD_CTRL,  '^' },
		{ GOWL_KEY_MOD_SHIFT, '+' }
	};
	guint i;

	if (spec->mods & GOWL_KEY_MOD_MOD5)
		g_string_append(out, "<^>!");
	for (i = 0; i < G_N_ELEMENTS(order); i++) {
		if (!(spec->mods & order[i].mod))
			continue;
		if (spec->left & order[i].mod)
			g_string_append_c(out, '<');
		else if (spec->right & order[i].mod)
			g_string_append_c(out, '>');
		g_string_append_c(out, order[i].sym);
	}
}

/* gowl notation's modifier names, with a hand where one is asked for:
 * `LCtrl+Hyper+'. */
static void
append_gowl_mods(
	GString              *out,
	const GowlHotkeySpec *spec
){
	static const struct { guint mod; const gchar *name; } order[] = {
		{ GOWL_KEY_MOD_LOGO,  "Super" },
		{ GOWL_KEY_MOD_ALT,   "Alt" },
		{ GOWL_KEY_MOD_CTRL,  "Ctrl" },
		{ GOWL_KEY_MOD_SHIFT, "Shift" },
		{ GOWL_KEY_MOD_MOD5,  "AltGr" },
		{ GOWL_KEY_MOD_MOD3,  "Hyper" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(order); i++) {
		if (!(spec->mods & order[i].mod))
			continue;
		if (spec->left & order[i].mod)
			g_string_append_c(out, 'L');
		else if (spec->right & order[i].mod)
			g_string_append_c(out, 'R');
		g_string_append_printf(out, "%s+", order[i].name);
	}
}

/**
 * gowl_hotkey_spec_to_string:
 * @spec: a hotkey
 *
 * Returns: (transfer full): it in canonical AutoHotkey notation --
 *   flags, then modifiers in the order `#!^+', then the key -- so two
 *   spellings of one hotkey print the same.  A Hyper (Mod3) hotkey,
 *   which AutoHotkey has no symbol for, prints in gowl notation.
 */
gchar *
gowl_hotkey_spec_to_string(const GowlHotkeySpec *spec)
{
	GString *out;
	g_autofree gchar *key = NULL;

	g_return_val_if_fail(spec != NULL, NULL);

	out = g_string_new(NULL);
	if (spec->flags & GOWL_HOTKEY_FLAG_WILDCARD
	    && spec->prefix.kind == GOWL_HOTKEY_INPUT_NONE)
		g_string_append_c(out, '*');
	if (spec->flags & GOWL_HOTKEY_FLAG_PASSTHROUGH)
		g_string_append_c(out, '~');
	if (spec->flags & GOWL_HOTKEY_FLAG_NO_SYNTHETIC)
		g_string_append_c(out, '$');
	key = gowl_hotkey_input_to_string(&spec->trigger);
	if (spec->prefix.kind != GOWL_HOTKEY_INPUT_NONE) {
		g_autofree gchar *pre = gowl_hotkey_input_to_string(&spec->prefix);

		g_string_append_printf(out, "%s & %s", pre, key);
	} else if (spec->mods & GOWL_KEY_MOD_MOD3) {
		append_gowl_mods(out, spec);
		g_string_append(out, key);
	} else {
		append_mods(out, spec);
		g_string_append(out, key);
	}
	if (spec->flags & GOWL_HOTKEY_FLAG_ON_RELEASE)
		g_string_append(out, " UP");
	return g_string_free(out, FALSE);
}

static gboolean
input_equal(
	const GowlHotkeyInput *a,
	const GowlHotkeyInput *b
){
	return a->kind == b->kind && a->keysym == b->keysym
	       && a->keysym_alt == b->keysym_alt && a->keycode == b->keycode
	       && a->button == b->button;
}

/**
 * gowl_hotkey_spec_equal:
 * @a: a hotkey
 * @b: another
 *
 * Returns: %TRUE when they are the same hotkey, however each was spelt
 */
gboolean
gowl_hotkey_spec_equal(
	const GowlHotkeySpec *a,
	const GowlHotkeySpec *b
){
	g_return_val_if_fail(a != NULL && b != NULL, FALSE);
	return input_equal(&a->trigger, &b->trigger)
	       && input_equal(&a->prefix, &b->prefix)
	       && a->mods == b->mods && a->left == b->left
	       && a->right == b->right && a->flags == b->flags;
}
