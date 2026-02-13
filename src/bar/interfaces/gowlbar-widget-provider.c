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

#include "gowlbar-widget-provider.h"

G_DEFINE_INTERFACE(GowlbarWidgetProvider, gowlbar_widget_provider,
                   G_TYPE_OBJECT)

static void
gowlbar_widget_provider_default_init(
	GowlbarWidgetProviderInterface *iface
){
	(void)iface;
}

/**
 * gowlbar_widget_provider_get_widgets:
 * @self: a #GowlbarWidgetProvider
 *
 * Returns: (transfer container) (element-type GowlbarWidget):
 *   a list of widgets provided by this module
 */
GList *
gowlbar_widget_provider_get_widgets(GowlbarWidgetProvider *self)
{
	GowlbarWidgetProviderInterface *iface;

	g_return_val_if_fail(GOWLBAR_IS_WIDGET_PROVIDER(self), NULL);

	iface = GOWLBAR_WIDGET_PROVIDER_GET_IFACE(self);
	if (iface->get_widgets != NULL)
		return iface->get_widgets(self);
	return NULL;
}
