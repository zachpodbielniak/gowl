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

#ifndef GOWL_LAYOUT_REGISTRY_H
#define GOWL_LAYOUT_REGISTRY_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GowlCompositor GowlCompositor;
typedef struct _GowlMonitor GowlMonitor;

/**
 * GowlLayoutArrangeFunc:
 * @compositor: the compositor
 * @monitor: the monitor being arranged
 *
 * A built-in layout.  Called with the monitor's window area already
 * computed and its gaps already applied by the caller's helpers; the
 * function walks the visible tiling clients and places them.
 */
typedef void (*GowlLayoutArrangeFunc) (GowlCompositor *compositor,
                                        GowlMonitor    *monitor);

/**
 * GowlLayoutEntry:
 * @name: the name a config or a keybind uses, e.g. "tile"
 * @symbol: the short indicator a bar renders, e.g. "[]="
 * @arrange: (nullable): a built-in arrange function
 * @provider: (nullable): a module implementing #GowlLayoutProvider
 *
 * One entry in the layout registry.  Exactly one of @arrange and
 * @provider is set: built-ins are plain functions inside the
 * compositor, module layouts are objects.  Both are peers here, which
 * is the point --- a module layout is selectable by name exactly like
 * `tile', rather than being a second-class thing bolted beside it.
 */
typedef struct {
	gchar                 *name;
	gchar                 *symbol;
	GowlLayoutArrangeFunc  arrange;
	gpointer               provider;
} GowlLayoutEntry;

/* The built-in layouts.  Defined in gowl-compositor.c, where the client
 * list and resize path live; declared here because the registry is what
 * holds pointers to them. */
void gowl_compositor_layout_tile      (GowlCompositor *self, GowlMonitor *m);
void gowl_compositor_layout_monocle   (GowlCompositor *self, GowlMonitor *m);
void gowl_compositor_layout_float     (GowlCompositor *self, GowlMonitor *m);
void gowl_compositor_layout_scrolling (GowlCompositor *self, GowlMonitor *m);

/**
 * gowl_layout_registry_init:
 * @self: a #GowlCompositor
 *
 * Creates the registry and adds the built-in layouts.  Called once
 * during startup, before any monitor exists.
 */
void gowl_layout_registry_init (GowlCompositor *self);

/**
 * gowl_layout_registry_finish:
 * @self: a #GowlCompositor
 *
 * Frees the registry.
 */
void gowl_layout_registry_finish (GowlCompositor *self);

/**
 * gowl_layout_register:
 * @self: a #GowlCompositor
 * @name: the layout's name; must be unique
 * @symbol: the short indicator for a bar
 * @arrange: (nullable): a built-in arrange function
 * @provider: (nullable): a #GowlLayoutProvider module
 *
 * Adds a layout.  Re-registering an existing @name replaces it, so a
 * module can override a built-in by taking its name.
 *
 * Returns: %TRUE when the layout was added or replaced.
 */
gboolean gowl_layout_register (GowlCompositor        *self,
                                const gchar           *name,
                                const gchar           *symbol,
                                GowlLayoutArrangeFunc  arrange,
                                gpointer               provider);

/**
 * gowl_layout_unregister:
 * @self: a #GowlCompositor
 * @name: the layout to remove
 *
 * Removes a layout.  A monitor currently using it falls back to the
 * first registered layout, so unloading a layout module cannot leave a
 * monitor pointing at nothing.
 *
 * Returns: %TRUE when a layout was removed.
 */
gboolean gowl_layout_unregister (GowlCompositor *self, const gchar *name);

/**
 * gowl_layout_lookup:
 * @self: a #GowlCompositor
 * @name: the layout name
 *
 * Returns: (transfer none) (nullable): the entry, or %NULL.
 */
GowlLayoutEntry *gowl_layout_lookup (GowlCompositor *self,
                                      const gchar    *name);

/**
 * gowl_layout_list:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer container) (element-type utf8): every registered
 *   layout name, in registration order.  Free the list, not the
 *   strings.
 */
GList *gowl_layout_list (GowlCompositor *self);

/**
 * gowl_layout_set:
 * @self: a #GowlCompositor
 * @monitor: (nullable): the monitor, or %NULL for the selected one
 * @name: the layout to select
 *
 * Selects a layout by name and re-arranges.
 *
 * Returns: %TRUE when @name named a registered layout.
 */
gboolean gowl_layout_set (GowlCompositor *self,
                           GowlMonitor    *monitor,
                           const gchar    *name);

/**
 * gowl_layout_cycle:
 * @self: a #GowlCompositor
 * @monitor: (nullable): the monitor, or %NULL for the selected one
 * @step: how far to move through the registry; negative goes back
 *
 * Moves to another layout, wrapping at both ends.
 *
 * Returns: (transfer none) (nullable): the newly selected layout name.
 */
const gchar *gowl_layout_cycle (GowlCompositor *self,
                                 GowlMonitor    *monitor,
                                 gint            step);

/**
 * gowl_layout_get:
 * @self: a #GowlCompositor
 * @monitor: (nullable): the monitor, or %NULL for the selected one
 *
 * Returns: (transfer none) (nullable): the monitor's layout entry.
 */
GowlLayoutEntry *gowl_layout_get (GowlCompositor *self,
                                   GowlMonitor    *monitor);

/**
 * gowl_layout_apply:
 * @self: a #GowlCompositor
 * @monitor: the monitor to arrange
 *
 * Runs the monitor's selected layout and updates its symbol.  Called
 * from gowl_compositor_arrange().
 */
void gowl_layout_apply (GowlCompositor *self, GowlMonitor *monitor);

/**
 * gowl_layout_adopt_providers:
 * @self: a #GowlCompositor
 *
 * Registers every module that implements #GowlLayoutProvider, using
 * the module's own name and symbol.  Called after modules load and
 * again on reload, so a layout module becomes selectable the moment it
 * is active.
 *
 * Returns: how many were adopted.
 */
guint gowl_layout_adopt_providers (GowlCompositor *self);

G_END_DECLS

#endif /* GOWL_LAYOUT_REGISTRY_H */
