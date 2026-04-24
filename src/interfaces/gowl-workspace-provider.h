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

#ifndef GOWL_WORKSPACE_PROVIDER_H
#define GOWL_WORKSPACE_PROVIDER_H

#include <glib-object.h>
#include "../core/gowl-workspace.h"

G_BEGIN_DECLS

#define GOWL_TYPE_WORKSPACE_PROVIDER (gowl_workspace_provider_get_type())

G_DECLARE_INTERFACE(GowlWorkspaceProvider, gowl_workspace_provider,
                    GOWL, WORKSPACE_PROVIDER, GObject)

/**
 * GowlWorkspaceProviderInterface:
 * @parent_iface: parent #GTypeInterface
 * @create: vfunc — allocate a fresh workspace (id assigned by the
 *   provider) and register it.  Returns a borrowed reference; the
 *   provider owns the instance's lifetime.
 * @switch_to: vfunc — make @workspace the active workspace.
 *   Implementations should arrange clients, swap per-monitor tag
 *   state, and emit `workspace-switched` on the compositor.
 *   Returning %FALSE means "already active" or "invalid target".
 * @destroy: vfunc — remove @workspace from the provider.
 *   Implementations may fold the released id's clients into
 *   another workspace or leave them orphaned at the caller's
 *   discretion.
 * @list: vfunc — return a %NULL-terminated list (transfer
 *   container) of current workspaces.  Caller frees with
 *   `g_list_free` (not `g_list_free_full` — the workspaces
 *   themselves are owned by the provider).
 * @get_current: vfunc — return the active workspace, or %NULL if
 *   none has been created yet.
 * @lookup: vfunc — look up a workspace by id.  Returns %NULL when
 *   not found.
 *
 * Contract for workspace management.  Gowl ships
 * #GowlFrameWorkspaceManager as the default, which maps one
 * workspace per Emacs frame for cmacs `--gowl` use.  Module
 * implementors can provide alternatives (e.g. fixed N workspaces
 * that persist across frames, tag-centric wrappers, etc.).
 *
 * The compositor only cares about the contract; whichever provider
 * is installed becomes authoritative for the `workspace-*` signals
 * on #GowlCompositor.
 */
struct _GowlWorkspaceProviderInterface {
	GTypeInterface parent_iface;

	GowlWorkspace * (*create)      (GowlWorkspaceProvider *self,
	                                 const gchar           *name,
	                                 guint32                tag_mask);
	gboolean        (*switch_to)   (GowlWorkspaceProvider *self,
	                                 GowlWorkspace         *workspace);
	gboolean        (*destroy)     (GowlWorkspaceProvider *self,
	                                 GowlWorkspace         *workspace);
	GList         * (*list)        (GowlWorkspaceProvider *self);
	GowlWorkspace * (*get_current) (GowlWorkspaceProvider *self);
	GowlWorkspace * (*lookup)      (GowlWorkspaceProvider *self,
	                                 guint64                id);
};

/* ---------------------------------------------------------------
 * Null-safe dispatch wrappers.  Each simply forwards to the vfunc
 * on @self when present; a %NULL self or missing vfunc returns a
 * benign default (NULL / FALSE / empty list).
 * --------------------------------------------------------------- */

GowlWorkspace *
gowl_workspace_provider_create(GowlWorkspaceProvider *self,
                                const gchar           *name,
                                guint32                tag_mask);

gboolean
gowl_workspace_provider_switch_to(GowlWorkspaceProvider *self,
                                   GowlWorkspace         *workspace);

gboolean
gowl_workspace_provider_destroy(GowlWorkspaceProvider *self,
                                 GowlWorkspace         *workspace);

/**
 * gowl_workspace_provider_list:
 * @self: a #GowlWorkspaceProvider, or %NULL
 *
 * Returns: (transfer container) (element-type GowlWorkspace)
 *          (nullable): a newly allocated list of current
 *          workspaces.  The list itself must be freed with
 *          `g_list_free`; the contained workspaces remain owned
 *          by the provider.
 */
GList *
gowl_workspace_provider_list(GowlWorkspaceProvider *self);

GowlWorkspace *
gowl_workspace_provider_get_current(GowlWorkspaceProvider *self);

GowlWorkspace *
gowl_workspace_provider_lookup(GowlWorkspaceProvider *self,
                                guint64                id);

G_END_DECLS

#endif /* GOWL_WORKSPACE_PROVIDER_H */
