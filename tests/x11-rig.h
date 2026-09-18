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
 * A headless compositor with its own Xwayland, and an xcb connection to
 * it, for the tests that drive real X11 windows through gowl.
 *
 * Everything an X11 test needs and nothing it should have to get right
 * twice: the compositor rig, the connect-time race with the xwm (see
 * x11_rig_connect_thread), and a way to wait on an X reply while the
 * compositor keeps running.  Included by tests/test-x11-*.c; every
 * function is static so each test binary carries its own copy.
 */

#ifndef GOWL_TESTS_X11_RIG_H
#define GOWL_TESTS_X11_RIG_H

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xcb_icccm.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;

	xcb_connection_t  *conn;
	xcb_screen_t      *screen;
	xcb_atom_t         net_wm_window_type;
	xcb_atom_t         net_wm_window_type_menu;
	gint               connected;   /* 1 up, -1 failed */
} X11Rig;

/* ── The compositor half ─────────────────────────────────────────── */

static G_GNUC_UNUSED void
x11_rig_pump(X11Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(
			wl_display_get_event_loop(r->compositor->wl_display), 0);
		wl_display_flush_clients(r->compositor->wl_display);
		g_usleep(1000);
	}
}

static G_GNUC_UNUSED gboolean
x11_rig_await(X11Rig *r, gint *flag, gint seconds)
{
	gint64 deadline = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (g_atomic_int_get(flag))
			return TRUE;
		x11_rig_pump(r, 10);
	}
	return g_atomic_int_get(flag) != 0;
}

static G_GNUC_UNUSED xcb_atom_t
x11_rig_intern(xcb_connection_t *conn, const gchar *name)
{
	xcb_intern_atom_reply_t *reply;
	xcb_atom_t atom = XCB_ATOM_NONE;

	reply = xcb_intern_atom_reply(conn,
		xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL);
	if (reply != NULL) {
		atom = reply->atom;
		free(reply);
	}
	return atom;
}

/*
 * Connecting is what spawns the lazy Xwayland, and the compositor
 * thread has to be pumping while that happens -- so the connect runs
 * on a thread, and the test thread takes the connection over once it
 * is up.
 *
 * Then: not one window before the window manager is up.  This
 * connection is served from the server's first moments, before wlroots'
 * xwm has selected SubstructureRedirect on the root.  A window mapped
 * in that gap maps unmanaged, and Xwayland's WL_SURFACE_SERIAL client
 * message, sent once at realize to whoever holds the redirect, goes to
 * nobody: the X window is viewable, the wl_surface exists, and the two
 * are never paired, so gowl never sees a map.  About one run in four.
 * xwm_create() selects the redirect first and takes the WM_S0 selection
 * last, so an owner for WM_S0 means the redirect is in place, and a
 * round trip through the same server is the only ordering that counts.
 */
static G_GNUC_UNUSED gpointer
x11_rig_connect_thread(gpointer data)
{
	X11Rig *r = data;
	gint screen_nr = 0;
	xcb_atom_t wm_s0;
	gint64 deadline;
	gboolean managed = FALSE;

	r->conn = xcb_connect(r->compositor->xwayland->display_name,
	                      &screen_nr);
	if (r->conn == NULL || xcb_connection_has_error(r->conn)) {
		g_atomic_int_set(&r->connected, -1);
		return NULL;
	}
	r->screen = xcb_setup_roots_iterator(xcb_get_setup(r->conn)).data;
	r->net_wm_window_type = x11_rig_intern(r->conn, "_NET_WM_WINDOW_TYPE");
	r->net_wm_window_type_menu =
		x11_rig_intern(r->conn, "_NET_WM_WINDOW_TYPE_MENU");
	wm_s0 = x11_rig_intern(r->conn, "WM_S0");
	if (r->screen == NULL || r->net_wm_window_type == XCB_ATOM_NONE
	    || r->net_wm_window_type_menu == XCB_ATOM_NONE
	    || wm_s0 == XCB_ATOM_NONE) {
		g_atomic_int_set(&r->connected, -1);
		return NULL;
	}

	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline) {
		xcb_get_selection_owner_reply_t *owner;

		owner = xcb_get_selection_owner_reply(r->conn,
			xcb_get_selection_owner(r->conn, wm_s0), NULL);
		managed = owner != NULL && owner->owner != XCB_WINDOW_NONE;
		free(owner);
		if (managed)
			break;
		g_usleep(5000);
	}
	g_atomic_int_set(&r->connected, managed ? 1 : -1);
	return NULL;
}

/*
 * Up to "connected to a managed X server".  Returns FALSE having
 * already skipped the test, so a caller just returns.
 */
static G_GNUC_UNUSED gboolean
x11_rig_up(X11Rig *r, const gchar *tag)
{
	const gchar *parent;
	GError      *error = NULL;
	GThread     *thread;
	g_autofree gchar *tmpl = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	tmpl = g_strdup_printf("gowl-%s-XXXXXX", tag);
	r->runtime = g_build_filename(parent, tmpl, NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	r->modules = gowl_module_manager_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}
	if (r->compositor->xwayland == NULL
	    || r->compositor->xwayland->display_name == NULL) {
		g_test_skip("no Xwayland here");
		return FALSE;
	}

	/* A headless backend has no input devices; only the announcement
	 * is stood in for.  Xwayland binds the seat and needs both to
	 * deliver either kind of event to its windows. */
	wlr_seat_set_capabilities(r->compositor->wlr_seat,
	                          WL_SEAT_CAPABILITY_POINTER
	                          | WL_SEAT_CAPABILITY_KEYBOARD);

	thread = g_thread_new("x11-rig-connect", x11_rig_connect_thread, r);
	x11_rig_await(r, &r->connected, 15);
	g_thread_join(thread);
	if (g_atomic_int_get(&r->connected) < 0) {
		g_test_skip("no managed X11 connection to the compositor's "
		            "Xwayland");
		return FALSE;
	}
	return TRUE;
}

static G_GNUC_UNUSED void
x11_rig_down(X11Rig *r)
{
	if (r->conn != NULL) {
		xcb_disconnect(r->conn);
		r->conn = NULL;
	}
	if (r->compositor != NULL)
		x11_rig_pump(r, 100);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* ── The X half ───────────────────────────────────────────────────── */

/*
 * The reply to @cookie, with the compositor pumping meanwhile.  The X
 * server answers most requests on its own, but anything that goes
 * through the window manager comes back over the Wayland side, and a
 * blocking xcb_*_reply() on this thread would wait for a compositor
 * that this thread is.
 */
static G_GNUC_UNUSED void *
x11_rig_reply(X11Rig *r, unsigned int sequence)
{
	gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

	xcb_flush(r->conn);
	while (g_get_monotonic_time() < deadline) {
		void *reply = NULL;
		xcb_generic_error_t *err = NULL;

		if (xcb_poll_for_reply(r->conn, sequence, &reply, &err)) {
			free(err);
			return reply;
		}
		x11_rig_pump(r, 5);
	}
	return NULL;
}

/* Drain events the test does not care about. */
static G_GNUC_UNUSED void
x11_rig_drain(X11Rig *r)
{
	xcb_generic_event_t *ev;

	while ((ev = xcb_poll_for_event(r->conn)) != NULL)
		free(ev);
}

/* A managed toplevel: an ordinary X11 window with a title. */
static G_GNUC_UNUSED xcb_window_t
x11_rig_create_toplevel(X11Rig *r, const gchar *title,
                        gint x, gint y, gint w, gint h)
{
	xcb_window_t win;
	uint32_t values[2];

	win = xcb_generate_id(r->conn);
	values[0] = r->screen->white_pixel;
	values[1] = XCB_EVENT_MASK_FOCUS_CHANGE
	            | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	xcb_create_window(r->conn, XCB_COPY_FROM_PARENT, win, r->screen->root,
	                  (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, r->screen->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
	xcb_change_property(r->conn, XCB_PROP_MODE_REPLACE, win,
	                    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
	                    (uint32_t)strlen(title), title);
	return win;
}

/*
 * An override-redirect window.  With a background pixel, or Xwayland
 * never commits a buffer for it and gowl never sees a map.
 *
 * _NET_WM_WINDOW_TYPE_MENU is what makes it PASSIVE: wlroots'
 * override_redirect_wants_focus() answers FALSE for a menu, tooltip,
 * dropdown, combo, DND icon and the like, and TRUE for an
 * override-redirect with no such type -- a Qt confirmation panel, a
 * wine dialog.
 */
static G_GNUC_UNUSED xcb_window_t
x11_rig_create_override_redirect(X11Rig *r, gboolean passive,
                                 xcb_window_t transient_for,
                                 gint x, gint y, gint w, gint h)
{
	xcb_window_t win;
	uint32_t values[2];

	win = xcb_generate_id(r->conn);
	values[0] = r->screen->white_pixel;
	values[1] = 1;
	xcb_create_window(r->conn, XCB_COPY_FROM_PARENT, win, r->screen->root,
	                  (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, r->screen->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);
	if (passive)
		xcb_change_property(r->conn, XCB_PROP_MODE_REPLACE, win,
		                    r->net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
		                    &r->net_wm_window_type_menu);
	if (transient_for != XCB_WINDOW_NONE)
		xcb_change_property(r->conn, XCB_PROP_MODE_REPLACE, win,
		                    XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW,
		                    32, 1, &transient_for);
	return win;
}

static G_GNUC_UNUSED void
x11_rig_map(X11Rig *r, xcb_window_t win)
{
	xcb_map_window(r->conn, win);
	xcb_flush(r->conn);
}

static G_GNUC_UNUSED void
x11_rig_unmap(X11Rig *r, xcb_window_t win)
{
	xcb_unmap_window(r->conn, win);
	xcb_flush(r->conn);
}

/* The gowl client for an X window, or NULL. */
static G_GNUC_UNUSED GowlClient *
x11_rig_client_for(X11Rig *r, xcb_window_t win)
{
	GList *l;

	for (l = r->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (c->xwayland_surface != NULL
		    && c->xwayland_surface->window_id == win
		    && gowl_client_get_wlr_surface(c) != NULL)
			return c;
	}
	return NULL;
}

/*
 * An unmanaged (override-redirect) window is never in the client list;
 * its GowlClient hangs off the wlr_xwayland_surface, which is found
 * through the surface under a point.
 */
static G_GNUC_UNUSED GowlClient *
x11_rig_unmanaged_at(X11Rig *r, gdouble lx, gdouble ly)
{
	struct wlr_scene_node *node;
	struct wlr_scene_surface *ss;
	struct wlr_xwayland_surface *xs;
	double sx, sy;

	node = wlr_scene_node_at(&r->compositor->scene->tree.node, lx, ly,
	                         &sx, &sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (ss == NULL)
		return NULL;
	xs = wlr_xwayland_surface_try_from_wlr_surface(ss->surface);
	if (xs == NULL || !xs->override_redirect)
		return NULL;
	return (GowlClient *)xs->data;
}

/* The wlr_surface at a layout point, whatever it belongs to. */
static G_GNUC_UNUSED struct wlr_surface *
x11_rig_surface_at(X11Rig *r, gdouble lx, gdouble ly)
{
	struct wlr_scene_node *node;
	struct wlr_scene_surface *ss;
	double sx, sy;

	node = wlr_scene_node_at(&r->compositor->scene->tree.node, lx, ly,
	                         &sx, &sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	return ss != NULL ? ss->surface : NULL;
}

/* Map @win and wait for gowl to have a mapped client for it. */
static G_GNUC_UNUSED GowlClient *
x11_rig_map_managed(X11Rig *r, xcb_window_t win)
{
	gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;

	x11_rig_map(r, win);
	while (g_get_monotonic_time() < deadline) {
		GowlClient *c = x11_rig_client_for(r, win);

		if (c != NULL)
			return c;
		x11_rig_pump(r, 10);
	}
	return NULL;
}

/* Map an override-redirect @win and wait until it is drawn at @lx,@ly. */
static G_GNUC_UNUSED GowlClient *
x11_rig_map_unmanaged(X11Rig *r, xcb_window_t win, gdouble lx, gdouble ly)
{
	gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;

	x11_rig_map(r, win);
	while (g_get_monotonic_time() < deadline) {
		GowlClient *c = x11_rig_unmanaged_at(r, lx, ly);

		if (c != NULL && c->xwayland_surface->window_id == win)
			return c;
		x11_rig_pump(r, 10);
	}
	return NULL;
}

/* Wait for a condition on the compositor side. */
#define X11_RIG_AWAIT(r, cond, seconds) \
	G_STMT_START { \
		gint64 deadline__ = g_get_monotonic_time() \
		                    + (seconds) * G_USEC_PER_SEC; \
		while (!(cond) && g_get_monotonic_time() < deadline__) \
			x11_rig_pump((r), 10); \
	} G_STMT_END

/* What the X server says has the keyboard: the application's view. */
static G_GNUC_UNUSED xcb_window_t
x11_rig_input_focus(X11Rig *r)
{
	xcb_get_input_focus_reply_t *reply;
	xcb_window_t focus;

	reply = x11_rig_reply(r, xcb_get_input_focus(r->conn).sequence);
	if (reply == NULL)
		return XCB_WINDOW_NONE;
	focus = reply->focus;
	free(reply);
	return focus;
}

static G_GNUC_UNUSED gboolean
x11_rig_await_input_focus(X11Rig *r, xcb_window_t want)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (x11_rig_input_focus(r) == want)
			return TRUE;
		x11_rig_pump(r, 20);
	}
	return FALSE;
}

/* Where the X server has the window, after whatever the WM did. */
static G_GNUC_UNUSED gboolean
x11_rig_geometry(X11Rig *r, xcb_window_t win, gint *x, gint *y,
                 gint *w, gint *h)
{
	xcb_get_geometry_reply_t *reply;

	reply = x11_rig_reply(r, xcb_get_geometry(r->conn, win).sequence);
	if (reply == NULL)
		return FALSE;
	if (x != NULL) *x = reply->x;
	if (y != NULL) *y = reply->y;
	if (w != NULL) *w = reply->width;
	if (h != NULL) *h = reply->height;
	free(reply);
	return TRUE;
}

/* A client-side ConfigureWindow: through the redirect to the WM. */
static G_GNUC_UNUSED void
x11_rig_configure(X11Rig *r, xcb_window_t win, gint x, gint y,
                  gint w, gint h)
{
	uint32_t values[4];

	values[0] = (uint32_t)x;
	values[1] = (uint32_t)y;
	values[2] = (uint32_t)w;
	values[3] = (uint32_t)h;
	xcb_configure_window(r->conn, win,
	                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
	                     | XCB_CONFIG_WINDOW_WIDTH
	                     | XCB_CONFIG_WINDOW_HEIGHT, values);
	xcb_flush(r->conn);
}

/* WM_HINTS with, or without, the urgency flag. */
static G_GNUC_UNUSED void
x11_rig_set_urgent(X11Rig *r, xcb_window_t win, gboolean urgent)
{
	xcb_icccm_wm_hints_t hints;

	memset(&hints, 0, sizeof hints);
	xcb_icccm_wm_hints_set_input(&hints, 1);
	if (urgent)
		xcb_icccm_wm_hints_set_urgency(&hints);
	xcb_icccm_set_wm_hints(r->conn, win, &hints);
	xcb_flush(r->conn);
}

static G_GNUC_UNUSED struct wlr_surface *
x11_rig_keyboard_focus(X11Rig *r)
{
	return r->compositor->wlr_seat->keyboard_state.focused_surface;
}

static G_GNUC_UNUSED struct wlr_surface *
x11_rig_pointer_focus(X11Rig *r)
{
	return r->compositor->wlr_seat->pointer_state.focused_surface;
}

static G_GNUC_UNUSED gboolean
x11_rig_is_override_redirect(struct wlr_surface *surface)
{
	struct wlr_xwayland_surface *xs;

	if (surface == NULL)
		return FALSE;
	xs = wlr_xwayland_surface_try_from_wlr_surface(surface);
	return xs != NULL && xs->override_redirect;
}

#endif /* GOWL_TESTS_X11_RIG_H */
