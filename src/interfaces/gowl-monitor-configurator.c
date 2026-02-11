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

#include "gowl-monitor-configurator.h"

G_DEFINE_INTERFACE(GowlMonitorConfigurator, gowl_monitor_configurator, G_TYPE_OBJECT)

static void
gowl_monitor_configurator_default_init(GowlMonitorConfiguratorInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_monitor_configurator_configure_output:
 * @self: a #GowlMonitorConfigurator
 * @monitor: (nullable): the monitor/output to configure
 *
 * Configures a monitor output, setting resolution, refresh rate,
 * position, transform, and scale as needed.
 */
void
gowl_monitor_configurator_configure_output(
	GowlMonitorConfigurator *self,
	gpointer                 monitor
){
	GowlMonitorConfiguratorInterface *iface;

	g_return_if_fail(GOWL_IS_MONITOR_CONFIGURATOR(self));

	iface = GOWL_MONITOR_CONFIGURATOR_GET_IFACE(self);
	if (iface->configure_output != NULL)
		iface->configure_output(self, monitor);
}
