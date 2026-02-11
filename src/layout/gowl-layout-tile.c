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

#include "gowl-layout-tile.h"

/*
 * gowl_layout_tile:
 *
 * Master/stack tiling layout. Master windows go on the left side,
 * stack windows go on the right. If there are fewer clients than
 * nmaster, all clients go in the master area.
 *
 * positions array: for each client i, positions[i*4+0]=x, [i*4+1]=y,
 * [i*4+2]=w, [i*4+3]=h
 */
void
gowl_layout_tile(
	gint    n,
	gint    nmaster,
	gdouble mfact,
	gint    x,
	gint    y,
	gint    w,
	gint    h,
	gint   *positions
){
	gint i;
	gint master_w;
	gint my, sy;     /* master y, stack y running counters */
	gint mh, sh;     /* per-client height in master, stack */
	gint nstack;

	if (n <= 0)
		return;

	g_return_if_fail(positions != NULL);

	/* clamp mfact */
	if (mfact < 0.05)
		mfact = 0.05;
	if (mfact > 0.95)
		mfact = 0.95;

	/* clamp nmaster */
	if (nmaster < 0)
		nmaster = 0;

	nstack = n - nmaster;
	if (nstack < 0)
		nstack = 0;

	/* Calculate master width */
	if (n <= nmaster) {
		/* All clients in master area - use full width */
		master_w = w;
	} else {
		master_w = (gint)(w * mfact);
	}

	/* Layout master windows */
	my = y;
	for (i = 0; i < n && i < nmaster; i++) {
		gint remaining;
		gint clients_left;

		remaining = h - (my - y);
		clients_left = (nmaster < n ? nmaster : n) - i;
		mh = remaining / clients_left;

		positions[i * 4 + 0] = x;
		positions[i * 4 + 1] = my;
		positions[i * 4 + 2] = master_w;
		positions[i * 4 + 3] = mh;

		my += mh;
	}

	/* Layout stack windows */
	sy = y;
	for (i = nmaster; i < n; i++) {
		gint remaining;
		gint clients_left;

		remaining = h - (sy - y);
		clients_left = n - i;
		sh = remaining / clients_left;

		positions[i * 4 + 0] = x + master_w;
		positions[i * 4 + 1] = sy;
		positions[i * 4 + 2] = w - master_w;
		positions[i * 4 + 3] = sh;

		sy += sh;
	}
}

const gchar *
gowl_layout_tile_symbol(void)
{
	return "[]=";
}
