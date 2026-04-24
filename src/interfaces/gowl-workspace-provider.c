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

#include "gowl-workspace-provider.h"

G_DEFINE_INTERFACE(GowlWorkspaceProvider, gowl_workspace_provider, G_TYPE_OBJECT)

static void
gowl_workspace_provider_default_init(GowlWorkspaceProviderInterface *iface)
{
	(void)iface;
}

GowlWorkspace *
gowl_workspace_provider_create(GowlWorkspaceProvider *self,
                                const gchar           *name,
                                guint32                tag_mask)
{
	GowlWorkspaceProviderInterface *iface;

	if (self == NULL)
		return NULL;

	g_return_val_if_fail(GOWL_IS_WORKSPACE_PROVIDER(self), NULL);

	iface = GOWL_WORKSPACE_PROVIDER_GET_IFACE(self);
	if (iface->create == NULL)
		return NULL;
	return iface->create(self, name, tag_mask);
}

gboolean
gowl_workspace_provider_switch_to(GowlWorkspaceProvider *self,
                                   GowlWorkspace         *workspace)
{
	GowlWorkspaceProviderInterface *iface;

	if (self == NULL)
		return FALSE;

	g_return_val_if_fail(GOWL_IS_WORKSPACE_PROVIDER(self), FALSE);

	iface = GOWL_WORKSPACE_PROVIDER_GET_IFACE(self);
	if (iface->switch_to == NULL)
		return FALSE;
	return iface->switch_to(self, workspace);
}

gboolean
gowl_workspace_provider_destroy(GowlWorkspaceProvider *self,
                                 GowlWorkspace         *workspace)
{
	GowlWorkspaceProviderInterface *iface;

	if (self == NULL)
		return FALSE;

	g_return_val_if_fail(GOWL_IS_WORKSPACE_PROVIDER(self), FALSE);

	iface = GOWL_WORKSPACE_PROVIDER_GET_IFACE(self);
	if (iface->destroy == NULL)
		return FALSE;
	return iface->destroy(self, workspace);
}

GList *
gowl_workspace_provider_list(GowlWorkspaceProvider *self)
{
	GowlWorkspaceProviderInterface *iface;

	if (self == NULL)
		return NULL;

	g_return_val_if_fail(GOWL_IS_WORKSPACE_PROVIDER(self), NULL);

	iface = GOWL_WORKSPACE_PROVIDER_GET_IFACE(self);
	if (iface->list == NULL)
		return NULL;
	return iface->list(self);
}

GowlWorkspace *
gowl_workspace_provider_get_current(GowlWorkspaceProvider *self)
{
	GowlWorkspaceProviderInterface *iface;

	if (self == NULL)
		return NULL;

	g_return_val_if_fail(GOWL_IS_WORKSPACE_PROVIDER(self), NULL);

	iface = GOWL_WORKSPACE_PROVIDER_GET_IFACE(self);
	if (iface->get_current == NULL)
		return NULL;
	return iface->get_current(self);
}

GowlWorkspace *
gowl_workspace_provider_lookup(GowlWorkspaceProvider *self,
                                guint64                id)
{
	GowlWorkspaceProviderInterface *iface;

	if (self == NULL)
		return NULL;

	g_return_val_if_fail(GOWL_IS_WORKSPACE_PROVIDER(self), NULL);

	iface = GOWL_WORKSPACE_PROVIDER_GET_IFACE(self);
	if (iface->lookup == NULL)
		return NULL;
	return iface->lookup(self, id);
}
