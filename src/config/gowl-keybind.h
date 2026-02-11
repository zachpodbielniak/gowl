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

#ifndef GOWL_KEYBIND_H
#define GOWL_KEYBIND_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gowl_keybind_parse:
 * @str: a keybind string such as "Super+Shift+Return"
 * @out_modifiers: (out): location for the parsed modifier bitmask
 * @out_keysym: (out): location for the parsed XKB keysym
 *
 * Parses a human-readable keybind string into a modifier bitmask
 * and an XKB keysym value. The string is split on "+" delimiters;
 * all tokens except the last are treated as modifier names (matched
 * case-insensitively), and the last token is resolved via
 * xkb_keysym_from_name().
 *
 * Recognised modifier names:
 *  - "Super", "Logo" -> GOWL_KEY_MOD_LOGO  (64)
 *  - "Shift"         -> GOWL_KEY_MOD_SHIFT  (1)
 *  - "Ctrl", "Control" -> GOWL_KEY_MOD_CTRL (4)
 *  - "Alt", "Mod1"   -> GOWL_KEY_MOD_ALT    (8)
 *  - "Mod2"          -> GOWL_KEY_MOD_MOD2   (16)
 *  - "Mod3"          -> GOWL_KEY_MOD_MOD3   (32)
 *  - "Mod5"          -> GOWL_KEY_MOD_MOD5  (128)
 *
 * Returns: %TRUE on success, %FALSE if the string is invalid
 */
gboolean
gowl_keybind_parse(
	const gchar *str,
	guint       *out_modifiers,
	guint       *out_keysym
);

/**
 * gowl_keybind_to_string:
 * @modifiers: a modifier bitmask (#GowlKeyMod flags)
 * @keysym: an XKB keysym value
 *
 * Converts a modifier bitmask and keysym back to a human-readable
 * string in the form "Mod+Mod+Key". Caller must free the returned
 * string with g_free().
 *
 * Returns: (transfer full): a newly allocated keybind string
 */
gchar *
gowl_keybind_to_string(
	guint modifiers,
	guint keysym
);

G_END_DECLS

#endif /* GOWL_KEYBIND_H */
