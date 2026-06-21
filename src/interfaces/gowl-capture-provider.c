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

#include "gowl-capture-provider.h"

enum {
	SIGNAL_SOURCE_ADDED,
	SIGNAL_SOURCE_REMOVED,
	N_SIGNALS
};

static guint capture_provider_signals[N_SIGNALS];

G_DEFINE_INTERFACE(GowlCaptureProvider, gowl_capture_provider, G_TYPE_OBJECT)

static void
gowl_capture_provider_default_init(GowlCaptureProviderInterface *iface)
{
	(void)iface;

	/**
	 * GowlCaptureProvider::source-added:
	 * @self: the provider
	 * @source: (type GowlCaptureSource): the newly capturable source
	 *
	 * Emitted when a capturable monitor or window appears.
	 */
	capture_provider_signals[SIGNAL_SOURCE_ADDED] =
		g_signal_new("source-added",
		             GOWL_TYPE_CAPTURE_PROVIDER,
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_CAPTURE_SOURCE);

	/**
	 * GowlCaptureProvider::source-removed:
	 * @self: the provider
	 * @source: (type GowlCaptureSource): the source that went away
	 *
	 * Emitted when a previously capturable monitor or window disappears.
	 */
	capture_provider_signals[SIGNAL_SOURCE_REMOVED] =
		g_signal_new("source-removed",
		             GOWL_TYPE_CAPTURE_PROVIDER,
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_CAPTURE_SOURCE);
}

/**
 * gowl_capture_provider_get_capabilities:
 * @self: a #GowlCaptureProvider
 *
 * Returns the bitmask of capture source types this provider supports.
 *
 * Returns: a #GowlCaptureCapability bitmask.
 */
GowlCaptureCapability
gowl_capture_provider_get_capabilities(GowlCaptureProvider *self)
{
	GowlCaptureProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_CAPTURE_PROVIDER(self),
	                     GOWL_CAPTURE_CAP_NONE);

	iface = GOWL_CAPTURE_PROVIDER_GET_IFACE(self);
	if (iface->get_capabilities != NULL)
		return iface->get_capabilities(self);
	return GOWL_CAPTURE_CAP_NONE;
}

/**
 * gowl_capture_provider_supports_window_capture:
 * @self: a #GowlCaptureProvider
 *
 * Convenience predicate: %TRUE when this provider can capture
 * individual windows (the capability mask includes
 * %GOWL_CAPTURE_CAP_WINDOW).
 *
 * Returns: %TRUE if per-window capture is available.
 */
gboolean
gowl_capture_provider_supports_window_capture(GowlCaptureProvider *self)
{
	g_return_val_if_fail(GOWL_IS_CAPTURE_PROVIDER(self), FALSE);

	return (gowl_capture_provider_get_capabilities(self)
	        & GOWL_CAPTURE_CAP_WINDOW) != 0;
}

/**
 * gowl_capture_provider_create_globals:
 * @self: a #GowlCaptureProvider
 * @display: the struct wl_display (as a gpointer)
 *
 * Creates the provider's Wayland protocol globals.  Called once at
 * compositor startup.
 *
 * Returns: %TRUE on success.
 */
gboolean
gowl_capture_provider_create_globals(
	GowlCaptureProvider *self,
	gpointer             display
){
	GowlCaptureProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_CAPTURE_PROVIDER(self), FALSE);

	iface = GOWL_CAPTURE_PROVIDER_GET_IFACE(self);
	if (iface->create_globals != NULL)
		return iface->create_globals(self, display);
	return FALSE;
}

/**
 * gowl_capture_provider_list_sources:
 * @self: a #GowlCaptureProvider
 *
 * Lists the currently capturable sources.
 *
 * Returns: (element-type GowlCaptureSource) (transfer full): a list of
 *          #GowlCaptureSource; free with
 *          g_list_free_full(list, (GDestroyNotify)gowl_capture_source_free).
 */
GList *
gowl_capture_provider_list_sources(GowlCaptureProvider *self)
{
	GowlCaptureProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_CAPTURE_PROVIDER(self), NULL);

	iface = GOWL_CAPTURE_PROVIDER_GET_IFACE(self);
	if (iface->list_sources != NULL)
		return iface->list_sources(self);
	return NULL;
}

/**
 * gowl_capture_provider_add_window:
 * @self: a #GowlCaptureProvider
 * @client: the #GowlClient that mapped
 *
 * Registers a newly-mapped toplevel as a capturable window.  A no-op on
 * monitor-only providers.
 */
void
gowl_capture_provider_add_window(
	GowlCaptureProvider *self,
	gpointer             client
){
	GowlCaptureProviderInterface *iface;

	g_return_if_fail(GOWL_IS_CAPTURE_PROVIDER(self));

	iface = GOWL_CAPTURE_PROVIDER_GET_IFACE(self);
	if (iface->add_window != NULL)
		iface->add_window(self, client);
}

/**
 * gowl_capture_provider_remove_window:
 * @self: a #GowlCaptureProvider
 * @client: the #GowlClient that unmapped/was destroyed
 *
 * Unregisters a toplevel.  A no-op on monitor-only providers.
 */
void
gowl_capture_provider_remove_window(
	GowlCaptureProvider *self,
	gpointer             client
){
	GowlCaptureProviderInterface *iface;

	g_return_if_fail(GOWL_IS_CAPTURE_PROVIDER(self));

	iface = GOWL_CAPTURE_PROVIDER_GET_IFACE(self);
	if (iface->remove_window != NULL)
		iface->remove_window(self, client);
}

/**
 * gowl_capture_provider_update_window:
 * @self: a #GowlCaptureProvider
 * @client: the #GowlClient whose title/app_id changed
 *
 * Refreshes a toplevel's advertised title/app_id.  A no-op on
 * monitor-only providers.
 */
void
gowl_capture_provider_update_window(
	GowlCaptureProvider *self,
	gpointer             client
){
	GowlCaptureProviderInterface *iface;

	g_return_if_fail(GOWL_IS_CAPTURE_PROVIDER(self));

	iface = GOWL_CAPTURE_PROVIDER_GET_IFACE(self);
	if (iface->update_window != NULL)
		iface->update_window(self, client);
}
