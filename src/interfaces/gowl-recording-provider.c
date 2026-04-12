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

#include "gowl-recording-provider.h"

G_DEFINE_INTERFACE(GowlRecordingProvider, gowl_recording_provider,
                   G_TYPE_OBJECT)

static void
gowl_recording_provider_default_init(
	GowlRecordingProviderInterface *iface
){
	(void)iface;
}

gboolean
gowl_recording_provider_start(
	GowlRecordingProvider *self,
	GowlCaptureMode        mode,
	const gchar           *output_name,
	gpointer               client,
	gint                   region_x,
	gint                   region_y,
	gint                   region_w,
	gint                   region_h,
	const gchar           *output_path,
	GError               **error
){
	GowlRecordingProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_RECORDING_PROVIDER(self), FALSE);

	iface = GOWL_RECORDING_PROVIDER_GET_IFACE(self);
	g_return_val_if_fail(iface->start != NULL, FALSE);

	return iface->start(self, mode, output_name, client,
	                    region_x, region_y, region_w, region_h,
	                    output_path, error);
}

gboolean
gowl_recording_provider_stop(
	GowlRecordingProvider  *self,
	gchar                 **output_path,
	GError                **error
){
	GowlRecordingProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_RECORDING_PROVIDER(self), FALSE);

	iface = GOWL_RECORDING_PROVIDER_GET_IFACE(self);
	g_return_val_if_fail(iface->stop != NULL, FALSE);

	return iface->stop(self, output_path, error);
}

gboolean
gowl_recording_provider_is_recording(GowlRecordingProvider *self)
{
	GowlRecordingProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_RECORDING_PROVIDER(self), FALSE);

	iface = GOWL_RECORDING_PROVIDER_GET_IFACE(self);
	if (iface->is_recording == NULL)
		return FALSE;

	return iface->is_recording(self);
}

void
gowl_recording_provider_finalize(GowlRecordingProvider *self)
{
	GowlRecordingProviderInterface *iface;

	g_return_if_fail(GOWL_IS_RECORDING_PROVIDER(self));

	iface = GOWL_RECORDING_PROVIDER_GET_IFACE(self);
	if (iface->finalize != NULL)
		iface->finalize(self);
}
