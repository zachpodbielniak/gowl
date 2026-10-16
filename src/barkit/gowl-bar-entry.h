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

#ifndef GOWL_BAR_ENTRY_H
#define GOWL_BAR_ENTRY_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlBarEntryResult:
 * @GOWL_BAR_ENTRY_IGNORED: the key means nothing to a text field; the
 *   caller should treat it as it would without an entry focused
 * @GOWL_BAR_ENTRY_EDITED: the text changed
 * @GOWL_BAR_ENTRY_MOVED: only the cursor moved
 * @GOWL_BAR_ENTRY_SUBMIT: the user asked to submit the text
 * @GOWL_BAR_ENTRY_PASTE: the user asked to paste; the caller fetches
 *   the clipboard (which cannot be read synchronously from the
 *   compositor thread) and calls gowl_bar_entry_insert() later
 * @GOWL_BAR_ENTRY_LEAVE_NEXT: Tab --- move the keyboard to the next item
 * @GOWL_BAR_ENTRY_LEAVE_PREV: Shift+Tab --- move it to the previous one
 *
 * What one key did to an entry.
 */
typedef enum {
	GOWL_BAR_ENTRY_IGNORED = 0,
	GOWL_BAR_ENTRY_EDITED,
	GOWL_BAR_ENTRY_MOVED,
	GOWL_BAR_ENTRY_SUBMIT,
	GOWL_BAR_ENTRY_PASTE,
	GOWL_BAR_ENTRY_LEAVE_NEXT,
	GOWL_BAR_ENTRY_LEAVE_PREV
} GowlBarEntryResult;

/**
 * GowlBarEntry:
 *
 * The editing state of one panel text field: its text and a cursor.
 *
 * Pure --- no compositor, no seat, no rendering --- so every editing
 * rule is a unit test away.  The cursor is a byte offset that always
 * sits on a UTF-8 character boundary.
 */
typedef struct _GowlBarEntry GowlBarEntry;

GowlBarEntry *gowl_bar_entry_new        (const gchar  *text);
void          gowl_bar_entry_free       (GowlBarEntry *self);

const gchar  *gowl_bar_entry_get_text   (const GowlBarEntry *self);
gsize         gowl_bar_entry_get_cursor (const GowlBarEntry *self);

/**
 * gowl_bar_entry_set_text:
 * @self: an entry
 * @text: (nullable): the new text
 *
 * Replaces the text and puts the cursor at its end.
 */
void          gowl_bar_entry_set_text   (GowlBarEntry *self,
                                          const gchar  *text);

/**
 * gowl_bar_entry_set_cursor:
 * @self: an entry
 * @cursor: a byte offset; clamped to the text and moved back to the
 *   nearest character boundary
 */
void          gowl_bar_entry_set_cursor (GowlBarEntry *self,
                                          gsize         cursor);

/**
 * gowl_bar_entry_insert:
 * @self: an entry
 * @text: UTF-8 to insert at the cursor; invalid UTF-8 is refused
 * @multiline: whether newlines are kept; a single-line entry turns
 *   them into spaces, so a pasted paragraph stays one line
 *
 * Returns: %TRUE when anything was inserted
 */
gboolean      gowl_bar_entry_insert     (GowlBarEntry *self,
                                          const gchar  *text,
                                          gboolean      multiline);

gboolean      gowl_bar_entry_backspace  (GowlBarEntry *self);
gboolean      gowl_bar_entry_delete     (GowlBarEntry *self);
gboolean      gowl_bar_entry_kill_word  (GowlBarEntry *self);
gboolean      gowl_bar_entry_kill_line  (GowlBarEntry *self);

/**
 * gowl_bar_entry_handle_key:
 * @self: an entry
 * @keysym: an xkb keysym, already shifted by the active modifiers
 * @modifiers: the gowl modifier mask (#GowlKeyMod bits)
 * @multiline: whether Return inserts a newline (then Ctrl+Return
 *   submits)
 *
 * Applies one key press.  Ctrl+Shift+V is not a paste (it is left for
 * the widget, as every other unclaimed Ctrl chord is).  Alt and Super
 * chords are always
 * %GOWL_BAR_ENTRY_IGNORED: they belong to the window manager, and a
 * text field that swallowed them would trap the user in a panel.
 *
 * Returns: what the key did
 */
GowlBarEntryResult gowl_bar_entry_handle_key (GowlBarEntry *self,
                                               guint         keysym,
                                               guint         modifiers,
                                               gboolean      multiline);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlBarEntry, gowl_bar_entry_free)

G_END_DECLS

#endif /* GOWL_BAR_ENTRY_H */
