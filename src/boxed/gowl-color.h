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

#ifndef GOWL_COLOR_H
#define GOWL_COLOR_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_COLOR (gowl_color_get_type())

/**
 * GowlColor:
 * @r: Red channel (0.0 .. 1.0).
 * @g: Green channel (0.0 .. 1.0).
 * @b: Blue channel (0.0 .. 1.0).
 * @a: Alpha channel (0.0 .. 1.0).
 *
 * An RGBA colour with floating-point channels normalised to [0, 1].
 */
struct _GowlColor {
	gdouble r;
	gdouble g;
	gdouble b;
	gdouble a;
};

GType       gowl_color_get_type    (void) G_GNUC_CONST;

GowlColor * gowl_color_new        (gdouble            r,
                                    gdouble            g,
                                    gdouble            b,
                                    gdouble            a);

GowlColor * gowl_color_new_from_hex(const gchar       *hex);

GowlColor * gowl_color_copy       (const GowlColor   *self);

void        gowl_color_free       (GowlColor          *self);

gchar *     gowl_color_to_hex     (const GowlColor   *self);

G_END_DECLS

#endif /* GOWL_COLOR_H */
