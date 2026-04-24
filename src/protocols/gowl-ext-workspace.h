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

#ifndef GOWL_EXT_WORKSPACE_H
#define GOWL_EXT_WORKSPACE_H

#include <glib-object.h>

struct wl_display;

G_BEGIN_DECLS

/**
 * GowlExtWorkspaceManager:
 *
 * Opaque handle returned by #gowl_ext_workspace_manager_register.
 * Holds the `ext_workspace_manager_v1` wl_global, tracks bound
 * clients, and bridges #GowlWorkspaceProvider events onto the
 * protocol.
 *
 * The manager listens to `workspace-created`, `workspace-switched`,
 * and `workspace-destroyed` on the owning #GowlCompositor and
 * translates each into the corresponding ext-workspace-v1
 * broadcasts.  Client `activate` requests invoke
 * #gowl_workspace_provider_switch_to on the installed provider.
 *
 * Built against the staging protocol definition shipped by
 * `wayland-protocols`; gowl's build system regenerates the
 * dispatcher headers via `wayland-scanner`.  When no workspace
 * provider is installed on the compositor, the global is still
 * advertised but broadcasts an empty workspace set — useful so
 * standalone / nested gowl can still coexist with a bar that
 * expects the protocol.
 */
typedef struct GowlExtWorkspaceManager GowlExtWorkspaceManager;

/**
 * gowl_ext_workspace_manager_register:
 * @compositor: a #GowlCompositor (passed as gpointer to avoid a
 *   circular header dep with `core/gowl-compositor.h`)
 * @display: the `wl_display` the global should be advertised on
 *
 * Creates the `ext_workspace_manager_v1` global and connects the
 * compositor workspace signals.  Returns an opaque handle that
 * the caller destroys via
 * #gowl_ext_workspace_manager_unregister at shutdown.
 *
 * Returns: (transfer full) (nullable): a handle, or %NULL if the
 *          global could not be created (e.g. display destroyed).
 */
GowlExtWorkspaceManager *
gowl_ext_workspace_manager_register(gpointer            compositor,
                                     struct wl_display *display);

/**
 * gowl_ext_workspace_manager_unregister:
 * @self: (nullable): a handle returned by
 *   #gowl_ext_workspace_manager_register
 *
 * Disconnects signal handlers, destroys the wl_global, and frees
 * @self.  Safe with %NULL.
 */
void
gowl_ext_workspace_manager_unregister(GowlExtWorkspaceManager *self);

G_END_DECLS

#endif /* GOWL_EXT_WORKSPACE_H */
