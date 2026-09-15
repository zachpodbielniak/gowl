/* test-blur-nodes.c -- the backdrop modules' nodes live and die with the
 * window, and follow it
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Mostly the blur module, which came first.  The last case is about the
 * pair: `blur' and `liquidglass' both hang a backdrop off the same place
 * in the same window's tree, so they are alternatives and
 * `window-backdrop' picks which one draws.  Both being loaded is normal
 * -- that is what makes Super+Shift+" instant -- so what has to hold is
 * that exactly one of them draws at a time, and that the one drawing is
 * the one that was asked for.
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
/* Every backdrop at once, which is how CMacs loads them. */
static const gchar *const both_backdrops[] = { "blur", "liquidglass",
                                               "liquidwater", "liquidrain",
                                               NULL };

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
	/*
	 * `window-backdrop' now picks between this module and the liquid
	 * glass, and its default is the glass -- so a rig that says nothing
	 * gets a blur module that correctly draws no backdrop, and every
	 * assertion below about where the backdrop went would fail for a
	 * reason that has nothing to do with what it is testing.  Ask for the
	 * blur explicitly; /blur-nodes/backdrop-style covers the other half.
	 */
	gowl_config_set_backdrop_style(r->config, GOWL_BACKDROP_BLUR);
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

/*
 * `window-backdrop' takes the blur's backdrop away and gives it back.
 *
 * The two backdrop modules draw into the same place in the same tree, so
 * both drawing means one is hidden behind the other while both still pay
 * for a capture and a blur on every tag switch.  The setting is what
 * stops that, and Super+Shift+" is what moves it -- so a blur module that
 * ignored it would show up as "cycling the backdrop does nothing", with
 * the glass dutifully drawn underneath where nobody can see it.
 *
 * The SHADOW is not part of the choice and must survive: it is this
 * module's alone, it has its own key, and a user who turns the backdrop
 * off has not asked to lose their shadows.
 */
/* Collects the labels `toast-requested' is raised with. */
static void
on_toast(GowlCompositor *comp, GowlMonitor *mon, const gchar *label,
         gpointer data)
{
	GPtrArray *seen = data;

	(void)comp;
	(void)mon;
	g_ptr_array_add(seen, g_strdup(label));
}

static void
test_backdrop_style_picks_the_module(void)
{
	Rig   r;
	Decor with, without, again;
	g_autoptr(GPtrArray) toasts = NULL;
	gulong toast_id;

	if (!modules_built(blur_alone))
		return;
	if (!rig_up(&r, blur_alone)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}

	/*
	 * The change has to SAY so.  A calm water or a subtle glass over a
	 * busy wallpaper looks a great deal like the blur it replaced, and
	 * the key that changes it is one press among four -- so without a
	 * toast the honest reaction to pressing it is "did that do
	 * anything?".  It rides `toast-requested', the same signal the layout
	 * indicator already draws, so it looks like the layout toast.
	 */
	toasts = g_ptr_array_new_with_free_func(g_free);
	toast_id = g_signal_connect(r.compositor, "toast-requested",
	                            G_CALLBACK(on_toast), toasts);

	/* On the compositor's list, because that list is exactly what
	 * gowl_compositor_set_backdrop_style() walks to tell the modules. */
	list_client(&r);
	as_tile(&r);
	with = decor_of(r.c);
	if (with.backdrop == NULL) {
		/* No wallpaper was captured here, so there is nothing to take
		 * away and nothing this case can say. */
		rig_down(&r);
		g_test_skip("no backdrop was built in this rig");
		return;
	}
	g_assert_nonnull(with.shadow);

	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_GLASS);
	without = decor_of(r.c);
	g_assert_null(without.backdrop);
	g_assert_nonnull(without.shadow);

	/* Nothing at all, which is the third stop on the cycle. */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_NONE);
	g_assert_null(decor_of(r.c).backdrop);
	g_assert_nonnull(decor_of(r.c).shadow);

	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_BLUR);
	again = decor_of(r.c);
	g_assert_nonnull(again.backdrop);
	g_assert_nonnull(again.shadow);

	/*
	 * And the order the key actually steps through, which is written out
	 * in the compositor rather than derived from the enum: everything
	 * that DRAWS something comes first, so one press from either shipped
	 * default lands on another look and turning the backdrop off takes
	 * the full way round.  Derived from the enum it would be none, blur,
	 * glass, water, rain, snow, leaves, fizz -- which nobody would notice
	 * was wrong except by pressing the key.
	 *
	 * The five that MOVE lead, grouped by what they are: the three
	 * weathers, then the two that are liquid in a pane.  Written out in
	 * full here rather than looped, because the ORDER is the thing being
	 * asserted and a loop over a copy of the same array would assert
	 * nothing at all.
	 */
	{
		static const GowlBackdropStyle expect[] = {
			GOWL_BACKDROP_SNOW, GOWL_BACKDROP_LEAVES,
			GOWL_BACKDROP_FIZZ, GOWL_BACKDROP_WATER,
			GOWL_BACKDROP_GLASS, GOWL_BACKDROP_BLUR,
			GOWL_BACKDROP_NONE, GOWL_BACKDROP_RAIN
		};
		guint i;

		gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_RAIN);
		for (i = 0; i < G_N_ELEMENTS(expect); i++) {
			gowl_compositor_cycle_backdrop_style(r.compositor, 1);
			g_assert_cmpint(gowl_compositor_get_backdrop_style(r.compositor),
			                ==, expect[i]);
		}
		/* Eight presses is all the way round, which is what the last
		 * entry above says.  And back the other way. */
		gowl_compositor_cycle_backdrop_style(r.compositor, -1);
		g_assert_cmpint(gowl_compositor_get_backdrop_style(r.compositor),
		                ==, GOWL_BACKDROP_NONE);
	}

	/*
	 * The names a person would reach for resolve as well as the nicks do.
	 *
	 * The carbonation's setting is spelled `fizz' because it is also the
	 * prefix on thirty config keys, but the toast says "Carbonation" --
	 * and a setting whose displayed name is not accepted as its own value
	 * is a trap laid for whoever reads the toast and types it.
	 */
	{
		GowlBackdropStyle got;

		g_assert_true(gowl_config_backdrop_style_from_name("carbonation",
		                                                   &got));
		g_assert_cmpint(got, ==, GOWL_BACKDROP_FIZZ);
		g_assert_true(gowl_config_backdrop_style_from_name("fizz", &got));
		g_assert_cmpint(got, ==, GOWL_BACKDROP_FIZZ);
		g_assert_true(gowl_config_backdrop_style_from_name("autumn", &got));
		g_assert_cmpint(got, ==, GOWL_BACKDROP_LEAVES);
		g_assert_true(gowl_config_backdrop_style_from_name("snow", &got));
		g_assert_cmpint(got, ==, GOWL_BACKDROP_SNOW);
		g_assert_false(gowl_config_backdrop_style_from_name("weather", &got));
	}

	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_BLUR);
	again = decor_of(r.c);
	/* Back to where it started, not to a second copy layered on the
	 * first: a module that added rather than restored would grow a node
	 * on every press of the key. */
	g_assert_cmpuint(again.count, ==, with.count);

	/* Every style named itself on the way past, and named itself as a
	 * person would say it rather than as the enum spells it. */
	g_signal_handler_disconnect(r.compositor, toast_id);
	{
		/* Every one of them, because a style added to the cycle and left
		 * out of the switch that labels it toasts "No backdrop" while
		 * quite visibly drawing something. */
		static const gchar *const expect[] = {
			"Liquid glass", "Liquid water", "Liquid rain",
			"Snow", "Falling leaves", "Carbonation",
			"Blur", "No backdrop"
		};
		guint i, j;

		for (j = 0; j < G_N_ELEMENTS(expect); j++) {
			gboolean saw = FALSE;

			for (i = 0; i < toasts->len; i++) {
				if (g_strcmp0(g_ptr_array_index(toasts, i), expect[j]) == 0)
					saw = TRUE;
			}
			if (!saw)
				g_error("switching to %s said nothing on screen", expect[j]);
		}
	}

	rig_down(&r);
}

/*
 * With both modules loaded, exactly one backdrop is drawn and it is the
 * one `window-backdrop' names.
 *
 * The two are told apart by the SIZE of the buffer they point at, which
 * is not a trick -- it is the architectural difference between them.  The
 * blur's backdrop is a crop of one monitor-sized picture shared by every
 * window; the glass is a render of THIS window, at this window's size,
 * because what it draws depends on where the window is.  Anything that
 * made the glass draw a monitor-sized buffer, or the blur a window-sized
 * one, would be a much larger bug than this case looks.
 *
 * Without it, the failure to catch is the quiet one: both modules
 * drawing, one hidden behind the other, each still paying for a capture
 * and a blur on every tag switch, and the key that is supposed to change
 * the look appearing to do nothing.
 */
static void
test_only_one_backdrop_draws(void)
{
	Rig   r;
	Decor d;
	gint  mon_w, win_w;

	if (!modules_built(both_backdrops))
		return;
	if (!rig_up(&r, both_backdrops)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}

	list_client(&r);
	/* rig_up() asks for the blur, because every other case here is about
	 * the blur.  This one starts from the shipped default instead. */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_GLASS);
	as_tile(&r);

	mon_w = r.compositor->selmon->wlr_output->width;
	win_w = r.c->frame.width > 0 ? r.c->frame.width : r.c->geom.width;
	/* The case only means anything while the two sizes differ. */
	g_assert_cmpint(mon_w, !=, win_w);

	d = decor_of(r.c);
	if (d.backdrop == NULL) {
		/* No wallpaper was captured in this rig, so neither module has
		 * anything to draw and there is nothing to tell apart. */
		rig_down(&r);
		g_test_skip("no backdrop was built in this rig");
		return;
	}

	/* Glass by default: one backdrop, and it is the window's own render. */
	g_assert_cmpint(gowl_compositor_get_backdrop_style(r.compositor),
	                ==, GOWL_BACKDROP_GLASS);
	g_assert_cmpint(
		wlr_scene_buffer_from_node(d.backdrop)->buffer->width, ==, win_w);

	/*
	 * Over to the water: still ONE backdrop, and a third distinguishable
	 * size.  The water renders at `water-scale' of the window (half by
	 * default) because a refracting surface hides the difference and it
	 * is a quarter of the pixels -- and unlike the other two it pays that
	 * cost every tick, not once per move.
	 */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_WATER);
	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	g_assert_cmpint(
		wlr_scene_buffer_from_node(d.backdrop)->buffer->width, ==, win_w / 2);

	/* Over to the blur: still one backdrop, now the shared picture. */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_BLUR);
	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	g_assert_cmpint(
		wlr_scene_buffer_from_node(d.backdrop)->buffer->width, ==, mon_w);

	/* None of them: the shadow stays, the backdrop goes. */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_NONE);
	d = decor_of(r.c);
	g_assert_null(d.backdrop);
	g_assert_nonnull(d.shadow);

	/* And back, without a second node having accumulated anywhere. */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_GLASS);
	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	g_assert_cmpint(
		wlr_scene_buffer_from_node(d.backdrop)->buffer->width, ==, win_w);
	g_assert_cmpuint(d.count, ==, 2);

	rig_down(&r);
}

/*
 * An HDR output gets its picture ENCODED, and not merely relabelled.
 *
 * Committing an image description tells the panel to read what follows
 * as PQ, where a code value is an absolute number of candelas rather
 * than a fraction of the display's maximum.  Nothing in wlroots then
 * restates the framebuffer for that meaning unless the renderer can
 * convert colour, which only its Vulkan one does -- so gowl encodes the
 * finished scene itself, through a pair of swapchains of its own.
 *
 * What this asserts is that the path RUNS.  The arithmetic is pinned
 * precisely in tests/test-pq-encode.c; the thing that can rot here is
 * the plumbing quietly falling back -- a format the fx layer will not
 * render into, a buffer it cannot sample -- and every one of those
 * failures is designed to be survivable, which is to say silent.  An
 * uncorrected HDR desktop looks like an HDR desktop.  It is merely far
 * too bright, and it costs a laptop its afternoon.
 *
 * The headless output advertises neither BT.2020 nor PQ, so they are
 * widened here; both are public members of struct wlr_output.
 */
static void
test_an_hdr_output_is_encoded(void)
{
	Rig          r;
	GowlMonitor *m;
	guint        i;

	if (!rig_up(&r, blur_alone)) {
		rig_down(&r);
		g_test_skip("no GLES2 compositor available");
		return;
	}
	if (r.compositor->monitors == NULL) {
		rig_down(&r);
		g_test_skip("no output in this rig");
		return;
	}
	m = (GowlMonitor *)r.compositor->monitors->data;

	/* Nothing allocated while the output is in SDR: the encode is for
	 * HDR alone and must not cost an ordinary session two swapchains. */
	g_assert_null(m->pq_scene);
	g_assert_null(m->pq_out);

	/*
	 * The flag is set directly rather than through
	 * gowl_monitor_set_hdr(), which would be the honest thing if the
	 * headless backend could take an HDR commit.  It cannot -- it
	 * refuses every 10-bit format and the image description with it --
	 * so going through the setter makes this case SKIP on every machine,
	 * which is a test that tells nobody anything.
	 *
	 * Nothing is lost by reaching past it.  Whether a driver accepts a
	 * PQ commit is the backend's business and fails loudly; the encode
	 * path reads this one flag and is the half that fails silently.
	 */
	m->wlr_output->supported_primaries |= WLR_COLOR_NAMED_PRIMARIES_BT2020;
	m->wlr_output->supported_transfer_functions |=
		WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
	m->hdr_enabled = TRUE;

	/* The premise: if this renderer could convert colour, gowl would
	 * rightly leave the job to it and there would be nothing to test. */
	g_assert_false(gowl_monitor_hdr_color_managed(m));

	for (i = 0; i < 40; i++) {
		wlr_output_schedule_frame(m->wlr_output);
		wl_event_loop_dispatch(r.compositor->event_loop, 5);
	}

	/*
	 * Both chains exist, which only happens inside the frame path: the
	 * scene was told to render into one of them and the encode acquired
	 * from the other.  A fallback to the uncorrected picture leaves both
	 * NULL, and leaves nothing else to see.
	 */
	g_assert_nonnull(m->pq_scene);
	g_assert_nonnull(m->pq_out);
	/* And it did not give up on the way: pq_warned is set exactly once,
	 * by an encode that would not run. */
	g_assert_false(m->pq_warned);

	/*
	 * And the switch takes it back out of the picture, which is what it
	 * is for: an HDR output that comes out wrong has two causes that
	 * look identical on the glass, and turning the encode off is how
	 * somebody finds out whether it was this one.
	 */
	gowl_compositor_drop_pq_encode(r.compositor);
	g_assert_null(m->pq_scene);
	gowl_config_set_hdr_encode(r.config, FALSE);
	for (i = 0; i < 20; i++) {
		wlr_output_schedule_frame(m->wlr_output);
		wl_event_loop_dispatch(r.compositor->event_loop, 5);
	}
	g_assert_null(m->pq_scene);
	g_assert_null(m->pq_out);

	m->hdr_enabled = FALSE;
	rig_down(&r);
}

/*
 * A DRAG MUST NOT REDRAW THE BACKDROP ONCE PER MOTION EVENT.
 *
 * client_placed runs once per pointer motion while a window is being
 * interactively moved or resized, and a mouse reports several times
 * faster than a screen refreshes -- so most of those renders are thrown
 * away before anyone sees them.  And `settled' cannot be used to notice:
 * nothing is animating the window, the pointer is moving it directly, so
 * it is TRUE throughout.
 *
 * The render is not even the expensive half.  A resize changes the
 * buffer size on every one of those events, and a changed size throws
 * the render swapchain away and allocates a new one -- several buffers
 * the size of the window, at whatever rate the mouse reports.  That was
 * the reported symptom: a floating window with an animated backdrop
 * behind it went to treacle under Super+right-drag.
 *
 * Asserted by watching WHICH BUFFER the scene node points at.  A render
 * takes a fresh one out of the swapchain, and a resized render allocates
 * a whole new swapchain, so the pointer changing is a render having
 * happened and the pointer standing still is one that did not.
 */
static void
test_a_dragged_window_is_not_redrawn_per_motion(void)
{
	Rig   r;
	Decor d;
	gint  before_w;
	gint  i;

	if (!rig_up(&r, both_backdrops)) {
		rig_down(&r);
		g_test_skip("no GLES2 compositor");
		return;
	}
	/* An ANIMATED backdrop, which is what was reported: those five have
	 * a frame hook that redraws them at the output's rate anyway, so a
	 * client_placed during a drag is pure waste. */
	gowl_compositor_set_backdrop_style(r.compositor, GOWL_BACKDROP_RAIN);

	list_client(&r);
	as_tile(&r);
	settle(&r);
	d = decor_of(r.c);
	if (d.backdrop == NULL) {
		rig_down(&r);
		g_test_skip("no backdrop was built in this rig");
		return;
	}

	/*
	 * Measured by the node's DEST SIZE rather than by which buffer it
	 * points at, and the difference is worth recording because the
	 * obvious version of this test passes either way.  A render takes a
	 * fresh buffer out of the swapchain, so the pointer changing looks
	 * like a render having happened -- but a resized render DESTROYS the
	 * swapchain and allocates a new one, and the allocator hands the
	 * same addresses straight back.  The pointer compared equal whether
	 * or not anything had been drawn.
	 *
	 * The dest size cannot lie: it is set in the same breath as the
	 * render, and a hook that returned early never reaches it.
	 */
	before_w = wlr_scene_buffer_from_node(d.backdrop)->dst_width;

	/*
	 * The control, and the test is worth nothing without it: with no
	 * grab in progress, a resize DOES follow at once.  A version that
	 * only checked the grabbed case would pass just as well on a module
	 * that had stopped drawing backdrops altogether.
	 */
	resize_to(&r, 200, 150, 460, 320);
	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	g_assert_cmpint(wlr_scene_buffer_from_node(d.backdrop)->dst_width,
	                !=, before_w);

	/* Now the pointer has hold of it. */
	r.compositor->cursor_mode = GOWL_CURSOR_MODE_RESIZE;
	r.compositor->grabbed_client = r.c;
	g_assert_true(gowl_compositor_client_is_grabbed(r.compositor, r.c));

	before_w = wlr_scene_buffer_from_node(d.backdrop)->dst_width;
	for (i = 0; i < 8; i++)
		resize_to(&r, 200, 150, 460 + i * 9, 320 + i * 7);

	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	/* Eight motion events and not a single redraw.  Before the fix this
	 * was eight renders and eight swapchain reallocations, which is what
	 * a thousand-hertz mouse turns into treacle. */
	g_assert_cmpint(wlr_scene_buffer_from_node(d.backdrop)->dst_width,
	                ==, before_w);

	/*
	 * AND THE DRAG DOES NOT STARVE IT.  Skipping the motion events is
	 * only half the design: the frame hook has to carry on redrawing the
	 * window at the output's own rate, or the backdrop would sit frozen
	 * at whatever it was when the drag began.  One tick of that output,
	 * still grabbed, and it has caught up.
	 */
	gowl_effects_frame(r.compositor, r.compositor->selmon,
	                   g_get_monotonic_time() + 60 * G_USEC_PER_SEC);
	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	g_assert_cmpint(wlr_scene_buffer_from_node(d.backdrop)->dst_width,
	                !=, before_w);

	/* And letting go goes back to following every placement. */
	r.compositor->cursor_mode = GOWL_CURSOR_MODE_NORMAL;
	r.compositor->grabbed_client = NULL;
	before_w = wlr_scene_buffer_from_node(d.backdrop)->dst_width;
	resize_to(&r, 200, 150, 540, 360);
	d = decor_of(r.c);
	g_assert_nonnull(d.backdrop);
	g_assert_cmpint(wlr_scene_buffer_from_node(d.backdrop)->dst_width,
	                !=, before_w);

	rig_down(&r);
}

/*
 * What counts as a grab, which is pure logic and worth pinning down: the
 * predicate decides whether five modules do any work at all, and each of
 * the three ways it can be wrong is silent.
 */
static void
test_what_counts_as_a_grab(void)
{
	Rig r;

	if (!rig_up(&r, blur_alone)) {
		rig_down(&r);
		g_test_skip("no GLES2 compositor");
		return;
	}
	list_client(&r);

	/* Nothing held. */
	g_assert_false(gowl_compositor_client_is_grabbed(r.compositor, r.c));
	g_assert_false(gowl_compositor_client_is_grabbed(r.compositor, NULL));

	/*
	 * A button held with no drag started is NOT a grab.  Treating it as
	 * one would freeze a backdrop for as long as somebody rested a
	 * finger on the button, which is a much stranger bug than the one
	 * this is fixing.
	 */
	r.compositor->grabbed_client = r.c;
	r.compositor->cursor_mode = GOWL_CURSOR_MODE_PRESSED;
	g_assert_false(gowl_compositor_client_is_grabbed(r.compositor, r.c));

	r.compositor->cursor_mode = GOWL_CURSOR_MODE_MOVE;
	g_assert_true(gowl_compositor_client_is_grabbed(r.compositor, r.c));
	r.compositor->cursor_mode = GOWL_CURSOR_MODE_RESIZE;
	g_assert_true(gowl_compositor_client_is_grabbed(r.compositor, r.c));

	/* And only the window actually being dragged.  Every OTHER window
	 * on the screen must go on animating -- one of them being dragged is
	 * not a reason for the rest of the desktop to stop. */
	g_assert_false(gowl_compositor_client_is_grabbed(r.compositor, NULL));
	r.compositor->grabbed_client = NULL;
	g_assert_false(gowl_compositor_client_is_grabbed(r.compositor, r.c));

	r.compositor->cursor_mode = GOWL_CURSOR_MODE_NORMAL;
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/blur-nodes/only-one-backdrop",
	                test_only_one_backdrop_draws);
	g_test_add_func("/blur-nodes/hdr-is-encoded",
	                test_an_hdr_output_is_encoded);
	g_test_add_func("/blur-nodes/backdrop-style",
	                test_backdrop_style_picks_the_module);
	g_test_add_func("/blur-nodes/follow-the-window",
	                test_nodes_follow_the_window);
	g_test_add_func("/blur-nodes/what-counts-as-a-grab",
	                test_what_counts_as_a_grab);
	g_test_add_func("/blur-nodes/a-dragged-window-is-not-redrawn-per-motion",
	                test_a_dragged_window_is_not_redrawn_per_motion);
	g_test_add_func("/blur-nodes/follow-a-resize",
	                test_nodes_follow_a_resize);
	g_test_add_data_func("/blur-nodes/follow-an-animation",
	                     with_animation, test_nodes_follow_an_animation);
	g_test_add_data_func("/blur-nodes/follow-an-animation/rounded",
	                     with_rounded, test_nodes_follow_an_animation);

	return g_test_run();
}
