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

#ifndef GOWLBAR_TAG_WIDGET_H
#define GOWLBAR_TAG_WIDGET_H

#include "gowlbar-widget.h"
#include "gowlbar-config.h"

G_BEGIN_DECLS

#define GOWLBAR_TYPE_TAG_WIDGET (gowlbar_tag_widget_get_type())

G_DECLARE_FINAL_TYPE(GowlbarTagWidget, gowlbar_tag_widget,
                     GOWLBAR, TAG_WIDGET, GowlbarWidget)

/**
 * GOWLBAR_TAG_COUNT:
 *
 * Number of tags supported (matching dwl-style 9-tag model).
 */
#define GOWLBAR_TAG_COUNT (9)

/**
 * gowlbar_tag_widget_new:
 * @config: (transfer none): the bar configuration for colours
 *
 * Creates a new tag indicator widget.
 *
 * Returns: (transfer full): a new #GowlbarTagWidget
 */
GowlbarTagWidget *gowlbar_tag_widget_new(GowlbarConfig *config);

/**
 * gowlbar_tag_widget_set_state:
 * @self: the tag widget
 * @active_mask: bitmask of currently active (selected) tags
 * @occupied_mask: bitmask of tags with clients
 * @urgent_mask: bitmask of tags with urgent clients
 * @sel_tags: bitmask of tags selected on this output
 *
 * Updates the tag state.  Triggers a redraw on next render call.
 */
void gowlbar_tag_widget_set_state(GowlbarTagWidget *self,
                                   guint32 active_mask,
                                   guint32 occupied_mask,
                                   guint32 urgent_mask,
                                   guint32 sel_tags);

G_END_DECLS

#endif /* GOWLBAR_TAG_WIDGET_H */
