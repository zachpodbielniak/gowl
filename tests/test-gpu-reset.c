/* test-gpu-reset.c -- a lost renderer is replaced, and nothing it drew
 * stays blank
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * wlroots emits a renderer's `lost' signal on a GPU reset and expects the
 * compositor to destroy that renderer and make another.  gowl's handler
 * made one and dropped it, leaving every output on the dead renderer: a
 * desktop that no longer drew.  A real GPU reset cannot be staged in a
 * test, but the signal can be emitted, and the signal is all the
 * compositor ever sees of one.
 *
 * Both cases start a real compositor -- headless backend, pixman
 * renderer, a private runtime directory, systemd off -- in a subprocess,
 * so that an abort is a failed test rather than a dead suite.
 *
 * The first loses the renderer three times, with a window capture
 * running throughout and, the last time, the screen locked behind an
 * animated backdrop.  It checks the swap: it waits for the event loop,
 * destroys the old renderer and allocator, moves every output -- the
 * capture's private one too -- to the new pair, listens for the next
 * loss, puts the cursor back, keeps the locked screen covered until
 * every monitor has a new backdrop frame, and announces
 * ::renderer-replaced.  Then it finalizes, which wlroots only allows
 * once every listener is off the renderers it destroyed.
 *
 * The second loads the modules that draw into scene buffers of their
 * own -- wallpaper, screenlock, roundcorners -- lets them draw, renders
 * so the scene uploads their pixels and lets go of the buffers, and
 * loses the renderer.  While the old one is destroyed their nodes are
 * blank; once the swap is done, none of them may be.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-frame-sink.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/* The modules that draw into scene buffers of their own.  The bar does
 * too, but it cannot be loaded without its shipped widgets polling the
 * machine the test runs on. */
static const gchar *const drawing_modules[] = {
	"wallpaper", "screenlock", "roundcorners"
};

/* A 4x4 PNG for the wallpaper: small enough to spell out, and a file the
 * wallpaper module decodes like any other. */
static const guint8 tiny_png[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
	0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
	0x00, 0x04, 0x08, 0x02, 0x00, 0x00, 0x00, 0x26, 0x93, 0x09, 0x29,
	0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63,
	0x30, 0x4e, 0x9b, 0x09, 0x47, 0x0c, 0xc4, 0x71, 0x00, 0xeb, 0x23,
	0x13, 0x21, 0x2a, 0x62, 0x60, 0x73, 0x00, 0x00, 0x00, 0x00, 0x49,
	0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

/* Records that the object a listener watched was destroyed, and runs
 * @then at that moment when set.  The listener takes itself off first:
 * wlroots asserts that a destroyed object's signals have no listeners
 * left. */
typedef struct {
	struct wl_listener listener;
	gboolean           gone;
	void             (*then)(gpointer data);
	gpointer           data;
} Gone;

static void
on_gone(struct wl_listener *listener, void *data)
{
	Gone *g;

	(void)data;
	g = wl_container_of(listener, g, listener);
	wl_list_remove(&listener->link);
	g->gone = TRUE;
	if (g->then != NULL)
		g->then(g->data);
}

static void
watch(Gone *g, struct wl_signal *signal, void (*then)(gpointer),
      gpointer data)
{
	memset(g, 0, sizeof(*g));
	g->listener.notify = on_gone;
	g->then = then;
	g->data = data;
	wl_signal_add(signal, &g->listener);
}

/* Nothing may reach the session this runs in: no systemd user targets,
 * and a runtime directory of its own, made inside the real one so its
 * path stays under the 108 bytes a socket path gets. */
static gchar *
isolate(guint outputs)
{
	const gchar *parent;
	gchar       *runtime;
	gchar       *count;

	/* A critical is what this looks for; the reset's own warning and an
	 * environment warning (no Xwayland binary, say) are not failures. */
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	runtime = g_build_filename(parent, "gowl-gpu-reset-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(runtime, 0700));

	count = g_strdup_printf("%u", outputs);
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", count, TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_free(count);
	return runtime;
}

static GowlCompositor *
start(GowlConfig *config, GowlModuleManager *modules)
{
	GowlCompositor *compositor;
	GError         *error;

	error = NULL;
	compositor = gowl_compositor_new();
	gowl_compositor_set_config(compositor, config);
	gowl_compositor_set_module_manager(compositor, modules);
	if (!gowl_compositor_start(compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);
	return compositor;
}

/* Have every monitor draw a frame the way the compositor draws them --
 * so the scene makes its textures, and lets go of the buffers it made
 * them from -- and wait until each has been committed.  The compositor
 * commits from its own frame handler, so this asks for a frame and runs
 * the loop until the output's commit count moves; a renderer that could
 * not draw would leave it where it was. */
static void
render(GowlCompositor *compositor)
{
	GList *l;

	for (l = compositor->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;
		guint32      seq;
		gint         i;

		seq = m->wlr_output->commit_seq;
		wlr_output_schedule_frame(m->wlr_output);
		for (i = 0; i < 100 && m->wlr_output->commit_seq == seq; i++)
			wl_event_loop_dispatch(compositor->event_loop, 10);
		g_assert_cmpuint(m->wlr_output->commit_seq, !=, seq);
	}
}

/* What a GPU reset looks like from here: the renderer's lost signal.  The
 * swap has to wait for the loop -- the signal can arrive in the middle of
 * a render pass, where the renderer must not be destroyed -- and then
 * destroy the old renderer and allocator.  @then runs while the old
 * renderer is destroyed, once the scene has dropped its textures. */
static void
lose_renderer(GowlCompositor *compositor, void (*then)(gpointer),
              gpointer data)
{
	Gone renderer_gone;
	Gone allocator_gone;

	watch(&renderer_gone, &compositor->renderer->events.destroy,
	      then, data);
	watch(&allocator_gone, &compositor->allocator->events.destroy,
	      NULL, NULL);

	wl_signal_emit_mutable(&compositor->renderer->events.lost, NULL);
	g_assert_false(renderer_gone.gone);
	g_assert_nonnull(compositor->gpu_reset_idle);

	wl_event_loop_dispatch_idle(compositor->event_loop);
	g_assert_true(renderer_gone.gone);
	g_assert_true(allocator_gone.gone);
	g_assert_null(compositor->gpu_reset_idle);
	g_assert_nonnull(compositor->renderer);
	g_assert_nonnull(compositor->allocator);
}

/* Everything that should point at the new renderer does. */
static void
assert_swapped(GowlCompositor *compositor)
{
	struct wlr_scene_output *scene_output;
	GList                   *l;
	guint                    outputs;

	/* The next loss is heard, on the new renderer. */
	g_assert_false(wl_list_empty(
		&compositor->renderer->events.lost.listener_list));

	/* Every monitor draws with the new pair... */
	g_assert_nonnull(compositor->monitors);
	for (l = compositor->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;

		g_assert_true(m->wlr_output->renderer == compositor->renderer);
		g_assert_true(m->wlr_output->allocator == compositor->allocator);
	}

	/* ...and so does every other output the scene renders to: a window
	 * capture's private one would otherwise render its next frame with
	 * the renderer that was just destroyed. */
	outputs = 0;
	wl_list_for_each(scene_output, &compositor->scene->outputs, link) {
		g_assert_true(scene_output->output->renderer
		              == compositor->renderer);
		g_assert_true(scene_output->output->allocator
		              == compositor->allocator);
		outputs++;
	}
	g_assert_cmpuint(outputs, >, g_list_length(compositor->monitors));
}

/* The renderer the first visible cursor image was made with, if any. */
static struct wlr_renderer *
cursor_renderer(GowlCompositor *compositor)
{
	GList *l;

	for (l = compositor->monitors; l != NULL; l = l->next) {
		GowlMonitor              *m = (GowlMonitor *)l->data;
		struct wlr_output_cursor *cursor;

		wl_list_for_each(cursor, &m->wlr_output->cursors, link) {
			if (cursor->enabled && cursor->texture != NULL)
				return cursor->texture->renderer;
		}
	}
	return NULL;
}

static void
count(GowlCompositor *compositor, gpointer data)
{
	(void)compositor;
	(*(guint *)data)++;
}

/* The first child: the swap itself. */
static void
lose_and_recover(void)
{
	static const guint8 frame[4 * 4 * 4];
	struct wlr_ext_image_capture_source_v1 *capture;
	struct wlr_scene_tree                  *captured;
	struct wlr_renderer                    *cursor_before;
	GowlCompositor                         *compositor;
	GowlModuleManager                      *modules;
	GowlConfig                             *config;
	const gchar                            *first;
	const gchar                            *second;
	gchar                                  *runtime;
	guint                                   replaced;

	runtime = isolate(2);
	config = gowl_config_new();
	modules = gowl_module_manager_new();
	compositor = start(config, modules);
	g_assert_cmpuint(g_list_length(compositor->monitors), ==, 2);
	replaced = 0;
	g_signal_connect(compositor, "renderer-replaced", G_CALLBACK(count),
	                 &replaced);

	/* A window capture in progress: wlroots gives it a private output,
	 * bound to the renderer of the moment. */
	captured = wlr_scene_tree_create(&compositor->scene->tree);
	capture = wlr_ext_image_capture_source_v1_create_with_scene_node(
		&captured->node, compositor->event_loop,
		compositor->allocator, compositor->renderer);
	g_assert_nonnull(capture);

	cursor_before = cursor_renderer(compositor);
	lose_renderer(compositor, NULL, NULL);
	assert_swapped(compositor);
	g_assert_cmpuint(replaced, ==, 1);
	render(compositor);

	/* The cursor image was made with the old renderer; it is made again
	 * with the new one.  A machine without a cursor theme has no image
	 * to check. */
	if (cursor_before != NULL)
		g_assert_true(cursor_renderer(compositor)
		              == compositor->renderer);
	else
		g_test_message("no cursor theme: the cursor image is unchecked");

	/* Again: the handler has to have moved to the renderer it made. */
	lose_renderer(compositor, NULL, NULL);
	assert_swapped(compositor);
	g_assert_cmpuint(replaced, ==, 2);
	render(compositor);

	/* Locked, behind an animated backdrop on both monitors: the solid
	 * backdrop is hidden while the frames show. */
	first = gowl_monitor_get_name(g_list_nth_data(compositor->monitors, 0));
	second = gowl_monitor_get_name(g_list_nth_data(compositor->monitors, 1));
	gowl_compositor_set_locked(compositor, TRUE);
	g_assert_true(compositor->locked_bg->node.enabled);
	gowl_compositor_push_lock_frame(compositor, first, frame, 4, 4, 16);
	gowl_compositor_push_lock_frame(compositor, second, frame, 4, 4, 16);
	g_assert_false(compositor->locked_bg->node.enabled);
	render(compositor);

	/* A reset blanks the frames, so the solid backdrop covers again... */
	lose_renderer(compositor, NULL, NULL);
	assert_swapped(compositor);
	g_assert_cmpuint(replaced, ==, 3);
	g_assert_true(compositor->locked_bg->node.enabled);

	/* ...until every monitor has a new frame, not just the first. */
	gowl_compositor_push_lock_frame(compositor, first, frame, 4, 4, 16);
	g_assert_true(compositor->locked_bg->node.enabled);
	gowl_compositor_push_lock_frame(compositor, second, frame, 4, 4, 16);
	g_assert_false(compositor->locked_bg->node.enabled);
	render(compositor);

	gowl_compositor_clear_lock_frame(compositor, NULL);
	g_assert_true(compositor->locked_bg->node.enabled);
	gowl_compositor_set_locked(compositor, FALSE);
	g_assert_false(compositor->locked_bg->node.enabled);

	wlr_scene_node_destroy(&captured->node);
	g_object_unref(compositor);
	g_object_unref(modules);
	g_object_unref(config);
	g_assert_cmpint(g_rmdir(runtime), ==, 0);
	g_free(runtime);
}

/* Buffer nodes under @node with nothing to show, and buffer nodes in all.
 * The scene's texture is private, so a node is judged by its buffer: at
 * the two moments this is asked -- while the old renderer is destroyed,
 * and after the swap before anything has rendered -- there is no texture
 * left for any node to show, and one without a buffer shows nothing. */
static void
count_blank(struct wlr_scene_node *node, guint *blank, guint *all)
{
	if (!node->enabled)
		return;
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;

		wl_list_for_each(child, &tree->children, link)
			count_blank(child, blank, all);
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		(*all)++;
		if (wlr_scene_buffer_from_node(node)->buffer == NULL)
			(*blank)++;
	}
}

typedef struct {
	GowlCompositor *compositor;
	guint           blank[GOWL_SCENE_LAYER_COUNT];
	guint           all[GOWL_SCENE_LAYER_COUNT];
} Census;

static void
take_census(GowlCompositor *compositor, Census *census)
{
	gint i;

	census->compositor = compositor;
	for (i = 0; i < GOWL_SCENE_LAYER_COUNT; i++) {
		census->blank[i] = 0;
		census->all[i] = 0;
		count_blank(&compositor->layers[i]->node, &census->blank[i],
		            &census->all[i]);
	}
}

static void
census_at_destroy(gpointer data)
{
	Census *census = (Census *)data;

	take_census(census->compositor, census);
}

/* The second child: the modules that draw scene buffers of their own. */
static void
modules_redraw(void)
{
	static const float border[4] = { 0.8f, 0.4f, 0.2f, 1.0f };
	GowlCompositor    *compositor;
	GowlModuleManager *modules;
	GowlConfig        *config;
	GowlModule        *wallpaper;
	GowlModule        *roundcorners;
	GowlClient        *client;
	GHashTable        *settings;
	GList             *l;
	Census             at_destroy;
	Census             after;
	gchar             *runtime;
	gchar             *png;
	gsize              m;
	gint               i;

	runtime = isolate(1);
	png = g_build_filename(runtime, "wallpaper.png", NULL);
	g_assert_true(g_file_set_contents(png, (const gchar *)tiny_png,
	                                  sizeof(tiny_png), NULL));

	/* Loaded, activated and configured the way main() does it. */
	modules = gowl_module_manager_new();
	for (m = 0; m < G_N_ELEMENTS(drawing_modules); m++) {
		GError *error = NULL;
		gchar  *path;

		path = g_strdup_printf("%s/%s.so", GOWL_TEST_MODULE_DIR,
		                       drawing_modules[m]);
		if (!gowl_module_manager_load_module(modules, path, &error))
			g_error("%s", error->message);
		g_free(path);
	}
	gowl_module_manager_activate_all(modules);
	wallpaper = gowl_module_manager_find_module(modules, "wallpaper");
	roundcorners = gowl_module_manager_find_module(modules,
	                                               "roundcorners");
	g_assert_nonnull(wallpaper);
	g_assert_nonnull(roundcorners);
	settings = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(settings, (gpointer)"path", png);
	gowl_module_configure(wallpaper, settings);
	g_hash_table_unref(settings);

	config = gowl_config_new();
	compositor = start(config, modules);
	gowl_module_manager_dispatch_startup(modules, compositor);

	/* A window for a border to go round: roundcorners draws into the
	 * client's scene tree, which is all of a client this needs. */
	client = gowl_client_new();
	client->scene = wlr_scene_tree_create(
		compositor->layers[GOWL_SCENE_LAYER_TILE]);
	wlr_scene_node_set_position(&client->scene->node, 40, 40);
	gowl_client_decorator_render_decoration(
		GOWL_CLIENT_DECORATOR(roundcorners), client, 320, 200, 2,
		border);

	/* Render, so the scene uploads the wallpaper and the border; then
	 * lock, and render again for the prompt that now covers both. */
	render(compositor);
	gowl_module_manager_dispatch_lock(modules, compositor);
	render(compositor);

	/* While the old renderer is destroyed, the scene has dropped its
	 * textures and all three modules' nodes have nothing to show.  That
	 * is what makes the rest mean something: a node still holding its
	 * buffer would survive the reset without anyone redrawing it. */
	at_destroy.compositor = compositor;
	lose_renderer(compositor, census_at_destroy, &at_destroy);
	g_assert_cmpuint(at_destroy.blank[GOWL_SCENE_LAYER_BG], >, 0);
	g_assert_cmpuint(at_destroy.blank[GOWL_SCENE_LAYER_TILE], >, 0);
	g_assert_cmpuint(at_destroy.blank[GOWL_SCENE_LAYER_BLOCK], >, 0);

	/* Once the swap is done every one of them has been drawn again. */
	take_census(compositor, &after);
	for (i = 0; i < GOWL_SCENE_LAYER_COUNT; i++) {
		g_assert_cmpuint(after.blank[i], ==, 0);
		g_assert_cmpuint(after.all[i], >=, at_destroy.all[i]);
	}
	render(compositor);

	/* What the modules drew goes before the compositor does. */
	gowl_module_manager_dispatch_unlock(modules, compositor);
	for (l = compositor->monitors; l != NULL; l = l->next)
		gowl_module_manager_dispatch_wallpaper_output_destroy(modules,
		                                                      l->data);
	gowl_module_manager_dispatch_shutdown(modules, compositor);
	wlr_scene_node_destroy(&client->scene->node);
	client->scene = NULL;
	g_object_unref(client);

	g_object_unref(compositor);
	g_object_unref(modules);
	g_object_unref(config);
	g_assert_cmpint(g_unlink(png), ==, 0);
	g_assert_cmpint(g_rmdir(runtime), ==, 0);
	g_free(png);
	g_free(runtime);
}

static void
test_lost_renderer_is_replaced(void)
{
	if (g_test_subprocess()) {
		lose_and_recover();
		return;
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC,
	                       G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

static void
test_modules_redraw(void)
{
	gsize m;

	if (g_test_subprocess()) {
		modules_redraw();
		return;
	}
	for (m = 0; m < G_N_ELEMENTS(drawing_modules); m++) {
		gchar    *path;
		gboolean  built;

		path = g_strdup_printf("%s/%s.so", GOWL_TEST_MODULE_DIR,
		                       drawing_modules[m]);
		built = g_file_test(path, G_FILE_TEST_EXISTS);
		g_free(path);
		if (!built) {
			g_test_skip("the modules are not built; run `make' first");
			return;
		}
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC,
	                       G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/gpu-reset/lost-renderer-is-replaced",
	                test_lost_renderer_is_replaced);
	g_test_add_func("/gpu-reset/modules-redraw", test_modules_redraw);

	return g_test_run();
}
