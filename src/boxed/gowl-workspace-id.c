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

#include "gowl-workspace-id.h"

G_DEFINE_BOXED_TYPE(GowlWorkspaceId, gowl_workspace_id,
                    gowl_workspace_id_copy,
                    gowl_workspace_id_free)

GowlWorkspaceId *
gowl_workspace_id_new(guint64 value)
{
	GowlWorkspaceId *self;

	self = g_slice_new(GowlWorkspaceId);
	self->value = value;
	return self;
}

GowlWorkspaceId *
gowl_workspace_id_copy(const GowlWorkspaceId *self)
{
	if (self == NULL)
		return NULL;
	return gowl_workspace_id_new(self->value);
}

void
gowl_workspace_id_free(GowlWorkspaceId *self)
{
	if (self == NULL)
		return;
	g_slice_free(GowlWorkspaceId, self);
}

guint64
gowl_workspace_id_get_value(const GowlWorkspaceId *self)
{
	if (self == NULL)
		return GOWL_WORKSPACE_ID_INVALID;
	return self->value;
}

gboolean
gowl_workspace_id_equals(const GowlWorkspaceId *a,
                          const GowlWorkspaceId *b)
{
	if (a == NULL && b == NULL)
		return TRUE;
	if (a == NULL || b == NULL)
		return FALSE;
	return a->value == b->value;
}
