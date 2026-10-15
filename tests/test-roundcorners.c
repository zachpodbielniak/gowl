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
 * The roundcorners decorator against the real .so, on a bare wlr_scene:
 * no compositor, no renderer, just the client scene tree its frame
 * hangs from.
 *
 * The frame is a child of a tree the compositor owns, and the
 * compositor can destroy that tree on a path that does not call
 * destroy_decoration first -- the unmanaged X11 unmap did, and closing
 * SteamVR crashed the session on the decorator's second destroy of the
 * freed frame.  The module now follows the frame node's own destroy
 * signal, so whatever order the compositor does things in, it never
 * touches a node it no longer has.
 */

#include <glib.h>
#include <wlr/types/wlr_scene.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "interfaces/gowl-client-decorator.h"
#include "module/gowl-module-manager.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

typedef struct {
	GowlModuleManager   *modules;
	GowlClientDecorator *decorator;
	struct wlr_scene    *scene;
	GowlClient          *client;
} Fixture;

static const float BORDER[4] = { 0.8f, 0.4f, 0.2f, 1.0f };

static void
fixture_setup(
	Fixture       *f,
	gconstpointer  data
){
	GError *error = NULL;
	gchar  *path;

	(void)data;

	f->modules = gowl_module_manager_new();
	path = g_strdup_printf("%s/roundcorners.so", GOWL_TEST_MODULE_DIR);
	if (!gowl_module_manager_load_module(f->modules, path, &error))
		g_error("%s", error->message);
	g_free(path);
	gowl_module_manager_activate_all(f->modules);
	f->decorator = (GowlClientDecorator *)
		gowl_module_manager_get_decorator(f->modules);
	g_assert_nonnull(f->decorator);

	f->scene = wlr_scene_create();
	f->client = gowl_client_new();
	f->client->scene = wlr_scene_tree_create(&f->scene->tree);
}

static void
fixture_teardown(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;

	gowl_client_decorator_destroy_decoration(f->decorator, f->client);
	if (f->client->scene != NULL)
		wlr_scene_node_destroy(&f->client->scene->node);
	f->client->scene = NULL;
	g_clear_object(&f->client);
	wlr_scene_node_destroy(&f->scene->tree.node);
	g_clear_object(&f->modules);
}

/* The frame: the one scene buffer directly in @tree. */
static struct wlr_scene_buffer *
frame_in(struct wlr_scene_tree *tree)
{
	struct wlr_scene_node *child;

	wl_list_for_each(child, &tree->children, link) {
		if (child->type == WLR_SCENE_NODE_BUFFER)
			return wlr_scene_buffer_from_node(child);
	}
	return NULL;
}

/* A render hangs one frame in the client's tree; destroy removes it and
 * lets go of the client. */
static void
render_then_destroy(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;

	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        200, 100, 2, BORDER);
	g_assert_nonnull(frame_in(f->client->scene));

	gowl_client_decorator_destroy_decoration(f->decorator, f->client);
	g_assert_null(frame_in(f->client->scene));
	/* The decorator's reference is gone: only the fixture's is left. */
	g_assert_cmpuint(G_OBJECT(f->client)->ref_count, ==, 1);
}

/*
 * The client's tree goes first, frame and all, and the decorator is
 * told afterwards -- the order the unmanaged X11 unmap used.  Neither
 * the late destroy_decoration nor a render on the client's next tree may
 * touch the freed frame; the render must build a new one.
 */
static void
the_tree_going_first_leaves_nothing_dangling(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;

	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        200, 100, 2, BORDER);
	g_assert_nonnull(frame_in(f->client->scene));

	/* The compositor tears the tree down without asking. */
	wlr_scene_node_destroy(&f->client->scene->node);
	f->client->scene = wlr_scene_tree_create(&f->scene->tree);

	/* Same size and colour as before: a cache that still believed in
	 * the old frame would skip this and leave the new tree bare. */
	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        200, 100, 2, BORDER);
	g_assert_nonnull(frame_in(f->client->scene));

	/* And once more down the same road, ending in destroy_decoration
	 * after the tree is gone: the SteamVR sequence itself. */
	wlr_scene_node_destroy(&f->client->scene->node);
	f->client->scene = NULL;
	gowl_client_decorator_destroy_decoration(f->decorator, f->client);
	g_assert_cmpuint(G_OBJECT(f->client)->ref_count, ==, 1);
}

/* A borderless render takes an existing frame away. */
static void
a_borderless_render_drops_the_frame(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;

	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        200, 100, 2, BORDER);
	g_assert_nonnull(frame_in(f->client->scene));
	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        200, 100, 0, BORDER);
	g_assert_null(frame_in(f->client->scene));
	g_assert_cmpuint(G_OBJECT(f->client)->ref_count, ==, 1);
}

/*
 * Mid-resize the frame on screen is stretched, not redrawn: a redraw is
 * a window-sized cairo image, a copy and an upload, on every motion
 * event, which is what made dragging a border lag.  Once the resize has
 * ended the same size is drawn for real.
 */
static void
a_resize_stretches_until_it_ends(
	Fixture       *f,
	gconstpointer  data
){
	GowlCompositor          *comp;
	struct wlr_scene_buffer *frame;
	struct wlr_buffer       *before;

	(void)data;
	comp = gowl_compositor_new();
	f->client->compositor = comp;

	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        200, 100, 2, BORDER);
	frame = frame_in(f->client->scene);
	g_assert_nonnull(frame);
	before = frame->buffer;

	comp->grabbed_client = f->client;
	comp->cursor_mode = GOWL_CURSOR_MODE_RESIZE;
	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        300, 150, 2, BORDER);
	g_assert_true(frame_in(f->client->scene) == frame);
	g_assert_true(frame->buffer == before);
	g_assert_cmpint(frame->dst_width, ==, 300);
	g_assert_cmpint(frame->dst_height, ==, 150);

	/* The grab is over: drawn at the size it came to. */
	comp->grabbed_client = NULL;
	comp->cursor_mode = GOWL_CURSOR_MODE_NORMAL;
	gowl_client_decorator_render_decoration(f->decorator, f->client,
	                                        300, 150, 2, BORDER);
	frame = frame_in(f->client->scene);
	g_assert_nonnull(frame);
	g_assert_cmpint(frame->buffer->width, ==, 300);
	g_assert_cmpint(frame->buffer->height, ==, 150);

	f->client->compositor = NULL;
	g_object_unref(comp);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/roundcorners/render-then-destroy", Fixture, NULL,
	           fixture_setup, render_then_destroy, fixture_teardown);
	g_test_add("/roundcorners/the-tree-going-first-leaves-nothing-dangling",
	           Fixture, NULL, fixture_setup,
	           the_tree_going_first_leaves_nothing_dangling,
	           fixture_teardown);
	g_test_add("/roundcorners/a-borderless-render-drops-the-frame",
	           Fixture, NULL, fixture_setup,
	           a_borderless_render_drops_the_frame, fixture_teardown);

	g_test_add("/roundcorners/a-resize-stretches-until-it-ends",
	           Fixture, NULL, fixture_setup,
	           a_resize_stretches_until_it_ends, fixture_teardown);

	return g_test_run();
}
