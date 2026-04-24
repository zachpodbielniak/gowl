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

#ifndef GOWL_FRAME_WORKSPACE_MANAGER_H
#define GOWL_FRAME_WORKSPACE_MANAGER_H

#include <glib-object.h>
#include "../interfaces/gowl-workspace-provider.h"

G_BEGIN_DECLS

#define GOWL_TYPE_FRAME_WORKSPACE_MANAGER \
	(gowl_frame_workspace_manager_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlFrameWorkspaceManager,
                          gowl_frame_workspace_manager,
                          GOWL, FRAME_WORKSPACE_MANAGER, GObject)

/**
 * GowlFrameWorkspaceManagerClass:
 * @parent_class: the parent #GObjectClass
 *
 * Default #GowlWorkspaceProvider implementation.  Workspaces are
 * allocated with monotonic ids starting at 1; inactive workspaces
 * are held by strong references in a private hash table keyed by
 * id.  The "active" workspace is exposed as a property so Elisp
 * can observe switches via `notify::active-workspace`.
 *
 * The manager intentionally does not touch per-monitor compositor
 * state — emitting the `workspace-switched` signal on the
 * compositor is the consumer's job.  Wiring happens in
 * #gowl_compositor_set_workspace_provider so the signals fire from
 * a single authoritative source regardless of who triggered the
 * switch.
 */
struct _GowlFrameWorkspaceManagerClass {
	GObjectClass parent_class;
};

/**
 * gowl_frame_workspace_manager_new:
 *
 * Creates a new default workspace manager.  No workspaces exist
 * until #gowl_workspace_provider_create is called.
 *
 * Returns: (transfer full): a new manager
 */
GowlFrameWorkspaceManager *
gowl_frame_workspace_manager_new(void);

G_END_DECLS

#endif /* GOWL_FRAME_WORKSPACE_MANAGER_H */
