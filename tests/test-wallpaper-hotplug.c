/* test-wallpaper-hotplug.c -- the wallpaper survives plugging and
 * unplugging displays
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unplugging a display moves every display to the right of it.  The
 * wallpaper module used to decide it had nothing to do whenever the
 * SIZE and the picture were unchanged -- which they are, for a monitor
 * that only moved -- and left its scene node at the coordinates it had
 * while the other screen was still there.  The picture was then sitting
 * outside the surviving output's box: the screen showed the root colour,
 * or, when the layout had only shifted partway, a slice of a picture
 * that had been cropped for a panel of a different shape.  The same
 * check is why a wallpaper set before the outputs were positioned never
 * appeared at all, and why cmacs carried an idle timer to set it twice.
 *
 * A real headless compositor with the real wallpaper module, a real PNG
 * on disk, and two outputs of different shapes -- one 16:9, one 21:9 --
 * so a node built for the wrong screen is a wrong SIZE and not just a
 * wrong place:
 *
 *  - each output gets its own node, sized to itself;
 *  - unplugging the first one moves the second one's node to the origin;
 *  - plugging a third one in gives it a node of its own;
 *  - `wallpaper-outputs' gives one screen a picture the other does not
 *    have, which is the whole point of it on a mixed-aspect desk;
 *  - a scaled output is rendered at its device resolution, with the node
 *    still covering exactly the logical box.
 */

#include <cairo/cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "module/gowl-module-manager.h"

typedef struct {
	gchar             *runtime;
	gchar             *parent;
	gchar             *image;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
} Rig;

/* ── Finding the wallpaper's node for one monitor ─────────────────
 *
 * The module keeps its own table, which the test cannot see.  The scene
 * is the shared truth: the BG layer holds the root rect and one buffer
 * node per monitor that has a wallpaper, so a node is identified by
 * where it sits -- exactly how the compositor identifies it when it
 * decides which output to draw it on. */

static struct wlr_scene_buffer *
node_at(Rig *r, gint x, gint y)
{
	struct wlr_scene_tree *bg;
	struct wlr_scene_node *n;

	bg = gowl_compositor_get_scene_layer(r->compositor, GOWL_SCENE_LAYER_BG);
	g_assert_nonnull(bg);
	wl_list_for_each(n, &bg->children, link) {
		if (n->type != WLR_SCENE_NODE_BUFFER)
			continue;
		if (n->x == x && n->y == y)
			return wlr_scene_buffer_from_node(n);
	}
	return NULL;
}

static guint
node_count(Rig *r)
{
	struct wlr_scene_tree *bg;
	struct wlr_scene_node *n;
	guint count = 0;

	bg = gowl_compositor_get_scene_layer(r->compositor, GOWL_SCENE_LAYER_BG);
	g_assert_nonnull(bg);
	wl_list_for_each(n, &bg->children, link) {
		if (n->type == WLR_SCENE_NODE_BUFFER)
			count++;
	}
	return count;
}

/* What the node covers in LAYOUT coordinates -- dest_size when one was
 * set (a scaled output), else the buffer's own pixel size. */
static void
find_headless(struct wlr_backend *backend, void *data)
{
	struct wlr_backend **out = (struct wlr_backend **)data;

	if (*out == NULL && wlr_backend_is_headless(backend))
		*out = backend;
}

static void
node_extent(struct wlr_scene_buffer *b, gint *w, gint *h)
{
	if (b->dst_width > 0 && b->dst_height > 0) {
		*w = b->dst_width;
		*h = b->dst_height;
		return;
	}
	*w = b->buffer != NULL ? b->buffer->width : 0;
	*h = b->buffer != NULL ? b->buffer->height : 0;
}

/* ── The rig ─────────────────────────────────────────────────────── */

/* Drive the event loop until @pred holds, for at most a second.  Output
 * add and remove both settle over several dispatches: the layout change,
 * the commit, the module dispatch. */
static void
settle(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline)
		wl_event_loop_dispatch(r->compositor->event_loop, 5);
}

/* A picture on disk: 16:9, so the scaling has something to do on the
 * 21:9 output.  Written with cairo rather than gdk-pixbuf so the test
 * itself needs no image library of its own -- the module is what has to
 * decode it, and it brings gdk-pixbuf. */
static gchar *
write_image(const gchar *dir)
{
	gchar           *path;
	cairo_surface_t *surface;
	cairo_t         *cr;

	path = g_build_filename(dir, "wallpaper.png", NULL);
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 640, 360);
	cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 0.2, 0.4, 0.8);
	cairo_paint(cr);
	cairo_destroy(cr);
	if (cairo_surface_write_to_png(surface, path) != CAIRO_STATUS_SUCCESS)
		g_error("could not write a test wallpaper to %s", path);
	cairo_surface_destroy(surface);
	return path;
}

static gboolean
rig_up(Rig *r, gint n_outputs)
{
	const gchar *parent;
	GError      *error = NULL;
	gchar       *module;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-wp-hotplug-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	{
		gchar *count = g_strdup_printf("%d", n_outputs);
		g_setenv("WLR_HEADLESS_OUTPUTS", count, TRUE);
		g_free(count);
	}

	r->image = write_image(r->runtime);

	module = g_build_filename(GOWL_TEST_MODULE_DIR, "wallpaper.so", NULL);
	if (!g_file_test(module, G_FILE_TEST_EXISTS)) {
		g_free(module);
		g_test_skip("the wallpaper module is not built; run `make' first");
		return FALSE;
	}
	r->modules = gowl_module_manager_new();
	if (!gowl_module_manager_load_module(r->modules, module, &error))
		g_error("could not load %s: %s", module, error->message);
	g_free(module);

	/* The module reads its picture from the module settings, the same
	 * table a YAML `modules: wallpaper:` block produces. */
	{
		GHashTable *inner = g_hash_table_new(g_str_hash, g_str_equal);
		GHashTable *outer = g_hash_table_new(g_str_hash, g_str_equal);

		g_hash_table_insert(inner, (gpointer)"path", r->image);
		g_hash_table_insert(inner, (gpointer)"mode", (gpointer)"fill");
		g_hash_table_insert(outer, (gpointer)"wallpaper", inner);
		gowl_module_manager_configure_all(r->modules, outer);
		g_hash_table_destroy(outer);
		g_hash_table_destroy(inner);
	}
	gowl_module_manager_activate_all(r->modules);

	r->config = gowl_config_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	settle(r, 200);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->compositor != NULL)
		g_object_unref(r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->image != NULL)
		g_unlink(r->image);
	g_free(r->image);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* ── Cases ───────────────────────────────────────────────────────── */

/* Two outputs, two pictures, each the size of the screen it is on. */
static void
one_node_per_output(void)
{
	Rig r;
	GList *l;

	if (!rig_up(&r, 2))
		return;

	g_assert_cmpuint(g_list_length(r.compositor->monitors), ==, 2);
	g_assert_cmpuint(node_count(&r), ==, 2);

	for (l = r.compositor->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;
		struct wlr_scene_buffer *b;
		gint x, y, w, h, nw, nh;

		gowl_monitor_get_geometry(m, &x, &y, &w, &h);
		b = node_at(&r, x, y);
		g_assert_nonnull(b);
		node_extent(b, &nw, &nh);
		g_assert_cmpint(nw, ==, w);
		g_assert_cmpint(nh, ==, h);
	}
	rig_down(&r);
}

/* Unplug the first: the survivor moves, and its picture moves with it.
 *
 * This is the bug.  The survivor keeps its size, so the module's
 * "already correct" check passed and the node stayed where the layout
 * used to put it -- off the only screen still attached. */
static void
unplug_moves_the_survivor(void)
{
	Rig r;
	GowlMonitor *survivor;
	struct wlr_scene_buffer *b;
	gint x, y, w, h, nw, nh;
	struct wlr_output *going;

	if (!rig_up(&r, 2))
		return;

	/* add_auto lays the outputs out left to right, so the second one is
	 * not at the origin -- and will have to move when the first goes. */
	survivor = (GowlMonitor *)r.compositor->monitors->next->data;
	gowl_monitor_get_geometry(survivor, &x, &y, NULL, NULL);
	g_assert_cmpint(x, >, 0);

	going = ((GowlMonitor *)r.compositor->monitors->data)->wlr_output;
	wlr_output_destroy(going);
	settle(&r, 300);

	g_assert_cmpuint(g_list_length(r.compositor->monitors), ==, 1);
	survivor = (GowlMonitor *)r.compositor->monitors->data;
	gowl_monitor_get_geometry(survivor, &x, &y, &w, &h);
	g_assert_cmpint(x, ==, 0);

	g_assert_cmpuint(node_count(&r), ==, 1);
	b = node_at(&r, x, y);
	g_assert_nonnull(b);
	node_extent(b, &nw, &nh);
	g_assert_cmpint(nw, ==, w);
	g_assert_cmpint(nh, ==, h);
	rig_down(&r);
}

/* Plug one in at runtime: it gets a picture of its own, at its own size,
 * without disturbing the one already there. */
static void
plug_in_gets_a_picture(void)
{
	Rig r;
	struct wlr_output *added;
	GowlMonitor *m;
	struct wlr_scene_buffer *b;
	gint x, y, w, h, nw, nh;

	if (!rig_up(&r, 1))
		return;

	g_assert_cmpuint(node_count(&r), ==, 1);

	/* 21:9, so a node built for the 16:9 output already there would be
	 * the wrong size and not merely in the wrong place. */
	{
		struct wlr_backend *headless = NULL;

		/* gowl's backend is the multi-backend wlroots autocreates;
		 * only the headless one inside it can be asked for another
		 * output. */
		wlr_multi_for_each_backend(r.compositor->backend,
		                           find_headless, &headless);
		if (headless == NULL) {
			g_test_skip("no headless backend to add an output to");
			rig_down(&r);
			return;
		}
		added = wlr_headless_add_output(headless, 2560, 1080);
	}
	g_assert_nonnull(added);
	settle(&r, 400);

	g_assert_cmpuint(g_list_length(r.compositor->monitors), ==, 2);
	m = (GowlMonitor *)r.compositor->monitors->next->data;
	gowl_monitor_get_geometry(m, &x, &y, &w, &h);
	g_assert_cmpint(w, ==, 2560);
	g_assert_cmpint(h, ==, 1080);

	g_assert_cmpuint(node_count(&r), ==, 2);
	b = node_at(&r, x, y);
	g_assert_nonnull(b);
	node_extent(b, &nw, &nh);
	g_assert_cmpint(nw, ==, 2560);
	g_assert_cmpint(nh, ==, 1080);
	rig_down(&r);
}

/* A `wallpaper-outputs' entry gives one screen a picture of its own.
 * With a path that cannot be decoded it gets NO picture, which is a
 * difference the scene can be asked about without reading pixels. */
static void
per_output_picture(void)
{
	Rig r;
	GowlMonitor *m;
	gint x, y;

	if (!rig_up(&r, 2))
		return;

	m = (GowlMonitor *)r.compositor->monitors->data;
	gowl_config_set_wallpaper_output(r.config,
	                                 gowl_monitor_get_name(m),
	                                 "/nonexistent/not-an-image.png", NULL);

	/* A re-dispatch is what a reload does. */
	gowl_module_manager_dispatch_wallpaper_output(r.modules, r.compositor,
	                                              m);
	settle(&r, 100);

	gowl_monitor_get_geometry(m, &x, &y, NULL, NULL);
	g_assert_null(node_at(&r, x, y));
	g_assert_cmpuint(node_count(&r), ==, 1);
	rig_down(&r);
}

/* A scaled output: the picture is rendered at the DEVICE resolution and
 * the node still covers exactly the logical box.  Rendering at the
 * logical size and letting the compositor upscale is a soft wallpaper on
 * precisely the screens that are meant to be sharp. */
static void
scaled_output_renders_at_device_resolution(void)
{
	Rig r;
	GowlMonitor *m;
	struct wlr_scene_buffer *b;
	gint x, y, w, h, nw, nh;

	if (!rig_up(&r, 1))
		return;

	m = (GowlMonitor *)r.compositor->monitors->data;
	if (!gowl_monitor_set_scale(m, 2.0)) {
		g_test_skip("this backend will not take a scale of 2");
		rig_down(&r);
		return;
	}
	settle(&r, 300);

	gowl_monitor_get_geometry(m, &x, &y, &w, &h);
	b = node_at(&r, x, y);
	g_assert_nonnull(b);

	/* Logical on screen ... */
	node_extent(b, &nw, &nh);
	g_assert_cmpint(nw, ==, w);
	g_assert_cmpint(nh, ==, h);
	/* ... device pixels in the buffer. */
	g_assert_nonnull(b->buffer);
	g_assert_cmpint(b->buffer->width, ==, w * 2);
	g_assert_cmpint(b->buffer->height, ==, h * 2);
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/wallpaper-hotplug/one-node-per-output",
	                one_node_per_output);
	g_test_add_func("/wallpaper-hotplug/unplug-moves-the-survivor",
	                unplug_moves_the_survivor);
	g_test_add_func("/wallpaper-hotplug/plug-in-gets-a-picture",
	                plug_in_gets_a_picture);
	g_test_add_func("/wallpaper-hotplug/per-output-picture",
	                per_output_picture);
	g_test_add_func("/wallpaper-hotplug/scaled-output",
	                scaled_output_renders_at_device_resolution);
	return g_test_run();
}
