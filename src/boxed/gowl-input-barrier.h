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

#ifndef GOWL_INPUT_BARRIER_H
#define GOWL_INPUT_BARRIER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_INPUT_BARRIER (gowl_input_barrier_get_type())

/**
 * GowlInputBarrierOrientation:
 * @GOWL_INPUT_BARRIER_INVALID: neither horizontal nor vertical (diagonal
 *   or zero-length) -- never installed.
 * @GOWL_INPUT_BARRIER_HORIZONTAL: a horizontal segment (y1 == y2); blocks
 *   vertical pointer motion across it.
 * @GOWL_INPUT_BARRIER_VERTICAL: a vertical segment (x1 == x2); blocks
 *   horizontal pointer motion across it.
 *
 * The geometric kind of a pointer barrier, derived from its endpoints by
 * gowl_input_barrier_classify().  The InputCapture portal spec requires a
 * barrier be axis-aligned: horizontal barriers have y1 == y2, vertical
 * barriers have x1 == x2.
 */
typedef enum {
	GOWL_INPUT_BARRIER_INVALID = 0,
	GOWL_INPUT_BARRIER_HORIZONTAL,
	GOWL_INPUT_BARRIER_VERTICAL
} GowlInputBarrierOrientation;

/**
 * GowlInputBarrier:
 * @id: caller-assigned non-zero identifier (0 is invalid per the portal
 *   spec); reported back in the Activated signal.
 * @x1: first endpoint x, layout-relative.
 * @y1: first endpoint y, layout-relative.
 * @x2: second endpoint x, layout-relative.
 * @y2: second endpoint y, layout-relative.
 *
 * A pointer barrier: an axis-aligned line segment on the outside boundary
 * of the zone union that triggers input capture when the cursor crosses
 * it.  Maps to the InputCapture portal barrier `{id u, position iiii}`.
 * This is a plain value type (boxed) carrying no wlroots state; the
 * orientation is computed on demand via gowl_input_barrier_classify().
 */
typedef struct _GowlInputBarrier GowlInputBarrier;

struct _GowlInputBarrier {
	guint id;
	gint  x1;
	gint  y1;
	gint  x2;
	gint  y2;
};

GType              gowl_input_barrier_get_type (void) G_GNUC_CONST;

GowlInputBarrier * gowl_input_barrier_new      (guint id,
                                                gint  x1,
                                                gint  y1,
                                                gint  x2,
                                                gint  y2);

GowlInputBarrier * gowl_input_barrier_copy     (const GowlInputBarrier *self);

void               gowl_input_barrier_free     (GowlInputBarrier       *self);

gboolean           gowl_input_barrier_equals   (const GowlInputBarrier *a,
                                                const GowlInputBarrier *b);

GowlInputBarrierOrientation gowl_input_barrier_classify
                                               (const GowlInputBarrier *self);

G_END_DECLS

#endif /* GOWL_INPUT_BARRIER_H */
