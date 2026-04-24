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

#include "gowl-prefix-key-policy.h"

G_DEFINE_INTERFACE(GowlPrefixKeyPolicy, gowl_prefix_key_policy, G_TYPE_OBJECT)

static void
gowl_prefix_key_policy_default_init(GowlPrefixKeyPolicyInterface *iface)
{
	/* No default implementation — consumers must subclass or plug
	 * in an implementation via cmacs-gobject closures. */
	(void)iface;
}

/**
 * gowl_prefix_key_policy_is_prefix:
 *
 * Null-safe dispatcher.  Returns %FALSE when @self is %NULL or the
 * interface vfunc is unset — both map to "no key is a prefix" which
 * is the safe default for standalone / nested gowl where no Elisp
 * policy is installed.
 */
gboolean
gowl_prefix_key_policy_is_prefix(GowlPrefixKeyPolicy *self,
                                  guint                modifiers,
                                  guint                keysym,
                                  guint                keycode)
{
	GowlPrefixKeyPolicyInterface *iface;

	if (self == NULL)
		return FALSE;

	g_return_val_if_fail(GOWL_IS_PREFIX_KEY_POLICY(self), FALSE);

	iface = GOWL_PREFIX_KEY_POLICY_GET_IFACE(self);
	if (iface->is_prefix == NULL)
		return FALSE;

	return iface->is_prefix(self, modifiers, keysym, keycode);
}
