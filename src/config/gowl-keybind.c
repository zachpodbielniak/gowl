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

#include "gowl-keybind.h"
#include "gowl-enums.h"

#include <glib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* Lookup table mapping modifier name strings to GowlKeyMod values.
 * Matching is case-insensitive. */
typedef struct {
	const gchar *name;
	guint        mod;
} GowlModEntry;

static const GowlModEntry mod_table[] = {
	{ "super",   GOWL_KEY_MOD_LOGO  },
	{ "logo",    GOWL_KEY_MOD_LOGO  },
	{ "shift",   GOWL_KEY_MOD_SHIFT },
	{ "ctrl",    GOWL_KEY_MOD_CTRL  },
	{ "control", GOWL_KEY_MOD_CTRL  },
	{ "alt",     GOWL_KEY_MOD_ALT   },
	{ "mod1",    GOWL_KEY_MOD_ALT   },
	{ "mod2",    GOWL_KEY_MOD_MOD2  },
	{ "mod3",    GOWL_KEY_MOD_MOD3  },
	{ "mod5",    GOWL_KEY_MOD_MOD5  },
	{ NULL,      0                   }
};

/**
 * try_parse_modifier:
 * @token: a single token from the keybind string (e.g. "Super")
 * @out_mod: (out): location for the modifier value if matched
 *
 * Attempts to match @token against the known modifier names,
 * performing a case-insensitive comparison. If a match is found,
 * the corresponding GowlKeyMod value is written to @out_mod.
 *
 * Returns: %TRUE if @token matched a modifier, %FALSE otherwise
 */
static gboolean
try_parse_modifier(
	const gchar *token,
	guint       *out_mod
){
	g_autofree gchar *lower = NULL;
	const GowlModEntry *entry;

	lower = g_ascii_strdown(token, -1);

	for (entry = mod_table; entry->name != NULL; entry++) {
		if (g_str_equal(lower, entry->name)) {
			*out_mod = entry->mod;
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * gowl_keybind_parse:
 * @str: a keybind string such as "Super+Shift+Return"
 * @out_modifiers: (out): location for the parsed modifier bitmask
 * @out_keysym: (out): location for the parsed XKB keysym
 *
 * Splits @str on "+" and resolves each token. All tokens except the
 * last must be recognised modifier names; the last token is resolved
 * to a keysym via xkb_keysym_from_name(). If any token is invalid
 * or xkb_keysym_from_name() returns XKB_KEY_NoSymbol, the function
 * returns %FALSE and does not modify the output parameters.
 *
 * Returns: %TRUE on success, %FALSE on invalid input
 */
gboolean
gowl_keybind_parse(
	const gchar *str,
	guint       *out_modifiers,
	guint       *out_keysym
){
	g_auto(GStrv) tokens = NULL;
	guint n_tokens;
	guint modifiers;
	xkb_keysym_t keysym;
	guint i;

	g_return_val_if_fail(str != NULL, FALSE);
	g_return_val_if_fail(out_modifiers != NULL, FALSE);
	g_return_val_if_fail(out_keysym != NULL, FALSE);

	tokens = g_strsplit(str, "+", -1);
	n_tokens = g_strv_length(tokens);

	if (n_tokens == 0)
		return FALSE;

	/* All tokens except the last are modifiers */
	modifiers = 0;
	for (i = 0; i < n_tokens - 1; i++) {
		guint mod_val;
		g_strstrip(tokens[i]);

		if (!try_parse_modifier(tokens[i], &mod_val)) {
			g_warning("gowl_keybind_parse: unknown modifier '%s' in '%s'",
			          tokens[i], str);
			return FALSE;
		}
		modifiers |= mod_val;
	}

	/* Last token is the key name */
	g_strstrip(tokens[n_tokens - 1]);
	keysym = xkb_keysym_from_name(tokens[n_tokens - 1],
	                               XKB_KEYSYM_CASE_INSENSITIVE);

	if (keysym == XKB_KEY_NoSymbol) {
		g_warning("gowl_keybind_parse: unknown keysym '%s' in '%s'",
		          tokens[n_tokens - 1], str);
		return FALSE;
	}

	*out_modifiers = modifiers;
	*out_keysym = (guint)keysym;
	return TRUE;
}

/**
 * gowl_keybind_to_string:
 * @modifiers: a modifier bitmask (#GowlKeyMod flags)
 * @keysym: an XKB keysym value
 *
 * Builds a human-readable keybind string by appending the names of
 * all set modifier bits, then the keysym name from xkb_keysym_get_name().
 * Parts are joined with "+".
 *
 * Returns: (transfer full): a newly allocated string; free with g_free()
 */
gchar *
gowl_keybind_to_string(
	guint modifiers,
	guint keysym
){
	GString *result;
	char name_buf[64];

	result = g_string_new(NULL);

	/* Append modifier names in a consistent order */
	if (modifiers & GOWL_KEY_MOD_LOGO)
		g_string_append(result, "Super+");
	if (modifiers & GOWL_KEY_MOD_CTRL)
		g_string_append(result, "Ctrl+");
	if (modifiers & GOWL_KEY_MOD_ALT)
		g_string_append(result, "Alt+");
	if (modifiers & GOWL_KEY_MOD_SHIFT)
		g_string_append(result, "Shift+");
	if (modifiers & GOWL_KEY_MOD_MOD2)
		g_string_append(result, "Mod2+");
	if (modifiers & GOWL_KEY_MOD_MOD3)
		g_string_append(result, "Mod3+");
	if (modifiers & GOWL_KEY_MOD_MOD5)
		g_string_append(result, "Mod5+");

	/* Append key name */
	xkb_keysym_get_name((xkb_keysym_t)keysym, name_buf, sizeof(name_buf));
	g_string_append(result, name_buf);

	return g_string_free(result, FALSE);
}
