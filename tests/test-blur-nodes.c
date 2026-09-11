/* test-blur-nodes.c -- the blur module's nodes live and die with the window
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The blur module hangs two scene buffers off a window's own scene tree:
 * a drop shadow and a blurred backdrop.  It is told DESTROY for two
 * different things.  A window that is gone: its tree went at unmap, with
 * the nodes in it.  And a window the compositor is taking over --
 * adopting it into the scratchpad, showing it in the panel, giving it
 * back -- whose tree stays.  The module used to treat the second like the
 * first and merely forget its nodes, so every trip into the scratchpad
 * and back left another tile-sized shadow and backdrop hanging off the
 * window, drawn across the next column of the panel.
 *
 * Runs the real blur.so in a headless compositor against a window that
 * is only a scene tree: the module decorates from the window's geometry
 * and never looks at its surface.  It draws with GLES2; where there is
 * none, the test is skipped.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_scene.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-effects.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	gboolean           started;
	GowlClient        *c;
} Rig;

/* Buffers anywhere under @node, enabled or not. */
static guint
count_buffers(struct wlr_scene_node *node)
{
	struct wlr_scene_tree *tree;
	struct wlr_scene_node *child;
	guint                  n;

	if (node->type == WLR_SCENE_NODE_BUFFER)
		return 1;
	if (node->type != WLR_SCENE_NODE_TREE)
		return 0;
	tree = wlr_scene_tree_from_node(node);
	n = 0;
	wl_list_for_each(child, &tree->children, link)
		n += count_buffers(child);
	return n;
}

/* Whether the window draws anything just past its right edge, where the
 * next column of a panel would be.  The scene's own hit test knows every
 * node's size, the shadow's texture-only buffer included. */
static gboolean
reaches_past_right(GowlClient *c)
{
	gdouble nx;
	gdouble ny;

	return wlr_scene_node_at(&c->scene->node,
	                         c->geom.x + c->geom.width + 8.5,
	                         c->geom.y + c->geom.height / 2.0,
	                         &nx, &ny) != NULL;
}

/* A fresh scene tree for the window, where the compositor would put it. */
static void
new_tree(Rig *r)
{
	r->c->scene = wlr_scene_tree_create(
		r->compositor->layers[GOWL_SCENE_LAYER_TILE]);
	wlr_scene_node_set_position(&r->c->scene->node, r->c->geom.x,
	                            r->c->geom.y);
}

static void
send(
	Rig                  *r,
	GowlSceneEffectEvent  event
){
	gowl_effects_client_event(r->compositor, r->c, event,
	                          event == GOWL_SCENE_EFFECT_GEOMETRY
	                          ? &r->c->geom : NULL, FALSE);
}

/* Placed as an ordinary tile, which is decorated. */
static void
as_tile(Rig *r)
{
	r->c->isoverlay = FALSE;
	r->c->overlay_visible = FALSE;
	r->c->overlay_group = 0;
	send(r, GOWL_SCENE_EFFECT_GEOMETRY);
}

/* What adopting a window and showing it in the panel sends: DESTROY with
 * the tree staying, then its place in the panel. */
static void
into_panel(Rig *r)
{
	send(r, GOWL_SCENE_EFFECT_DESTROY);
	r->c->isoverlay = TRUE;
	r->c->overlay_visible = TRUE;
	r->c->overlay_group = 1;
	send(r, GOWL_SCENE_EFFECT_GEOMETRY);
}

/* And giving it back: DESTROY again, then placed as a tile. */
static void
out_of_panel(Rig *r)
{
	send(r, GOWL_SCENE_EFFECT_DESTROY);
	as_tile(r);
}

/* A headless compositor drawing with GLES2, the blur module loaded, and a
 * translucent window.  FALSE when there is no GLES2 to draw with. */
static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	gchar       *path;
	GError      *error;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in: no systemd user
	 * targets, and a runtime directory of its own inside the real one,
	 * which keeps a socket path under the 108 bytes it gets. */
	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-blur-nodes-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "gles2", TRUE);

	error = NULL;
	r->modules = gowl_module_manager_new();
	path = g_build_filename(GOWL_TEST_MODULE_DIR, "blur.so", NULL);
	if (!gowl_module_manager_load_module(r->modules, path, &error))
		g_error("could not load %s: %s", path, error->message);
	g_free(path);
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
	if (!wlr_renderer_is_gles2(r->compositor->renderer))
		return FALSE;
	g_assert_nonnull(r->compositor->selmon);

	r->c = gowl_client_new();
	r->c->compositor = r->compositor;
	r->c->mon = r->compositor->selmon;
	r->c->bw = 2;
	r->c->alpha = 0.8f;
	r->c->geom.x = 200;
	r->c->geom.y = 150;
	r->c->geom.width = 400;
	r->c->geom.height = 300;
	new_tree(r);
	return TRUE;
}

/* Modules before the compositor, as the compositor tears down; a started
 * compositor leaves nothing behind in its runtime directory. */
static void
rig_down(Rig *r)
{
	if (r->c != NULL)
		send(r, GOWL_SCENE_EFFECT_DESTROY);
	if (r->started)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	if (r->c != NULL) {
		if (r->c->scene != NULL)
			wlr_scene_node_destroy(&r->c->scene->node);
		r->c->scene = NULL;
		r->c->mon = NULL;
		g_object_unref(r->c);
	}
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->started)
		g_assert_cmpint(g_rmdir(r->runtime), ==, 0);
	else
		g_rmdir(r->runtime);
	g_free(r->runtime);
}

static void
test_nodes_follow_the_window(void)
{
	Rig    r;
	gchar *path;
	guint  n;
	guint  trip;

	path = g_build_filename(GOWL_TEST_MODULE_DIR, "blur.so", NULL);
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_free(path);
		g_test_skip("the blur module is not built; run `make' first");
		return;
	}
	g_free(path);
	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}

	/* A tile: a shadow reaching past its edges, and whatever backdrop the
	 * wallpaper capture made -- counted, not assumed. */
	as_tile(&r);
	n = count_buffers(&r.c->scene->node);
	g_assert_cmpuint(n, >=, 1);
	g_assert_true(reaches_past_right(r.c));

	/* Into the panel.  Nothing of the tile's may stay on the window, and
	 * a column gets nothing new: the module leaves overlays alone. */
	into_panel(&r);
	g_assert_cmpuint(count_buffers(&r.c->scene->node), ==, 0);
	g_assert_false(reaches_past_right(r.c));

	/* Hovering it changes its opacity, which wakes the module too. */
	gowl_effects_alpha_changed(r.c, 1.0f);
	g_assert_cmpuint(count_buffers(&r.c->scene->node), ==, 0);

	/* Out and back in, again and again: one set as a tile, none in the
	 * panel, never an extra pair. */
	for (trip = 0; trip < 3; trip++) {
		out_of_panel(&r);
		g_assert_cmpuint(count_buffers(&r.c->scene->node), ==, n);
		g_assert_true(reaches_past_right(r.c));
		into_panel(&r);
		g_assert_cmpuint(count_buffers(&r.c->scene->node), ==, 0);
	}

	/* Closed the way the compositor closes a window: UNMAP, the tree
	 * destroyed with the nodes in it, then DESTROY.  Mapped again, it is
	 * decorated from scratch. */
	out_of_panel(&r);
	send(&r, GOWL_SCENE_EFFECT_UNMAP);
	wlr_scene_node_destroy(&r.c->scene->node);
	send(&r, GOWL_SCENE_EFFECT_DESTROY);
	new_tree(&r);
	as_tile(&r);
	g_assert_cmpuint(count_buffers(&r.c->scene->node), ==, n);

	/* A tree that goes without a word: the nodes' pointers go with it, so
	 * the next placement draws a full set on the new tree rather than
	 * taking a dead shadow for a live one. */
	wlr_scene_node_destroy(&r.c->scene->node);
	new_tree(&r);
	as_tile(&r);
	g_assert_cmpuint(count_buffers(&r.c->scene->node), ==, n);

	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/blur-nodes/follow-the-window",
	                test_nodes_follow_the_window);

	return g_test_run();
}
