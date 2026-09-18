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
 * A real X11 client under the compositor's own Xwayland, because the
 * bug this pins lived exactly where no unit test reached.
 *
 * Steam's "Games" menu, Zoom's "End" dropdown, every tooltip over a
 * game: an X11 override-redirect window that did not ask for the
 * keyboard.  The pointer arriving on it -- which is how a menu gets
 * used -- ran sloppy focus, sloppy focus called focus_client() on the
 * menu, and focus_client() deactivated the window the menu belongs to.
 * CEF, Qt and wine all read a parent losing focus as "clicked away" and
 * close the menu.  So every menu closed the moment the pointer reached
 * it, and the parent flashed (deactivated, then re-activated when the
 * menu unmapped and focus fell back) on every tooltip crossed.
 *
 * The keyboard focus is asserted from both sides: the seat's
 * focused_surface, and GetInputFocus asked of the X server, which is
 * what the application itself sees.  The pointer focus is asserted too,
 * so a "fix" that stopped the pointer reaching the menu would fail here
 * rather than pass by accident.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

#ifdef GOWL_HAVE_XWAYLAND

#include <xcb/xcb.h>

#define TOPLEVEL_W (300)
#define TOPLEVEL_H (200)

/* Where the menu goes, in X (== layout) coordinates, and a point
 * inside it.  The toplevel is tiled over the whole headless output, so
 * the menu sits on top of its own parent, the way a real one does. */
#define MENU_X (40)
#define MENU_Y (40)
#define MENU_W (120)
#define MENU_H (80)
#define INSIDE_MENU_X (MENU_X + 30)
#define INSIDE_MENU_Y (MENU_Y + 20)

typedef struct {
	const gchar *display;

	xcb_connection_t *conn;
	xcb_screen_t     *screen;
	xcb_window_t      toplevel;
	xcb_window_t      menu;
	xcb_window_t      grabber;
	xcb_atom_t        net_wm_window_type;
	xcb_atom_t        net_wm_window_type_menu;

	/* Commands from the test thread, answered by the client thread. */
	gint connected;      /* 1 once the toplevel is mapped; -1 on failure */
	gint want_menu;      /* map the passive menu */
	gint menu_up;
	gint want_grabber;   /* map an override-redirect that wants focus */
	gint grabber_up;
	gint want_unmap_grabber;
	gint grabber_down;
	gint want_focus;     /* ask the server who has the keyboard */
	gint focus_answer;   /* 1 when @focus_window is fresh */
	xcb_window_t focus_window;
	gint stop;
} XClient;

static xcb_atom_t
intern(xcb_connection_t *conn, const gchar *name)
{
	xcb_intern_atom_cookie_t cookie;
	xcb_intern_atom_reply_t *reply;
	xcb_atom_t atom = XCB_ATOM_NONE;

	cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
	reply = xcb_intern_atom_reply(conn, cookie, NULL);
	if (reply != NULL) {
		atom = reply->atom;
		free(reply);
	}
	return atom;
}

/* An override-redirect window with a background the server paints, so
 * Xwayland has a pixmap to commit and gowl sees a mapped surface. */
static xcb_window_t
make_override_redirect(XClient *x, gboolean passive)
{
	xcb_window_t win;
	uint32_t values[2];

	win = xcb_generate_id(x->conn);
	values[0] = x->screen->white_pixel;
	values[1] = 1;
	xcb_create_window(x->conn, XCB_COPY_FROM_PARENT, win, x->screen->root,
	                  MENU_X, MENU_Y, MENU_W, MENU_H, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                  x->screen->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);

	/*
	 * _NET_WM_WINDOW_TYPE_MENU is what makes it PASSIVE: wlroots'
	 * override_redirect_wants_focus() answers FALSE for a menu,
	 * tooltip, dropdown, combo, DND icon and the like, and TRUE for an
	 * override-redirect with no such type -- a Qt confirmation panel,
	 * a wine dialog -- which is the other case below.
	 */
	if (passive)
		xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, win,
		                    x->net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
		                    &x->net_wm_window_type_menu);
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, win,
	                    XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1,
	                    &x->toplevel);
	xcb_map_window(x->conn, win);
	xcb_flush(x->conn);
	return win;
}

static gpointer
x_client_thread(gpointer data)
{
	XClient *x = data;
	uint32_t values[2];
	gint screen_nr = 0;

	/* Connecting is what spawns the lazy Xwayland; the compositor
	 * thread has to be pumping meanwhile, which is why this is a
	 * thread at all. */
	x->conn = xcb_connect(x->display, &screen_nr);
	if (x->conn == NULL || xcb_connection_has_error(x->conn)) {
		g_atomic_int_set(&x->connected, -1);
		return NULL;
	}
	x->screen = xcb_setup_roots_iterator(xcb_get_setup(x->conn)).data;
	x->net_wm_window_type      = intern(x->conn, "_NET_WM_WINDOW_TYPE");
	x->net_wm_window_type_menu = intern(x->conn,
	                                    "_NET_WM_WINDOW_TYPE_MENU");
	if (x->screen == NULL || x->net_wm_window_type == XCB_ATOM_NONE
	    || x->net_wm_window_type_menu == XCB_ATOM_NONE) {
		g_atomic_int_set(&x->connected, -1);
		return NULL;
	}

	/*
	 * Not one window before the window manager is up.
	 *
	 * This connection is what spawned Xwayland, so it is served from
	 * the server's first moments -- before wlroots' xwm has connected
	 * and selected SubstructureRedirect on the root.  A window mapped
	 * in that gap maps unmanaged, and Xwayland's WL_SURFACE_SERIAL
	 * client message, sent once at realize to whoever holds the
	 * redirect, goes to nobody: the X window is viewable, the
	 * wl_surface exists, and the two are never paired, so gowl never
	 * sees a map.  About one run in four, on this machine.
	 *
	 * xwm_create() selects the redirect first and takes the WM_S0
	 * selection last, so an owner for WM_S0 means the redirect is in
	 * place.  A round trip through the same server is the only
	 * ordering that counts; a flag on the compositor side is not.
	 */
	{
		xcb_atom_t wm_s0 = intern(x->conn, "WM_S0");
		gint64     deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
		gboolean   managed = FALSE;

		while (wm_s0 != XCB_ATOM_NONE
		       && g_get_monotonic_time() < deadline
		       && !g_atomic_int_get(&x->stop)) {
			xcb_get_selection_owner_reply_t *owner;

			owner = xcb_get_selection_owner_reply(x->conn,
				xcb_get_selection_owner(x->conn, wm_s0), NULL);
			managed = owner != NULL && owner->owner != XCB_WINDOW_NONE;
			free(owner);
			if (managed)
				break;
			g_usleep(5000);
		}
		if (!managed) {
			g_atomic_int_set(&x->connected, -1);
			return NULL;
		}
	}

	/* The managed toplevel the menu belongs to. */
	x->toplevel = xcb_generate_id(x->conn);
	values[0] = x->screen->white_pixel;
	values[1] = XCB_EVENT_MASK_FOCUS_CHANGE;
	xcb_create_window(x->conn, XCB_COPY_FROM_PARENT, x->toplevel,
	                  x->screen->root, 0, 0, TOPLEVEL_W, TOPLEVEL_H, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                  x->screen->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, x->toplevel,
	                    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 11,
	                    "popup-focus");
	xcb_map_window(x->conn, x->toplevel);
	xcb_flush(x->conn);
	g_atomic_int_set(&x->connected, 1);

	while (!g_atomic_int_get(&x->stop)) {
		xcb_generic_event_t *ev;

		if (g_atomic_int_get(&x->want_menu) == 1) {
			g_atomic_int_set(&x->want_menu, 2);
			x->menu = make_override_redirect(x, TRUE);
			g_atomic_int_set(&x->menu_up, 1);
		}
		if (g_atomic_int_get(&x->want_grabber) == 1) {
			g_atomic_int_set(&x->want_grabber, 2);
			x->grabber = make_override_redirect(x, FALSE);
			g_atomic_int_set(&x->grabber_up, 1);
		}
		if (g_atomic_int_get(&x->want_unmap_grabber) == 1) {
			g_atomic_int_set(&x->want_unmap_grabber, 2);
			xcb_unmap_window(x->conn, x->grabber);
			xcb_flush(x->conn);
			g_atomic_int_set(&x->grabber_down, 1);
		}
		if (g_atomic_int_get(&x->want_focus) == 1) {
			xcb_get_input_focus_reply_t *reply;

			g_atomic_int_set(&x->want_focus, 2);
			reply = xcb_get_input_focus_reply(x->conn,
				xcb_get_input_focus(x->conn), NULL);
			x->focus_window = reply != NULL ? reply->focus
			                                : XCB_WINDOW_NONE;
			free(reply);
			g_atomic_int_set(&x->focus_answer, 1);
		}

		while ((ev = xcb_poll_for_event(x->conn)) != NULL)
			free(ev);
		if (xcb_connection_has_error(x->conn))
			break;
		g_usleep(2000);
	}
	xcb_disconnect(x->conn);
	return NULL;
}

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
} Rig;

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError      *error = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-x11-popup-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	/* The path under test.  Off, and the pointer never focuses
	 * anything. */
	g_object_set(r->config, "sloppyfocus", TRUE, NULL);
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
	return TRUE;
}

static void
rig_down(Rig *r)
{
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

static void
pump(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(
			wl_display_get_event_loop(r->compositor->wl_display), 0);
		wl_display_flush_clients(r->compositor->wl_display);
		g_usleep(1000);
	}
}

static gboolean
await(Rig *r, gint *flag, gint seconds)
{
	gint64 deadline = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (g_atomic_int_get(flag))
			return TRUE;
		pump(r, 10);
	}
	return g_atomic_int_get(flag) != 0;
}

/* The managed X11 window, once gowl has mapped it. */
static GowlClient *
find_x11_toplevel(Rig *r)
{
	GList *l;

	for (l = r->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (c->xwayland_surface != NULL
		    && !c->xwayland_surface->override_redirect
		    && gowl_client_get_wlr_surface(c) != NULL)
			return c;
	}
	return NULL;
}

static GowlClient *
await_x11_toplevel(Rig *r)
{
	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		GowlClient *c = find_x11_toplevel(r);

		if (c != NULL)
			return c;
		pump(r, 10);
	}
	return NULL;
}

static struct wlr_surface *
keyboard_focus(Rig *r)
{
	return r->compositor->wlr_seat->keyboard_state.focused_surface;
}

static struct wlr_surface *
pointer_focus(Rig *r)
{
	return r->compositor->wlr_seat->pointer_state.focused_surface;
}

static gboolean
is_override_redirect(struct wlr_surface *surface)
{
	struct wlr_xwayland_surface *xs;

	if (surface == NULL)
		return FALSE;
	xs = wlr_xwayland_surface_try_from_wlr_surface(surface);
	return xs != NULL && xs->override_redirect;
}

/* What the X server says has the keyboard -- the application's view. */
static xcb_window_t
x_input_focus(Rig *r, XClient *x)
{
	g_atomic_int_set(&x->focus_answer, 0);
	g_atomic_int_set(&x->want_focus, 1);
	if (!await(r, &x->focus_answer, 3))
		return XCB_WINDOW_NONE;
	return x->focus_window;
}

static gboolean
await_x_input_focus(Rig *r, XClient *x, xcb_window_t want)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (x_input_focus(r, x) == want)
			return TRUE;
		pump(r, 20);
	}
	return FALSE;
}

/* Drive the pointer onto the menu until the seat's pointer focus is an
 * override-redirect surface: that is the precondition, and the moment
 * the old sloppy-focus path fired. */
static gboolean
pointer_onto_override_redirect(Rig *r)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	gint   wiggle = 0;

	while (g_get_monotonic_time() < deadline) {
		gowl_compositor_inject_pointer_warp(r->compositor,
			INSIDE_MENU_X + (wiggle & 1), INSIDE_MENU_Y);
		wiggle++;
		pump(r, 20);
		if (is_override_redirect(pointer_focus(r)))
			return TRUE;
	}
	return FALSE;
}

typedef struct {
	Rig         r;
	XClient     x;
	GThread    *thread;
	GowlClient *toplevel;
} Scene;

static void
scene_down(Scene *s)
{
	g_atomic_int_set(&s->x.stop, 1);
	pump(&s->r, 100);
	if (s->thread != NULL)
		g_thread_join(s->thread);
	rig_down(&s->r);
}

/* Up to "the toplevel is mapped, focused, and the X server agrees".
 * Returns FALSE having already skipped the test. */
static gboolean
scene_up(Scene *s)
{
	memset(s, 0, sizeof(*s));
	if (!rig_up(&s->r))
		return FALSE;

	s->x.display = s->r.compositor->xwayland->display_name;
	s->thread = g_thread_new("x11-popup-client", x_client_thread, &s->x);

	if (!await(&s->r, &s->x.connected, 10)
	    || g_atomic_int_get(&s->x.connected) < 0) {
		g_test_skip("no X11 connection to the compositor's Xwayland");
		scene_down(s);
		return FALSE;
	}
	s->toplevel = await_x11_toplevel(&s->r);
	if (s->toplevel == NULL) {
		g_test_skip("the X11 toplevel never mapped here");
		scene_down(s);
		return FALSE;
	}

	gowl_compositor_focus_client(s->r.compositor, s->toplevel, TRUE);
	pump(&s->r, 100);
	g_assert_true(keyboard_focus(&s->r)
	              == gowl_client_get_wlr_surface(s->toplevel));
	if (!await_x_input_focus(&s->r, &s->x, s->x.toplevel)) {
		g_test_skip("the X server never granted the toplevel focus");
		scene_down(s);
		return FALSE;
	}
	return TRUE;
}

static void
a_menu_the_pointer_reaches_keeps_its_parent_focused(void)
{
	Scene s;
	struct wlr_surface *parent;

	if (!scene_up(&s))
		return;
	parent = gowl_client_get_wlr_surface(s.toplevel);

	g_atomic_int_set(&s.x.want_menu, 1);
	g_assert_true(await(&s.r, &s.x.menu_up, 3));

	/* The pointer arrives on the menu.  That it arrived is asserted
	 * first: a menu that never received the pointer would make the
	 * rest of this vacuous. */
	g_assert_true(pointer_onto_override_redirect(&s.r));

	/*
	 * And the keyboard did not follow it.  Both halves: the seat still
	 * points at the parent, and the X server -- which is what CEF, Qt
	 * and wine actually watch -- still says the parent has focus.  The
	 * old path moved the seat to the menu and deactivated the parent,
	 * which SetInputFocus'd it away; either alone was enough to close
	 * the menu.
	 */
	pump(&s.r, 100);
	g_assert_true(keyboard_focus(&s.r) == parent);
	g_assert_cmpuint(x_input_focus(&s.r, &s.x), ==, s.x.toplevel);

	/* A click on it -- choosing an item -- is the other way a passive
	 * popup used to be focused. */
	gowl_compositor_inject_button(s.r.compositor, BTN_LEFT, TRUE);
	pump(&s.r, 30);
	gowl_compositor_inject_button(s.r.compositor, BTN_LEFT, FALSE);
	pump(&s.r, 100);
	g_assert_true(keyboard_focus(&s.r) == parent);
	g_assert_cmpuint(x_input_focus(&s.r, &s.x), ==, s.x.toplevel);

	scene_down(&s);
}

static void
a_popup_that_wants_the_keyboard_still_gets_it(void)
{
	Scene s;
	struct wlr_surface *parent;

	if (!scene_up(&s))
		return;
	parent = gowl_client_get_wlr_surface(s.toplevel);

	/*
	 * The guard against over-correction.  Zoom's "Leave meeting"
	 * panel is override-redirect too, and it DOES want the keyboard:
	 * gowl focuses it at map and holds it as the exclusive client.
	 * A fix that treated every override-redirect as passive would
	 * leave that panel deaf.
	 */
	g_atomic_int_set(&s.x.want_grabber, 1);
	g_assert_true(await(&s.r, &s.x.grabber_up, 3));
	{
		gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

		while (g_get_monotonic_time() < deadline
		       && !is_override_redirect(keyboard_focus(&s.r)))
			pump(&s.r, 20);
	}
	g_assert_true(is_override_redirect(keyboard_focus(&s.r)));
	g_assert_true(s.r.compositor->exclusive_focus != NULL);

	/* The pointer wandering over it changes nothing. */
	g_assert_true(pointer_onto_override_redirect(&s.r));
	pump(&s.r, 100);
	g_assert_true(is_override_redirect(keyboard_focus(&s.r)));

	/* And when it goes, the keyboard comes back to the parent. */
	g_atomic_int_set(&s.x.want_unmap_grabber, 1);
	g_assert_true(await(&s.r, &s.x.grabber_down, 3));
	{
		gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

		while (g_get_monotonic_time() < deadline
		       && keyboard_focus(&s.r) != parent)
			pump(&s.r, 20);
	}
	g_assert_true(keyboard_focus(&s.r) == parent);
	g_assert_null(s.r.compositor->exclusive_focus);

	scene_down(&s);
}

#else /* !GOWL_HAVE_XWAYLAND */

static void
a_menu_the_pointer_reaches_keeps_its_parent_focused(void)
{
	g_test_skip("built without Xwayland");
}

static void
a_popup_that_wants_the_keyboard_still_gets_it(void)
{
	g_test_skip("built without Xwayland");
}

#endif

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/x11-popup/a-menu-the-pointer-reaches-keeps-its-parent-focused",
	                a_menu_the_pointer_reaches_keeps_its_parent_focused);
	g_test_add_func("/x11-popup/a-popup-that-wants-the-keyboard-still-gets-it",
	                a_popup_that_wants_the_keyboard_still_gets_it);

	return g_test_run();
}
