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

#ifndef GOWL_GAP_PROVIDER_H
#define GOWL_GAP_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_GAP_PROVIDER (gowl_gap_provider_get_type())

G_DECLARE_INTERFACE(GowlGapProvider, gowl_gap_provider, GOWL, GAP_PROVIDER, GObject)

struct _GowlGapProviderInterface {
	GTypeInterface parent_iface;

	void (*get_gaps) (GowlGapProvider *self, gpointer monitor, gint *inner_h, gint *inner_v, gint *outer_h, gint *outer_v);
};

/* Public dispatch functions */
void gowl_gap_provider_get_gaps (GowlGapProvider *self, gpointer monitor, gint *inner_h, gint *inner_v, gint *outer_h, gint *outer_v);

G_END_DECLS

#endif /* GOWL_GAP_PROVIDER_H */
