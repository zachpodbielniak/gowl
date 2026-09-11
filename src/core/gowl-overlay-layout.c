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
 * gowl-overlay-layout.c - where an overlay panel sits on an output, and
 * how the scratchpad splits one between windows.
 *
 * Pure arithmetic: no wlroots, no compositor, so tests/test-overlay-layout
 * exercises it directly.  The dropdown and the scratchpad both size their
 * panels here, which is the whole guarantee that the scratchpad covers
 * the same space as the quake shell from the opposite edge.
 */

#include "gowl-overlay-layout.h"

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
){
	gint w;
	gint h;

	/* An absolute size wins; a fraction applies only without one, and
	 * an unset fraction falls back to the dropdown's defaults. */
	w = width_abs > 0
		? width_abs
		: (gint)(area_width * (width_pct > 0 ? width_pct
		                                     : GOWL_OVERLAY_DEFAULT_WIDTH_PCT));
	h = height_abs > 0
		? height_abs
		: (gint)(area_height * (height_pct > 0 ? height_pct
		                                       : GOWL_OVERLAY_DEFAULT_HEIGHT_PCT));

	/* Never larger than the area, and never degenerate -- an output
	 * that has no size yet still gets a real rectangle. */
	w = CLAMP(w, 1, MAX(1, area_width));
	h = CLAMP(h, 1, MAX(1, area_height));

	switch (anchor) {
	case 1: /* bottom */
		*out_x = area_x + (area_width - w) / 2;
		*out_y = area_y + area_height - h;
		break;
	case 2: /* left */
		*out_x = area_x;
		*out_y = area_y + (area_height - h) / 2;
		break;
	case 3: /* right */
		*out_x = area_x + area_width - w;
		*out_y = area_y + (area_height - h) / 2;
		break;
	default: /* top */
		*out_x = area_x + (area_width - w) / 2;
		*out_y = area_y;
		break;
	}
	*out_width  = w;
	*out_height = h;
}

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
){
	gint columns;
	gint usable;
	gint base;
	gint extra;
	gint x;
	gint i;

	if (n == 0 || index >= n)
		return FALSE;

	columns = (gint)n;
	if (gap < 0)
		gap = 0;

	/* A gap that would leave a column under a pixel gives way until
	 * every column keeps at least one. */
	if (columns > 1 && panel_width - gap * (columns - 1) < columns)
		gap = MAX(0, (panel_width - columns) / (columns - 1));

	/* A panel narrower than one pixel per window overflows rather than
	 * handing out zero-width columns. */
	usable = MAX(columns, panel_width - gap * (columns - 1));
	base   = usable / columns;
	extra  = usable % columns;

	/* The leftover pixels go one each to the first columns, so the
	 * columns fill the panel instead of leaving a sliver at its edge. */
	x = panel_x;
	for (i = 0; i < (gint)index; i++)
		x += base + (i < extra ? 1 : 0) + gap;

	*out_x      = x;
	*out_y      = panel_y;
	*out_width  = base + ((gint)index < extra ? 1 : 0);
	*out_height = panel_height;
	return TRUE;
}
