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

#ifndef GOWL_SCREENSHOT_PROVIDER_H
#define GOWL_SCREENSHOT_PROVIDER_H

#include <glib-object.h>
#include "gowl-enums.h"
#include "gowl-types.h"
#include "boxed/gowl-capture-result.h"

G_BEGIN_DECLS

#define GOWL_TYPE_SCREENSHOT_PROVIDER (gowl_screenshot_provider_get_type())

G_DECLARE_INTERFACE(GowlScreenshotProvider, gowl_screenshot_provider,
                    GOWL, SCREENSHOT_PROVIDER, GObject)

/**
 * GowlScreenshotCallback:
 * @result: (transfer full): the capture result
 * @user_data: (closure): data passed to the capture call
 *
 * Callback invoked when a screenshot capture completes (or is
 * cancelled).  The callee takes ownership of @result and must
 * free it with gowl_capture_result_free().
 */
typedef void (*GowlScreenshotCallback)(GowlCaptureResult *result,
                                       gpointer           user_data);

/**
 * GowlScreenshotProviderInterface:
 * @capture: Initiates a capture operation.  For %GOWL_CAPTURE_MODE_AREA
 *           this is asynchronous — the callback fires after the user
 *           completes or cancels the rubber-band selection.  For all
 *           other modes the callback fires synchronously before
 *           capture() returns.
 * @is_selecting: Returns %TRUE if an interactive area selection is
 *                currently in progress.
 * @cancel: Cancels an in-progress area selection.
 *
 * Interface for modules that provide screenshot functionality.
 * Implementors receive capture requests and deliver results via
 * the supplied callback.
 */
struct _GowlScreenshotProviderInterface {
	GTypeInterface parent_iface;

	/* vtable */
	void     (*capture)      (GowlScreenshotProvider  *self,
	                          GowlCaptureMode          mode,
	                          const gchar             *output_name,
	                          gpointer                 client,
	                          GowlScreenshotCallback   cb,
	                          gpointer                 user_data);
	gboolean (*is_selecting) (GowlScreenshotProvider  *self);
	void     (*cancel)       (GowlScreenshotProvider  *self);
};

/**
 * gowl_screenshot_provider_capture:
 * @self: a #GowlScreenshotProvider
 * @mode: the capture mode
 * @output_name: (nullable): output name for %GOWL_CAPTURE_MODE_DESKTOP
 * @client: (nullable): the #GowlClient for %GOWL_CAPTURE_MODE_WINDOW
 * @cb: (scope async): completion callback
 * @user_data: (closure cb): data for @cb
 *
 * Initiates a screen capture.
 */
void     gowl_screenshot_provider_capture      (GowlScreenshotProvider  *self,
                                                 GowlCaptureMode          mode,
                                                 const gchar             *output_name,
                                                 gpointer                 client,
                                                 GowlScreenshotCallback   cb,
                                                 gpointer                 user_data);

/**
 * gowl_screenshot_provider_is_selecting:
 * @self: a #GowlScreenshotProvider
 *
 * Returns: %TRUE if an interactive area selection is in progress
 */
gboolean gowl_screenshot_provider_is_selecting (GowlScreenshotProvider  *self);

/**
 * gowl_screenshot_provider_cancel:
 * @self: a #GowlScreenshotProvider
 *
 * Cancels an in-progress interactive area selection.  If no selection
 * is active, this is a no-op.
 */
void     gowl_screenshot_provider_cancel       (GowlScreenshotProvider  *self);

G_END_DECLS

#endif /* GOWL_SCREENSHOT_PROVIDER_H */
