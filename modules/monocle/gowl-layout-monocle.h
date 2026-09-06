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

#ifndef GOWL_LAYOUT_MONOCLE_H
#define GOWL_LAYOUT_MONOCLE_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * gowl_layout_monocle:
 * @n: total number of clients
 * @x: area x offset
 * @y: area y offset
 * @w: area width
 * @h: area height
 * @positions: (out): array of 4*n gints (x,y,w,h for each client)
 *
 * Monocle layout: all clients get the full area.
 */
void
gowl_layout_monocle(
	gint  n,
	gint  x,
	gint  y,
	gint  w,
	gint  h,
	gint *positions
);

/**
 * gowl_layout_monocle_symbol:
 *
 * Returns: (transfer none): the layout symbol string for monocle layout
 */
const gchar *
gowl_layout_monocle_symbol(void);

G_END_DECLS

#endif /* GOWL_LAYOUT_MONOCLE_H */
