/* test-window-below.c -- a floating window pushed behind the tiling
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Floating means "on top".  That is not a policy the compositor chose,
 * it is what the FLOAT scene layer IS -- it sits above the whole of the
 * TILE layer, so there is no amount of lowering within it that puts a
 * floating window behind a tiled one.  The only way to draw one behind
 * the tiling is to move it into the tiling LAYER and lower it there,
 * while leaving everything else about it floating: out of the layout,
 * at its own size, focusable, movable.
 *
 * That split is where the bugs are, and none of them are visible in the
 * flag:
 *
 *   THE LAYER, not the order.  A version that only lowered the node
 *   would look right in a test that asked "is it at the bottom of its
 *   parent" and would be drawn over every tile on screen.
 *
 *   IT HAS TO SURVIVE A RE-TILE.  arrange() puts every client on the
 *   monitor back in its layer, and what that used to mean was "every
 *   floating window into the FLOAT layer" -- so a window sunk once
 *   surfaces again on the next thing that re-tiles anything: a new
 *   window, a closed one, a nudged master factor, a tag switch.
 *
 *   FOCUS MUST NOT RAISE IT.  The window keeps the keyboard after being
 *   pushed down (that is what makes the same key bring it back), and
 *   focusing normally raises.  The raise has to be skipped or the
 *   feature undoes itself.
 *
 * Which end of a wlr_scene_tree's child list is the top is not
 * documented in the header, so the first case establishes it against
 * wlr_scene_node_raise_to_top() rather than assuming, and everything
 * after it is written in terms of that answer.
 *
 * The rig is tests/test-float-toggle.c's, for the same reason: one
 * thing is interposed -- gowl_compositor_place_client(), where a
 * layout's answer would meet an xdg_toplevel a fabricated client does
 * not have -- and everything above that line is the real code.  The two
 * wlroots seat calls are interposed as well, and only so that
 * gowl_compositor_focus_client() can be driven at a client with no
 * surface; nothing about the stacking goes through them.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

#define RIG_MAX_CLIENTS 8

/* See the note above: records the box instead of configuring a toplevel. */
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	client->geom = (struct wlr_box){ x, y, width, height };
	client->frame = client->geom;
}

/*
 * The two calls focus_client makes into a client's SURFACE.
 *
 * A fabricated client has none -- there is no xdg_toplevel behind it --
 * and wlr_seat_keyboard_notify_enter() asserts on a NULL surface, so
 * without these the focus case could not run at all.  Neither is
 * involved in stacking, which is what the case is about.
 */
void
wlr_seat_keyboard_notify_enter(struct wlr_seat *seat,
                               struct wlr_surface *surface,
                               const uint32_t *keycodes, size_t num_keycodes,
                               const struct wlr_keyboard_modifiers *modifiers)
{
	(void)seat; (void)surface; (void)keycodes;
	(void)num_keycodes; (void)modifiers;
}

uint32_t
wlr_xdg_toplevel_set_activated(struct wlr_xdg_toplevel *toplevel,
                               bool activated)
{
	(void)toplevel; (void)activated;
	return 0;
}

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	gboolean           started;
	GowlClient        *c[RIG_MAX_CLIENTS];
	gint               count;
} Rig;

/* The tiling layout: without a layout provider there are no tiles for
 * anything to go behind, and every case here would pass vacuously. */
static const gchar *const layout_modules[] = { "tile", NULL };

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError      *error = NULL;
	guint        i;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-below-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->modules = gowl_module_manager_new();
	for (i = 0; layout_modules[i] != NULL; i++) {
		gchar *file = g_strconcat(layout_modules[i], ".so", NULL);
		gchar *path = g_build_filename(GOWL_TEST_MODULE_DIR, file, NULL);

		if (!gowl_module_manager_load_module(r->modules, path, &error))
			g_error("could not load %s: %s", path, error->message);
		g_free(path);
		g_free(file);
	}
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
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	g_assert_nonnull(r->compositor->selmon);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	gint i;

	if (r->started)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	for (i = 0; i < r->count; i++) {
		r->compositor->clients = g_list_remove(r->compositor->clients,
		                                       r->c[i]);
		r->compositor->fstack = g_list_remove(r->compositor->fstack,
		                                      r->c[i]);
		if (r->c[i]->scene != NULL)
			wlr_scene_node_destroy(&r->c[i]->scene->node);
		g_object_unref(r->c[i]);
	}
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_rmdir(r->runtime);
	g_free(r->runtime);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* A tiled window on the selected monitor's visible tag, in the focus
 * stack as well as the client list -- the actions work on whatever
 * focustop() answers. */
static GowlClient *
add_client(Rig *r)
{
	GowlMonitor *m = r->compositor->selmon;
	GowlClient  *c;

	g_assert_cmpint(r->count, <, RIG_MAX_CLIENTS);
	c = gowl_client_new();
	c->compositor = r->compositor;
	c->mon = m;
	c->bw = 0;
	c->tags = m->tagset[m->seltags];
	c->geom = m->w;
	c->frame = c->geom;
	c->scene = wlr_scene_tree_create(
		r->compositor->layers[GOWL_SCENE_LAYER_TILE]);
	{
		static const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
		gint i;

		for (i = 0; i < 4; i++)
			c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, grey);
	}

	r->compositor->clients = g_list_append(r->compositor->clients, c);
	r->compositor->fstack = g_list_prepend(r->compositor->fstack, c);
	r->c[r->count++] = c;
	return c;
}

static void
focus(Rig *r, GowlClient *c)
{
	r->compositor->fstack = g_list_remove(r->compositor->fstack, c);
	r->compositor->fstack = g_list_prepend(r->compositor->fstack, c);
	g_assert_true(focustop_public(r->compositor, r->compositor->selmon) == c);
}

static void
press(Rig *r, GowlAction action, const gchar *arg)
{
	GowlKeybindEntry kb;

	memset(&kb, 0, sizeof(kb));
	kb.action = action;
	kb.arg = (gchar *)arg;
	g_assert_true(gowl_compositor_run_keybind_entry(r->compositor, &kb));
}

/* Which scene layer a window's tree is parented into. */
static gint
layer_of(Rig *r, GowlClient *c)
{
	gint i;

	for (i = 0; i < GOWL_SCENE_LAYER_COUNT; i++) {
		if (c->scene->node.parent == r->compositor->layers[i])
			return i;
	}
	return -1;
}

/* How far down its parent's child list a node sits, counting from the
 * head.  Which end that is, is what the first case settles. */
static gint
index_in_parent(struct wlr_scene_node *node)
{
	struct wlr_scene_node *n;
	gint i = 0;

	wl_list_for_each(n, &node->parent->children, link) {
		if (n == node)
			return i;
		i++;
	}
	return -1;
}

static gboolean
is_tiling(Rig *r, GowlClient *c)
{
	GList   *tiling = gowl_compositor_tiling_clients(r->compositor,
	                                                 r->compositor->selmon);
	gboolean found = g_list_find(tiling, c) != NULL;

	g_list_free(tiling);
	return found;
}

/*
 * The assertion the whole feature reduces to: @c is drawn behind every
 * tiled window on its monitor.
 *
 * Both halves, because either alone passes for a broken version -- the
 * right layer with the wrong order is drawn on top of the tiles, and
 * the right order in the FLOAT layer is drawn on top of all of them.
 */
static void
assert_behind_the_tiling(Rig *r, GowlClient *c)
{
	gint  mine;
	GList *l;

	g_assert_cmpint(layer_of(r, c), ==, GOWL_SCENE_LAYER_TILE);
	mine = index_in_parent(&c->scene->node);
	g_assert_cmpint(mine, >=, 0);

	for (l = r->compositor->clients; l != NULL; l = l->next) {
		GowlClient *o = (GowlClient *)l->data;

		if (o == c || o->isfloating)
			continue;
		g_assert_cmpint(layer_of(r, o), ==, GOWL_SCENE_LAYER_TILE);
		g_assert_cmpint(mine, <, index_in_parent(&o->scene->node));
	}
}

/* ── The convention ──────────────────────────────────────────────── */

/*
 * The END of a scene tree's child list is the top of the stack.
 *
 * wlr_scene.h documents raise_to_top() and lower_to_bottom() but says
 * nothing about which way `children' runs, so every other case here
 * would be asserting a guess.  This asks wlroots instead, and if a
 * future version flips the list this is the one case that fails --
 * with a message that says what changed rather than "the window is in
 * the wrong place".
 */
static void
test_the_end_of_the_list_is_the_top(void)
{
	Rig r;
	GowlClient *a, *b;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);

	wlr_scene_node_raise_to_top(&a->scene->node);
	g_assert_cmpint(index_in_parent(&a->scene->node), >,
	                index_in_parent(&b->scene->node));

	wlr_scene_node_lower_to_bottom(&a->scene->node);
	g_assert_cmpint(index_in_parent(&a->scene->node), <,
	                index_in_parent(&b->scene->node));

	rig_down(&r);
}

/* ── One window ──────────────────────────────────────────────────── */

/*
 * A floating window goes behind the tiling, and comes back.
 *
 * The layer is the half that matters: a floating window is in the FLOAT
 * layer, which is ABOVE the whole tiling layer, so "behind the tiles"
 * cannot be expressed by ordering alone.  And it is still floating
 * afterwards -- out of the layout, keeping its own box -- which is the
 * difference between this and the float key.
 */
static void
test_a_floating_window_goes_behind_the_tiling(void)
{
	Rig r;
	GowlClient *a, *b;
	struct wlr_box floated;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);

	focus(&r, b);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	g_assert_true(b->isfloating);
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);
	floated = b->geom;

	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);

	assert_behind_the_tiling(&r, b);
	/* Still floating in every other respect. */
	g_assert_true(b->isfloating);
	g_assert_false(is_tiling(&r, b));
	g_assert_true(wlr_box_equal(&b->geom, &floated));
	/* And the window it is now behind still has the whole screen. */
	g_assert_true(is_tiling(&r, a));
	g_assert_cmpint(a->geom.width, ==, r.compositor->selmon->w.width);

	/* Back up. */
	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);
	g_assert_true(b->isfloating);
	g_assert_true(wlr_box_equal(&b->geom, &floated));

	rig_down(&r);
}

/*
 * It stays behind when the tiling is re-arranged.
 *
 * This is the case that fails for the obvious implementation.
 * arrange() re-parents every client on the monitor into its layer, and
 * wlr_scene_node_reparent() puts a node at the TOP of its new parent --
 * so a window lowered once and never again pops back over the tiles the
 * first time anything re-tiles: a new window, a closed window, a
 * changed master factor, a tag switch.
 */
static void
test_it_stays_behind_when_the_tiling_moves(void)
{
	Rig r;
	GowlClient *a, *b, *c;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, b);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);
	assert_behind_the_tiling(&r, b);

	/* A plain re-arrange. */
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	assert_behind_the_tiling(&r, b);

	/* A new tiled window, which is what re-tiling looks like in use. */
	c = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	g_assert_true(is_tiling(&r, a));
	g_assert_true(is_tiling(&r, c));
	assert_behind_the_tiling(&r, b);

	rig_down(&r);
}

/*
 * Focusing it does not bring it back up.
 *
 * The window keeps the keyboard while it is down -- that is what makes
 * "press the key again" work without hunting for the window first --
 * and focus_client() raises what it focuses.  If the raise were not
 * skipped the feature would undo itself on the very keystroke that
 * precedes undoing it deliberately.
 */
static void
test_focusing_it_does_not_raise_it(void)
{
	Rig r;
	GowlClient *a, *b;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, b);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);
	assert_behind_the_tiling(&r, b);

	/* Away and back, with the lift the real focus path asks for. */
	gowl_compositor_focus_client(r.compositor, a, TRUE);
	assert_behind_the_tiling(&r, b);
	gowl_compositor_focus_client(r.compositor, b, TRUE);
	assert_behind_the_tiling(&r, b);

	/* And it is the focused window, so the same key reaches it. */
	g_assert_true(focustop_public(r.compositor, r.compositor->selmon) == b);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);

	rig_down(&r);
}

/*
 * Rejoining the tiling forgets it.
 *
 * "Behind the tiling" is a floating-window state; a window that is one
 * of the tiles has nothing to be behind.  A flag left set would come
 * back the next time the window floated, which reads as the key having
 * been pressed by itself.
 */
static void
test_unfloating_forgets_it(void)
{
	Rig r;
	GowlClient *a, *b;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, b);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);
	g_assert_true(gowl_compositor_get_client_below(r.compositor, b));
	g_assert_true(is_tiling(&r, a));

	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	g_assert_false(b->isfloating);
	g_assert_false(gowl_compositor_get_client_below(r.compositor, b));
	g_assert_true(is_tiling(&r, b));

	/* Floating it again starts on top, where a floating window goes. */
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);

	rig_down(&r);
}

/*
 * A tiled window cannot go behind the tiling.
 *
 * It is already in it, so the request has no meaning -- and because the
 * key is a toggle, setting the flag anyway would make the NEXT press
 * appear to do nothing.
 */
static void
test_a_tiled_window_is_left_alone(void)
{
	Rig r;
	GowlClient *a, *b;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, b);

	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);

	g_assert_false(gowl_compositor_get_client_below(r.compositor, b));
	g_assert_true(is_tiling(&r, b));
	g_assert_true(is_tiling(&r, a));
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_TILE);

	rig_down(&r);
}

/*
 * An explicit direction, for a bind that is not a toggle.
 *
 * "on" twice has to leave the window down rather than flip it, which is
 * the difference between an argument and a toggle and is the only thing
 * a script driving this can rely on.
 */
static void
test_on_and_off_are_not_toggles(void)
{
	Rig r;
	GowlClient *a, *b;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, b);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	g_assert_true(is_tiling(&r, a));

	press(&r, GOWL_ACTION_TOGGLE_BELOW, "on");
	assert_behind_the_tiling(&r, b);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, "on");
	assert_behind_the_tiling(&r, b);

	press(&r, GOWL_ACTION_TOGGLE_BELOW, "off");
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, "off");
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);

	rig_down(&r);
}

/* ── All of them ─────────────────────────────────────────────────── */

/*
 * The second key sends every floating window down, and brings them all
 * back.
 *
 * The tiled window must not move: "all floating windows" is the scope,
 * and a version that pushed everything down would leave the screen
 * looking unchanged while having quietly re-stacked the tiling.
 */
static void
test_all_of_them_at_once(void)
{
	Rig r;
	GowlClient *tiled, *f1, *f2;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	tiled = add_client(&r);
	f1 = add_client(&r);
	f2 = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, f1);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	focus(&r, f2);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	g_assert_cmpint(layer_of(&r, f1), ==, GOWL_SCENE_LAYER_FLOAT);
	g_assert_cmpint(layer_of(&r, f2), ==, GOWL_SCENE_LAYER_FLOAT);

	press(&r, GOWL_ACTION_TOGGLE_BELOW_ALL, NULL);
	assert_behind_the_tiling(&r, f1);
	assert_behind_the_tiling(&r, f2);
	g_assert_true(is_tiling(&r, tiled));
	g_assert_false(gowl_compositor_get_client_below(r.compositor, tiled));

	press(&r, GOWL_ACTION_TOGGLE_BELOW_ALL, NULL);
	g_assert_cmpint(layer_of(&r, f1), ==, GOWL_SCENE_LAYER_FLOAT);
	g_assert_cmpint(layer_of(&r, f2), ==, GOWL_SCENE_LAYER_FLOAT);

	rig_down(&r);
}

/*
 * From a mixed state the key goes ALL THE WAY DOWN first.
 *
 * Toggling each window on its own would swap them -- one up, one down
 * -- which looks like nothing happened and is never what anybody
 * pressing "send them all back" meant.  The direction is decided by the
 * windows, so one already down does not change the answer.
 */
static void
test_a_mixed_state_goes_down_first(void)
{
	Rig r;
	GowlClient *tiled, *f1, *f2;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	tiled = add_client(&r);
	f1 = add_client(&r);
	f2 = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);
	focus(&r, f1);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);
	press(&r, GOWL_ACTION_TOGGLE_BELOW, NULL);
	focus(&r, f2);
	press(&r, GOWL_ACTION_TOGGLE_FLOAT, NULL);

	/* One down, one up. */
	g_assert_true(gowl_compositor_get_client_below(r.compositor, f1));
	g_assert_false(gowl_compositor_get_client_below(r.compositor, f2));

	press(&r, GOWL_ACTION_TOGGLE_BELOW_ALL, NULL);
	assert_behind_the_tiling(&r, f1);
	assert_behind_the_tiling(&r, f2);

	/* And now that they agree, the next press lifts both. */
	press(&r, GOWL_ACTION_TOGGLE_BELOW_ALL, NULL);
	g_assert_cmpint(layer_of(&r, f1), ==, GOWL_SCENE_LAYER_FLOAT);
	g_assert_cmpint(layer_of(&r, f2), ==, GOWL_SCENE_LAYER_FLOAT);
	g_assert_true(is_tiling(&r, tiled));

	rig_down(&r);
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/window-below/the-end-of-the-list-is-the-top",
	                test_the_end_of_the_list_is_the_top);
	g_test_add_func("/window-below/a-floating-window-goes-behind",
	                test_a_floating_window_goes_behind_the_tiling);
	g_test_add_func("/window-below/it-stays-behind-when-the-tiling-moves",
	                test_it_stays_behind_when_the_tiling_moves);
	g_test_add_func("/window-below/focusing-it-does-not-raise-it",
	                test_focusing_it_does_not_raise_it);
	g_test_add_func("/window-below/unfloating-forgets-it",
	                test_unfloating_forgets_it);
	g_test_add_func("/window-below/a-tiled-window-is-left-alone",
	                test_a_tiled_window_is_left_alone);
	g_test_add_func("/window-below/on-and-off-are-not-toggles",
	                test_on_and_off_are_not_toggles);
	g_test_add_func("/window-below/all-of-them-at-once",
	                test_all_of_them_at_once);
	g_test_add_func("/window-below/a-mixed-state-goes-down-first",
	                test_a_mixed_state_goes_down_first);

	return g_test_run();
}
