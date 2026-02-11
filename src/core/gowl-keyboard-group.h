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

#ifndef GOWL_KEYBOARD_GROUP_H
#define GOWL_KEYBOARD_GROUP_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_KEYBOARD_GROUP (gowl_keyboard_group_get_type())

G_DECLARE_FINAL_TYPE(GowlKeyboardGroup, gowl_keyboard_group, GOWL, KEYBOARD_GROUP, GObject)

/**
 * gowl_keyboard_group_new:
 *
 * Creates a new #GowlKeyboardGroup.
 *
 * Returns: (transfer full): a newly created #GowlKeyboardGroup
 */
GowlKeyboardGroup *gowl_keyboard_group_new              (void);

gint               gowl_keyboard_group_get_repeat_rate   (GowlKeyboardGroup *self);
void               gowl_keyboard_group_set_repeat_rate   (GowlKeyboardGroup *self,
                                                          gint               rate);

gint               gowl_keyboard_group_get_repeat_delay  (GowlKeyboardGroup *self);
void               gowl_keyboard_group_set_repeat_delay  (GowlKeyboardGroup *self,
                                                          gint               delay);

G_END_DECLS

#endif /* GOWL_KEYBOARD_GROUP_H */
