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
	format.format    = output->render_format;
	format.len       = 1;
	format.capacity  = 1;
	format.modifiers = &modifier;

	sheet->swapchain = wlr_swapchain_create(sheet->compositor->allocator,
	                                        output->width, output->height,
	                                        &format);
	if (sheet->swapchain == NULL) {
		/* Some drivers refuse the output's render format for an
		 * off-screen buffer.  Plain opaque 8888 is the format every GBM
		 * allocator in existence can produce. */
		format.format = DRM_FORMAT_XRGB8888;
		sheet->swapchain = wlr_swapchain_create(
			sheet->compositor->allocator,
			output->width, output->height, &format);
	}
	return sheet->swapchain != NULL;
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
	sheet->width      = monitor->wlr_output->width;
	sheet->height     = monitor->wlr_output->height;

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
	 * must own the whole screen instead.
	 */
	wlr_scene_node_place_above(&sheet->tree->node,
	                           (flags & GOWL_FX_SHEET_ABOVE_TOP) != 0
	                               ? &compositor->layers[GOWL_SCENE_LAYER_TOP]->node
	                               : &compositor->layers[GOWL_SCENE_LAYER_FLOAT]->node);
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
	pixman_region32_init_rect(&opaque, 0, 0,
	                          monitor->m.width, monitor->m.height);
	wlr_scene_buffer_set_opaque_region(sheet->node, &opaque);
	pixman_region32_fini(&opaque);

	/*
	 * Two kinds of client sit above the sheet in gowl's layer order:
	 * fullscreen clients, and the embedder-pinned ones.  Each is taken
	 * down individually and put back from a HELD REFERENCE, so a window
	 * closed while the effect runs cannot leave a dangling pointer to
	 * re-enable.
	 */
	sheet->hidden = g_ptr_array_new_with_free_func(g_object_unref);
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

	return sheet;
}

void
gowl_fx_sheet_free(GowlFxSheet *sheet)
{
	GowlMonitor *monitor;

	if (sheet == NULL)
		return;

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

struct wlr_buffer *
gowl_fx_sheet_acquire(GowlFxSheet *sheet)
{
	if (sheet == NULL || sheet->swapchain == NULL)
		return NULL;
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
	if (width != NULL)
		*width = sheet != NULL ? sheet->width : 0;
	if (height != NULL)
		*height = sheet != NULL ? sheet->height : 0;
}
