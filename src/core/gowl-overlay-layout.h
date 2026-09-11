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

#ifndef GOWL_OVERLAY_LAYOUT_H
#define GOWL_OVERLAY_LAYOUT_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GOWL_OVERLAY_DEFAULT_WIDTH_PCT:
 *
 * Width an overlay panel takes when none is configured: the whole usable
 * width of its output.
 */
#define GOWL_OVERLAY_DEFAULT_WIDTH_PCT (1.0)

/**
 * GOWL_OVERLAY_DEFAULT_HEIGHT_PCT:
 *
 * Height an overlay panel takes when none is configured: two thirds of
 * the output's usable height, the Guake-style dropdown's size.
 */
#define GOWL_OVERLAY_DEFAULT_HEIGHT_PCT (2.0 / 3.0)

/**
 * gowl_overlay_panel_box:
 * @area_x: left edge of the output's usable area
 * @area_y: top edge of the output's usable area
 * @area_width: width of the usable area
 * @area_height: height of the usable area
 * @width_pct: fraction of @area_width; <= 0 means the default
 * @height_pct: fraction of @area_height; <= 0 means the default
 * @width_abs: width in pixels; > 0 takes precedence over @width_pct
 * @height_abs: height in pixels; > 0 takes precedence over @height_pct
 * @anchor: the edge the panel attaches to: 0 top, 1 bottom, 2 left, 3 right
 * @out_x: (out): the panel's left edge
 * @out_y: (out): the panel's top edge
 * @out_width: (out): the panel's width, between 1 and @area_width
 * @out_height: (out): the panel's height, between 1 and @area_height
 *
 * Computes the rectangle an overlay panel occupies on an output.  The
 * Guake-style dropdown (anchored top) and the scratchpad (anchored
 * bottom) both come from here, which is what keeps them the same size.
 * A top or bottom panel is centred horizontally, a left or right one
 * vertically.
 */
void
gowl_overlay_panel_box(
	gint     area_x,
	gint     area_y,
	gint     area_width,
	gint     area_height,
	gdouble  width_pct,
	gdouble  height_pct,
	gint     width_abs,
	gint     height_abs,
	gint     anchor,
	gint    *out_x,
	gint    *out_y,
	gint    *out_width,
	gint    *out_height
);

/**
 * gowl_overlay_tile_columns:
 * @panel_x: the panel's left edge
 * @panel_y: the panel's top edge
 * @panel_width: the panel's width
 * @panel_height: the panel's height
 * @n: how many windows share the panel
 * @index: which of them, from 0
 * @gap: pixels between neighbouring columns; negative is 0, and a gap
 *   that would leave a column under a pixel wide shrinks to fit
 * @out_x: (out): the column's left edge
 * @out_y: (out): the column's top edge
 * @out_width: (out): the column's width
 * @out_height: (out): the column's height
 *
 * Splits a panel into @n equal columns and returns column @index.  The
 * columns fill the panel exactly -- leftover pixels go one each to the
 * first columns -- and every column has the panel's top edge and
 * height, so a slide that moves each column from the output's edge to
 * its place moves them as one panel.
 *
 * Returns: %FALSE, leaving the outputs untouched, if @n is 0 or @index
 *   is out of range
 */
gboolean
gowl_overlay_tile_columns(
	gint   panel_x,
	gint   panel_y,
	gint   panel_width,
	gint   panel_height,
	guint  n,
	guint  index,
	gint   gap,
	gint  *out_x,
	gint  *out_y,
	gint  *out_width,
	gint  *out_height
);

G_END_DECLS

#endif /* GOWL_OVERLAY_LAYOUT_H */
