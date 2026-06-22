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

#ifndef GOWL_INPUT_ZONE_H
#define GOWL_INPUT_ZONE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_INPUT_ZONE (gowl_input_zone_get_type())

/**
 * GowlInputZone:
 * @width: zone width in layout pixels.
 * @height: zone height in layout pixels.
 * @x: zone left edge, layout-relative (may be negative).
 * @y: zone top edge, layout-relative (may be negative).
 * @output: (nullable): the source output name (e.g. "DP-1"); informational.
 *
 * A rectangular region of the captured desktop, mapping one-to-one to the
 * InputCapture portal's Zone tuple `(width u, height u, x i, y i)`.  This
 * is a plain value type (boxed) deliberately free of any wlroots struct,
 * so the same description crosses the input-capture state machine, the
 * private Wayland protocol, and the test suite.  Pointer barriers are
 * validated against the union of zones; see #GowlInputBarrier.
 */
typedef struct _GowlInputZone GowlInputZone;

struct _GowlInputZone {
	guint  width;
	guint  height;
	gint   x;
	gint   y;
	gchar *output;
};

GType           gowl_input_zone_get_type (void) G_GNUC_CONST;

GowlInputZone * gowl_input_zone_new      (guint        width,
                                          guint        height,
                                          gint         x,
                                          gint         y,
                                          const gchar *output);

GowlInputZone * gowl_input_zone_copy     (const GowlInputZone *self);

void            gowl_input_zone_free     (GowlInputZone       *self);

gboolean        gowl_input_zone_equals   (const GowlInputZone *a,
                                          const GowlInputZone *b);

gboolean        gowl_input_zone_contains_point
                                         (const GowlInputZone *self,
                                          gint                 x,
                                          gint                 y);

G_END_DECLS

#endif /* GOWL_INPUT_ZONE_H */
