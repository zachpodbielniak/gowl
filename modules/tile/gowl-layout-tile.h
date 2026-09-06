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

#ifndef GOWL_LAYOUT_TILE_H
#define GOWL_LAYOUT_TILE_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * gowl_layout_tile:
 * @n: total number of clients to arrange
 * @nmaster: number of master windows
 * @mfact: master area ratio (0.0 to 1.0)
 * @x: area x offset
 * @y: area y offset
 * @w: area width
 * @h: area height
 * @positions: (out): array of 4*n gints (x,y,w,h for each client)
 *
 * Compute the tiled layout positions for n clients in the given area.
 * Master windows go on the left, stack on the right.
 */
void
gowl_layout_tile(
	gint   n,
	gint   nmaster,
	gdouble mfact,
	gint   x,
	gint   y,
	gint   w,
	gint   h,
	gint  *positions
);

/**
 * gowl_layout_tile_symbol:
 *
 * Returns: (transfer none): the layout symbol string for tile layout
 */
const gchar *
gowl_layout_tile_symbol(void);

G_END_DECLS

#endif /* GOWL_LAYOUT_TILE_H */
