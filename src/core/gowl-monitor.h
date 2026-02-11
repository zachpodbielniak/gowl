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
 * gowl_monitor_get_layout_symbol:
 * @self: a #GowlMonitor
 *
 * Returns the display symbol for the active layout.
 *
 * Returns: (transfer none) (nullable): the layout symbol string
 */
const gchar   *gowl_monitor_get_layout_symbol (GowlMonitor *self);

G_END_DECLS

#endif /* GOWL_MONITOR_H */
