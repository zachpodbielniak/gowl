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

#include "gowl-tag-mask.h"

G_DEFINE_BOXED_TYPE(GowlTagMask, gowl_tag_mask,
                    gowl_tag_mask_copy, gowl_tag_mask_free)

/**
 * gowl_tag_mask_new:
 * @mask: initial 32-bit bitmask value
 *
 * Allocates a new #GowlTagMask initialised with @mask.
 *
 * Returns: (transfer full): a newly allocated #GowlTagMask. Free with
 *          gowl_tag_mask_free().
 */
GowlTagMask *
gowl_tag_mask_new(guint32 mask)
{
	GowlTagMask *self;

	self = g_slice_new(GowlTagMask);
	self->mask = mask;

	return self;
}

/**
 * gowl_tag_mask_copy:
 * @self: (not nullable): a #GowlTagMask to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_tag_mask_free().
 */
GowlTagMask *
gowl_tag_mask_copy(const GowlTagMask *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_tag_mask_new(self->mask);
}

/**
 * gowl_tag_mask_free:
 * @self: (nullable): a #GowlTagMask to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_tag_mask_free(GowlTagMask *self)
{
	if (self != NULL) {
		g_slice_free(GowlTagMask, self);
	}
}

/**
 * gowl_tag_mask_set_tag:
 * @self: (not nullable): a #GowlTagMask
 * @tag: zero-based tag index (must be < %GOWL_MAX_TAGS)
 *
 * Sets the bit corresponding to @tag in the mask.
 */
void
gowl_tag_mask_set_tag(
	GowlTagMask *self,
	guint         tag
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(tag < GOWL_MAX_TAGS);

	self->mask |= GOWL_TAGMASK(tag);
}

/**
 * gowl_tag_mask_clear_tag:
 * @self: (not nullable): a #GowlTagMask
 * @tag: zero-based tag index (must be < %GOWL_MAX_TAGS)
 *
 * Clears the bit corresponding to @tag in the mask.
 */
void
gowl_tag_mask_clear_tag(
	GowlTagMask *self,
	guint         tag
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(tag < GOWL_MAX_TAGS);

	self->mask &= ~GOWL_TAGMASK(tag);
}

/**
 * gowl_tag_mask_toggle_tag:
 * @self: (not nullable): a #GowlTagMask
 * @tag: zero-based tag index (must be < %GOWL_MAX_TAGS)
 *
 * Toggles (flips) the bit corresponding to @tag in the mask.
 */
void
gowl_tag_mask_toggle_tag(
	GowlTagMask *self,
	guint         tag
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(tag < GOWL_MAX_TAGS);

	self->mask ^= GOWL_TAGMASK(tag);
}

/**
 * gowl_tag_mask_has_tag:
 * @self: (not nullable): a #GowlTagMask
 * @tag: zero-based tag index (must be < %GOWL_MAX_TAGS)
 *
 * Tests whether @tag is set in the mask.
 *
 * Returns: %TRUE if @tag is set, %FALSE otherwise.
 */
gboolean
gowl_tag_mask_has_tag(
	const GowlTagMask *self,
	guint               tag
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(tag < GOWL_MAX_TAGS, FALSE);

	return GOWL_TAG_IS_SET(self->mask, tag);
}

/**
 * gowl_tag_mask_is_empty:
 * @self: (not nullable): a #GowlTagMask
 *
 * Tests whether the mask has no tags set.
 *
 * Returns: %TRUE if no bits are set, %FALSE otherwise.
 */
gboolean
gowl_tag_mask_is_empty(const GowlTagMask *self)
{
	g_return_val_if_fail(self != NULL, TRUE);

	return (self->mask == 0);
}

/**
 * gowl_tag_mask_count_tags:
 * @self: (not nullable): a #GowlTagMask
 *
 * Counts the number of tags (set bits) in the mask using the
 * Hamming weight (popcount) algorithm.
 *
 * Returns: the number of set bits in the mask.
 */
guint
gowl_tag_mask_count_tags(const GowlTagMask *self)
{
	guint32 v;
	guint count;

	g_return_val_if_fail(self != NULL, 0);

	/* Brian Kernighan's popcount */
	v = self->mask;
	count = 0;
	while (v) {
		v &= v - 1;
		count++;
	}

	return count;
}
