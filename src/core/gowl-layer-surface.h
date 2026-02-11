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

#ifndef GOWL_LAYER_SURFACE_H
#define GOWL_LAYER_SURFACE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_LAYER_SURFACE (gowl_layer_surface_get_type())

G_DECLARE_FINAL_TYPE(GowlLayerSurface, gowl_layer_surface, GOWL, LAYER_SURFACE, GObject)

GowlLayerSurface *gowl_layer_surface_new        (void);

gint               gowl_layer_surface_get_layer   (GowlLayerSurface *self);
gboolean           gowl_layer_surface_is_mapped   (GowlLayerSurface *self);

G_END_DECLS

#endif /* GOWL_LAYER_SURFACE_H */
