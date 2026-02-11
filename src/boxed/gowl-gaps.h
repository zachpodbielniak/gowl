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

#ifndef GOWL_GAPS_H
#define GOWL_GAPS_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_GAPS (gowl_gaps_get_type())

/**
 * GowlGaps:
 * @inner_h: Horizontal gap between adjacent tiled clients (pixels).
 * @inner_v: Vertical gap between adjacent tiled clients (pixels).
 * @outer_h: Horizontal gap between clients and the monitor edge (pixels).
 * @outer_v: Vertical gap between clients and the monitor edge (pixels).
 *
 * Gap configuration for the tiling layout engine.
 */
struct _GowlGaps {
	gint inner_h;
	gint inner_v;
	gint outer_h;
	gint outer_v;
};

GType      gowl_gaps_get_type (void) G_GNUC_CONST;

GowlGaps * gowl_gaps_new     (gint               inner_h,
                               gint               inner_v,
                               gint               outer_h,
                               gint               outer_v);

GowlGaps * gowl_gaps_copy    (const GowlGaps    *self);

void       gowl_gaps_free    (GowlGaps           *self);

gboolean   gowl_gaps_equals  (const GowlGaps    *a,
                               const GowlGaps    *b);

G_END_DECLS

#endif /* GOWL_GAPS_H */
