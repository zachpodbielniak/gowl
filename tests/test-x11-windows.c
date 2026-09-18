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
 * What an X11 window is owed by its window manager, checked against
 * gowl through a real Xwayland.
 *
 * Each case here is one thing dwl -- which gowl's X11 handling was
 * ported from -- does and gowl did not, found by reading the two side
 * by side after the menu-focus bug (tests/test-x11-popup-focus.c)
 * turned out to be a dropped guard from that port.  Every one of them
 * is a thing Steam, wine or Zoom does under gowl every day.
 */

#include <glib.h>

#ifdef GOWL_HAVE_XWAYLAND

#include "x11-rig.h"

/*
 * A dialog floats over the window it belongs to, on that window's
 * tags.
 *
 * Steam's "Add a Non-Steam Game", a wine file chooser, Zoom's settings:
 * every one sets WM_TRANSIENT_FOR on its parent.  gowl put them through
 * the tiling like any other toplevel -- a dialog the size of half the
 * screen, squeezing the window it was opened from -- and on whatever
 * tag happened to be selected, which is not necessarily the parent's.
 */
static void
a_transient_floats_on_its_parent(void)
{
	X11Rig r;
	GowlMonitor *m;
	xcb_window_t a_win, b_win;
	GowlClient *a, *b;

	if (!x11_rig_up(&r, "x11-transient"))
		return;
	m = gowl_compositor_get_selected_monitor(r.compositor);
	g_assert_nonnull(m);

	/* The parent, on tag 2. */
	gowl_monitor_set_tags(m, 1u << 1);
	a_win = x11_rig_create_toplevel(&r, "parent", 0, 0, 400, 300);
	a = x11_rig_map_managed(&r, a_win);
	g_assert_nonnull(a);
	g_assert_cmpuint(a->tags, ==, 1u << 1);

	/* Then the view moves on -- as it does while a dialog is opening
	 * from a window that was left on another tag. */
	gowl_monitor_set_tags(m, 1u << 0);
	x11_rig_pump(&r, 50);

	b_win = x11_rig_create_toplevel(&r, "dialog", 0, 0, 200, 120);
	xcb_change_property(r.conn, XCB_PROP_MODE_REPLACE, b_win,
	                    XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1,
	                    &a_win);
	b = x11_rig_map_managed(&r, b_win);
	g_assert_nonnull(b);

	g_assert_true(b->isfloating);
	g_assert_true(b->mon == a->mon);
	g_assert_cmpuint(b->tags, ==, a->tags);

	x11_rig_down(&r);
}

/*
 * A floating window that resizes itself is followed.
 *
 * A game switching resolution, a dialog growing for a longer message:
 * a ConfigureWindow from the client, which the redirect turns into a
 * request to the window manager.  gowl granted it verbatim and then
 * forgot it had -- c->geom, the frame, the border rects and the clip
 * all stayed at the old size, so the content was cut off or the border
 * hung in space, and the hit-test disagreed with the picture.
 */
static void
a_floating_window_that_configures_itself_is_followed(void)
{
	X11Rig r;
	xcb_window_t win;
	GowlClient *c;
	gint bw, gx = 0, gy = 0, gw = 0, gh = 0;

	if (!x11_rig_up(&r, "x11-configure"))
		return;

	win = x11_rig_create_toplevel(&r, "floater", 0, 0, 300, 200);
	c = x11_rig_map_managed(&r, win);
	g_assert_nonnull(c);
	gowl_compositor_set_floating(r.compositor, c, TRUE);
	x11_rig_pump(&r, 100);
	g_assert_true(c->isfloating);
	bw = (gint)c->bw;

	x11_rig_configure(&r, win, 100, 80, 400, 300);
	X11_RIG_AWAIT(&r, c->geom.width == 400 + 2 * bw, 3);

	/* The X server has the size the client asked for... */
	g_assert_true(x11_rig_geometry(&r, win, &gx, &gy, &gw, &gh));
	g_assert_cmpint(gw, ==, 400);
	g_assert_cmpint(gh, ==, 300);

	/* ...and so does gowl, frame and all. */
	g_assert_cmpint(c->geom.width, ==, 400 + 2 * bw);
	g_assert_cmpint(c->geom.height, ==, 300 + 2 * bw);
	g_assert_cmpint(c->geom.x, ==, 100 - bw);
	g_assert_cmpint(c->geom.y, ==, 80 - bw);
	g_assert_cmpint(c->scene->node.x, ==, 100 - bw);
	g_assert_cmpint(c->scene->node.y, ==, 80 - bw);

	x11_rig_down(&r);
}

/*
 * A menu that moves after it mapped is drawn where it went.
 *
 * A tooltip that follows the pointer, a submenu re-anchored to fit on
 * screen, a combo dropdown that flips upward: the client moves its
 * own override-redirect window with ConfigureWindow, which needs no
 * permission, and the X server tells the WM after the fact.  gowl read
 * the position once, at map, and never again -- so the picture stayed
 * at the first place while the client believed it had moved, and its
 * clicks landed on whatever was under the new place.
 */
static void
a_menu_that_moves_is_drawn_where_it_went(void)
{
	X11Rig r;
	xcb_window_t win;
	GowlClient *menu;
	struct wlr_surface *surface;

	if (!x11_rig_up(&r, "x11-menu-move"))
		return;

	win = x11_rig_create_override_redirect(&r, TRUE, XCB_WINDOW_NONE,
	                                       40, 40, 120, 80);
	menu = x11_rig_map_unmanaged(&r, win, 50, 50);
	g_assert_nonnull(menu);
	surface = gowl_client_get_wlr_surface(menu);
	g_assert_nonnull(surface);

	x11_rig_configure(&r, win, 300, 200, 120, 80);
	X11_RIG_AWAIT(&r, x11_rig_surface_at(&r, 310, 210) == surface, 3);

	g_assert_true(x11_rig_surface_at(&r, 310, 210) == surface);
	g_assert_true(x11_rig_surface_at(&r, 50, 50) != surface);
	g_assert_cmpint(menu->scene->node.x, ==, 300);
	g_assert_cmpint(menu->scene->node.y, ==, 200);

	x11_rig_down(&r);
}

/*
 * An urgency hint marks the window.
 *
 * "Your download finished", "someone messaged you", "the game is
 * ready": an X11 client that wants attention sets the UrgencyHint bit
 * in WM_HINTS.  Wayland clients reach the same flag through
 * xdg-activation and the bar shows their tag in the urgent colour; X11
 * clients set the hint and nothing happened, because gowl never
 * listened for it.
 */
static void
an_urgent_hint_marks_the_window(void)
{
	X11Rig r;
	xcb_window_t a_win, b_win;
	GowlClient *a, *b;

	if (!x11_rig_up(&r, "x11-urgent"))
		return;

	a_win = x11_rig_create_toplevel(&r, "focused", 0, 0, 300, 200);
	a = x11_rig_map_managed(&r, a_win);
	b_win = x11_rig_create_toplevel(&r, "other", 0, 0, 300, 200);
	b = x11_rig_map_managed(&r, b_win);
	g_assert_nonnull(a);
	g_assert_nonnull(b);

	gowl_compositor_focus_client(r.compositor, a, TRUE);
	x11_rig_pump(&r, 50);
	g_assert_true(x11_rig_keyboard_focus(&r)
	              == gowl_client_get_wlr_surface(a));

	/* The other window asks for attention. */
	x11_rig_set_urgent(&r, b_win, TRUE);
	X11_RIG_AWAIT(&r, gowl_client_get_urgent(b), 3);
	g_assert_true(gowl_client_get_urgent(b));

	/* And withdraws it. */
	x11_rig_set_urgent(&r, b_win, FALSE);
	X11_RIG_AWAIT(&r, !gowl_client_get_urgent(b), 3);
	g_assert_false(gowl_client_get_urgent(b));

	/* The focused window asking for attention is already looked at:
	 * dwl ignores it and so does gowl, or the window the user is
	 * typing into would turn red. */
	x11_rig_set_urgent(&r, a_win, TRUE);
	x11_rig_pump(&r, 300);
	g_assert_false(gowl_client_get_urgent(a));

	x11_rig_down(&r);
}

/*
 * A menu over a fullscreen window can be seen.
 *
 * Steam Big Picture, a wine game in fullscreen, Zoom sharing a screen
 * full-window: their own menus and tooltips are override-redirect
 * windows.  gowl drew unmanaged surfaces in the FLOAT layer, which sits
 * BELOW the FS layer -- and below the fullscreen backdrop rect that
 * lives there too -- so every menu a fullscreen X11 program opened was
 * drawn underneath it, invisible and unclickable.  dwl keeps a layer
 * for them above FS.
 */
static void
a_menu_over_a_fullscreen_window_is_seen(void)
{
	X11Rig r;
	xcb_window_t win, menu_win;
	GowlClient *c, *menu;

	if (!x11_rig_up(&r, "x11-fs-menu"))
		return;

	win = x11_rig_create_toplevel(&r, "game", 0, 0, 300, 200);
	c = x11_rig_map_managed(&r, win);
	g_assert_nonnull(c);
	gowl_compositor_set_client_fullscreen(r.compositor, c, TRUE);
	x11_rig_pump(&r, 200);
	g_assert_true(c->isfullscreen);
	g_assert_true(x11_rig_surface_at(&r, 50, 50)
	              == gowl_client_get_wlr_surface(c));

	menu_win = x11_rig_create_override_redirect(&r, TRUE, win,
	                                            40, 40, 120, 80);
	x11_rig_map(&r, menu_win);
	X11_RIG_AWAIT(&r, x11_rig_unmanaged_at(&r, 50, 50) != NULL, 3);

	menu = x11_rig_unmanaged_at(&r, 50, 50);
	g_assert_nonnull(menu);
	g_assert_cmpuint(menu->xwayland_surface->window_id, ==, menu_win);

	x11_rig_down(&r);
}

#else /* !GOWL_HAVE_XWAYLAND */

static void skip(void) { g_test_skip("built without Xwayland"); }
static void a_transient_floats_on_its_parent(void) { skip(); }
static void a_floating_window_that_configures_itself_is_followed(void) { skip(); }
static void a_menu_that_moves_is_drawn_where_it_went(void) { skip(); }
static void an_urgent_hint_marks_the_window(void) { skip(); }
static void a_menu_over_a_fullscreen_window_is_seen(void) { skip(); }

#endif

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/x11/a-transient-floats-on-its-parent",
	                a_transient_floats_on_its_parent);
	g_test_add_func("/x11/a-floating-window-that-configures-itself-is-followed",
	                a_floating_window_that_configures_itself_is_followed);
	g_test_add_func("/x11/a-menu-that-moves-is-drawn-where-it-went",
	                a_menu_that_moves_is_drawn_where_it_went);
	g_test_add_func("/x11/an-urgent-hint-marks-the-window",
	                an_urgent_hint_marks_the_window);
	g_test_add_func("/x11/a-menu-over-a-fullscreen-window-is-seen",
	                a_menu_over_a_fullscreen_window_is_seen);

	return g_test_run();
}
