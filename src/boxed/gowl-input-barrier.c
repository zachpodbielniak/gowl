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

#include "gowl-input-barrier.h"

G_DEFINE_BOXED_TYPE(GowlInputBarrier, gowl_input_barrier,
                    gowl_input_barrier_copy, gowl_input_barrier_free)

/**
 * gowl_input_barrier_new:
 * @id: caller-assigned identifier (non-zero is required to be valid)
 * @x1: first endpoint x
 * @y1: first endpoint y
 * @x2: second endpoint x
 * @y2: second endpoint y
 *
 * Allocates a new #GowlInputBarrier.  The endpoints are stored verbatim;
 * validity (axis-alignment, non-zero id, on-edge placement) is decided
 * later by the input-capture state machine, not here.
 *
 * Returns: (transfer full): a new #GowlInputBarrier. Free with
 *          gowl_input_barrier_free().
 */
GowlInputBarrier *
gowl_input_barrier_new(
	guint id,
	gint  x1,
	gint  y1,
	gint  x2,
	gint  y2
){
	GowlInputBarrier *self;

	self = g_slice_new0(GowlInputBarrier);
	self->id = id;
	self->x1 = x1;
	self->y1 = y1;
	self->x2 = x2;
	self->y2 = y2;

	return self;
}

/**
 * gowl_input_barrier_copy:
 * @self: (not nullable): a #GowlInputBarrier to copy
 *
 * Creates a copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy. Free with
 *          gowl_input_barrier_free().
 */
GowlInputBarrier *
gowl_input_barrier_copy(const GowlInputBarrier *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_input_barrier_new(self->id, self->x1, self->y1,
	                              self->x2, self->y2);
}

/**
 * gowl_input_barrier_free:
 * @self: (nullable): a #GowlInputBarrier to free
 *
 * Releases @self. Safe to call with %NULL.
 */
void
gowl_input_barrier_free(GowlInputBarrier *self)
{
	if (self != NULL)
		g_slice_free(GowlInputBarrier, self);
}

/**
 * gowl_input_barrier_equals:
 * @a: (not nullable): first barrier
 * @b: (not nullable): second barrier
 *
 * Tests whether two barriers have the same id and endpoints.
 *
 * Returns: %TRUE if equal, %FALSE otherwise.
 */
gboolean
gowl_input_barrier_equals(
	const GowlInputBarrier *a,
	const GowlInputBarrier *b
){
	g_return_val_if_fail(a != NULL, FALSE);
	g_return_val_if_fail(b != NULL, FALSE);

	return a->id == b->id
	       && a->x1 == b->x1 && a->y1 == b->y1
	       && a->x2 == b->x2 && a->y2 == b->y2;
}

/**
 * gowl_input_barrier_classify:
 * @self: (not nullable): a #GowlInputBarrier
 *
 * Determines the barrier's orientation from its endpoints, per the
 * InputCapture portal rules: a horizontal barrier has y1 == y2 (and
 * non-zero horizontal extent), a vertical barrier has x1 == x2 (and
 * non-zero vertical extent).  A diagonal segment, a zero-length point, or
 * a barrier with id 0 is %GOWL_INPUT_BARRIER_INVALID and must not be
 * installed.
 *
 * Returns: the #GowlInputBarrierOrientation.
 */
GowlInputBarrierOrientation
gowl_input_barrier_classify(const GowlInputBarrier *self)
{
	g_return_val_if_fail(self != NULL, GOWL_INPUT_BARRIER_INVALID);

	/* id 0 is reserved/invalid per the portal spec. */
	if (self->id == 0)
		return GOWL_INPUT_BARRIER_INVALID;

	/* Horizontal: same y, differing x. */
	if (self->y1 == self->y2 && self->x1 != self->x2)
		return GOWL_INPUT_BARRIER_HORIZONTAL;

	/* Vertical: same x, differing y. */
	if (self->x1 == self->x2 && self->y1 != self->y2)
		return GOWL_INPUT_BARRIER_VERTICAL;

	/* Diagonal or zero-length. */
	return GOWL_INPUT_BARRIER_INVALID;
}
