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

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

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
static gboolean
capture_state(GowlCompositor *compositor, GowlMonitor *monitor,
               struct wlr_output_state *state)
{
	if (compositor == NULL || monitor == NULL
	    || monitor->scene_output == NULL || monitor->wlr_output == NULL)
		return FALSE;

	wlr_output_state_init(state);
	wlr_damage_ring_add_whole(&monitor->scene_output->damage_ring);

	if (!wlr_scene_output_build_state(monitor->scene_output, state, NULL)
	    || (state->committed & WLR_OUTPUT_STATE_BUFFER) == 0
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

	if (self == NULL || out == NULL)
		return FALSE;
	if (!capture_state(compositor, monitor, &state))
		return FALSE;

	divisor = MAX(1, divisor);
	texture = wlr_texture_from_buffer(compositor->renderer, state.buffer);
	if (texture != NULL) {
		ok = gowl_fx_texture_store(self, out, texture,
		                           MAX(1, state.buffer->width / divisor),
		                           MAX(1, state.buffer->height / divisor));
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
