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
 * gowl-hotkey-send.h - AutoHotkey's Send syntax, as a list of events.
 *
 *   ^c                  Ctrl+c
 *   {Enter}  {Tab 3}    a named key; three of them
 *   {Shift down}x{Shift up}
 *   {U+00E9}            a character by code point
 *   {Blind}^{Right}     leave the held modifiers alone
 *   {Raw}^c             the rest is literal: ^, c
 *   {Text}any thing     the rest is text, typed (or committed) as is
 *   {{} {}} {^} {!}     the literal characters
 *   {LButton} {Click}   a click
 *   {Sleep 100}         (gowl) a pause while the rest waits
 *
 * Pure: the module turns the events into keycodes against the keymap in
 * force when it types them.
 */

#ifndef GOWL_HOTKEY_SEND_H
#define GOWL_HOTKEY_SEND_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
	HK_SEND_KEY,      /* a key by keysym (or keycode): tap, down or up */
	HK_SEND_CHAR,     /* a character: its key if the layout has one */
	HK_SEND_TEXT,     /* text to type or commit as is */
	HK_SEND_BUTTON,   /* a pointer button: tap, down or up */
	HK_SEND_SLEEP     /* wait @ms before the rest */
} HkSendKind;

typedef struct {
	HkSendKind kind;
	guint32    keysym;    /* KEY */
	guint32    keycode;   /* KEY: an evdev keycode (code:NN), else 0 */
	guint32    button;    /* BUTTON */
	gint       updown;    /* 0 tap, 1 down, -1 up */
	guint      mods;      /* KEY/CHAR/BUTTON tap: modifiers to hold around it */
	gunichar   ch;        /* CHAR */
	gchar     *text;      /* TEXT */
	guint      ms;        /* SLEEP */
} HkSendEvent;

typedef struct {
	GArray  *events;      /* HkSendEvent */
	gboolean blind;       /* {Blind}: do not lift held modifiers */
} HkSendPlan;

HkSendPlan *hk_send_parse      (const gchar  *keys,
                                GError      **error);
HkSendPlan *hk_send_text_plan  (const gchar  *text);
void        hk_send_plan_free  (HkSendPlan   *plan);
gchar      *hk_send_describe   (const HkSendPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HkSendPlan, hk_send_plan_free)

G_END_DECLS

#endif /* GOWL_HOTKEY_SEND_H */
