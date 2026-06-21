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

#ifndef GOWL_MONITOR_H
#define GOWL_MONITOR_H

#include <glib-object.h>
#include "../boxed/gowl-geometry.h"

struct wlr_output;
struct wlr_scene_output;

G_BEGIN_DECLS

#define GOWL_TYPE_MONITOR (gowl_monitor_get_type())

G_DECLARE_FINAL_TYPE(GowlMonitor, gowl_monitor, GOWL, MONITOR, GObject)

/**
 * gowl_monitor_new:
 *
 * Creates a new #GowlMonitor.
 *
 * Returns: (transfer full): a newly created #GowlMonitor
 */
GowlMonitor   *gowl_monitor_new              (void);

/**
 * gowl_monitor_emit_usable_area_changed:
 * @self: a #GowlMonitor
 * @old_x: previous usable area x (monitor-local pixels)
 * @old_y: previous usable area y
 * @old_w: previous usable area width
 * @old_h: previous usable area height
 * @new_x: new usable area x
 * @new_y: new usable area y
 * @new_w: new usable area width
 * @new_h: new usable area height
 *
 * Emits the `usable-area-changed` signal with two #GowlGeometry
 * arguments.  Call this from the compositor after the window-area
 * computation in `gowl_compositor_arrangelayers` detects a change.
 *
 * Listeners (notably cmacs `--gowl` to reflow embedded app
 * buffers) can connect to the signal; standalone and nested
 * consumers that don't connect pay nothing.
 */
void
gowl_monitor_emit_usable_area_changed(GowlMonitor *self,
                                       gint old_x, gint old_y,
                                       gint old_w, gint old_h,
                                       gint new_x, gint new_y,
                                       gint new_w, gint new_h);

/**
 * gowl_monitor_get_tags:
 * @self: a #GowlMonitor
 *
 * Returns the currently active tag bitmask (from the selected tag set).
 *
 * Returns: the active tag bitmask
 */
guint32        gowl_monitor_get_tags          (GowlMonitor *self);

/**
 * gowl_monitor_set_tags:
 * @self: a #GowlMonitor
 * @tags: the new tag bitmask
 *
 * Replaces the active tag set with @tags.
 */
void           gowl_monitor_set_tags          (GowlMonitor *self,
                                               guint32      tags);

/**
 * gowl_monitor_toggle_tag:
 * @self: a #GowlMonitor
 * @tag: tag bit to toggle
 *
 * Toggles a single tag bit in the active tag set.
 */
void           gowl_monitor_toggle_tag        (GowlMonitor *self,
                                               guint32      tag);

/**
 * gowl_monitor_get_mfact:
 * @self: a #GowlMonitor
 *
 * Returns the master area factor.
 *
 * Returns: the master factor (0.0 - 1.0)
 */
gdouble        gowl_monitor_get_mfact         (GowlMonitor *self);

/**
 * gowl_monitor_set_mfact:
 * @self: a #GowlMonitor
 * @mfact: new master factor
 *
 * Sets the master area factor.
 */
void           gowl_monitor_set_mfact         (GowlMonitor *self,
                                               gdouble      mfact);

/**
 * gowl_monitor_get_nmaster:
 * @self: a #GowlMonitor
 *
 * Returns the number of windows in the master area.
 *
 * Returns: the master count
 */
gint           gowl_monitor_get_nmaster       (GowlMonitor *self);

/**
 * gowl_monitor_set_nmaster:
 * @self: a #GowlMonitor
 * @nmaster: the new master count
 *
 * Sets the number of windows in the master area.
 */
void           gowl_monitor_set_nmaster       (GowlMonitor *self,
                                               gint         nmaster);

/**
 * gowl_monitor_get_vsplit:
 * @self: a #GowlMonitor
 *
 * Returns whether the tile layout uses the vsplit orientation.
 *
 * Returns: TRUE if vsplit (master on top) is enabled
 */
gboolean       gowl_monitor_get_vsplit        (GowlMonitor *self);

/**
 * gowl_monitor_set_vsplit:
 * @self: a #GowlMonitor
 * @vsplit: TRUE for vsplit (master on top), FALSE for normal (master left)
 *
 * Sets the tile split orientation.  Caller must arrange afterwards.
 */
void           gowl_monitor_set_vsplit        (GowlMonitor *self,
                                               gboolean     vsplit);

/**
 * gowl_monitor_get_layout_symbol:
 * @self: a #GowlMonitor
 *
 * Returns the display symbol for the active layout.
 *
 * Returns: (transfer none) (nullable): the layout symbol string
 */
const gchar   *gowl_monitor_get_layout_symbol (GowlMonitor *self);

/**
 * gowl_monitor_get_name:
 * @self: a #GowlMonitor
 *
 * Returns the output name (e.g. "eDP-1", "HDMI-A-1").
 *
 * Returns: (transfer none) (nullable): the output name string
 */
const gchar   *gowl_monitor_get_name          (GowlMonitor *self);

/**
 * gowl_monitor_get_geometry:
 * @self: a #GowlMonitor
 * @x: (out) (nullable): return location for x coordinate
 * @y: (out) (nullable): return location for y coordinate
 * @width: (out) (nullable): return location for width
 * @height: (out) (nullable): return location for height
 *
 * Returns the monitor's layout-relative geometry.
 */
void           gowl_monitor_get_geometry      (GowlMonitor *self,
                                               gint        *x,
                                               gint        *y,
                                               gint        *width,
                                               gint        *height);

/**
 * gowl_monitor_get_window_area:
 * @self: a #GowlMonitor
 * @x: (out) (nullable): usable area x
 * @y: (out) (nullable): usable area y
 * @width: (out) (nullable): usable area width
 * @height: (out) (nullable): usable area height
 *
 * Returns the usable window area after subtracting exclusive
 * zones (layer-shell, bar height).
 */
void           gowl_monitor_get_window_area   (GowlMonitor *self,
                                               gint        *x,
                                               gint        *y,
                                               gint        *width,
                                               gint        *height);

/**
 * gowl_monitor_get_wlr_output:
 * @self: a #GowlMonitor
 *
 * Returns the underlying wlr_output for this monitor.
 *
 * Returns: (transfer none) (nullable): the struct wlr_output, or %NULL
 */
struct wlr_output *gowl_monitor_get_wlr_output (GowlMonitor *self);

/**
 * gowl_monitor_get_scene_output:
 * @self: a #GowlMonitor
 *
 * Returns the wlr_scene_output for this monitor.
 * Only valid after the monitor is attached to the scene.
 *
 * Returns: (transfer none) (nullable): the wlr_scene_output, or %NULL
 */
struct wlr_scene_output *gowl_monitor_get_scene_output (GowlMonitor *self);

/**
 * gowl_monitor_get_modes:
 * @self: a #GowlMonitor
 *
 * Returns a list of available output modes.
 *
 * Returns: (transfer full) (element-type GowlOutputMode): available modes
 */
GList          *gowl_monitor_get_modes         (GowlMonitor *self);

/**
 * gowl_monitor_get_current_mode:
 * @self: a #GowlMonitor
 *
 * Returns the currently active output mode.
 *
 * Returns: (transfer full) (nullable): the current mode, or %NULL
 */
GowlOutputMode *gowl_monitor_get_current_mode  (GowlMonitor *self);

/**
 * gowl_monitor_set_mode:
 * @self: a #GowlMonitor
 * @width: horizontal resolution in pixels
 * @height: vertical resolution in pixels
 * @refresh_mhz: refresh rate in millihertz (e.g. 60000 for 60 Hz)
 *
 * Sets the output mode.  Matches against advertised modes first,
 * falls back to a custom mode if no exact match.
 *
 * Returns: %TRUE on success
 */
gboolean        gowl_monitor_set_mode          (GowlMonitor *self,
                                                gint         width,
                                                gint         height,
                                                gint         refresh_mhz);

/**
 * gowl_monitor_get_position:
 * @self: a #GowlMonitor
 * @x: (out) (nullable): return location for x coordinate
 * @y: (out) (nullable): return location for y coordinate
 *
 * Returns the layout-relative position of the monitor.
 */
void            gowl_monitor_get_position      (GowlMonitor *self,
                                                gint        *x,
                                                gint        *y);

/**
 * gowl_monitor_set_position:
 * @self: a #GowlMonitor
 * @x: x coordinate in layout space
 * @y: y coordinate in layout space
 *
 * Sets the monitor's position in the output layout.
 * Switches from auto-layout to manual positioning.
 *
 * Returns: %TRUE on success
 */
gboolean        gowl_monitor_set_position      (GowlMonitor *self,
                                                gint         x,
                                                gint         y);

/**
 * gowl_monitor_get_enabled:
 * @self: a #GowlMonitor
 *
 * Returns whether the output is enabled.
 *
 * Returns: %TRUE if the output is enabled
 */
gboolean        gowl_monitor_get_enabled       (GowlMonitor *self);

/**
 * gowl_monitor_set_enabled:
 * @self: a #GowlMonitor
 * @enabled: whether to enable the output
 *
 * Enables or disables the output.
 *
 * Returns: %TRUE on success
 */
gboolean        gowl_monitor_set_enabled       (GowlMonitor *self,
                                                gboolean     enabled);

/**
 * gowl_monitor_get_scale:
 * @self: a #GowlMonitor
 *
 * Returns the output scale factor.
 *
 * Returns: the scale factor
 */
gdouble         gowl_monitor_get_scale         (GowlMonitor *self);

/**
 * gowl_monitor_set_scale:
 * @self: a #GowlMonitor
 * @scale: scale factor (e.g. 1.0, 1.5, 2.0)
 *
 * Sets the output scale factor.
 *
 * Returns: %TRUE on success
 */
gboolean        gowl_monitor_set_scale         (GowlMonitor *self,
                                                gdouble      scale);

/**
 * gowl_monitor_get_transform:
 * @self: a #GowlMonitor
 *
 * Returns the output transform (rotation/flip).
 * Values match `enum wl_output_transform`:
 * 0=normal, 1=90, 2=180, 3=270, 4=flipped, 5-7=flipped+rotation.
 *
 * Returns: the transform value
 */
gint            gowl_monitor_get_transform     (GowlMonitor *self);

/**
 * gowl_monitor_set_transform:
 * @self: a #GowlMonitor
 * @transform: transform value (0-7, see `enum wl_output_transform`)
 *
 * Sets the output transform (rotation/flip).
 *
 * Returns: %TRUE on success
 */
gboolean        gowl_monitor_set_transform     (GowlMonitor *self,
                                                gint         transform);

/**
 * gowl_monitor_get_layer_surfaces:
 * @self: a #GowlMonitor
 *
 * Returns the list of layer surfaces on this monitor.
 *
 * Returns: (transfer none) (element-type GowlLayerSurface): the layer
 *   surface list, owned by the monitor
 */
GList          *gowl_monitor_get_layer_surfaces (GowlMonitor *self);

G_END_DECLS

#endif /* GOWL_MONITOR_H */
