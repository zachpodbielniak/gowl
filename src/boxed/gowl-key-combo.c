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

#include "gowl-key-combo.h"
#include <xkbcommon/xkbcommon.h>

G_DEFINE_BOXED_TYPE(GowlKeyCombo, gowl_key_combo,
                    gowl_key_combo_copy, gowl_key_combo_free)

/**
 * gowl_key_combo_new:
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 *
 * Allocates a new #GowlKeyCombo representing the given modifier + keysym
 * pair.
 *
 * Returns: (transfer full): a newly allocated #GowlKeyCombo. Free with
 *          gowl_key_combo_free().
 */
GowlKeyCombo *
gowl_key_combo_new(
	guint modifiers,
	guint keysym
){
	GowlKeyCombo *self;

	self = g_slice_new(GowlKeyCombo);
	self->modifiers = modifiers;
	self->keysym = keysym;

	return self;
}

/**
 * gowl_key_combo_copy:
 * @self: (not nullable): a #GowlKeyCombo to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_key_combo_free().
 */
GowlKeyCombo *
gowl_key_combo_copy(const GowlKeyCombo *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_key_combo_new(self->modifiers, self->keysym);
}

/**
 * gowl_key_combo_free:
 * @self: (nullable): a #GowlKeyCombo to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_key_combo_free(GowlKeyCombo *self)
{
	if (self != NULL) {
		g_slice_free(GowlKeyCombo, self);
	}
}

/**
 * gowl_key_combo_equals:
 * @a: (not nullable): first #GowlKeyCombo
 * @b: (not nullable): second #GowlKeyCombo
 *
 * Tests whether two key combinations have identical modifiers and keysym.
 *
 * Returns: %TRUE if @a and @b are equal, %FALSE otherwise.
 */
gboolean
gowl_key_combo_equals(
	const GowlKeyCombo *a,
	const GowlKeyCombo *b
){
	g_return_val_if_fail(a != NULL, FALSE);
	g_return_val_if_fail(b != NULL, FALSE);

	return (a->modifiers == b->modifiers && a->keysym == b->keysym);
}

/**
 * gowl_key_combo_to_string:
 * @self: (not nullable): a #GowlKeyCombo
 *
 * Produces a human-readable representation of the key combination,
 * for example "Logo+Shift+Return".  Modifier names are separated by '+'.
 *
 * Returns: (transfer full): a newly allocated string. Free with g_free().
 */
gchar *
gowl_key_combo_to_string(const GowlKeyCombo *self)
{
	GString *str;
	char keybuf[64];

	g_return_val_if_fail(self != NULL, NULL);

	str = g_string_new(NULL);

	/* Append active modifier names in a fixed order */
	if (self->modifiers & GOWL_KEY_MOD_LOGO) {
		g_string_append(str, "Logo+");
	}
	if (self->modifiers & GOWL_KEY_MOD_CTRL) {
		g_string_append(str, "Ctrl+");
	}
	if (self->modifiers & GOWL_KEY_MOD_ALT) {
		g_string_append(str, "Alt+");
	}
	if (self->modifiers & GOWL_KEY_MOD_SHIFT) {
		g_string_append(str, "Shift+");
	}
	if (self->modifiers & GOWL_KEY_MOD_CAPS) {
		g_string_append(str, "Caps+");
	}
	if (self->modifiers & GOWL_KEY_MOD_MOD2) {
		g_string_append(str, "Mod2+");
	}
	if (self->modifiers & GOWL_KEY_MOD_MOD3) {
		g_string_append(str, "Mod3+");
	}
	if (self->modifiers & GOWL_KEY_MOD_MOD5) {
		g_string_append(str, "Mod5+");
	}

	/* Append the keysym name via xkbcommon */
	xkb_keysym_get_name(self->keysym, keybuf, sizeof(keybuf));
	g_string_append(str, keybuf);

	return g_string_free(str, FALSE);
}
