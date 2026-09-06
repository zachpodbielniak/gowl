/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef GOWL_SCENE_SNAPSHOT_H
#define GOWL_SCENE_SNAPSHOT_H

#include <glib.h>
#include <wlr/types/wlr_scene.h>

/* Private compositor helper. Coordinates are relative to the source tree;
 * copying each buffer's position as well as its size keeps subsurfaces
 * together. The source can be destroyed as soon as capture returns. */
typedef struct {
	struct wlr_scene_tree *tree;
	GArray *parts;
	gint width, height;
} GowlSceneSnapshot;

GowlSceneSnapshot *gowl_scene_snapshot_new(struct wlr_scene_tree *parent,
                                          struct wlr_scene_tree *source,
                                          gint width, gint height);
void gowl_scene_snapshot_free(GowlSceneSnapshot *snapshot);
void gowl_scene_snapshot_resize(GowlSceneSnapshot *snapshot,
                                 gint width, gint height);
void gowl_scene_snapshot_set_opacity(GowlSceneSnapshot *snapshot, gfloat alpha);

#endif
