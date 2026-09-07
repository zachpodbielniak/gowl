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

#ifndef GOWL_BAR_LAYOUT_H
#define GOWL_BAR_LAYOUT_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlBarRegion:
 * @GOWL_BAR_REGION_LEFT: packed from the bar's left edge, in order.
 * @GOWL_BAR_REGION_CENTER: centred as a group, or about an anchor.
 * @GOWL_BAR_REGION_RIGHT: packed from the bar's right edge, in order,
 *   so the first entry ends up furthest right.
 *
 * Which of the bar's three regions an item belongs to.
 */
typedef enum {
	GOWL_BAR_REGION_LEFT = 0,
	GOWL_BAR_REGION_CENTER,
	GOWL_BAR_REGION_RIGHT,
	GOWL_BAR_REGION_COUNT
} GowlBarRegion;

/**
 * GowlBarSlot:
 * @x: the item's left edge, or 0 when hidden
 * @width: the item's width, or 0 when it did not fit
 * @visible: %FALSE when the item was dropped for want of room
 *
 * Where one bar item landed.
 */
typedef struct {
	gint     x;
	gint     width;
	gboolean visible;
} GowlBarSlot;

/**
 * gowl_bar_layout_run:
 * @bar_width: the bar's width in pixels
 * @pad: padding kept clear at each end of the bar
 * @gap: space between adjacent items
 * @widths: (array length=n_items): each item's natural width
 * @regions: (array length=n_items): each item's region
 * @n_items: how many items
 * @anchor: the index of the centre region's anchor item, or -1
 * @slots: (out caller-allocates) (array length=n_items): where each
 *   item landed
 *
 * Lays the bar out in one pass.
 *
 * With @anchor set, that item is centred on the bar's midpoint and the
 * rest of the centre region packs around it in order --- so a clock
 * stays dead centre no matter what appears beside it, which is the
 * whole point of an anchor.  With @anchor at -1 the centre region is
 * centred as a block.
 *
 * The centre region is then shifted, not clipped, if it would collide
 * with the left or right region: a bar that overlaps its own widgets
 * looks broken, while one whose centre has drifted a little looks
 * crowded, which is the truth.  Items that still cannot fit are marked
 * invisible from the centre outwards.
 *
 * Returns: the number of items that ended up visible
 */
gint gowl_bar_layout_run (gint                 bar_width,
                           gint                 pad,
                           gint                 gap,
                           const gint          *widths,
                           const GowlBarRegion *regions,
                           gint                 n_items,
                           gint                 anchor,
                           GowlBarSlot         *slots);

/**
 * gowl_bar_layout_hit:
 * @slots: (array length=n_items): a laid-out bar
 * @n_items: how many items
 * @x: an x coordinate in bar-local pixels
 *
 * Returns: the index of the item under @x, or -1
 */
gint gowl_bar_layout_hit (const GowlBarSlot *slots, gint n_items, gint x);

/**
 * gowl_bar_layout_region_from_string:
 * @name: `left', `center'/`centre', or `right'
 * @out: (out): the parsed region
 *
 * Returns: %TRUE when @name named a region
 */
gboolean gowl_bar_layout_region_from_string (const gchar   *name,
                                              GowlBarRegion *out);

/**
 * gowl_bar_layout_region_name:
 * @region: a region
 *
 * Returns: (transfer none): its canonical name
 */
const gchar *gowl_bar_layout_region_name (GowlBarRegion region);

G_END_DECLS

#endif /* GOWL_BAR_LAYOUT_H */
