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

#ifndef GOWL_WORKSPACE_H
#define GOWL_WORKSPACE_H

#include <glib-object.h>
#include "../boxed/gowl-workspace-id.h"

G_BEGIN_DECLS

#define GOWL_TYPE_WORKSPACE (gowl_workspace_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlWorkspace, gowl_workspace,
                          GOWL, WORKSPACE, GObject)

/**
 * GowlWorkspaceClass:
 * @parent_class: parent GObjectClass
 *
 * Derivable concrete class representing one workspace.  Subclass to
 * attach per-module metadata (e.g. pertag state, per-workspace
 * wallpaper).  Standard usage keeps the base class — properties are
 * sufficient.
 *
 * A workspace owns:
 *   - a stable numeric #GowlWorkspaceId (allocated by the provider)
 *   - an optional display name
 *   - a 32-bit tag bitmask (so tags / monocle / tile semantics keep
 *     working unchanged inside a workspace)
 *
 * It deliberately does NOT own a client list — workspace →
 * GowlClient* mapping is maintained by the provider so compositor
 * client-lifecycle code stays in one place.  Workspaces can be
 * compared, stored in hash tables (keyed by id via
 * `gowl_workspace_id_get_value`), and serialised without touching
 * compositor mutable state.
 */
struct _GowlWorkspaceClass {
	GObjectClass parent_class;
};

/**
 * gowl_workspace_new:
 * @id: the identifier to assign.  Must be unique within the owning
 *   provider.
 * @name: (nullable): an optional display name; copied
 * @tag_mask: initial tag bitmask (GowlKeyMod-style 32-bit set)
 *
 * Creates a fresh workspace.  Normal callers go through
 * #gowl_workspace_provider_create, which allocates the id, wraps
 * this constructor, and tracks the result internally.
 *
 * Returns: (transfer full): a new #GowlWorkspace
 */
GowlWorkspace *
gowl_workspace_new(guint64      id,
                    const gchar *name,
                    guint32      tag_mask);

/**
 * gowl_workspace_get_id:
 * @self: a #GowlWorkspace
 *
 * Returns: the numeric id, or %GOWL_WORKSPACE_ID_INVALID if @self
 *          is not a valid workspace.
 */
guint64
gowl_workspace_get_id(GowlWorkspace *self);

/**
 * gowl_workspace_get_id_boxed:
 * @self: a #GowlWorkspace
 *
 * Same as #gowl_workspace_get_id but returns the boxed wrapper so
 * the id round-trips through GIR signatures without losing type
 * information.  Caller owns the returned boxed value.
 *
 * Returns: (transfer full): a new #GowlWorkspaceId
 */
GowlWorkspaceId *
gowl_workspace_get_id_boxed(GowlWorkspace *self);

/**
 * gowl_workspace_get_name:
 * @self: a #GowlWorkspace
 *
 * Returns: (transfer none) (nullable): the workspace name, or
 *          %NULL if unset.  Do not free.
 */
const gchar *
gowl_workspace_get_name(GowlWorkspace *self);

/**
 * gowl_workspace_set_name:
 * @self: a #GowlWorkspace
 * @name: (nullable): new name; copied, may be %NULL to clear
 *
 * Changes the display name and emits `notify::name`.
 */
void
gowl_workspace_set_name(GowlWorkspace *self, const gchar *name);

/**
 * gowl_workspace_get_tag_mask:
 * @self: a #GowlWorkspace
 *
 * Returns: the 32-bit tag bitmask associated with this workspace.
 *          Tags operate inside the workspace exactly as in
 *          standalone gowl.
 */
guint32
gowl_workspace_get_tag_mask(GowlWorkspace *self);

/**
 * gowl_workspace_set_tag_mask:
 * @self: a #GowlWorkspace
 * @mask: the new tag bitmask
 *
 * Changes the tag bitmask and emits `notify::tag-mask`.
 */
void
gowl_workspace_set_tag_mask(GowlWorkspace *self, guint32 mask);

G_END_DECLS

#endif /* GOWL_WORKSPACE_H */
