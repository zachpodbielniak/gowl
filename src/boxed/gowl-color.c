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

#include "gowl-color.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

G_DEFINE_BOXED_TYPE(GowlColor, gowl_color,
                    gowl_color_copy, gowl_color_free)

/**
 * gowl_color_new:
 * @r: red channel (0.0 .. 1.0)
 * @g: green channel (0.0 .. 1.0)
 * @b: blue channel (0.0 .. 1.0)
 * @a: alpha channel (0.0 .. 1.0)
 *
 * Allocates a new #GowlColor with the given channel values.
 *
 * Returns: (transfer full): a newly allocated #GowlColor. Free with
 *          gowl_color_free().
 */
GowlColor *
gowl_color_new(
	gdouble r,
	gdouble g,
	gdouble b,
	gdouble a
){
	GowlColor *self;

	self = g_slice_new(GowlColor);
	self->r = CLAMP(r, 0.0, 1.0);
	self->g = CLAMP(g, 0.0, 1.0);
	self->b = CLAMP(b, 0.0, 1.0);
	self->a = CLAMP(a, 0.0, 1.0);

	return self;
}

/**
 * gowl_color_new_from_hex:
 * @hex: a hex colour string in the form "#rrggbb" or "#rrggbbaa"
 *
 * Parses a CSS-style hex colour string into a #GowlColor.  If @hex is
 * %NULL, malformed, or has an unexpected length, the function returns
 * opaque black as a fallback.
 *
 * Returns: (transfer full): a newly allocated #GowlColor. Free with
 *          gowl_color_free().
 */
GowlColor *
gowl_color_new_from_hex(const gchar *hex)
{
	guint rv, gv, bv, av;
	gsize len;

	/* Default to opaque black on bad input */
	rv = 0;
	gv = 0;
	bv = 0;
	av = 255;

	if (hex == NULL) {
		return gowl_color_new(0.0, 0.0, 0.0, 1.0);
	}

	/* Skip leading '#' if present */
	if (hex[0] == '#') {
		hex++;
	}

	len = strlen(hex);

	if (len == 6) {
		/* Parse #rrggbb */
		if (sscanf(hex, "%02x%02x%02x", &rv, &gv, &bv) != 3) {
			rv = 0;
			gv = 0;
			bv = 0;
		}
		av = 255;
	} else if (len == 8) {
		/* Parse #rrggbbaa */
		if (sscanf(hex, "%02x%02x%02x%02x", &rv, &gv, &bv, &av) != 4) {
			rv = 0;
			gv = 0;
			bv = 0;
			av = 255;
		}
	}

	return gowl_color_new((gdouble)rv / 255.0,
	                       (gdouble)gv / 255.0,
	                       (gdouble)bv / 255.0,
	                       (gdouble)av / 255.0);
}

/**
 * gowl_color_copy:
 * @self: (not nullable): a #GowlColor to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_color_free().
 */
GowlColor *
gowl_color_copy(const GowlColor *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_color_new(self->r, self->g, self->b, self->a);
}

/**
 * gowl_color_free:
 * @self: (nullable): a #GowlColor to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_color_free(GowlColor *self)
{
	if (self != NULL) {
		g_slice_free(GowlColor, self);
	}
}

/**
 * gowl_color_to_hex:
 * @self: (not nullable): a #GowlColor
 *
 * Converts the colour to a CSS-style hex string.  If alpha is fully opaque
 * (1.0) the result is "#rrggbb"; otherwise "#rrggbbaa".
 *
 * Returns: (transfer full): a newly allocated hex string. Free with g_free().
 */
gchar *
gowl_color_to_hex(const GowlColor *self)
{
	guint rv, gv, bv, av;

	g_return_val_if_fail(self != NULL, NULL);

	rv = (guint)(self->r * 255.0 + 0.5);
	gv = (guint)(self->g * 255.0 + 0.5);
	bv = (guint)(self->b * 255.0 + 0.5);
	av = (guint)(self->a * 255.0 + 0.5);

	if (av == 255) {
		return g_strdup_printf("#%02x%02x%02x", rv, gv, bv);
	}

	return g_strdup_printf("#%02x%02x%02x%02x", rv, gv, bv, av);
}
