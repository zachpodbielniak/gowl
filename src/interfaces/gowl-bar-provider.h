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

#ifndef GOWL_BAR_PROVIDER_H
#define GOWL_BAR_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_BAR_PROVIDER (gowl_bar_provider_get_type())

G_DECLARE_INTERFACE(GowlBarProvider, gowl_bar_provider, GOWL, BAR_PROVIDER, GObject)

struct _GowlBarProviderInterface {
	GTypeInterface parent_iface;

	gint (*get_bar_height) (GowlBarProvider *self, gpointer monitor);
	void (*render_bar)     (GowlBarProvider *self, gpointer monitor);
	/* Optional: returns top and bottom insets for @monitor.  If not
	   implemented, the public dispatch falls back to get_bar_height
	   and returns it as the top inset, bottom = 0. */
	void (*get_bar_insets) (GowlBarProvider *self, gpointer monitor,
	                        gint *top, gint *bottom);
	/* Optional: hit-test monitor-local (@x, @y) to a 0-based tag
	   index, or -1 if the point is not over a tag box.  Powers
	   clickable tags. */
	gint (*tag_at)         (GowlBarProvider *self, gpointer monitor,
	                        gint x, gint y);
};

/* Public dispatch functions */
gint gowl_bar_provider_get_bar_height (GowlBarProvider *self, gpointer monitor);
void gowl_bar_provider_render_bar     (GowlBarProvider *self, gpointer monitor);

/**
 * gowl_bar_provider_tag_at:
 * @self: a #GowlBarProvider
 * @monitor: (nullable): the monitor whose bar to hit-test
 * @x: x coordinate relative to the monitor's left edge
 * @y: y coordinate relative to the monitor's top edge
 *
 * Maps a point in monitor-local coordinates to the 0-based tag index
 * whose indicator box contains it, or -1 if the point is not over a
 * tag box (or the provider does not implement the optional method).
 *
 * Returns: the 0-based tag index, or -1
 */
gint gowl_bar_provider_tag_at         (GowlBarProvider *self, gpointer monitor,
                                        gint x, gint y);

/**
 * gowl_bar_provider_get_bar_insets:
 * @self: a #GowlBarProvider
 * @monitor: (nullable): the monitor to query
 * @top: (out): top inset in pixels, or 0 if no top bar
 * @bottom: (out): bottom inset in pixels, or 0 if no bottom bar
 *
 * Returns the vertical space this provider reserves on the given
 * monitor.  Providers that implement only the legacy
 * @get_bar_height are transparently reported as top-only.
 */
void gowl_bar_provider_get_bar_insets (GowlBarProvider *self, gpointer monitor,
                                        gint *top, gint *bottom);

G_END_DECLS

#endif /* GOWL_BAR_PROVIDER_H */
