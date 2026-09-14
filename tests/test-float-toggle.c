/* test-float-toggle.c -- taking a window out of the tiling and back
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * `toggle_float' is two operations wearing one name, and they fail in
 * different ways:
 *
 *   FLOATING A TILED WINDOW has to take it out of the layout, and the
 *   half that gets forgotten is the OTHER windows.  A window that leaves
 *   the tiling without the rest being re-arranged leaves a hole where it
 *   was -- the tiling is still correct for a window count that is no
 *   longer true, which reads as "the layout broke" rather than as "the
 *   float key is wrong".
 *
 *   UNFLOATING has to put it back into whatever layout the CURRENT TAG
 *   is using, at wherever that layout decides -- not at the geometry it
 *   happened to be floating at.  A window that rejoins the tiling while
 *   keeping its floating box overlaps its neighbours, and the next
 *   re-tile silently fixes it, so the bug only shows on the frame after
 *   the keystroke and then goes away.
 *
 * Both are invisible to a test that only checks the flag, which is why
 * this checks the SCENE LAYER and the GEOMETRY instead: the flag is not
 * the behaviour, it is the thing the behaviour is derived from.
 *
 * Driven through the keybind ACTION rather than through setfloating(),
 * so what is asserted is the path a key press actually takes --
 * including that the action operates on the FOCUSED window and refuses
 * a fullscreen one.
 *
 * ONE THING IS INTERPOSED and nothing else: gowl_compositor_place_client(),
 * which is where a layout's answer meets wlroots.  The same trick
 * tests/test-layout-orientation.c uses, and for the same reason -- the
 * real thing calls resize_client(), which configures the client's
 * xdg_toplevel, and a window a test can fabricate has none.  Everything
 * above that line is the real code: the real action, the real
 * setfloating(), the real arrange, the real tile layout deciding who is
 * in it and where they go.  Only the last step writes the box into the
 * client instead of into the scene.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

#define RIG_MAX_CLIENTS 8

/*
 * Where a layout's answer would meet wlroots.  See the note above: the
 * real one configures an xdg_toplevel that a fabricated client does not
 * have, so this records the box and the assertions read it back.  The
 * frame follows the geometry, because that is what the real one does
 * once the window has landed and several things here read it.
 */
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	client->geom = (struct wlr_box){ x, y, width, height };
	client->frame = client->geom;
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

/* Only the tiling layout, which is what "the current tag's layout" means
 * for every case here.  Without a layout provider `arrange' has nothing
 * to ask and every geometry assertion below would pass vacuously. */
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
	r->runtime = g_build_filename(parent, "gowl-float-XXXXXX", NULL);
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

/*
 * A tiled window on the selected monitor's visible tag.
 *
 * Pushed onto the FOCUS STACK as well as the client list, because the
 * action works on whatever focustop() answers -- so a rig that only
 * listed the clients would be testing the toggle against NULL.
 */
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
	/* The four border rects every mapped window carries.  Drawing the
	 * frame sizes them, so a tree without them segfaults the moment the
	 * layout places the window -- which is the first thing every case
	 * here does. */
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

/* Make @c the one the action will act on. */
static void
focus(Rig *r, GowlClient *c)
{
	r->compositor->fstack = g_list_remove(r->compositor->fstack, c);
	r->compositor->fstack = g_list_prepend(r->compositor->fstack, c);
	g_assert_true(focustop_public(r->compositor, r->compositor->selmon) == c);
}

/* What the key does, through the path a key press takes. */
static void
press_toggle_float(Rig *r)
{
	GowlKeybindEntry kb;

	memset(&kb, 0, sizeof(kb));
	kb.action = GOWL_ACTION_TOGGLE_FLOAT;
	g_assert_true(gowl_compositor_run_keybind_entry(r->compositor, &kb));
}

/* Which scene layer a window's tree is parented into.  The flag says
 * what the compositor thinks; this says what is actually drawn, and a
 * floating window still stacked among the tiles is the failure the flag
 * cannot see. */
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

static gboolean
is_tiling(Rig *r, GowlClient *c)
{
	GList   *tiling = gowl_compositor_tiling_clients(r->compositor,
	                                                 r->compositor->selmon);
	gboolean found = g_list_find(tiling, c) != NULL;

	g_list_free(tiling);
	return found;
}

/* ── The toggle ──────────────────────────────────────────────────── */

/*
 * Floating a tiled window takes it out of the layout AND re-tiles what
 * is left.
 *
 * The second half is the one that gets forgotten.  Two windows share the
 * screen; float one and the other has to grow into the whole of it.  A
 * version that only flipped the flag and reparented would leave the
 * remaining window at half width with a hole beside it, which reads as
 * the layout being broken rather than as the float key being wrong.
 */
static void
test_floating_a_window_retiles_the_rest(void)
{
	Rig r;
	GowlClient *a, *b;
	struct wlr_box before;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	b = add_client(&r);
	gowl_compositor_arrange(r.compositor, r.compositor->selmon);

	/* Two tiled windows, neither of them filling the screen. */
	g_assert_true(is_tiling(&r, a));
	g_assert_true(is_tiling(&r, b));
	g_assert_cmpint(a->geom.width, <, r.compositor->selmon->w.width);
	before = a->geom;

	focus(&r, b);
	press_toggle_float(&r);

	/* The floated one is out of the tiling and drawn above it. */
	g_assert_true(b->isfloating);
	g_assert_false(is_tiling(&r, b));
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_FLOAT);

	/* And the one left behind grew into the space.  Not merely "still
	 * tiled": the whole point is that the layout was re-run. */
	g_assert_true(is_tiling(&r, a));
	g_assert_cmpint(a->geom.width, >, before.width);
	g_assert_cmpint(a->geom.width, ==, r.compositor->selmon->w.width);

	rig_down(&r);
}

/*
 * Unfloating puts the window back into the CURRENT TAG'S layout, at
 * wherever that layout puts it.
 *
 * Not at the box it was floating at, which is the failure that hides:
 * the window rejoins the tiling overlapping its neighbours, and the next
 * re-tile for any other reason silently corrects it -- so the bug is
 * visible for one frame after the keystroke and then gone.
 */
static void
test_unfloating_retiles_onto_the_current_tag(void)
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
	focus(&r, b);
	press_toggle_float(&r);
	g_assert_true(b->isfloating);

	/* Drag it somewhere a layout would never put it. */
	b->geom.x = r.compositor->selmon->w.x + 37;
	b->geom.y = r.compositor->selmon->w.y + 91;
	b->geom.width = 123;
	b->geom.height = 77;

	press_toggle_float(&r);

	g_assert_false(b->isfloating);
	g_assert_true(is_tiling(&r, b));
	g_assert_cmpint(layer_of(&r, b), ==, GOWL_SCENE_LAYER_TILE);

	/* The floating box is gone: it is where the tiling put it, sharing
	 * the screen with the other window rather than sitting at 123x77. */
	g_assert_cmpint(b->geom.width, !=, 123);
	g_assert_cmpint(b->geom.height, !=, 77);
	g_assert_cmpint(a->geom.width + b->geom.width, ==,
	                r.compositor->selmon->w.width);
	g_assert_true(wlr_box_empty(&a->geom) == false);

	rig_down(&r);
}

/*
 * It acts on the FOCUSED window, which is the only one the user is
 * pointing at.
 *
 * A toggle that floated whatever happened to be first in the client list
 * would look right in every single-window test and be wrong in every
 * real use.
 */
static void
test_it_toggles_the_focused_window(void)
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
	c = add_client(&r);

	focus(&r, b);
	press_toggle_float(&r);
	g_assert_false(a->isfloating);
	g_assert_true(b->isfloating);
	g_assert_false(c->isfloating);

	focus(&r, c);
	press_toggle_float(&r);
	g_assert_false(a->isfloating);
	g_assert_true(b->isfloating);
	g_assert_true(c->isfloating);

	/* And back, one at a time. */
	press_toggle_float(&r);
	g_assert_false(c->isfloating);
	g_assert_true(b->isfloating);

	rig_down(&r);
}

/*
 * A fullscreen window is left alone.
 *
 * Fullscreen already owns the whole output and is drawn in its own
 * layer, so floating it would reparent it out of that layer while it
 * still believes it is fullscreen -- a window that covers the screen,
 * ignores the layout, and is in neither state's list.  The action
 * refuses rather than untangling it; unfullscreen first.
 */
static void
test_a_fullscreen_window_is_left_alone(void)
{
	Rig r;
	GowlClient *a;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	a = add_client(&r);
	focus(&r, a);
	a->isfullscreen = TRUE;

	press_toggle_float(&r);
	g_assert_false(a->isfloating);

	a->isfullscreen = FALSE;
	press_toggle_float(&r);
	g_assert_true(a->isfloating);

	rig_down(&r);
}

/*
 * An empty tag is not a crash.
 *
 * focustop() answers NULL with nothing focused, and the action is
 * reachable from a key that does not care whether a window is there.
 */
static void
test_no_window_is_not_a_crash(void)
{
	Rig r;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	press_toggle_float(&r);
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/float-toggle/floating-a-window-retiles-the-rest",
	                test_floating_a_window_retiles_the_rest);
	g_test_add_func("/float-toggle/unfloating-retiles-onto-the-current-tag",
	                test_unfloating_retiles_onto_the_current_tag);
	g_test_add_func("/float-toggle/it-toggles-the-focused-window",
	                test_it_toggles_the_focused_window);
	g_test_add_func("/float-toggle/a-fullscreen-window-is-left-alone",
	                test_a_fullscreen_window_is_left_alone);
	g_test_add_func("/float-toggle/no-window-is-not-a-crash",
	                test_no_window_is_not_a_crash);

	return g_test_run();
}
