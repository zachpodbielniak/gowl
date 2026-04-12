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

#include "gowl-bar-provider.h"

G_DEFINE_INTERFACE(GowlBarProvider, gowl_bar_provider, G_TYPE_OBJECT)

static void
gowl_bar_provider_default_init(GowlBarProviderInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_bar_provider_get_bar_height:
 * @self: a #GowlBarProvider
 * @monitor: (nullable): the monitor to query bar height for
 *
 * Returns the height in pixels of the status bar for the given monitor.
 *
 * Returns: the bar height in pixels, or 0 if no bar
 */
gint
gowl_bar_provider_get_bar_height(
	GowlBarProvider *self,
	gpointer         monitor
){
	GowlBarProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_BAR_PROVIDER(self), 0);

	iface = GOWL_BAR_PROVIDER_GET_IFACE(self);
	if (iface->get_bar_height != NULL)
		return iface->get_bar_height(self, monitor);
	return 0;
}

/**
 * gowl_bar_provider_render_bar:
 * @self: a #GowlBarProvider
 * @monitor: (nullable): the monitor to render the bar on
 *
 * Renders the status bar on the given monitor.
 */
void
gowl_bar_provider_render_bar(
	GowlBarProvider *self,
	gpointer         monitor
){
	GowlBarProviderInterface *iface;

	g_return_if_fail(GOWL_IS_BAR_PROVIDER(self));

	iface = GOWL_BAR_PROVIDER_GET_IFACE(self);
	if (iface->render_bar != NULL)
		iface->render_bar(self, monitor);
}

void
gowl_bar_provider_get_bar_insets(
	GowlBarProvider *self,
	gpointer         monitor,
	gint            *top,
	gint            *bottom
){
	GowlBarProviderInterface *iface;
	gint t = 0, b = 0;

	g_return_if_fail(GOWL_IS_BAR_PROVIDER(self));

	iface = GOWL_BAR_PROVIDER_GET_IFACE(self);
	if (iface->get_bar_insets != NULL) {
		iface->get_bar_insets(self, monitor, &t, &b);
	} else if (iface->get_bar_height != NULL) {
		/* Back-compat: providers that only implement the legacy
		   scalar height are treated as top-only. */
		t = iface->get_bar_height(self, monitor);
	}

	if (top    != NULL) *top    = t;
	if (bottom != NULL) *bottom = b;
}
