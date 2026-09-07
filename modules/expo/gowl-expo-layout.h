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
 * gowl-expo-layout.h -- where the tiles go, and how the grid becomes one
 * screen again.
 *
 * Pure arithmetic: no wlroots, no GL, no compositor.  Everything about
 * the overview that can be quietly wrong -- a grid that does not fit, a
 * zoom that does not land exactly on the tile it came from, a click that
 * picks the tile next to the one under the pointer -- is decided here so
 * a test can check it without a screen.
 */

#ifndef GOWL_EXPO_LAYOUT_H
#define GOWL_EXPO_LAYOUT_H

#include <glib.h>

G_BEGIN_DECLS

#define GOWL_EXPO_MAX_TAGS 9

/**
 * GowlExpoRect:
 * @x: left edge, in output pixels
 * @y: top edge
 * @width: width
 * @height: height
 */
typedef struct {
	gdouble x, y, width, height;
} GowlExpoRect;

/**
 * GowlExpoLayout:
 * @count: how many tiles were laid out
 * @columns: columns used
 * @rows: rows used
 * @cell: each tile's rectangle, in output pixels
 *
 * The overview at full spread.
 */
typedef struct {
	gint         count;
	gint         columns;
	gint         rows;
	GowlExpoRect cell[GOWL_EXPO_MAX_TAGS];
} GowlExpoLayout;

/**
 * gowl_expo_layout_build:
 * @layout: (out): the layout to fill in
 * @count: tiles to place, 1..%GOWL_EXPO_MAX_TAGS
 * @columns: columns to use, or 0 to choose a near-square grid
 * @width: output width in pixels
 * @height: output height in pixels
 * @gap: spacing as a fraction of a cell, 0.0..0.4
 *
 * Lays @count tiles out in a grid that fills @width by @height.
 *
 * Every cell keeps the OUTPUT'S aspect ratio, because each one is a
 * picture of that output: a cell shaped differently would either letterbox
 * the desktop inside it or stretch it, and a stretched desktop in an
 * overview is immediately, distractingly wrong.  So the grid is sized to
 * whichever of width or height runs out first and centred in the other.
 *
 * Returns: %TRUE when a layout was produced.
 */
gboolean gowl_expo_layout_build (GowlExpoLayout *layout,
                                 gint            count,
                                 gint            columns,
                                 gdouble         width,
                                 gdouble         height,
                                 gdouble         gap);

/**
 * gowl_expo_layout_transform:
 * @layout: a built layout
 * @anchor: the tile the zoom starts and finishes on
 * @progress: 0.0 closed (one tile filling the screen) to 1.0 open
 * @width: output width in pixels
 * @height: output height in pixels
 * @scale: (out): the factor to scale every cell by
 * @offset_x: (out): what to add to a scaled cell's x
 * @offset_y: (out): what to add to a scaled cell's y
 *
 * The single transform that takes the whole grid from "the anchor tile is
 * the screen" to "here is the grid".
 *
 * Everything moves TOGETHER under one transform rather than each tile
 * animating separately, which is what makes it read as the camera pulling
 * back from one desktop rather than as nine pictures flying into place.
 * At @progress 0 it is exact: the anchor cell maps to precisely the
 * output rectangle, so the overview opens out of the real desktop and
 * closes back into it with no cut.
 */
void gowl_expo_layout_transform (const GowlExpoLayout *layout,
                                 gint                  anchor,
                                 gdouble               progress,
                                 gdouble               width,
                                 gdouble               height,
                                 gdouble              *scale,
                                 gdouble              *offset_x,
                                 gdouble              *offset_y);

/**
 * gowl_expo_layout_at:
 * @layout: a built layout
 * @scale: from gowl_expo_layout_transform()
 * @offset_x: from gowl_expo_layout_transform()
 * @offset_y: from gowl_expo_layout_transform()
 * @x: pointer x, in output pixels
 * @y: pointer y
 *
 * Which tile is under a point, accounting for the current zoom.
 *
 * Returns: a tile index, or -1 when the point is between or outside them.
 */
gint gowl_expo_layout_at (const GowlExpoLayout *layout,
                          gdouble               scale,
                          gdouble               offset_x,
                          gdouble               offset_y,
                          gdouble               x,
                          gdouble               y);

/**
 * gowl_expo_layout_step:
 * @layout: a built layout
 * @from: the tile selected now
 * @dx: -1 for left, +1 for right, 0 for neither
 * @dy: -1 for up, +1 for down, 0 for neither
 *
 * Moves the selection by one cell.
 *
 * Movement is clamped rather than wrapped.  A grid is a picture of a row
 * of tags, and running off the right-hand end onto the left is
 * disorienting in a way that stopping is not.
 *
 * Returns: the new tile index.
 */
gint gowl_expo_layout_step (const GowlExpoLayout *layout,
                            gint                  from,
                            gint                  dx,
                            gint                  dy);

G_END_DECLS

#endif /* GOWL_EXPO_LAYOUT_H */
