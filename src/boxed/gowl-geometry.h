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

#ifndef GOWL_GEOMETRY_H
#define GOWL_GEOMETRY_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_GEOMETRY (gowl_geometry_get_type())

/**
 * GowlGeometry:
 * @x: Horizontal position in pixels.
 * @y: Vertical position in pixels.
 * @width: Width in pixels.
 * @height: Height in pixels.
 *
 * An axis-aligned rectangle used for surface and monitor geometry.
 */
struct _GowlGeometry {
	gint x;
	gint y;
	gint width;
	gint height;
};

GType          gowl_geometry_get_type (void) G_GNUC_CONST;

GowlGeometry * gowl_geometry_new     (gint                  x,
                                       gint                  y,
                                       gint                  width,
                                       gint                  height);

GowlGeometry * gowl_geometry_copy    (const GowlGeometry   *self);

void           gowl_geometry_free    (GowlGeometry          *self);

gboolean       gowl_geometry_equals  (const GowlGeometry   *a,
                                       const GowlGeometry   *b);

gboolean       gowl_geometry_contains(const GowlGeometry   *self,
                                       gint                  px,
                                       gint                  py);

G_END_DECLS

#endif /* GOWL_GEOMETRY_H */
