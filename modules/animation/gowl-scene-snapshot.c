/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-animation"

#include "gowl-scene-snapshot.h"
#include "gowl-wlroots-compat.h"

#include <math.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>

typedef struct {
	struct wlr_scene_buffer *node;
	struct wlr_buffer *buffer;
	struct wlr_box box;
} SnapshotPart;

static gboolean
capture_tree(GowlSceneSnapshot *snapshot, struct wlr_scene_tree *tree,
              gint x, gint y)
{
	struct wlr_scene_node *node;

	wl_list_for_each(node, &tree->children, link) {
		struct wlr_scene_buffer *source;
		struct wlr_scene_surface *surface;
		struct wlr_buffer *buffer;
		SnapshotPart part = { 0 };

		if (!node->enabled)
			continue;
		if (node->type == WLR_SCENE_NODE_TREE) {
			if (!capture_tree(snapshot, wlr_scene_tree_from_node(node),
			                  x + node->x, y + node->y))
				return FALSE;
			continue;
		}
		if (node->type != WLR_SCENE_NODE_BUFFER)
			continue;

		source = wlr_scene_buffer_from_node(node);
		/* Scene buffers may release their buffer after importing its
		 * texture. The surface still owns the rendered client buffer. */
		surface = wlr_scene_surface_try_from_buffer(source);
		buffer = surface != NULL && surface->surface->buffer != NULL
			? &surface->surface->buffer->base : source->buffer;
		if (buffer == NULL)
			return FALSE; /* Never hide a window behind a partial copy. */

		part.box.x = x + node->x;
		part.box.y = y + node->y;
		part.box.width = source->dst_width;
		part.box.height = source->dst_height;
		if (part.box.width == 0 || part.box.height == 0) {
			gboolean rotated = source->transform & 1;

			part.box.width = rotated ? buffer->height : buffer->width;
			part.box.height = rotated ? buffer->width : buffer->height;
		}
		part.node = wlr_scene_buffer_create(snapshot->tree, buffer);
		if (part.node == NULL)
			return FALSE;
		part.buffer = wlr_buffer_lock(buffer);
		g_array_append_val(snapshot->parts, part);

		/* In particular, keep the cropped source rectangle: copying a
		 * raw GTK buffer includes invisible CSD shadows and shifts the
		 * content when we hand back to the real window. */
		wlr_scene_buffer_set_source_box(part.node, &source->src_box);
		wlr_scene_buffer_set_transform(part.node, source->transform);
		wlr_scene_buffer_set_filter_mode(part.node, source->filter_mode);
		wlr_scene_buffer_set_opacity(part.node, source->opacity);
#if GOWL_HAVE_WLROOTS_0_20
		wlr_scene_buffer_set_transfer_function(part.node, source->transfer_function);
		wlr_scene_buffer_set_primaries(part.node, source->primaries);
		wlr_scene_buffer_set_color_encoding(part.node, source->color_encoding);
		wlr_scene_buffer_set_color_range(part.node, source->color_range);
#endif
	}
	return TRUE;
}

GowlSceneSnapshot *
gowl_scene_snapshot_new(struct wlr_scene_tree *parent,
                        struct wlr_scene_tree *source, gint width, gint height)
{
	GowlSceneSnapshot *snapshot;

	if (parent == NULL || source == NULL || width <= 0 || height <= 0)
		return NULL;

	snapshot = g_new0(GowlSceneSnapshot, 1);
	snapshot->tree = wlr_scene_tree_create(parent);
	snapshot->parts = g_array_new(FALSE, FALSE, sizeof(SnapshotPart));
	snapshot->width = width;
	snapshot->height = height;
	if (snapshot->tree == NULL || !capture_tree(snapshot, source, 0, 0)
	    || snapshot->parts->len == 0) {
		gowl_scene_snapshot_free(snapshot);
		return NULL;
	}
	gowl_scene_snapshot_resize(snapshot, width, height);
	return snapshot;
}

void
gowl_scene_snapshot_free(GowlSceneSnapshot *snapshot)
{
	guint i;

	if (snapshot == NULL)
		return;
	if (snapshot->tree != NULL)
		wlr_scene_node_destroy(&snapshot->tree->node);
	for (i = 0; i < snapshot->parts->len; i++) {
		SnapshotPart *part = &g_array_index(snapshot->parts, SnapshotPart, i);

		wlr_buffer_unlock(part->buffer);
	}
	g_array_unref(snapshot->parts);
	g_free(snapshot);
}

void
gowl_scene_snapshot_resize(GowlSceneSnapshot *snapshot, gint width, gint height)
{
	guint i;
	gdouble sx, sy;

	if (snapshot == NULL)
		return;
	sx = (gdouble)MAX(1, width) / snapshot->width;
	sy = (gdouble)MAX(1, height) / snapshot->height;
	for (i = 0; i < snapshot->parts->len; i++) {
		SnapshotPart *part = &g_array_index(snapshot->parts, SnapshotPart, i);
		gint x = (gint)lround(part->box.x * sx);
		gint y = (gint)lround(part->box.y * sy);
		gint right = (gint)lround((part->box.x + part->box.width) * sx);
		gint bottom = (gint)lround((part->box.y + part->box.height) * sy);

		/* Round shared edges identically, avoiding seams between tiles. */
		wlr_scene_node_set_position(&part->node->node, x, y);
		wlr_scene_buffer_set_dest_size(part->node, MAX(1, right - x),
		                                MAX(1, bottom - y));
	}
}

void
gowl_scene_snapshot_set_opacity(GowlSceneSnapshot *snapshot, gfloat alpha)
{
	guint i;

	if (snapshot == NULL)
		return;
	for (i = 0; i < snapshot->parts->len; i++) {
		SnapshotPart *part = &g_array_index(snapshot->parts, SnapshotPart, i);

		wlr_scene_buffer_set_opacity(part->node, CLAMP(alpha, 0.0f, 1.0f));
	}
}
