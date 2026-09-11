/* test-blur-nodes.c -- the blur module's nodes live and die with the
 * window, and follow it
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
 * The nodes must also follow the window.  GEOMETRY is a consumable hook,
 * which stops at the first provider to claim it, and the animation module
 * claims it for every tile -- so the blur module never heard that a tile
 * had been resized.  Its shadow kept the old size and its backdrop the old
 * crop until something else happened to wake it, and a window that shrank
 * drew them across its neighbour.  The resize cases load animation.so
 * beside it and hold both nodes to the frame the window is drawn at:
 * after a placement the animation makes at once, on every step of one it
 * animates, and where that one lands.  Once more with the rounded borders
 * loaded, which draw the frame by another path through the compositor.
 *
 * Runs the real modules in a headless compositor against a window that is
 * only a scene tree, dressed the way the compositor dresses one: four
 * border rects and, where the animation needs something to take a picture
 * of, a stand-in for the client's drawing.  None of the modules looks at a
 * surface.  It draws with GLES2; where there is none, the test is skipped.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-effects.h"
#include "core/gowl-frame-sink.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/* The modules each case loads, blur being the one under test. */
static const gchar *const blur_alone[]     = { "blur", NULL };
static const gchar *const with_animation[] = { "blur", "animation", NULL };
static const gchar *const with_rounded[]   = { "blur", "animation",
                                               "roundcorners", NULL };

typedef struct {
	gchar             *parent;   /* XDG_RUNTIME_DIR before the rig */
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	gboolean           started;
	GowlClient        *c;
	gboolean           listed;   /* in the compositor's client list */
	gboolean           rounded;  /* the rounded borders are loaded */
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

/* A fresh scene tree for the window, where the compositor would put it,
 * with the four border rects every mapped window carries: drawing the
 * frame sizes them. */
static void
new_tree(Rig *r)
{
	static const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	gint               i;

	r->c->scene = wlr_scene_tree_create(
		r->compositor->layers[GOWL_SCENE_LAYER_TILE]);
	wlr_scene_node_set_position(&r->c->scene->node, r->c->geom.x,
	                            r->c->geom.y);
	for (i = 0; i < 4; i++)
		r->c->border[i] = wlr_scene_rect_create(r->c->scene, 0, 0, grey);
	r->c->scene_surface = NULL;
}

/*
 * Something for the animation to take a picture of.  A window it cannot
 * photograph is placed at once rather than stretched, so the animated
 * cases give the tree a stand-in for the client's drawing: one small
 * buffer scaled over the content area, in a subtree where the client's
 * own surface would be.
 */
static void
add_drawing(Rig *r)
{
	guint8                   pixels[4 * 4 * 4];
	struct wlr_buffer       *buffer;
	struct wlr_scene_buffer *drawing;

	memset(pixels, 0xff, sizeof(pixels));
	buffer = gowl_raw_buffer_create(pixels, 4, 4, 4 * 4);
	g_assert_nonnull(buffer);
	r->c->scene_surface = wlr_scene_tree_create(r->c->scene);
	wlr_scene_node_set_position(&r->c->scene_surface->node, r->c->bw,
	                            r->c->bw);
	drawing = wlr_scene_buffer_create(r->c->scene_surface, buffer);
	wlr_buffer_drop(buffer);
	g_assert_nonnull(drawing);
	wlr_scene_buffer_set_dest_size(drawing,
		r->c->geom.width - 2 * (gint)r->c->bw,
		r->c->geom.height - 2 * (gint)r->c->bw);
}

/* The animation ticks the windows the compositor lists. */
static void
list_client(Rig *r)
{
	r->compositor->clients = g_list_prepend(r->compositor->clients, r->c);
	r->listed = TRUE;
}

static void
send(
	Rig                  *r,
	GowlSceneEffectEvent  event
){
	gowl_effects_client_event(r->compositor, r->c, event, NULL, FALSE);
}

/*
 * What resize_client() does with a new geometry, as far as the effects
 * can tell: offer it as GEOMETRY and, when no provider claims it, put the
 * window there itself.  Claimed or not, the frame is then drawn by
 * gowl_compositor_apply_frame_geometry() -- the claimant's call or the
 * compositor's -- and that is where every provider hears of it.
 */
static void
place(
	Rig                  *r,
	const struct wlr_box *previous
){
	if (gowl_effects_client_event(r->compositor, r->c,
	                              GOWL_SCENE_EFFECT_GEOMETRY, previous,
	                              FALSE))
		return;
	wlr_scene_node_set_position(&r->c->scene->node, r->c->geom.x,
	                            r->c->geom.y);
	gowl_compositor_apply_frame_geometry(r->compositor, r->c,
	                                     r->c->geom.width,
	                                     r->c->geom.height);
}

/* Placed as an ordinary tile, which is decorated. */
static void
as_tile(Rig *r)
{
	r->c->isoverlay = FALSE;
	r->c->overlay_visible = FALSE;
	r->c->overlay_group = 0;
	place(r, &r->c->geom);
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
	place(r, &r->c->geom);
}

/* And giving it back: DESTROY again, then placed as a tile. */
static void
out_of_panel(Rig *r)
{
	send(r, GOWL_SCENE_EFFECT_DESTROY);
	as_tile(r);
}

/* Re-tiled to @x,@y at @width x @height, the way a layout change does. */
static void
resize_to(
	Rig  *r,
	gint  x,
	gint  y,
	gint  width,
	gint  height
){
	struct wlr_box previous;

	previous = r->c->geom;
	r->c->geom.x = x;
	r->c->geom.y = y;
	r->c->geom.width = width;
	r->c->geom.height = height;
	place(r, &previous);
}

/* A frame drawn at @now: every provider's tick, the animation's too. */
static void
frame_at(
	Rig    *r,
	gint64  now
){
	gowl_effects_frame(r->compositor, NULL, now);
}

/* A frame long after anything could have started, so all of it is over
 * and the window has landed. */
static void
settle(Rig *r)
{
	frame_at(r, g_get_monotonic_time() + 60 * G_USEC_PER_SEC);
	g_assert_false(gowl_effects_has_geometry(r->c));
}

/*
 * The decorations: the buffers hung directly off the window's tree.  The
 * client's drawing, and the animation's picture of it, sit in subtrees and
 * the borders are rects, so every buffer child is one -- the blur module's
 * backdrop is the one cropped out of its wallpaper, a rounded frame the
 * one that lets the pointer through, and the shadow the other.
 */
typedef struct {
	struct wlr_scene_node *shadow;
	struct wlr_scene_node *backdrop;
	struct wlr_scene_node *rounded;
	guint                  count;
} Decor;

static Decor
decor_of(GowlClient *c)
{
	Decor                  d;
	struct wlr_scene_node *child;

	memset(&d, 0, sizeof(d));
	wl_list_for_each(child, &c->scene->children, link) {
		struct wlr_scene_buffer *buffer;

		if (child->type != WLR_SCENE_NODE_BUFFER)
			continue;
		buffer = wlr_scene_buffer_from_node(child);
		d.count++;
		if (buffer->point_accepts_input != NULL)
			d.rounded = child;
		else if (!wlr_fbox_empty(&buffer->src_box))
			d.backdrop = child;
		else
			d.shadow = child;
	}
	return d;
}

/* Whether @node itself draws layout pixel @x,@y -- asked the way the
 * pointer asks, which knows every node's size, a texture-only buffer's
 * included. */
static gboolean
covers(
	struct wlr_scene_node *node,
	gint                   x,
	gint                   y
){
	gdouble nx;
	gdouble ny;

	return wlr_scene_node_at(node, x + 0.5, y + 0.5, &nx, &ny) != NULL;
}

/* The rectangle @node draws, walked out from pixel @x,@y along each axis
 * to the first pixel it does not draw; empty when it does not draw @x,@y
 * at all. */
static struct wlr_box
measure(
	struct wlr_scene_node *node,
	gint                   x,
	gint                   y
){
	struct wlr_box box;
	gint           left;
	gint           right;
	gint           top;
	gint           bottom;

	memset(&box, 0, sizeof(box));
	box.x = x;
	box.y = y;
	if (!covers(node, x, y))
		return box;

	for (left = x; left > x - 10000 && covers(node, left - 1, y); left--)
		;
	for (right = x + 1; right < x + 10000 && covers(node, right, y);
	     right++)
		;
	for (top = y; top > y - 10000 && covers(node, x, top - 1); top--)
		;
	for (bottom = y + 1; bottom < y + 10000 && covers(node, x, bottom);
	     bottom++)
		;
	box.x = left;
	box.y = top;
	box.width = right - left;
	box.height = bottom - top;
	return box;
}

/* That @got, the window's @what, is @want -- compared as text, so a
 * failure says where the node is as well as that it is wrong. */
static void
assert_box(
	const gchar          *when,
	const gchar          *what,
	const struct wlr_box *got,
	const struct wlr_box *want
){
	gchar *got_text;
	gchar *want_text;

	got_text = g_strdup_printf("%s: %s at %d,%d %dx%d", when, what,
	                           got->x, got->y, got->width, got->height);
	want_text = g_strdup_printf("%s: %s at %d,%d %dx%d", when, what,
	                            want->x, want->y, want->width,
	                            want->height);
	g_assert_cmpstr(got_text, ==, want_text);
	g_free(got_text);
	g_free(want_text);
}

/* Where the rounded frame is.  It turns the pointer away, so it cannot be
 * found by asking; nothing is ever rendered here, so the scene still holds
 * the buffer it was given, and that is its size. */
static struct wlr_box
rounded_box(struct wlr_scene_node *node)
{
	struct wlr_scene_buffer *buffer;
	struct wlr_box           box;

	buffer = wlr_scene_buffer_from_node(node);
	g_assert_nonnull(buffer->buffer);
	memset(&box, 0, sizeof(box));
	wlr_scene_node_coords(node, &box.x, &box.y);
	box.width = buffer->dst_width > 0 ? buffer->dst_width
	                                  : buffer->buffer->width;
	box.height = buffer->dst_height > 0 ? buffer->dst_height
	                                    : buffer->buffer->height;
	return box;
}

/*
 * The picture the shadow shows: the size it was drawn for, not the size it
 * is shown at.  Nothing is rendered here, so the scene still holds the
 * buffer it was drawn into.  Compared by size, never by pointer: a picture
 * drawn again is freed and allocated anew, and malloc hands the new node
 * and buffer the old ones' addresses as often as not.
 */
static struct wlr_box
shadow_picture(GowlClient *c)
{
	struct wlr_scene_node   *node;
	struct wlr_scene_buffer *shadow;
	struct wlr_box           box;

	node = decor_of(c).shadow;
	g_assert_nonnull(node);
	shadow = wlr_scene_buffer_from_node(node);
	g_assert_nonnull(shadow->buffer);
	memset(&box, 0, sizeof(box));
	box.width = shadow->buffer->width;
	box.height = shadow->buffer->height;
	return box;
}

/*
 * The decorations are where the window is DRAWN -- c->frame, which the
 * animation moves a step at a time -- and not where the layout means it
 * to end up.  The backdrop is exactly behind the frame, the shadow around
 * it by its padding and offset, a rounded frame the frame's own size, and
 * nothing else hangs off the window.
 */
static void
assert_follows(
	Rig         *r,
	const gchar *when
){
	Decor          d;
	struct wlr_box frame;
	struct wlr_box want;
	struct wlr_box got;
	gint           pad;

	d = decor_of(r->c);
	frame = r->c->frame;
	g_assert_cmpint(frame.width, >, 0);
	g_assert_cmpint(frame.height, >, 0);

	if (d.backdrop == NULL)
		g_error("%s: the window has no backdrop", when);
	got = measure(d.backdrop, frame.x + frame.width / 2,
	              frame.y + frame.height / 2);
	assert_box(when, "backdrop", &got, &frame);

	pad = 2 * gowl_config_get_shadow_radius(r->config);
	want.x = frame.x - pad + gowl_config_get_shadow_offset_x(r->config);
	want.y = frame.y - pad + gowl_config_get_shadow_offset_y(r->config);
	want.width = frame.width + 2 * pad;
	want.height = frame.height + 2 * pad;
	if (d.shadow == NULL)
		g_error("%s: the window has no shadow", when);
	got = measure(d.shadow, want.x + want.width / 2,
	              want.y + want.height / 2);
	assert_box(when, "shadow", &got, &want);

	if (r->rounded) {
		if (d.rounded == NULL)
			g_error("%s: the window has no rounded frame", when);
		got = rounded_box(d.rounded);
		assert_box(when, "rounded frame", &got, &frame);
	}
	g_assert_cmpuint(d.count, ==, r->rounded ? 3 : 2);

	/* And a window at rest has a shadow drawn for its own size, not one
	 * stretched from a picture of another: a stretched shadow has the
	 * wrong corners. */
	if (!gowl_effects_has_geometry(r->c)) {
		got = shadow_picture(r->c);
		memset(&want, 0, sizeof(want));
		want.width = frame.width + 2 * pad;
		want.height = frame.height + 2 * pad;
		assert_box(when, "shadow picture", &got, &want);
	}
}

/* Whether every module in @names has been built; skips the case when one
 * has not. */
static gboolean
modules_built(const gchar *const *names)
{
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		gchar    *file;
		gchar    *path;
		gboolean  built;

		file = g_strconcat(names[i], ".so", NULL);
		path = g_build_filename(GOWL_TEST_MODULE_DIR, file, NULL);
		built = g_file_test(path, G_FILE_TEST_EXISTS);
		g_free(path);
		g_free(file);
		if (!built) {
			g_test_skip("a module this needs is not built; run `make' first");
			return FALSE;
		}
	}
	return TRUE;
}

/* A headless compositor drawing with GLES2, the modules in @names loaded,
 * and a translucent window.  FALSE when there is no GLES2 to draw with. */
static gboolean
rig_up(
	Rig                *r,
	const gchar *const *names
){
	const gchar *parent;
	GError      *error;
	guint        i;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in: no systemd user
	 * targets, and a runtime directory of its own inside the real one,
	 * which keeps a socket path under the 108 bytes it gets.  The real one
	 * is put back afterwards, so every case nests in the same place. */
	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
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
	for (i = 0; names[i] != NULL; i++) {
		gchar *file;
		gchar *path;

		file = g_strconcat(names[i], ".so", NULL);
		path = g_build_filename(GOWL_TEST_MODULE_DIR, file, NULL);
		if (!gowl_module_manager_load_module(r->modules, path, &error))
			g_error("could not load %s: %s", path, error->message);
		g_free(path);
		g_free(file);
		if (g_strcmp0(names[i], "roundcorners") == 0)
			r->rounded = TRUE;
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
	gint i;

	if (r->c != NULL)
		send(r, GOWL_SCENE_EFFECT_DESTROY);
	if (r->started)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	if (r->listed)
		r->compositor->clients = g_list_remove(r->compositor->clients,
		                                       r->c);
	if (r->c != NULL) {
		if (r->c->scene != NULL)
			wlr_scene_node_destroy(&r->c->scene->node);
		r->c->scene = NULL;
		r->c->scene_surface = NULL;
		for (i = 0; i < 4; i++)
			r->c->border[i] = NULL;
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
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_free(r->parent);
}

static void
test_nodes_follow_the_window(void)
{
	Rig   r;
	guint n;
	guint trip;

	if (!modules_built(blur_alone))
		return;
	if (!rig_up(&r, blur_alone)) {
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

/*
 * Resized by layouts that the animation module places at once -- this
 * window has nothing to take a picture of, so there is nothing to stretch
 * -- but still claims, so GEOMETRY stops there and the decorations have to
 * hear of the new frame some other way.  Narrower, as Super+h leaves the
 * master column; wider than it ever was; moved and resized, as a neighbour
 * closing re-tiles it; and only moved, which the animation does slide.
 */
static void
test_nodes_follow_a_resize(void)
{
	Rig r;

	if (!modules_built(with_animation))
		return;
	if (!rig_up(&r, with_animation)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}
	list_client(&r);

	as_tile(&r);
	assert_follows(&r, "placed");

	resize_to(&r, 200, 150, 250, 300);
	assert_follows(&r, "narrower");

	resize_to(&r, 200, 150, 520, 300);
	assert_follows(&r, "wider");

	resize_to(&r, 640, 100, 300, 200);
	assert_follows(&r, "moved and resized");

	resize_to(&r, 400, 100, 300, 200);
	g_assert_true(gowl_effects_has_geometry(r.c));
	assert_follows(&r, "sliding");
	settle(&r);
	g_assert_true(wlr_box_equal(&r.c->frame, &r.c->geom));
	assert_follows(&r, "slid");

	rig_down(&r);
}

/* One step of an animation checked: the decorations drawn with the window,
 * which is still moving, and the shadow still the picture it had before
 * the move began -- stretched, not drawn again. */
static void
assert_step(
	Rig                  *r,
	const gchar          *what,
	const gchar          *step,
	const struct wlr_box *picture
){
	struct wlr_box  got;
	gchar          *when;

	when = g_strdup_printf("%s, %s", what, step);
	g_assert_true(gowl_effects_has_geometry(r->c));
	assert_follows(r, when);
	got = shadow_picture(r->c);
	assert_box(when, "shadow picture", &got, picture);
	g_free(when);
}

/*
 * Animated from where the window is to @x,@y at @width x @height, and
 * checked on the first step, twice on the way and where it lands.
 *
 * The decorations are drawn with the window at every step -- neither left
 * at the old frame nor sent ahead to the new one, either of which puts
 * them across the neighbour for as long as the animation runs.  And while
 * it moves the shadow is stretched, never drawn again: drawing it is the
 * one expensive thing the module does, and an animation comes a frame at
 * a time.
 */
static void
animate_to(
	Rig         *r,
	gint         x,
	gint         y,
	gint         width,
	gint         height,
	const gchar *what
){
	struct wlr_box  picture;
	gint64          start;
	gint64          duration;
	gchar          *when;

	picture = shadow_picture(r->c);
	duration = (gint64)gowl_config_get_animation_duration(r->config) * 1000;
	start = g_get_monotonic_time();
	resize_to(r, x, y, width, height);
	assert_step(r, what, "first step", &picture);

	frame_at(r, start + duration / 3);
	assert_step(r, what, "a third of the way", &picture);

	frame_at(r, start + 2 * duration / 3);
	assert_step(r, what, "two thirds of the way", &picture);

	settle(r);
	g_assert_true(wlr_box_equal(&r->c->frame, &r->c->geom));
	when = g_strdup_printf("%s, landed", what);
	assert_follows(r, when);
	g_free(when);
}

/* A window the animation really animates: it pops in, then is resized
 * three ways.  @data names the modules, the rounded borders or not. */
static void
test_nodes_follow_an_animation(gconstpointer data)
{
	const gchar *const *names;
	Rig                 r;

	names = data;
	if (!modules_built(names))
		return;
	if (!rig_up(&r, names)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}
	add_drawing(&r);
	list_client(&r);

	/* Mapped: it pops in, decorated on the way and where it lands. */
	as_tile(&r);
	g_assert_true(gowl_effects_has_geometry(r.c));
	assert_follows(&r, "popping in");
	settle(&r);
	g_assert_true(wlr_box_equal(&r.c->frame, &r.c->geom));
	assert_follows(&r, "mapped");

	animate_to(&r, 200, 150, 250, 300, "narrower");
	animate_to(&r, 200, 150, 520, 300, "wider");
	animate_to(&r, 420, 120, 360, 260, "moved and resized");

	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/blur-nodes/follow-the-window",
	                test_nodes_follow_the_window);
	g_test_add_func("/blur-nodes/follow-a-resize",
	                test_nodes_follow_a_resize);
	g_test_add_data_func("/blur-nodes/follow-an-animation",
	                     with_animation, test_nodes_follow_an_animation);
	g_test_add_data_func("/blur-nodes/follow-an-animation/rounded",
	                     with_rounded, test_nodes_follow_an_animation);

	return g_test_run();
}
