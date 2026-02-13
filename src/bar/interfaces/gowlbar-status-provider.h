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

#ifndef GOWLBAR_STATUS_PROVIDER_H
#define GOWLBAR_STATUS_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_STATUS_PROVIDER (gowlbar_status_provider_get_type())

G_DECLARE_INTERFACE(GowlbarStatusProvider, gowlbar_status_provider,
                    GOWLBAR, STATUS_PROVIDER, GObject)

struct _GowlbarStatusProviderInterface {
	GTypeInterface parent_iface;

	gchar *(*get_status) (GowlbarStatusProvider *self);
};

gchar *gowlbar_status_provider_get_status(GowlbarStatusProvider *self);

G_END_DECLS

#endif /* GOWLBAR_STATUS_PROVIDER_H */
