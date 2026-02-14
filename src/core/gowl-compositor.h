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

#ifndef GOWL_COMPOSITOR_H
#define GOWL_COMPOSITOR_H

#include "gowl-types.h"
#include <wayland-server-core.h>

G_BEGIN_DECLS

#define GOWL_TYPE_COMPOSITOR (gowl_compositor_get_type())

G_DECLARE_FINAL_TYPE(GowlCompositor, gowl_compositor, GOWL, COMPOSITOR, GObject)

/**
 * gowl_compositor_new:
 *
 * Creates a new #GowlCompositor instance.  The compositor is the main
 * singleton that owns the Wayland display, wlroots backend, renderer,
 * allocator, and scene graph.
 *
 * Returns: (transfer full): a newly created #GowlCompositor
 */
GowlCompositor *gowl_compositor_new   (void);

/**
 * gowl_compositor_set_config:
 * @self: a #GowlCompositor
 * @config: (transfer none): the #GowlConfig to use
 *
 * Sets the configuration object.  Must be called before
 * gowl_compositor_start().  The compositor borrows the reference;
 * the caller retains ownership.
 */
void            gowl_compositor_set_config (GowlCompositor *self,
                                            GowlConfig     *config);

/**
 * gowl_compositor_get_config:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the current #GowlConfig
 */
GowlConfig     *gowl_compositor_get_config (GowlCompositor *self);

/**
 * gowl_compositor_set_module_manager:
 * @self: a #GowlCompositor
 * @mgr: (transfer none): the #GowlModuleManager to use
 *
 * Sets the module manager.  Must be called before
 * gowl_compositor_start().  The compositor borrows the reference.
 */
void            gowl_compositor_set_module_manager (GowlCompositor  *self,
                                                    GowlModuleManager *mgr);

/**
 * gowl_compositor_set_ipc:
 * @self: a #GowlCompositor
 * @ipc: (transfer none) (nullable): the #GowlIpc server to use
 *
 * Sets the IPC server.  The compositor borrows the reference and
 * will push state events (tags, layout, title, focus) to subscribed
 * clients whenever those change.  May be %NULL to disable IPC events.
 */
void            gowl_compositor_set_ipc (GowlCompositor *self,
                                         GowlIpc        *ipc);

/**
 * gowl_compositor_get_ipc:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the current #GowlIpc
 */
GowlIpc        *gowl_compositor_get_ipc (GowlCompositor *self);

/**
 * gowl_compositor_start:
 * @self: a #GowlCompositor
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Initialises the wlroots backend, renderer, allocator, scene graph,
 * Wayland protocols, input devices, and opens a Wayland socket.
 * On failure, @error is set and %FALSE is returned.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean        gowl_compositor_start (GowlCompositor  *self,
                                       GError          **error);

/**
 * gowl_compositor_run:
 * @self: a #GowlCompositor
 *
 * Enters the Wayland event loop.  This call blocks until
 * gowl_compositor_quit() is called.
 */
void            gowl_compositor_run   (GowlCompositor  *self);

/**
 * gowl_compositor_quit:
 * @self: a #GowlCompositor
 *
 * Requests the compositor to exit its event loop.
 */
void            gowl_compositor_quit  (GowlCompositor  *self);

/**
 * gowl_compositor_get_event_loop:
 * @self: a #GowlCompositor
 *
 * Returns the Wayland event loop used by the compositor.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the event loop
 */
struct wl_event_loop *gowl_compositor_get_event_loop (GowlCompositor *self);

/**
 * gowl_compositor_get_socket_name:
 * @self: a #GowlCompositor
 *
 * Returns the Wayland socket name (e.g. "wayland-0").
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the socket name string
 */
const gchar    *gowl_compositor_get_socket_name (GowlCompositor *self);

G_END_DECLS

#endif /* GOWL_COMPOSITOR_H */
