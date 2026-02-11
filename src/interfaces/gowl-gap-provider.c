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

#include "gowl-gap-provider.h"

G_DEFINE_INTERFACE(GowlGapProvider, gowl_gap_provider, G_TYPE_OBJECT)

static void
gowl_gap_provider_default_init(GowlGapProviderInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_gap_provider_get_gaps:
 * @self: a #GowlGapProvider
 * @monitor: (nullable): the monitor to query gaps for
 * @inner_h: (out) (nullable): location to store horizontal inner gap
 * @inner_v: (out) (nullable): location to store vertical inner gap
 * @outer_h: (out) (nullable): location to store horizontal outer gap
 * @outer_v: (out) (nullable): location to store vertical outer gap
 *
 * Retrieves the gap configuration for the given monitor. Inner gaps
 * are between clients; outer gaps are between clients and screen edges.
 */
void
gowl_gap_provider_get_gaps(
	GowlGapProvider *self,
	gpointer         monitor,
	gint            *inner_h,
	gint            *inner_v,
	gint            *outer_h,
	gint            *outer_v
){
	GowlGapProviderInterface *iface;

	g_return_if_fail(GOWL_IS_GAP_PROVIDER(self));

	iface = GOWL_GAP_PROVIDER_GET_IFACE(self);
	if (iface->get_gaps != NULL)
		iface->get_gaps(self, monitor, inner_h, inner_v, outer_h, outer_v);
}
