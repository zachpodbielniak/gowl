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

#ifndef GOWLBAR_OUTPUT_H
#define GOWLBAR_OUTPUT_H

#include <glib-object.h>
#include <wayland-client.h>

#include "gowlbar-config.h"
#include "gowlbar-widget.h"

/* layer-shell types (defined in generated protocol header) */
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

G_BEGIN_DECLS

#define GOWLBAR_TYPE_OUTPUT (gowlbar_output_get_type())

G_DECLARE_FINAL_TYPE(GowlbarOutput, gowlbar_output, GOWLBAR, OUTPUT, GObject)

/**
 * gowlbar_output_new:
 * @wl_output: the Wayland output
 * @name: human-readable output name
 * @global_name: registry global name
 *
 * Creates a new per-output bar surface.
 *
 * Returns: (transfer full): a new #GowlbarOutput
 */
GowlbarOutput *gowlbar_output_new(struct wl_output *wl_output,
                                    const gchar      *name,
                                    guint32           global_name);

/**
 * gowlbar_output_setup_surface:
 * @self: the bar output
 * @compositor: the wl_compositor
 * @layer_shell: the layer shell
 * @shm: the wl_shm
 * @height: bar height in pixels
 *
 * Creates the wl_surface, layer surface, and requests the
 * appropriate anchoring and exclusive zone.
 */
void gowlbar_output_setup_surface(GowlbarOutput              *self,
                                   struct wl_compositor        *compositor,
                                   struct zwlr_layer_shell_v1  *layer_shell,
                                   struct wl_shm               *shm,
                                   gint                         height);

/**
 * gowlbar_output_render:
 * @self: the bar output
 *
 * Renders the bar content into the wl_shm buffer and commits.
 */
void gowlbar_output_render(GowlbarOutput *self);

/**
 * gowlbar_output_get_wl_output:
 * @self: the bar output
 *
 * Returns: (transfer none): the underlying wl_output
 */
struct wl_output *gowlbar_output_get_wl_output(GowlbarOutput *self);

/**
 * gowlbar_output_get_name:
 * @self: the bar output
 *
 * Returns: (transfer none): the output name
 */
const gchar *gowlbar_output_get_name(GowlbarOutput *self);

/**
 * gowlbar_output_get_global_name:
 * @self: the bar output
 *
 * Returns: the registry global name
 */
guint32 gowlbar_output_get_global_name(GowlbarOutput *self);

/**
 * gowlbar_output_set_config:
 * @self: the bar output
 * @config: (transfer none): the bar configuration
 *
 * Sets the configuration used for rendering.
 */
void gowlbar_output_set_config(GowlbarOutput *self, GowlbarConfig *config);

/**
 * gowlbar_output_set_widgets:
 * @self: the bar output
 * @widgets: (element-type GowlbarWidget) (transfer none): widget list
 *
 * Sets the list of widgets to render.  The output borrows the list
 * and does not take ownership.
 */
void gowlbar_output_set_widgets(GowlbarOutput *self, GList *widgets);

G_END_DECLS

#endif /* GOWLBAR_OUTPUT_H */
