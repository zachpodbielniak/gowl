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

#include "gowl-expo-layout.h"

#include <math.h>
#include <string.h>

gboolean
gowl_expo_layout_build(
	GowlExpoLayout *layout,
	gint            count,
	gint            columns,
	gdouble         width,
	gdouble         height,
	gdouble         gap
){
	gdouble cell_w, cell_h, grid_h, origin_y, pad;
	gint    rows, i;

	if (layout == NULL || count <= 0 || width <= 0.0 || height <= 0.0)
		return FALSE;

	memset(layout, 0, sizeof(*layout));
	count = MIN(count, GOWL_EXPO_MAX_TAGS);
	gap   = CLAMP(gap, 0.0, 0.4);

	/*
	 * A near-square grid unless told otherwise.  ceil(sqrt(n)) gives 2x2
	 * for four, 3x3 for nine and 3x2 for six, which is what somebody
	 * drawing it by hand would do.
	 */
	if (columns <= 0)
		columns = (gint)ceil(sqrt((gdouble)count));
	columns = CLAMP(columns, 1, count);
	rows = (count + columns - 1) / columns;

	/*
	 * Each cell keeps the OUTPUT'S shape, because each is a picture of
	 * that output.  So try filling the width, and fall back to filling
	 * the height when that would make the grid too tall -- the same
	 * "contain" fit an image viewer uses, applied to the whole grid.
	 */
	cell_w = width / ((gdouble)columns * (1.0 + gap) + gap);
	cell_h = cell_w * (height / width);
	if (cell_h * ((gdouble)rows * (1.0 + gap) + gap) > height) {
		cell_h = height / ((gdouble)rows * (1.0 + gap) + gap);
		cell_w = cell_h * (width / height);
	}

	pad = cell_w * gap;
	grid_h = (gdouble)rows * cell_h + (gdouble)(rows - 1) * (cell_h * gap);
	origin_y = (height - grid_h) * 0.5;
	/* Columns are centred per ROW rather than once for the grid, so a
	 * short last row sits under the middle of the ones above it. */

	layout->count   = count;
	layout->columns = columns;
	layout->rows    = rows;

	for (i = 0; i < count; i++) {
		gint col = i % columns;
		gint row = i / columns;
		gdouble row_w;

		layout->cell[i].width  = cell_w;
		layout->cell[i].height = cell_h;
		layout->cell[i].y = origin_y + (gdouble)row * (cell_h + cell_h * gap);

		/*
		 * A short last row is centred rather than left-aligned.  Nine
		 * tags in a 3x3 grid never hit this; seven do, and a lone tile
		 * hanging off the left of the bottom row looks like a mistake.
		 */
		{
			gint in_row = MIN(columns, count - row * columns);

			row_w = (gdouble)in_row * cell_w + (gdouble)(in_row - 1) * pad;
			layout->cell[i].x = (width - row_w) * 0.5
			                    + (gdouble)col * (cell_w + pad);
		}
	}
	return TRUE;
}

void
gowl_expo_layout_transform(
	const GowlExpoLayout *layout,
	gint                  anchor,
	gdouble               progress,
	gdouble               width,
	gdouble               height,
	gdouble              *scale,
	gdouble              *offset_x,
	gdouble              *offset_y
){
	const GowlExpoRect *cell;
	gdouble s_closed, s, anchor_cx, anchor_cy, target_cx, target_cy;

	if (scale != NULL)    *scale = 1.0;
	if (offset_x != NULL) *offset_x = 0.0;
	if (offset_y != NULL) *offset_y = 0.0;

	if (layout == NULL || layout->count <= 0)
		return;

	anchor = CLAMP(anchor, 0, layout->count - 1);
	cell = &layout->cell[anchor];
	if (cell->width <= 0.0 || cell->height <= 0.0)
		return;

	/*
	 * The closed scale is whatever makes the anchor cell exactly the
	 * output.  Exactly, not approximately: this is the number that makes
	 * the overview open out of the live desktop with no cut, in the same
	 * way the cube's camera distance does.
	 */
	s_closed = width / cell->width;
	s = s_closed + (1.0 - s_closed) * CLAMP(progress, 0.0, 1.0);

	anchor_cx = cell->x + cell->width * 0.5;
	anchor_cy = cell->y + cell->height * 0.5;

	/* Where the anchor's centre should land: the middle of the screen
	 * when closed, its own grid position when open. */
	target_cx = (width * 0.5) + (anchor_cx - width * 0.5)
	            * CLAMP(progress, 0.0, 1.0);
	target_cy = (height * 0.5) + (anchor_cy - height * 0.5)
	            * CLAMP(progress, 0.0, 1.0);

	if (scale != NULL)
		*scale = s;
	if (offset_x != NULL)
		*offset_x = target_cx - anchor_cx * s;
	if (offset_y != NULL)
		*offset_y = target_cy - anchor_cy * s;
}

gint
gowl_expo_layout_at(
	const GowlExpoLayout *layout,
	gdouble               scale,
	gdouble               offset_x,
	gdouble               offset_y,
	gdouble               x,
	gdouble               y
){
	gint i;

	if (layout == NULL || scale <= 0.0)
		return -1;

	for (i = 0; i < layout->count; i++) {
		gdouble cx = layout->cell[i].x * scale + offset_x;
		gdouble cy = layout->cell[i].y * scale + offset_y;
		gdouble cw = layout->cell[i].width * scale;
		gdouble ch = layout->cell[i].height * scale;

		if (x >= cx && y >= cy && x < cx + cw && y < cy + ch)
			return i;
	}
	return -1;
}

gint
gowl_expo_layout_step(const GowlExpoLayout *layout, gint from, gint dx, gint dy)
{
	gint col, row, target;

	if (layout == NULL || layout->count <= 0)
		return 0;

	from = CLAMP(from, 0, layout->count - 1);
	col = from % layout->columns;
	row = from / layout->columns;

	col = CLAMP(col + dx, 0, layout->columns - 1);
	row = CLAMP(row + dy, 0, layout->rows - 1);

	target = row * layout->columns + col;
	/*
	 * A short last row leaves holes in the rectangle.  Stepping into one
	 * lands on the nearest real tile instead of on nothing, so the
	 * selection never disappears.
	 */
	if (target >= layout->count)
		target = layout->count - 1;
	return target;
}
