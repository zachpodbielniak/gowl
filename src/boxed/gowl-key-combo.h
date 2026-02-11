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

#ifndef GOWL_KEY_COMBO_H
#define GOWL_KEY_COMBO_H

#include "gowl-types.h"
#include "gowl-enums.h"

G_BEGIN_DECLS

#define GOWL_TYPE_KEY_COMBO (gowl_key_combo_get_type())

/**
 * GowlKeyCombo:
 * @modifiers: Bitmask of #GowlKeyMod flags active for this combination.
 * @keysym: XKB keysym value for the key.
 *
 * A modifier + keysym pair representing a keyboard shortcut.
 */
struct _GowlKeyCombo {
	guint modifiers;
	guint keysym;
};

GType          gowl_key_combo_get_type (void) G_GNUC_CONST;

GowlKeyCombo * gowl_key_combo_new     (guint                modifiers,
                                        guint                keysym);

GowlKeyCombo * gowl_key_combo_copy    (const GowlKeyCombo  *self);

void           gowl_key_combo_free    (GowlKeyCombo         *self);

gboolean       gowl_key_combo_equals  (const GowlKeyCombo  *a,
                                        const GowlKeyCombo  *b);

gchar *        gowl_key_combo_to_string(const GowlKeyCombo  *self);

G_END_DECLS

#endif /* GOWL_KEY_COMBO_H */
