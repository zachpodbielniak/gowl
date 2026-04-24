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

#ifndef GOWL_PREFIX_KEY_POLICY_H
#define GOWL_PREFIX_KEY_POLICY_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_PREFIX_KEY_POLICY (gowl_prefix_key_policy_get_type())

G_DECLARE_INTERFACE(GowlPrefixKeyPolicy, gowl_prefix_key_policy,
                    GOWL, PREFIX_KEY_POLICY, GObject)

/**
 * GowlPrefixKeyPolicyInterface:
 * @parent_iface: parent GTypeInterface
 * @is_prefix: vfunc — returns %TRUE if the given key press should
 *   redirect keyboard focus to the Emacs client.  Called on every
 *   key press (not release) when the currently focused client is
 *   NOT the Emacs client.  The compositor uses the return value to
 *   decide whether to invoke
 *   #gowl_seat_push_focus_redirect.
 *
 * A runtime-pluggable contract that decides whether a given key
 * press should pre-empt the currently focused embed and redirect
 * the key stream to Emacs.  The default implementation ships as
 * #GowlStaticPrefixKeyPolicy but users are free to plug a custom
 * implementation (including an Elisp-backed GObject closure via
 * cmacs-gobject).
 *
 * Install the policy via #gowl_compositor_set_prefix_key_policy.
 * When no policy is set, the compositor never redirects — this is
 * the default for standalone and nested gowl.
 */
struct _GowlPrefixKeyPolicyInterface {
	GTypeInterface parent_iface;

	gboolean (*is_prefix) (GowlPrefixKeyPolicy *self,
	                        guint                modifiers,
	                        guint                keysym,
	                        guint                keycode);
};

/**
 * gowl_prefix_key_policy_is_prefix:
 * @self: a #GowlPrefixKeyPolicy
 * @modifiers: modifier bitmask (GowlKeyMod flags) at the time of the press
 * @keysym: XKB keysym for the press
 * @keycode: raw hardware keycode (may be ignored by most policies)
 *
 * Dispatcher for the #GowlPrefixKeyPolicyInterface.is_prefix vfunc.
 * Safe to call with a %NULL self (returns %FALSE).
 *
 * Returns: %TRUE if the key press is a prefix key that should
 *          redirect focus to Emacs.
 */
gboolean
gowl_prefix_key_policy_is_prefix(GowlPrefixKeyPolicy *self,
                                  guint                modifiers,
                                  guint                keysym,
                                  guint                keycode);

G_END_DECLS

#endif /* GOWL_PREFIX_KEY_POLICY_H */
