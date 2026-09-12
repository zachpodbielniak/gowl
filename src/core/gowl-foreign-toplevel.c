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
 * wlr-foreign-toplevel-management-v1.
 *
 * The window list as a taskbar, a dock, `wlrctl toplevel focus' or a
 * screen-share window picker sees it: one handle per managed window
 * carrying title, app-id, the outputs it is on and whether it is
 * active or fullscreen.  The ext-foreign-toplevel-list gowl already
 * served (gowl-capture-wlroots.c) is the read-only half of this; the
 * requests are what was missing -- activate, close, fullscreen --
 * and they land on the same paths a keybind uses, so a taskbar click
 * obeys the same `focus-on-activate' policy as xdg-activation.
 *
 * What is NOT listed: embedded clients (Emacs owns them; a taskbar
 * that raised one would fight the editor), unmanaged X11 popups, and
 * a module's overlays (the dropdown, a scratchpad member) -- a
 * taskbar entry for a hidden quake terminal would be a lie.  Minimize
 * and maximize requests are acknowledged and ignored: gowl has
 * neither state.
 */

#include "gowl-core-private.h"

static gboolean
listed(GowlClient *c)
{
	if (c->isembedded || c->isoverlay)
		return FALSE;
#ifdef GOWL_HAVE_XWAYLAND
	if (c->xwayland_surface != NULL
	    && c->xwayland_surface->override_redirect)
		return FALSE;
#endif
	return TRUE;
}

static void
on_ft_request_activate(struct wl_listener *listener, void *data)
{
	GowlClient *c = wl_container_of(listener, c, ft_request_activate);
	(void)data;

	if (c->compositor != NULL)
		gowl_compositor_activate_client(c->compositor, c);
}

static void
on_ft_request_close(struct wl_listener *listener, void *data)
{
	GowlClient *c = wl_container_of(listener, c, ft_request_close);
	(void)data;

	gowl_client_close(c);
}

static void
on_ft_request_fullscreen(struct wl_listener *listener, void *data)
{
	GowlClient *c = wl_container_of(listener, c, ft_request_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;

	if (c->compositor != NULL)
		gowl_compositor_set_client_fullscreen(c->compositor, c,
		                                      ev->fullscreen);
}

/**
 * gowl_foreign_toplevel_init:
 * @self: the compositor, during gowl_compositor_start()
 */
void
gowl_foreign_toplevel_init(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->foreign_toplevel_mgr =
		wlr_foreign_toplevel_manager_v1_create(self->wl_display);
	if (self->foreign_toplevel_mgr == NULL)
		g_warning("foreign-toplevel: manager not created");
}

/**
 * gowl_foreign_toplevel_client_map:
 * @self: the compositor
 * @c: a client that just mapped and is in the managed lists
 *
 * Creates the handle.  Called from on_client_map() after the rules and
 * the embedder have had their say, since both decide whether the
 * window is listed at all.
 */
void
gowl_foreign_toplevel_client_map(
	GowlCompositor *self,
	GowlClient     *c
){
	struct wlr_foreign_toplevel_handle_v1 *h;

	if (self->foreign_toplevel_mgr == NULL || c->foreign_toplevel != NULL
	    || !listed(c))
		return;

	h = wlr_foreign_toplevel_handle_v1_create(self->foreign_toplevel_mgr);
	if (h == NULL)
		return;
	c->foreign_toplevel = h;
	h->data = c;

	wlr_foreign_toplevel_handle_v1_set_title(h,
		c->title != NULL ? c->title : "");
	wlr_foreign_toplevel_handle_v1_set_app_id(h,
		c->app_id != NULL ? c->app_id : "");
	wlr_foreign_toplevel_handle_v1_set_fullscreen(h, c->isfullscreen);
	if (c->mon != NULL && c->mon->wlr_output != NULL)
		wlr_foreign_toplevel_handle_v1_output_enter(h,
			c->mon->wlr_output);

	c->ft_request_activate.notify = on_ft_request_activate;
	wl_signal_add(&h->events.request_activate, &c->ft_request_activate);
	c->ft_request_close.notify = on_ft_request_close;
	wl_signal_add(&h->events.request_close, &c->ft_request_close);
	c->ft_request_fullscreen.notify = on_ft_request_fullscreen;
	wl_signal_add(&h->events.request_fullscreen,
	              &c->ft_request_fullscreen);
}

/**
 * gowl_foreign_toplevel_client_unmap:
 * @c: a client on its way out
 *
 * Destroys the handle, taking the listeners off first.  Safe to call
 * for a client that never had one.
 */
void
gowl_foreign_toplevel_client_unmap(GowlClient *c)
{
	if (c->foreign_toplevel == NULL)
		return;
	wl_list_remove(&c->ft_request_activate.link);
	wl_list_remove(&c->ft_request_close.link);
	wl_list_remove(&c->ft_request_fullscreen.link);
	wlr_foreign_toplevel_handle_v1_destroy(c->foreign_toplevel);
	c->foreign_toplevel = NULL;
}

void
gowl_foreign_toplevel_client_title(GowlClient *c)
{
	if (c->foreign_toplevel == NULL)
		return;
	wlr_foreign_toplevel_handle_v1_set_title(c->foreign_toplevel,
		c->title != NULL ? c->title : "");
	wlr_foreign_toplevel_handle_v1_set_app_id(c->foreign_toplevel,
		c->app_id != NULL ? c->app_id : "");
}

void
gowl_foreign_toplevel_client_activated(
	GowlClient *c,
	gboolean    activated
){
	if (c->foreign_toplevel == NULL)
		return;
	wlr_foreign_toplevel_handle_v1_set_activated(c->foreign_toplevel,
	                                             activated);
}

void
gowl_foreign_toplevel_client_fullscreen(
	GowlClient *c,
	gboolean    fullscreen
){
	if (c->foreign_toplevel == NULL)
		return;
	wlr_foreign_toplevel_handle_v1_set_fullscreen(c->foreign_toplevel,
	                                              fullscreen);
}

void
gowl_foreign_toplevel_client_monitor(
	GowlClient  *c,
	GowlMonitor *old_mon,
	GowlMonitor *new_mon
){
	if (c->foreign_toplevel == NULL)
		return;
	if (old_mon != NULL && old_mon->wlr_output != NULL)
		wlr_foreign_toplevel_handle_v1_output_leave(c->foreign_toplevel,
			old_mon->wlr_output);
	if (new_mon != NULL && new_mon->wlr_output != NULL)
		wlr_foreign_toplevel_handle_v1_output_enter(c->foreign_toplevel,
			new_mon->wlr_output);
}
