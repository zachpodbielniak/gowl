/* test-layout-hotplug.c -- a layout follows the shape its output has NOW
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every axis-aware layout (tile, scrolling, and the rest built on
 * modules/layout-axis.h) asks the monitor for its shape on every
 * arrange, and tests/test-layout-orientation.c proves the layouts do
 * the right thing with whatever shape they are handed.  What that test
 * cannot see is whether anything ASKS them again when the shape
 * changes underneath windows that are already placed: it sets the box
 * by hand and calls the layout itself.
 *
 * This one goes through the compositor: a real headless backend, a
 * landscape output the session starts with, and then the two ways a
 * screen turns tall while a session is running --
 *
 *   - a portrait monitor is plugged in (a dock), and windows open on it;
 *   - an output is rotated, with windows already tiled on it.
 *
 * In both the windows must come out stacked top to bottom, without
 * anyone calling arrange by hand.
 *
 * gowl_compositor_place_client() is interposed, as in the other layout
 * tests: the real one configures an xdg_toplevel a fabricated window
 * does not have.  Everything that decides WHETHER and HOW to place is
 * the real code.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

#define RIG_MAX_CLIENTS 8

/* Where a layout's answer would meet wlroots; recorded instead. */
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	(void)self;
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

static const gchar *const layout_modules[] = { "tile", "scrolling", NULL };

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
	r->runtime = g_build_filename(parent, "gowl-layout-hp-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	/* The laptop: wider than it is tall. */
	g_setenv("GOWL_OUTPUT_SIZE", "1920x1200", TRUE);

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
	g_unsetenv("GOWL_OUTPUT_SIZE");
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_rmdir(r->runtime);
	g_free(r->runtime);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* A tiled window on @m's visible tag, with the border rects a mapped
 * window carries (drawing the frame sizes them). */
static GowlClient *
add_client(Rig *r, GowlMonitor *m)
{
	static const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	GowlClient *c;
	gint i;

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
	for (i = 0; i < 4; i++)
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, grey);
	r->compositor->clients = g_list_append(r->compositor->clients, c);
	r->compositor->fstack = g_list_prepend(r->compositor->fstack, c);
	r->c[r->count++] = c;
	return c;
}

static void
settle(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline)
		wl_event_loop_dispatch(r->compositor->event_loop, 5);
}

static void
find_headless(struct wlr_backend *backend, void *data)
{
	struct wlr_backend **out = (struct wlr_backend **)data;

	if (*out == NULL && wlr_backend_is_headless(backend))
		*out = backend;
}

/* Two windows side by side: same height, one to the right of the other. */
static void
assert_side_by_side(GowlClient *a, GowlClient *b, GowlMonitor *m)
{
	g_assert_cmpint(a->geom.height, ==, m->w.height);
	g_assert_cmpint(b->geom.height, ==, m->w.height);
	g_assert_cmpint(a->geom.x, !=, b->geom.x);
	g_assert_cmpint(a->geom.y, ==, b->geom.y);
}

/* Two windows stacked: full width each, one above the other. */
static void
assert_stacked(GowlClient *a, GowlClient *b, GowlMonitor *m)
{
	g_assert_cmpint(a->geom.width, ==, m->w.width);
	g_assert_cmpint(b->geom.width, ==, m->w.width);
	g_assert_cmpint(a->geom.y, !=, b->geom.y);
	g_assert_cmpint(a->geom.x, ==, b->geom.x);
}

/*
 * A portrait monitor plugged into a running landscape session lays its
 * windows out top to bottom, for every axis-aware layout.
 */
static void
test_hotplugged_portrait(gconstpointer data)
{
	const gchar *layout = data;
	struct wlr_backend *headless = NULL;
	struct wlr_output *added;
	GowlMonitor *laptop, *tall;
	GowlClient *a, *b;
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor here");
		return;
	}
	laptop = r.compositor->selmon;
	g_assert_cmpint(laptop->m.width, >, laptop->m.height);

	wlr_multi_for_each_backend(r.compositor->backend, find_headless,
	                           &headless);
	if (headless == NULL) {
		rig_down(&r);
		g_test_skip("no headless backend to add an output to");
		return;
	}
	/* GOWL_OUTPUT_SIZE sizes EVERY mode-less output; the dock's monitor
	 * brings its own size. */
	g_unsetenv("GOWL_OUTPUT_SIZE");
	added = wlr_headless_add_output(headless, 1080, 1920);
	g_assert_nonnull(added);
	settle(&r, 100);
	tall = (GowlMonitor *)added->data;
	g_assert_nonnull(tall);
	g_assert_cmpint(tall->m.height, >, tall->m.width);

	g_assert_true(gowl_layout_set(r.compositor, tall, layout));
	a = add_client(&r, tall);
	b = add_client(&r, tall);
	gowl_compositor_arrange(r.compositor, tall);

	assert_stacked(a, b, tall);
	rig_down(&r);
}

/*
 * Rotating an output that already has windows tiled on it re-tiles
 * them for the new shape, and rotating back restores the old one --
 * nobody but the rotation is involved.
 */
static void
test_rotated_with_windows(gconstpointer data)
{
	const gchar *layout = data;
	GowlMonitor *m;
	GowlClient *a, *b;
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor here");
		return;
	}
	m = r.compositor->selmon;
	g_assert_true(gowl_layout_set(r.compositor, m, layout));
	a = add_client(&r, m);
	b = add_client(&r, m);
	gowl_compositor_arrange(r.compositor, m);
	assert_side_by_side(a, b, m);

	g_assert_true(gowl_monitor_set_transform(m, WL_OUTPUT_TRANSFORM_90));
	settle(&r, 50);
	g_assert_cmpint(m->m.height, >, m->m.width);
	assert_stacked(a, b, m);

	g_assert_true(gowl_monitor_set_transform(m, WL_OUTPUT_TRANSFORM_NORMAL));
	settle(&r, 50);
	g_assert_cmpint(m->m.width, >, m->m.height);
	assert_side_by_side(a, b, m);
	rig_down(&r);
}

/*
 * A resize that reaches the monitor through a path other than the
 * layout-change listener still re-tiles.
 *
 * Several paths write the new geometry themselves --
 * gowl_compositor_notify_output_resized() (a nested backend's
 * request_state, the output-resized fallback) and the safety nets in
 * gowl_monitor_set_mode/_scale/_transform/_position -- and each one set
 * the window area to the full output (m->w = m->m) before calling
 * arrangelayers.  arrangelayers re-tiles only when the usable area it
 * computes differs from m->w; with nothing reserving space, the area IS
 * the output, so it compared equal to what had just been written and
 * the windows kept the layout of the old shape: side by side on a
 * screen that had turned tall.
 *
 * The layout listener is unhooked for the change so the path under
 * test is the only one that sees it -- exactly the window those paths
 * exist for ("if on_layout_change() did not fire").
 */
static void
test_resize_path_retiles(gconstpointer data)
{
	const gchar *layout = data;
	struct wlr_output_state state;
	GowlMonitor *m;
	GowlClient *a, *b;
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor here");
		return;
	}
	m = r.compositor->selmon;
	g_assert_true(gowl_layout_set(r.compositor, m, layout));
	a = add_client(&r, m);
	b = add_client(&r, m);
	gowl_compositor_arrange(r.compositor, m);
	assert_side_by_side(a, b, m);

	wl_list_remove(&r.compositor->layout_change.link);
	wl_list_init(&r.compositor->layout_change.link);
	wlr_output_state_init(&state);
	wlr_output_state_set_transform(&state, WL_OUTPUT_TRANSFORM_90);
	g_assert_true(wlr_output_commit_state(m->wlr_output, &state));
	wlr_output_state_finish(&state);
	/* Still the old shape: nothing has told the monitor yet. */
	g_assert_cmpint(m->m.width, >, m->m.height);

	gowl_compositor_notify_output_resized(r.compositor, m->wlr_output);
	g_assert_cmpint(m->m.height, >, m->m.width);
	assert_stacked(a, b, m);

	wl_list_remove(&r.compositor->layout_change.link);
	wl_signal_add(&r.compositor->output_layout->events.change,
	              &r.compositor->layout_change);
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/layout/hotplug/portrait-monitor/tile",
	                     "tile", test_hotplugged_portrait);
	g_test_add_data_func("/layout/hotplug/portrait-monitor/scrolling",
	                     "scrolling", test_hotplugged_portrait);
	g_test_add_data_func("/layout/hotplug/rotated-output/tile",
	                     "tile", test_rotated_with_windows);
	g_test_add_data_func("/layout/hotplug/rotated-output/scrolling",
	                     "scrolling", test_rotated_with_windows);
	g_test_add_data_func("/layout/hotplug/resize-path-retiles/tile",
	                     "tile", test_resize_path_retiles);
	g_test_add_data_func("/layout/hotplug/resize-path-retiles/scrolling",
	                     "scrolling", test_resize_path_retiles);
	return g_test_run();
}
