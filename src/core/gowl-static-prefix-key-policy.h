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

#ifndef GOWL_STATIC_PREFIX_KEY_POLICY_H
#define GOWL_STATIC_PREFIX_KEY_POLICY_H

#include <glib-object.h>
#include "../interfaces/gowl-prefix-key-policy.h"

G_BEGIN_DECLS

#define GOWL_TYPE_STATIC_PREFIX_KEY_POLICY \
	(gowl_static_prefix_key_policy_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlStaticPrefixKeyPolicy,
                          gowl_static_prefix_key_policy,
                          GOWL, STATIC_PREFIX_KEY_POLICY, GObject)

/**
 * GowlStaticPrefixKeyPolicyClass:
 * @parent_class: the parent class
 *
 * Default #GowlPrefixKeyPolicy implementation backed by a simple
 * table of (modifiers, keysym) pairs.  Matches are exact on modifier
 * bitmask and keysym; keycode is ignored.
 *
 * Subclasses may override the #GowlPrefixKeyPolicyInterface.is_prefix
 * vfunc to implement alternative lookup strategies (regex on keysym
 * names, context-sensitive matching, etc.) while reusing the
 * table-add/remove helpers.
 */
struct _GowlStaticPrefixKeyPolicyClass {
	GObjectClass parent_class;
};

/**
 * gowl_static_prefix_key_policy_new:
 *
 * Creates an empty static policy.  Populate with
 * #gowl_static_prefix_key_policy_add or
 * #gowl_static_prefix_key_policy_add_from_string.
 *
 * Returns: (transfer full): a new #GowlStaticPrefixKeyPolicy
 */
GowlStaticPrefixKeyPolicy *gowl_static_prefix_key_policy_new(void);

/**
 * gowl_static_prefix_key_policy_new_from_strings:
 * @keybinds: (array zero-terminated=1) (element-type utf8): a
 *   %NULL-terminated array of keybind strings parsed via
 *   #gowl_keybind_parse (e.g. "Control+x", "Control+c", "Meta+x").
 *
 * Convenience constructor: parses each string and appends a row to
 * the table.  Strings that fail to parse are skipped with a
 * warning.
 *
 * Returns: (transfer full): a new #GowlStaticPrefixKeyPolicy
 */
GowlStaticPrefixKeyPolicy *
gowl_static_prefix_key_policy_new_from_strings(const gchar * const *keybinds);

/**
 * gowl_static_prefix_key_policy_add:
 * @self: a #GowlStaticPrefixKeyPolicy
 * @modifiers: modifier bitmask
 * @keysym: XKB keysym
 *
 * Appends a row to the table.  Duplicates are allowed but wasteful.
 */
void
gowl_static_prefix_key_policy_add(GowlStaticPrefixKeyPolicy *self,
                                   guint                      modifiers,
                                   guint                      keysym);

/**
 * gowl_static_prefix_key_policy_add_from_string:
 * @self: a #GowlStaticPrefixKeyPolicy
 * @keybind: a keybind string (see #gowl_keybind_parse)
 *
 * Parses @keybind and appends it.  Returns %FALSE (with a warning)
 * if the string is invalid.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_static_prefix_key_policy_add_from_string(
	GowlStaticPrefixKeyPolicy *self,
	const gchar               *keybind);

/**
 * gowl_static_prefix_key_policy_clear:
 * @self: a #GowlStaticPrefixKeyPolicy
 *
 * Removes every entry from the table.
 */
void
gowl_static_prefix_key_policy_clear(GowlStaticPrefixKeyPolicy *self);

/**
 * gowl_static_prefix_key_policy_size:
 * @self: a #GowlStaticPrefixKeyPolicy
 *
 * Returns: number of entries currently in the table
 */
guint
gowl_static_prefix_key_policy_size(GowlStaticPrefixKeyPolicy *self);

G_END_DECLS

#endif /* GOWL_STATIC_PREFIX_KEY_POLICY_H */
