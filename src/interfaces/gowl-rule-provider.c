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

#include "gowl-rule-provider.h"

G_DEFINE_INTERFACE(GowlRuleProvider, gowl_rule_provider, G_TYPE_OBJECT)

static void
gowl_rule_provider_default_init(GowlRuleProviderInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_rule_provider_match_client:
 * @self: a #GowlRuleProvider
 * @app_id: (nullable): the application ID of the client
 * @title: (nullable): the title of the client window
 *
 * Matches a client against the configured rules using its app_id and title.
 * Returns the matched rule data, or %NULL if no rule matches.
 *
 * Returns: (transfer none) (nullable): the matched rule, or %NULL
 */
gpointer
gowl_rule_provider_match_client(
	GowlRuleProvider *self,
	const gchar      *app_id,
	const gchar      *title
){
	GowlRuleProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_RULE_PROVIDER(self), NULL);

	iface = GOWL_RULE_PROVIDER_GET_IFACE(self);
	if (iface->match_client != NULL)
		return iface->match_client(self, app_id, title);
	return NULL;
}
