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

#ifndef GOWL_RULE_PROVIDER_H
#define GOWL_RULE_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_RULE_PROVIDER (gowl_rule_provider_get_type())

G_DECLARE_INTERFACE(GowlRuleProvider, gowl_rule_provider, GOWL, RULE_PROVIDER, GObject)

struct _GowlRuleProviderInterface {
	GTypeInterface parent_iface;

	gpointer (*match_client) (GowlRuleProvider *self, const gchar *app_id, const gchar *title);
};

/* Public dispatch functions */
gpointer gowl_rule_provider_match_client (GowlRuleProvider *self, const gchar *app_id, const gchar *title);

G_END_DECLS

#endif /* GOWL_RULE_PROVIDER_H */
