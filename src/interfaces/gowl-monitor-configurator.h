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

#ifndef GOWL_MONITOR_CONFIGURATOR_H
#define GOWL_MONITOR_CONFIGURATOR_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_MONITOR_CONFIGURATOR (gowl_monitor_configurator_get_type())

G_DECLARE_INTERFACE(GowlMonitorConfigurator, gowl_monitor_configurator, GOWL, MONITOR_CONFIGURATOR, GObject)

struct _GowlMonitorConfiguratorInterface {
	GTypeInterface parent_iface;

	void (*configure_output) (GowlMonitorConfigurator *self, gpointer monitor);
};

/* Public dispatch functions */
void gowl_monitor_configurator_configure_output (GowlMonitorConfigurator *self, gpointer monitor);

G_END_DECLS

#endif /* GOWL_MONITOR_CONFIGURATOR_H */
