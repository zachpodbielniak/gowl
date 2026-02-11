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

#ifndef GOWL_TAG_MASK_H
#define GOWL_TAG_MASK_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_TAG_MASK (gowl_tag_mask_get_type())

/**
 * GowlTagMask:
 * @mask: A 32-bit bitmask where bit N represents tag N (0-based).
 *
 * Compact bitmask for tracking which tags are active or assigned.
 */
struct _GowlTagMask {
	guint32 mask;
};

GType         gowl_tag_mask_get_type  (void) G_GNUC_CONST;

GowlTagMask * gowl_tag_mask_new      (guint32              mask);

GowlTagMask * gowl_tag_mask_copy     (const GowlTagMask   *self);

void          gowl_tag_mask_free     (GowlTagMask          *self);

void          gowl_tag_mask_set_tag  (GowlTagMask          *self,
                                       guint                 tag);

void          gowl_tag_mask_clear_tag(GowlTagMask          *self,
                                       guint                 tag);

void          gowl_tag_mask_toggle_tag(GowlTagMask         *self,
                                        guint                tag);

gboolean      gowl_tag_mask_has_tag  (const GowlTagMask   *self,
                                       guint                 tag);

gboolean      gowl_tag_mask_is_empty (const GowlTagMask   *self);

guint         gowl_tag_mask_count_tags(const GowlTagMask   *self);

G_END_DECLS

#endif /* GOWL_TAG_MASK_H */
