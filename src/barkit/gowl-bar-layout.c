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

#include "barkit/gowl-bar-layout.h"

#include <string.h>

/**
 * SECTION:gowl-bar-layout
 * @title: Bar layout
 * @short_description: three-region placement with a centre anchor
 *
 * Pure geometry, no drawing and no GObject, so the awkward cases --- a
 * centre group wider than the space between the other two, an anchor
 * pushed off the bar by a long window title --- are unit-testable
 * rather than something you find by squinting at a screenshot.
 */

/**
 * gowl_bar_layout_region_from_string:
 * @name: a region name
 * @out: (out): the parsed region
 *
 * Returns: %TRUE when @name named a region
 */
gboolean
gowl_bar_layout_region_from_string(const gchar *name, GowlBarRegion *out)
{
	if (name == NULL)
		return FALSE;

	if (g_ascii_strcasecmp(name, "left") == 0) {
		if (out != NULL)
			*out = GOWL_BAR_REGION_LEFT;
		return TRUE;
	}
	if (g_ascii_strcasecmp(name, "center") == 0 ||
	    g_ascii_strcasecmp(name, "centre") == 0 ||
	    g_ascii_strcasecmp(name, "middle") == 0) {
		if (out != NULL)
			*out = GOWL_BAR_REGION_CENTER;
		return TRUE;
	}
	if (g_ascii_strcasecmp(name, "right") == 0) {
		if (out != NULL)
			*out = GOWL_BAR_REGION_RIGHT;
		return TRUE;
	}
	return FALSE;
}

/**
 * gowl_bar_layout_region_name:
 * @region: a region
 *
 * Returns: (transfer none): its canonical name
 */
const gchar *
gowl_bar_layout_region_name(GowlBarRegion region)
{
	switch (region) {
	case GOWL_BAR_REGION_LEFT:   return "left";
	case GOWL_BAR_REGION_CENTER: return "center";
	case GOWL_BAR_REGION_RIGHT:  return "right";
	default:                     return "right";
	}
}

/* Total width of every item in @region, including the gaps between
   them.  Zero-width items are skipped entirely --- a plugin whose
   text is currently empty must not leave a gap behind. */
static gint
region_extent(const gint *widths, const GowlBarRegion *regions,
              gint n_items, GowlBarRegion region, gint gap)
{
	gint total, count, i;

	total = 0;
	count = 0;
	for (i = 0; i < n_items; i++) {
		if (regions[i] != region || widths[i] <= 0)
			continue;
		total += widths[i];
		count++;
	}
	if (count > 1)
		total += gap * (count - 1);
	return total;
}

/**
 * gowl_bar_layout_run:
 * @bar_width: the bar's width in pixels
 * @pad: padding at each end
 * @gap: space between items
 * @widths: (array length=n_items): natural widths
 * @regions: (array length=n_items): regions
 * @n_items: how many items
 * @anchor: the centre anchor's index, or -1
 * @slots: (out caller-allocates) (array length=n_items): results
 *
 * Returns: the number of visible items
 */
gint
gowl_bar_layout_run(gint bar_width, gint pad, gint gap,
                    const gint *widths, const GowlBarRegion *regions,
                    gint n_items, gint anchor, GowlBarSlot *slots)
{
	gint left_end, right_start, center_w, center_x;
	gint mid, avail_start, avail_end;
	gint x, i, visible;

	g_return_val_if_fail(widths != NULL, 0);
	g_return_val_if_fail(regions != NULL, 0);
	g_return_val_if_fail(slots != NULL, 0);

	for (i = 0; i < n_items; i++) {
		slots[i].x       = 0;
		slots[i].width   = 0;
		slots[i].visible = FALSE;
	}
	if (n_items <= 0 || bar_width <= 0)
		return 0;

	visible = 0;

	/* Left region, packed rightwards from the padding. */
	x = pad;
	for (i = 0; i < n_items; i++) {
		if (regions[i] != GOWL_BAR_REGION_LEFT || widths[i] <= 0)
			continue;
		slots[i].x       = x;
		slots[i].width   = widths[i];
		slots[i].visible = TRUE;
		visible++;
		x += widths[i] + gap;
	}
	left_end = (x > pad) ? (x - gap) : pad;

	/* Right region, packed leftwards from the far padding.  Walking
	   the list forwards while moving left means the first entry in
	   the config ends up furthest right, which is how every dwm-
	   descended bar reads its status list. */
	x = bar_width - pad;
	for (i = 0; i < n_items; i++) {
		if (regions[i] != GOWL_BAR_REGION_RIGHT || widths[i] <= 0)
			continue;
		x -= widths[i];
		slots[i].x       = x;
		slots[i].width   = widths[i];
		slots[i].visible = TRUE;
		visible++;
		x -= gap;
	}
	right_start = (x < bar_width - pad) ? (x + gap) : (bar_width - pad);

	center_w = region_extent(widths, regions, n_items,
	                         GOWL_BAR_REGION_CENTER, gap);
	if (center_w <= 0)
		return visible;

	mid         = bar_width / 2;
	avail_start = left_end + gap;
	avail_end   = right_start - gap;

	if (anchor >= 0 && anchor < n_items &&
	    regions[anchor] == GOWL_BAR_REGION_CENTER &&
	    widths[anchor] > 0) {
		gint before, after, anchor_x;

		/* Everything before the anchor in config order sits to its
		   left, everything after to its right; the anchor itself is
		   centred on the bar.  center_x then names the left edge of
		   the whole group so the clamp below can treat both cases
		   identically. */
		before = 0;
		after  = 0;
		for (i = 0; i < n_items; i++) {
			if (regions[i] != GOWL_BAR_REGION_CENTER ||
			    widths[i] <= 0 || i == anchor)
				continue;
			if (i < anchor)
				before += widths[i] + gap;
			else
				after += widths[i] + gap;
		}
		anchor_x = mid - widths[anchor] / 2;
		center_x = anchor_x - before;
		center_w = before + widths[anchor] + after;
	} else {
		center_x = mid - center_w / 2;
	}

	/* Shift, do not clip.  A centre group that has been nudged aside
	   still shows every widget; one that has been clipped silently
	   loses the tail of the list. */
	if (center_x + center_w > avail_end)
		center_x = avail_end - center_w;
	if (center_x < avail_start)
		center_x = avail_start;
	if (center_x < pad)
		center_x = pad;

	x = center_x;
	for (i = 0; i < n_items; i++) {
		if (regions[i] != GOWL_BAR_REGION_CENTER || widths[i] <= 0)
			continue;
		if (x + widths[i] > bar_width - pad) {
			/* Genuinely out of room: leave it hidden rather than
			   drawing it over the right region. */
			continue;
		}
		slots[i].x       = x;
		slots[i].width   = widths[i];
		slots[i].visible = TRUE;
		visible++;
		x += widths[i] + gap;
	}

	return visible;
}

/**
 * gowl_bar_layout_hit:
 * @slots: (array length=n_items): a laid-out bar
 * @n_items: how many items
 * @x: a bar-local x coordinate
 *
 * Returns: the index of the item under @x, or -1
 */
gint
gowl_bar_layout_hit(const GowlBarSlot *slots, gint n_items, gint x)
{
	gint i;

	if (slots == NULL)
		return -1;

	for (i = 0; i < n_items; i++) {
		if (!slots[i].visible || slots[i].width <= 0)
			continue;
		if (x >= slots[i].x && x < slots[i].x + slots[i].width)
			return i;
	}
	return -1;
}
