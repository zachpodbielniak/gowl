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
 * Input methods: text-input-v3, input-method-v2, and the virtual
 * keyboards that come with them.
 *
 * The compositor is a relay.  An application with a text field binds
 * text-input-v3 and says "I am typing here, this is the text around
 * the cursor, the cursor is at this rectangle".  The input method --
 * fcitx5, ibus's Wayland front end, an on-screen keyboard -- binds
 * input-method-v2 and answers with pre-edit text, committed text and
 * deletions.  Neither sees the other; everything crosses here, and
 * the relay decides which text input the input method is talking to:
 * the one on the surface that holds the keyboard.
 *
 * Three more pieces make it usable rather than merely present:
 *
 *   - the KEYBOARD GRAB.  The input method asks for every key while it
 *     is active, so that typing "ni hao" produces pinyin candidates
 *     rather than the letters.  The key handler in gowl-compositor.c
 *     asks gowl_text_input_grab_key() before forwarding a key to the
 *     focused client; a grabbed key goes to the input method instead.
 *     Compositor keybinds still run first: Super+1 switches tags even
 *     mid-composition.
 *
 *   - VIRTUAL KEYBOARDS.  A key the input method does not want (an
 *     arrow, Return, a letter in Latin mode) comes back through a
 *     zwp_virtual_keyboard_v1 it created.  gowl never listened for
 *     virtual keyboards, deliberately: docs/input-recording.org
 *     records why (a virtual keyboard that joins the keyboard group
 *     becomes indistinguishable from hardware to the recorder).  So
 *     they are handled HERE and kept apart: one from the input
 *     method's own client is forwarded straight to the focused
 *     surface -- it has already been through the keybinds once, as a
 *     real key -- and any other (wtype, a KVM) goes through
 *     gowl_compositor_inject_key(), the synthetic path, which the
 *     recorder does not tap.
 *
 *   - POPUPS.  The candidate window is an input-method popup surface
 *     the compositor places itself, under the text cursor of the
 *     focused window, flipped above it at the bottom of the screen.
 *     It lives on the overlay layer so it shows over a fullscreen
 *     window too.
 *
 * Modelled on sway's text_input.c; simplified to one seat and one
 * input method, which is what the protocol allows per seat anyway.
 */

#include "gowl-core-private.h"
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>

typedef struct _GowlTextInputRelay GowlTextInputRelay;

/* What a text input is waiting to do once the client is ready */
typedef enum {
	GOWL_TEXT_PENDING_NONE = 0,
	GOWL_TEXT_PENDING_EDIT,
	GOWL_TEXT_PENDING_READ
} GowlTextPending;

/* The most text one commit_string carries.  A Wayland message is at
   most 4096 bytes on the wire, header and length word included, and
   libwayland answers a longer one by disconnecting the client it was
   for -- the application the user was typing into.  So text is cut on
   character boundaries well inside that. */
#define GOWL_TEXT_EDIT_CHUNK_BYTES (3072)

/* text-input-v3 limits surrounding text to 4000 bytes, and a client
   with more sends a window around the cursor.  Toolkits cut that
   window at the limit or within a few characters of it (GTK: at most
   4000, never less than 4000 minus one character per cut end), so
   text this long or longer is taken for a window and anything shorter
   for all the client chose to send. */
#define GOWL_TEXT_SURROUNDING_PARTIAL_BYTES (3900)

typedef struct {
	GowlTextInputRelay        *relay;
	struct wlr_text_input_v3  *input;
	struct wl_listener         enable;
	struct wl_listener         commit;
	struct wl_listener         disable;
	struct wl_listener         destroy;

	/* One operation from inside the compositor waiting on the client:
	   an edit (gowl_compositor_replace_text) or a read of the field
	   (gowl_compositor_read_text_field).  `pending' says which, and is
	   the ONLY test for "something is waiting" -- a callback may be
	   NULL.  `borrowed': the compositor entered this text input only
	   for the operation, with no input method running, and leaves it
	   again afterwards.

	   An edit's text goes in pieces (GOWL_TEXT_EDIT_CHUNK_BYTES):
	   `pending_text' is what is still to send, `pending_offset' how far
	   into it the pieces have got, and each piece after the first waits
	   for the client to commit the state the last one left, so its
	   `done' carries a serial the client accepts. */
	gboolean                   borrowed;
	GowlTextPending            pending;
	gchar                     *pending_text;
	gsize                      pending_offset;
	guint                      pending_delete;
	guint                      pending_delete_after;
	gboolean                   pending_started;
	GowlTextCommitFunc         pending_func;
	GowlTextFieldReadFunc      pending_read_func;
	gpointer                   pending_data;
	struct wl_event_source    *pending_timeout;
} GowlTextInput;

typedef struct {
	GowlTextInputRelay                *relay;
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_tree             *tree;
	struct wl_listener                 destroy;
	struct wl_listener                 commit;
} GowlInputPopup;

typedef struct {
	GowlTextInputRelay           *relay;
	struct wlr_virtual_keyboard_v1 *vkb;
	struct wl_listener            key;
	struct wl_listener            modifiers;
	struct wl_listener            destroy;
} GowlVirtualKeyboard;

struct _GowlTextInputRelay {
	GowlCompositor *compositor;

	struct wlr_text_input_manager_v3   *text_input_mgr;
	struct wlr_input_method_manager_v2 *input_method_mgr;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wl_listener new_text_input;
	struct wl_listener new_input_method;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener keyboard_focus_change;

	GList *text_inputs;      /* GowlTextInput* */
	GList *popups;           /* GowlInputPopup* */
	GList *virtual_keyboards;/* GowlVirtualKeyboard* */

	struct wlr_input_method_v2 *input_method;
	struct wl_listener im_commit;
	struct wl_listener im_new_popup;
	struct wl_listener im_grab_keyboard;
	struct wl_listener im_destroy;
	struct wl_listener grab_destroy;
};

static void relay_send_state(GowlTextInputRelay *relay, GowlTextInput *ti);
static void popup_place(GowlInputPopup *p);
static gboolean ti_pending_step(GowlTextInput *ti);
static void ti_finish_pending(GowlTextInput *ti, gboolean delivered);

/* -----------------------------------------------------------
 * Which text input is the one
 * ----------------------------------------------------------- */

static struct wlr_surface *
relay_focused_surface(GowlTextInputRelay *relay)
{
	GowlCompositor *self = relay->compositor;

	if (self->wlr_seat == NULL)
		return NULL;
	return self->wlr_seat->keyboard_state.focused_surface;
}

/* The enabled text input on the surface that holds the keyboard. */
static GowlTextInput *
relay_active_text_input(GowlTextInputRelay *relay)
{
	GList *l;

	for (l = relay->text_inputs; l != NULL; l = l->next) {
		GowlTextInput *ti = (GowlTextInput *)l->data;

		if (ti->input->focused_surface != NULL
		    && ti->input->current_enabled)
			return ti;
	}
	return NULL;
}

static void
relay_deactivate_im(GowlTextInputRelay *relay)
{
	if (relay->input_method == NULL || !relay->input_method->active)
		return;
	wlr_input_method_v2_send_deactivate(relay->input_method);
	wlr_input_method_v2_send_done(relay->input_method);
}

static void
relay_activate_im(GowlTextInputRelay *relay, GowlTextInput *ti)
{
	if (relay->input_method == NULL)
		return;
	if (!relay->input_method->active)
		wlr_input_method_v2_send_activate(relay->input_method);
	relay_send_state(relay, ti);
}

/* Forward what the application said about its text to the input
 * method, then `done' so it acts on it as one update. */
static void
relay_send_state(GowlTextInputRelay *relay, GowlTextInput *ti)
{
	struct wlr_input_method_v2 *im = relay->input_method;
	struct wlr_text_input_v3 *input = ti->input;
	GList *l;

	if (im == NULL)
		return;

	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
		wlr_input_method_v2_send_surrounding_text(im,
			input->current.surrounding.text,
			input->current.surrounding.cursor,
			input->current.surrounding.anchor);
	wlr_input_method_v2_send_text_change_cause(im,
		input->current.text_change_cause);
	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
		wlr_input_method_v2_send_content_type(im,
			input->current.content_type.hint,
			input->current.content_type.purpose);
	wlr_input_method_v2_send_done(im);

	/* The cursor moved: the candidate window follows. */
	for (l = relay->popups; l != NULL; l = l->next)
		popup_place((GowlInputPopup *)l->data);
}

/* -----------------------------------------------------------
 * text-input-v3
 * ----------------------------------------------------------- */

static void
on_text_input_enable(struct wl_listener *listener, void *data)
{
	GowlTextInput *ti = wl_container_of(listener, ti, enable);
	(void)data;

	/* The client enabled the text input gowl entered for an edit or a
	   read (the first enabling commit is signalled as `enable', not
	   `commit') */
	if (ti->pending != GOWL_TEXT_PENDING_NONE) {
		gboolean was_borrowed = ti->borrowed;

		ti_pending_step(ti);
		if (was_borrowed)
			return;
	}
	if (ti->relay->input_method == NULL) {
		g_debug("text-input: enabled with no input method");
		return;
	}
	relay_activate_im(ti->relay, ti);
}

static void
on_text_input_commit(struct wl_listener *listener, void *data)
{
	GowlTextInput *ti = wl_container_of(listener, ti, commit);
	(void)data;

	/* The client enabled the text input it was entered for, or has
	   committed what the last piece of an edit left: the next step
	   goes now.  A borrowed text input has no input method behind it
	   to relay to. */
	if (ti->pending != GOWL_TEXT_PENDING_NONE
	    && ti->input->current_enabled) {
		gboolean was_borrowed = ti->borrowed;

		ti_pending_step(ti);
		if (was_borrowed)
			return;
	}
	if (!ti->input->current_enabled || ti->relay->input_method == NULL)
		return;
	relay_send_state(ti->relay, ti);
}

static void
on_text_input_disable(struct wl_listener *listener, void *data)
{
	GowlTextInput *ti = wl_container_of(listener, ti, disable);
	(void)data;

	/* Only the active one deactivates the input method: another text
	 * input of the same client going quiet is not a change. */
	if (relay_active_text_input(ti->relay) == NULL)
		relay_deactivate_im(ti->relay);
}

static void
on_text_input_destroy(struct wl_listener *listener, void *data)
{
	GowlTextInput *ti = wl_container_of(listener, ti, destroy);
	GowlTextInputRelay *relay = ti->relay;
	(void)data;

	/* Gone with an edit or a read waiting: it is not delivered (and
	   there is nothing left to leave) */
	ti->borrowed = FALSE;
	if (ti->pending != GOWL_TEXT_PENDING_NONE)
		ti_finish_pending(ti, FALSE);
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->destroy.link);
	relay->text_inputs = g_list_remove(relay->text_inputs, ti);
	g_free(ti);

	if (relay_active_text_input(relay) == NULL)
		relay_deactivate_im(relay);
}

static void
on_new_text_input(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay =
		wl_container_of(listener, relay, new_text_input);
	struct wlr_text_input_v3 *input = data;
	struct wlr_surface *focus;
	GowlTextInput *ti;

	ti = g_new0(GowlTextInput, 1);
	ti->relay = relay;
	ti->input = input;
	ti->enable.notify = on_text_input_enable;
	wl_signal_add(&input->events.enable, &ti->enable);
	ti->commit.notify = on_text_input_commit;
	wl_signal_add(&input->events.commit, &ti->commit);
	ti->disable.notify = on_text_input_disable;
	wl_signal_add(&input->events.disable, &ti->disable);
	ti->destroy.notify = on_text_input_destroy;
	wl_signal_add(&input->events.destroy, &ti->destroy);
	relay->text_inputs = g_list_prepend(relay->text_inputs, ti);

	/* A text input created by the client that already has the
	 * keyboard is entered at once: the focus change that would have
	 * told it has already happened. */
	focus = relay_focused_surface(relay);
	if (focus != NULL && relay->input_method != NULL
	    && wl_resource_get_client(input->resource)
	       == wl_resource_get_client(focus->resource))
		wlr_text_input_v3_send_enter(input, focus);
}

/* -----------------------------------------------------------
 * Keyboard focus
 *
 * Enter is sent to every text input of the client that gained the
 * keyboard, leave to every text input that had a surface; and only
 * while an input method exists -- a text input that is entered with
 * nobody behind it would have the application waiting on an IME that
 * is not there.
 * ----------------------------------------------------------- */

static void
relay_set_focus(GowlTextInputRelay *relay, struct wlr_surface *surface)
{
	GList *l;
	gboolean had_active = relay_active_text_input(relay) != NULL;

	for (l = relay->text_inputs; l != NULL; l = l->next) {
		GowlTextInput *ti = (GowlTextInput *)l->data;
		struct wlr_text_input_v3 *input = ti->input;

		if (input->focused_surface != NULL) {
			if (input->focused_surface == surface)
				continue;
			wlr_text_input_v3_send_leave(input);
		}
		if (surface != NULL && relay->input_method != NULL
		    && wl_resource_get_client(input->resource)
		       == wl_resource_get_client(surface->resource))
			wlr_text_input_v3_send_enter(input, surface);
	}

	if (had_active && relay_active_text_input(relay) == NULL)
		relay_deactivate_im(relay);
}

static void
on_keyboard_focus_change(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay =
		wl_container_of(listener, relay, keyboard_focus_change);
	struct wlr_seat_keyboard_focus_change_event *ev = data;

	relay_set_focus(relay, ev->new_surface);
}

/* -----------------------------------------------------------
 * input-method-v2
 * ----------------------------------------------------------- */

static void
on_im_commit(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay = wl_container_of(listener, relay, im_commit);
	struct wlr_input_method_v2 *im = data;
	GowlTextInput *ti;
	struct wlr_text_input_v3 *input;

	ti = relay_active_text_input(relay);
	if (ti == NULL)
		return;
	input = ti->input;

	if (im->current.preedit.text != NULL)
		wlr_text_input_v3_send_preedit_string(input,
			im->current.preedit.text,
			im->current.preedit.cursor_begin,
			im->current.preedit.cursor_end);
	if (im->current.commit_text != NULL)
		wlr_text_input_v3_send_commit_string(input,
			im->current.commit_text);
	if (im->current.delete.before_length != 0
	    || im->current.delete.after_length != 0)
		wlr_text_input_v3_send_delete_surrounding_text(input,
			im->current.delete.before_length,
			im->current.delete.after_length);
	wlr_text_input_v3_send_done(input);
}

static void
on_grab_destroy(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay = wl_container_of(listener, relay, grab_destroy);
	(void)data;

	wl_list_remove(&relay->grab_destroy.link);
	wl_list_init(&relay->grab_destroy.link);
}

static void
on_im_grab_keyboard(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay =
		wl_container_of(listener, relay, im_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	struct wlr_keyboard *kb;

	/* The grab gets the seat's keyboard: its keymap and repeat info
	 * go to the input method, which needs them to interpret the
	 * keycodes it is about to receive. */
	kb = wlr_seat_get_keyboard(relay->compositor->wlr_seat);
	if (kb != NULL)
		wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
	relay->grab_destroy.notify = on_grab_destroy;
	wl_signal_add(&grab->events.destroy, &relay->grab_destroy);
}

static void
on_im_destroy(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay = wl_container_of(listener, relay, im_destroy);
	GList *l;
	(void)data;

	wl_list_remove(&relay->im_commit.link);
	wl_list_remove(&relay->im_new_popup.link);
	wl_list_remove(&relay->im_grab_keyboard.link);
	wl_list_remove(&relay->im_destroy.link);
	if (!wl_list_empty(&relay->grab_destroy.link)) {
		wl_list_remove(&relay->grab_destroy.link);
		wl_list_init(&relay->grab_destroy.link);
	}
	relay->input_method = NULL;

	/* No input method: the applications must not wait on one. */
	for (l = relay->text_inputs; l != NULL; l = l->next) {
		GowlTextInput *ti = (GowlTextInput *)l->data;

		if (ti->input->focused_surface != NULL)
			wlr_text_input_v3_send_leave(ti->input);
	}
}

static void
on_new_input_method(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay =
		wl_container_of(listener, relay, new_input_method);
	struct wlr_input_method_v2 *im = data;
	struct wlr_surface *focus;
	GowlTextInput *ti;

	/* One per seat, the protocol says; a second one is told so. */
	if (relay->input_method != NULL) {
		g_warning("input-method: a second input method connected; "
		          "told it the seat is taken");
		wlr_input_method_v2_send_unavailable(im);
		return;
	}
	relay->input_method = im;
	relay->im_commit.notify = on_im_commit;
	wl_signal_add(&im->events.commit, &relay->im_commit);
	relay->im_new_popup.notify = NULL; /* set below */
	relay->im_grab_keyboard.notify = on_im_grab_keyboard;
	wl_signal_add(&im->events.grab_keyboard, &relay->im_grab_keyboard);
	relay->im_destroy.notify = on_im_destroy;
	wl_signal_add(&im->events.destroy, &relay->im_destroy);

	/* Popups are wired in a moment; the listener has to exist before
	 * the signal is added to. */
	{
		extern void gowl_text_input_popup_listen(GowlTextInputRelay *relay);
		gowl_text_input_popup_listen(relay);
	}

	/* Text inputs the focused client already has are entered now,
	 * and one that was enabled while there was nobody to talk to
	 * gets its input method. */
	focus = relay_focused_surface(relay);
	if (focus != NULL)
		relay_set_focus(relay, focus);
	ti = relay_active_text_input(relay);
	if (ti != NULL)
		relay_activate_im(relay, ti);
}

/* -----------------------------------------------------------
 * The keyboard grab, as the key handler sees it
 * ----------------------------------------------------------- */

/**
 * gowl_text_input_grab_key:
 * @self: the compositor
 * @kb: the keyboard the key came from
 * @time_msec: the event time
 * @keycode: evdev keycode
 * @state: WL_KEYBOARD_KEY_STATE_*
 *
 * Returns: %TRUE if the input method holds a keyboard grab and took
 *   the key, in which case it must not be forwarded to the client
 */
gboolean
gowl_text_input_grab_key(
	GowlCompositor      *self,
	struct wlr_keyboard *kb,
	guint32              time_msec,
	guint32              keycode,
	guint32              state
){
	GowlTextInputRelay *relay = (GowlTextInputRelay *)self->text_input_relay;
	struct wlr_input_method_keyboard_grab_v2 *grab;

	if (relay == NULL || relay->input_method == NULL)
		return FALSE;
	grab = relay->input_method->keyboard_grab;
	if (grab == NULL)
		return FALSE;
	/* The input method's own virtual keyboard sending a key back: it
	 * already declined that one.  Handing it over again would loop. */
	if (kb != NULL && kb != grab->keyboard) {
		struct wlr_virtual_keyboard_v1 *vkb =
			wlr_input_device_get_virtual_keyboard(&kb->base);

		if (vkb != NULL && wl_resource_get_client(vkb->resource)
		    == wl_resource_get_client(relay->input_method->resource))
			return FALSE;
	}
	wlr_input_method_keyboard_grab_v2_send_key(grab, time_msec, keycode,
	                                            state);
	return TRUE;
}

/**
 * gowl_text_input_grab_modifiers:
 * @self: the compositor
 * @kb: the keyboard whose modifiers changed
 *
 * Returns: %TRUE if a grab took the modifier update
 */
gboolean
gowl_text_input_grab_modifiers(
	GowlCompositor      *self,
	struct wlr_keyboard *kb
){
	GowlTextInputRelay *relay = (GowlTextInputRelay *)self->text_input_relay;
	struct wlr_input_method_keyboard_grab_v2 *grab;

	if (relay == NULL || relay->input_method == NULL || kb == NULL)
		return FALSE;
	grab = relay->input_method->keyboard_grab;
	if (grab == NULL)
		return FALSE;
	wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &kb->modifiers);
	return TRUE;
}

/* -----------------------------------------------------------
 * Popups (the candidate window)
 * ----------------------------------------------------------- */

static GowlClient *
relay_focused_client(GowlTextInputRelay *relay, struct wlr_surface **out)
{
	GowlCompositor *self = relay->compositor;
	GowlTextInput *ti = relay_active_text_input(relay);
	struct wlr_surface *surface;
	GList *l;

	if (ti == NULL || ti->input->focused_surface == NULL)
		return NULL;
	surface = wlr_surface_get_root_surface(ti->input->focused_surface);
	if (out != NULL)
		*out = surface;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (gowl_client_get_wlr_surface(c) == surface)
			return c;
	}
	return NULL;
}

static void
popup_place(GowlInputPopup *p)
{
	GowlTextInputRelay *relay = p->relay;
	GowlTextInput *ti = relay_active_text_input(relay);
	GowlClient *c;
	struct wlr_box cursor;
	struct wlr_box area;
	gint sx = 0, sy = 0;
	gint x, y, w, h;

	if (p->tree == NULL)
		return;
	c = relay_focused_client(relay, NULL);
	if (ti == NULL || c == NULL || c->scene_surface == NULL) {
		wlr_scene_node_set_enabled(&p->tree->node, FALSE);
		return;
	}
	wlr_scene_node_set_enabled(&p->tree->node, TRUE);

	/* Where the application's text cursor is, in layout coordinates:
	 * the surface's origin plus the rectangle it reported.  A client
	 * that reports none gets the popup at its top-left. */
	wlr_scene_node_coords(&c->scene_surface->node, &sx, &sy);
	cursor = ti->input->current.cursor_rectangle;
	if (!(ti->input->active_features
	      & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)) {
		cursor.x = cursor.y = 0;
		cursor.width = cursor.height = 0;
	}

	w = p->popup->surface->current.width;
	h = p->popup->surface->current.height;
	x = sx + cursor.x;
	y = sy + cursor.y + cursor.height;

	/* Keep it on the monitor: flip above the cursor at the bottom
	 * edge, slide left at the right edge. */
	area = c->mon != NULL ? c->mon->m : (struct wlr_box){ 0, 0, 0, 0 };
	if (area.width > 0) {
		if (y + h > area.y + area.height && sy + cursor.y - h >= area.y)
			y = sy + cursor.y - h;
		if (x + w > area.x + area.width)
			x = MAX(area.x, area.x + area.width - w);
	}
	wlr_scene_node_set_position(&p->tree->node, x, y);

	/* Tell the popup where the cursor is relative to itself, which is
	 * how it draws its little arrow. */
	cursor.x = sx + cursor.x - x;
	cursor.y = sy + cursor.y - y;
	wlr_input_popup_surface_v2_send_text_input_rectangle(p->popup, &cursor);
}

static void
on_popup_commit(struct wl_listener *listener, void *data)
{
	GowlInputPopup *p = wl_container_of(listener, p, commit);
	(void)data;

	/* The size is only known once it has drawn itself. */
	popup_place(p);
}

static void
on_popup_destroy(struct wl_listener *listener, void *data)
{
	GowlInputPopup *p = wl_container_of(listener, p, destroy);
	GowlTextInputRelay *relay = p->relay;
	(void)data;

	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->commit.link);
	if (p->tree != NULL)
		wlr_scene_node_destroy(&p->tree->node);
	relay->popups = g_list_remove(relay->popups, p);
	g_free(p);
}

static void
on_im_new_popup(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay = wl_container_of(listener, relay, im_new_popup);
	struct wlr_input_popup_surface_v2 *popup = data;
	GowlCompositor *self = relay->compositor;
	GowlInputPopup *p;

	p = g_new0(GowlInputPopup, 1);
	p->relay = relay;
	p->popup = popup;
	p->tree = wlr_scene_subsurface_tree_create(
		self->layers[GOWL_SCENE_LAYER_OVERLAY], popup->surface);
	p->destroy.notify = on_popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
	p->commit.notify = on_popup_commit;
	wl_signal_add(&popup->surface->events.commit, &p->commit);
	relay->popups = g_list_prepend(relay->popups, p);
	popup_place(p);
}

void
gowl_text_input_popup_listen(GowlTextInputRelay *relay)
{
	relay->im_new_popup.notify = on_im_new_popup;
	wl_signal_add(&relay->input_method->events.new_popup_surface,
	              &relay->im_new_popup);
}

/* -----------------------------------------------------------
 * Virtual keyboards
 * ----------------------------------------------------------- */

static gboolean
vkb_is_input_methods(GowlVirtualKeyboard *v)
{
	GowlTextInputRelay *relay = v->relay;

	return relay->input_method != NULL
	       && wl_resource_get_client(v->vkb->resource)
	          == wl_resource_get_client(relay->input_method->resource);
}

static void
on_vkb_key(struct wl_listener *listener, void *data)
{
	GowlVirtualKeyboard *v = wl_container_of(listener, v, key);
	struct wlr_keyboard_key_event *ev = data;
	GowlCompositor *self = v->relay->compositor;

	if (vkb_is_input_methods(v)) {
		/* Back from the input method, declined: straight to the
		 * focused surface with the virtual keyboard's own state,
		 * which carries the keymap the input method was given. */
		wlr_seat_set_keyboard(self->wlr_seat, &v->vkb->keyboard);
		wlr_seat_keyboard_notify_key(self->wlr_seat, ev->time_msec,
		                             ev->keycode, ev->state);
		return;
	}
	/* Anyone else's (wtype, a remote): the synthetic path, which runs
	 * the keybinds and skips the recorder. */
	gowl_compositor_inject_key(self, ev->keycode,
		ev->state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void
on_vkb_modifiers(struct wl_listener *listener, void *data)
{
	GowlVirtualKeyboard *v = wl_container_of(listener, v, modifiers);
	GowlCompositor *self = v->relay->compositor;
	(void)data;

	if (!vkb_is_input_methods(v))
		return;
	wlr_seat_set_keyboard(self->wlr_seat, &v->vkb->keyboard);
	wlr_seat_keyboard_notify_modifiers(self->wlr_seat,
	                                   &v->vkb->keyboard.modifiers);
}

static void
on_vkb_destroy(struct wl_listener *listener, void *data)
{
	GowlVirtualKeyboard *v = wl_container_of(listener, v, destroy);
	GowlTextInputRelay *relay = v->relay;
	GowlCompositor *self = relay->compositor;
	(void)data;

	/* The seat must not keep pointing at a keyboard that is gone. */
	if (self->wlr_seat != NULL
	    && wlr_seat_get_keyboard(self->wlr_seat) == &v->vkb->keyboard
	    && self->wlr_kb_group != NULL)
		wlr_seat_set_keyboard(self->wlr_seat,
		                      &self->wlr_kb_group->keyboard);
	wl_list_remove(&v->key.link);
	wl_list_remove(&v->modifiers.link);
	wl_list_remove(&v->destroy.link);
	relay->virtual_keyboards = g_list_remove(relay->virtual_keyboards, v);
	g_free(v);
}

static void
on_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
	GowlTextInputRelay *relay =
		wl_container_of(listener, relay, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vkb = data;
	GowlVirtualKeyboard *v;

	v = g_new0(GowlVirtualKeyboard, 1);
	v->relay = relay;
	v->vkb = vkb;
	v->key.notify = on_vkb_key;
	wl_signal_add(&vkb->keyboard.events.key, &v->key);
	v->modifiers.notify = on_vkb_modifiers;
	wl_signal_add(&vkb->keyboard.events.modifiers, &v->modifiers);
	v->destroy.notify = on_vkb_destroy;
	wl_signal_add(&vkb->keyboard.base.events.destroy, &v->destroy);
	relay->virtual_keyboards = g_list_prepend(relay->virtual_keyboards, v);
}

/* -----------------------------------------------------------
 * Lifetime
 * ----------------------------------------------------------- */

/**
 * gowl_text_input_init:
 * @self: the compositor, during gowl_compositor_start(), after the
 *   seat exists
 *
 * Creates the text-input-v3 and input-method-v2 globals and starts
 * listening for virtual keyboards.
 */
void
gowl_text_input_init(GowlCompositor *self)
{
	GowlTextInputRelay *relay;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(self->wlr_seat != NULL);

	relay = g_new0(GowlTextInputRelay, 1);
	relay->compositor = self;
	wl_list_init(&relay->grab_destroy.link);
	wl_list_init(&relay->im_commit.link);
	wl_list_init(&relay->im_new_popup.link);
	wl_list_init(&relay->im_grab_keyboard.link);
	wl_list_init(&relay->im_destroy.link);

	relay->text_input_mgr = wlr_text_input_manager_v3_create(self->wl_display);
	relay->input_method_mgr =
		wlr_input_method_manager_v2_create(self->wl_display);
	relay->virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(self->wl_display);

	/* wlroots 0.20 renamed both managers' "a new one arrived" signals
	 * (text_input -> new_text_input, input_method -> new_input_method);
	 * the payload is the same. */
	relay->new_text_input.notify = on_new_text_input;
	relay->new_input_method.notify = on_new_input_method;
#if GOWL_HAVE_WLROOTS_0_20
	wl_signal_add(&relay->text_input_mgr->events.new_text_input,
	              &relay->new_text_input);
	wl_signal_add(&relay->input_method_mgr->events.new_input_method,
	              &relay->new_input_method);
#else
	wl_signal_add(&relay->text_input_mgr->events.text_input,
	              &relay->new_text_input);
	wl_signal_add(&relay->input_method_mgr->events.input_method,
	              &relay->new_input_method);
#endif
	relay->new_virtual_keyboard.notify = on_new_virtual_keyboard;
	wl_signal_add(&relay->virtual_keyboard_mgr->events.new_virtual_keyboard,
	              &relay->new_virtual_keyboard);
	relay->keyboard_focus_change.notify = on_keyboard_focus_change;
	wl_signal_add(&self->wlr_seat->keyboard_state.events.focus_change,
	              &relay->keyboard_focus_change);

	self->text_input_relay = relay;
}

/**
 * gowl_text_input_finish:
 * @self: the compositor, during teardown, before the display goes
 */
void
gowl_text_input_finish(GowlCompositor *self)
{
	GowlTextInputRelay *relay = (GowlTextInputRelay *)self->text_input_relay;
	GList *l;

	if (relay == NULL)
		return;

	wl_list_remove(&relay->new_text_input.link);
	wl_list_remove(&relay->new_input_method.link);
	wl_list_remove(&relay->new_virtual_keyboard.link);
	wl_list_remove(&relay->keyboard_focus_change.link);
	if (relay->input_method != NULL) {
		wl_list_remove(&relay->im_commit.link);
		wl_list_remove(&relay->im_new_popup.link);
		wl_list_remove(&relay->im_grab_keyboard.link);
		wl_list_remove(&relay->im_destroy.link);
	}
	if (!wl_list_empty(&relay->grab_destroy.link))
		wl_list_remove(&relay->grab_destroy.link);
	for (l = relay->text_inputs; l != NULL; l = l->next) {
		GowlTextInput *ti = (GowlTextInput *)l->data;

		wl_list_remove(&ti->enable.link);
		wl_list_remove(&ti->commit.link);
		wl_list_remove(&ti->disable.link);
		wl_list_remove(&ti->destroy.link);
		/* Shutting down: a waiting edit is dropped without its
		   callback, whose owner is going away too */
		if (ti->pending_timeout != NULL)
			wl_event_source_remove(ti->pending_timeout);
		g_free(ti->pending_text);
		g_free(ti);
	}
	g_list_free(relay->text_inputs);
	for (l = relay->popups; l != NULL; l = l->next) {
		GowlInputPopup *p = (GowlInputPopup *)l->data;

		wl_list_remove(&p->destroy.link);
		wl_list_remove(&p->commit.link);
		if (p->tree != NULL)
			wlr_scene_node_destroy(&p->tree->node);
		g_free(p);
	}
	g_list_free(relay->popups);
	for (l = relay->virtual_keyboards; l != NULL; l = l->next) {
		GowlVirtualKeyboard *v = (GowlVirtualKeyboard *)l->data;

		wl_list_remove(&v->key.link);
		wl_list_remove(&v->modifiers.link);
		wl_list_remove(&v->destroy.link);
		g_free(v);
	}
	g_list_free(relay->virtual_keyboards);
	g_free(relay);
	self->text_input_relay = NULL;
}

/* -----------------------------------------------------------
 * Committing text from inside the compositor
 * ----------------------------------------------------------- */

/* @client's text input: the enabled one on its surface, else -- with
   no input method running -- one of its text inputs that is entered
   nowhere and so can be borrowed.  NULL when neither. */
static GowlTextInput *
relay_text_input_for(
	GowlCompositor *self,
	GowlClient     *client,
	gboolean       *borrow
){
	GowlTextInputRelay *relay;
	struct wlr_surface *surface;
	struct wl_client *owner;
	GowlTextInput *spare = NULL;
	GList *l;

	*borrow = FALSE;
	relay = (GowlTextInputRelay *)self->text_input_relay;
	if (relay == NULL || client == NULL)
		return NULL;
	surface = gowl_client_get_wlr_surface(client);
	if (surface == NULL || surface->resource == NULL)
		return NULL;
	owner = wl_resource_get_client(surface->resource);
	for (l = relay->text_inputs; l != NULL; l = l->next) {
		GowlTextInput *ti = (GowlTextInput *)l->data;

		if (ti->input->focused_surface == surface
		    && ti->input->current_enabled)
			return ti;
		if (spare == NULL && ti->input->focused_surface == NULL
		    && ti->pending == GOWL_TEXT_PENDING_NONE
		    && wl_resource_get_client(ti->input->resource) == owner)
			spare = ti;
	}
	/* Borrowing is only for when there is no input method: with one,
	   the relay enters text inputs itself, and one that is not
	   enabled is simply not a text field */
	if (spare == NULL || relay->input_method != NULL
	    || relay_focused_surface(relay) != surface)
		return NULL;
	*borrow = TRUE;
	return spare;
}

/* The first piece of an edit, or the next: the deletions go with the
   first only, and the client applies each `done' as one change -- the
   protocol orders it, delete first, then commit, whatever order the
   events come in. */
static void
ti_send_chunk(GowlTextInput *ti)
{
	const gchar *rest;
	gsize left;
	gsize n;

	rest = (ti->pending_text != NULL)
		? ti->pending_text + ti->pending_offset : "";
	left = strlen(rest);
	n = MIN(left, (gsize)GOWL_TEXT_EDIT_CHUNK_BYTES);
	/* Never cut inside a character: back up over continuation bytes */
	while (n > 0 && n < left && (((guchar)rest[n]) & 0xC0) == 0x80)
		n--;

	if (!ti->pending_started
	    && (ti->pending_delete > 0 || ti->pending_delete_after > 0))
		wlr_text_input_v3_send_delete_surrounding_text(ti->input,
			ti->pending_delete, ti->pending_delete_after);
	if (n > 0) {
		g_autofree gchar *piece = g_strndup(rest, n);

		wlr_text_input_v3_send_commit_string(ti->input, piece);
	}
	wlr_text_input_v3_send_done(ti->input);
	ti->pending_started = TRUE;
	ti->pending_offset += n;
}

/* Whether the client has said what is in its field.  The feature bit
   is set by the enabling commit that carried set_surrounding_text. */
static gboolean
ti_has_surrounding(GowlTextInput *ti)
{
	return ti->input->current_enabled
	       && (ti->input->active_features
	           & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) != 0;
}

/* The client is ready for the waiting operation's next step: an edit
   sends a piece, a read looks for the surrounding text.  Returns TRUE
   when that finished the operation (the callback has run and @ti may
   have been given another), FALSE when it still waits. */
static gboolean
ti_pending_step(GowlTextInput *ti)
{
	switch (ti->pending) {
	case GOWL_TEXT_PENDING_EDIT:
		ti_send_chunk(ti);
		if (ti->pending_text == NULL
		    || ti->pending_text[ti->pending_offset] == '\0') {
			ti_finish_pending(ti, TRUE);
			return TRUE;
		}
		/* More to send: it waits for the client to commit what this
		   piece left, and the wait starts again */
		if (ti->pending_timeout != NULL)
			wl_event_source_timer_update(ti->pending_timeout,
			                             GOWL_TEXT_COMMIT_WAIT_MS);
		return FALSE;
	case GOWL_TEXT_PENDING_READ:
		/* Enabled without saying what the text is: a later commit
		   may, and the timeout reports it if none does */
		if (!ti_has_surrounding(ti))
			return FALSE;
		ti_finish_pending(ti, TRUE);
		return TRUE;
	case GOWL_TEXT_PENDING_NONE:
	default:
		return TRUE;
	}
}

/* What the client said about its field, copied out for a reader.
   @text_out receives the copy @info->text points at; the caller frees
   it after the callback.  Offsets a client sent past the text, or
   inside a character, are pulled back: the reader is told bytes it
   can cut the text at. */
static void
ti_fill_info(
	GowlTextInput     *ti,
	gboolean           delivered,
	GowlTextFieldInfo *info,
	gchar            **text_out
){
	struct wlr_text_input_v3 *input = ti->input;
	const gchar *text;
	gsize len;
	guint cursor;
	guint anchor;

	memset(info, 0, sizeof(*info));
	*text_out = NULL;
	/* What kind of field it is goes out even when its text does not:
	   "a password field that would not say what is in it" is exactly
	   the case a reader must be able to tell from "no text" */
	if (input->current_enabled
	    && (input->active_features
	        & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) != 0) {
		info->content_hint = input->current.content_type.hint;
		info->content_purpose = input->current.content_type.purpose;
	}
	if (!delivered || !ti_has_surrounding(ti)) {
		info->status = input->current_enabled
			? GOWL_TEXT_FIELD_NO_SURROUNDING
			: GOWL_TEXT_FIELD_NO_FIELD;
		return;
	}
	text = (input->current.surrounding.text != NULL)
		? input->current.surrounding.text : "";
	/* The text is the client's: one that is not UTF-8 cannot be
	   handed on, and repairing it would move every offset */
	if (!g_utf8_validate(text, -1, NULL)) {
		info->status = GOWL_TEXT_FIELD_NO_SURROUNDING;
		return;
	}
	len = strlen(text);
	cursor = MIN(input->current.surrounding.cursor, (guint)len);
	anchor = MIN(input->current.surrounding.anchor, (guint)len);
	while (cursor > 0 && (((guchar)text[cursor]) & 0xC0) == 0x80)
		cursor--;
	while (anchor > 0 && (((guchar)text[anchor]) & 0xC0) == 0x80)
		anchor--;

	*text_out = g_strdup(text);
	info->status = GOWL_TEXT_FIELD_OK;
	info->text = *text_out;
	info->cursor = cursor;
	info->anchor = anchor;
	info->complete = len < (gsize)GOWL_TEXT_SURROUNDING_PARTIAL_BYTES;
}

/* The waiting operation is over, done or not: a borrowed text input is
   left again, and the caller told.  The text input is put back to
   "nothing waiting" BEFORE the callback, which may start the next
   operation on it. */
static void
ti_finish_pending(
	GowlTextInput *ti,
	gboolean       delivered
){
	GowlTextPending kind = ti->pending;
	GowlTextCommitFunc func = ti->pending_func;
	GowlTextFieldReadFunc read_func = ti->pending_read_func;
	gpointer data = ti->pending_data;
	GowlTextFieldInfo info;
	g_autofree gchar *text = NULL;

	memset(&info, 0, sizeof(info));
	if (kind == GOWL_TEXT_PENDING_READ)
		ti_fill_info(ti, delivered, &info, &text);

	if (ti->pending_timeout != NULL) {
		wl_event_source_remove(ti->pending_timeout);
		ti->pending_timeout = NULL;
	}
	g_clear_pointer(&ti->pending_text, g_free);
	ti->pending = GOWL_TEXT_PENDING_NONE;
	ti->pending_offset = 0;
	ti->pending_delete = 0;
	ti->pending_delete_after = 0;
	ti->pending_started = FALSE;
	ti->pending_func = NULL;
	ti->pending_read_func = NULL;
	ti->pending_data = NULL;
	if (ti->borrowed && ti->input->focused_surface != NULL)
		wlr_text_input_v3_send_leave(ti->input);
	ti->borrowed = FALSE;

	if (kind == GOWL_TEXT_PENDING_READ) {
		if (read_func != NULL)
			read_func(&info, data);
	} else if (func != NULL) {
		func(delivered, data);
	}
}

/* The client never enabled the text input (it has no text field), or
   stopped answering part way through */
static gint
on_pending_timeout(gpointer data)
{
	GowlTextInput *ti = data;

	/* A wl_event_loop timer is not removed by firing: finishing
	   removes it, which libwayland allows from its own callback */
	ti_finish_pending(ti, FALSE);
	return 0;
}

/* Start the wait every pending operation has: without it a client that
   never answers would hold its text input for good */
static void
ti_arm_timeout(
	GowlCompositor *self,
	GowlTextInput  *ti
){
	ti->pending_timeout = wl_event_loop_add_timer(
		gowl_compositor_get_event_loop(self),
		(wl_event_loop_timer_func_t)on_pending_timeout, ti);
	if (ti->pending_timeout != NULL)
		wl_event_source_timer_update(ti->pending_timeout,
		                             GOWL_TEXT_COMMIT_WAIT_MS);
}

/**
 * gowl_compositor_client_has_text_input:
 * @self: a #GowlCompositor
 * @client: a window
 *
 * Whether gowl_compositor_commit_text() may reach @client: it has a
 * text-input-v3 enabled on its surface right now, or -- with no input
 * method running -- a text input gowl can enter for the length of one
 * edit.  The second is a promise of an attempt, not of a text field:
 * an application with nothing focused that takes text never enables
 * it, and the commit then reports it was not delivered.
 *
 * Returns: %TRUE if text can be committed to @client
 */
gboolean
gowl_compositor_client_has_text_input(
	GowlCompositor *self,
	GowlClient     *client
){
	gboolean borrow;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	return relay_text_input_for(self, client, &borrow) != NULL;
}

/**
 * gowl_compositor_replace_text:
 * @self: a #GowlCompositor
 * @client: the window, which must hold the keyboard focus
 * @delete_before: bytes of text before the cursor to remove first
 * @delete_after: bytes of text after the cursor to remove first
 * @text: (nullable): UTF-8 text to insert at the cursor
 * @func: (scope async) (nullable): called once when a
 *   %GOWL_TEXT_COMMIT_PENDING edit is delivered or given up on
 * @user_data: (closure func): data for @func
 *
 * Replaces text in @client's focused text field the way an input
 * method does: one delete_surrounding_text, reaching both ways from the
 * cursor, and one commit_string, applied together on `done'.  Unlike
 * typed keys this needs no keymap, so any character arrives intact (no
 * clipboard round trip for an emoji), and the change is a single edit
 * the application can undo as one.  With @delete_before and
 * @delete_after covering everything gowl_compositor_read_text_field()
 * reported, it replaces what was read.
 *
 * A text input that is enabled already gets the edit at once.  With no
 * input method running, gowl enters one of @client's text inputs for
 * the edit and leaves it straight after: the client answers the enter
 * by enabling it -- if a text field has focus -- and the edit goes then
 * (%GOWL_TEXT_COMMIT_PENDING; @func says how it went, on this thread).
 * A client that does not enable it within %GOWL_TEXT_COMMIT_WAIT_MS had
 * no text field.
 *
 * Text longer than one Wayland message can carry is sent in pieces,
 * each after the first once the client has committed the state the one
 * before left; such an edit is %GOWL_TEXT_COMMIT_PENDING even on an
 * enabled text input, and is reported undelivered if the client stops
 * answering part way -- some of the text is then in the field.
 *
 * Only one edit or read waits on a text input at a time: a second is
 * %GOWL_TEXT_COMMIT_UNAVAILABLE.
 *
 * Returns: whether the edit was sent, will be, or cannot be
 */
GowlTextCommitResult
gowl_compositor_replace_text(
	GowlCompositor     *self,
	GowlClient         *client,
	guint               delete_before,
	guint               delete_after,
	const gchar        *text,
	GowlTextCommitFunc  func,
	gpointer            user_data
){
	GowlTextInput *ti;
	gboolean borrow;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self),
	                     GOWL_TEXT_COMMIT_UNAVAILABLE);

	if (text != NULL && !g_utf8_validate(text, -1, NULL))
		return GOWL_TEXT_COMMIT_UNAVAILABLE;
	ti = relay_text_input_for(self, client, &borrow);
	if (ti == NULL || ti->pending != GOWL_TEXT_PENDING_NONE)
		return GOWL_TEXT_COMMIT_UNAVAILABLE;

	ti->pending = GOWL_TEXT_PENDING_EDIT;
	ti->pending_text = g_strdup(text);
	ti->pending_offset = 0;
	ti->pending_delete = delete_before;
	ti->pending_delete_after = delete_after;
	ti->pending_started = FALSE;
	ti->pending_data = user_data;
	ti_arm_timeout(self, ti);

	if (!borrow) {
		/* Enabled already: the first piece goes now.  An edit that
		   fits in it is over, and -- as it always was -- reported by
		   the return value alone, so the callback is kept back until
		   it is known to be needed */
		ti->pending_func = NULL;
		if (ti_pending_step(ti))
			return GOWL_TEXT_COMMIT_SENT;
		ti->pending_func = func;
		return GOWL_TEXT_COMMIT_PENDING;
	}

	/* Borrowed: entered now, edited when the client enables it */
	ti->pending_func = func;
	ti->borrowed = TRUE;
	wlr_text_input_v3_send_enter(ti->input,
	                             gowl_client_get_wlr_surface(client));
	return GOWL_TEXT_COMMIT_PENDING;
}

/**
 * gowl_compositor_commit_text:
 * @self: a #GowlCompositor
 * @client: the window, which must hold the keyboard focus
 * @delete_before: bytes of text before the cursor to remove first
 * @text: (nullable): UTF-8 text to insert at the cursor
 * @func: (scope async) (nullable): called once when a
 *   %GOWL_TEXT_COMMIT_PENDING edit is delivered or given up on
 * @user_data: (closure func): data for @func
 *
 * gowl_compositor_replace_text() deleting nothing after the cursor:
 * what an abbreviation expander needs, which only ever takes back what
 * was just typed.
 *
 * Returns: whether the edit was sent, will be, or cannot be
 */
GowlTextCommitResult
gowl_compositor_commit_text(
	GowlCompositor     *self,
	GowlClient         *client,
	guint               delete_before,
	const gchar        *text,
	GowlTextCommitFunc  func,
	gpointer            user_data
){
	return gowl_compositor_replace_text(self, client, delete_before, 0,
	                                    text, func, user_data);
}

/**
 * gowl_compositor_read_text_field:
 * @self: a #GowlCompositor
 * @client: the window, which must hold the keyboard focus
 * @func: (scope async): given the field, exactly once unless this
 *   returns %GOWL_TEXT_COMMIT_UNAVAILABLE
 * @user_data: (closure func): data for @func
 *
 * Reads the text of @client's focused text field: what the client
 * reports as text-input-v3 surrounding text, with the cursor and the
 * other end of the selection as byte offsets into it.
 *
 * A text input that is enabled already is read at once and @func runs
 * before this returns (%GOWL_TEXT_COMMIT_SENT).  With no input method
 * running, gowl borrows one of @client's text inputs exactly as
 * gowl_compositor_replace_text() does -- enter, wait for the client to
 * enable it and say what its text is, read, leave -- and @func runs
 * later, on this thread (%GOWL_TEXT_COMMIT_PENDING).  A client that has
 * not answered within %GOWL_TEXT_COMMIT_WAIT_MS is reported as
 * %GOWL_TEXT_FIELD_NO_FIELD, or %GOWL_TEXT_FIELD_NO_SURROUNDING if it
 * did enable the text input and only kept its text to itself.
 *
 * Whether the text is the whole field cannot be known, only whether it
 * was certainly cut: the protocol limits surrounding text to 4000
 * bytes, and a client with more sends a window around the cursor.
 * #GowlTextFieldInfo.complete is %FALSE for text within a few
 * characters of that limit.  %TRUE means "all the client chose to
 * send", which for some widgets is less than the field: a multi-line
 * GTK text view reports the paragraph around the cursor.  Either way
 * the offsets are true of the text given, so replacing exactly that
 * text -- @cursor bytes before, the rest after -- is always sound.
 *
 * Returns: %GOWL_TEXT_COMMIT_SENT when @func has already run,
 *   %GOWL_TEXT_COMMIT_PENDING when it will, and
 *   %GOWL_TEXT_COMMIT_UNAVAILABLE when no text input reaches @client or
 *   one is busy with another edit or read (@func is not called)
 */
GowlTextCommitResult
gowl_compositor_read_text_field(
	GowlCompositor        *self,
	GowlClient            *client,
	GowlTextFieldReadFunc  func,
	gpointer               user_data
){
	GowlTextInput *ti;
	gboolean borrow;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self),
	                     GOWL_TEXT_COMMIT_UNAVAILABLE);
	g_return_val_if_fail(func != NULL, GOWL_TEXT_COMMIT_UNAVAILABLE);

	ti = relay_text_input_for(self, client, &borrow);
	if (ti == NULL || ti->pending != GOWL_TEXT_PENDING_NONE)
		return GOWL_TEXT_COMMIT_UNAVAILABLE;

	ti->pending = GOWL_TEXT_PENDING_READ;
	ti->pending_read_func = func;
	ti->pending_data = user_data;

	if (!borrow) {
		/* Enabled already: what it last committed is the answer,
		   with or without surrounding text.  No timer was armed,
		   so there is nothing to wait out */
		ti_finish_pending(ti, TRUE);
		return GOWL_TEXT_COMMIT_SENT;
	}

	/* Borrowed: entered now, read when the client enables it */
	ti_arm_timeout(self, ti);
	ti->borrowed = TRUE;
	wlr_text_input_v3_send_enter(ti->input,
	                             gowl_client_get_wlr_surface(client));
	return GOWL_TEXT_COMMIT_PENDING;
}
