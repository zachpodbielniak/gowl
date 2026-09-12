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
 * keyboard-shortcuts-inhibit-v1.
 *
 * A VM console, a VNC or RDP viewer, a game: something that wants
 * Super+1 to reach the guest rather than switch the host's tag.  A
 * client asks per surface; the compositor grants it while -- and only
 * while -- that surface holds the keyboard.  Granting it to an
 * unfocused surface would let a background window disable the
 * compositor's keys, so the sync runs on every focus change and the
 * key handler asks gowl_shortcuts_inhibited() before it looks at a
 * single bind.
 *
 * What stays outside the inhibitor: the two escape hatches
 * (Super+Escape for input capture, Super+Shift+Escape for a
 * recording), the session lock, and the Emacs prefix-key policy under
 * cmacs -- those are the compositor's promise to the person at the
 * keyboard, not shortcuts a client may take.
 */

#include "gowl-core-private.h"

static struct wlr_surface *
focused_surface(GowlCompositor *self)
{
	if (self->wlr_seat == NULL)
		return NULL;
	return self->wlr_seat->keyboard_state.focused_surface;
}

/**
 * gowl_shortcuts_inhibit_sync:
 * @self: the compositor
 *
 * Activates the inhibitor of the focused surface, if it has one, and
 * deactivates every other.  Called after every keyboard focus change.
 */
void
gowl_shortcuts_inhibit_sync(GowlCompositor *self)
{
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inh;
	struct wlr_surface *focus;

	if (self->shortcuts_inhibit_mgr == NULL)
		return;

	focus = focused_surface(self);
	wl_list_for_each(inh, &self->shortcuts_inhibit_mgr->inhibitors, link) {
		gboolean want = focus != NULL && inh->surface == focus;

		if (want && !inh->active)
			wlr_keyboard_shortcuts_inhibitor_v1_activate(inh);
		else if (!want && inh->active)
			wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inh);
	}
}

/**
 * gowl_shortcuts_inhibited:
 * @self: the compositor
 *
 * Returns: %TRUE if the focused surface holds an active inhibitor,
 *   in which case the key handler forwards every key to it and runs
 *   no keybind, module bind or embedder intercept.
 */
gboolean
gowl_shortcuts_inhibited(GowlCompositor *self)
{
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inh;
	struct wlr_surface *focus;

	if (self->shortcuts_inhibit_mgr == NULL || self->locked)
		return FALSE;

	focus = focused_surface(self);
	if (focus == NULL)
		return FALSE;

	wl_list_for_each(inh, &self->shortcuts_inhibit_mgr->inhibitors, link)
		if (inh->active && inh->surface == focus)
			return TRUE;
	return FALSE;
}

static void
on_new_shortcuts_inhibitor(struct wl_listener *listener, void *data)
{
	GowlCompositor *self =
		wl_container_of(listener, self, new_shortcuts_inhibitor);
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inh = data;

	/* The protocol wants an answer: active now if it is the focused
	 * surface asking, otherwise it waits (inactive) until focus comes
	 * to it and the sync above turns it on. */
	if (inh->surface != NULL && inh->surface == focused_surface(self))
		wlr_keyboard_shortcuts_inhibitor_v1_activate(inh);
}

/**
 * gowl_shortcuts_inhibit_init:
 * @self: the compositor, during gowl_compositor_start()
 */
void
gowl_shortcuts_inhibit_init(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->shortcuts_inhibit_mgr =
		wlr_keyboard_shortcuts_inhibit_v1_create(self->wl_display);
	if (self->shortcuts_inhibit_mgr == NULL) {
		g_warning("keyboard-shortcuts-inhibit: manager not created");
		return;
	}
	self->new_shortcuts_inhibitor.notify = on_new_shortcuts_inhibitor;
	wl_signal_add(&self->shortcuts_inhibit_mgr->events.new_inhibitor,
	              &self->new_shortcuts_inhibitor);
}

/**
 * gowl_shortcuts_inhibit_finish:
 * @self: the compositor, during teardown
 */
void
gowl_shortcuts_inhibit_finish(GowlCompositor *self)
{
	if (self->shortcuts_inhibit_mgr == NULL)
		return;
	wl_list_remove(&self->new_shortcuts_inhibitor.link);
	wl_list_init(&self->new_shortcuts_inhibitor.link);
	self->shortcuts_inhibit_mgr = NULL;
}
