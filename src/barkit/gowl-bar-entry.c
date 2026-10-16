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

#include "barkit/gowl-bar-entry.h"
#include "gowl-enums.h"

#include <string.h>
#include <xkbcommon/xkbcommon.h>

/**
 * SECTION:gowl-bar-entry
 * @title: GowlBarEntry
 * @short_description: the editing rules of a panel text field
 *
 * The bar host keeps one of these per text field while a panel is
 * open.  Everything a key can do to the text is decided here, with no
 * compositor in sight, so the rules can be tested exhaustively.
 */

struct _GowlBarEntry {
	GString *text;
	gsize    cursor;   /* byte offset, on a character boundary */
};

/**
 * gowl_bar_entry_new:
 * @text: (nullable): the initial text
 *
 * Returns: (transfer full): a new entry with the cursor at the end
 */
GowlBarEntry *
gowl_bar_entry_new(const gchar *text)
{
	GowlBarEntry *self;

	self = g_new0(GowlBarEntry, 1);
	self->text = g_string_new(NULL);
	gowl_bar_entry_set_text(self, text);
	return self;
}

/**
 * gowl_bar_entry_free:
 * @self: (nullable) (transfer full): an entry
 */
void
gowl_bar_entry_free(GowlBarEntry *self)
{
	if (self == NULL)
		return;
	g_string_free(self->text, TRUE);
	g_free(self);
}

/**
 * gowl_bar_entry_get_text:
 * @self: an entry
 *
 * Returns: (transfer none): the text
 */
const gchar *
gowl_bar_entry_get_text(const GowlBarEntry *self)
{
	g_return_val_if_fail(self != NULL, "");
	return self->text->str;
}

/**
 * gowl_bar_entry_get_cursor:
 * @self: an entry
 *
 * Returns: the cursor's byte offset
 */
gsize
gowl_bar_entry_get_cursor(const GowlBarEntry *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->cursor;
}

void
gowl_bar_entry_set_text(GowlBarEntry *self, const gchar *text)
{
	g_return_if_fail(self != NULL);

	g_string_truncate(self->text, 0);
	/* Text the plugin hands over is trusted to be UTF-8 no more than
	   typed text is: a field that cannot be walked by character
	   cannot place a cursor either. */
	if (text != NULL && g_utf8_validate(text, -1, NULL))
		g_string_append(self->text, text);
	self->cursor = self->text->len;
}

void
gowl_bar_entry_set_cursor(GowlBarEntry *self, gsize cursor)
{
	const gchar *start, *at;

	g_return_if_fail(self != NULL);

	if (cursor > self->text->len)
		cursor = self->text->len;

	/* Back up to the start of the character @cursor landed inside. */
	start = self->text->str;
	at = start + cursor;
	if (cursor < self->text->len && (((guchar)*at) & 0xC0) == 0x80)
		at = g_utf8_find_prev_char(start, at);
	self->cursor = (at != NULL) ? (gsize)(at - start) : 0;
}

gboolean
gowl_bar_entry_insert(GowlBarEntry *self, const gchar *text,
                      gboolean multiline)
{
	g_autofree gchar *clean = NULL;
	gchar *c;

	g_return_val_if_fail(self != NULL, FALSE);

	if (text == NULL || text[0] == '\0')
		return FALSE;
	if (!g_utf8_validate(text, -1, NULL))
		return FALSE;

	clean = g_strdup(text);
	for (c = clean; *c != '\0'; c++) {
		/* Control characters other than a kept newline become
		   spaces: a pasted tab or carriage return has no business
		   in a one-line field, and a NUL cannot get here. */
		if (*c == '\n' && multiline)
			continue;
		if (*c == '\r' && multiline) {
			*c = ' ';
			continue;
		}
		if ((guchar)*c < 0x20 || *c == 0x7f)
			*c = ' ';
	}

	g_string_insert(self->text, (gssize)self->cursor, clean);
	self->cursor += strlen(clean);
	return TRUE;
}

gboolean
gowl_bar_entry_backspace(GowlBarEntry *self)
{
	const gchar *prev;
	gsize at;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->cursor == 0)
		return FALSE;
	prev = g_utf8_find_prev_char(self->text->str,
	                             self->text->str + self->cursor);
	at = (prev != NULL) ? (gsize)(prev - self->text->str) : 0;
	g_string_erase(self->text, (gssize)at, (gssize)(self->cursor - at));
	self->cursor = at;
	return TRUE;
}

gboolean
gowl_bar_entry_delete(GowlBarEntry *self)
{
	const gchar *next;
	gsize end;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->cursor >= self->text->len)
		return FALSE;
	next = g_utf8_next_char(self->text->str + self->cursor);
	end = (gsize)(next - self->text->str);
	g_string_erase(self->text, (gssize)self->cursor,
	               (gssize)(end - self->cursor));
	return TRUE;
}

/*
 * Ctrl+W: delete back to the start of the previous word, the way a
 * shell does --- trailing spaces first, then the word itself.
 */
gboolean
gowl_bar_entry_kill_word(GowlBarEntry *self)
{
	const gchar *start, *p, *prev;
	gsize at;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->cursor == 0)
		return FALSE;

	start = self->text->str;
	p = start + self->cursor;

	while (p > start) {
		prev = g_utf8_find_prev_char(start, p);
		if (prev == NULL || !g_unichar_isspace(g_utf8_get_char(prev)))
			break;
		p = prev;
	}
	while (p > start) {
		prev = g_utf8_find_prev_char(start, p);
		if (prev == NULL || g_unichar_isspace(g_utf8_get_char(prev)))
			break;
		p = prev;
	}

	at = (gsize)(p - start);
	g_string_erase(self->text, (gssize)at, (gssize)(self->cursor - at));
	self->cursor = at;
	return TRUE;
}

/* Ctrl+U: everything before the cursor goes. */
gboolean
gowl_bar_entry_kill_line(GowlBarEntry *self)
{
	g_return_val_if_fail(self != NULL, FALSE);

	if (self->cursor == 0)
		return FALSE;
	g_string_erase(self->text, 0, (gssize)self->cursor);
	self->cursor = 0;
	return TRUE;
}

static gboolean
entry_left(GowlBarEntry *self)
{
	const gchar *prev;

	if (self->cursor == 0)
		return FALSE;
	prev = g_utf8_find_prev_char(self->text->str,
	                             self->text->str + self->cursor);
	self->cursor = (prev != NULL) ? (gsize)(prev - self->text->str) : 0;
	return TRUE;
}

static gboolean
entry_right(GowlBarEntry *self)
{
	if (self->cursor >= self->text->len)
		return FALSE;
	self->cursor = (gsize)(g_utf8_next_char(self->text->str +
	                                        self->cursor) -
	                       self->text->str);
	return TRUE;
}

GowlBarEntryResult
gowl_bar_entry_handle_key(GowlBarEntry *self, guint keysym, guint modifiers,
                          gboolean multiline)
{
	gboolean ctrl;
	guint32 cp;

	g_return_val_if_fail(self != NULL, GOWL_BAR_ENTRY_IGNORED);

	/* Window-manager chords pass through untouched. */
	if (modifiers & (GOWL_KEY_MOD_ALT | GOWL_KEY_MOD_LOGO))
		return GOWL_BAR_ENTRY_IGNORED;

	ctrl = (modifiers & GOWL_KEY_MOD_CTRL) != 0;

	switch (keysym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (multiline && !ctrl) {
			gowl_bar_entry_insert(self, "\n", TRUE);
			return GOWL_BAR_ENTRY_EDITED;
		}
		return GOWL_BAR_ENTRY_SUBMIT;
	case XKB_KEY_Tab:
		return (modifiers & GOWL_KEY_MOD_SHIFT)
			? GOWL_BAR_ENTRY_LEAVE_PREV
			: GOWL_BAR_ENTRY_LEAVE_NEXT;
	case XKB_KEY_ISO_Left_Tab:
		return GOWL_BAR_ENTRY_LEAVE_PREV;
	case XKB_KEY_BackSpace:
		if (ctrl)
			return gowl_bar_entry_kill_word(self)
				? GOWL_BAR_ENTRY_EDITED : GOWL_BAR_ENTRY_MOVED;
		return gowl_bar_entry_backspace(self)
			? GOWL_BAR_ENTRY_EDITED : GOWL_BAR_ENTRY_MOVED;
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		return gowl_bar_entry_delete(self)
			? GOWL_BAR_ENTRY_EDITED : GOWL_BAR_ENTRY_MOVED;
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		entry_left(self);
		return GOWL_BAR_ENTRY_MOVED;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		entry_right(self);
		return GOWL_BAR_ENTRY_MOVED;
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		self->cursor = 0;
		return GOWL_BAR_ENTRY_MOVED;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		self->cursor = self->text->len;
		return GOWL_BAR_ENTRY_MOVED;
	default:
		break;
	}

	if (ctrl) {
		switch (keysym) {
		case XKB_KEY_a:
		case XKB_KEY_A:
			self->cursor = 0;
			return GOWL_BAR_ENTRY_MOVED;
		case XKB_KEY_e:
		case XKB_KEY_E:
			self->cursor = self->text->len;
			return GOWL_BAR_ENTRY_MOVED;
		case XKB_KEY_b:
		case XKB_KEY_B:
			entry_left(self);
			return GOWL_BAR_ENTRY_MOVED;
		case XKB_KEY_f:
		case XKB_KEY_F:
			entry_right(self);
			return GOWL_BAR_ENTRY_MOVED;
		case XKB_KEY_u:
		case XKB_KEY_U:
			return gowl_bar_entry_kill_line(self)
				? GOWL_BAR_ENTRY_EDITED : GOWL_BAR_ENTRY_MOVED;
		case XKB_KEY_w:
		case XKB_KEY_W:
			return gowl_bar_entry_kill_word(self)
				? GOWL_BAR_ENTRY_EDITED : GOWL_BAR_ENTRY_MOVED;
		case XKB_KEY_v:
		case XKB_KEY_V:
			return GOWL_BAR_ENTRY_PASTE;
		default:
			/* Any other Ctrl chord is the plugin's (Ctrl+S to
			   swap languages, Ctrl+L to pick one ...). */
			return GOWL_BAR_ENTRY_IGNORED;
		}
	}

	/* Shift+Insert pastes too, for the terminal-trained hand. */
	if (keysym == XKB_KEY_Insert && (modifiers & GOWL_KEY_MOD_SHIFT))
		return GOWL_BAR_ENTRY_PASTE;

	cp = xkb_keysym_to_utf32(keysym);
	if (cp >= 0x20 && cp != 0x7f && g_unichar_validate((gunichar)cp)) {
		gchar buf[8];
		gint n;

		n = g_unichar_to_utf8((gunichar)cp, buf);
		buf[n] = '\0';
		gowl_bar_entry_insert(self, buf, multiline);
		return GOWL_BAR_ENTRY_EDITED;
	}

	return GOWL_BAR_ENTRY_IGNORED;
}
