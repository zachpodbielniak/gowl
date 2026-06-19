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

#ifndef GOWL_BAR_H
#define GOWL_BAR_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_BAR (gowl_bar_get_type())

G_DECLARE_FINAL_TYPE(GowlBar, gowl_bar, GOWL, BAR, GObject)

GowlBar   *gowl_bar_new          (void);

gint        gowl_bar_get_height   (GowlBar  *self);
void        gowl_bar_set_height   (GowlBar  *self,
                                   gint      height);

gboolean    gowl_bar_is_visible   (GowlBar  *self);
void        gowl_bar_set_visible  (GowlBar  *self,
                                   gboolean  visible);

/**
 * gowl_bar_tag_hit:
 * @x: x coordinate relative to the monitor's left edge
 * @y: y coordinate relative to the monitor's top edge
 * @monitor_height: the monitor's height in pixels
 * @top_height: height of the top tag bar, or 0 if it has no tag row
 * @bottom_height: height of the bottom tag bar, or 0 if none
 * @pad: left padding before the first tag box
 * @tag_count: number of tag boxes (each @top_height/@bottom_height wide)
 *
 * Pure geometry hit-test for the left-edge tag indicator.  Returns the
 * 0-based tag index whose box contains (@x, @y), or -1 when the point
 * is outside every tag box (wrong vertical band, in the left padding,
 * or past the last box).  Stateless so it can be unit-tested directly;
 * the bar module supplies the per-slot heights.
 *
 * Returns: the 0-based tag index, or -1
 */
gint        gowl_bar_tag_hit      (gint x, gint y, gint monitor_height,
                                   gint top_height, gint bottom_height,
                                   gint pad, gint tag_count);

G_END_DECLS

#endif /* GOWL_BAR_H */
