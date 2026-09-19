/* test-menu-render.c -- the menu card in a real compositor
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * test-menu-module covers what the menu DECIDES against a compositor
 * that was never started.  This covers the half that needs a scene: the
 * card is measured, drawn into a cairo surface and parked in the overlay
 * layer, and none of that runs without an output, a renderer and a
 * theme.
 *
 * What is asserted:
 *
 *   IT DRAWS SOMETHING, AND WHERE.  A node in the overlay layer, sized,
 *   and centred on the focused output.  A menu that opened without
 *   drawing would pass every test in test-menu-module -- the rows, the
 *   cursor, the commands are all correct -- and be invisible.
 *
 *   THE CARD FITS THE OUTPUT.  A tree with a hundred rows must not
 *   produce a surface taller than the screen it is centred on, because
 *   the top and bottom of it would then be off the edges and the first
 *   row would not be reachable.
 *
 *   CLOSING TAKES THE NODE WITH IT.  An overlay left behind covers the
 *   desktop with a picture of a menu that no longer answers keys, which
 *   is indistinguishable from the compositor having hung.
 *
 *   IT DOES NOT HOLD THE OUTPUT AWAKE.  The card is still: it must ask
 *   for a frame when it changes and then stop asking.  An overlay that
 *   asks every frame is two scene renders a refresh on a desktop where
 *   nothing is moving, which on a laptop is the battery.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "menu/gowl-menu.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-keybind-handler.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <linux/input-event-codes.h>

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlModuleManager *modules;
	GowlConfig        *config;
	GowlCompositor    *compositor;
	GowlModule        *menu;
	gboolean           started;
} Rig;

/* A tree big enough to need capping, built here rather than read from
 * disk so the assertions do not depend on what menu.yaml happens to
 * hold today. */
static gchar *
tall_tree(guint rows)
{
	GString *out = g_string_new("menu:\n");
	guint i;

	g_string_append(out, "  - id: few\n    label: Few\n    items:\n");
	g_string_append(out,
		"      - {id: one, label: One, spawn: \"/bin/true\"}\n");
	g_string_append(out, "  - id: many\n    label: Many\n    items:\n");
	for (i = 0; i < rows; i++) {
		g_string_append_printf(out,
			"      - {id: r%u, label: \"Row %u\", spawn: \"/bin/true\"}\n",
			i, i);
	}
	return g_string_free(out, FALSE);
}

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError      *error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *tree = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in. */
	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-menu-render-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "gles2", TRUE);

	r->modules = gowl_module_manager_new();
	path = g_build_filename(GOWL_TEST_MODULE_DIR, "menu.so", NULL);
	if (!gowl_module_manager_load_module(r->modules, path, &error))
		g_error("could not load %s: %s", path, error->message);
	gowl_module_manager_activate_all(r->modules);

	r->config = gowl_config_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	r->started = TRUE;

	tree = tall_tree(120);
	g_assert_true(gowl_menu_load_data(gowl_menu_get_default(), tree,
	                                  FALSE, NULL));

	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	g_assert_nonnull(r->compositor->selmon);

	r->menu = gowl_module_manager_find_module(r->modules, "menu");
	g_assert_nonnull(r->menu);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->started)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->started)
		g_assert_cmpint(g_rmdir(r->runtime), ==, 0);
	else
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	g_free(r->parent);
}

static gchar *
run(Rig *r, const gchar *command, const gchar *args)
{
	return gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(r->menu),
	                                       command, args);
}

/* The one buffer node in the overlay layer, or NULL. */
static struct wlr_scene_buffer *
card_node(Rig *r)
{
	struct wlr_scene_tree *overlay;
	struct wlr_scene_node *node;

	overlay = gowl_compositor_get_scene_layer(r->compositor,
	                                          GOWL_SCENE_LAYER_OVERLAY);
	if (overlay == NULL)
		return NULL;
	wl_list_for_each(node, &overlay->children, link) {
		if (node->type == WLR_SCENE_NODE_BUFFER)
			return wlr_scene_buffer_from_node(node);
	}
	return NULL;
}

static void
test_opening_draws_a_card(void)
{
	Rig r;
	struct wlr_scene_buffer *card;
	gint x = 0, y = 0;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	g_assert_null(card_node(&r));
	g_free(run(&r, "menu", "few"));

	card = card_node(&r);
	g_assert_nonnull(card);
	g_assert_cmpint(card->dst_width, >, 0);
	g_assert_cmpint(card->dst_height, >, 0);

	/* Centred across the focused output, give or take the drop
	 * shadow's share of the surface, and a little ABOVE the middle
	 * -- where launchers sit, so a list that grows grows downward --
	 * but never off the top. */
	wlr_scene_node_coords(&card->node, &x, &y);
	{
		GowlMonitor *m = r.compositor->selmon;
		gint want_x = m->w.x + (m->w.width - card->dst_width) / 2;
		gint centre_y = m->w.y + (m->w.height - card->dst_height) / 2;

		g_assert_cmpint(ABS(x - want_x), <=, 2);
		g_assert_cmpint(y, <=, centre_y + 2);
		g_assert_cmpint(y, >=, m->w.y);
		g_assert_cmpint(y + card->dst_height, <=, m->w.y + m->w.height);
	}

	rig_down(&r);
}

static void
test_a_long_list_still_fits(void)
{
	Rig r;
	struct wlr_scene_buffer *card;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	/* 120 rows: far more than any screen, and the failure mode is a
	 * card whose first row is off the top of the output. */
	g_free(run(&r, "menu", "many"));
	card = card_node(&r);
	g_assert_nonnull(card);
	g_assert_cmpint(card->dst_height, <=, r.compositor->selmon->w.height);
	g_assert_cmpint(card->dst_width, <=, r.compositor->selmon->w.width);

	rig_down(&r);
}

static void
test_closing_takes_the_node(void)
{
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	g_free(run(&r, "menu", "few"));
	g_assert_nonnull(card_node(&r));
	g_free(run(&r, "menu-close", NULL));
	/* An overlay left behind is a picture of a menu that no longer
	 * answers keys, which looks exactly like a hung compositor. */
	g_assert_null(card_node(&r));

	rig_down(&r);
}

static void
test_a_still_card_stops_asking_for_frames(void)
{
	Rig r;
	struct wlr_scene_output *so;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	g_free(run(&r, "menu", "few"));
	so = r.compositor->selmon->scene_output;
	g_assert_nonnull(so);

	/* Drawing the card damaged the output, so the first question
	 * answers yes.  Commit, and the card -- which is not animating --
	 * must then have nothing more to ask for. */
	g_assert_true(wlr_scene_output_needs_frame(so));
	wlr_scene_output_commit(so, NULL);
	g_assert_false(wlr_scene_output_needs_frame(so));

	rig_down(&r);
}

/*
 * The pointer, and the coordinate mapping behind it.
 *
 * A click is hit-tested against the LAST render's rectangles, which are
 * in the card's own content coordinates, while the event arrives in
 * layout coordinates -- so the mapping subtracts where the surface
 * landed and where the card sits inside it.  Get either term wrong and
 * the centre of the card reads as outside it, which closes the menu on
 * every click: the menu then works perfectly from the keyboard and is
 * unusable with the mouse.
 */
static void
test_a_click_on_the_card_is_not_a_click_outside(void)
{
	Rig r;
	struct wlr_scene_buffer *card;
	gint x = 0, y = 0;
	guint row;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	/*
	 * One row at the root, and it OPENS something rather than running
	 * it.  That is what tells the two outcomes apart: a click that
	 * lands enters the submenu and leaves the card up, and a click the
	 * mapping sent outside closes it.  A row that merely ran would
	 * close the card too, and the test would pass either way -- which
	 * is exactly how the first version of this passed against a
	 * deliberately broken mapping.
	 */
	g_assert_true(gowl_menu_load_data(gowl_menu_get_default(),
		"menu:\n"
		"  - id: only\n"
		"    label: Only\n"
		"    items:\n"
		"      - {id: deep, label: Deep, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	g_free(run(&r, "menu", NULL));
	card = card_node(&r);
	g_assert_nonnull(card);
	wlr_scene_node_coords(&card->node, &x, &y);

	/* Down the card in steps: the row is below the header and above
	 * the shadow, and exactly where depends on the theme's metrics. */
	for (row = 55; row <= 95; row += 5) {
		r.compositor->wlr_cursor->x = x + card->dst_width / 2.0;
		r.compositor->wlr_cursor->y = y + card->dst_height * (row / 100.0);
		g_assert_true(gowl_mouse_handler_handle_button(
			GOWL_MOUSE_HANDLER(r.menu), BTN_LEFT, 1, 0));
		if (card_node(&r) == NULL)
			g_error("a click %u%% down the card closed it: the "
			        "pointer mapping put it outside", row);
		{
			/* Did it ENTER, or just sit there?  Toggling to the
			 * route we should now be in closes only if we are. */
			g_autofree gchar *reply = run(&r, "menu", "only");

			if (g_strcmp0(reply, "OK closed") == 0)
				break;
			/* Not on the row yet: still at the root, so that
			 * toggle moved us into `only'.  Back out and try
			 * further down. */
			g_free(run(&r, "menu-close", NULL));
			g_free(run(&r, "menu", NULL));
			card = card_node(&r);
			g_assert_nonnull(card);
		}
	}
	g_assert_cmpuint(row, <=, 95);

	/* And the other half of the same mapping: a click genuinely
	 * outside closes. */
	g_free(run(&r, "menu", NULL));
	card = card_node(&r);
	g_assert_nonnull(card);
	wlr_scene_node_coords(&card->node, &x, &y);
	r.compositor->wlr_cursor->x = x - 40.0;
	r.compositor->wlr_cursor->y = y - 40.0;
	g_assert_true(gowl_mouse_handler_handle_button(
		GOWL_MOUSE_HANDLER(r.menu), BTN_LEFT, 1, 0));
	g_assert_null(card_node(&r));

	rig_down(&r);
}

static void
test_the_pointer_is_ignored_while_closed(void)
{
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	/* Closed, so every event belongs to somebody else -- a menu that
	 * claimed the pointer while invisible would stop focus following
	 * the mouse for the rest of the session. */
	g_assert_false(gowl_mouse_handler_handle_motion(
		GOWL_MOUSE_HANDLER(r.menu), 10.0, 10.0));
	g_assert_false(gowl_mouse_handler_handle_button(
		GOWL_MOUSE_HANDLER(r.menu), BTN_LEFT, 1, 0));
	g_assert_false(gowl_mouse_handler_handle_axis(
		GOWL_MOUSE_HANDLER(r.menu), 0, 1.0, 1, 0));

	rig_down(&r);
}

/*
 * A row whose action reloads this very tree.
 *
 * `Reload this menu' is a shipped row, and what it runs frees every
 * entry -- including the one being activated.  Reading the row back
 * after the call, or handing the dispatcher a pointer into it, is a read
 * of freed memory in the compositor process; under `cmacs --gowl' that
 * is the whole desktop, and it happens only when somebody uses the menu
 * to fix the menu.  Any `spawn:' that rewrites menu.yaml has the same
 * shape a moment later, so it is not a one-row problem.
 *
 * It needs this rig rather than the module test's: the action goes out
 * through the compositor's keybind dispatcher and back in through the
 * module manager, so a compositor without one never runs it at all.
 */
static void
test_a_row_that_reloads_the_tree(void)
{
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	g_assert_true(gowl_menu_load_data(gowl_menu_get_default(),
		"menu:\n"
		"  - {id: refresh, label: Reload, command: \"menu-refresh\"}\n",
		FALSE, NULL));

	g_free(run(&r, "menu-open", NULL));
	g_assert_nonnull(card_node(&r));

	/* Choose it.  The reload happens inside the activation. */
	g_assert_true(gowl_keybind_handler_handle_key(
		GOWL_KEYBIND_HANDLER(r.menu), 0, XKB_KEY_Return, TRUE));

	/* Still answering, and still drawing: a menu that reloaded itself
	 * out from under its own activation would be a crash, and the next
	 * best evidence is that everything still works. */
	{
		g_autofree gchar *reply = run(&r, "menu-list", NULL);

		g_assert_nonnull(reply);
	}
	g_free(run(&r, "menu-close", NULL));
	g_assert_null(card_node(&r));

	rig_down(&r);
}

/*
 * Opening a list of applications must not stop the desktop.
 *
 * This is the bug the feature shipped with, and it was not subtle: an
 * icon name costs tens to hundreds of milliseconds to turn into pixels
 * -- finding the file is a walk of every icon theme, and decoding it
 * spawns a sandboxed loader -- and a dozen rows of that, done on the
 * compositor thread while somebody is TYPING, froze the whole session.
 *
 * The threshold sits in the gap between the two possible
 * implementations rather than at any particular speed.  Opening a list
 * of every application on the machine costs something either way --
 * enumerating the desktop entries, then measuring a row for each -- and
 * that is tens of milliseconds.  Resolving the icons on top of it is
 * most of a second, because a fresh process has to build barkit's index
 * first and then decode a dozen images.  Measured here: 70ms against
 * 693ms.  A quarter of a second is comfortably between them and close
 * to neither.
 */
static void
test_opening_a_list_of_apps_does_not_block(void)
{
	Rig r;
	gint64 t0, elapsed;
	guint i;

	/* Nothing to be slow about: no themes, no walk, no test. */
	if (!g_file_test("/usr/share/icons/hicolor", G_FILE_TEST_IS_DIR)) {
		g_test_skip("no icon themes installed");
		return;
	}
	if (!rig_up(&r)) {
		g_test_skip("no headless compositor");
		rig_down(&r);
		return;
	}

	g_assert_true(gowl_menu_load_data(gowl_menu_get_default(),
		"menu:\n  - {id: apps, label: Apps, provider: apps}\n",
		FALSE, NULL));

	t0 = g_get_monotonic_time();
	g_free(run(&r, "menu-open", "apps"));
	elapsed = g_get_monotonic_time() - t0;

	g_assert_nonnull(card_node(&r));
	if (elapsed > 250000) {
		g_error("opening a list of applications took %.0f ms: the "
		        "icons are being resolved on the compositor thread",
		        elapsed / 1000.0);
	}

	/*
	 * And the worker does eventually ANSWER.
	 *
	 * Not drawing on the compositor thread is only half of it: an
	 * answer that never comes back is a menu whose icons are
	 * permanently the fallback glyph, which looks exactly like the
	 * feature not working.
	 *
	 * Asked through the output's own "is there anything to draw", not
	 * through the scene buffer's pointer -- wlroots swaps that during
	 * compositing, so it changes whether or not anything redrew.
	 */
	{
		struct wlr_scene_output *so = r.compositor->selmon->scene_output;
		gboolean redrew = FALSE;

		g_assert_nonnull(so);
		wlr_scene_output_commit(so, NULL);
		g_assert_false(wlr_scene_output_needs_frame(so));

		for (i = 0; i < 400; i++) {
			wl_event_loop_dispatch(
				gowl_compositor_get_event_loop(r.compositor), 10);
			if (wlr_scene_output_needs_frame(so)) {
				redrew = TRUE;
				break;
			}
		}
		g_assert_true(redrew);
	}

	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_setenv("GOWL_MENU_NO_HISTORY", "1", TRUE);

	g_test_add_func("/menu-render/opening-draws-a-card",
	                test_opening_draws_a_card);
	g_test_add_func("/menu-render/a-long-list-still-fits",
	                test_a_long_list_still_fits);
	g_test_add_func("/menu-render/closing-takes-the-node",
	                test_closing_takes_the_node);
	g_test_add_func("/menu-render/a-still-card-stops-asking-for-frames",
	                test_a_still_card_stops_asking_for_frames);
	g_test_add_func("/menu-render/a-click-on-the-card-is-not-outside",
	                test_a_click_on_the_card_is_not_a_click_outside);
	g_test_add_func("/menu-render/the-pointer-is-ignored-while-closed",
	                test_the_pointer_is_ignored_while_closed);
	g_test_add_func("/menu-render/a-row-that-reloads-the-tree",
	                test_a_row_that_reloads_the_tree);
	g_test_add_func("/menu-render/opening-a-list-of-apps-does-not-block",
	                test_opening_a_list_of_apps_does_not_block);

	return g_test_run();
}
