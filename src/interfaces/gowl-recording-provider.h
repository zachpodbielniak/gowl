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

#ifndef GOWL_RECORDING_PROVIDER_H
#define GOWL_RECORDING_PROVIDER_H

#include <glib-object.h>
#include "gowl-enums.h"
#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_RECORDING_PROVIDER (gowl_recording_provider_get_type())

G_DECLARE_INTERFACE(GowlRecordingProvider, gowl_recording_provider,
                    GOWL, RECORDING_PROVIDER, GObject)

/**
 * GowlRecordingProviderInterface:
 * @start: Start continuous screen recording.  Returns %TRUE on success.
 *         For %GOWL_CAPTURE_MODE_AREA the region coordinates are
 *         provided via @region_x, @region_y, @region_w, @region_h
 *         (obtained from a prior screenshot area selection).
 * @stop:  Stop a recording in progress.  Stores the output file path
 *         in @output_path (caller frees).  Returns %TRUE on success.
 * @is_recording: Returns %TRUE if a recording is currently active.
 *
 * Interface for modules that provide screen recording functionality.
 * Recording captures frames continuously and encodes them to a video
 * file (typically via an ffmpeg subprocess).
 */
struct _GowlRecordingProviderInterface {
	GTypeInterface parent_iface;

	/* vtable */
	gboolean (*start)        (GowlRecordingProvider *self,
	                          GowlCaptureMode        mode,
	                          const gchar           *output_name,
	                          gpointer               client,
	                          gint                   region_x,
	                          gint                   region_y,
	                          gint                   region_w,
	                          gint                   region_h,
	                          const gchar           *output_path,
	                          GError               **error);
	gboolean (*stop)         (GowlRecordingProvider *self,
	                          gchar                **output_path,
	                          GError               **error);
	gboolean (*is_recording) (GowlRecordingProvider *self);
	void     (*finalize)     (GowlRecordingProvider *self);
};

/**
 * gowl_recording_provider_start:
 * @self: a #GowlRecordingProvider
 * @mode: the capture mode
 * @output_name: (nullable): output name for %GOWL_CAPTURE_MODE_DESKTOP
 * @client: (nullable): the #GowlClient for %GOWL_CAPTURE_MODE_WINDOW
 * @region_x: region X offset (only for %GOWL_CAPTURE_MODE_AREA)
 * @region_y: region Y offset (only for %GOWL_CAPTURE_MODE_AREA)
 * @region_w: region width (only for %GOWL_CAPTURE_MODE_AREA)
 * @region_h: region height (only for %GOWL_CAPTURE_MODE_AREA)
 * @output_path: (nullable): output file path, or %NULL for default
 * @error: (nullable): return location for a #GError
 *
 * Starts continuous screen recording.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gowl_recording_provider_start        (GowlRecordingProvider *self,
                                                GowlCaptureMode        mode,
                                                const gchar           *output_name,
                                                gpointer               client,
                                                gint                   region_x,
                                                gint                   region_y,
                                                gint                   region_w,
                                                gint                   region_h,
                                                const gchar           *output_path,
                                                GError               **error);

/**
 * gowl_recording_provider_stop:
 * @self: a #GowlRecordingProvider
 * @output_path: (out) (transfer full) (nullable): receives the
 *               output file path.  Caller must g_free().
 * @error: (nullable): return location for a #GError
 *
 * Stops the current recording.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gowl_recording_provider_stop         (GowlRecordingProvider *self,
                                                gchar                **output_path,
                                                GError               **error);

/**
 * gowl_recording_provider_is_recording:
 * @self: a #GowlRecordingProvider
 *
 * Returns: %TRUE if a recording is currently active
 */
gboolean gowl_recording_provider_is_recording (GowlRecordingProvider *self);

/**
 * gowl_recording_provider_finalize:
 * @self: a #GowlRecordingProvider
 *
 * Blocks until any recording subprocess (e.g. ffmpeg) started by
 * stop() has fully exited and finalized its output file.  This
 * is a separate step from stop() so that the caller can release
 * any locks held during the fast cleanup before blocking on the
 * potentially slow finalization.
 */
void     gowl_recording_provider_finalize     (GowlRecordingProvider *self);

G_END_DECLS

#endif /* GOWL_RECORDING_PROVIDER_H */
