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
 * gowl-client-input.c - Input aimed at one window, and window order.
 *
 * Every injection path gowl had delivered to whatever has keyboard
 * focus.  Macros need more: "type this into the terminal" while the
 * editor keeps focus.  gowl_compositor_send_key_to_client() moves the
 * seat's keyboard focus to the target surface for the length of the
 * keystrokes and puts it back, with the modifiers the keys need and
 * without touching the real keyboard's xkb state -- the physical
 * keyboard's held keys are handed back to the original surface on the
 * way out, so nothing is left stuck.
 *
 * It is synthetic input: never recorded, never diverted to a KVM, and
 * refused while the session is locked or a launcher/popup holds the
 * keyboard (the same two grabs the focus gate honours).  The target
 * sees a brief wl_keyboard.enter/leave around the keys; a client that
 * pauses on focus loss (some games) will notice.
 */

#include "gowl-core-private.h"

#include <linux/input-event-codes.h>
#include <string.h>
#include <time.h>

static guint32
now_msec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (guint32)((guint64)ts.tv_sec * 1000 + (guint64)ts.tv_nsec / 1000000);
}

/**
 * gowl_compositor_keysym_to_keycode:
 * @self: a #GowlCompositor
 * @keysym: an XKB keysym
 * @out_shift: (out) (optional): %TRUE when the keysym sits on a shifted
 *   level of its key, so Shift must be held for it
 *
 * Finds the key that produces @keysym in the current keymap.
 *
 * Returns: the evdev keycode, or 0 when no key produces it
 */
guint32
gowl_compositor_keysym_to_keycode(
	GowlCompositor *self,
	guint32         keysym,
	gboolean       *out_shift
){
	struct xkb_keymap *keymap;
	xkb_keycode_t kc;

	if (out_shift != NULL)
		*out_shift = FALSE;
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), 0);
	if (self->wlr_kb_group == NULL)
		return 0;
	keymap = self->wlr_kb_group->keyboard.keymap;
	if (keymap == NULL)
		return 0;

	/* Level 0 of every key first, so `a' is plain `a' and not the
	   shifted level of some other key that also produces it. */
	for (kc = xkb_keymap_min_keycode(keymap);
	     kc <= xkb_keymap_max_keycode(keymap); kc++) {
		const xkb_keysym_t *syms;
		gint n;
		gint i;

		n = xkb_keymap_key_get_syms_by_level(keymap, kc, 0, 0, &syms);
		for (i = 0; i < n; i++)
			if (syms[i] == keysym)
				return (guint32)(kc - 8);
	}
	for (kc = xkb_keymap_min_keycode(keymap);
	     kc <= xkb_keymap_max_keycode(keymap); kc++) {
		xkb_level_index_t level;
		xkb_level_index_t n_levels;

		n_levels = xkb_keymap_num_levels_for_key(keymap, kc, 0);
		for (level = 1; level < n_levels; level++) {
			const xkb_keysym_t *syms;
			gint n;
			gint i;

			n = xkb_keymap_key_get_syms_by_level(keymap, kc, 0, level, &syms);
			for (i = 0; i < n; i++) {
				if (syms[i] == keysym) {
					if (out_shift != NULL)
						*out_shift = TRUE;
					return (guint32)(kc - 8);
				}
			}
		}
	}
	return 0;
}

/* The xkb depressed mask for #GowlKeyMod bits, in the live keymap. */
static xkb_mod_mask_t
mods_to_mask(
	struct xkb_keymap *keymap,
	guint32            mods
){
	static const struct {
		guint32      bit;
		const gchar *name;
	} table[] = {
		{ GOWL_KEY_MOD_SHIFT, XKB_MOD_NAME_SHIFT },
		{ GOWL_KEY_MOD_CTRL,  XKB_MOD_NAME_CTRL  },
		{ GOWL_KEY_MOD_ALT,   XKB_MOD_NAME_ALT   },
		{ GOWL_KEY_MOD_LOGO,  XKB_MOD_NAME_LOGO  },
		{ GOWL_KEY_MOD_MOD3,  "Mod3"             },
		{ GOWL_KEY_MOD_MOD5,  "Mod5"             },
	};
	xkb_mod_mask_t mask;
	guint i;

	mask = 0;
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		xkb_mod_index_t idx;

		if ((mods & table[i].bit) == 0)
			continue;
		idx = xkb_keymap_mod_get_index(keymap, table[i].name);
		if (idx != XKB_MOD_INVALID)
			mask |= (xkb_mod_mask_t)1 << idx;
	}
	return mask;
}

/* The surface keyboard focus is taken from, and must be given back to. */
typedef struct {
	struct wlr_surface *previous;
	struct wlr_surface *target;
	struct wlr_keyboard *kb;
} TargetFocus;

/*
 * Aims the seat's keyboard at @c.  Refused while locked or grabbed:
 * a macro must never type past a lock screen, nor under a launcher that
 * owns the keyboard.
 */
static gboolean
target_begin(
	GowlCompositor *self,
	GowlClient     *c,
	TargetFocus    *tf
){
	memset(tf, 0, sizeof *tf);
	if (self->wlr_seat == NULL || self->wlr_kb_group == NULL || c == NULL)
		return FALSE;
	if (self->locked || gowl_compositor_keyboard_is_grabbed(self))
		return FALSE;
	tf->target = gowl_client_get_wlr_surface(c);
	if (tf->target == NULL)
		return FALSE;
	tf->kb = &self->wlr_kb_group->keyboard;
	tf->previous = self->wlr_seat->keyboard_state.focused_surface;

	wlr_seat_set_keyboard(self->wlr_seat, tf->kb);
	if (tf->previous != tf->target)
		wlr_seat_keyboard_notify_enter(self->wlr_seat, tf->target,
		                               NULL, 0, &tf->kb->modifiers);
	return TRUE;
}

/* Gives focus back, with the real keyboard's held keys and modifiers. */
static void
target_end(
	GowlCompositor *self,
	TargetFocus    *tf
){
	wlr_seat_keyboard_notify_modifiers(self->wlr_seat, &tf->kb->modifiers);
	if (tf->previous == tf->target)
		return;
	if (tf->previous != NULL)
		wlr_seat_keyboard_notify_enter(self->wlr_seat, tf->previous,
		                               tf->kb->keycodes, tf->kb->num_keycodes,
		                               &tf->kb->modifiers);
	else
		wlr_seat_keyboard_notify_clear_focus(self->wlr_seat);
}

/* One press and release to the focused-for-now target, with @mods held
   for it through a modifiers event of its own. */
static void
target_tap(
	GowlCompositor *self,
	TargetFocus    *tf,
	guint32         keycode,
	guint32         mods
){
	struct wlr_keyboard_modifiers m;
	guint32 t;

	m = tf->kb->modifiers;
	m.depressed = mods_to_mask(tf->kb->keymap, mods);
	wlr_seat_keyboard_notify_modifiers(self->wlr_seat, &m);
	t = now_msec();
	wlr_seat_keyboard_notify_key(self->wlr_seat, t, keycode,
	                             WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(self->wlr_seat, t, keycode,
	                             WL_KEYBOARD_KEY_STATE_RELEASED);
}

/**
 * gowl_compositor_send_key_to_client:
 * @self: a #GowlCompositor
 * @client: the window to type into; it keeps no focus afterwards
 * @keycode: an evdev keycode
 * @modifiers: #GowlKeyMod bits held for the key
 *
 * Taps one key into @client whether or not it has focus, then gives
 * keyboard focus back to whatever had it.  Synthetic: not recorded, not
 * captured, and no compositor keybind sees it.
 *
 * Returns: %TRUE when delivered; %FALSE while the session is locked, a
 *   launcher or popup holds the keyboard, or @client has no surface
 */
gboolean
gowl_compositor_send_key_to_client(
	GowlCompositor *self,
	GowlClient     *client,
	guint32         keycode,
	guint32         modifiers
){
	TargetFocus tf;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(GOWL_IS_CLIENT(client), FALSE);

	if (!target_begin(self, client, &tf))
		return FALSE;
	target_tap(self, &tf, keycode, modifiers);
	target_end(self, &tf);
	return TRUE;
}

/**
 * gowl_compositor_send_text_to_client:
 * @self: a #GowlCompositor
 * @client: the window to type into
 * @text: UTF-8 text
 *
 * Types @text into @client, one key per character, under a single focus
 * change.  A character no key in the current layout produces is skipped
 * (and counted out of the return value).  Newlines are Return, tabs Tab.
 *
 * Returns: how many characters were typed, -1 when refused (see
 *   gowl_compositor_send_key_to_client())
 */
gint
gowl_compositor_send_text_to_client(
	GowlCompositor *self,
	GowlClient     *client,
	const gchar    *text
){
	TargetFocus tf;
	const gchar *p;
	gint sent;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), -1);
	g_return_val_if_fail(GOWL_IS_CLIENT(client), -1);
	g_return_val_if_fail(text != NULL, -1);

	if (!g_utf8_validate(text, -1, NULL))
		return -1;
	if (!target_begin(self, client, &tf))
		return -1;

	sent = 0;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar ch;
		xkb_keysym_t sym;
		guint32 keycode;
		gboolean shift;

		ch = g_utf8_get_char(p);
		if (ch == '\n')
			sym = XKB_KEY_Return;
		else if (ch == '\t')
			sym = XKB_KEY_Tab;
		else
			sym = xkb_utf32_to_keysym(ch);
		keycode = sym != XKB_KEY_NoSymbol
			? gowl_compositor_keysym_to_keycode(self, sym, &shift) : 0;
		if (keycode == 0)
			continue;
		target_tap(self, &tf, keycode, shift ? GOWL_KEY_MOD_SHIFT : 0);
		sent++;
	}
	target_end(self, &tf);
	return sent;
}

/**
 * gowl_compositor_reorder_clients:
 * @self: a #GowlCompositor
 * @order: (element-type GowlClient): the clients, in the order wanted
 *
 * Puts the clients in @order into that order, in the slots of the
 * client list they already occupy; every other client keeps its place.
 * The client list is the tiling order, so sorting the windows of one
 * tag is: collect them, sort, call this.  Monitors that changed are
 * re-arranged once each.  Unknown or repeated clients are ignored.
 */
void
gowl_compositor_reorder_clients(
	GowlCompositor *self,
	GList          *order
){
	g_autoptr(GPtrArray) slots = NULL;
	g_autoptr(GPtrArray) wanted = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GHashTable) monitors = NULL;
	GHashTableIter iter;
	gpointer mon;
	GList *l;
	guint i;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	/* The wanted order, known clients only, each once */
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	wanted = g_ptr_array_new();
	for (l = order; l != NULL; l = l->next) {
		if (l->data == NULL || g_hash_table_contains(seen, l->data)
		    || g_list_find(self->clients, l->data) == NULL)
			continue;
		g_hash_table_add(seen, l->data);
		g_ptr_array_add(wanted, l->data);
	}
	if (wanted->len < 2)
		return;

	/* The list links they occupy now, in list order */
	slots = g_ptr_array_new();
	for (l = self->clients; l != NULL; l = l->next)
		if (g_hash_table_contains(seen, l->data))
			g_ptr_array_add(slots, l);

	/* Refill those links in the wanted order */
	monitors = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (i = 0; i < slots->len; i++) {
		GList *link = g_ptr_array_index(slots, i);
		GowlClient *c = g_ptr_array_index(wanted, i);

		if (link->data != (gpointer)c) {
			if (((GowlClient *)link->data)->mon != NULL)
				g_hash_table_add(monitors,
				                 ((GowlClient *)link->data)->mon);
			if (c->mon != NULL)
				g_hash_table_add(monitors, c->mon);
			link->data = c;
		}
	}

	g_hash_table_iter_init(&iter, monitors);
	while (g_hash_table_iter_next(&iter, &mon, NULL))
		gowl_compositor_arrange(self, (GowlMonitor *)mon);
}
