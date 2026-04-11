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

#include "gowl-screenshot-provider.h"

G_DEFINE_INTERFACE(GowlScreenshotProvider, gowl_screenshot_provider,
                   G_TYPE_OBJECT)

static void
gowl_screenshot_provider_default_init(
	GowlScreenshotProviderInterface *iface
){
	(void)iface;
}

void
gowl_screenshot_provider_capture(
	GowlScreenshotProvider  *self,
	GowlCaptureMode          mode,
	const gchar             *output_name,
	gpointer                 client,
	GowlScreenshotCallback   cb,
	gpointer                 user_data
){
	GowlScreenshotProviderInterface *iface;

	g_return_if_fail(GOWL_IS_SCREENSHOT_PROVIDER(self));
	g_return_if_fail(cb != NULL);

	iface = GOWL_SCREENSHOT_PROVIDER_GET_IFACE(self);
	g_return_if_fail(iface->capture != NULL);

	iface->capture(self, mode, output_name, client, cb, user_data);
}

gboolean
gowl_screenshot_provider_is_selecting(GowlScreenshotProvider *self)
{
	GowlScreenshotProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_SCREENSHOT_PROVIDER(self), FALSE);

	iface = GOWL_SCREENSHOT_PROVIDER_GET_IFACE(self);
	if (iface->is_selecting == NULL)
		return FALSE;

	return iface->is_selecting(self);
}

void
gowl_screenshot_provider_cancel(GowlScreenshotProvider *self)
{
	GowlScreenshotProviderInterface *iface;

	g_return_if_fail(GOWL_IS_SCREENSHOT_PROVIDER(self));

	iface = GOWL_SCREENSHOT_PROVIDER_GET_IFACE(self);
	if (iface->cancel != NULL)
		iface->cancel(self);
}
