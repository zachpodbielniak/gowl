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
 * gowl-hotkey-send.c - Parsing AutoHotkey's Send syntax.
 *
 * Outside braces, `^ ! + #' are Ctrl, Alt, Shift and Super for the next
 * key only; a newline is Enter and a tab is Tab; every other character
 * is typed.  Inside braces is one key name with an optional count or
 * `down'/`up', or one of the special forms in the header.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "gowl-hotkey-send.h"
#include "hotkey/gowl-hotkey.h"
#include "gowl-enums.h"

#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define HK_SEND_MAX_REPEAT (500)
#define HK_SEND_MAX_SLEEP  (10000)

static void
add_event(
	HkSendPlan  *plan,
	HkSendEvent *ev
){
	g_array_append_val(plan->events, *ev);
}

static void
add_char(
	HkSendPlan *plan,
	gunichar    ch,
	guint       mods
){
	HkSendEvent ev;

	memset(&ev, 0, sizeof ev);
	if (ch == '\n' || ch == '\t') {
		ev.kind = HK_SEND_KEY;
		ev.keysym = ch == '\n' ? XKB_KEY_Return : XKB_KEY_Tab;
	} else if (mods != 0) {
		/* ^c: the key that types c, with Ctrl -- a chord, not text */
		ev.kind = HK_SEND_KEY;
		ev.keysym = xkb_utf32_to_keysym(ch);
	} else {
		ev.kind = HK_SEND_CHAR;
		ev.ch = ch;
	}
	ev.mods = mods;
	add_event(plan, &ev);
}

/* Adds literal text, joined to a TEXT event right before it. */
static void
add_text(
	HkSendPlan  *plan,
	const gchar *text
){
	HkSendEvent ev;

	if (*text == '\0')
		return;
	if (plan->events->len > 0) {
		HkSendEvent *last = &g_array_index(plan->events, HkSendEvent,
		                                   plan->events->len - 1);

		if (last->kind == HK_SEND_TEXT) {
			gchar *joined = g_strconcat(last->text, text, NULL);

			g_free(last->text);
			last->text = joined;
			return;
		}
	}
	memset(&ev, 0, sizeof ev);
	ev.kind = HK_SEND_TEXT;
	ev.text = g_strdup(text);
	add_event(plan, &ev);
}

static void
plan_clear_event(gpointer p)
{
	HkSendEvent *ev = p;

	g_free(ev->text);
}

static HkSendPlan *
plan_new(void)
{
	HkSendPlan *plan = g_new0(HkSendPlan, 1);

	plan->events = g_array_new(FALSE, TRUE, sizeof(HkSendEvent));
	g_array_set_clear_func(plan->events, plan_clear_event);
	return plan;
}

void
hk_send_plan_free(HkSendPlan *plan)
{
	if (plan == NULL)
		return;
	g_array_unref(plan->events);
	g_free(plan);
}

/* `{...}': @inner is what is between the braces. */
static gboolean
parse_brace(
	HkSendPlan   *plan,
	const gchar  *inner,
	guint         mods,
	GError      **error
){
	g_auto(GStrv) parts = NULL;
	GowlHotkeyInput in;
	const gchar *name;
	gint updown = 0;
	guint repeat = 1;
	guint i;
	HkSendEvent ev;

	/* A single character, even a space or a symbol: {!} {^} {{} {}} */
	if (g_utf8_strlen(inner, -1) == 1) {
		add_char(plan, g_utf8_get_char(inner), mods);
		return TRUE;
	}
	if (g_ascii_strncasecmp(inner, "U+", 2) == 0) {
		guint64 cp;

		if (!g_ascii_string_to_unsigned(inner + 2, 16, 1, 0x10FFFF, &cp,
		                                NULL)) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			            "{%s}: U+ takes a hexadecimal code point", inner);
			return FALSE;
		}
		add_char(plan, (gunichar)cp, mods);
		return TRUE;
	}

	parts = g_strsplit(inner, " ", 2);
	name = parts[0];

	/* {Click} / {Click right}: a tap of a button */
	if (g_ascii_strcasecmp(name, "Click") == 0) {
		const gchar *which = parts[1] != NULL ? g_strstrip(parts[1]) : "left";

		memset(&ev, 0, sizeof ev);
		ev.kind = HK_SEND_BUTTON;
		ev.mods = mods;
		if (g_ascii_strcasecmp(which, "left") == 0)
			ev.button = BTN_LEFT;
		else if (g_ascii_strcasecmp(which, "right") == 0)
			ev.button = BTN_RIGHT;
		else if (g_ascii_strcasecmp(which, "middle") == 0)
			ev.button = BTN_MIDDLE;
		else if (gowl_hotkey_input_parse(which, &in, NULL)
		         && in.kind == GOWL_HOTKEY_INPUT_BUTTON)
			ev.button = in.button;
		else {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			            "{%s}: Click takes left, right, middle or a button "
			            "name", inner);
			return FALSE;
		}
		add_event(plan, &ev);
		return TRUE;
	}
	if (parts[1] != NULL) {
		gchar *arg = g_strstrip(parts[1]);
		guint64 n;

		if (g_ascii_strcasecmp(arg, "down") == 0)
			updown = 1;
		else if (g_ascii_strcasecmp(arg, "up") == 0)
			updown = -1;
		else if (g_ascii_strcasecmp(name, "Sleep") == 0
		         && g_ascii_string_to_unsigned(arg, 10, 0, HK_SEND_MAX_SLEEP,
		                                       &n, NULL)) {
			memset(&ev, 0, sizeof ev);
			ev.kind = HK_SEND_SLEEP;
			ev.ms = (guint)n;
			add_event(plan, &ev);
			return TRUE;
		} else if (g_ascii_string_to_unsigned(arg, 10, 0, HK_SEND_MAX_REPEAT,
		                                      &n, NULL)) {
			repeat = (guint)n;
		} else {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			            "{%s}: after the key name comes a count (0..%d), "
			            "down or up", inner, HK_SEND_MAX_REPEAT);
			return FALSE;
		}
	}
	if (g_ascii_strcasecmp(name, "Sleep") == 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "{Sleep}: give the milliseconds (0..%d), {Sleep 100}",
		            HK_SEND_MAX_SLEEP);
		return FALSE;
	}

	if (!gowl_hotkey_input_parse(name, &in, error)) {
		g_prefix_error(error, "{%s}: ", inner);
		return FALSE;
	}
	if (in.kind == GOWL_HOTKEY_INPUT_WHEEL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "{%s}: the wheel is the mouse action's (wheel:)", inner);
		return FALSE;
	}
	for (i = 0; i < (updown != 0 ? 1u : repeat); i++) {
		memset(&ev, 0, sizeof ev);
		ev.updown = updown;
		ev.mods = updown == 0 ? mods : 0;
		if (in.kind == GOWL_HOTKEY_INPUT_BUTTON) {
			ev.kind = HK_SEND_BUTTON;
			ev.button = in.button;
		} else {
			ev.kind = HK_SEND_KEY;
			ev.keysym = in.keysym;
			ev.keycode = in.keycode;
		}
		add_event(plan, &ev);
	}
	return TRUE;
}

/**
 * hk_send_parse: (skip)
 * @keys: keys in AutoHotkey's Send syntax
 * @error: what is wrong, and where
 *
 * Returns: (transfer full) (nullable): the plan
 */
HkSendPlan *
hk_send_parse(
	const gchar  *keys,
	GError      **error
){
	g_autoptr(HkSendPlan) plan = NULL;
	const gchar *p;
	guint mods = 0;

	g_return_val_if_fail(keys != NULL, NULL);
	if (!g_utf8_validate(keys, -1, NULL)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "the keys are not UTF-8");
		return NULL;
	}
	plan = plan_new();
	p = keys;
	while (*p != '\0') {
		gunichar c = g_utf8_get_char(p);

		switch (c) {
		case '^': mods |= GOWL_KEY_MOD_CTRL;  p++; continue;
		case '!': mods |= GOWL_KEY_MOD_ALT;   p++; continue;
		case '+': mods |= GOWL_KEY_MOD_SHIFT; p++; continue;
		case '#': mods |= GOWL_KEY_MOD_LOGO;  p++; continue;
		case '\r': p++; continue;
		default:
			break;
		}
		if (c == '{') {
			const gchar *close;
			g_autofree gchar *inner = NULL;

			/* {{} and {}} are braces themselves */
			if (strncmp(p, "{{}", 3) == 0 || strncmp(p, "{}}", 3) == 0) {
				add_char(plan, p[1] == '{' ? '{' : '}', mods);
				mods = 0;
				p += 3;
				continue;
			}
			close = strchr(p + 1, '}');
			if (close == NULL) {
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				            "`%s': a { with no }", p);
				return NULL;
			}
			inner = g_strndup(p + 1, (gsize)(close - p - 1));
			p = close + 1;
			if (g_ascii_strcasecmp(inner, "Blind") == 0) {
				plan->blind = TRUE;
				continue;
			}
			if (g_ascii_strcasecmp(inner, "Raw") == 0) {
				/* The rest, character by character, nothing special */
				while (*p != '\0') {
					add_char(plan, g_utf8_get_char(p), 0);
					p = g_utf8_next_char(p);
				}
				break;
			}
			if (g_ascii_strcasecmp(inner, "Text") == 0) {
				add_text(plan, p);
				break;
			}
			if (*inner == '\0') {
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				            "an empty {}");
				return NULL;
			}
			if (!parse_brace(plan, inner, mods, error))
				return NULL;
			mods = 0;
			continue;
		}
		add_char(plan, c, mods);
		mods = 0;
		p = g_utf8_next_char(p);
	}
	if (mods != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "`%s' ends in a modifier with no key after it (for the "
		            "character itself write {^} {!} {+} {#})", keys);
		return NULL;
	}
	return g_steal_pointer(&plan);
}

/**
 * hk_send_text_plan: (skip)
 * @text: literal text
 *
 * SendText: every character typed as itself, a newline as Enter.
 *
 * Returns: (transfer full): the plan
 */
HkSendPlan *
hk_send_text_plan(const gchar *text)
{
	HkSendPlan *plan = plan_new();
	const gchar *p;

	g_return_val_if_fail(text != NULL, plan);
	if (!g_utf8_validate(text, -1, NULL))
		return plan;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
		if (*p != '\r')
			add_char(plan, g_utf8_get_char(p), 0);
	return plan;
}

/**
 * hk_send_describe: (skip)
 * @plan: a plan
 *
 * Returns: (transfer full): one line, for tests and `hotkey-send --dry'
 */
gchar *
hk_send_describe(const HkSendPlan *plan)
{
	GString *out = g_string_new(plan->blind ? "blind " : "");
	guint i;

	for (i = 0; i < plan->events->len; i++) {
		const HkSendEvent *ev = &g_array_index(plan->events, HkSendEvent, i);
		gchar name[64];

		if (i > 0)
			g_string_append_c(out, ' ');
		if (ev->mods & GOWL_KEY_MOD_LOGO)
			g_string_append(out, "Super+");
		if (ev->mods & GOWL_KEY_MOD_CTRL)
			g_string_append(out, "Ctrl+");
		if (ev->mods & GOWL_KEY_MOD_ALT)
			g_string_append(out, "Alt+");
		if (ev->mods & GOWL_KEY_MOD_SHIFT)
			g_string_append(out, "Shift+");
		switch (ev->kind) {
		case HK_SEND_KEY:
			if (ev->keycode != 0)
				g_string_append_printf(out, "code:%u", ev->keycode);
			else if (xkb_keysym_get_name(ev->keysym, name, sizeof name) > 0)
				g_string_append(out, name);
			break;
		case HK_SEND_CHAR: {
			gchar buf[8];
			gint n = g_unichar_to_utf8(ev->ch, buf);

			buf[n] = '\0';
			g_string_append_printf(out, "'%s'", buf);
			break;
		}
		case HK_SEND_TEXT:
			g_string_append_printf(out, "text:\"%s\"", ev->text);
			break;
		case HK_SEND_BUTTON:
			g_string_append_printf(out, "button:%u", ev->button);
			break;
		case HK_SEND_SLEEP:
			g_string_append_printf(out, "sleep:%u", ev->ms);
			break;
		default:
			break;
		}
		if (ev->updown > 0)
			g_string_append(out, ":down");
		else if (ev->updown < 0)
			g_string_append(out, ":up");
	}
	return g_string_free(out, FALSE);
}
