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

#include "gowl-static-prefix-key-policy.h"
#include "../config/gowl-keybind.h"

#include <string.h>

typedef struct {
	guint modifiers;
	guint keysym;
} GowlStaticPrefixEntry;

typedef struct {
	GArray *entries; /* element-type GowlStaticPrefixEntry */
} GowlStaticPrefixKeyPolicyPrivate;

static void gowl_static_prefix_key_policy_iface_init(
	GowlPrefixKeyPolicyInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlStaticPrefixKeyPolicy,
                        gowl_static_prefix_key_policy,
                        G_TYPE_OBJECT,
                        G_ADD_PRIVATE(GowlStaticPrefixKeyPolicy)
                        G_IMPLEMENT_INTERFACE(
                            GOWL_TYPE_PREFIX_KEY_POLICY,
                            gowl_static_prefix_key_policy_iface_init))

#define GET_PRIV(self)                                                  \
	((GowlStaticPrefixKeyPolicyPrivate *)                           \
	 gowl_static_prefix_key_policy_get_instance_private(self))

static gboolean
gowl_static_prefix_key_policy_is_prefix(GowlPrefixKeyPolicy *policy,
                                         guint                modifiers,
                                         guint                keysym,
                                         guint                keycode)
{
	GowlStaticPrefixKeyPolicy *self;
	GowlStaticPrefixKeyPolicyPrivate *priv;
	guint i;

	(void)keycode;

	g_return_val_if_fail(
		GOWL_IS_STATIC_PREFIX_KEY_POLICY(policy), FALSE);

	self = GOWL_STATIC_PREFIX_KEY_POLICY(policy);
	priv = GET_PRIV(self);

	for (i = 0; i < priv->entries->len; i++) {
		GowlStaticPrefixEntry *e =
			&g_array_index(priv->entries,
			               GowlStaticPrefixEntry, i);

		if (e->modifiers == modifiers && e->keysym == keysym)
			return TRUE;
	}

	return FALSE;
}

static void
gowl_static_prefix_key_policy_iface_init(GowlPrefixKeyPolicyInterface *iface)
{
	iface->is_prefix = gowl_static_prefix_key_policy_is_prefix;
}

static void
gowl_static_prefix_key_policy_finalize(GObject *object)
{
	GowlStaticPrefixKeyPolicy *self =
		GOWL_STATIC_PREFIX_KEY_POLICY(object);
	GowlStaticPrefixKeyPolicyPrivate *priv = GET_PRIV(self);

	g_clear_pointer(&priv->entries, g_array_unref);

	G_OBJECT_CLASS(gowl_static_prefix_key_policy_parent_class)->finalize(
		object);
}

static void
gowl_static_prefix_key_policy_class_init(GowlStaticPrefixKeyPolicyClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gowl_static_prefix_key_policy_finalize;
}

static void
gowl_static_prefix_key_policy_init(GowlStaticPrefixKeyPolicy *self)
{
	GowlStaticPrefixKeyPolicyPrivate *priv = GET_PRIV(self);

	priv->entries = g_array_new(FALSE, FALSE,
	                             sizeof(GowlStaticPrefixEntry));
}

GowlStaticPrefixKeyPolicy *
gowl_static_prefix_key_policy_new(void)
{
	return g_object_new(GOWL_TYPE_STATIC_PREFIX_KEY_POLICY, NULL);
}

GowlStaticPrefixKeyPolicy *
gowl_static_prefix_key_policy_new_from_strings(const gchar * const *keybinds)
{
	GowlStaticPrefixKeyPolicy *self;
	guint i;

	self = gowl_static_prefix_key_policy_new();
	if (keybinds == NULL)
		return self;

	for (i = 0; keybinds[i] != NULL; i++)
		gowl_static_prefix_key_policy_add_from_string(self, keybinds[i]);

	return self;
}

void
gowl_static_prefix_key_policy_add(GowlStaticPrefixKeyPolicy *self,
                                   guint                      modifiers,
                                   guint                      keysym)
{
	GowlStaticPrefixKeyPolicyPrivate *priv;
	GowlStaticPrefixEntry entry;

	g_return_if_fail(GOWL_IS_STATIC_PREFIX_KEY_POLICY(self));

	priv = GET_PRIV(self);
	entry.modifiers = modifiers;
	entry.keysym    = keysym;
	g_array_append_val(priv->entries, entry);
}

gboolean
gowl_static_prefix_key_policy_add_from_string(
	GowlStaticPrefixKeyPolicy *self,
	const gchar               *keybind)
{
	guint modifiers;
	guint keysym;

	g_return_val_if_fail(GOWL_IS_STATIC_PREFIX_KEY_POLICY(self), FALSE);
	g_return_val_if_fail(keybind != NULL, FALSE);

	if (!gowl_keybind_parse(keybind, &modifiers, &keysym)) {
		g_warning("gowl-static-prefix-key-policy: failed to parse '%s'",
		          keybind);
		return FALSE;
	}

	gowl_static_prefix_key_policy_add(self, modifiers, keysym);
	return TRUE;
}

void
gowl_static_prefix_key_policy_clear(GowlStaticPrefixKeyPolicy *self)
{
	GowlStaticPrefixKeyPolicyPrivate *priv;

	g_return_if_fail(GOWL_IS_STATIC_PREFIX_KEY_POLICY(self));

	priv = GET_PRIV(self);
	g_array_set_size(priv->entries, 0);
}

guint
gowl_static_prefix_key_policy_size(GowlStaticPrefixKeyPolicy *self)
{
	GowlStaticPrefixKeyPolicyPrivate *priv;

	g_return_val_if_fail(GOWL_IS_STATIC_PREFIX_KEY_POLICY(self), 0);

	priv = GET_PRIV(self);
	return priv->entries->len;
}
