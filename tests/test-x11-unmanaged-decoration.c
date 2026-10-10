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
 * X11 windows whose override-redirect bit is not the same at every
 * point of their life, under the compositor's own Xwayland with the
 * roundcorners decorator loaded.
 *
 * Closing SteamVR killed the session.  Its vrmonitor window is created
 * as an ordinary X window -- so gowl gave it a border width -- and
 * mapped override-redirect, so it went down the unmanaged path.  A
 * repaint then had roundcorners hang a frame in its scene tree; the
 * unmanaged unmap destroyed that tree, frame and all, without telling
 * the decorator; and when the window was destroyed the decorator
 * destroyed its frame a second time.  SIGSEGV in wlr_scene_node_destroy.
 *
 * The same family: a window mapped managed whose bit flips to
 * override-redirect while mapped was unmapped down the unmanaged path
 * -- left in the client list, frame freed under the decorator.
 *
 * A window's managed/unmanaged answer is now settled at map and kept to
 * unmap, an unmanaged window is never framed, and either unmap releases
 * the decorator before the tree goes.  These drive the real X sequences.
 */

#include <glib.h>

#ifdef GOWL_HAVE_XWAYLAND

#include "x11-rig.h"
#include "interfaces/gowl-client-decorator.h"
#include "module/gowl-module-manager.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/* Where the popup goes, in X (== layout) coordinates, and a point
 * inside it. */
#define POPUP_X (60)
#define POPUP_Y (60)
#define POPUP_W (180)
#define POPUP_H (132)
#define INSIDE_POPUP_X (POPUP_X + 20)
#define INSIDE_POPUP_Y (POPUP_Y + 20)

/*
 * Up to "managed X connection, roundcorners loaded and started".
 * Returns FALSE having already skipped the test.
 */
static gboolean
rig_up_with_roundcorners(
	X11Rig      *r,
	const gchar *tag
){
	GError *error = NULL;
	gchar  *path;

	if (!x11_rig_up(r, tag))
		return FALSE;

	/* Loaded, activated and started the way main() does it; the
	 * decorator is looked up per call, so loading after start is
	 * enough for every path below to find it. */
	path = g_strdup_printf("%s/roundcorners.so", GOWL_TEST_MODULE_DIR);
	if (!gowl_module_manager_load_module(r->modules, path, &error))
		g_error("%s", error->message);
	g_free(path);
	gowl_module_manager_activate_all(r->modules);
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	g_assert_nonnull(gowl_module_manager_get_decorator(r->modules));
	return TRUE;
}

/*
 * Frames hung directly in @c's scene tree.  roundcorners' frame is the
 * only scene *buffer* that is a direct child: the surface is a subtree,
 * and the plain borders are rects.
 */
static guint
frames_on(GowlClient *c)
{
	struct wlr_scene_node *child;
	guint n;

	n = 0;
	if (c->scene == NULL)
		return 0;
	wl_list_for_each(child, &c->scene->children, link) {
		if (child->type == WLR_SCENE_NODE_BUFFER)
			n++;
	}
	return n;
}

/* Set or clear override-redirect on @win, then nudge it so the X server
 * sends a ConfigureNotify -- which carries the bit -- to the xwm. */
static void
set_override_redirect(
	X11Rig       *r,
	xcb_window_t  win,
	gboolean      on,
	gint          x,
	gint          y
){
	uint32_t value;
	uint32_t where[2];

	value = on ? 1 : 0;
	xcb_change_window_attributes(r->conn, win, XCB_CW_OVERRIDE_REDIRECT,
	                             &value);
	where[0] = (uint32_t)x;
	where[1] = (uint32_t)y;
	xcb_configure_window(r->conn, win,
	                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, where);
	xcb_flush(r->conn);
}

/*
 * vrmonitor's shape: created as an ordinary window, so it has a border
 * width, then mapped override-redirect.  Nothing may frame it -- not an
 * opacity change, not a frame-geometry pass -- and unmapping and
 * destroying it must leave the decorator holding nothing.
 */
static void
a_popup_created_as_a_window_is_never_framed(void)
{
	X11Rig       r;
	xcb_window_t win;
	GowlClient  *c;
	uint32_t     on;

	if (!rig_up_with_roundcorners(&r, "x11-unmanaged-frame"))
		return;

	win = x11_rig_create_toplevel(&r, "vrmonitor", POPUP_X, POPUP_Y,
	                              POPUP_W, POPUP_H);
	x11_rig_pump(&r, 50);
	on = 1;
	xcb_change_window_attributes(r.conn, win, XCB_CW_OVERRIDE_REDIRECT,
	                             &on);
	c = x11_rig_map_unmanaged(&r, win, INSIDE_POPUP_X, INSIDE_POPUP_Y);
	if (c == NULL) {
		g_test_skip("the override-redirect window never mapped here");
		x11_rig_down(&r);
		return;
	}
	g_object_add_weak_pointer(G_OBJECT(c), (gpointer *)&c);

	/* The precondition the crash needed: a border width from creation
	 * time, on a window that mapped unmanaged. */
	g_assert_cmpuint(c->bw, >, 0);
	g_assert_true(c->isunmanaged);
	g_assert_null(g_list_find(r.compositor->clients, c));
	g_assert_cmpuint(frames_on(c), ==, 0);

	/* Every route a repaint takes to the decorator. */
	gowl_client_set_alpha(c, 0.5f);
	gowl_compositor_apply_frame_geometry(r.compositor, c,
	                                     POPUP_W, POPUP_H);
	g_assert_cmpuint(frames_on(c), ==, 0);

	/* Unmap, then destroy: SteamVR closing. */
	x11_rig_unmap(&r, win);
	X11_RIG_AWAIT(&r, c->scene == NULL, 3);
	g_assert_null(c->scene);
	g_assert_false(c->isunmanaged);
	xcb_destroy_window(r.conn, win);
	xcb_flush(r.conn);
	X11_RIG_AWAIT(&r, c == NULL, 3);
	g_assert_null(c);

	x11_rig_down(&r);
}

/*
 * A managed window whose bit flips to override-redirect while it is
 * mapped is still torn down as the managed window it was mapped as: it
 * leaves the client list, its frame goes through the decorator, and
 * once destroyed nothing holds it.
 */
static void
a_window_that_turns_override_redirect_unmaps_as_managed(void)
{
	X11Rig       r;
	xcb_window_t win;
	GowlClient  *c;

	if (!rig_up_with_roundcorners(&r, "x11-unmanaged-flip"))
		return;

	win = x11_rig_create_toplevel(&r, "flips", 0, 0, 300, 200);
	c = x11_rig_map_managed(&r, win);
	if (c == NULL) {
		g_test_skip("the X11 toplevel never mapped here");
		x11_rig_down(&r);
		return;
	}
	g_object_add_weak_pointer(G_OBJECT(c), (gpointer *)&c);
	X11_RIG_AWAIT(&r, frames_on(c) == 1, 3);
	g_assert_false(c->isunmanaged);
	g_assert_cmpuint(frames_on(c), ==, 1);

	set_override_redirect(&r, win, TRUE, 10, 10);
	X11_RIG_AWAIT(&r, c->xwayland_surface->override_redirect, 3);
	if (!c->xwayland_surface->override_redirect) {
		g_test_skip("the X server never reported the new "
		            "override-redirect bit");
		x11_rig_down(&r);
		return;
	}
	g_assert_false(c->isunmanaged);

	x11_rig_unmap(&r, win);
	X11_RIG_AWAIT(&r, c->scene == NULL, 3);
	g_assert_null(c->scene);
	g_assert_null(g_list_find(r.compositor->clients, c));
	g_assert_null(g_list_find(r.compositor->fstack, c));

	xcb_destroy_window(r.conn, win);
	xcb_flush(r.conn);
	X11_RIG_AWAIT(&r, c == NULL, 3);
	g_assert_null(c);

	x11_rig_down(&r);
}

/*
 * The other direction: a popup mapped unmanaged whose bit clears while
 * it is mapped stays unmanaged until it unmaps -- never in the client
 * list, never framed.
 */
static void
a_popup_that_turns_managed_stays_unmanaged_until_unmap(void)
{
	X11Rig       r;
	xcb_window_t win;
	GowlClient  *c;

	if (!rig_up_with_roundcorners(&r, "x11-unmanaged-clear"))
		return;

	win = x11_rig_create_override_redirect(&r, FALSE, XCB_WINDOW_NONE,
	                                       POPUP_X, POPUP_Y,
	                                       POPUP_W, POPUP_H);
	c = x11_rig_map_unmanaged(&r, win, INSIDE_POPUP_X, INSIDE_POPUP_Y);
	if (c == NULL) {
		g_test_skip("the override-redirect window never mapped here");
		x11_rig_down(&r);
		return;
	}
	g_object_add_weak_pointer(G_OBJECT(c), (gpointer *)&c);
	g_assert_true(c->isunmanaged);

	set_override_redirect(&r, win, FALSE, POPUP_X + 5, POPUP_Y + 5);
	X11_RIG_AWAIT(&r, !c->xwayland_surface->override_redirect, 3);
	if (c->xwayland_surface->override_redirect) {
		g_test_skip("the X server never reported the cleared "
		            "override-redirect bit");
		x11_rig_down(&r);
		return;
	}
	g_assert_true(c->isunmanaged);

	gowl_client_set_alpha(c, 0.5f);
	g_assert_cmpuint(frames_on(c), ==, 0);
	g_assert_null(g_list_find(r.compositor->clients, c));

	x11_rig_unmap(&r, win);
	X11_RIG_AWAIT(&r, c->scene == NULL, 3);
	g_assert_null(c->scene);
	xcb_destroy_window(r.conn, win);
	xcb_flush(r.conn);
	X11_RIG_AWAIT(&r, c == NULL, 3);
	g_assert_null(c);

	x11_rig_down(&r);
}

#else /* !GOWL_HAVE_XWAYLAND */

static void
a_popup_created_as_a_window_is_never_framed(void)
{
	g_test_skip("built without Xwayland");
}

static void
a_window_that_turns_override_redirect_unmaps_as_managed(void)
{
	g_test_skip("built without Xwayland");
}

static void
a_popup_that_turns_managed_stays_unmanaged_until_unmap(void)
{
	g_test_skip("built without Xwayland");
}

#endif

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/x11-unmanaged/a-popup-created-as-a-window-is-never-framed",
	                a_popup_created_as_a_window_is_never_framed);
	g_test_add_func("/x11-unmanaged/a-window-that-turns-override-redirect-unmaps-as-managed",
	                a_window_that_turns_override_redirect_unmaps_as_managed);
	g_test_add_func("/x11-unmanaged/a-popup-that-turns-managed-stays-unmanaged-until-unmap",
	                a_popup_that_turns_managed_stays_unmanaged_until_unmap);

	return g_test_run();
}
