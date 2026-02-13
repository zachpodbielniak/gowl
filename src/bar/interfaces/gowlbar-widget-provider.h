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

#ifndef GOWLBAR_WIDGET_PROVIDER_H
#define GOWLBAR_WIDGET_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_WIDGET_PROVIDER (gowlbar_widget_provider_get_type())

G_DECLARE_INTERFACE(GowlbarWidgetProvider, gowlbar_widget_provider,
                    GOWLBAR, WIDGET_PROVIDER, GObject)

struct _GowlbarWidgetProviderInterface {
	GTypeInterface parent_iface;

	GList *(*get_widgets) (GowlbarWidgetProvider *self);
};

GList *gowlbar_widget_provider_get_widgets(GowlbarWidgetProvider *self);

G_END_DECLS

#endif /* GOWLBAR_WIDGET_PROVIDER_H */
