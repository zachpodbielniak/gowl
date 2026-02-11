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
};

/* Public dispatch functions */
gint gowl_bar_provider_get_bar_height (GowlBarProvider *self, gpointer monitor);
void gowl_bar_provider_render_bar     (GowlBarProvider *self, gpointer monitor);

G_END_DECLS

#endif /* GOWL_BAR_PROVIDER_H */
