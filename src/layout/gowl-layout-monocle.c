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

#include "gowl-layout-monocle.h"

/*
 * gowl_layout_monocle:
 *
 * Monocle layout: every client occupies the full area.
 */
void
gowl_layout_monocle(
	gint  n,
	gint  x,
	gint  y,
	gint  w,
	gint  h,
	gint *positions
){
	gint i;

	if (n <= 0)
		return;

	g_return_if_fail(positions != NULL);

	for (i = 0; i < n; i++) {
		positions[i * 4 + 0] = x;
		positions[i * 4 + 1] = y;
		positions[i * 4 + 2] = w;
		positions[i * 4 + 3] = h;
	}
}

const gchar *
gowl_layout_monocle_symbol(void)
{
	return "[M]";
}
