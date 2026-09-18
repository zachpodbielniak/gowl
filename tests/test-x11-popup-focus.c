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
#include <linux/input-event-codes.h>

#ifdef GOWL_HAVE_XWAYLAND

#include "x11-rig.h"

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
	X11Rig       r;
	xcb_window_t toplevel_win;
	GowlClient  *toplevel;
} Scene;

/* Up to "the toplevel is mapped, focused, and the X server agrees".
 * Returns FALSE having already skipped the test. */
static gboolean
scene_up(Scene *s)
{
	memset(s, 0, sizeof(*s));
	if (!x11_rig_up(&s->r, "x11-popup"))
		return FALSE;

	s->toplevel_win = x11_rig_create_toplevel(&s->r, "popup-focus",
	                                          0, 0, 300, 200);
	s->toplevel = x11_rig_map_managed(&s->r, s->toplevel_win);
	if (s->toplevel == NULL) {
		g_test_skip("the X11 toplevel never mapped here");
		x11_rig_down(&s->r);
		return FALSE;
	}

	gowl_compositor_focus_client(s->r.compositor, s->toplevel, TRUE);
	x11_rig_pump(&s->r, 100);
	g_assert_true(x11_rig_keyboard_focus(&s->r)
	              == gowl_client_get_wlr_surface(s->toplevel));
	if (!x11_rig_await_input_focus(&s->r, s->toplevel_win)) {
		g_test_skip("the X server never granted the toplevel focus");
		x11_rig_down(&s->r);
		return FALSE;
	}
	return TRUE;
}

/* Drive the pointer onto the menu until the seat's pointer focus is an
 * override-redirect surface: that is the precondition, and the moment
 * the old sloppy-focus path fired. */
static gboolean
pointer_onto_override_redirect(X11Rig *r)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	gint   wiggle = 0;

	while (g_get_monotonic_time() < deadline) {
		gowl_compositor_inject_pointer_warp(r->compositor,
			INSIDE_MENU_X + (wiggle & 1), INSIDE_MENU_Y);
		wiggle++;
		x11_rig_pump(r, 20);
		if (x11_rig_is_override_redirect(x11_rig_pointer_focus(r)))
			return TRUE;
	}
	return FALSE;
}

static void
a_menu_the_pointer_reaches_keeps_its_parent_focused(void)
{
	Scene s;
	struct wlr_surface *parent;
	xcb_window_t menu;

	if (!scene_up(&s))
		return;
	parent = gowl_client_get_wlr_surface(s.toplevel);

	menu = x11_rig_create_override_redirect(&s.r, TRUE, s.toplevel_win,
	                                        MENU_X, MENU_Y, MENU_W, MENU_H);
	g_assert_nonnull(x11_rig_map_unmanaged(&s.r, menu,
	                                       INSIDE_MENU_X, INSIDE_MENU_Y));

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
	x11_rig_pump(&s.r, 100);
	g_assert_true(x11_rig_keyboard_focus(&s.r) == parent);
	g_assert_cmpuint(x11_rig_input_focus(&s.r), ==, s.toplevel_win);

	/* A click on it -- choosing an item -- is the other way a passive
	 * popup used to be focused. */
	gowl_compositor_inject_button(s.r.compositor, BTN_LEFT, TRUE);
	x11_rig_pump(&s.r, 30);
	gowl_compositor_inject_button(s.r.compositor, BTN_LEFT, FALSE);
	x11_rig_pump(&s.r, 100);
	g_assert_true(x11_rig_keyboard_focus(&s.r) == parent);
	g_assert_cmpuint(x11_rig_input_focus(&s.r), ==, s.toplevel_win);

	x11_rig_down(&s.r);
}

static void
a_popup_that_wants_the_keyboard_still_gets_it(void)
{
	Scene s;
	struct wlr_surface *parent;
	xcb_window_t grabber;

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
	grabber = x11_rig_create_override_redirect(&s.r, FALSE, s.toplevel_win,
	                                           MENU_X, MENU_Y, MENU_W, MENU_H);
	x11_rig_map(&s.r, grabber);
	X11_RIG_AWAIT(&s.r,
	              x11_rig_is_override_redirect(x11_rig_keyboard_focus(&s.r)),
	              3);
	g_assert_true(x11_rig_is_override_redirect(x11_rig_keyboard_focus(&s.r)));
	g_assert_true(s.r.compositor->exclusive_focus != NULL);

	/* The pointer wandering over it changes nothing. */
	g_assert_true(pointer_onto_override_redirect(&s.r));
	x11_rig_pump(&s.r, 100);
	g_assert_true(x11_rig_is_override_redirect(x11_rig_keyboard_focus(&s.r)));

	/* And when it goes, the keyboard comes back to the parent. */
	x11_rig_unmap(&s.r, grabber);
	X11_RIG_AWAIT(&s.r, x11_rig_keyboard_focus(&s.r) == parent, 3);
	g_assert_true(x11_rig_keyboard_focus(&s.r) == parent);
	g_assert_null(s.r.compositor->exclusive_focus);

	x11_rig_down(&s.r);
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
