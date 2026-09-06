/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Exercise actual scene nodes and animation clocks without a display server. */

#include "core/gowl-animation.h"
#include "core/gowl-core-private.h"
#include "core/gowl-frame-sink.h"
#include "interfaces/gowl-client-decorator.h"

#include <glib/gstdio.h>
#include <unistd.h>

/* A separate decoration node exercises the module path: updating the
 * client's four built-in border rects cannot repair this frame. */
G_DECLARE_FINAL_TYPE(TestDecorator, test_decorator, TEST, DECORATOR, GowlModule)
struct _TestDecorator { GowlModule parent_instance; };

static void
test_decorator_render(GowlClientDecorator *self, gpointer client,
                      gint width, gint height, guint bw, const float *color)
{
	GowlClient *c = client;
	struct wlr_scene_rect *frame = g_object_get_data(G_OBJECT(c), "test-frame");

	if (frame == NULL) {
		frame = wlr_scene_rect_create(c->scene, width, bw, color);
		g_object_set_data(G_OBJECT(c), "test-frame", frame);
	}
	wlr_scene_rect_set_size(frame, width, bw);
	wlr_scene_rect_set_color(frame, color);
}

static void
test_decorator_iface_init(GowlClientDecoratorInterface *iface)
{
	iface->render_decoration = test_decorator_render;
}

G_DEFINE_TYPE_WITH_CODE(TestDecorator, test_decorator, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_CLIENT_DECORATOR, test_decorator_iface_init))

static const gchar *
test_decorator_name(GowlModule *self)
{
	return "test-decorator";
}

static void
test_decorator_init(TestDecorator *self)
{
}

static gboolean
test_decorator_activate(GowlModule *self)
{
	return TRUE;
}

static void
test_decorator_class_init(TestDecoratorClass *klass)
{
	GOWL_MODULE_CLASS(klass)->get_name = test_decorator_name;
	GOWL_MODULE_CLASS(klass)->activate = test_decorator_activate;
}

typedef struct {
	GowlCompositor *compositor;
	GowlClient *client;
	struct wlr_scene *scene;
	struct wlr_scene_buffer *content;
} Fixture;

static struct wlr_scene_buffer *
add_buffer(struct wlr_scene_tree *parent, gint width, gint height)
{
	guint8 pixel[4] = { 80, 120, 200, 255 };
	struct wlr_buffer *buffer = gowl_raw_buffer_create(pixel, 1, 1, 4);
	struct wlr_scene_buffer *node = wlr_scene_buffer_create(parent, buffer);

	wlr_scene_buffer_set_dest_size(node, width, height);
	wlr_buffer_drop(buffer);
	return node;
}

static void
setup(Fixture *f, gconstpointer data)
{
	GowlClient *c;
	gint i;
	const float color[4] = { 0.2f, 0.4f, 0.8f, 1.0f };

	f->scene = wlr_scene_create();
	f->compositor = gowl_compositor_new();
	f->compositor->config = gowl_config_new();
	f->compositor->module_mgr = gowl_module_manager_new();
	f->compositor->layers[GOWL_SCENE_LAYER_FLOAT] =
		wlr_scene_tree_create(&f->scene->tree);
	c = f->client = gowl_client_new();
	c->compositor = f->compositor;
	c->scene = wlr_scene_tree_create(&f->scene->tree);
	c->scene_surface = wlr_scene_tree_create(c->scene);
	c->geom = (struct wlr_box){ 100, 200, 1004, 604 };
	c->bw = 2;
	c->anim_placed = TRUE;
	memcpy(c->border_color, color, sizeof(color));
	for (i = 0; i < 4; i++)
		c->border[i] = wlr_scene_rect_create(c->scene, 1, 1, color);
	f->content = add_buffer(c->scene_surface, 1000, 600);
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	f->compositor->clients = g_list_prepend(NULL, c);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	gowl_animation_close_finish_all(f->compositor);
	gowl_animation_cancel(f->client);
	gowl_animation_open_cancel(f->client);
	wlr_scene_node_destroy(&f->scene->tree.node);
	f->client->scene = NULL;
	f->client->scene_surface = NULL;
	g_object_unref(f->client);
	g_clear_object(&f->compositor->config);
	g_clear_object(&f->compositor->module_mgr);
	g_object_unref(f->compositor);
}

static struct wlr_scene_buffer *
first_buffer(GowlSceneSnapshot *snapshot)
{
	struct wlr_scene_node *node;

	g_assert_nonnull(snapshot);
	node = wl_container_of(snapshot->tree->children.next, node, link);
	return wlr_scene_buffer_from_node(node);
}

static void
load_config(Fixture *f, const gchar *yaml)
{
	gchar *path;
	GError *error = NULL;
	gint fd = g_file_open_tmp("gowl-animation-XXXXXX", &path, &error);

	g_assert_no_error(error);
	close(fd);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	g_assert_no_error(error);
	g_assert_true(gowl_config_load_yaml(f->compositor->config, path, &error));
	g_assert_no_error(error);
	g_unlink(path);
	g_free(path);
}

static void
test_pop_and_opacity(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	struct wlr_scene_buffer *ghost;
	gint64 start;

	gowl_client_set_alpha(c, 0.6f);
	gowl_animation_open_start(f->compositor, c);
	g_assert_true(c->anim_active);
	g_assert_true(c->anim_pop);
	ghost = first_buffer(c->anim_ghost);
	g_assert_false(c->scene_surface->node.enabled);
	g_assert_cmpfloat(ghost->opacity, ==, 0.0f);
	g_assert_cmpfloat(c->border[0]->color[3], ==, 0.0f);
	g_assert_cmpint(c->anim_from.width, <, c->geom.width * 9 / 10);
	g_assert_cmpint(ABS(2 * c->anim_from.x + c->anim_from.width
	                   - 2 * c->geom.x - c->geom.width), <=, 1);
	start = c->anim_start_us;
	gowl_animation_tick(f->compositor, NULL, start + 60000);
	g_assert_cmpfloat(ghost->opacity, >, 0.1f);
	g_assert_cmpfloat(ghost->opacity, <, 0.6f);
	g_assert_cmpint(c->border[0]->width, ==, c->anim_cur.width);
	g_assert_cmpfloat((gdouble)ghost->dst_width / ghost->dst_height, >, 1000.0 / 600.0);
	gowl_animation_tick(f->compositor, NULL, start + 135000);
	g_assert_cmpfloat((gdouble)ghost->dst_width / ghost->dst_height, <, 1000.0 / 600.0);
	gowl_animation_tick(f->compositor, NULL, start + 180000);
	g_assert_cmpfloat_with_epsilon(ghost->opacity, 0.6f, 0.001f);
	g_assert_false(c->anim_opening);
	g_assert_true(c->anim_active);
	/* Entrance overshoot is small and settles at the exact layout rect. */
	g_assert_cmpint(c->anim_cur.width, >, c->geom.width);
	g_assert_cmpint(c->anim_cur.width, <, c->geom.width * 103 / 100);
	gowl_animation_tick(f->compositor, NULL, start + c->anim_dur_us);
	g_assert_false(c->anim_active);
	g_assert_null(c->anim_ghost);
	g_assert_true(c->scene_surface->node.enabled);
	g_assert_cmpfloat_with_epsilon(f->content->opacity, 0.6f, 0.001f);
	g_assert_cmpint(c->scene->node.y, ==, c->geom.y);
	g_assert_false(gowl_animation_tick(f->compositor, NULL, start + 500000));
	g_assert_cmpint(c->scene->node.y, ==, c->geom.y);
}

static void
test_tag_reveal_border(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	struct wlr_scene_rect *frame;
	gint pass, i;

	if (data != NULL) {
		g_assert_true(gowl_module_manager_register(f->compositor->module_mgr,
		              test_decorator_get_type(), NULL));
		gowl_module_manager_activate_all(f->compositor->module_mgr);
	}
	gowl_compositor_apply_frame_geometry(f->compositor, c,
	                                     c->geom.width, c->geom.height);
	frame = data != NULL ? g_object_get_data(G_OBJECT(c), "test-frame")
	                     : c->border[0];
	g_assert_nonnull(frame);

	/* Revisit a stationary window repeatedly without a focus or resize
	 * event to repaint its frame. Both focus colours must survive. */
	for (pass = 0; pass < 2; pass++) {
		gint64 start;

		c->border_color[0] = pass == 0 ? 0.2f : 0.6f;
		wlr_scene_node_set_enabled(&c->scene->node, FALSE);
		gowl_animation_reveal_start(f->compositor, c);
		wlr_scene_node_set_enabled(&c->scene->node, TRUE);
		gowl_compositor_apply_frame_geometry(f->compositor, c,
		                                     c->geom.width, c->geom.height);
		g_assert_false(c->anim_active);
		g_assert_cmpfloat(frame->color[3], ==, 0.0f);
		start = c->anim_open_start_us;
		g_assert_true(gowl_animation_tick(f->compositor, NULL, start + 60000));
		g_assert_cmpfloat(frame->color[3], >, 0.0f);
		g_assert_cmpfloat(frame->color[3], <, 1.0f);
		g_assert_false(gowl_animation_tick(f->compositor, NULL,
		                                   start + c->anim_open_dur_us));
		for (i = 0; i < 4; i++)
			g_assert_cmpfloat(frame->color[i], ==, c->border_color[i]);
	}
	/* Interrupted fades must restore the decoration as well. */
	gowl_animation_reveal_start(f->compositor, c);
	gowl_animation_open_cancel(c);
	g_assert_cmpfloat(frame->color[3], ==, 1.0f);
}

static void
test_jiggle_settle(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	struct wlr_box grab = c->geom, destination = c->geom;
	struct wlr_scene_rect *frame = NULL;
	gint64 start, duration;

	if (data != NULL) {
		g_assert_true(gowl_module_manager_register(f->compositor->module_mgr,
		              test_decorator_get_type(), NULL));
		gowl_module_manager_activate_all(f->compositor->module_mgr);
	}
	grab.x -= 300;
	gowl_animation_settle(f->compositor, c, &grab);
	g_assert_true(c->anim_active);
	g_assert_nonnull(c->anim_ghost);
	g_assert_true(wlr_box_equal(&c->anim_cur, &destination));
	start = c->anim_start_us;
	duration = c->anim_dur_us;
	gowl_animation_tick(f->compositor, NULL, start + duration / 8);
	g_assert_cmpint(c->anim_cur.width, >, destination.width);
	g_assert_cmpint(c->anim_cur.height, <, destination.height);
	if (data != NULL) {
		frame = g_object_get_data(G_OBJECT(c), "test-frame");
		g_assert_cmpint(frame->width, ==, c->anim_cur.width);
		g_assert_cmpfloat(frame->color[3], ==, 1.0f);
	}
	gowl_animation_tick(f->compositor, NULL, start + 3 * duration / 8);
	g_assert_cmpint(c->anim_cur.width, <, destination.width);
	g_assert_cmpint(c->anim_cur.height, >, destination.height);
	/* Jiggle never modifies the pointer's final placement. */
	g_assert_true(wlr_box_equal(&c->geom, &destination));
	g_assert_false(gowl_animation_tick(f->compositor, NULL, start + duration));
	g_assert_true(wlr_box_equal(&c->anim_cur, &destination));
	g_assert_cmpint(c->scene->node.x, ==, destination.x);
	g_assert_null(c->anim_ghost);
	g_assert_true(c->scene_surface->node.enabled);
	if (frame != NULL)
		g_assert_cmpint(frame->width, ==, destination.width);
	/* A click without movement and excluded clients do not wobble. */
	gowl_animation_settle(f->compositor, c, &destination);
	g_assert_false(c->anim_active);
	c->isfullscreen = TRUE;
	gowl_animation_settle(f->compositor, c, &grab);
	g_assert_false(c->anim_active);
	c->isfullscreen = FALSE;
	c->isembedded = TRUE;
	gowl_animation_settle(f->compositor, c, &grab);
	g_assert_false(c->anim_active);
}

static void
test_jiggle_move_resize(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	struct wlr_box from = c->geom;
	struct wlr_xdg_surface xdg = { .geometry = { 0, 0, 1000, 600 } };
	struct wlr_xdg_toplevel toplevel = { .base = &xdg };
	gint64 start, duration;
	gint dw = data != NULL ? 240 : 0;

	load_config(f, "animation-curve: linear\n");
	/* The client still has its old buffer when the new size is requested. */
	c->xdg_toplevel = &toplevel;
	c->geom.x += 300;
	c->geom.width += dw;
	gowl_animation_start(f->compositor, c, &from, &c->geom);
	g_assert_nonnull(c->anim_ghost);
	start = c->anim_start_us;
	duration = c->anim_dur_us;
	gowl_animation_tick(f->compositor, NULL, start + duration / 8);
	g_assert_cmpint(c->anim_cur.width, >, from.width + dw / 8);
	g_assert_cmpint(c->border[0]->width, ==, c->anim_cur.width);
	g_assert_cmpint(first_buffer(c->anim_ghost)->dst_width, ==,
	                c->anim_cur.width - 2 * (gint)c->bw);
	gowl_animation_tick(f->compositor, NULL, start + 3 * duration / 8);
	g_assert_cmpint(c->anim_cur.width, <, from.width + 3 * dw / 8);
	g_assert_false(gowl_animation_tick(f->compositor, NULL, start + duration));
	g_assert_true(wlr_box_equal(&c->anim_cur, &c->geom));
	c->xdg_toplevel = NULL;
}

static void
test_jiggle_disabled(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	struct wlr_box from = c->geom;

	load_config(f, "animation-jiggle-strength: 0.4\n");
	load_config(f, "animation-jiggle-strength: 3\n");
	g_assert_cmpfloat(gowl_config_get_animation_jiggle_strength(f->compositor->config), ==, 0.4);
	load_config(f, "animation-jiggle-strength: 0\n");
	c->geom.x += 300;
	gowl_animation_settle(f->compositor, c, &from);
	g_assert_false(c->anim_active);
	gowl_animation_start(f->compositor, c, &from, &c->geom);
	g_assert_true(c->anim_active); /* The ordinary move still animates. */
	g_assert_null(c->anim_ghost);  /* It keeps live content at zero strength. */
	gowl_animation_tick(f->compositor, NULL, c->anim_start_us + c->anim_dur_us / 8);
	g_assert_cmpint(c->anim_cur.width, ==, c->geom.width);
	g_assert_cmpint(c->anim_cur.height, ==, c->geom.height);
}

static void
test_same_target_and_retarget(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	struct wlr_box from, target;
	gint64 start;

	gowl_animation_open_start(f->compositor, c);
	start = c->anim_start_us;
	gowl_animation_tick(f->compositor, NULL, start + 50000);
	from = c->anim_cur;
	gowl_animation_start(f->compositor, c, &from, &c->geom);
	g_assert_cmpint(c->anim_start_us, ==, start);
	g_assert_true(c->anim_pop);
	/* Retarget even if the caller's old layout rect equals its new one. */
	target = c->geom;
	target.x += 200;
	gowl_animation_start(f->compositor, c, &target, &target);
	g_assert_true(wlr_box_equal(&c->anim_from, &from));
	g_assert_true(wlr_box_equal(&c->anim_to, &target));
	g_assert_false(c->anim_pop);
}

static void
test_snapshot_subsurfaces(Fixture *f, gconstpointer data)
{
	struct wlr_scene_tree *sub = wlr_scene_tree_create(f->client->scene_surface);
	struct wlr_scene_buffer *source, *copy;
	struct wlr_scene_node *node;
	struct wlr_fbox crop = { 0.1, 0.2, 0.6, 0.5 };
	GowlSceneSnapshot *snapshot;

	wlr_scene_node_set_position(&sub->node, 200, 100);
	source = add_buffer(sub, 300, 200);
	wlr_scene_node_set_position(&source->node, 10, 20);
	wlr_scene_buffer_set_source_box(source, &crop);
	wlr_scene_buffer_set_transform(source, WL_OUTPUT_TRANSFORM_90);
	snapshot = gowl_scene_snapshot_new(&f->scene->tree,
	                                    f->client->scene_surface, 1000, 600);
	g_assert_nonnull(snapshot);
	g_assert_cmpuint(snapshot->parts->len, ==, 2);
	node = wl_container_of(snapshot->tree->children.prev, node, link);
	copy = wlr_scene_buffer_from_node(node);
	g_assert_cmpfloat(copy->src_box.x, ==, crop.x);
	g_assert_cmpfloat(copy->src_box.width, ==, crop.width);
	g_assert_cmpint(copy->transform, ==, WL_OUTPUT_TRANSFORM_90);
	gowl_scene_snapshot_resize(snapshot, 500, 300);
	g_assert_cmpint(node->x, ==, 105);
	g_assert_cmpint(node->y, ==, 60);
	g_assert_cmpint(copy->dst_width, ==, 150);
	g_assert_cmpint(copy->dst_height, ==, 100);
	/* Destroying the source cannot invalidate the held copy. */
	wlr_scene_node_destroy(&sub->node);
	g_assert_nonnull(copy->buffer);
	gowl_scene_snapshot_free(snapshot);
}

static void
test_close_during_pop(Fixture *f, gconstpointer data)
{
	GowlClient *c = f->client;
	GowlSceneSnapshot *snapshot;
	struct wlr_scene_buffer *ghost;
	gfloat alpha;
	gint width, height;

	gowl_animation_open_start(f->compositor, c);
	gowl_animation_tick(f->compositor, NULL, c->anim_start_us + 60000);
	snapshot = c->anim_ghost;
	ghost = first_buffer(snapshot);
	alpha = ghost->opacity;
	width = ghost->dst_width;
	height = ghost->dst_height;
	gowl_animation_close_start(f->compositor, c);
	g_assert_nonnull(f->compositor->close_anims);
	g_assert_null(c->anim_ghost);
	g_assert_cmpfloat(ghost->opacity, ==, alpha);
	g_assert_cmpint(ghost->dst_width, ==, width);
	g_assert_null(wlr_scene_node_at(&snapshot->tree->node,
		c->scene->node.x + c->bw + 10, c->scene->node.y + c->bw + 10,
		NULL, NULL));
	gowl_animation_cancel(c);
	gowl_animation_open_cancel(c);
	gowl_animation_tick(f->compositor, NULL, g_get_monotonic_time() + 50000);
	g_assert_cmpfloat(ghost->opacity, >, alpha * 0.4f);
	g_assert_cmpfloat(ghost->opacity, <, alpha);
	g_assert_cmpint(ghost->dst_width, <, width);
	/* Closing squeezes vertically as it shrinks, rather than only zooming. */
	g_assert_cmpfloat((gdouble)ghost->dst_width / ghost->dst_height, >,
	                  (gdouble)width / height);
	gowl_animation_tick(f->compositor, NULL, g_get_monotonic_time() + 200000);
	g_assert_null(f->compositor->close_anims);
}

static void
test_disabled_and_hidden(Fixture *f, gconstpointer data)
{
	wlr_scene_node_set_enabled(&f->client->scene->node, FALSE);
	gowl_animation_close_start(f->compositor, f->client);
	g_assert_null(f->compositor->close_anims);
	wlr_scene_node_set_enabled(&f->client->scene->node, TRUE);
	load_config(f, "animations: false\n");
	gowl_animation_open_start(f->compositor, f->client);
	g_assert_false(f->client->anim_opening);
	g_assert_false(f->client->anim_active);
	g_assert_true(f->client->scene_surface->node.enabled);
	load_config(f, "animations: true\nanimation-duration-open: 0\n");
	gowl_animation_open_start(f->compositor, f->client);
	g_assert_false(f->client->anim_active);
	g_assert_false(f->client->anim_opening);
}

static void
test_config_and_embedded(Fixture *f, gconstpointer data)
{
	load_config(f, "animation-popin-scale: 0.87\nanimation-curve-open: ease-out-quint\n"
	               "animation-duration-open: 410\n");
	gowl_animation_open_start(f->compositor, f->client);
	g_assert_cmpint(f->client->anim_dur_us, ==, 410000);
	g_assert_cmpint(f->client->anim_from.width, ==, 873);
	g_assert_cmpstr(gowl_config_get_animation_curve_open(f->compositor->config),
	                ==, "ease-out-quint");
	load_config(f, "animation-popin-scale: -5\n");
	g_assert_cmpfloat(gowl_config_get_animation_popin_scale(f->compositor->config),
	                  ==, 0.87);
	gowl_animation_cancel(f->client);
	gowl_animation_open_cancel(f->client);
	f->client->isembedded = TRUE;
	gowl_animation_open_start(f->compositor, f->client);
	g_assert_false(f->client->anim_active);
	g_assert_false(f->client->anim_opening);
}

static void
test_input_through_snapshot(Fixture *f, gconstpointer data)
{
	/* wlroots hit-testing only needs the geometry, input region and
	 * surface lists. Use an empty XDG tree with a CSD geometry offset. */
	struct wlr_surface surface = { 0 };
	struct wlr_xdg_surface xdg = { 0 };
	struct wlr_xdg_toplevel toplevel = { 0 };
	GowlClient *c = f->client;
	gdouble x, y, sx, sy;

	gowl_animation_open_start(f->compositor, c);
	surface.current.width = 1020;
	surface.current.height = 640;
	surface.mapped = true;
	wl_list_init(&surface.current.subsurfaces_below);
	wl_list_init(&surface.current.subsurfaces_above);
	pixman_region32_init_rect(&surface.input_region, 0, 0, 1020, 640);
	wl_list_init(&xdg.popups);
	xdg.surface = &surface;
	xdg.geometry = (struct wlr_box){ 10, 20, 1000, 600 };
	toplevel.base = &xdg;
	c->xdg_toplevel = &toplevel;
	x = c->scene->node.x + c->bw + (c->anim_cur.width - 2 * c->bw) / 2.0;
	y = c->scene->node.y + c->bw + (c->anim_cur.height - 2 * c->bw) / 2.0;
	g_assert_true(gowl_animation_surface_at(c, x, y, &sx, &sy) == &surface);
	g_assert_cmpfloat_with_epsilon(sx, 510.0, 0.001);
	g_assert_cmpfloat_with_epsilon(sy, 320.0, 0.001);
	/* An input-transparent part must still pass through the snapshot. */
	pixman_region32_clear(&surface.input_region);
	g_assert_null(gowl_animation_surface_at(c, x, y, &sx, &sy));
	c->xdg_toplevel = NULL;
	pixman_region32_fini(&surface.input_region);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/animation/scene/pop-opacity", Fixture, NULL, setup,
	           test_pop_and_opacity, teardown);
	g_test_add("/animation/scene/tag-reveal-border", Fixture, NULL, setup,
	           test_tag_reveal_border, teardown);
	g_test_add("/animation/scene/tag-reveal-decoration", Fixture, GINT_TO_POINTER(1), setup,
	           test_tag_reveal_border, teardown);
	g_test_add("/animation/scene/jiggle-settle", Fixture, NULL, setup,
	           test_jiggle_settle, teardown);
	g_test_add("/animation/scene/jiggle-settle-decoration", Fixture, GINT_TO_POINTER(1), setup,
	           test_jiggle_settle, teardown);
	g_test_add("/animation/scene/jiggle-move", Fixture, NULL, setup,
	           test_jiggle_move_resize, teardown);
	g_test_add("/animation/scene/jiggle-resize", Fixture, GINT_TO_POINTER(1), setup,
	           test_jiggle_move_resize, teardown);
	g_test_add("/animation/scene/jiggle-disabled", Fixture, NULL, setup,
	           test_jiggle_disabled, teardown);
	g_test_add("/animation/scene/retarget", Fixture, NULL, setup,
	           test_same_target_and_retarget, teardown);
	g_test_add("/animation/scene/subsurfaces", Fixture, NULL, setup,
	           test_snapshot_subsurfaces, teardown);
	g_test_add("/animation/scene/close-mid-pop", Fixture, NULL, setup,
	           test_close_during_pop, teardown);
	g_test_add("/animation/scene/disabled-hidden", Fixture, NULL, setup,
	           test_disabled_and_hidden, teardown);
	g_test_add("/animation/scene/config-embedded", Fixture, NULL, setup,
	           test_config_and_embedded, teardown);
	g_test_add("/animation/scene/input", Fixture, NULL, setup,
	           test_input_through_snapshot, teardown);
	return g_test_run();
}
