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
 *
 * gowl_compositor_deliver_key() is the general form: a press or a
 * release on its own (so a key can be HELD in another window), with the
 * real modifiers or chosen ones, and an X11 route.  The seat route alone
 * cannot reach an X11 window: Xwayland gives every key to the window
 * holding X input focus, whichever wl_surface was entered, so for one of
 * those X focus is moved too and each step is confirmed with the X
 * server (gowl-xwayland-sync.c).  A batch (input_batch_begin/end) lets
 * several deliveries share one restore of focus.
 *
 * Two limits come from the protocols, not from here:
 *
 *   - a Wayland client forgets its pressed keys on wl_keyboard.leave, so
 *     a key cannot stay held in a Wayland window that does not have
 *     focus: a press delivered there is, to the client, a tap;
 *   - X key state is the server's, shared by every X11 window, so a key
 *     held for one background X11 window is held for the X server --
 *     X autorepeat, if a client asked for it, goes to whichever window
 *     has X focus.
 */

#include "gowl-core-private.h"
#include "gowl-xwayland-sync.h"

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

static gboolean delivery_allowed (GowlCompositor *self, GowlClient *c);
#ifdef GOWL_HAVE_XWAYLAND
static gboolean x11_deliver      (GowlCompositor *self, GowlClient *c,
                                  guint32 keycode, gboolean pressed,
                                  gint mods);
#endif

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
 * An X11 window is reached through X focus, confirmed with the X server
 * (see gowl_compositor_deliver_key()): the seat route alone would type
 * into whichever X11 window has focus instead.
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

#ifdef GOWL_HAVE_XWAYLAND
	if (client->xwayland_surface != NULL) {
		gboolean ok;

		if (!delivery_allowed(self, client))
			return FALSE;
		gowl_compositor_input_batch_begin(self);
		ok = x11_deliver(self, client, keycode, TRUE, (gint)modifiers)
		     && x11_deliver(self, client, keycode, FALSE,
		                    (gint)modifiers);
		gowl_compositor_input_batch_end(self);
		return ok;
	}
#endif

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
#ifdef GOWL_HAVE_XWAYLAND
	if (client->xwayland_surface != NULL) {
		if (!delivery_allowed(self, client))
			return -1;
		sent = 0;
		gowl_compositor_input_batch_begin(self);
		for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
			gunichar ch;
			xkb_keysym_t sym;
			guint32 keycode;
			gboolean shift;
			gint mods;

			ch = g_utf8_get_char(p);
			sym = ch == '\n' ? XKB_KEY_Return
			    : ch == '\t' ? XKB_KEY_Tab : xkb_utf32_to_keysym(ch);
			keycode = sym != XKB_KEY_NoSymbol
				? gowl_compositor_keysym_to_keycode(self, sym, &shift) : 0;
			if (keycode == 0)
				continue;
			mods = shift ? GOWL_KEY_MOD_SHIFT : 0;
			if (x11_deliver(self, client, keycode, TRUE, mods)
			    && x11_deliver(self, client, keycode, FALSE, mods))
				sent++;
		}
		gowl_compositor_input_batch_end(self);
		return sent;
	}
#endif
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

/* ── Targeted delivery: presses, releases, X11 and batches ───────────── */

/* One key event to the focused-for-now target, with @mods held for it
 * (-1: the real keyboard's modifiers, already current on the seat). */
static void
target_key(
	GowlCompositor *self,
	TargetFocus    *tf,
	guint32         keycode,
	gboolean        pressed,
	gint            mods
){
	if (mods >= 0) {
		struct wlr_keyboard_modifiers m;

		m = tf->kb->modifiers;
		m.depressed = mods_to_mask(tf->kb->keymap, (guint32)mods);
		m.latched = 0;
		wlr_seat_keyboard_notify_modifiers(self->wlr_seat, &m);
	}
	wlr_seat_keyboard_notify_key(self->wlr_seat, now_msec(), keycode,
	                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                                     : WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* Shared refusal: locked, grabbed, or nothing to deliver to. */
static gboolean
delivery_allowed(
	GowlCompositor *self,
	GowlClient     *c
){
	if (self->wlr_seat == NULL || self->wlr_kb_group == NULL)
		return FALSE;
	if (self->locked || gowl_compositor_keyboard_is_grabbed(self))
		return FALSE;
	return gowl_client_get_wlr_surface(c) != NULL;
}

#ifdef GOWL_HAVE_XWAYLAND

/* Drops a sync whose server went away; the next delivery reconnects. */
static void
xsync_drop(GowlCompositor *self)
{
	if (self->xsync_source != NULL) {
		wl_event_source_remove(self->xsync_source);
		self->xsync_source = NULL;
	}
	g_clear_pointer(&self->xsync, gowl_xwayland_sync_free);
}

/* Keeps the connection read while nobody is waiting on it. */
static gint
on_xsync_readable(
	gint     fd,
	guint32  mask,
	gpointer data
){
	GowlCompositor *self = GOWL_COMPOSITOR(data);

	(void)fd;
	if (self->xsync == NULL)
		return 0;
	gowl_xwayland_sync_drain(self->xsync);
	if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0
	    || !gowl_xwayland_sync_is_alive(self->xsync))
		xsync_drop(self);
	return 0;
}

/* The X connection, made on first use; NULL when it cannot be (no
 * Xwayland yet, or it refused).  Never starts a lazy Xwayland: callers
 * only get here holding an X11 client, which means it is running. */
static GowlXwaylandSync *
xsync_get(GowlCompositor *self)
{
	g_autoptr(GError) error = NULL;
	static gboolean warned = FALSE;

	if (self->xsync != NULL && !gowl_xwayland_sync_is_alive(self->xsync))
		xsync_drop(self);
	if (self->xsync != NULL)
		return self->xsync;
	if (self->xwayland == NULL || self->xwayland->display_name == NULL)
		return NULL;

	self->xsync = gowl_xwayland_sync_new(self->xwayland->display_name,
	                                     &error);
	if (self->xsync == NULL) {
		if (!warned) {
			g_warning("X11 delivery is unconfirmed: %s",
			          error != NULL ? error->message : "no connection");
			warned = TRUE;
		}
		return NULL;
	}
	self->xsync_source = wl_event_loop_add_fd(
		self->event_loop, gowl_xwayland_sync_get_fd(self->xsync),
		WL_EVENT_READABLE, on_xsync_readable, self);
	return self->xsync;
}

static gboolean
surface_is_xwayland(struct wlr_surface *surface)
{
	return surface != NULL
	       && wlr_xwayland_surface_try_from_wlr_surface(surface) != NULL;
}

/* One key event into Xwayland through the seat, confirmed processed.
 * X focus decides which window it reaches. */
static gboolean
x11_send(
	GowlCompositor   *self,
	GowlXwaylandSync *sync,
	guint32           keycode,
	gboolean          pressed
){
	if (sync != NULL)
		gowl_xwayland_sync_drain(sync);
	wlr_seat_keyboard_notify_key(self->wlr_seat, now_msec(), keycode,
	                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                                     : WL_KEYBOARD_KEY_STATE_RELEASED);
	wl_display_flush_clients(self->wl_display);
	if (sync == NULL) {
		g_usleep(8000);
		return TRUE;
	}
	return gowl_xwayland_sync_wait_key(sync, keycode, pressed,
	                                   self->delivery_timeout_ms);
}

/* A key this batch sent into Xwayland, and how the server had it. */
typedef struct {
	guint32 keycode;
	gint    was_down;   /* 1, 0, or -1 when the server did not say */
} GowlBatchKey;

/* Remembers @keycode, with its state @down before this batch first
 * touched it, for batch_end. */
static void
batch_note_key(
	GowlCompositor *self,
	guint32         keycode,
	gint            down
){
	GowlBatchKey bk;
	guint i;

	if (self->batch_x_keys == NULL)
		self->batch_x_keys = g_array_new(FALSE, FALSE,
		                                 sizeof(GowlBatchKey));
	for (i = 0; i < self->batch_x_keys->len; i++)
		if (g_array_index(self->batch_x_keys, GowlBatchKey,
		                  i).keycode == keycode)
			return;
	bk.keycode = keycode;
	bk.was_down = down;
	g_array_append_val(self->batch_x_keys, bk);
}

/*
 * One key event to an X11 window that may not have focus, inside a
 * batch (the batch puts focus back).  The steps, each confirmed:
 *
 *   1. the seat's keyboard must be on SOME Xwayland surface, or the key
 *      never reaches Xwayland at all; any of them will do, because
 *      Xwayland routes by X focus, so a focused X11 window keeps its
 *      wl focus and sees no leave/enter;
 *   2. X focus to the target, and wait until the server agrees;
 *   3. the key, flushed, and wait until the server has processed it.
 *
 * Step 3 has to respect that an X server has ONE key state for every
 * window: a press of a key already down (the focused window holding it,
 * or another target just given it) is dropped as autorepeat, and a
 * release of a key already up goes nowhere.  So the server is asked
 * first, and the event is preceded by its opposite when needed -- a
 * stray release before a press, a stray press before a release -- which
 * the target takes as nothing worse than a key it did not know was
 * held.  batch_end puts the state back for the window returned to.
 */
static gboolean
x11_deliver(
	GowlCompositor *self,
	GowlClient     *c,
	guint32         keycode,
	gboolean        pressed,
	gint            mods
){
	GowlXwaylandSync *sync;
	struct wlr_keyboard *kb;
	struct wlr_surface *focused;
	struct wlr_surface *surface;
	GowlClient *sel;
	gboolean ok;

	kb = &self->wlr_kb_group->keyboard;
	surface = gowl_client_get_wlr_surface(c);
	sync = xsync_get(self);

	/* 1. a wl focus Xwayland will route */
	focused = self->wlr_seat->keyboard_state.focused_surface;
	wlr_seat_set_keyboard(self->wlr_seat, kb);
	if (!surface_is_xwayland(focused)) {
		if (!self->batch_wl_moved) {
			self->batch_wl_moved = TRUE;
			self->batch_wl_origin = focused;
		}
		/* No keys: the held ones belong to the window we left. */
		wlr_seat_keyboard_notify_enter(self->wlr_seat, surface, NULL, 0,
		                               &kb->modifiers);
	}

	/* 2. X focus */
	if (!self->batch_x_moved) {
		sel = gowl_compositor_get_focused_client(self);
		self->batch_x_moved = TRUE;
		self->batch_x_origin =
			(sel != NULL && sel->xwayland_surface != NULL) ? sel : NULL;
	}
	wlr_xwayland_surface_activate(c->xwayland_surface, TRUE);
	if (sync != NULL
	    && !gowl_xwayland_sync_wait_focus(sync,
	                                      c->xwayland_surface->window_id,
	                                      self->delivery_timeout_ms)) {
		g_info("deliver: X focus never reached window 0x%x",
		       (guint)c->xwayland_surface->window_id);
		return FALSE;
	}

	/* 3. the key, into the state the server is in */
	if (sync != NULL) {
		gint down = gowl_xwayland_sync_key_is_down(sync, keycode,
		                                           self->delivery_timeout_ms);

		batch_note_key(self, keycode, down);
		if (down == 1 && pressed)
			x11_send(self, sync, keycode, FALSE);
		else if (down == 0 && !pressed)
			x11_send(self, sync, keycode, TRUE);
	}
	if (mods >= 0) {
		struct wlr_keyboard_modifiers m;

		m = kb->modifiers;
		m.depressed = mods_to_mask(kb->keymap, (guint32)mods);
		m.latched = 0;
		wlr_seat_keyboard_notify_modifiers(self->wlr_seat, &m);
	}
	ok = x11_send(self, sync, keycode, pressed);
	if (mods >= 0)
		wlr_seat_keyboard_notify_modifiers(self->wlr_seat,
		                                   &kb->modifiers);
	if (!ok)
		g_info("deliver: Xwayland never processed key %u", keycode);
	return ok;
}

/* With X focus back on the window that had it: each key this batch
 * touched goes back to the state the server had it in before, so that
 * window's own next event for it -- the release of a key it holds, a
 * press of a key a target was left holding -- is not dropped against a
 * state the targets left behind.  The cost is borne by the targets: a
 * held key's release later finds it up and is preceded by a press,
 * which a target sees as autorepeat. */
static void
x11_restore_keys(
	GowlCompositor   *self,
	GowlXwaylandSync *sync
){
	guint i;

	if (sync == NULL || self->batch_x_keys == NULL)
		return;
	for (i = 0; i < self->batch_x_keys->len; i++) {
		GowlBatchKey *bk = &g_array_index(self->batch_x_keys,
		                                  GowlBatchKey, i);
		gint down;

		if (bk->was_down < 0)
			continue;
		down = gowl_xwayland_sync_key_is_down(sync, bk->keycode,
		                                      self->delivery_timeout_ms);
		if (down < 0 || down == bk->was_down)
			continue;
		x11_send(self, sync, bk->keycode, bk->was_down == 1);
	}
}

#endif /* GOWL_HAVE_XWAYLAND */

/**
 * gowl_compositor_input_batch_begin:
 * @self: a #GowlCompositor
 *
 * Opens a batch: until the matching gowl_compositor_input_batch_end(),
 * deliveries to X11 windows leave X focus (and the seat's keyboard, if
 * it had to move) where the last one put it, and the end puts it back
 * once.  Batches nest; only the outermost end restores.  A batch must
 * open and close within one call: nothing may dispatch the event loop
 * in between.
 */
void
gowl_compositor_input_batch_begin(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->batch_depth++;
}

/**
 * gowl_compositor_input_batch_end:
 * @self: a #GowlCompositor
 *
 * Closes a batch.  The outermost end gives X input focus back to the
 * X11 window that had it (waiting until the server agrees, so the next
 * real key cannot overtake it), or to nobody, and re-enters the surface
 * the seat's keyboard was on with the keys still physically held.
 */
void
gowl_compositor_input_batch_end(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(self->batch_depth > 0);

	if (--self->batch_depth > 0)
		return;

#ifdef GOWL_HAVE_XWAYLAND
	if (self->batch_x_moved) {
		GowlClient *origin = self->batch_x_origin;

		if (origin != NULL && origin->xwayland_surface != NULL
		    && g_list_find(self->clients, origin) != NULL) {
			GowlXwaylandSync *sync;

			wlr_xwayland_surface_activate(origin->xwayland_surface,
			                              TRUE);
			sync = xsync_get(self);
			if (sync != NULL
			    && !gowl_xwayland_sync_wait_focus(sync,
			            origin->xwayland_surface->window_id,
			            self->delivery_timeout_ms))
				g_info("deliver: X focus did not come back to "
				       "window 0x%x",
				       (guint)origin->xwayland_surface->window_id);
			else if (surface_is_xwayland(
				         self->wlr_seat->keyboard_state.focused_surface))
				x11_restore_keys(self, sync);
		} else {
			/* No X11 window had focus before: give it to none.
			 * Deactivating any one surface clears the xwm's. */
			GList *l;

			for (l = self->clients; l != NULL; l = l->next) {
				GowlClient *c = (GowlClient *)l->data;

				if (c->xwayland_surface != NULL) {
					wlr_xwayland_surface_activate(
						c->xwayland_surface, FALSE);
					break;
				}
			}
		}
	}
#endif

	if (self->batch_wl_moved && self->wlr_seat != NULL
	    && self->wlr_kb_group != NULL) {
		struct wlr_keyboard *kb = &self->wlr_kb_group->keyboard;

		if (self->batch_wl_origin != NULL)
			wlr_seat_keyboard_notify_enter(self->wlr_seat,
			                               self->batch_wl_origin,
			                               kb->keycodes,
			                               kb->num_keycodes,
			                               &kb->modifiers);
		else
			wlr_seat_keyboard_notify_clear_focus(self->wlr_seat);
	}

	if (self->batch_x_keys != NULL)
		g_array_set_size(self->batch_x_keys, 0);
	self->batch_x_moved = FALSE;
	self->batch_x_origin = NULL;
	self->batch_wl_moved = FALSE;
	self->batch_wl_origin = NULL;
}

/**
 * gowl_compositor_deliver_key:
 * @self: a #GowlCompositor
 * @client: the window; it keeps no focus afterwards
 * @keycode: an evdev keycode
 * @pressed: %TRUE for the press, %FALSE for the release
 * @modifiers: #GowlKeyMod bits to hold for the key, or -1 for the real
 *   keyboard's modifiers as they are
 * @delivery: how to reach the window; see #GowlKeyDelivery
 *
 * Puts one key event into @client whether or not it has focus, then
 * gives focus back (at the end of the enclosing batch, if any).
 * Synthetic: not recorded, not captured, and no compositor keybind sees
 * it.  Refused while the session is locked or a launcher or popup holds
 * the keyboard.
 *
 * For an X11 window this blocks until Xwayland has processed the key,
 * at most the delivery timeout per step (gowl_compositor_set_delivery_
 * timeout()), so the caller can rely on the order of what it sends.
 *
 * Returns: %TRUE when delivered (and, for an X11 window, processed);
 *   %FALSE when refused or when Xwayland did not confirm in time
 */
gboolean
gowl_compositor_deliver_key(
	GowlCompositor  *self,
	GowlClient      *client,
	guint32          keycode,
	gboolean         pressed,
	gint             modifiers,
	GowlKeyDelivery  delivery
){
	TargetFocus tf;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(GOWL_IS_CLIENT(client), FALSE);

	if (!delivery_allowed(self, client))
		return FALSE;

	if (delivery == GOWL_KEY_DELIVERY_AUTO)
		delivery = client->xwayland_surface != NULL
		           ? GOWL_KEY_DELIVERY_ACTIVATE : GOWL_KEY_DELIVERY_SEAT;
	if (delivery == GOWL_KEY_DELIVERY_ACTIVATE
	    && client->xwayland_surface == NULL)
		delivery = GOWL_KEY_DELIVERY_SEAT;

#ifdef GOWL_HAVE_XWAYLAND
	if (delivery == GOWL_KEY_DELIVERY_ACTIVATE) {
		gboolean ok;

		gowl_compositor_input_batch_begin(self);
		ok = x11_deliver(self, client, keycode, pressed, modifiers);
		gowl_compositor_input_batch_end(self);
		return ok;
	}
#endif

	if (!target_begin(self, client, &tf))
		return FALSE;
	target_key(self, &tf, keycode, pressed, modifiers);
	target_end(self, &tf);
	return TRUE;
}

/**
 * gowl_compositor_await_x11_key:
 * @self: a #GowlCompositor
 * @keycode: an evdev keycode just sent through the seat
 * @pressed: whether it was the press or the release
 *
 * For a key the compositor itself just forwarded to a focused X11
 * window: blocks until Xwayland has processed it, at most the delivery
 * timeout.  Moving X focus elsewhere before that is a race the key can
 * lose -- it would land in the window focus moved to.  Returns at once
 * when the seat's keyboard is not on an X11 window.
 *
 * Returns: %TRUE once processed, or when there is nothing to wait for
 */
gboolean
gowl_compositor_await_x11_key(
	GowlCompositor *self,
	guint32         keycode,
	gboolean        pressed
){
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

#ifdef GOWL_HAVE_XWAYLAND
	{
		GowlXwaylandSync *sync;

		if (self->wlr_seat == NULL || !surface_is_xwayland(
			    self->wlr_seat->keyboard_state.focused_surface))
			return TRUE;
		sync = xsync_get(self);
		if (sync == NULL)
			return TRUE;
		wl_display_flush_clients(self->wl_display);
		return gowl_xwayland_sync_wait_key(sync, keycode, pressed,
		                                   self->delivery_timeout_ms);
	}
#else
	(void)keycode;
	(void)pressed;
	return TRUE;
#endif
}

/**
 * gowl_compositor_deliver_button:
 * @self: a #GowlCompositor
 * @client: the window under which the click lands
 * @button: a linux/input-event-codes.h button, e.g. BTN_LEFT
 * @pressed: %TRUE for the press, %FALSE for the release
 * @x: surface-local x of the click
 * @y: surface-local y of the click
 *
 * Puts one button event into @client at a point of its surface, then
 * hands the pointer back to the surface it was on, at the position it
 * was at.  Pointer input needs no X focus: Xwayland gives a click to the
 * window under its pointer, which the enter places, and the Wayland
 * events reach Xwayland in order on its one socket.  Uses the seat's
 * low-level senders, so the real button state the seat tracks is not
 * disturbed.  Refused while locked.
 *
 * Returns: %TRUE when delivered
 */
gboolean
gowl_compositor_deliver_button(
	GowlCompositor *self,
	GowlClient     *client,
	guint32         button,
	gboolean        pressed,
	gdouble         x,
	gdouble         y
){
	struct wlr_surface *surface;
	struct wlr_surface *previous;
	gdouble px, py;
	guint32 t;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(GOWL_IS_CLIENT(client), FALSE);

	if (self->wlr_seat == NULL || self->locked)
		return FALSE;
	surface = gowl_client_get_wlr_surface(client);
	if (surface == NULL)
		return FALSE;

	previous = self->wlr_seat->pointer_state.focused_surface;
	px = self->wlr_seat->pointer_state.sx;
	py = self->wlr_seat->pointer_state.sy;
	t = now_msec();

	wlr_seat_pointer_enter(self->wlr_seat, surface, x, y);
	wlr_seat_pointer_send_motion(self->wlr_seat, t, x, y);
	wlr_seat_pointer_send_button(self->wlr_seat, t, button,
	                             pressed ? WL_POINTER_BUTTON_STATE_PRESSED
	                                     : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_send_frame(self->wlr_seat);

	if (previous != surface) {
		if (previous != NULL) {
			wlr_seat_pointer_enter(self->wlr_seat, previous, px, py);
			wlr_seat_pointer_send_motion(self->wlr_seat, t, px, py);
		} else {
			wlr_seat_pointer_clear_focus(self->wlr_seat);
		}
		wlr_seat_pointer_send_frame(self->wlr_seat);
	} else {
		wlr_seat_pointer_send_motion(self->wlr_seat, t, px, py);
		wlr_seat_pointer_send_frame(self->wlr_seat);
	}
	return TRUE;
}

/**
 * gowl_compositor_set_delivery_timeout:
 * @self: a #GowlCompositor
 * @timeout_ms: how long one step of an X11 delivery may wait for
 *   Xwayland (focus to move, a key to be processed), 5..2000
 *
 * Every X11 delivery blocks the compositor for up to a few of these if
 * Xwayland stops answering, so it is clamped.
 */
void
gowl_compositor_set_delivery_timeout(
	GowlCompositor *self,
	guint           timeout_ms
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	self->delivery_timeout_ms = CLAMP(timeout_ms, 5, 2000);
}

/**
 * gowl_compositor_get_delivery_timeout:
 * @self: a #GowlCompositor
 *
 * Returns: the per-step X11 delivery timeout, in milliseconds
 */
guint
gowl_compositor_get_delivery_timeout(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), 0);
	return self->delivery_timeout_ms;
}
