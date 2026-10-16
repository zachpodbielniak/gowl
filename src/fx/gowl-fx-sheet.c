/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Taking an output for the length of an effect.
 *
 * The sheet is one opaque, monitor-sized buffer parked in the scene.  It
 * hides what is below it by COVERING it, and that is a deliberate choice
 * rather than laziness: the scene layers are shared by every output while
 * tags are per-monitor, so an effect that switched a layer off to make
 * room on one screen would blank the other one.  Only the few things gowl
 * stacks ABOVE the sheet have to be taken down by hand.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include "core/gowl-core-private.h"

#include <drm_fourcc.h>

#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

struct _GowlFxSheet {
	GowlCompositor          *compositor;   /* unowned */
	GowlMonitor             *monitor;      /* unowned */
	struct wlr_scene_tree   *tree;
	struct wlr_scene_buffer *node;
	struct wlr_swapchain    *swapchain;
	GPtrArray               *hidden;       /* GowlClient*, referenced */
	gboolean                 hid_fullscreen_bg;
	gboolean                 is_filter;
	gint                     width;
	gint                     height;
};

static gboolean
sheet_swapchain_init(GowlFxSheet *sheet)
{
	struct wlr_drm_format format;
	uint64_t              modifier = DRM_FORMAT_MOD_INVALID;
	struct wlr_output    *output = sheet->monitor->wlr_output;

	memset(&format, 0, sizeof(format));
	/*
	 * A FILTER SHEET IS ALLOCATED WITH AN ALPHA CHANNEL IT NEVER USES.
	 *
	 * wlr_scene decides a buffer is opaque from its FORMAT before it
	 * looks at any region: an XRGB buffer occludes everything under it
	 * whatever gowl_fx_sheet_new() declares.  A filter sheet must not
	 * occlude -- see the comment there -- so it has to be a format the
	 * scene cannot assume about.  The shader writes alpha 1 everywhere,
	 * so this costs one ignored byte per pixel and nothing else.
	 */
	format.format    = sheet->is_filter ? DRM_FORMAT_ARGB8888
	                                    : output->render_format;
	format.len       = 1;
	format.capacity  = 1;
	format.modifiers = &modifier;

	sheet->swapchain = wlr_swapchain_create(sheet->compositor->allocator,
	                                        sheet->width, sheet->height,
	                                        &format);
	if (sheet->swapchain == NULL) {
		/* Some drivers refuse the output's render format for an
		 * off-screen buffer.  Plain opaque 8888 is the format every GBM
		 * allocator in existence can produce. */
		format.format = sheet->is_filter ? DRM_FORMAT_ARGB8888
		                                : DRM_FORMAT_XRGB8888;
		sheet->swapchain = wlr_swapchain_create(
			sheet->compositor->allocator,
			sheet->width, sheet->height, &format);
	}
	return sheet->swapchain != NULL;
}

/*
 * Live sheets, so a capture can hide them.
 *
 * A sheet hangs off scene->tree beside the layer trees rather than
 * inside one, which is deliberate -- it has to be able to sit above or
 * below whole layers.  The cost is that hiding "every client layer" does
 * not hide it, and a sheet holds a picture of the desktop WITH windows.
 * Blur rebuilding its wallpaper backdrop while a cube or expo sheet was
 * parked therefore blurred the OUTGOING tag's windows into the backdrop
 * and cached the result as current.
 *
 * Compositor thread only, so a plain list needs no locking.
 */
static GList *fx_live_sheets = NULL;

GList *
gowl_fx_sheet_live(void)
{
	return fx_live_sheets;
}

struct wlr_scene_tree *
gowl_fx_sheet_tree(GowlFxSheet *sheet)
{
	return sheet != NULL ? sheet->tree : NULL;
}

gboolean
gowl_fx_sheet_is_filter(GowlFxSheet *sheet)
{
	return sheet != NULL && sheet->is_filter;
}

GowlFxSheet *
gowl_fx_sheet_new(GowlCompositor   *compositor,
                  GowlMonitor      *monitor,
                  GowlFxSheetFlags  flags)
{
	GowlFxSheet      *sheet;
	pixman_region32_t opaque;
	GList            *l;

	if (compositor == NULL || monitor == NULL
	    || monitor->wlr_output == NULL || monitor->scene_output == NULL
	    || monitor->m.width <= 0 || monitor->m.height <= 0)
		return NULL;

	sheet = g_new0(GowlFxSheet, 1);
	sheet->compositor = compositor;
	sheet->monitor    = monitor;
	sheet->is_filter  = (flags & GOWL_FX_SHEET_FILTER) != 0;
	/*
	 * UPRIGHT, the size gowl_fx_capture() stores at: the sheet is a
	 * scene buffer in layout coordinates, and the output turns it on the
	 * way out like everything else in the scene.  The mode's own width
	 * and height are the panel's orientation, not the screen's, and a
	 * sheet that size on a rotated output came out squeezed into the
	 * other axis.
	 */
	gowl_fx_capture_size(monitor, 1, &sheet->width, &sheet->height);
	if (sheet->width <= 0 || sheet->height <= 0) {
		g_free(sheet);
		return NULL;
	}

	if (!sheet_swapchain_init(sheet)) {
		g_warning("fx: no off-screen buffers for %s; effects on that "
		          "output will be skipped",
		          monitor->wlr_output->name != NULL
		              ? monitor->wlr_output->name : "?");
		g_free(sheet);
		return NULL;
	}

	sheet->tree = wlr_scene_tree_create(&compositor->scene->tree);
	if (sheet->tree == NULL) {
		g_clear_pointer(&sheet->swapchain, wlr_swapchain_destroy);
		g_free(sheet);
		return NULL;
	}

	/*
	 * Under the top layer by default, so the bar and notifications stay
	 * live above the effect --- which is what a panel does during a
	 * workspace animation everywhere else.  ABOVE_TOP is for effects that
	 * must own the whole screen instead, and ABOVE_OVERLAY for one that
	 * must own it including the fullscreen and overlay layers.  The
	 * session lock stays above all three: a lock a shader could hide is
	 * not a lock.
	 */
	{
		GowlSceneLayer under = GOWL_SCENE_LAYER_FLOAT;

		if ((flags & GOWL_FX_SHEET_ABOVE_OVERLAY) != 0)
			under = GOWL_SCENE_LAYER_OVERLAY;
		else if ((flags & GOWL_FX_SHEET_ABOVE_TOP) != 0)
			under = GOWL_SCENE_LAYER_TOP;
		wlr_scene_node_place_above(&sheet->tree->node,
		                           &compositor->layers[under]->node);
	}
	wlr_scene_node_set_position(&sheet->tree->node,
	                            monitor->m.x, monitor->m.y);

	sheet->node = wlr_scene_buffer_create(sheet->tree, NULL);
	if (sheet->node == NULL) {
		wlr_scene_node_destroy(&sheet->tree->node);
		g_clear_pointer(&sheet->swapchain, wlr_swapchain_destroy);
		g_free(sheet);
		return NULL;
	}

	wlr_scene_buffer_set_dest_size(sheet->node,
	                               monitor->m.width, monitor->m.height);

	/*
	 * A FILTER SHEET DECLARES NO OPAQUE REGION, AND THAT IS THE WHOLE
	 * DIFFERENCE BETWEEN A TUBE AND A FROZEN SCREEN.
	 *
	 * The opaque region is an optimisation hint, and wlr_scene acts on
	 * it twice: it culls damage from nodes it can prove are hidden, and
	 * it sends frame callbacks only to surfaces that are visible.  A
	 * full-screen opaque sheet therefore tells every window on the
	 * machine that it cannot be seen -- so their damage stops
	 * scheduling frames and their frame callbacks stop arriving, and
	 * they stop drawing altogether.
	 *
	 * For the effects that park a sheet for a second that is a saving.
	 * For one that stays up, it is fatal and it is SILENT: the screen
	 * keeps showing the picture that was captured when the sheet went
	 * up, the compositor sits at no CPU, nothing is logged, and it reads
	 * exactly like the editor having hung.  It has not; it is drawing
	 * into a scene nobody is compositing.
	 *
	 * And the hint would be a lie in any case.  A filter sheet is a
	 * PHOTOGRAPH of what is underneath it, retaken every frame: the
	 * things it covers are the things it is made of, so they have to
	 * keep drawing for it to have anything to photograph.
	 *
	 * tests/test-crt-frames.c is this paragraph as an assertion.
	 */
	if (!sheet->is_filter) {
		pixman_region32_init_rect(&opaque, 0, 0,
		                          monitor->m.width, monitor->m.height);
		wlr_scene_buffer_set_opaque_region(sheet->node, &opaque);
		pixman_region32_fini(&opaque);
	}

	/*
	 * Two kinds of client sit above the sheet in gowl's layer order:
	 * fullscreen clients, and the embedder-pinned ones.  Each is taken
	 * down individually and put back from a HELD REFERENCE, so a window
	 * closed while the effect runs cannot leave a dangling pointer to
	 * re-enable.
	 */
	sheet->hidden = g_ptr_array_new_with_free_func(g_object_unref);

	/*
	 * Nothing to get out of the way when the sheet is above every layer
	 * a client can be in.  This is not an optimisation: an effect that
	 * re-captures the screen every frame would find these clients
	 * SWITCHED OFF in its own capture and draw a desktop with the
	 * fullscreen window missing from it.
	 */
	if ((flags & GOWL_FX_SHEET_ABOVE_OVERLAY) != 0) {
		fx_live_sheets = g_list_prepend(fx_live_sheets, sheet);
		return sheet;
	}

	for (l = compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;
		gboolean fullscreen;

		if (c->mon != monitor || c->scene == NULL || !c->scene->node.enabled)
			continue;

		fullscreen = c->scene->node.parent
			== compositor->layers[GOWL_SCENE_LAYER_FS];
		if (fullscreen && (flags & GOWL_FX_SHEET_KEEP_FULLSCREEN) != 0)
			continue;
		if (!fullscreen && !gowl_fx_client_is_pinned(c))
			continue;

		wlr_scene_node_set_enabled(&c->scene->node, FALSE);
		g_ptr_array_add(sheet->hidden, g_object_ref(c));
	}

	if (monitor->fullscreen_bg != NULL && monitor->fullscreen_bg->node.enabled
	    && (flags & GOWL_FX_SHEET_KEEP_FULLSCREEN) == 0) {
		wlr_scene_node_set_enabled(&monitor->fullscreen_bg->node, FALSE);
		sheet->hid_fullscreen_bg = TRUE;
	}

	fx_live_sheets = g_list_prepend(fx_live_sheets, sheet);
	return sheet;
}

void
gowl_fx_sheet_free(GowlFxSheet *sheet)
{
	GowlMonitor *monitor;

	if (sheet == NULL)
		return;

	fx_live_sheets = g_list_remove(fx_live_sheets, sheet);

	monitor = sheet->monitor;

	/* Put the clients back BEFORE the sheet goes, so no frame can catch
	 * the output with neither on it. */
	if (sheet->hidden != NULL) {
		guint i;

		for (i = 0; i < sheet->hidden->len; i++) {
			GowlClient *c = g_ptr_array_index(sheet->hidden, i);

			if (c->scene != NULL)
				wlr_scene_node_set_enabled(&c->scene->node, TRUE);
		}
		g_ptr_array_free(sheet->hidden, TRUE);
	}
	if (sheet->hid_fullscreen_bg && monitor != NULL
	    && monitor->fullscreen_bg != NULL)
		wlr_scene_node_set_enabled(&monitor->fullscreen_bg->node, TRUE);

	if (sheet->tree != NULL)
		wlr_scene_node_destroy(&sheet->tree->node);
	g_clear_pointer(&sheet->swapchain, wlr_swapchain_destroy);

	/* A scene node vanishing is not damage the output notices by itself,
	 * so without this the effect's last frame stays frozen on screen. */
	if (monitor != NULL && monitor->scene_output != NULL)
		wlr_damage_ring_add_whole(&monitor->scene_output->damage_ring);

	g_free(sheet);
}

/*
 * Follow the output if it was turned or re-moded under a sheet that is
 * still up.  The tube and the magnifier keep theirs for as long as they
 * are on, so the size they were made at is not a size they can keep.
 * The old swapchain goes only once a new one exists; a buffer the scene
 * still shows holds its own lock and outlives it.
 */
static void
sheet_follow_output(GowlFxSheet *sheet)
{
	struct wlr_swapchain *old;
	gint                  width, height, was_w, was_h;

	if (sheet->monitor == NULL || sheet->swapchain == NULL)
		return;

	gowl_fx_capture_size(sheet->monitor, 1, &width, &height);
	if (width <= 0 || height <= 0
	    || (width == sheet->width && height == sheet->height))
		return;

	old   = sheet->swapchain;
	was_w = sheet->width;
	was_h = sheet->height;

	sheet->swapchain = NULL;
	sheet->width     = width;
	sheet->height    = height;
	if (!sheet_swapchain_init(sheet)) {
		sheet->swapchain = old;
		sheet->width     = was_w;
		sheet->height    = was_h;
		return;
	}
	g_clear_pointer(&old, wlr_swapchain_destroy);
}

struct wlr_buffer *
gowl_fx_sheet_acquire(GowlFxSheet *sheet)
{
	if (sheet == NULL || sheet->swapchain == NULL)
		return NULL;
	sheet_follow_output(sheet);
	return wlr_swapchain_acquire(sheet->swapchain);
}

void
gowl_fx_sheet_present(GowlFxSheet *sheet, struct wlr_buffer *buffer)
{
	if (sheet == NULL || sheet->node == NULL || buffer == NULL)
		return;

	wlr_scene_buffer_set_buffer(sheet->node, buffer);

	/* Re-fit every frame: the output may have been reshaped under us, and
	 * a stale size letterboxes the effect. */
	if (sheet->monitor != NULL) {
		wlr_scene_node_set_position(&sheet->tree->node,
		                            sheet->monitor->m.x, sheet->monitor->m.y);
		wlr_scene_buffer_set_dest_size(sheet->node,
		                               sheet->monitor->m.width,
		                               sheet->monitor->m.height);
	}
}

void
gowl_fx_sheet_set_visible(GowlFxSheet *sheet, gboolean visible)
{
	if (sheet == NULL || sheet->tree == NULL)
		return;
	wlr_scene_node_set_enabled(&sheet->tree->node, visible);
}

GowlMonitor *
gowl_fx_sheet_get_monitor(GowlFxSheet *sheet)
{
	return sheet != NULL ? sheet->monitor : NULL;
}

void
gowl_fx_sheet_get_size(GowlFxSheet *sheet, gint *width, gint *height)
{
	/* The size the NEXT acquire hands out, so a caller that asks first
	 * and draws second agrees with the buffer it gets. */
	if (sheet != NULL)
		sheet_follow_output(sheet);
	if (width != NULL)
		*width = sheet != NULL ? sheet->width : 0;
	if (height != NULL)
		*height = sheet != NULL ? sheet->height : 0;
}
