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

#include "gowl-frame-workspace-manager.h"

typedef struct {
	GHashTable    *workspaces;  /* guint64 -> GowlWorkspace* (owned) */
	GowlWorkspace *active;      /* borrowed pointer into workspaces  */
	guint64        next_id;     /* monotonic id allocator (>= 1)     */
} GowlFrameWorkspaceManagerPrivate;

static void gowl_frame_workspace_manager_iface_init(
	GowlWorkspaceProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(
	GowlFrameWorkspaceManager,
	gowl_frame_workspace_manager,
	G_TYPE_OBJECT,
	G_ADD_PRIVATE(GowlFrameWorkspaceManager)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_WORKSPACE_PROVIDER,
	                      gowl_frame_workspace_manager_iface_init))

#define GET_PRIV(self) \
	((GowlFrameWorkspaceManagerPrivate *) \
	 gowl_frame_workspace_manager_get_instance_private(self))

/* ---------------------------------------------------------------
 * GowlWorkspaceProvider vtable implementations
 * --------------------------------------------------------------- */

static GowlWorkspace *
frame_manager_create(GowlWorkspaceProvider *provider,
                      const gchar           *name,
                      guint32                tag_mask)
{
	GowlFrameWorkspaceManager        *self;
	GowlFrameWorkspaceManagerPrivate *priv;
	GowlWorkspace                    *ws;
	guint64                           id;

	self = GOWL_FRAME_WORKSPACE_MANAGER(provider);
	priv = GET_PRIV(self);

	id = priv->next_id++;
	ws = gowl_workspace_new(id, name, tag_mask);

	g_hash_table_insert(priv->workspaces,
	                    g_memdup2(&id, sizeof(id)),
	                    ws);

	/* First workspace becomes active automatically. */
	if (priv->active == NULL)
		priv->active = ws;

	return ws;
}

static gboolean
frame_manager_switch_to(GowlWorkspaceProvider *provider,
                         GowlWorkspace         *workspace)
{
	GowlFrameWorkspaceManager        *self;
	GowlFrameWorkspaceManagerPrivate *priv;

	if (workspace == NULL)
		return FALSE;

	self = GOWL_FRAME_WORKSPACE_MANAGER(provider);
	priv = GET_PRIV(self);

	if (priv->active == workspace)
		return FALSE;

	priv->active = workspace;
	return TRUE;
}

static gboolean
frame_manager_destroy(GowlWorkspaceProvider *provider,
                       GowlWorkspace         *workspace)
{
	GowlFrameWorkspaceManager        *self;
	GowlFrameWorkspaceManagerPrivate *priv;
	guint64                           id;

	if (workspace == NULL)
		return FALSE;

	self = GOWL_FRAME_WORKSPACE_MANAGER(provider);
	priv = GET_PRIV(self);

	id = gowl_workspace_get_id(workspace);

	if (!g_hash_table_contains(priv->workspaces, &id))
		return FALSE;

	if (priv->active == workspace)
		priv->active = NULL;  /* consumer chooses a replacement */

	/* The hash table's value destroy frees the workspace. */
	g_hash_table_remove(priv->workspaces, &id);

	return TRUE;
}

static GList *
frame_manager_list(GowlWorkspaceProvider *provider)
{
	GowlFrameWorkspaceManager        *self;
	GowlFrameWorkspaceManagerPrivate *priv;
	GList                            *result = NULL;
	GHashTableIter                    iter;
	gpointer                          value;

	self = GOWL_FRAME_WORKSPACE_MANAGER(provider);
	priv = GET_PRIV(self);

	g_hash_table_iter_init(&iter, priv->workspaces);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		result = g_list_prepend(result, value);

	return g_list_reverse(result);
}

static GowlWorkspace *
frame_manager_get_current(GowlWorkspaceProvider *provider)
{
	GowlFrameWorkspaceManager *self;

	self = GOWL_FRAME_WORKSPACE_MANAGER(provider);
	return GET_PRIV(self)->active;
}

static GowlWorkspace *
frame_manager_lookup(GowlWorkspaceProvider *provider,
                      guint64                id)
{
	GowlFrameWorkspaceManager *self;

	self = GOWL_FRAME_WORKSPACE_MANAGER(provider);
	return (GowlWorkspace *)g_hash_table_lookup(
		GET_PRIV(self)->workspaces, &id);
}

static void
gowl_frame_workspace_manager_iface_init(
	GowlWorkspaceProviderInterface *iface)
{
	iface->create      = frame_manager_create;
	iface->switch_to   = frame_manager_switch_to;
	iface->destroy     = frame_manager_destroy;
	iface->list        = frame_manager_list;
	iface->get_current = frame_manager_get_current;
	iface->lookup      = frame_manager_lookup;
}

/* ---------------------------------------------------------------
 * GObject lifecycle
 * --------------------------------------------------------------- */

static void
gowl_frame_workspace_manager_finalize(GObject *object)
{
	GowlFrameWorkspaceManager        *self;
	GowlFrameWorkspaceManagerPrivate *priv;

	self = GOWL_FRAME_WORKSPACE_MANAGER(object);
	priv = GET_PRIV(self);

	g_clear_pointer(&priv->workspaces, g_hash_table_unref);

	G_OBJECT_CLASS(gowl_frame_workspace_manager_parent_class)->finalize(
		object);
}

static void
gowl_frame_workspace_manager_class_init(GowlFrameWorkspaceManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = gowl_frame_workspace_manager_finalize;
}

static void
gowl_frame_workspace_manager_init(GowlFrameWorkspaceManager *self)
{
	GowlFrameWorkspaceManagerPrivate *priv = GET_PRIV(self);

	priv->workspaces = g_hash_table_new_full(
		g_int64_hash, g_int64_equal,
		g_free,       g_object_unref);
	priv->active  = NULL;
	priv->next_id = 1;  /* 0 reserved as invalid sentinel */
}

GowlFrameWorkspaceManager *
gowl_frame_workspace_manager_new(void)
{
	return g_object_new(GOWL_TYPE_FRAME_WORKSPACE_MANAGER, NULL);
}
