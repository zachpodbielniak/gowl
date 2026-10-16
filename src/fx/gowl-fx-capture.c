/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Capturing what an output WOULD look like.
 *
 * Every effect that shows the desktop as something other than itself --
 * a cube face, an expo tile, a switcher preview -- needs a picture of a
 * state that is not currently on screen.  The only way to get one is to
 * tell the scene a temporary lie, render it, and take the lie back.
 *
 * The render is never presented, which is what makes the lie safe.  What
 * would NOT be safe is failing to undo it, so the visibility scratchpad
 * below remembers every node it touches and restores in one call that
 * callers can put on every exit path.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include "core/gowl-core-private.h"

#include <string.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

typedef struct {
	struct wlr_scene_node *node;
	gboolean               enabled;
} GowlFxVisSave;

struct _GowlFxVis {
	GArray *saved;
};

GowlFxVis *
gowl_fx_vis_begin(void)
{
	GowlFxVis *vis = g_new0(GowlFxVis, 1);

	vis->saved = g_array_new(FALSE, FALSE, sizeof(GowlFxVisSave));
	return vis;
}

void
gowl_fx_vis_set(GowlFxVis *vis, struct wlr_scene_node *node, gboolean enabled)
{
	GowlFxVisSave entry;
	guint i;

	if (vis == NULL || node == NULL)
		return;

	/* Remember only the FIRST state seen for a node.  A caller that
	 * touches the same node twice must still restore to what was there
	 * before it started, not to what it set in between. */
	for (i = 0; i < vis->saved->len; i++) {
		if (g_array_index(vis->saved, GowlFxVisSave, i).node == node) {
			wlr_scene_node_set_enabled(node, enabled);
			return;
		}
	}

	entry.node    = node;
	entry.enabled = node->enabled;
	g_array_append_val(vis->saved, entry);
	wlr_scene_node_set_enabled(node, enabled);
}

void
gowl_fx_vis_restore(GowlFxVis *vis)
{
	guint i;

	if (vis == NULL)
		return;

	/* Backwards, so a node touched more than once ends on its original
	 * state even if the bookkeeping above is ever relaxed. */
	for (i = vis->saved->len; i > 0; i--) {
		GowlFxVisSave *entry =
			&g_array_index(vis->saved, GowlFxVisSave, i - 1);

		wlr_scene_node_set_enabled(entry->node, entry->enabled);
	}
	g_array_free(vis->saved, TRUE);
	g_free(vis);
}

gboolean
gowl_fx_client_is_pinned(GowlClient *client)
{
	return client != NULL && (client->isembedded || client->isoverlay);
}

void
gowl_fx_vis_show_tags(GowlFxVis      *vis,
                      GowlCompositor *compositor,
                      GowlMonitor    *monitor,
                      guint32         tags)
{
	GList *l;

	if (vis == NULL || compositor == NULL || monitor == NULL)
		return;

	for (l = compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != monitor || c->scene == NULL
		    || gowl_fx_client_is_pinned(c))
			continue;
		gowl_fx_vis_set(vis, &c->scene->node, (c->tags & tags) != 0);
	}
}

void
gowl_fx_vis_show_only(GowlFxVis      *vis,
                      GowlCompositor *compositor,
                      GowlMonitor    *monitor,
                      GowlClient     *client)
{
	GList *l;

	if (vis == NULL || compositor == NULL || monitor == NULL)
		return;

	for (l = compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != monitor || c->scene == NULL)
			continue;
		gowl_fx_vis_set(vis, &c->scene->node, c == client);
	}
}

/*
 * gowl_fx_vis_hide_sheets:
 * @vis: a #GowlFxVis
 *
 * Hides every effect sheet currently parked in the scene.
 *
 * Hiding "every client layer" does not hide these: a #GowlFxSheet is a
 * direct child of scene->tree, a SIBLING of the layer trees, so that it
 * can sit above or below whole layers.  And a sheet is not decoration --
 * it is an opaque, monitor-sized picture of the desktop WITH its windows
 * in it, parked there by the cube, expo and switcher while they own the
 * output.
 *
 * So any capture meant to see past the windows has to hide these too.
 * Blur rebuilding its wallpaper backdrop during a tag switch, while the
 * cube's sheet was up, otherwise blurred the tag being LEFT into the
 * backdrop and cached it as the current one -- a translucent terminal on
 * tag 2 showing ghosts of the chat window from tag 4.
 */
void
gowl_fx_vis_hide_sheets(GowlFxVis *vis)
{
	GList *l;

	if (vis == NULL)
		return;

	for (l = gowl_fx_sheet_live(); l != NULL; l = l->next) {
		struct wlr_scene_tree *tree = gowl_fx_sheet_tree(l->data);

		if (tree != NULL)
			gowl_fx_vis_set(vis, &tree->node, FALSE);
	}
}

void
gowl_fx_vis_hide_layer(GowlFxVis      *vis,
                       GowlCompositor *compositor,
                       GowlSceneLayer  layer,
                       gboolean        keep_pinned)
{
	struct wlr_scene_node *node;
	struct wlr_scene_tree *tree;

	if (vis == NULL || compositor == NULL
	    || layer < 0 || layer >= GOWL_SCENE_LAYER_COUNT)
		return;

	tree = compositor->layers[layer];
	if (tree == NULL)
		return;

	if (!keep_pinned) {
		gowl_fx_vis_set(vis, &tree->node, FALSE);
		return;
	}

	/*
	 * A layer switched off a child at a time rather than wholesale,
	 * because the overlay layer holds two unrelated things: layer-shell
	 * surfaces, which an effect usually wants to stay live above it, and
	 * embedder-pinned clients, which belong with the desktop.
	 */
	wl_list_for_each(node, &tree->children, link) {
		GList *l;
		gboolean pinned = FALSE;

		for (l = compositor->clients; l != NULL && !pinned; l = l->next) {
			GowlClient *c = l->data;

			if (gowl_fx_client_is_pinned(c) && c->scene != NULL
			    && &c->scene->node == node)
				pinned = TRUE;
		}
		if (!pinned)
			gowl_fx_vis_set(vis, node, FALSE);
	}
}

/*
 * Render the scene for one output into a buffer from its own swapchain.
 *
 * wlr_scene_output_build_state() skips a render when nothing has changed,
 * and between two back-to-back captures nothing has from its point of
 * view -- hence the forced whole damage before, and again after, so the
 * real frame that follows still draws everything.
 */
/*
 * Take every filter sheet out of the scene for the length of a capture.
 *
 * A filter sheet is not a picture of some other state of the desktop --
 * it is a picture of the desktop AS IT IS, put through a shader and
 * parked on top, which is what modules/crt does every frame.  So it must
 * never be in a capture:
 *
 *   - its own, or the tube photographs its last frame and the picture
 *     recedes into itself the way a camera pointed at a monitor does;
 *   - anybody else's, or a magnifier, a cube face or a switcher preview
 *     is built from an already-filtered picture and gets a second tube
 *     applied to it when the filter runs again.
 *
 * Here rather than in each caller because every capture wants it and a
 * caller that forgot would look right until the day somebody switched
 * the tube on.
 */
static GowlFxVis *
capture_hide_filters(void)
{
	GowlFxVis *vis = NULL;
	GList     *l;

	for (l = gowl_fx_sheet_live(); l != NULL; l = l->next) {
		struct wlr_scene_tree *tree;

		if (!gowl_fx_sheet_is_filter(l->data))
			continue;
		tree = gowl_fx_sheet_tree(l->data);
		if (tree == NULL)
			continue;
		if (vis == NULL)
			vis = gowl_fx_vis_begin();
		gowl_fx_vis_set(vis, &tree->node, FALSE);
	}
	return vis;
}

static gboolean
capture_state(GowlCompositor *compositor, GowlMonitor *monitor,
               struct wlr_output_state *state)
{
	GowlFxVis *filters;
	gboolean   ok;

	if (compositor == NULL || monitor == NULL
	    || monitor->scene_output == NULL || monitor->wlr_output == NULL)
		return FALSE;

	wlr_output_state_init(state);
	wlr_damage_ring_add_whole(&monitor->scene_output->damage_ring);

	filters = capture_hide_filters();
	ok = wlr_scene_output_build_state(monitor->scene_output, state, NULL);
	if (filters != NULL)
		gowl_fx_vis_restore(filters);

	if (!ok || (state->committed & WLR_OUTPUT_STATE_BUFFER) == 0
	    || state->buffer == NULL) {
		wlr_output_state_finish(state);
		wlr_damage_ring_add_whole(&monitor->scene_output->damage_ring);
		return FALSE;
	}
	return TRUE;
}

gboolean
gowl_fx_capture(GowlFxGl       *self,
                GowlCompositor *compositor,
                GowlMonitor    *monitor,
                GowlFxTexture  *out,
                gint            divisor)
{
	struct wlr_output_state state;
	struct wlr_texture     *texture;
	gboolean                ok = FALSE;
	gint                    transform;
	gint                    width, height;

	if (self == NULL || out == NULL)
		return FALSE;
	if (!capture_state(compositor, monitor, &state))
		return FALSE;

	/*
	 * Stored UPRIGHT: in the output's logical orientation, at its
	 * transformed size, whatever the panel's rotation.
	 *
	 * The buffer wlr_scene rendered into is the output's own, with the
	 * output transform already applied -- a 1080x1920 panel turned on
	 * its side renders 1080x1920 pixels with the desktop lying on its
	 * side in them.  Everything that reads a capture works in layout
	 * coordinates (a window frame, a crop, a magnifier centre) and
	 * feeds the result back into the scene, which rotates it AGAIN on
	 * the way out.  Handing those readers the raw buffer showed the
	 * wallpaper 90 degrees off, and stretched, inside every translucent
	 * window on a rotated screen.  Undoing the transform once, here,
	 * puts every effect right at once.
	 */
	divisor   = MAX(1, divisor);
	transform = (gint)monitor->wlr_output->transform;
	gowl_fx_capture_size(monitor, divisor, &width, &height);
	texture = wlr_texture_from_buffer(compositor->renderer, state.buffer);
	if (texture != NULL) {
		ok = gowl_fx_texture_store_upright(self, out, texture,
		                                   width, height, transform);
		wlr_texture_destroy(texture);
	}

	wlr_output_state_finish(&state);
	wlr_damage_ring_add_whole(&monitor->scene_output->damage_ring);
	return ok;
}

gboolean
gowl_fx_capture_to_buffer(GowlFxGl           *self,
                          GowlCompositor     *compositor,
                          GowlMonitor        *monitor,
                          struct wlr_buffer **out)
{
	struct wlr_output_state state;

	if (out == NULL)
		return FALSE;
	*out = NULL;

	if (!capture_state(compositor, monitor, &state))
		return FALSE;

	/* Keep the buffer past wlr_output_state_finish(), which drops the
	 * swapchain's reference. */
	*out = wlr_buffer_lock(state.buffer);
	wlr_output_state_finish(&state);
	wlr_damage_ring_add_whole(&monitor->scene_output->damage_ring);
	return *out != NULL;
}

/*
 * The four corners of the quad, in the order gowl_fx_draw_quad_uv()
 * draws them, mapped from the UPRIGHT picture to where wlr_scene put
 * the same point in the output's buffer.
 *
 * wlr_scene places a logical box in the buffer with
 * wlr_box_transform(box, invert(output transform), ...), so asking
 * wlroots the same question here keeps the two in step by construction
 * rather than by a table that could disagree with it.  A unit square
 * stands in for both extents: every output transform is a combination
 * of an axis swap and reflections, which normalised coordinates survive
 * unchanged, so the logical size and the buffer size cancel.
 */
void
gowl_fx_upright_uv(gint transform, gfloat uv[8])
{
	static const gdouble corner[4][2] = {
		{ 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 1.0 },
	};
	enum wl_output_transform inv;
	gint i;

	inv = wlr_output_transform_invert(
		(enum wl_output_transform)(transform & 7));

	for (i = 0; i < 4; i++) {
		struct wlr_fbox in  = { corner[i][0], corner[i][1], 0.0, 0.0 };
		struct wlr_fbox got;

		wlr_fbox_transform(&got, &in, inv, 1.0, 1.0);
		uv[i * 2]     = (gfloat)got.x;
		uv[i * 2 + 1] = (gfloat)got.y;
	}
}

void
gowl_fx_capture_size(GowlMonitor *monitor, gint divisor,
                     gint *width, gint *height)
{
	gint w = 0, h = 0;

	if (monitor != NULL && monitor->wlr_output != NULL)
		wlr_output_transformed_resolution(monitor->wlr_output, &w, &h);

	divisor = MAX(1, divisor);
	if (width != NULL)
		*width = w > 0 ? MAX(1, w / divisor) : 0;
	if (height != NULL)
		*height = h > 0 ? MAX(1, h / divisor) : 0;
}

void
gowl_fx_capture_key_take(GowlFxCaptureKey *key, GowlMonitor *monitor)
{
	if (key == NULL)
		return;

	memset(key, 0, sizeof(*key));
	if (monitor == NULL || monitor->wlr_output == NULL)
		return;

	gowl_fx_capture_size(monitor, 1, &key->width, &key->height);
	key->transform = (gint)monitor->wlr_output->transform;
	key->serial    = monitor->backdrop_serial;
	key->valid     = TRUE;
}

gboolean
gowl_fx_capture_key_matches(const GowlFxCaptureKey *key,
                            GowlMonitor            *monitor)
{
	GowlFxCaptureKey now;

	if (key == NULL || !key->valid)
		return FALSE;

	gowl_fx_capture_key_take(&now, monitor);
	return now.valid
	       && now.width     == key->width
	       && now.height    == key->height
	       && now.transform == key->transform
	       && now.serial    == key->serial;
}
