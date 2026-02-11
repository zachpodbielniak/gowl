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

#ifndef GOWL_BORDER_SPEC_H
#define GOWL_BORDER_SPEC_H

#include "gowl-types.h"
#include "boxed/gowl-color.h"

G_BEGIN_DECLS

#define GOWL_TYPE_BORDER_SPEC (gowl_border_spec_get_type())

/**
 * GowlBorderSpec:
 * @width: Border thickness in pixels.
 * @focus_color: Colour drawn around the focused client.
 * @unfocus_color: Colour drawn around unfocused clients.
 * @urgent_color: Colour drawn around clients requesting urgent attention.
 *
 * Describes the border decoration applied to compositor clients.
 */
struct _GowlBorderSpec {
	gint      width;
	GowlColor focus_color;
	GowlColor unfocus_color;
	GowlColor urgent_color;
};

GType           gowl_border_spec_get_type (void) G_GNUC_CONST;

GowlBorderSpec *gowl_border_spec_new     (gint                    width,
                                           const GowlColor        *focus_color,
                                           const GowlColor        *unfocus_color,
                                           const GowlColor        *urgent_color);

GowlBorderSpec *gowl_border_spec_copy    (const GowlBorderSpec   *self);

void            gowl_border_spec_free    (GowlBorderSpec          *self);

G_END_DECLS

#endif /* GOWL_BORDER_SPEC_H */
