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

#ifndef GOWL_CLIENT_H
#define GOWL_CLIENT_H

#include <glib-object.h>
#include <sys/types.h>

struct wlr_surface;

G_BEGIN_DECLS

#define GOWL_TYPE_CLIENT (gowl_client_get_type())

G_DECLARE_FINAL_TYPE(GowlClient, gowl_client, GOWL, CLIENT, GObject)

GowlClient    *gowl_client_new               (void);

/* Tag accessors */
guint32        gowl_client_get_tags           (GowlClient  *self);
void           gowl_client_set_tags           (GowlClient  *self,
                                               guint32      tags);

/* State accessors */
gboolean       gowl_client_get_floating       (GowlClient  *self);
void           gowl_client_set_floating       (GowlClient  *self,
                                               gboolean     floating);

gboolean       gowl_client_get_fullscreen     (GowlClient  *self);
void           gowl_client_set_fullscreen     (GowlClient  *self,
                                               gboolean     fullscreen);

gboolean       gowl_client_get_urgent         (GowlClient  *self);
void           gowl_client_set_urgent         (GowlClient  *self,
                                               gboolean     urgent);

/* String accessors */
const gchar   *gowl_client_get_title          (GowlClient  *self);
void           gowl_client_set_title          (GowlClient  *self,
                                               const gchar *title);

const gchar   *gowl_client_get_app_id         (GowlClient  *self);
void           gowl_client_set_app_id         (GowlClient  *self,
                                               const gchar *app_id);

/* Geometry accessors */
void           gowl_client_get_geometry       (GowlClient  *self,
                                               gint        *x,
                                               gint        *y,
                                               gint        *width,
                                               gint        *height);
void           gowl_client_set_geometry       (GowlClient  *self,
                                               gint         x,
                                               gint         y,
                                               gint         width,
                                               gint         height);

/* Monitor accessor */
gpointer       gowl_client_get_monitor        (GowlClient  *self);
void           gowl_client_set_monitor        (GowlClient  *self,
                                               gpointer     monitor);

/**
 * gowl_client_get_id:
 * @self: a #GowlClient
 *
 * Returns a unique identifier for this client.  The ID is stable
 * for the lifetime of the client object and is never reused.
 *
 * Returns: the unique client ID
 */
guint          gowl_client_get_id             (GowlClient  *self);

/**
 * gowl_client_close:
 * @self: a #GowlClient
 *
 * Sends a close request to the client's XDG toplevel.  The client
 * may choose to ignore the request (e.g. to show an "unsaved" dialog).
 */
void           gowl_client_close              (GowlClient  *self);

/**
 * gowl_client_get_pid:
 * @self: a #GowlClient
 *
 * Returns the PID of the process that owns this client's Wayland
 * connection, obtained via wl_client_get_credentials().
 *
 * Returns: the process ID, or -1 if unavailable
 */
pid_t          gowl_client_get_pid            (GowlClient  *self);

/**
 * gowl_client_get_process_info:
 * @self: a #GowlClient
 *
 * Returns process information (pid, comm, cmdline, cwd) for the
 * process that owns this client's Wayland connection.
 *
 * Returns: (transfer full) (nullable): a #GowlProcessInfo, or %NULL
 *          if the PID is unavailable.  Free with gowl_process_info_free().
 */
GowlProcessInfo *gowl_client_get_process_info  (GowlClient  *self);

/**
 * gowl_client_get_wlr_surface:
 * @self: a #GowlClient
 *
 * Returns the underlying wlr_surface for this client.  Useful for
 * screenshot and surface inspection operations.
 *
 * Returns: (transfer none) (nullable): the struct wlr_surface, or %NULL
 */
struct wlr_surface *gowl_client_get_wlr_surface (GowlClient *self);

/**
 * gowl_client_get_scene:
 * @self: a #GowlClient
 *
 * Returns the client's top-level scene tree container node.
 * Border decorations and the surface tree are children of this node.
 *
 * Returns: (transfer none) (nullable): the wlr_scene_tree
 */
struct wlr_scene_tree *gowl_client_get_scene    (GowlClient *self);

/**
 * gowl_client_get_border_width:
 * @self: a #GowlClient
 *
 * Returns: the border width in pixels
 */
guint              gowl_client_get_border_width  (GowlClient  *self);

/**
 * gowl_client_set_border_width:
 * @self: a #GowlClient
 * @width: new border width in pixels
 *
 * Sets the border width and updates the border scene rects.
 */
void               gowl_client_set_border_width  (GowlClient  *self,
                                                   guint        width);

/**
 * gowl_client_set_visible:
 * @self: a #GowlClient
 * @visible: %TRUE to show, %FALSE to hide
 *
 * Shows or hides the client without destroying it.
 */
void               gowl_client_set_visible       (GowlClient  *self,
                                                   gboolean     visible);

/**
 * gowl_client_get_embedded:
 * @self: a #GowlClient
 *
 * Returns whether this client is externally managed (embedded).
 * Embedded clients are skipped by the tiling arrange pass — the
 * embedder controls their position, visibility, and scene layer.
 *
 * Returns: %TRUE if the client is embedded
 */
gboolean           gowl_client_get_embedded      (GowlClient  *self);

/**
 * gowl_client_set_embedded:
 * @self: a #GowlClient
 * @embedded: %TRUE to mark as embedded
 *
 * Marks the client as externally managed.  When embedded, the
 * compositor's arrange() will not reparent, show/hide, or
 * tile the client.
 */
void               gowl_client_set_embedded      (GowlClient  *self,
                                                   gboolean     embedded);

/**
 * gowl_client_get_alpha:
 * @self: a #GowlClient
 *
 * Returns the opacity of this client.  The value ranges from
 * 0.0 (fully transparent) to 1.0 (fully opaque).
 *
 * Returns: the alpha value
 */
gfloat             gowl_client_get_alpha         (GowlClient  *self);

/**
 * gowl_client_set_alpha:
 * @self: a #GowlClient
 * @alpha: opacity value between 0.0 and 1.0
 *
 * Sets the opacity of this client by walking its scene tree and
 * calling wlr_scene_buffer_set_opacity() on each buffer node.
 * Values are clamped to the [0.0, 1.0] range.
 */
void               gowl_client_set_alpha         (GowlClient  *self,
                                                   gfloat       alpha);

/**
 * gowl_client_set_rule_overrides:
 * @self: a #GowlClient
 * @tags: tag bitmask to stash (0 = no override)
 * @monitor: monitor index to stash (-1 = no override)
 * @geom_set: %TRUE if the caller also set geometry via
 *            gowl_client_set_geometry() and wants the compositor
 *            to honour it instead of centering
 *
 * Stashes initial-placement overrides for consumption by the
 * compositor's on_client_map() path, which reads them after the
 * @client-pre-map signal returns and before calling setmon().
 *
 * Intended as the only public entry point for modules that
 * implement placement policy (e.g. the windowrules module) —
 * writing the fields directly would require access to the
 * private client struct.
 */
void gowl_client_set_rule_overrides (GowlClient *self,
                                      guint32     tags,
                                      gint        monitor,
                                      gboolean    geom_set);

G_END_DECLS

#endif /* GOWL_CLIENT_H */
